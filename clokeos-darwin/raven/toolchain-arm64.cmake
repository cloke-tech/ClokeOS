set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
set(CMAKE_C_COMPILER_TARGET arm64-apple-macos11.0)
set(CMAKE_CXX_COMPILER_TARGET arm64-apple-macos11.0)
# arm64 runs on the Mac itself: no emulator, and the build tools of the custom commands start directly
set(CMAKE_CROSSCOMPILING FALSE)
