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
  src/http/http_auth.cpp
  src/http/http_server.cpp
  src/metrics/ops_counters.cpp
  src/metrics/app_label.cpp
)
target_include_directories(aios_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
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

add_library(aios_client STATIC
  src/client/session.cpp
  src/client/wire.cpp
  src/client/changelog.cpp
  src/client/string.cpp
  src/client/map.cpp
  src/client/unordered_map.cpp
  src/client/set.cpp
  src/client/list.cpp
  src/client/deque.cpp
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

add_executable(aiosd src/main.cpp)
target_link_libraries(aiosd PRIVATE aios_core)

add_executable(aios-bench tools/aios_bench.cpp)
target_link_libraries(aios-bench PRIVATE aios_client)

add_executable(aios-store-bench tools/aios_store_bench.cpp)
target_link_libraries(aios-store-bench PRIVATE aios_core)

add_executable(aios tools/aios.cpp)
target_link_libraries(aios PRIVATE aios_core)

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
  tests/test_main.cpp
)
target_link_libraries(aios_tests PRIVATE aios_client)
add_test(NAME aios_tests COMMAND aios_tests)

install(TARGETS aiosd aios aios-bench aios-store-bench RUNTIME DESTINATION bin)
install(TARGETS aios_client ARCHIVE DESTINATION lib)
install(FILES config/aiosd.example.yaml DESTINATION share/aios)
