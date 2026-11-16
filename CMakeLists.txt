cmake_minimum_required(VERSION 3.21)

# MacDroid targets macOS 26 (Tahoe) and later only, so the native Liquid Glass
# appearance is always available.
set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version")

project(MacDroid VERSION 1.0.0
    DESCRIPTION "Android file browser for macOS over MTP"
    HOMEPAGE_URL "https://tuanquynh.com"
    LANGUAGES C CXX OBJCXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)

find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMTP REQUIRED IMPORTED_TARGET libmtp)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

# Interposes libusb_reset_device() to stop a libmtp/libusb crash when a phone
# cannot be opened (see src/platform/UsbResetGuard.c). Must be a dylib: dyld
# ignores interposing in the main executable.
add_library(MacDroidUsbGuard SHARED src/platform/UsbResetGuard.c)
target_link_libraries(MacDroidUsbGuard PRIVATE PkgConfig::LIBUSB)
set_target_properties(MacDroidUsbGuard PROPERTIES
    INSTALL_NAME_DIR "@rpath"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/MacDroid.app/Contents/Frameworks")

qt_standard_project_setup(I18N_SOURCE_LANGUAGE en I18N_TRANSLATED_LANGUAGES vi)

set(SOURCES
    src/main.cpp
    src/app/AppController.h        src/app/AppController.cpp
    src/app/Settings.h             src/app/Settings.cpp
    src/app/SelfTest.cpp
    src/app/Theme.h                src/app/Theme.cpp
    src/core/MtpTypes.h            src/core/MtpTypes.cpp
    src/core/MtpWorker.h           src/core/MtpWorker.cpp
    src/core/UsbMonitor.h          src/core/UsbMonitor.cpp
    src/core/TransferManager.h     src/core/TransferManager.cpp
    src/ui/BrowserWindow.h         src/ui/BrowserWindow.cpp
    src/ui/FileModel.h             src/ui/FileModel.cpp
    src/ui/FileViews.h             src/ui/FileViews.cpp
    src/ui/Sidebar.h               src/ui/Sidebar.cpp
    src/ui/GlassWidgets.h          src/ui/GlassWidgets.cpp
    src/ui/TransfersPopup.h        src/ui/TransfersPopup.cpp
    src/ui/TrayController.h        src/ui/TrayController.cpp
    src/ui/Icons.h                 src/ui/Icons.cpp
    src/platform/MacPlatform.h     src/platform/MacPlatform.mm
)

# App icon: generated from docs/app-icon.png at build time.
set(APP_ICON_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/docs/app-icon.png)
set(APP_ICON_ICNS ${CMAKE_CURRENT_BINARY_DIR}/MacDroid.icns)
add_custom_command(
    OUTPUT ${APP_ICON_ICNS}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cmake/make-icns.sh ${APP_ICON_SOURCE} ${APP_ICON_ICNS}
    DEPENDS ${APP_ICON_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/make-icns.sh ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compose-icon.swift
    COMMENT "Generating MacDroid.icns"
    VERBATIM)
set_source_files_properties(${APP_ICON_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

qt_add_executable(MacDroid MACOSX_BUNDLE ${SOURCES} ${APP_ICON_ICNS})

qt_add_translations(MacDroid
    TS_FILES i18n/macdroid_en.ts i18n/macdroid_vi.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none
)

target_include_directories(MacDroid PRIVATE src)
target_compile_definitions(MacDroid PRIVATE PROJECT_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(MacDroid PRIVATE
    Qt6::Widgets
    PkgConfig::LIBMTP
    MacDroidUsbGuard
    "-framework AppKit"
    "-framework IOKit"
    "-framework CoreFoundation"
    "-framework UniformTypeIdentifiers"
    "-framework QuickLookThumbnailing"
    "-framework UserNotifications"
    "-framework ServiceManagement"
)

set_source_files_properties(src/platform/MacPlatform.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

set_target_properties(MacDroid PROPERTIES
    BUILD_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH "@executable_path/../Frameworks"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER com.bliksund.macdroid
    MACOSX_BUNDLE_ICON_FILE MacDroid.icns
    MACOSX_BUNDLE_BUNDLE_NAME MacDroid
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_COPYRIGHT "© 2026 Jason Tuan. https://tuanquynh.com"
)

install(TARGETS MacDroid BUNDLE DESTINATION .)
