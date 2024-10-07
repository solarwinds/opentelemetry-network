// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "kubernetes_rpc_server.h"

#include <arpa/inet.h>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <spdlog/formatter.h>

#include "channel/buffered_writer.h"
#include "generated/ebpf_net/ingest/writer.h"
#include "generated/kubernetes_info.pb.h"
#include "kubernetes_owner_kind.h"
#include "platform/types.h"
#include "resync_channel.h"
#include "util/boot_time.h"
#include "util/log.h"
#include "util/lookup3_hasher.h"
#include <util/protobuf_log.h>

// A libfmt formatter for ReplicaSetInfo, used to print addresses in LOG::trace
// etc.
namespace fmt {
template <> struct formatter<collector::OwnerInfo> {
    // Parsing (optional, since we don't need custom specifiers)
    template <typename ParseContext>
    constexpr auto parse(ParseContext &ctx) { return ctx.begin(); }

    // Formatting method
    template <typename FormatContext>
    auto format(const collector::OwnerInfo &owner, FormatContext &ctx) {
        return format_to(ctx.out(), "OwnerInfo(uid: {}, name: {}, kind: {})",
                         owner.uid(), owner.name(), owner.kind());
    }
};
template <> struct formatter<collector::ContainerInfo> {
    template <typename ParseContext>
    constexpr auto parse(ParseContext &ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const collector::ContainerInfo &container, FormatContext &ctx) {
        return format_to(ctx.out(), "ContainerInfo(id: {}, name: {}, image: {})",
                         container.id(), container.name(), container.image());
    }
};
template <> struct formatter<collector::PodInfo> {
    template <typename ParseContext>
    constexpr auto parse(ParseContext &ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const collector::PodInfo &pod, FormatContext &ctx) {
        // Format the containers and container_infos into a string
        std::string containers_str;
        for (const auto& container : pod.container_infos()) {
            containers_str += fmt::format("{}", container);
            containers_str += ", ";
        }

        return format_to(ctx.out(),
                         "PodInfo(uid: {}, ip: {}, name: {}, owner: {}, ns: {}, version: {}, is_host_network: {}, containers: [{}])",
                         pod.uid(), pod.ip(), pod.name(), pod.owner(), pod.ns(),
                         pod.version(), pod.is_host_network(), containers_str);
    }
};
template <> struct formatter<collector::ReplicaSetInfo> {
    template <typename ParseContext>
    constexpr auto parse(ParseContext &ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const collector::ReplicaSetInfo &replica_set, FormatContext &ctx) {
        return format_to(ctx.out(), "ReplicaSetInfo(uid: {}, owner: {})",
                         replica_set.uid(), replica_set.owner());
    }
};
template <> struct formatter<collector::JobInfo> {
    template <typename ParseContext>
    constexpr auto parse(ParseContext &ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const collector::JobInfo &job, FormatContext &ctx) {
        return format_to(ctx.out(), "JobInfo(uid: {}, owner: {})",
                         job.uid(), job.owner());
    }
};
} // namespace fmt

namespace collector {
using ::grpc::ServerContext;
using ::grpc::ServerReaderWriter;
using ::grpc::Status;
using ::grpc::WriteOptions;

namespace {
// K8sHandler maintain keeps track of the state of Pod & ReplicaSet.
// It consumes the Pod & ReplicaSet events sent back by k8s-watcher, and
// decides whether & what messages to be sent back to the reducer.
class K8sHandler {
public:
  // Does not take ownership of |writer|
  explicit K8sHandler(ebpf_net::ingest::Writer *writer) : writer_(writer) {}

  ~K8sHandler() {}

  // Returns true if gRpc stream needs to be restarted.
  bool need_restart() const;

