cmake_minimum_required(VERSION 3.21)
project(folio LANGUAGES CXX)

# Folio is packaged into /usr, and a build that names no prefix (bin/build,
# the Arch package) has always looked for its plugins there.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX /usr CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The tests are built unless -DBUILD_TESTING=OFF, which a package can pass
# to build Folio alone, without Qt Test.
option(BUILD_TESTING "Build the tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Concurrent)
if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
find_package(ZLIB REQUIRED)

# Everything but main.cpp, shared by the app and the tests, so a new source
# file in src/ needs no line here.
file(GLOB folio_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM folio_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(folio_core STATIC ${folio_sources})
target_include_directories(folio_core PUBLIC src)
target_link_libraries(folio_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus Qt6::Concurrent
  PkgConfig::GIO PkgConfig::FONTCONFIG ZLIB::ZLIB)
target_compile_options(folio_core PUBLIC -Wall -Wextra)

# Precompile Folio's QML and JS with qmlcachegen so cold start loads compiled
# bytecode/C++ from the binary instead of parsing QML on launch. Optional
# preview viewers (QtQuick.Pdf, QtMultimedia) stay uncompiled resources so
# builds without optional Qt modules still succeed.
file(GLOB folio_qml_files CONFIGURE_DEPENDS src/*.qml src/*.js)
set(folio_optional_qml
  ${CMAKE_SOURCE_DIR}/src/PdfPreview.qml
  ${CMAKE_SOURCE_DIR}/src/MediaPreview.qml
)
list(REMOVE_ITEM folio_qml_files ${folio_optional_qml})
foreach(f IN LISTS folio_qml_files folio_optional_qml)
  get_filename_component(name "${f}" NAME)
  set_source_files_properties("${f}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()
qt_add_qml_module(folio_core
  URI FolioUI
  VERSION 1.0
  RESOURCE_PREFIX /
  OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/FolioUI"
  NO_PLUGIN
  QML_FILES ${folio_qml_files}
  RESOURCES ${folio_optional_qml}
)

add_executable(folio src/main.cpp src/resources.qrc)
target_link_libraries(folio PRIVATE folio_core)

# Where an installed Folio finds its bundled plugins: /usr/share/folio/plugins
# for the usual -DCMAKE_INSTALL_PREFIX=/usr.
target_compile_definitions(folio_core PRIVATE
  FOLIO_PACKAGED_PLUGINS="${CMAKE_INSTALL_FULL_DATADIR}/folio/plugins")

if(BUILD_TESTING)
  # The tests compile the same sources, run offscreen, and read the QML they
  # check straight from src/, or whole windows from the resources.
  enable_testing()
  add_executable(folio_tests tests/folio_tests.cpp src/resources.qrc)
  target_link_libraries(folio_tests PRIVATE folio_core Qt6::Test)
  add_test(NAME folio_tests COMMAND folio_tests)
  set_tests_properties(folio_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
  # The bundled plugins, which the tests run against stubs.
  target_compile_definitions(folio_tests PRIVATE FOLIO_PLUGINS="${CMAKE_SOURCE_DIR}/plugins")
endif()

# `cmake --install build` (with DESTDIR for a package) puts Folio where a
# package does, so a packager needs no list of files of their own. Tests stay
# out of it.
install(TARGETS folio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/folio.desktop share/folio-chooser.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# Other apps' Open and Save dialogs: `folio chooser` is an xdg-desktop-portal
# FileChooser backend, which D-Bus starts on demand.
install(FILES share/folio.portal DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
# D-Bus starts the chooser by its full path, so it's the one this install puts
# folio at, not the /usr/bin the source names.
file(READ share/org.freedesktop.impl.portal.desktop.folio.service portal_service)
string(REPLACE "/usr/bin/folio" "${CMAKE_INSTALL_FULL_BINDIR}/folio" portal_service "${portal_service}")
file(WRITE ${CMAKE_BINARY_DIR}/org.freedesktop.impl.portal.desktop.folio.service "${portal_service}")
install(FILES ${CMAKE_BINARY_DIR}/org.freedesktop.impl.portal.desktop.folio.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES share/folio.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/folio)
install(FILES README.md docs/plugins.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
# The bundled plugins keep their modes, so their commands stay executable.
install(DIRECTORY plugins DESTINATION ${CMAKE_INSTALL_DATADIR}/folio USE_SOURCE_PERMISSIONS)
