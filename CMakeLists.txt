cmake_minimum_required(VERSION 3.25)
project(WaylandShader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ECM 6.22 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs6)
include(KDECMakeSettings)
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets DBus Concurrent CorePrivate)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6Config REQUIRED)
find_package(KWin 6.6 REQUIRED)
if(KWin_VERSION VERSION_GREATER_EQUAL "6.7")
    message(FATAL_ERROR "This effect targets the KWin 6.6 rendering API. KWin 6.7 requires a separate API port.")
endif()
find_package(epoxy REQUIRED)
find_path(LIBRASHADER_INCLUDE_DIR librashader.h PATH_SUFFIXES librashader REQUIRED)
find_library(LIBRASHADER_LIBRARY waylandshader-rashader REQUIRED)

add_library(shader_runtime STATIC src/shader_runtime.cpp)
set_target_properties(shader_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(shader_runtime PUBLIC src PRIVATE ${LIBRASHADER_INCLUDE_DIR})
target_link_libraries(shader_runtime PUBLIC Qt6::Core Qt6::Gui epoxy::epoxy PRIVATE ${LIBRASHADER_LIBRARY})

kcoreaddons_add_plugin(waylandshader
    SOURCES src/effect.cpp src/plugin.cpp
    INSTALL_NAMESPACE "kwin/effects/plugins"
)
set_property(TARGET waylandshader APPEND PROPERTY AUTOMOC_MACRO_NAMES KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED)
target_link_libraries(waylandshader PRIVATE
    shader_runtime KWin::kwin Qt6::CorePrivate Qt6::Concurrent Qt6::DBus
    KF6::CoreAddons KF6::ConfigCore
)

add_library(controller_client STATIC src/controller_client.cpp)
target_include_directories(controller_client PUBLIC src)
target_link_libraries(controller_client PUBLIC Qt6::Core Qt6::DBus)

add_executable(waylandshader_controller src/controller_main.cpp)
set_target_properties(waylandshader_controller PROPERTIES OUTPUT_NAME waylandshader-controller)
target_link_libraries(waylandshader_controller PRIVATE controller_client Qt6::Widgets)
add_executable(waylandshaderctl src/ctl_main.cpp)
target_link_libraries(waylandshaderctl PRIVATE controller_client)

option(WAYLANDSHADER_BUILD_TESTS "Build real OpenGL shader regressions (requires Mesa EGL)" OFF)
if(WAYLANDSHADER_BUILD_TESTS)
    enable_testing()
    add_executable(shader_runtime_test tests/shader_runtime_test.cpp)
    target_link_libraries(shader_runtime_test PRIVATE shader_runtime)
    add_test(NAME shader_runtime COMMAND shader_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME shader_runtime_gl33 COMMAND shader_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set_tests_properties(shader_runtime shader_runtime_gl33 PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(shader_runtime_gl33 PROPERTIES ENVIRONMENT "MESA_GL_VERSION_OVERRIDE=3.3")
endif()

install(TARGETS waylandshader_controller waylandshaderctl RUNTIME DESTINATION ${KDE_INSTALL_BINDIR})
install(FILES data/org.waylandshader.Controller.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES "WaylandShader-Guide.txt" DESTINATION ${KDE_INSTALL_DOCBUNDLEDIR}/waylandshader)
