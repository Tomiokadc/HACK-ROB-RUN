cmake_minimum_required(VERSION 3.20)
project(RobRunProbe LANGUAGES CXX)
add_executable(robrun_probe main.cpp driver_client/driver_client.cpp)
target_compile_features(robrun_probe PRIVATE cxx_std_20)
target_compile_options(robrun_probe PRIVATE /utf-8 /W4)

add_executable(robrun_esp_preview WIN32 overlay/preview.cpp driver_client/driver_client.cpp)
target_compile_features(robrun_esp_preview PRIVATE cxx_std_20)
target_compile_options(robrun_esp_preview PRIVATE /utf-8 /W4)
target_link_libraries(robrun_esp_preview PRIVATE user32 gdi32 d3d11 dxgi dcomp d2d1 dwrite)

set_property(TARGET robrun_esp_preview PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")