  void owner_new_or_modified(const std::string &uid, const OwnerInfo &owner_info);
  void owner_deleted(const std::string &uid, const OwnerInfo &owner_info);
  void replica_set_new_or_modified(const ReplicaSetInfo &rs_info);
  void replica_set_deleted(const ReplicaSetInfo &rs_info);
  void job_new_or_modified(const JobInfo &job_info);
  void job_deleted(const JobInfo &job_info);
  void pod_new_or_modified(const PodInfo &pod_info);
  void pod_deleted(const PodInfo &pod_info);

private:
  // Max number of Pods allowed to wait for the ReplicaSet infos.
  static constexpr u64 max_waiting_pods_ = 10000;

  // Max number of deleted owners before they are purged.
  static constexpr u64 max_deleted_owners_ = 10000;

  u64 get_id(const std::string &uid);
  void send_pod_new(const PodInfo &pod_info, const OwnerInfo &owner);
  void send_pod_new_no_owner(const PodInfo &pod_info);
  void send_pod_containers(const PodInfo &pod_info);

  struct OwnerStore {
    //
    // Existing ReplicaSet/Job's OwnerInfo that we know of. Here the key is the
    // id of the ReplicaSet/Job.
    std::unordered_map<u64, OwnerInfo, ::util::Lookup3Hasher<u64>> infos;

    // ReplicaSet metadata which has been deleted recently.
    std::deque<u64> deleted;

    // Maps from the id of the ReplicaSet which is not available yet,
    // to list of ids of the Pods relying on this ReplicaSet.
    std::unordered_map<u64, std::vector<u64>, ::util::Lookup3Hasher<u64>> waiting;
  };

  struct PodStore {
    // Existing Pod metadata that we know of.
    std::unordered_map<u64, PodInfo, ::util::Lookup3Hasher<u64>> infos;

    // Set of Pods whose info has been sent back to pipeline server.
    std::unordered_set<u64, ::util::Lookup3Hasher<u64>> live;

    // Set of Pods that replies on a yet-to-be-seen ReplicaSet/Job.
    std::unordered_set<u64, ::util::Lookup3Hasher<u64>> waiting;
  };

  // Maps ReplicaSet's, Job's and Pod's UID to a sequential id.
  // This is to avoid storing the UID (a long string) in multiple places.
  u64 next_id_ = 0;
  std::unordered_map<std::string, u64> uid_to_id_;

  OwnerStore owners_;
  PodStore pods_;

