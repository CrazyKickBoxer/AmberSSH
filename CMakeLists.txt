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
    src/render/bloom.h     src/render/bloom.cpp
    src/render/composite.h src/render/composite.cpp
    src/render/prims.h     src/render/prims.cpp
    src/term/vtparser.h    src/term/vtparser.cpp
    src/term/grid.h        src/term/grid.cpp
    src/term/charwidth.h   src/term/charwidth.cpp
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
    src/ssh/Vitals.h          src/ssh/Vitals.cpp
    src/term/palette.h
    src/term/input.h       src/term/input.cpp
    src/ssh/ring.h
    src/ssh/session.h      src/ssh/session.cpp
    src/ssh/transport.h    src/ssh/transports.cpp
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

target_link_libraries(AmberSSH PRIVATE
    ${AMBER_LIBSSH2}
    OpenSSL::SSL OpenSSL::Crypto
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
