cmake_minimum_required(VERSION 3.24)
project(aios VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Required so static libs can link into MODULE plugins (libXrdAios.so) on Linux.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(AiosBuildOptions)          # AIOS_WERROR, AIOS_SANITIZE
include(macos-homebrew)            # no-op except on APPLE; sets AIOS_HOMEBREW_PREFIX

list(APPEND CMAKE_PREFIX_PATH "/usr/local")

# Version header: build/generated/include/aios_version.hpp (AIOS_VERSION_STRING, …).
find_package(Git QUIET)
set(AIOS_GIT_DESCRIBE "")
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE AIOS_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(AIOS_GIT_DESCRIBE)
  set(AIOS_VERSION_FULL "${PROJECT_VERSION} (${AIOS_GIT_DESCRIBE})")
else()
  set(AIOS_VERSION_FULL "${PROJECT_VERSION}")
endif()
set(AIOS_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
configure_file(cmake/aios_version.hpp.in "${AIOS_GENERATED_INCLUDE_DIR}/aios_version.hpp" @ONLY)

# CMake 3.30+ removed FindBoost module; prefer BoostConfig.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

find_package(Boost 1.70 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
# Alma/RHEL FindSQLite3 may set variables without creating the imported target.
if(NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 UNKNOWN IMPORTED)
  set_target_properties(SQLite3::SQLite3 PROPERTIES
    IMPORTED_LOCATION "${SQLite3_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
endif()

include(FetchContent)

# Third-party sources are pinned to the commit behind each release tag so a moved
# or deleted tag cannot change what gets built. Commit hashes cannot be fetched
# with a shallow clone, hence GIT_SHALLOW FALSE. To build offline, pre-populate
# the sources and configure with -DFETCHCONTENT_FULLY_DISCONNECTED=ON, or point
# -DFETCHCONTENT_SOURCE_DIR_<NAME>=... at a local checkout (see README "Build").
#
#   yaml-cpp      0.8.0   f7320141120f720aecc4c32be25586e7da9eb978
#   nlohmann/json v3.11.3 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
#   googletest    v1.15.2 b514bdc898e2951020cbdca1304b75f5950d1f59

# yaml-cpp 0.8.0 still declares an old cmake_minimum_required; allow configure on CMake 4+.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG f7320141120f720aecc4c32be25586e7da9eb978   # 0.8.0
  GIT_SHALLOW FALSE
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03   # v3.11.3
  GIT_SHALLOW FALSE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59   # v1.15.2
  GIT_SHALLOW FALSE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# Treat dependency headers as system headers so their warnings never trip AIOS_WERROR.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
  foreach(_dep yaml-cpp nlohmann_json gtest gtest_main gmock gmock_main)
    if(TARGET ${_dep})
      set_target_properties(${_dep} PROPERTIES SYSTEM ON)
    endif()
  endforeach()
  unset(_dep)
endif()

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
      PATHS ${AIOS_HOMEBREW_PREFIX}/include /usr/local/include /usr/include)
    find_library(ISAL_LIBRARY NAMES isal
      PATHS ${AIOS_HOMEBREW_PREFIX}/lib /usr/local/lib /usr/lib)
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
      PATHS ${AIOS_HOMEBREW_PREFIX}/include /usr/local/include /usr/include)
    find_library(ZSTD_LIBRARY NAMES zstd
      PATHS ${AIOS_HOMEBREW_PREFIX}/lib /usr/local/lib /usr/lib)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
      set(AIOS_HAVE_ZSTD ON)
      message(STATUS "ZSTD: enabled (${ZSTD_LIBRARY})")
    else()
      message(STATUS "ZSTD: not found (compression=zstd disabled)")
    endif()
  endif()
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
aios_apply_werror()   # -Werror only for AIOS targets (declared below), never for _deps

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
  src/util/file_io.cpp
  src/util/compression.cpp
  src/util/aes_gcm.cpp
  src/util/ticket.cpp
  src/util/write_grant.cpp
  src/store/object_store.cpp
  src/store/fs_clone.cpp
  src/store/local_stores.cpp
  src/cluster/cluster_map.cpp
  src/cluster/map_monitor.cpp
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
  src/http/principal_store.cpp
  src/http/quota_admin.cpp
  src/http/qos_admin.cpp
  src/http/backup_policy.cpp
  src/http/posix_layout_store.cpp
  src/http/vbd_registry.cpp
  src/http/space_history.cpp
  src/metrics/ops_counters.cpp
  src/metrics/app_label.cpp
  src/metrics/qos_rates.cpp
  src/metrics/frontend_io.cpp
)
target_include_directories(aios_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  $<BUILD_INTERFACE:${AIOS_GENERATED_INCLUDE_DIR}>
)
# Compiled-in fallback for the admin SPA: the *installed* location, not the source
# tree (a binary must not depend on its build checkout). Development builds still
# find web/admin through the AIOS_ADMIN_WEB env var or the cwd-relative probes in
# find_admin_web_root() (src/http/http_server.cpp).
target_compile_definitions(aios_core PRIVATE
  "AIOS_ADMIN_WEB_DEFAULT=\"${CMAKE_INSTALL_FULL_DATADIR}/aios/admin\""
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
  src/client/vbd_registry_client.cpp
  src/bench/http_bench.cpp
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
  src/posix/posix_lease.cpp
  src/posix/quota_ledger.cpp
  src/posix/qos_controller.cpp
)
target_include_directories(aios_posix PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel
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
  src/http/tls_stream.cpp
)
target_include_directories(aios_s3 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aios_s3 PUBLIC aios_core aios_posix aios_cuobject OpenSSL::SSL)

# Optional FUSE3 mount helpers (high-level aios-fuse and low-level aios-fusell).
option(AIOS_WITH_FUSE "Build aios-fuse / aios-fusell when libfuse3 is available" ON)
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
      PATHS ${AIOS_HOMEBREW_PREFIX}/include /usr/local/include /usr/include)
    find_library(FUSE3_LIBRARY NAMES fuse3
      PATHS ${AIOS_HOMEBREW_PREFIX}/lib /usr/local/lib /usr/lib)
    if(FUSE3_INCLUDE_DIR AND FUSE3_LIBRARY)
      set(AIOS_HAVE_FUSE ON)
      message(STATUS "FUSE3: enabled (${FUSE3_LIBRARY})")
    else()
      message(STATUS "FUSE3: not found (aios-fuse / aios-fusell disabled)")
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
target_link_libraries(aios PRIVATE aios_client)

