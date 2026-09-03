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

    ${CMAKE_SOURCE_DIR}/src/term/vtparser.cpp
    ${CMAKE_SOURCE_DIR}/src/term/grid.cpp
    ${CMAKE_SOURCE_DIR}/src/term/charwidth.cpp
    ${CMAKE_SOURCE_DIR}/src/term/ImageDecode.cpp
    ${CMAKE_SOURCE_DIR}/src/term/input.cpp
    ${CMAKE_SOURCE_DIR}/src/term/SyntaxTint.cpp
    ${CMAKE_SOURCE_DIR}/src/utility/SecureString.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/Paths.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/CredentialStore.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/ConPty.cpp
    ${CMAKE_SOURCE_DIR}/src/profiles/ProfileStore.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Chrome.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/CommandJournal.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/Guardian.cpp
    ${CMAKE_SOURCE_DIR}/src/sessions/Workspaces.cpp
)

target_include_directories(AmberTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(AmberTests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    AMBER_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
)
if(MSVC)
    target_compile_options(AmberTests PRIVATE /W3 /utf-8 /EHsc /MP)
endif()

target_link_libraries(AmberTests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    d3d12 dxgi dxguid advapi32 bcrypt shell32 ole32
)

include(Catch)
catch_discover_tests(AmberTests)
