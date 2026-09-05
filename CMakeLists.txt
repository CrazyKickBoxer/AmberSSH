cmake_minimum_required(VERSION 3.24)
project(AmberSSH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "AmberSSH Native is a Windows 11 application.")
endif()

# Non-rendering core: reused verbatim by the unit-test target.
set(AMBER_CORE_SOURCES
    src/utility/SecureString.h   src/utility/SecureString.cpp
    src/platform/Paths.h         src/platform/Paths.cpp
    src/platform/CredentialStore.h src/platform/CredentialStore.cpp
    src/profiles/ConnectionProfile.h
    src/profiles/ProfileStore.h   src/profiles/ProfileStore.cpp
    src/sessions/CommandJournal.h src/sessions/CommandJournal.cpp
    src/sessions/Guardian.h      src/sessions/Guardian.cpp
    src/sessions/CommandBlocks.h src/sessions/CommandBlocks.cpp
    src/sessions/PaneLayout.h    src/sessions/PaneLayout.cpp
    src/sessions/Workspaces.h    src/sessions/Workspaces.cpp
    src/term/SyntaxTint.h        src/term/SyntaxTint.cpp
    src/sessions/Session.h
    src/ui/ConnectionDialog.h    src/ui/ConnectionDialog.cpp
    src/ui/SftpPanel.h           src/ui/SftpPanel.cpp
)

set(AMBER_SOURCES
    src/main.cpp
    src/app.h              src/app.cpp
    src/platform/Taskbar.h src/platform/Taskbar.cpp
    src/common.h
    src/dx/device.h        src/dx/device.cpp
    src/dx/frame.h         src/dx/frame.cpp
    src/dx/shaders.h       src/dx/shaders.cpp
    src/render/particles.h src/render/particles.cpp
    src/render/desktop.h   src/render/desktop.cpp
    src/render/bloom.h     src/render/bloom.cpp
    src/render/composite.h src/render/composite.cpp
    src/render/prims.h     src/render/prims.cpp
    src/term/vtparser.h    src/term/vtparser.cpp
    src/term/grid.h        src/term/grid.cpp
    src/term/charwidth.h   src/term/charwidth.cpp
    src/term/Graphemes.h   src/term/Graphemes.cpp
    src/term/ImageDecode.h src/term/ImageDecode.cpp
    src/platform/AudioLevel.h src/platform/AudioLevel.cpp
    src/platform/Notify.h     src/platform/Notify.cpp
    src/platform/JumpList.h   src/platform/JumpList.cpp
    src/platform/Hello.h      src/platform/Hello.cpp
    src/platform/ConPty.h     src/platform/ConPty.cpp
    src/ssh/SftpClient.h      src/ssh/SftpClient.cpp
    src/ui/SftpBrowser.h      src/ui/SftpBrowser.cpp
    src/ui/ForwardsDialog.h   src/ui/ForwardsDialog.cpp
    src/ui/Chrome.h           src/ui/Chrome.cpp
    src/ui/AboutDialog.h      src/ui/AboutDialog.cpp
    src/ui/SafetyDialog.h     src/ui/SafetyDialog.cpp
    src/ssh/Vitals.h          src/ssh/Vitals.cpp
    src/ssh/SyncPlan.h        src/ssh/SyncPlan.cpp
    src/utility/Hash.h        src/utility/Hash.cpp
    src/security/HostSigil.h     src/security/HostSigil.cpp
    src/security/BlastRadius.h   src/security/BlastRadius.cpp
    src/security/PrivacyCloak.h  src/security/PrivacyCloak.cpp
    src/remote/XAuth.h           src/remote/XAuth.cpp
    src/remote/RemoteDisplay.h   src/remote/RemoteDisplay.cpp
    src/amberx/control/Protocol.h src/amberx/control/Protocol.cpp
    src/amberx/control/Handshake.h src/amberx/control/Handshake.cpp
    src/amberx/control/Pipe.h     src/amberx/control/Pipe.cpp
    src/amberx/AmberXController.h src/amberx/AmberXController.cpp src/amberx/PreviewClient.cpp
    src/term/palette.h
    src/term/input.h       src/term/input.cpp
    src/ssh/ring.h
    src/ssh/session.h      src/ssh/session.cpp
    src/ssh/transport.h    src/ssh/transports.cpp
    src/vnc/RfbDes.h       src/vnc/RfbDes.cpp
    src/vnc/RfbProtocol.h  src/vnc/RfbProtocol.cpp
    src/vnc/RfbDecoders.h  src/vnc/RfbDecoders.cpp
    src/vnc/RfbClient.h    src/vnc/RfbClient.cpp
    src/vnc/VncSession.h   src/vnc/VncSession.cpp
    src/vnc/Keysyms.h      src/vnc/Keysyms.cpp
    src/vnc/SelfCheckServer.h src/vnc/SelfCheckServer.cpp
    src/sessions/VncTab.h  src/app_vnc.cpp   src/app_vnccheck.cpp
    src/glyphs/sampler.h   src/glyphs/sampler.cpp
    ${AMBER_CORE_SOURCES}
)

