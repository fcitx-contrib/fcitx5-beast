cmake_minimum_required(VERSION 3.27)

project(fcitx5-beast VERSION 0.1.0)

include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_TEST "Build Test" On)
set(ADDON_TYPE "StaticLibrary" CACHE STRING "StaticLibrary or SharedLibrary")

if (NOT "${ADDON_TYPE}" MATCHES "^(StaticLibrary|SharedLibrary)$")
  message(FATAL_ERROR "ADDON_TYPE must be StaticLibrary or SharedLibrary")
endif()

find_package(Gettext REQUIRED)
find_package(Fcitx5Core 5 REQUIRED)
find_package(Boost 1.83 REQUIRED)

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_subdirectory(src)

if (ENABLE_TEST)
    enable_testing()
    add_subdirectory(test)
endif()