add_executable(aios-cuobj-s3 tools/aios_cuobj_s3.cpp)
target_link_libraries(aios-cuobj-s3 PRIVATE aios_core aios_cuobject)
install(TARGETS aios-cuobj-s3 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(AIOS_HAVE_FUSE)
  add_executable(aios-fuse
    tools/aios_fuse.cpp
    src/posix/fuse3_ops.cpp
  )
  add_executable(aios-fusell
    tools/aios_fusell.cpp
    src/posix/fuse3_ll_ops.cpp
  )
  foreach(_fuse_bin aios-fuse aios-fusell)
    target_link_libraries(${_fuse_bin} PRIVATE aios_posix)
    if(TARGET PkgConfig::FUSE3)
      target_link_libraries(${_fuse_bin} PRIVATE PkgConfig::FUSE3)
    else()
      target_include_directories(${_fuse_bin} PRIVATE ${FUSE3_INCLUDE_DIR})
      target_link_libraries(${_fuse_bin} PRIVATE ${FUSE3_LIBRARY})
    endif()
  endforeach()
  install(TARGETS aios-fuse aios-fusell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Optional XRootD OSS plugin (libXrdAios) over libaios_posix.
option(AIOS_WITH_XROOTD "Build XrdAios OSS plugin when XRootD is available" ON)
set(AIOS_HAVE_XROOTD OFF)
if(AIOS_WITH_XROOTD)
  # Lookup order:
  #   1. XRootD_ROOT / XROOTD_ROOT (-D or environment) — an install prefix
  #      (include/xrootd, lib64) *or* a source+build checkout (src/, build/src, build/lib)
  #   2. find_package(XRootD CONFIG) — XRootD ≥ 5 installs XRootDConfig.cmake
  #   3. pkg-config `xrootd`
  #   4. system prefixes and the Homebrew prefix
  # The per-component cache variables XROOTD_INCLUDE_DIR / XROOTD_VERSION_INCLUDE_DIR /
  # XROOTD_UTILS_LIBRARY / XROOTD_SERVER_LIBRARY can always be set explicitly.
  set(XRootD_ROOT "" CACHE PATH "XRootD install prefix or source+build tree (also XROOTD_ROOT / \$XRootD_ROOT)")
  if(NOT XRootD_ROOT AND XROOTD_ROOT)
    set(XRootD_ROOT "${XROOTD_ROOT}")
  endif()
  if(NOT XRootD_ROOT AND DEFINED ENV{XRootD_ROOT} AND NOT "$ENV{XRootD_ROOT}" STREQUAL "")
    set(XRootD_ROOT "$ENV{XRootD_ROOT}")
  endif()
  if(NOT XRootD_ROOT AND DEFINED ENV{XROOTD_ROOT} AND NOT "$ENV{XROOTD_ROOT}" STREQUAL "")
    set(XRootD_ROOT "$ENV{XROOTD_ROOT}")
  endif()

  set(_xrd_inc_hints "")
  set(_xrd_lib_hints "")
  if(XRootD_ROOT)
    list(APPEND _xrd_inc_hints
      "${XRootD_ROOT}/include/xrootd" "${XRootD_ROOT}/include"
      "${XRootD_ROOT}/src" "${XRootD_ROOT}/build/src")
    list(APPEND _xrd_lib_hints
      "${XRootD_ROOT}/lib64" "${XRootD_ROOT}/lib" "${XRootD_ROOT}/build/lib")
  endif()

  if(NOT XRootD_ROOT)
    find_package(XRootD CONFIG QUIET)
    if(XRootD_FOUND)
      if(XRootD_INCLUDE_DIRS)
        list(APPEND _xrd_inc_hints ${XRootD_INCLUDE_DIRS})
      endif()
      if(XRootD_LIBRARY_DIRS)
        list(APPEND _xrd_lib_hints ${XRootD_LIBRARY_DIRS})
      endif()
      if(TARGET XRootD::XrdUtils)
        get_target_property(_xrd_utils_loc XRootD::XrdUtils LOCATION)
        if(_xrd_utils_loc)
          get_filename_component(_xrd_utils_dir "${_xrd_utils_loc}" DIRECTORY)
          list(APPEND _xrd_lib_hints "${_xrd_utils_dir}")
        endif()
      endif()
    endif()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(XROOTD_PC QUIET xrootd)
      if(XROOTD_PC_FOUND)
        list(APPEND _xrd_inc_hints ${XROOTD_PC_INCLUDE_DIRS})
        list(APPEND _xrd_lib_hints ${XROOTD_PC_LIBRARY_DIRS})
      endif()
    endif()
  endif()

  find_path(XROOTD_INCLUDE_DIR NAMES XrdOss/XrdOss.hh
    HINTS ${_xrd_inc_hints}
    PATHS /usr/include/xrootd /usr/local/include/xrootd ${AIOS_HOMEBREW_PREFIX}/include/xrootd
  )
  find_path(XROOTD_VERSION_INCLUDE_DIR NAMES XrdVersion.hh
    HINTS ${_xrd_inc_hints} ${XROOTD_INCLUDE_DIR}
    PATHS /usr/include/xrootd /usr/local/include/xrootd ${AIOS_HOMEBREW_PREFIX}/include/xrootd
  )
  find_library(XROOTD_UTILS_LIBRARY NAMES XrdUtils
    HINTS ${_xrd_lib_hints}
    PATHS /usr/lib64 /usr/lib /usr/local/lib ${AIOS_HOMEBREW_PREFIX}/lib
  )
  find_library(XROOTD_SERVER_LIBRARY NAMES XrdServer
    HINTS ${_xrd_lib_hints}
    PATHS /usr/lib64 /usr/lib /usr/local/lib ${AIOS_HOMEBREW_PREFIX}/lib
  )
  unset(_xrd_inc_hints)
  unset(_xrd_lib_hints)

  if(XROOTD_INCLUDE_DIR AND XROOTD_VERSION_INCLUDE_DIR AND XROOTD_UTILS_LIBRARY
     AND XROOTD_SERVER_LIBRARY)
    set(AIOS_HAVE_XROOTD ON)
    message(STATUS "XRootD: enabled (headers ${XROOTD_INCLUDE_DIR}; ${XROOTD_SERVER_LIBRARY})")
  else()
    message(STATUS "XRootD: not found — libXrdAios.so will NOT be built "
      "(AIOS_WITH_XROOTD=ON is the default; pass -DXRootD_ROOT=<prefix or source+build tree>, "
      "or -DAIOS_WITH_XROOTD=OFF to silence this)")
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
  target_include_directories(XrdAios PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # XRootD headers have unused-parameter warnings; keep them out of AIOS_WERROR.
  target_include_directories(XrdAios SYSTEM PRIVATE
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
  install(TARGETS XrdAios LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES config/xrootd.aios.example.cf DESTINATION ${CMAKE_INSTALL_DATADIR}/aios)
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
  install(TARGETS aios-kbridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES kernel/aios_kabi.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aios)
endif()

# Userspace control for aiosvd.ko (AIOS Volume Device).
add_executable(aios-vd tools/aios_vd.cpp)
target_include_directories(aios-vd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aios-vd PRIVATE aios_client)
install(TARGETS aios-vd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES kernel/aiosvd_uapi.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aios)

enable_testing()
add_executable(aios_tests
  tests/test_framing.cpp
  tests/test_framing_raw.cpp
  tests/test_aios_scan.cpp
  tests/test_auth.cpp
  tests/test_fs_clone.cpp
  tests/test_object_store.cpp
  tests/test_cluster_map.cpp
  tests/test_map_monitor.cpp
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
  tests/test_put_pipeline.cpp
  tests/test_crc32c.cpp
  tests/test_versions.cpp
  tests/test_ec.cpp
  tests/test_layout.cpp
  tests/test_locks_watches.cpp
  tests/test_pubsub.cpp
  tests/test_stl_client.cpp
  tests/test_append.cpp
  tests/test_admin.cpp
  tests/test_space_history.cpp
  tests/test_posix_fs.cpp
  tests/test_xrd_cred.cpp
  tests/test_archive.cpp
  tests/test_backup.cpp
  tests/test_bugfix_regressions.cpp
  tests/test_aiosd_regressions.cpp
  tests/test_code_review_regressions.cpp
  tests/test_code_review_more.cpp
  tests/test_session_wire.cpp
  tests/test_review2_object.cpp
  tests/test_review2_http.cpp
  tests/test_review2_store.cpp
  tests/test_review2_posix.cpp
  tests/test_review2_kabi.cpp
  tests/test_process_smoke.cpp
  tests/test_process_consensus.cpp
  tests/test_client_io.cpp
  tests/test_ticket_auth.cpp
  tests/test_s3_tls.cpp
)
target_include_directories(aios_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel
)
target_link_libraries(aios_tests PRIVATE aios_client aios_posix aios_s3 aios_xrd_cred GTest::gtest_main)
# The process-level smoke test spawns the real daemon and CLI binaries.
target_compile_definitions(aios_tests PRIVATE
  AIOS_TEST_AIOSD_PATH="$<TARGET_FILE:aiosd>"
  AIOS_TEST_AIOS_CLI_PATH="$<TARGET_FILE:aios>"
)
add_dependencies(aios_tests aiosd aios)
gtest_discover_tests(aios_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES RUN_SERIAL TRUE
)

install(TARGETS aiosd aios aios-bench aios-store-bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# aios_client and aios_posix are static and depend on aios_core (HTTP HMAC helpers,
# config, JSON); install all three so the installed archives are linkable:
#   -laios_posix -laios_client -laios_core + yaml-cpp, sqlite3, crypto, pthread
#   (+ zstd / isal when enabled) — see README "STL-like C++ client → Build / link".
install(TARGETS aios_core aios_client aios_posix ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/posix/aios_posix.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aios)
install(FILES kernel/aiosfs_uapi.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aios)
# Public client headers keep their "client/…" include spelling: add -I<prefix>/include/aios.
install(DIRECTORY src/client/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aios/client
  FILES_MATCHING PATTERN "*.hpp"
  PATTERN "vbd_registry_client.hpp" EXCLUDE)   # pulls in daemon-side object_service.hpp
install(FILES "${AIOS_GENERATED_INCLUDE_DIR}/aios_version.hpp" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aios)
install(FILES config/aiosd.example.yaml DESTINATION ${CMAKE_INSTALL_DATADIR}/aios)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/aios)
install(DIRECTORY web/admin DESTINATION ${CMAKE_INSTALL_DATADIR}/aios)
