find_package(Catch2 3 CONFIG REQUIRED)

# The terminal core is compiled directly into the test target. It only needs the
# Windows/D3D12 *headers* (for shared types in common.h), never the runtime, so
# linking the import libs is enough.
add_executable(AmberTests
    AnsiParserTests.cpp
    ColorSgrTests.cpp
    TerminalBufferTests.cpp
    SelectionTests.cpp
    ProfileStoreTests.cpp
    SecureStringTests.cpp
    ShaderContractTests.cpp
    SyntaxTintTests.cpp
    CommandJournalTests.cpp
    WorkspaceTests.cpp
    PasteGuardTests.cpp
    LocalSessionTests.cpp
    GuardianTests.cpp
    CommandBlockTests.cpp
    CompatFixtureTests.cpp
    PaneLayoutTests.cpp
    SyncPlanTests.cpp
    RemoteNameTests.cpp
    RemoteInputFuzzTests.cpp
    HostKeyDecisionTests.cpp
    DownloadPlanTests.cpp
    CredentialStoreTests.cpp
    ContrastTests.cpp
    HostSigilTests.cpp
    BlastRadiusTests.cpp
    PrivacyCloakTests.cpp
    XAuthTests.cpp
    RemoteDisplayTests.cpp
    AmberXProtocolTests.cpp
    AmberXFuzzTests.cpp
    AmberXHandshakeTests.cpp
    VncDesTests.cpp
    VncProtocolTests.cpp
    VncClientTests.cpp
    VncSessionTests.cpp
    VncKeysymTests.cpp
    VncTlsTests.cpp

    ${CMAKE_SOURCE_DIR}/src/vnc/Keysyms.cpp
    ${CMAKE_SOURCE_DIR}/src/vnc/RfbTls.cpp
    ${CMAKE_SOURCE_DIR}/src/vnc/RfbDes.cpp
    ${CMAKE_SOURCE_DIR}/src/vnc/RfbProtocol.cpp
    ${CMAKE_SOURCE_DIR}/src/vnc/RfbDecoders.cpp
    ${CMAKE_SOURCE_DIR}/src/vnc/RfbClient.cpp
    ${CMAKE_SOURCE_DIR}/src/vnc/VncSession.cpp
    ${CMAKE_SOURCE_DIR}/src/term/vtparser.cpp
    ${CMAKE_SOURCE_DIR}/src/term/grid.cpp
    ${CMAKE_SOURCE_DIR}/src/term/charwidth.cpp
    ${CMAKE_SOURCE_DIR}/src/term/Graphemes.cpp
    ${CMAKE_SOURCE_DIR}/src/term/ImageDecode.cpp
    ${CMAKE_SOURCE_DIR}/src/term/input.cpp
    ${CMAKE_SOURCE_DIR}/src/term/SyntaxTint.cpp
    ${CMAKE_SOURCE_DIR}/src/utility/SecureString.cpp
    ${CMAKE_SOURCE_DIR}/src/utility/Hash.cpp
    ${CMAKE_SOURCE_DIR}/src/ssh/SyncPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/ssh/RemoteName.cpp
    ${CMAKE_SOURCE_DIR}/src/ssh/HostKeyDecision.cpp
    ${CMAKE_SOURCE_DIR}/src/ssh/DownloadPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/Paths.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/CredentialStore.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/ConPty.cpp
    ${CMAKE_SOURCE_DIR}/src/profiles/ProfileStore.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Chrome.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/CommandJournal.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/Guardian.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/CommandBlocks.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/PaneLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/Workspaces.cpp
    ${CMAKE_SOURCE_DIR}/src/security/HostSigil.cpp
    ${CMAKE_SOURCE_DIR}/src/security/BlastRadius.cpp
    ${CMAKE_SOURCE_DIR}/src/security/PrivacyCloak.cpp
    ${CMAKE_SOURCE_DIR}/src/remote/XAuth.cpp
    ${CMAKE_SOURCE_DIR}/src/remote/RemoteDisplay.cpp
    ${CMAKE_SOURCE_DIR}/src/amberx/control/Protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/amberx/control/Handshake.cpp
)

target_include_directories(AmberTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(AmberTests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    AMBER_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
    # so the fuzz corpus is found whatever directory the tests run from
    AMBER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(AmberTests PRIVATE /W3 /utf-8 /EHsc /MP)
    # -DAMBER_ASAN=ON builds the tests under AddressSanitizer. It is off by
    # default because it roughly triples the run time and needs the ASAN
    # runtime DLL beside the executable, and on for the job that matters: the
    # fuzz targets, where "it did not crash" is a much weaker statement
    # without it. Incremental linking is incompatible with /fsanitize.
    if(AMBER_ASAN)
        target_compile_options(AmberTests PRIVATE /fsanitize=address /Zi)
        target_link_options(AmberTests PRIVATE /INCREMENTAL:NO /DEBUG)
        # Catch2 comes from vcpkg built without ASAN, so its objects carry
        # annotate_string/annotate_vector = 0 and ours would carry 1; the
        # linker refuses the mixture. Turning the container annotations off
        # on our side matches it.
        #
        # What that costs: overflow *within* a std::string or std::vector's
        # spare capacity is no longer caught. What it keeps, which is the
        # reason this exists: heap overflow and underflow, use-after-free,
        # double free, stack-buffer overflow, and global overflow — every one
        # of which applies to the raw buffers the parsers actually use.
        target_compile_definitions(AmberTests PRIVATE
            _DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
        message(STATUS "AmberTests: AddressSanitizer enabled (container annotations off)")
        # The ASAN runtime is a DLL and it is not on PATH. Without it beside
        # the executable the tests do not start at all, with an error that
        # says nothing about sanitizers.
        find_file(AMBER_ASAN_DLL clang_rt.asan_dynamic-x86_64.dll
                  HINTS "$ENV{VCToolsInstallDir}/bin/Hostx64/x64"
                        "${CMAKE_CXX_COMPILER}/../"
                  NO_CACHE)
        if(AMBER_ASAN_DLL)
            add_custom_command(TARGET AmberTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${AMBER_ASAN_DLL}" "$<TARGET_FILE_DIR:AmberTests>")
        else()
            message(WARNING "ASAN runtime DLL not found; copy clang_rt.asan_dynamic-x86_64.dll "
                            "next to AmberTests.exe by hand or it will not start.")
        endif()
    endif()
endif()

find_package(ZLIB REQUIRED)   # the VNC ZRLE decoder, and the tests that deflate fixtures for it
find_package(OpenSSL REQUIRED) # VeNCrypt's TLS client, and the in-process TLS server the tests stand up
target_link_libraries(AmberTests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB
    OpenSSL::SSL OpenSSL::Crypto crypt32
    d3d12 dxgi dxguid advapi32 bcrypt shell32 ole32 ws2_32
)

include(Catch)
catch_discover_tests(AmberTests)