add_executable(AmberSSH WIN32 ${AMBER_SOURCES} app.manifest assets/AmberSSH.rc)

target_include_directories(AmberSSH PRIVATE src assets)
target_compile_definitions(AmberSSH PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
    # Warnings are errors for project sources; vcpkg headers are not affected.
    # Symbols in every configuration so a crash record's offset resolves to
    # a function and line (the PDB sits next to the exe; not shipped).
    target_link_options(AmberSSH PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    target_compile_options(AmberSSH PRIVATE /W4 /WX /utf-8 /EHsc /MP /Zi
        /wd4201  # nameless struct/union: used by the D3D12 headers
        /wd4324  # structure padded due to alignas
    )
endif()

# ---------------------------------------------------------------- dependencies
find_package(OpenSSL REQUIRED)
find_package(libssh2 CONFIG QUIET)
if(NOT libssh2_FOUND)
    find_package(Libssh2 CONFIG REQUIRED)
endif()
if(TARGET libssh2::libssh2_shared)
    set(AMBER_LIBSSH2 libssh2::libssh2_shared)
elseif(TARGET libssh2::libssh2_static)
    set(AMBER_LIBSSH2 libssh2::libssh2_static)
elseif(TARGET libssh2::libssh2)
    set(AMBER_LIBSSH2 libssh2::libssh2)
else()
    set(AMBER_LIBSSH2 Libssh2::libssh2)
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(directx-dxc CONFIG REQUIRED)        # Microsoft::DirectXShaderCompiler + dxc tool

# zlib is already in the dependency closure through libssh2; VNC's ZRLE
# decoder links it directly (see THIRD-PARTY-NOTICES.md).
find_package(ZLIB REQUIRED)
target_link_libraries(AmberSSH PRIVATE
    ${AMBER_LIBSSH2}
    OpenSSL::SSL OpenSSL::Crypto
    ZLIB::ZLIB
    Microsoft::DirectXShaderCompiler
    d3d12 dxgi dxguid dwrite dwmapi
    nlohmann_json::nlohmann_json
    ws2_32 crypt32 user32 gdi32 shell32 ole32 advapi32 bcrypt comctl32 shlwapi
)

# ------------------------------------------------------- build-time DXC shaders
# Shaders are compiled to DXIL (.cso) at build time with DXC. At runtime the
# engine loads shaders/<stem>.<entry>.cso; if a .cso is missing it falls back to
# runtime compilation of shaders/<stem>.hlsl through dxcompiler.dll.
set(AMBER_SHADER_BIN ${CMAKE_CURRENT_BINARY_DIR}/shaders_bin)
file(MAKE_DIRECTORY ${AMBER_SHADER_BIN})
set(AMBER_CSO_OUTPUTS "")

function(amber_shader HLSL ENTRY PROFILE)
    get_filename_component(STEM ${HLSL} NAME_WE)
    set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${HLSL})
    set(OUT ${AMBER_SHADER_BIN}/${STEM}.${ENTRY}.cso)
    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${DIRECTX_DXC_TOOL} -T ${PROFILE} -E ${ENTRY}
                -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                $<$<CONFIG:Debug>:-Zi> $<$<CONFIG:Debug>:-Qembed_debug> -O3
                -Fo ${OUT} ${SRC}
        DEPENDS ${SRC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/amber_common.hlsli
        COMMENT "DXC ${HLSL} : ${ENTRY} (${PROFILE})"
        VERBATIM)
    set(AMBER_CSO_OUTPUTS ${AMBER_CSO_OUTPUTS} ${OUT} PARENT_SCOPE)
endfunction()

amber_shader(particle_sim.hlsl   CSMain      cs_6_0)
amber_shader(particle_draw.hlsl  VSMain      vs_6_0)
amber_shader(particle_draw.hlsl  PSMain      ps_6_0)
# the VNC particle desktop (shaders/desktop_*.hlsl, motion_fields.hlsli)
amber_shader(desktop_energy.hlsl CSMain      cs_6_0)
amber_shader(desktop_sim.hlsl    CSMain      cs_6_0)
amber_shader(desktop_draw.hlsl   VSMain      vs_6_0)
amber_shader(desktop_draw.hlsl   PSMain      ps_6_0)
amber_shader(bloom_down.hlsl     CSPrefilter cs_6_0)
amber_shader(bloom_down.hlsl     CSDown      cs_6_0)
amber_shader(bloom_up.hlsl       CSUp        cs_6_0)
amber_shader(composite.hlsl      VSMain      vs_6_0)
amber_shader(composite.hlsl      PSMain      ps_6_0)
amber_shader(rects.hlsl          VSMain      vs_6_0)
amber_shader(rects.hlsl          PSMain      ps_6_0)
amber_shader(text_draw.hlsl      VSMain      vs_6_0)
amber_shader(text_draw.hlsl      PSMain      ps_6_0)
amber_shader(emoji_draw.hlsl     VSMain      vs_6_0)
amber_shader(emoji_draw.hlsl     PSMain      ps_6_0)

add_custom_target(AmberShaders ALL DEPENDS ${AMBER_CSO_OUTPUTS})
add_dependencies(AmberSSH AmberShaders)

# Deploy shader sources + compiled DXIL next to the executable.
add_custom_command(TARGET AmberSSH POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:AmberSSH>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${AMBER_SHADER_BIN} $<TARGET_FILE_DIR:AmberSSH>/shaders
    COMMENT "Deploying shaders")

# Deploy bundled fonts (open-source terminal/futuristic faces) next to the exe.
# The sampler loads exe\fonts into a private DirectWrite collection, so these
# work without a system install.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/fonts)
    add_custom_command(TARGET AmberSSH POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/fonts $<TARGET_FILE_DIR:AmberSSH>/fonts
        COMMENT "Deploying bundled fonts")
endif()


# ------------------------------------------------------------------------ tests
enable_testing()
add_subdirectory(tests)

# ---- AmberXHost: the isolated per-session process (docs/amberx/) ----------
# A console executable, launched by AmberSSH into a Job Object and reached only
# over a DACL'd named pipe. Shares the protocol/handshake/pipe sources with the
# main target rather than a library, so there is exactly one build of each.
add_executable(AmberXHost
    src/amberx/host/main.cpp
    src/amberx/control/Protocol.cpp
    src/amberx/control/Handshake.cpp
    src/amberx/control/Pipe.cpp
    src/utility/Hash.cpp
)
target_include_directories(AmberXHost PRIVATE src)
target_compile_definitions(AmberXHost PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(AmberXHost PRIVATE /W3 /utf-8 /EHsc /MP)
endif()
target_link_libraries(AmberXHost PRIVATE bcrypt advapi32 dbghelp)
# With the X.Org core built (-DAMBERX_CORE=ON), the host carries the server:
# the Windows backend joins the target and the four static libraries link in.
# Without it, main.cpp compiles to the transport-only stub, as before.
if(AMBERX_CORE)
    target_sources(AmberXHost PRIVATE src/amberx/host/WinBackend.cpp
                                      src/amberx/host/WinKeymap.cpp
                                      src/ui/Chrome.cpp)
    target_compile_definitions(AmberXHost PRIVATE AMBERX_HAVE_SERVER)
    # shcore: GetDpiForMonitor, so each monitor reports its own scale to RANDR
    target_link_libraries(AmberXHost PRIVATE AmberXServer AmberXCore AmberXPixman AmberXFont
                                             gdi32 user32 shcore)
    # a linker map, so an exception offset in the host log resolves to a symbol
    target_link_options(AmberXHost PRIVATE /MAP)
endif()
# Built alongside AmberSSH.exe so the controller finds it next to itself.
add_dependencies(AmberSSH AmberXHost)

# ---- AmberXCore: the audited X.Org core, opt-in (docs/amberx/PHASE-1-GATE.md)
# OFF by default: it compiles 228 upstream files and needs the pinned trees
# fetched by third_party/amberx/fetch-upstream.sh. Its own CMakeLists runs
# the provenance and licence-allowlist gates at configure time.
option(AMBERX_CORE "Build the audited X.Org core as AmberXCore (requires fetch-upstream.sh)" OFF)
add_subdirectory(third_party/amberx)
