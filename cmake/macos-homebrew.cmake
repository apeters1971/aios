# Homebrew search paths for macOS builds.
#
# Included from the top-level CMakeLists.txt on APPLE only. It makes the default
# `cmake -S . -B build` work on a Homebrew machine without a long
# -DCMAKE_PREFIX_PATH, while keeping all machine-specific paths out of the main
# build description. Override the prefix with -DAIOS_HOMEBREW_PREFIX=... or the
# HOMEBREW_PREFIX environment variable; set -DAIOS_HOMEBREW_PREFIX="" to disable.

if(NOT APPLE)
  return()
endif()

if(NOT DEFINED AIOS_HOMEBREW_PREFIX)
  if(DEFINED ENV{HOMEBREW_PREFIX} AND IS_DIRECTORY "$ENV{HOMEBREW_PREFIX}")
    set(_aios_brew "$ENV{HOMEBREW_PREFIX}")
  elseif(IS_DIRECTORY /opt/homebrew/opt)
    set(_aios_brew /opt/homebrew)          # Apple silicon default
  elseif(IS_DIRECTORY /usr/local/Homebrew)
    set(_aios_brew /usr/local)             # Intel default
  else()
    set(_aios_brew "")
  endif()
  set(AIOS_HOMEBREW_PREFIX "${_aios_brew}" CACHE PATH
    "Homebrew prefix used to locate Boost/OpenSSL/SQLite/zstd/ISA-L/fuse (empty = off)")
  unset(_aios_brew)
endif()

if(AIOS_HOMEBREW_PREFIX STREQUAL "")
  return()
endif()

message(STATUS "Homebrew: using prefix ${AIOS_HOMEBREW_PREFIX}")
list(APPEND CMAKE_PREFIX_PATH "${AIOS_HOMEBREW_PREFIX}")

# Keg-only formulas are not linked into ${prefix}/{include,lib}; add their opt/ dirs.
foreach(_keg sqlite openssl@3 isa-l zstd boost)
  if(IS_DIRECTORY "${AIOS_HOMEBREW_PREFIX}/opt/${_keg}")
    list(APPEND CMAKE_PREFIX_PATH "${AIOS_HOMEBREW_PREFIX}/opt/${_keg}")
  endif()
endforeach()
unset(_keg)

# Let pkg-config see keg-only .pc files too (fuse3 / libisal / libzstd fallbacks).
if(IS_DIRECTORY "${AIOS_HOMEBREW_PREFIX}/lib/pkgconfig")
  set(ENV{PKG_CONFIG_PATH} "${AIOS_HOMEBREW_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
