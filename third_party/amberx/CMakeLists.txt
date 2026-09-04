# AmberXCore — the audited X.Org device-independent core as a static library.
#
# OPT-IN (-DAMBERX_CORE=ON). It compiles 228 upstream files and needs the
# pinned trees fetched first, so it is not part of the ordinary AmberSSH
# build. Building it is the Phase 1 "reproducible build"; linking anything
# against it is Phase 2.
#
# Two gates run at configure time and each one FAILS the configure, not
# warns: a pinned tree whose HEAD is not the audited hash, and a source file
# outside the allowlisted subtrees. The rule from the prompt is an allowlist,
# so the check is "is this file in an approved directory", never "is it in a
# forbidden one".
if(NOT AMBERX_CORE)
    return()
endif()

set(AMBERX_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(AMBERX_UPSTREAM ${AMBERX_ROOT}/upstream)

# AmberSSH is a C++ project; the X.Org core is C. Without this the target
# has no language for its sources and CMake cannot pick a linker.
enable_language(C)

# ---- gate 1: provenance ------------------------------------------------------
# The hashes here duplicate docs/amberx/SOURCE-PROVENANCE.md on purpose: a
# document can be edited without anyone noticing; a configure failure cannot.
# '|' rather than ';' between tree and hash: a semicolon is CMake's list
# separator and an unquoted expansion would split each pin into two elements.
set(AMBERX_PINS
    "xserver|65d790bd208ec380b196eb98f144abb0b32e334d"
    "xorgproto|c18d2bc22813793bba7f0e4e603c0104d7724802"
    "pixman|9cc163c9da0fb4da430641715313d95a6ec466d9"
    "libxfont|975cc6526e892c6fad8fa49d0fef58565fbaa003"
    "libxkbfile|39a5f8e67615f443e76146769d5f5f9abc5ebd2f")
find_package(Git REQUIRED)
foreach(pin IN LISTS AMBERX_PINS)
    string(REPLACE "|" ";" parts "${pin}")
    list(GET parts 0 tree)
    list(GET parts 1 want)
    if(NOT EXISTS ${AMBERX_UPSTREAM}/${tree}/.git)
        message(FATAL_ERROR "AmberX: ${tree} is not fetched. Run third_party/amberx/fetch-upstream.sh")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${AMBERX_UPSTREAM}/${tree} rev-parse HEAD
                    OUTPUT_VARIABLE got OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT got STREQUAL want)
        message(FATAL_ERROR "AmberX: ${tree} is at ${got}, the audited tree is ${want}. Refusing to build an unaudited tree.")
    endif()
    # And the tree must be pristine: a local edit is a patch that is not in
    # the patch series, which SOURCE-PROVENANCE.md promises never happens.
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${AMBERX_UPSTREAM}/${tree} status --porcelain
                    OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(dirty)
        message(FATAL_ERROR "AmberX: ${tree} has local modifications:\n${dirty}\nUpstream trees are never edited in place.")
    endif()
endforeach()

# ---- gate 2: the allowlist ---------------------------------------------------
include(${AMBERX_ROOT}/AmberXCore-sources.cmake)
set(AMBERX_APPROVED_DIRS dix mi fb miext/damage miext/sync render randr xfixes
                         damageext composite present xkb Xi Xext)
set(AMBERX_EXCLUDED_FILES miext/sync/misyncshm.c)
set(AMBERX_XEXT_SUBSET Xext/bigreq.c Xext/shape.c Xext/sync.c Xext/xcmisc.c
                       Xext/xtest.c Xext/security.c Xext/hashtable.c Xext/geext.c
                       Xext/xace.c)
foreach(src ${AMBERX_CORE_SOURCES})
    file(RELATIVE_PATH rel ${AMBERX_UPSTREAM}/xserver ${src})
    get_filename_component(dir ${rel} DIRECTORY)
    if(NOT dir IN_LIST AMBERX_APPROVED_DIRS)
        message(FATAL_ERROR "AmberX licence gate: ${rel} is not in an allowlisted subtree (LICENSE-MATRIX.md)")
    endif()
    if(rel IN_LIST AMBERX_EXCLUDED_FILES)
        message(FATAL_ERROR "AmberX licence gate: ${rel} is explicitly excluded (REJECTED-COMPONENTS.md)")
    endif()
    if(dir STREQUAL "Xext" AND NOT rel IN_LIST AMBERX_XEXT_SUBSET)
        message(FATAL_ERROR "AmberX licence gate: ${rel} is not in the approved Xext subset")
    endif()
endforeach()
list(LENGTH AMBERX_CORE_SOURCES n)
message(STATUS "AmberX: provenance and allowlist gates passed for ${n} sources")

# ---- the library ---------------------------------------------------------------
# The Visual Studio generator adds /DWIN32 /D_WINDOWS to every C target.
# X.Org treats WIN32 as "the Cygwin/MinGW port": include/os.h:721 then does
# `typedef _sigset_t sigset_t;` expecting MinGW's signal.h, and Xos.h takes
# similar turns. MSVC has none of that, and the probe never defined WIN32 —
# it compiled all 229 files without it. Strip both so the library sees the
# flags the probe measured. Anything in Phase 2 that needs <windows.h> lives
# in its own translation units and never shares one with an X header.
string(REPLACE "/DWIN32" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REPLACE "/D_WINDOWS" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
add_library(AmberXCore STATIC ${AMBERX_CORE_SOURCES})
target_include_directories(AmberXCore PRIVATE
    ${AMBERX_ROOT}/config-msvc
    ${AMBERX_ROOT}/config-msvc/compat
    ${AMBERX_ROOT}/config-msvc/pixman
    ${AMBERX_UPSTREAM}/xserver/include
    ${AMBERX_UPSTREAM}/xserver/Xext
    ${AMBERX_UPSTREAM}/xserver/miext/damage
    ${AMBERX_UPSTREAM}/xserver/miext/sync
    ${AMBERX_UPSTREAM}/xserver/render
    ${AMBERX_UPSTREAM}/xserver/xkb
    ${AMBERX_UPSTREAM}/xserver/Xi
    ${AMBERX_UPSTREAM}/xserver/mi
    ${AMBERX_UPSTREAM}/xserver/fb
    ${AMBERX_UPSTREAM}/xserver/composite
    ${AMBERX_UPSTREAM}/xserver/randr
    ${AMBERX_UPSTREAM}/xserver/present
    ${AMBERX_UPSTREAM}/xserver/xfixes
    ${AMBERX_UPSTREAM}/xserver/damageext
    ${AMBERX_UPSTREAM}/xserver/dix
    ${AMBERX_UPSTREAM}/xorgproto/include
    ${AMBERX_UPSTREAM}/libxfont/include
    ${AMBERX_UPSTREAM}/libxkbfile/include
    ${AMBERX_UPSTREAM}/pixman/pixman)
target_compile_definitions(AmberXCore PRIVATE
    HAVE_DIX_CONFIG_H _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(AmberXCore PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
if(MSVC)
    # /W3 here, unlike the probe's /w: the probe measured errors; the build
    # is where warnings get counted and, eventually, fixed.
    target_compile_options(AmberXCore PRIVATE /W3 /utf-8 /TC)
endif()
