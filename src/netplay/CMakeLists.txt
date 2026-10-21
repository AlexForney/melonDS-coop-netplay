# Lockstep netplay for the KH Days co-op mod (see src/netplay/Lockstep.h).

add_library(netplay STATIC
    Fiber.cpp
    Lockstep.cpp
)
target_include_directories(netplay PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(netplay PUBLIC core)

# The determinism probe: headless, no Qt.
add_executable(melonDS-netprobe
    probe/main.cpp
    probe/ProbePlatform.cpp
)
target_link_libraries(melonDS-netprobe PRIVATE netplay core)
