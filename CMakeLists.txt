cmake_minimum_required(VERSION 3.20)
project(AmberSSH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH REQUIRED libssh)

add_executable(AmberSSH
    src/main.cpp
    src/MainWindow.cpp
    src/SshClient.cpp
    src/TerminalEmulator.cpp
    src/Logging.cpp
)

target_include_directories(AmberSSH PRIVATE include)

target_link_libraries(AmberSSH PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    ${LIBSSH_LIBRARIES}
)

target_compile_definitions(AmberSSH PRIVATE
    QT_NO_KEYWORDS
)

install(TARGETS AmberSSH RUNTIME DESTINATION bin)
