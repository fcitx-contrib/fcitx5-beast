cmake_minimum_required(VERSION 3.27)

project(fcitx5-beast VERSION 0.1.0)

find_package(Gettext REQUIRED)
find_package(Fcitx5Core 5 REQUIRED)
find_package(Boost 1.83 REQUIRED)

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_subdirectory(src)
