# Lockstep netplay for the KH Days co-op mod (see src/netplay/Lockstep.h, Session.h).

include(FixInterfaceIncludes)

add_library(netplay STATIC
    Consoles.cpp
    Fiber.cpp
    Lockstep.cpp
    Session.cpp
)
target_include_directories(netplay PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(netplay PUBLIC core)

if (USE_VCPKG)
    find_package(unofficial-enet CONFIG REQUIRED)
    target_link_libraries(netplay PUBLIC unofficial::enet::enet)
    find_package(zstd CONFIG REQUIRED)
    target_link_libraries(netplay PUBLIC $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NetplayENet REQUIRED IMPORTED_TARGET libenet)
    fix_interface_includes(PkgConfig::NetplayENet)
    target_link_libraries(netplay PUBLIC PkgConfig::NetplayENet)
    # snapshots (rejoining, resync) are compressed
    pkg_check_modules(NetplayZstd REQUIRED IMPORTED_TARGET libzstd)
    fix_interface_includes(PkgConfig::NetplayZstd)
    target_link_libraries(netplay PUBLIC PkgConfig::NetplayZstd)
endif()
if (WIN32)
    target_link_libraries(netplay PUBLIC ws2_32 winmm)
endif()

# The determinism probe: headless, no Qt.
add_executable(melonDS-netprobe
    probe/main.cpp
    probe/ProbePlatform.cpp
)
target_link_libraries(melonDS-netprobe PRIVATE netplay core)
