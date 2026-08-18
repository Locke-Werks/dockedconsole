cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DockVersion)

project(dockedconsole VERSION ${DOCK_VERSION} LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Docked Console targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# x64 only. Matching Forge: it removes WOW64 registry and Program Files
# redirection from the problem entirely. It also means SetWindowLongPtrW is
# always the right call, so the 32-bit SetWindowLongW fallback the C# version
# carried in Native.cs is simply deleted.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Docked Console is x64 only. Configure with an x64 toolchain.")
endif()

option(DOCK_WERROR    "Treat warnings as errors" OFF)
option(DOCK_HARDENING "Apply exploit-mitigation compiler and linker flags" ON)

include(DockWarnings)
include(DockHardening)
include(DockManifest)

# RC wants forward slashes or doubled backslashes in a path literal. CMake hands
# out forward slashes, so pass the path through rather than composing one in the
# template.
set(DOCK_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/dockedconsole.ico")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/dockedconsole.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dockedconsole.rc"
    @ONLY
)

# WIN32 is the subsystem, not the platform: it makes this a GUI binary so running
# it never opens a console of its own. That is why --stop and --help have to
# borrow the caller's console; see parent_console.
add_executable(dockedconsole WIN32
    src/main.cpp
    src/win32.cpp
    src/json.cpp
    src/config.cpp
    src/monitors.cpp
    src/diag_log.cpp
    src/elevation.cpp
    src/autostart.cpp
    src/parent_console.cpp
    src/app_bar.cpp
    src/enforcer.cpp
    src/taskbar_claim.cpp
    src/terminal.cpp
    src/tray.cpp
    src/dock_window.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/dockedconsole.rc"
)

target_include_directories(dockedconsole PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# The version reaches the code the same way it reaches the resource: from
# cmake/DockVersion.cmake, so the banner in the log cannot drift from the
# VERSIONINFO the installer reads.
target_compile_definitions(dockedconsole PRIVATE
    DOCK_VERSION_STRING="${DOCK_VERSION}"
)

# All in-box. Nothing here comes from a package manager, so a clone builds with
# nothing but Visual Studio and CMake installed.
target_link_libraries(dockedconsole PRIVATE
    user32
    shell32     # SHAppBarMessage, Shell_NotifyIconW, SHGetKnownFolderPath
    advapi32    # OpenProcessToken, GetTokenInformation, the HKCU Run key
    gdi32       # CreateSolidBrush for the dock background
    ole32       # CoTaskMemFree, for SHGetKnownFolderPath's output
    dwmapi      # DWMWA_EXTENDED_FRAME_BOUNDS and DWMWA_CLOAKED, the enforcer's eyes
    shlwapi
    pathcch     # long-path-safe path building; the manifest is longPathAware
)

dock_apply_warnings(dockedconsole)
dock_apply_hardening(dockedconsole)
dock_embed_manifest(dockedconsole "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")

# The release workflow signs whatever lands in stage/, then Forge packs it.
install(TARGETS dockedconsole RUNTIME DESTINATION .)

message(STATUS "Docked Console ${DOCK_VERSION}  hardening=${DOCK_HARDENING} werror=${DOCK_WERROR}")