  ebpf_net::ingest::Writer *writer_;
};

bool K8sHandler::need_restart() const
{
  return pods_.waiting.size() >= max_waiting_pods_;
}

void K8sHandler::send_pod_new(const PodInfo &pod_info, const OwnerInfo &owner)
{
  LOG::trace("Server: enqueue POD New: {}", pod_info.uid());

  jb_blob uid{pod_info.uid().data(), (u16)pod_info.uid().size()};

  writer_->pod_new_with_name(
      uid,
      (u32)(inet_addr(pod_info.ip().c_str())),
      jb_blob{owner.name().data(), (u16)owner.name().size()},
      jb_blob{pod_info.name().c_str(), (u16)pod_info.name().size()},
      (uint8_t)KubernetesOwnerKindFromString(owner.kind()),
      jb_blob{owner.uid().data(), (u16)owner.uid().size()},
      (pod_info.is_host_network() ? 1 : 0),
      jb_blob{pod_info.ns().data(), (u16)pod_info.ns().size()},
      jb_blob{pod_info.version().data(), (u16)pod_info.version().size()});

  send_pod_containers(pod_info);
}

void K8sHandler::send_pod_new_no_owner(const PodInfo &pod_info)
{
  LOG::trace("Server: enqueue POD New (No Owner): {}", pod_info.uid());

  jb_blob uid{pod_info.uid().data(), (u16)pod_info.uid().size()};

  writer_->pod_new_with_name(
      uid,
      (u32)(inet_addr(pod_info.ip().c_str())),
      jb_blob{pod_info.name().c_str(), (u16)pod_info.name().size()},
      jb_blob{pod_info.name().c_str(), (u16)pod_info.name().size()},
      (uint8_t)(KubernetesOwnerKind::NoOwner),
      jb_blob{"", (u16)0},
      (pod_info.is_host_network() ? 1 : 0),
      jb_blob{pod_info.ns().data(), (u16)pod_info.ns().size()},
      jb_blob{pod_info.version().data(), (u16)pod_info.version().size()});

  send_pod_containers(pod_info);
}

void K8sHandler::send_pod_containers(const PodInfo &pod_info)
{
  jb_blob uid{pod_info.uid().data(), (u16)pod_info.uid().size()};

  for (int i = 0; i < pod_info.container_infos_size(); ++i) {
    std::string const &cid = pod_info.container_infos(i).id();
    std::string const &name = pod_info.container_infos(i).name();
    std::string const &image = pod_info.container_infos(i).image();
    writer_->pod_container(
        uid,
        jb_blob{cid.data(), (u16)cid.size()},
        jb_blob{name.data(), (u16)name.size()},
        jb_blob{image.data(), (u16)image.size()});
  }
}

u64 K8sHandler::get_id(const std::string &uid)
{
  const auto iter = uid_to_id_.find(uid);
  if (iter != uid_to_id_.end()) {
    return iter->second;
  }

  u64 id = next_id_++;
  uid_to_id_.emplace(uid, id);
  return id;
}

void K8sHandler::owner_new_or_modified(const std::string &uid, const OwnerInfo &owner_info)
{
  u64 id = get_id(uid);
  auto iter = owners_.infos.find(id);
  if (iter != owners_.infos.end()) {
    // updated
    iter->second.MergeFrom(owner_info);
  } else {
    // insert
    owners_.infos.emplace(id, std::move(owner_info));
  }

  // See if any Pod is waiting for this id
  auto waiting_iter = owners_.waiting.find(id);
  if (waiting_iter == owners_.waiting.end()) {
    return;
  }

  for (u64 pod_id : waiting_iter->second) {
    auto pod_iter = pods_.infos.find(pod_id);
    if (pod_iter == pods_.infos.end()) {
      // The POD since has been deleted.
      continue;
    }

    u64 current_owner_id = get_id(pod_iter->second.owner().uid());
    if (current_owner_id != id) {
      // The POD since has been updated with new owner.
      continue;
    }

    if (KubernetesOwnerIsDeployment(owner_info.kind()) ||
        KubernetesOwnerIsCronJob(owner_info.kind())) {
      send_pod_new(pod_iter->second, owner_info);
    } else {
      send_pod_new(pod_iter->second, pod_iter->second.owner());
    }
    pods_.waiting.erase(pod_id);
    pods_.live.insert(pod_id);
  }
  owners_.waiting.erase(waiting_iter);
}

void K8sHandler::owner_deleted(const std::string &uid, const OwnerInfo &owner_info)
{
  u64 id = get_id(uid);
  auto iter = owners_.infos.find(id);
  if (iter == owners_.infos.end()) {
    uid_to_id_.erase(uid);
    return;
  }

  owners_.deleted.push_back(id);
  if (owners_.deleted.size() <= max_deleted_owners_) {
    return;
  }

  // There are more than |max_deleted_owners_| entries in the set,
  // so remove the oldest one.
  u64 expired_id = owners_.deleted.front();
  owners_.deleted.pop_front();
  auto expired_iter = owners_.infos.find(expired_id);
  if (expired_iter == owners_.infos.end()) {
    LOG::info("Owner removed before it expires.");
    return;
  }

  uid_to_id_.erase(expired_iter->second.uid());
  owners_.infos.erase(expired_id);
}

void K8sHandler::replica_set_new_or_modified(const ReplicaSetInfo &rs_info)
{
  if (rs_info.uid().empty()) {
    LOG::warn("ReplicaSet info without UID. {}", rs_info);
    return;
  }

  owner_new_or_modified(rs_info.uid(), rs_info.owner());
}

void K8sHandler::replica_set_deleted(const ReplicaSetInfo &rs_info)
{
  if (rs_info.uid().empty()) {
    LOG::warn("ReplicaSet info without UID. {}", rs_info);
    return;
  }

  owner_deleted(rs_info.uid(), rs_info.owner());
}


void K8sHandler::job_new_or_modified(const JobInfo &job_info)
{
  if (job_info.uid().empty()) {
    LOG::warn("Job info without UID. {}", job_info);
    return;
  }

  owner_new_or_modified(job_info.uid(), job_info.owner());
}

void K8sHandler::job_deleted(const JobInfo &job_info)
{
  if (job_info.uid().empty()) {
    LOG::warn("Job info without UID. {}", job_info);
    return;
  }

  owner_deleted(job_info.uid(), job_info.owner());
}

void K8sHandler::pod_new_or_modified(const PodInfo &pod_info)
{
  if (pod_info.uid().empty()) {
    LOG::warn("Pod info without UID. {}", pod_info);
    return;
  }

  u64 id = get_id(pod_info.uid());

  auto iter = pods_.infos.find(id);
  if (iter != pods_.infos.end()) {
    iter->second.MergeFrom(pod_info);
    LOG::trace("Merged pod into internal state: {}", pod_info);
  } else {
    LOG::trace("Adding pod into internal state: {}", pod_info);
    iter = pods_.infos.emplace(id, std::move(pod_info)).first;
  }

  if (pods_.live.find(id) != pods_.live.end()) {
    // TODO: we might want to define and pod_modified message and send it back
    //       to pipeline server
    LOG::trace("Pod has already been reported. Sending containers only. {}", pod_info);
    send_pod_containers(pod_info);
    return;
  }

  const auto &pod = iter->second;
  if (pod.ip().empty()) {
    LOG::trace("Pod has not been reported, but its ip is empty. IP empty: {}", pod.ip().empty());
    return;
  }

  if (!pod.has_owner()) {
    LOG::trace("Pod does not have owner. Sending. {}", pod_info);
    send_pod_new_no_owner(pod);
    pods_.live.insert(id);
    return;
  }

  if (!KubernetesOwnerIsReplicaSet(pod.owner().kind()) &&
      !KubernetesOwnerIsJob(pod.owner().kind()))
  {
     // Not owned by a ReplicaSet or Job, just send new_pod
    LOG::trace("Pod is not owned by ReplicaSet/Job. Sending. {}", pod_info);
    send_pod_new(pod, pod.owner());
    pods_.live.insert(id);
    return;
  }

   // Pod is owned by replica set or Job, need to check if the owner exists.
  u64 owner_id = get_id(pod.owner().uid());
  auto owner_iter = owners_.infos.find(owner_id);

  if (owner_iter == owners_.infos.end()) {
    // We have not seen the ReplicaSet/Job yet, needs to wait.
    auto waiting_iter = owners_.waiting.find(owner_id);
    if (waiting_iter == owners_.waiting.end()) {
      owners_.waiting[owner_id] = std::vector<u64>({id});
      LOG::trace("Pod's Owner queue did not exist, added queue and enqueued. {}", pod_info);
    } else {
      waiting_iter->second.push_back(id);
      LOG::trace("Pod's Owner did not exist, enqueued to existing queue. {}", pod_info);
    }
    pods_.waiting.insert(id);
    return;
  }

  const OwnerInfo &owner = owner_iter->second;
  if (KubernetesOwnerIsReplicaSet(pod.owner().kind())) {
    // Pod is owned by ReplicaSet
    if (KubernetesOwnerIsDeployment(owner.kind())) {
      LOG::trace("Pod's owner ReplicaSet has Deployment owner. Sending pod with owner {}", owner);
      send_pod_new(pod, owner);
    } else {
      LOG::trace("Pod's owner ReplicaSet has non-Deployment owner. Sending pod with owner {}", pod.owner());
      send_pod_new(pod, pod.owner());
    }
  } else if (KubernetesOwnerIsJob(pod.owner().kind())) {
    // Pod is owned by job
    if (KubernetesOwnerIsCronJob(owner.kind())) {
      LOG::trace("Pod's owner Job has CronJob owner. Sending pod with owner {}", owner);
      send_pod_new(pod, owner);
    } else {
      LOG::trace("Pod's owner Job has non-CronJob owner. Sending pod with owner {}", pod.owner());
      send_pod_new(pod, pod.owner());
    }
  }
  pods_.live.insert(id);
}

void K8sHandler::pod_deleted(const PodInfo &pod_info)
{
  if (pod_info.uid().empty()) {
    LOG::error("Pod delete event without UID. ({})", pod_info);
    return;
  }

  u64 id = get_id(pod_info.uid());
  auto live_iter = pods_.live.find(id);
  if (live_iter != pods_.live.end()) {
    LOG::trace("Server: enqueue POD Delete: {}\n", pod_info.uid());

    writer_->pod_delete(jb_blob{pod_info.uid().data(), (u16)pod_info.uid().size()});
  }

  pods_.live.erase(id);
  pods_.infos.erase(id);
  pods_.waiting.erase(id);
  uid_to_id_.erase(pod_info.uid());
}
} // namespace

KubernetesRpcServer::KubernetesRpcServer(ResyncChannelFactory *channel_factory, std::size_t collect_buffer_size)
    : channel_factory_(channel_factory), collect_buffer_size_(collect_buffer_size)
{}

KubernetesRpcServer::~KubernetesRpcServer() {}

Status KubernetesRpcServer::Collect(ServerContext *context, ServerReaderWriter<Response, Info> *reader_writer)
{
  std::function<void(void)> reset_callback = [&]() {
    Response response;
    WriteOptions options;
    options.set_write_through().set_last_message();
    LOG::info("Relay: notify watcher to stop.");

    reader_writer->Write(response, options);
    LOG::info("Relay: canceling watcher.");
    context->TryCancel();
  };

  std::unique_ptr<ResyncChannel> resync_channel = channel_factory_->new_channel(reset_callback);

  channel::BufferedWriter buffered_writer(*resync_channel, collect_buffer_size_);

  ebpf_net::ingest::Writer writer(buffered_writer, monotonic, get_boot_time());

  K8sHandler handler(&writer);
  Info info;
  while (reader_writer->Read(&info)) {
    if (info.type() == Info::K8S_REPLICASET) {
      const ReplicaSetInfo &rs_info = info.rs_info();
      switch (info.event()) {
      case Info_Event_ADDED:
      case Info_Event_MODIFIED:
        handler.replica_set_new_or_modified(rs_info);
        break;
      case Info_Event_DELETED:
        handler.replica_set_deleted(rs_info);
        break;
      case Info_Event_ERROR:
      default:
        // do nothing now.
        break;
      }
    } if (info.type() == Info::K8S_JOB) {
      const JobInfo &job_info = info.job_info();
      switch (info.event()) {
      case Info_Event_ADDED:
      case Info_Event_MODIFIED:
        handler.job_new_or_modified(job_info);
        break;
      case Info_Event_DELETED:
        handler.job_deleted(job_info);
        break;
      case Info_Event_ERROR:
        // Got an error/unhandled event from the k8s watch API, ignore it.
      default:
        // do nothing now.
        break;
      }
    } else {
      // K8S_POD
      const PodInfo &pod_info = info.pod_info();
      switch (info.event()) {
      case Info_Event_ADDED:
      case Info_Event_MODIFIED:
        handler.pod_new_or_modified(pod_info);
        break;
      case Info_Event_DELETED:
        handler.pod_deleted(pod_info);
        break;
      case Info_Event_ERROR:
        // do nothing now.
      default:
        break;
      }
    }
    if (handler.need_restart()) {
      break;
    }
    // Always flush after every send.
    // The internal queue in ResyncQueue will buffer multiple messages
    // together instead.
    buffered_writer.flush();
  } // while()

  // Discard anything left.
  buffered_writer.reset();

  // Always returns CANCELLED, since the stream should not be broken unless
  // something bad happens.
  return Status::CANCELLED;
}

} // namespace collector
