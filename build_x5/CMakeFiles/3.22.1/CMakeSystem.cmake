set(CMAKE_HOST_SYSTEM "Linux-5.15.167.4-microsoft-standard-WSL2")
set(CMAKE_HOST_SYSTEM_NAME "Linux")
set(CMAKE_HOST_SYSTEM_VERSION "5.15.167.4-microsoft-standard-WSL2")
set(CMAKE_HOST_SYSTEM_PROCESSOR "x86_64")

include("/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake")

set(CMAKE_SYSTEM "Linux-1")
set(CMAKE_SYSTEM_NAME "Linux")
set(CMAKE_SYSTEM_VERSION "1")
set(CMAKE_SYSTEM_PROCESSOR "aarch64")

set(CMAKE_CROSSCOMPILING "TRUE")

set(CMAKE_SYSTEM_LOADED 1)
