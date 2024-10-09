# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

include_guard()

find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL Include Dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL Libraries: ${OPENSSL_LIBRARIES}")
# Include OpenSSL 1.1.1n headers first
include_directories(BEFORE /root/install/openssl/include)
# Add OpenSSL installation directory to CMAKE_PREFIX_PATH
set(CMAKE_PREFIX_PATH "/root/install/openssl" ${CMAKE_PREFIX_PATH})


# Print for verification
message(STATUS "OpenSSL Include Directory: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")



find_package(AWSSDK REQUIRED)
set(AWS_SERVICES ec2 s3 core)
AWSSDK_DETERMINE_LIBS_TO_LINK(AWS_SERVICES AWSSDK_LIBS)

message(STATUS "aws-sdk include Libraries: ${AWSSDK_INCLUDE_DIR}")
list(APPEND AWSSDK_LIBS ${OPENSSL_LIBRARIES})
message(STATUS "aws-sdk Libraries: ${AWSSDK_LIBS}")
message(STATUS "cmake prefix Directory: ${CMAKE_PREFIX_PATH}")
message(STATUS "All Include Directories: ${CMAKE_INCLUDE_PATH}")
get_property(dirs DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
foreach(dir ${dirs})
  message(STATUS "Include dir='${dir}'")
endforeach()



# list(APPEND AWSSDK_LIBS ${HOME}/openssl/lib/libssl.a ${HOME}/openssl/lib/libcrypto.a)
# message(STATUS "aws-sdk Libraries: ${AWSSDK_LIBS}")


# Unfortunately AWSSDK_DETERMINE_LIBS_TO_LINK will list SSL and Crypto libraries
# simply as "ssl" and "crypto", not using the full path with which the AWS SDK
# was configured inside the build-env.
# list(TRANSFORM AWSSDK_LIBS REPLACE "^ssl$" OpenSSL::SSL)
# list(TRANSFORM AWSSDK_LIBS REPLACE "^crypto$" OpenSSL::Crypto)

add_library(aws-sdk-cpp INTERFACE)
target_link_libraries(
  aws-sdk-cpp
  INTERFACE
    ${AWSSDK_LIBS}
    ${AWSSDK_LIBS}
    s2n
    dl
)
target_include_directories(
  aws-sdk-cpp
  INTERFACE
    ${AWSSDK_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)
