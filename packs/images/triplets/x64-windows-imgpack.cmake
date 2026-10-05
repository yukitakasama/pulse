# Pulse image preview pack: one self-contained pulse-imgpack.exe (static CRT,
# no VC++ runtime needed). The LGPL libraries stay separate, replaceable DLLs.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
if(PORT MATCHES "^(libheif|libde265)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
set(VCPKG_BUILD_TYPE release)
