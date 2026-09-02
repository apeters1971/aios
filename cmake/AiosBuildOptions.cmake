# Warning-as-error and sanitizer switches shared by every target (including
# FetchContent dependencies, so ASan/TSan instrumentation is consistent).
#
#   -DAIOS_WERROR=ON                 add -Werror
#   -DAIOS_SANITIZE=asan-ubsan|tsan  instrument compile + link of all targets

option(AIOS_WERROR "Treat warnings as errors" OFF)

set(AIOS_SANITIZE "off" CACHE STRING "Sanitizer set: off | asan-ubsan | tsan")
set_property(CACHE AIOS_SANITIZE PROPERTY STRINGS off asan-ubsan tsan)

if(AIOS_SANITIZE STREQUAL "asan-ubsan")
  set(_aios_san_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif(AIOS_SANITIZE STREQUAL "tsan")
  set(_aios_san_flags -fsanitize=thread -fno-omit-frame-pointer)
elseif(AIOS_SANITIZE STREQUAL "off" OR AIOS_SANITIZE STREQUAL "")
  set(_aios_san_flags "")
else()
  message(FATAL_ERROR "AIOS_SANITIZE must be one of: off, asan-ubsan, tsan (got '${AIOS_SANITIZE}')")
endif()

if(_aios_san_flags)
  message(STATUS "Sanitizers: ${AIOS_SANITIZE} (${_aios_san_flags})")
  add_compile_options(${_aios_san_flags})
  add_link_options(${_aios_san_flags})
  # Sanitizer runtimes are unhappy with fully optimized frames; keep debug info.
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(WARNING "AIOS_SANITIZE with CMAKE_BUILD_TYPE=Release; RelWithDebInfo or Debug gives usable reports")
  endif()
endif()
unset(_aios_san_flags)

# Emitted where the project's own warning flags are added (after the
# FetchContent dependencies, so third-party warnings never become errors).
function(aios_apply_werror)
  if(AIOS_WERROR)
    message(STATUS "Werror: ON")
    add_compile_options(-Werror)
  endif()
endfunction()
