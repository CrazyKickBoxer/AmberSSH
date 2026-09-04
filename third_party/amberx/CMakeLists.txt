# AmberXCore — the audited X.Org device-independent core as a static library.
#
# OPT-IN (-DAMBERX_CORE=ON). It compiles the allowlisted upstream files and needs the
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
set(AMBERX_APPROVED_DIRS dix mi fb miext/damage miext/sync miext/rootless render randr xfixes
                         damageext composite present xkb Xi Xext)
set(AMBERX_EXCLUDED_FILES miext/sync/misyncshm.c xkb/ddxLoad.c)
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
    ${AMBERX_UPSTREAM}/xserver/miext/rootless
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

# ---- pixman ---------------------------------------------------------------------
# The portable C paths only. pixman's SIMD files need per-file /arch flags and
# a runtime dispatcher; that is an optimisation for a later phase, and until it
# is measured the general implementation is the honest baseline. pixman is
# MSVC-aware natively (thread-local storage, force-inline) and needs no
# generated config when HAVE_CONFIG_H is left undefined.
set(AMBERX_PIXMAN_DIR ${AMBERX_UPSTREAM}/pixman/pixman)
set(AMBERX_PIXMAN_SOURCES
    pixman.c pixman-access.c pixman-access-accessors.c pixman-bits-image.c
    pixman-combine32.c pixman-combine-float.c pixman-conical-gradient.c
    pixman-edge.c pixman-edge-accessors.c pixman-fast-path.c pixman-filter.c
    pixman-general.c pixman-glyph.c pixman-gradient-walker.c pixman-image.c
    pixman-implementation.c pixman-linear-gradient.c pixman-matrix.c
    pixman-noop.c pixman-radial-gradient.c pixman-region16.c pixman-region32.c
    pixman-region64f.c pixman-solid-fill.c pixman-timer.c pixman-trap.c
    pixman-utils.c pixman-x86.c pixman-arm.c pixman-mips.c pixman-ppc.c pixman-riscv.c)
