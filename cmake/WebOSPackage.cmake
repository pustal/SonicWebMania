# WebOSPackage.cmake
#
# Adds the "webos-package" target: stages the IPK layout under
#   <build>/ipk/  appinfo.json, icons, bin/SonicWebMania, lib/libSDL2-2.0.so.0
# strips the binary and builds dist/<id>_<version>_arm.ipk with ares-package
# (from @webos-tools/cli), plus dist/<id>.manifest.json for the webOS Homebrew
# Channel. Also adds "webos-install" (ares-install to SWM_ARES_DEVICE, or the
# default device).

find_program(SWM_ARES_PACKAGE ares-package)
find_program(SWM_ARES_INSTALL ares-install)

set(SWM_IPK_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ipk")
set(SWM_DIST_DIR "${SWM_ROOT}/dist")
set(SWM_IPK_NAME "${SWM_APP_ID}_${PROJECT_VERSION}_arm.ipk")
set(SWM_MANIFEST_NAME "${SWM_APP_ID}.manifest.json")
set(SWM_PROJECT_URL "https://github.com/pustal/SonicWebMania")
set(SWM_ICON_URI "https://raw.githubusercontent.com/pustal/SonicWebMania/main/webos/largeIcon.png")

configure_file("${SWM_ROOT}/webos/appinfo.json.in" "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" @ONLY)

if(NOT CMAKE_STRIP)
    set(CMAKE_STRIP strip)
endif()

add_custom_target(webos-stage
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SWM_IPK_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SWM_IPK_STAGE}/bin" "${SWM_IPK_STAGE}/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" "${SWM_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SWM_ROOT}/webos/icon.png" "${SWM_ROOT}/webos/largeIcon.png" "${SWM_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:RetroEngine>" "${SWM_IPK_STAGE}/bin/SonicWebMania"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${SWM_IPK_STAGE}/bin/SonicWebMania"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SWM_SDL2_RUNTIME_LIBRARY}" "${SWM_IPK_STAGE}/lib/"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${SWM_IPK_STAGE}/lib/libSDL2-2.0.so.0"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SWM_ROOT}/LICENSE.md" "${SWM_ROOT}/THIRD_PARTY_NOTICES.md" "${SWM_IPK_STAGE}/"
    DEPENDS RetroEngine
    COMMENT "Staging the IPK contents in ${SWM_IPK_STAGE}"
    VERBATIM
)

if(SWM_ARES_PACKAGE)
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SWM_DIST_DIR}"
        COMMAND "${SWM_ARES_PACKAGE}" --no-minify "${SWM_IPK_STAGE}" -o "${SWM_DIST_DIR}"
        COMMAND "${CMAKE_COMMAND}" -DSTAGE=${SWM_IPK_STAGE} -DIPK=${SWM_DIST_DIR}/${SWM_IPK_NAME}
                -P "${SWM_ROOT}/cmake/ReportFootprint.cmake"
        COMMAND "${CMAKE_COMMAND}"
                -DIPK=${SWM_DIST_DIR}/${SWM_IPK_NAME}
                -DOUTPUT=${SWM_DIST_DIR}/${SWM_MANIFEST_NAME}
                -DAPP_ID=${SWM_APP_ID}
                -DVERSION=${PROJECT_VERSION}
                -DTITLE=SonicWebMania
                "-DDESCRIPTION=Sonic Mania decompilation for webOS. Bring your own game data."
                -DICON_URI=${SWM_ICON_URI}
                -DSOURCE_URL=${SWM_PROJECT_URL}
                -P "${SWM_ROOT}/cmake/WriteManifest.cmake"
        DEPENDS webos-stage
        COMMENT "Building ${SWM_IPK_NAME}"
        VERBATIM
    )
else()
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E echo "ares-package not found: npm install -g @webos-tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E false
        DEPENDS webos-stage
    )
endif()

set(SWM_ARES_DEVICE "" CACHE STRING "ares device name for webos-install (empty = ares default device)")
if(SWM_ARES_INSTALL)
    set(_swm_device_args)
    if(SWM_ARES_DEVICE)
        set(_swm_device_args -d "${SWM_ARES_DEVICE}")
    endif()
    add_custom_target(webos-install
        COMMAND "${SWM_ARES_INSTALL}" ${_swm_device_args} "${SWM_DIST_DIR}/${SWM_IPK_NAME}"
        DEPENDS webos-package
        COMMENT "Installing ${SWM_IPK_NAME} on the TV"
        VERBATIM
    )
endif()
