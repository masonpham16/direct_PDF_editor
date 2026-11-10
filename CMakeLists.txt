cmake_minimum_required(VERSION 3.21)
project(DirectPDF VERSION 0.1.0 LANGUAGES CXX)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Homebrew installs Qt (and keg-only poppler-qt6) under /opt/homebrew on Apple
# Silicon or /usr/local on Intel Macs. Make sure both are searchable.
if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /opt/homebrew/opt/qt /opt/homebrew/opt/qtbase /opt/homebrew/opt/poppler-qt6 /opt/homebrew/opt/qpdf /usr/local /usr/local/opt/qt /usr/local/opt/qtbase /usr/local/opt/poppler-qt6 /usr/local/opt/qpdf)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Core PrintSupport)

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/opt/poppler-qt6/lib/pkgconfig:/usr/local/opt/poppler-qt6/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(POPPLER_QT6 REQUIRED poppler-qt6)

# poppler-qt6 bundles its own copy of the core (non-Qt) poppler library and
# headers; PdfDocument::imageBoxes() calls directly into that core API (the
# Qt bindings don't expose per-image geometry), so find that library too.
find_library(POPPLER_CORE_LIBRARY NAMES poppler HINTS ${POPPLER_QT6_LIBRARY_DIRS})
if(NOT POPPLER_CORE_LIBRARY)
  message(FATAL_ERROR "Could not find core poppler library alongside poppler-qt6")
endif()

# qpdf provides the PDF-writing capability (metadata edits, page reordering,
# bookmark/outline writing) — Poppler here is read-only.
find_package(qpdf CONFIG REQUIRED)

if(APPLE)
  set(APP_ICON assets/AppIcon.icns)
  set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
else()
  set(APP_ICON)
endif()

add_executable(directpdf
  MACOSX_BUNDLE
  src/main.cpp
  src/FormatPanel.cpp
  src/FormatPanel.h
  src/MainWindow.cpp
  src/MainWindow.h
  src/PdfDocument.cpp
  src/PdfDocument.h
  src/PdfView.cpp
  src/PdfView.h
  src/PdfWriter.cpp
  src/PdfWriter.h
  src/ThumbnailPanel.cpp
  src/ThumbnailPanel.h
  resources.qrc
  ${APP_ICON}
)

if(APPLE)
  set_target_properties(directpdf PROPERTIES
    OUTPUT_NAME "DirectPDF"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/assets/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_ICON_FILE AppIcon.icns
  )
endif()

target_include_directories(directpdf PRIVATE ${POPPLER_QT6_INCLUDE_DIRS})
target_link_directories(directpdf PRIVATE ${POPPLER_QT6_LIBRARY_DIRS})
target_link_libraries(directpdf PRIVATE
  Qt6::Widgets
  Qt6::Gui
  Qt6::Core
  Qt6::PrintSupport
  ${POPPLER_QT6_LIBRARIES}
  ${POPPLER_CORE_LIBRARY}
  qpdf::libqpdf
)
target_compile_options(directpdf PRIVATE ${POPPLER_QT6_CFLAGS_OTHER})
target_compile_definitions(directpdf PRIVATE APP_VERSION="${PROJECT_VERSION}")