list(TRANSFORM AMBERX_PIXMAN_SOURCES PREPEND ${AMBERX_PIXMAN_DIR}/)
add_library(AmberXPixman STATIC ${AMBERX_PIXMAN_SOURCES})
target_include_directories(AmberXPixman PUBLIC ${AMBERX_PIXMAN_DIR} ${AMBERX_ROOT}/config-msvc/pixman)
target_compile_definitions(AmberXPixman PRIVATE PACKAGE="pixman" _CRT_SECURE_NO_WARNINGS)
set_target_properties(AmberXPixman PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(AmberXPixman PRIVATE /W3 /utf-8 /TC)
endif()

# ---- libXfont2 ---------------------------------------------------------------------
# The subset the core actually calls: pattern cache, font names, glyph
# extents, and the built-in "fixed"/"cursor" fonts the server cannot start
# without. No FreeType (a dependency and a second licence), no font-server
# client (sockets), no compressed font files (zlib/bzip2), no catalogue
# directories (realpath). Every excluded piece is out for scope, not licence.
set(AMBERX_XFONT_DIR ${AMBERX_UPSTREAM}/libxfont/src)
set(AMBERX_XFONT_SOURCES
    fontfile/bitsource.c fontfile/bufio.c fontfile/decompress.c fontfile/defaults.c fontfile/dirfile.c fontfile/gunzip.c
    fontfile/fileio.c fontfile/filewr.c fontfile/fontdir.c fontfile/fontfile.c
    fontfile/fontscale.c fontfile/register.c fontfile/renderers.c
    bitmap/bdfread.c bitmap/bdfutils.c bitmap/bitmap.c bitmap/bitmapfunc.c
    bitmap/bitmaputil.c bitmap/bitscale.c bitmap/fontink.c bitmap/pcfread.c
    bitmap/pcfwrite.c bitmap/snfread.c
    builtins/dir.c builtins/file.c builtins/fonts.c builtins/fpe.c builtins/render.c
    util/fontaccel.c util/fontnames.c util/fontutil.c util/fontxlfd.c
    util/format.c util/miscutil.c util/patcache.c util/private.c util/reallocarray.c
    util/utilbitmap.c
    stubs/atom.c stubs/libxfontstubs.c)
list(TRANSFORM AMBERX_XFONT_SOURCES PREPEND ${AMBERX_XFONT_DIR}/)
add_library(AmberXFont STATIC ${AMBERX_XFONT_SOURCES})
target_include_directories(AmberXFont
    PUBLIC  ${AMBERX_UPSTREAM}/libxfont/include ${AMBERX_UPSTREAM}/xorgproto/include
    PRIVATE ${AMBERX_UPSTREAM}/libxfont ${AMBERX_XFONT_DIR} ${AMBERX_ROOT}/config-msvc/compat)
# strlcpy/strlcat are the server's os/ definitions; reallocarray is libXfont2's own
# (the server's is xreallocarray). Both declared by src/util/replace.h.
target_compile_definitions(AmberXFont PRIVATE
    XFONT_FONTFILE XFONT_BITMAP XFONT_BUILTINS XFONT_PCFFORMAT XFONT_SNFFORMAT
    XFONT_BDFFORMAT X_GZIP_FONT_COMPRESSION
    # O_NOFOLLOW guards a symlink race on POSIX open(); the CRT open() has no
    # such flag. Defined away rather than shimmed: the only directories this
    # server ever opens are ones it was launched with, never one a client names.
    O_NOFOLLOW=0
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
# The built-in fonts (fixed, cursor) are stored gzip-compressed inside
# libXfont2 itself, so zlib is a requirement of the runtime closure, not an
# option. It is the same zlib AmberSSH already ships for libssh2.
find_package(ZLIB REQUIRED)
target_link_libraries(AmberXFont PUBLIC ZLIB::ZLIB)
set_target_properties(AmberXFont PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(AmberXFont PRIVATE /W3 /utf-8 /TC /FI${AMBERX_ROOT}/config-msvc/xfont-compat.h)
endif()

# ---- AmberXServer: AmberX's own os and DDX layers, plus retained os/ files ----
# The X side of AmberXHost (docs/amberx/PHASE-2-SPEC.md). C, compiled against
# the X headers, never against <windows.h>: everything it needs from Windows
# goes through src/amberx/server/amberwin.h.
#
# The retained os/ files are the pure-C ones the port keeps unmodified. This
# list is its own allowlist: an os/ file not named here is not compiled, and
# the licence matrix records os/ as "approved — subset" on exactly this basis.
set(AMBERX_OS_RETAINED
    os/mitauth.c os/oscolor.c os/xprintf.c os/strlcpy.c os/strlcat.c
    os/strndup.c os/reallocarray.c os/timingsafe_memcmp.c)
# Not retained: os/xsha1.c (its CryptoAPI path drags <windows.h> into an X
# translation unit; x_sha1_* is implemented on the Windows side over BCrypt)
# and os/strcasecmp.c (BSD u_char; two one-line CRT mappings in os_misc.c).
list(TRANSFORM AMBERX_OS_RETAINED PREPEND ${AMBERX_UPSTREAM}/xserver/)
set(AMBERX_SERVER_DIR ${CMAKE_SOURCE_DIR}/src/amberx/server)
add_library(AmberXServer STATIC
    ${AMBERX_SERVER_DIR}/os_alloc.c
    ${AMBERX_SERVER_DIR}/os_log.c
    ${AMBERX_SERVER_DIR}/os_timer.c
    ${AMBERX_SERVER_DIR}/os_auth.c
    ${AMBERX_SERVER_DIR}/os_misc.c
    ${AMBERX_SERVER_DIR}/os_connection.c
    ${AMBERX_SERVER_DIR}/ddx_screen.c
    ${AMBERX_SERVER_DIR}/ddx_input.c
    ${AMBERX_SERVER_DIR}/ddx_keymap.c
    ${AMBERX_SERVER_DIR}/ddx_frames.c
    ${AMBERX_SERVER_DIR}/ddx_wm.c
    ${AMBERX_SERVER_DIR}/server_main.c
    ${AMBERX_OS_RETAINED})
target_include_directories(AmberXServer PRIVATE
    ${AMBERX_SERVER_DIR}
    ${AMBERX_UPSTREAM}/xserver/os)
# the same view of the tree the core has
get_target_property(_core_inc AmberXCore INCLUDE_DIRECTORIES)
target_include_directories(AmberXServer PRIVATE ${_core_inc})
target_compile_definitions(AmberXServer PRIVATE
    HAVE_DIX_CONFIG_H _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS)
set_target_properties(AmberXServer PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(AmberXServer PRIVATE /W3 /utf-8 /TC)
endif()

# ---- AmberXFontProbe: opens the built-in fonts through libXfont2 alone -----
add_executable(AmberXFontProbe
    ${CMAKE_SOURCE_DIR}/tests/amberx/FontProbe.c
    ${AMBERX_UPSTREAM}/xserver/os/strlcpy.c
    ${AMBERX_UPSTREAM}/xserver/os/strlcat.c)
target_include_directories(AmberXFontProbe PRIVATE ${_core_inc} ${AMBERX_UPSTREAM}/libxfont/include)
target_compile_definitions(AmberXFontProbe PRIVATE HAVE_DIX_CONFIG_H _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_link_libraries(AmberXFontProbe PRIVATE AmberXFont ZLIB::ZLIB)
set_target_properties(AmberXFontProbe PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(AmberXFontProbe PRIVATE /W3 /utf-8 /TC)
endif()
