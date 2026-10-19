cmake_minimum_required(VERSION 3.20)

project(pipeduck
    VERSION 1.0.0
    DESCRIPTION "Automatic audio ducking for PipeWire, controlled from the system tray"
    HOMEPAGE_URL "https://github.com/ErRusso/pipeduck"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PIPEDUCK_BUILD_TESTS "Build the unit tests" ON)
option(PIPEDUCK_GUI "Build the GTK4 graphical tuner" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 glib-2.0)
pkg_check_modules(NCURSES REQUIRED IMPORTED_TARGET ncursesw)

if(PIPEDUCK_GUI)
    pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
    if(NOT GTK4_FOUND)
        message(WARNING "GTK4 not found; building without the graphical tuner")
        set(PIPEDUCK_GUI OFF)
    endif()
endif()

# Embed the tray pixmaps (ARGB) into a generated header.
set(ICON_SIZES 16 22 24 32 48 64)
set(ICON_FILES "")
foreach(size IN LISTS ICON_SIZES)
    list(APPEND ICON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/pixmap/pipeduck-${size}.argb")
    list(APPEND ICON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/pixmap/pipeduck-on-${size}.argb")
endforeach()
string(JOIN "," ICON_FILES_JOINED ${ICON_FILES})
set(ICON_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/pipeduck_icons.hpp")
add_custom_command(
    OUTPUT "${ICON_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DICONS=${ICON_FILES_JOINED}"
        -DOUTPUT=${ICON_HEADER}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedIcons.cmake"
    DEPENDS ${ICON_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedIcons.cmake"
    COMMENT "Embedding tray icon pixmaps")
add_custom_target(pipeduck_icons DEPENDS "${ICON_HEADER}")

add_library(pipeduck_core STATIC
    src/common.cpp
    src/config.cpp
    src/identity.cpp
    src/vad.cpp
    src/pulse.cpp
    src/volume.cpp
    src/pipewire.cpp
    src/routing.cpp
    src/duck.cpp
    src/icons.cpp
)
add_dependencies(pipeduck_core pipeduck_icons)
target_include_directories(pipeduck_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pipeduck_core
    PUBLIC PkgConfig::PIPEWIRE PkgConfig::PULSE PkgConfig::JSONC)
target_compile_definitions(pipeduck_core
    PRIVATE PIPEDUCK_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/icons")
target_compile_options(pipeduck_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(pipeduck
    src/main.cpp
    src/cli.cpp
    src/tray.cpp
    src/tune.cpp
)
target_include_directories(pipeduck PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pipeduck PRIVATE pipeduck_core PkgConfig::GIO PkgConfig::NCURSES)
target_compile_definitions(pipeduck PRIVATE PIPEDUCK_VERSION="${PROJECT_VERSION}")
target_compile_options(pipeduck PRIVATE -Wall -Wextra -Wpedantic)

if(PIPEDUCK_GUI)
    target_sources(pipeduck PRIVATE src/tune_gui.cpp)
    target_link_libraries(pipeduck PRIVATE PkgConfig::GTK4)
    target_compile_definitions(pipeduck PRIVATE PIPEDUCK_GUI=1)
endif()

include(GNUInstallDirs)
install(TARGETS pipeduck RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES assets/applications/pipeduck.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(DIRECTORY assets/icons/hicolor DESTINATION "${CMAKE_INSTALL_DATADIR}/icons")
install(DIRECTORY assets/icons/pixmap DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/pipeduck")
install(FILES docs/pipeduck.1 DESTINATION "${CMAKE_INSTALL_DATADIR}/man/man1")
install(FILES README.md LICENSE NOTICE CHANGELOG.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")

if(PIPEDUCK_BUILD_TESTS)
    enable_testing()
    add_executable(pipeduck_tests tests/test_core.cpp)
    target_link_libraries(pipeduck_tests PRIVATE pipeduck_core)
    target_compile_options(pipeduck_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME pipeduck_tests COMMAND pipeduck_tests)
endif()

message(STATUS "pipeduck ${PROJECT_VERSION}")
message(STATUS "  graphical tuner: ${PIPEDUCK_GUI}")
message(STATUS "  unit tests:      ${PIPEDUCK_BUILD_TESTS}")
