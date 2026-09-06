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
