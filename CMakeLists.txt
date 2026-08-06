cmake_minimum_required(VERSION 3.24)
project(aios VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local" "/opt/homebrew/opt/sqlite")

# CMake 3.30+ removed FindBoost module; prefer BoostConfig.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

find_package(Boost 1.70 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

include(FetchContent)

# yaml-cpp 0.8.0 still declares an old cmake_minimum_required; allow configure on CMake 4+.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  GIT_SHALLOW TRUE
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# Optional Intel ISA-L for Reed–Solomon EC (m>1). XOR (m=1) always available.
option(AIOS_WITH_ISAL "Enable Intel ISA-L Reed-Solomon erasure coding when found" ON)
set(AIOS_HAVE_ISAL OFF)
if(AIOS_WITH_ISAL)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(ISAL QUIET IMPORTED_TARGET libisal)
  endif()
  if(TARGET PkgConfig::ISAL)
    set(AIOS_HAVE_ISAL ON)
    message(STATUS "ISA-L: enabled (pkg-config)")
  else()
    find_path(ISAL_INCLUDE_DIR NAMES isa-l.h
      PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(ISAL_LIBRARY NAMES isal
      PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(ISAL_INCLUDE_DIR AND ISAL_LIBRARY)
      set(AIOS_HAVE_ISAL ON)
      message(STATUS "ISA-L: enabled (${ISAL_LIBRARY})")
    else()
      message(STATUS "ISA-L: not found (EC m>1 disabled; XOR m=1 still available)")
    endif()
  endif()
endif()

# Optional NVIDIA cuObjServer for S3 GPUDirect / RDMA offload.
option(AIOS_WITH_CUOBJECT "Enable cuObjServer when the SDK is found" ON)
set(AIOS_HAVE_CUOBJECT OFF)
if(AIOS_WITH_CUOBJECT)
  if(DEFINED ENV{CUOBJECT_ROOT} AND NOT CUOBJECT_ROOT)
    set(CUOBJECT_ROOT "$ENV{CUOBJECT_ROOT}")
  endif()
  find_path(CUOBJECT_INCLUDE_DIR
    NAMES cuObjServer.h cuobject/cuObjServer.h
    PATHS ${CUOBJECT_ROOT} ${CUOBJECT_ROOT}/include
          /usr/local/include /usr/include
    PATH_SUFFIXES include)
  find_library(CUOBJECT_LIBRARY
    NAMES cuobjserver cuObjServer
    PATHS ${CUOBJECT_ROOT} ${CUOBJECT_ROOT}/lib ${CUOBJECT_ROOT}/lib64
          /usr/local/lib /usr/lib
    PATH_SUFFIXES lib lib64)
  if(CUOBJECT_INCLUDE_DIR AND CUOBJECT_LIBRARY)
    set(AIOS_HAVE_CUOBJECT ON)
    message(STATUS "cuObjServer: enabled (${CUOBJECT_LIBRARY})")
  else()
    message(STATUS "cuObjServer: not found (S3 RDMA offload stub/null only)")
  endif()
endif()

# Optional ZSTD for whole-object PUT compression.
option(AIOS_WITH_ZSTD "Enable ZSTD object compression when libzstd is found" ON)
set(AIOS_HAVE_ZSTD OFF)
if(AIOS_WITH_ZSTD)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD QUIET IMPORTED_TARGET libzstd)
  endif()
  if(TARGET PkgConfig::ZSTD)
    set(AIOS_HAVE_ZSTD ON)
    message(STATUS "ZSTD: enabled (pkg-config)")
  else()
    find_path(ZSTD_INCLUDE_DIR NAMES zstd.h
      PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(ZSTD_LIBRARY NAMES zstd
      PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
      set(AIOS_HAVE_ZSTD ON)
      message(STATUS "ZSTD: enabled (${ZSTD_LIBRARY})")
    else()
      message(STATUS "ZSTD: not found (compression=zstd disabled)")
    endif()
  endif()
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(aios_core STATIC
  src/config.cpp
  src/membership.cpp
  src/gossip.cpp
  src/net/framing.cpp
  src/net/server.cpp
  src/net/client.cpp
  src/net/object_client.cpp
  src/fs/mounts.cpp
  src/fs/aios_scan.cpp
  src/fs/fs_table.cpp
  src/util/auth.cpp
  src/util/base64.cpp
  src/util/crc32c.cpp
  src/util/compression.cpp
  src/util/aes_gcm.cpp
  src/store/object_store.cpp
  src/store/fs_clone.cpp
  src/store/local_stores.cpp
  src/cluster/cluster_map.cpp
  src/cluster/place.cpp
  src/ec/xor_parity.cpp
  src/ec/isal_rs.cpp
  src/ec/codec_factory.cpp
  src/object/object_layout.cpp
  src/object/locks_watches.cpp
  src/object/pubsub.cpp
  src/object/object_service.cpp
  src/object/repair.cpp
  src/object/transition.cpp
  src/object/object_io.cpp
  src/object/archive_bag.cpp
  src/object/archive_pack.cpp
  src/object/archive_tape.cpp
  src/object/backup.cpp
  src/http/http_auth.cpp
  src/http/http_server.cpp
  src/http/s3_auth.cpp
  src/http/s3_iam.cpp
  src/http/quota_admin.cpp
  src/http/qos_admin.cpp
  src/http/backup_policy.cpp
  src/http/posix_layout_store.cpp
  src/metrics/ops_counters.cpp
  src/metrics/app_label.cpp
  src/metrics/qos_rates.cpp
  src/metrics/frontend_io.cpp
)
target_include_directories(aios_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(aios_core PRIVATE
  "AIOS_ADMIN_WEB_DEFAULT=\"${CMAKE_CURRENT_SOURCE_DIR}/web/admin\""
)
target_link_libraries(aios_core PUBLIC
  Threads::Threads
  Boost::headers
  OpenSSL::Crypto
  SQLite3::SQLite3
  yaml-cpp
  nlohmann_json::nlohmann_json
)
if(AIOS_HAVE_ISAL)
  target_compile_definitions(aios_core PUBLIC AIOS_HAVE_ISAL=1)
  if(TARGET PkgConfig::ISAL)
    target_link_libraries(aios_core PUBLIC PkgConfig::ISAL)
  else()
    target_include_directories(aios_core PUBLIC ${ISAL_INCLUDE_DIR})
    target_link_libraries(aios_core PUBLIC ${ISAL_LIBRARY})
  endif()
endif()
if(AIOS_HAVE_ZSTD)
  target_compile_definitions(aios_core PUBLIC AIOS_HAVE_ZSTD=1)
  if(TARGET PkgConfig::ZSTD)
    target_link_libraries(aios_core PUBLIC PkgConfig::ZSTD)
  else()
    target_include_directories(aios_core PUBLIC ${ZSTD_INCLUDE_DIR})
    target_link_libraries(aios_core PUBLIC ${ZSTD_LIBRARY})
  endif()
endif()

add_library(aios_client STATIC
  src/client/session.cpp
  src/client/wire.cpp
  src/client/changelog.cpp
  src/client/string.cpp
  src/client/mutex.cpp
)
target_include_directories(aios_client PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Auth helpers live in aios_core (http_auth); link aios_core when using aios_client alone.
target_link_libraries(aios_client PUBLIC
  Threads::Threads
  Boost::headers
  OpenSSL::Crypto
  nlohmann_json::nlohmann_json
)
target_link_libraries(aios_client PUBLIC aios_core)

add_library(aios_posix STATIC
  src/posix/posix_fs.cpp
  src/posix/posix_layout.cpp
  src/posix/posix_rstat.cpp
  src/posix/quota_ledger.cpp
  src/posix/qos_controller.cpp
)
target_include_directories(aios_posix PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aios_posix PUBLIC aios_client)

# cuObject / GPUDirect bridge (null+stub always; NVIDIA when AIOS_HAVE_CUOBJECT).
add_library(aios_cuobject STATIC
  src/cuobject/cuobject_null.cpp
  src/cuobject/cuobject_stub.cpp
  src/cuobject/cuobject_nvidia.cpp
)
target_include_directories(aios_cuobject PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aios_cuobject PUBLIC aios_core)
if(AIOS_HAVE_CUOBJECT)
  target_compile_definitions(aios_cuobject PUBLIC AIOS_HAVE_CUOBJECT=1)
  target_include_directories(aios_cuobject PUBLIC ${CUOBJECT_INCLUDE_DIR})
  target_link_libraries(aios_cuobject PUBLIC ${CUOBJECT_LIBRARY})
endif()

# S3-compatible front-end (FS-backed). Separate from aios_core to avoid link cycle with aios_posix.
add_library(aios_s3 STATIC
  src/http/s3_server.cpp
  src/http/s3_range.cpp
)
target_include_directories(aios_s3 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aios_s3 PUBLIC aios_core aios_posix aios_cuobject)

# Optional FUSE3 mount helper.
option(AIOS_WITH_FUSE "Build aios-fuse when libfuse3 is available" ON)
set(AIOS_HAVE_FUSE OFF)
if(AIOS_WITH_FUSE)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
  endif()
  if(TARGET PkgConfig::FUSE3)
    set(AIOS_HAVE_FUSE ON)
    message(STATUS "FUSE3: enabled (pkg-config)")
  else()
    find_path(FUSE3_INCLUDE_DIR NAMES fuse3/fuse.h
      PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(FUSE3_LIBRARY NAMES fuse3
      PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(FUSE3_INCLUDE_DIR AND FUSE3_LIBRARY)
      set(AIOS_HAVE_FUSE ON)
      message(STATUS "FUSE3: enabled (${FUSE3_LIBRARY})")
    else()
      message(STATUS "FUSE3: not found (aios-fuse disabled)")
    endif()
  endif()
endif()

add_executable(aiosd src/main.cpp)
target_link_libraries(aiosd PRIVATE aios_core aios_s3)

add_executable(aios-bench tools/aios_bench.cpp)
target_link_libraries(aios-bench PRIVATE aios_client)

add_executable(aios-store-bench tools/aios_store_bench.cpp)
target_link_libraries(aios-store-bench PRIVATE aios_core)

add_executable(aios tools/aios.cpp)
target_link_libraries(aios PRIVATE aios_core)

add_executable(aios-cuobj-s3 tools/aios_cuobj_s3.cpp)
target_link_libraries(aios-cuobj-s3 PRIVATE aios_core aios_cuobject)
install(TARGETS aios-cuobj-s3 RUNTIME DESTINATION bin)

if(AIOS_HAVE_FUSE)
  add_executable(aios-fuse
    tools/aios_fuse.cpp
    src/posix/fuse3_ops.cpp
  )
  target_link_libraries(aios-fuse PRIVATE aios_posix)
  if(TARGET PkgConfig::FUSE3)
    target_link_libraries(aios-fuse PRIVATE PkgConfig::FUSE3)
  else()
    target_include_directories(aios-fuse PRIVATE ${FUSE3_INCLUDE_DIR})
    target_link_libraries(aios-fuse PRIVATE ${FUSE3_LIBRARY})
  endif()
  install(TARGETS aios-fuse RUNTIME DESTINATION bin)
endif()

# Optional XRootD OSS plugin (libXrdAios) over libaios_posix.
option(AIOS_WITH_XROOTD "Build XrdAios OSS plugin when XRootD is available" ON)
set(AIOS_HAVE_XROOTD OFF)
if(AIOS_WITH_XROOTD)
  if(DEFINED ENV{XRootD_ROOT} AND NOT XRootD_ROOT)
    set(XRootD_ROOT "$ENV{XRootD_ROOT}")
  endif()
  if(DEFINED ENV{XROOTD_ROOT} AND NOT XRootD_ROOT)
    set(XRootD_ROOT "$ENV{XROOTD_ROOT}")
  endif()
  find_path(XROOTD_INCLUDE_DIR NAMES XrdOss/XrdOss.hh
    HINTS
      ${XRootD_ROOT}/include/xrootd
      ${XRootD_ROOT}/include
      ${XRootD_ROOT}/src
      ${CMAKE_SOURCE_DIR}/../xrootd/src
      /usr/include/xrootd
      /usr/local/include/xrootd
      /opt/homebrew/include/xrootd
  )
  find_path(XROOTD_VERSION_INCLUDE_DIR NAMES XrdVersion.hh
    HINTS
      ${XRootD_ROOT}/include/xrootd
      ${XRootD_ROOT}/include
      ${XRootD_ROOT}/build/src
      ${CMAKE_SOURCE_DIR}/../xrootd/build/src
      ${XROOTD_INCLUDE_DIR}
      /usr/include/xrootd
      /usr/local/include/xrootd
  )
  find_library(XROOTD_UTILS_LIBRARY NAMES XrdUtils
    HINTS
      ${XRootD_ROOT}/lib64
      ${XRootD_ROOT}/lib
      ${XRootD_ROOT}/build/lib
      ${CMAKE_SOURCE_DIR}/../xrootd/build/lib
      /usr/lib64
      /usr/lib
      /usr/local/lib
      /opt/homebrew/lib
  )
  find_library(XROOTD_SERVER_LIBRARY NAMES XrdServer
    HINTS
      ${XRootD_ROOT}/lib64
      ${XRootD_ROOT}/lib
      ${XRootD_ROOT}/build/lib
      ${CMAKE_SOURCE_DIR}/../xrootd/build/lib
      /usr/lib64
      /usr/lib
      /usr/local/lib
      /opt/homebrew/lib
  )
  if(XROOTD_INCLUDE_DIR AND XROOTD_VERSION_INCLUDE_DIR AND XROOTD_UTILS_LIBRARY
     AND XROOTD_SERVER_LIBRARY)
    set(AIOS_HAVE_XROOTD ON)
    message(STATUS "XRootD: enabled (${XROOTD_UTILS_LIBRARY})")
  else()
    message(STATUS "XRootD: not found (libXrdAios disabled; set XRootD_ROOT)")
  endif()
endif()

# Always build PWD name→uid/gid helper (used by plugin + unit tests).
add_library(aios_xrd_cred STATIC src/xrd/aios_xrd_cred.cpp)
target_include_directories(aios_xrd_cred PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(AIOS_HAVE_XROOTD)
  add_library(XrdAios MODULE
    src/xrd/XrdAiosOss.cpp
    src/xrd/XrdAiosDF.cpp
    src/xrd/XrdAiosUtil.cpp
  )
  target_include_directories(XrdAios PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${XROOTD_INCLUDE_DIR}
    ${XROOTD_VERSION_INCLUDE_DIR}
  )
  target_link_libraries(XrdAios PRIVATE
    aios_posix aios_xrd_cred ${XROOTD_UTILS_LIBRARY} ${XROOTD_SERVER_LIBRARY})
  set_target_properties(XrdAios PROPERTIES
    PREFIX "lib"
    SUFFIX ".so"
    OUTPUT_NAME "XrdAios"
  )
  install(TARGETS XrdAios LIBRARY DESTINATION lib)
  install(FILES config/xrootd.aios.example.cf DESTINATION share/aios)
endif()

# Userspace bridge for the AlmaLinux 9 aiosfs kernel prototype (kernel/aiosfs).
option(AIOS_WITH_KBRIDGE "Build aios-kbridge userspace daemon for aiosfs.ko" ON)
if(AIOS_WITH_KBRIDGE)
  add_executable(aios-kbridge tools/aios_kbridge.cpp)
  target_include_directories(aios-kbridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel
  )
  target_link_libraries(aios-kbridge PRIVATE aios_posix)
  install(TARGETS aios-kbridge RUNTIME DESTINATION bin)
  install(FILES kernel/aios_kabi.h DESTINATION include/aios)
endif()

# Userspace control for aiosvd.ko (AIOS Volume Device).
add_executable(aios-vd tools/aios_vd.cpp)
target_include_directories(aios-vd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS aios-vd RUNTIME DESTINATION bin)
install(FILES kernel/aiosvd_uapi.h DESTINATION include/aios)

enable_testing()
add_executable(aios_tests
  tests/test_framing.cpp
  tests/test_framing_raw.cpp
  tests/test_aios_scan.cpp
  tests/test_auth.cpp
  tests/test_fs_clone.cpp
  tests/test_object_store.cpp
  tests/test_cluster_map.cpp
  tests/test_placement.cpp
  tests/test_object_rpc.cpp
  tests/test_object_rpc_advanced.cpp
  tests/test_object_service_advanced.cpp
  tests/test_store_range.cpp
  tests/test_store_advanced.cpp
  tests/test_http_auth.cpp
  tests/test_s3_auth.cpp
  tests/test_s3_iam.cpp
  tests/test_quota.cpp
  tests/test_qos.cpp
  tests/test_frontend_io.cpp
  tests/test_compression.cpp
  tests/test_cuobject_s3.cpp
  tests/test_s3_range.cpp
  tests/test_http_api.cpp
  tests/test_http_wire.cpp
  tests/test_http_ec.cpp
  tests/test_mini_cluster.cpp
  tests/test_crc32c.cpp
  tests/test_versions.cpp
  tests/test_ec.cpp
  tests/test_layout.cpp
  tests/test_locks_watches.cpp
  tests/test_pubsub.cpp
  tests/test_stl_client.cpp
  tests/test_append.cpp
  tests/test_admin.cpp
  tests/test_posix_fs.cpp
  tests/test_xrd_cred.cpp
  tests/test_archive.cpp
  tests/test_backup.cpp
  tests/test_main.cpp
)
target_link_libraries(aios_tests PRIVATE aios_client aios_posix aios_s3 aios_xrd_cred)
add_test(NAME aios_tests COMMAND aios_tests)

install(TARGETS aiosd aios aios-bench aios-store-bench RUNTIME DESTINATION bin)
install(TARGETS aios_client aios_posix ARCHIVE DESTINATION lib)
install(FILES src/posix/aios_posix.h DESTINATION include/aios)
install(FILES config/aiosd.example.yaml DESTINATION share/aios)
install(DIRECTORY web/admin DESTINATION share/aios)
