# Overlay of vcpkg's libheif port (vcpkg d46283cf) for Pulse's image preview pack.
# Changes: a "dav1d" feature, every encoder off, no plugin loading.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO  strukturag/libheif
    REF "v${VERSION}"
    SHA512 c953cb9c3a4c0f1052f4d1e0475143cfea6c0a1ab787acb33a2fd173e0460b665717a56a99b864226dfe62bd139c92a66fe5dec75b91e2a333b77a0571e3c5a8
    HEAD_REF master
    PATCHES
        cxx-linkage-pkgconfig.diff
        find-modules.diff
        gdk-pixbuf.patch
        symbol-exports.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        dav1d       WITH_DAV1D
        iso23001-17 WITH_UNCOMPRESSED_CODEC
        iso23001-17 VCPKG_LOCK_FIND_PACKAGE_ZLIB
        jpeg        WITH_JPEG_DECODER
        jpeg        VCPKG_LOCK_FIND_PACKAGE_JPEG
        openjpeg    WITH_OpenJPEG_DECODER
        openjpeg    VCPKG_LOCK_FIND_PACKAGE_OpenJPEG
)

vcpkg_find_acquire_program(PKGCONFIG)
set(ENV{PKG_CONFIG} "${PKGCONFIG}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTING=OFF
        -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
        "-DCMAKE_PROJECT_INCLUDE=${CURRENT_PORT_DIR}/cmake-project-include.cmake"
        -DPLUGIN_DIRECTORY=  # empty
        # Pulse image pack: decoders only, built into heif.dll. No encoder
        # (x265 / x264 are GPL), no plugin loading from disk.
        -DENABLE_PLUGIN_LOADING=OFF
        -DWITH_DAV1D_PLUGIN=OFF
        -DWITH_LIBDE265_PLUGIN=OFF
        -DWITH_X265=OFF
        -DWITH_X264=OFF
        -DWITH_AOM_DECODER=OFF
        -DWITH_AOM_ENCODER=OFF
        -DWITH_JPEG_ENCODER=OFF
        -DWITH_OpenJPEG_ENCODER=OFF
        -DWITH_OPENJPH_ENCODER=OFF
        -DWITH_SvtEnc=OFF
        -DWITH_RAV1E=OFF
        -DWITH_KVAZAAR=OFF
        -DWITH_GDK_PIXBUF=OFF
        -DWITH_HEADER_COMPRESSION=OFF
        -DBUILD_DOCUMENTATION=OFF
        -DWITH_EXAMPLES=OFF
        -DWITH_LIBSHARPYUV=OFF
        -DWITH_OpenH264_DECODER=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_Brotli=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_Doxygen=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_LIBDE265=ON   # feature candidate
        -DVCPKG_LOCK_FIND_PACKAGE_PNG=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_TIFF=OFF
        ${FEATURE_OPTIONS}
    OPTIONS_RELEASE
        "-DPLUGIN_INSTALL_DIRECTORY=${CURRENT_PACKAGES_DIR}/plugins/libheif"
    OPTIONS_DEBUG
        "-DPLUGIN_INSTALL_DIRECTORY=${CURRENT_PACKAGES_DIR}/debug/plugins/libheif"
    MAYBE_UNUSED_VARIABLES
        VCPKG_LOCK_FIND_PACKAGE_Brotli
        VCPKG_LOCK_FIND_PACKAGE_OpenJPEG
        VCPKG_LOCK_FIND_PACKAGE_ZLIB
        WITH_DAV1D_PLUGIN
        WITH_LIBDE265_PLUGIN
        WITH_SvtEnc
        WITH_RAV1E
        WITH_KVAZAAR
        WITH_OPENJPH_ENCODER
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/libheif")
vcpkg_fixup_pkgconfig()

if (VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libheif/heif_library.h" "!defined(LIBHEIF_STATIC_BUILD)" "1")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libheif/heif_library.h" "!defined(LIBHEIF_STATIC_BUILD)" "0")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib/libheif" "${CURRENT_PACKAGES_DIR}/debug/lib/libheif")

file(GLOB maybe_plugins "${CURRENT_PACKAGES_DIR}/plugins/libheif/*")
if(maybe_plugins STREQUAL "")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/plugins" "${CURRENT_PACKAGES_DIR}/debug/plugins")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
