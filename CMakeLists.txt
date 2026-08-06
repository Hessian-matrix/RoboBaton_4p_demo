cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ROBOBATON_RELEASE_VERSION)
string(STRIP "${ROBOBATON_RELEASE_VERSION}" ROBOBATON_RELEASE_VERSION)
if(NOT ROBOBATON_RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a SemVer core version")
endif()
project(x5_imu_uart_open_demo VERSION ${ROBOBATON_RELEASE_VERSION} LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
add_compile_options(
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-DNDEBUG>
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(ICM42688_SO "${CMAKE_CURRENT_SOURCE_DIR}/lib/libicm42688.so.2.0.0")
if(NOT EXISTS "${ICM42688_SO}")
  message(FATAL_ERROR "Missing ${ICM42688_SO}. Run the canonical producer build so libicm42688.so.2.0.0 and compatibility copies exist in this demo's lib/ directory.")
endif()

set(SC132_SO "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsc132.so.2.0.0")
if(NOT EXISTS "${SC132_SO}")
  message(FATAL_ERROR "Missing ${SC132_SO}. Run the canonical producer build so libsc132.so.2.0.0 and compatibility copies exist in this demo's lib/ directory.")
endif()

set(PRRTSP_SO "${CMAKE_CURRENT_SOURCE_DIR}/lib/libprrtsp.so.2.0.0")
if(NOT EXISTS "${PRRTSP_SO}")
  message(FATAL_ERROR "Missing ${PRRTSP_SO}. Run the canonical producer build so libprrtsp.so.2.0.0 and compatibility copies exist in this demo's lib/ directory.")
endif()

set(X5_USR_X5_INCLUDE_DIR "" CACHE PATH "X5 usr_x5 include directory that provides hbn_api.h")
if(NOT X5_USR_X5_INCLUDE_DIR AND CMAKE_SYSROOT AND EXISTS "${CMAKE_SYSROOT}/usr_x5/include/hbn_api.h")
  set(X5_USR_X5_INCLUDE_DIR "${CMAKE_SYSROOT}/usr_x5/include")
endif()

set(SC132_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(X5_USR_X5_INCLUDE_DIR)
  list(APPEND SC132_INCLUDE_DIRS "${X5_USR_X5_INCLUDE_DIR}")
endif()

set(X5_HOBOT_LIB_DIR "" CACHE PATH "X5 hobot library directory that provides libhbmem.so")
if(NOT X5_HOBOT_LIB_DIR AND CMAKE_SYSROOT AND EXISTS "${CMAKE_SYSROOT}/usr_x5/hobot/lib/libhbmem.so")
  set(X5_HOBOT_LIB_DIR "${CMAKE_SYSROOT}/usr_x5/hobot/lib")
endif()
if(X5_HOBOT_LIB_DIR AND EXISTS "${X5_HOBOT_LIB_DIR}/libhbmem.so")
  set(HBMEM_SO "${X5_HOBOT_LIB_DIR}/libhbmem.so")
else()
  find_library(HBMEM_SO NAMES hbmem PATHS /usr/hobot/lib /usr/lib /lib)
endif()
if(NOT HBMEM_SO)
  message(FATAL_ERROR "Missing libhbmem.so. mosaic_rtsp_demo uses X5 hbmem graphic buffers for encoder-owned external NV12 input.")
endif()


add_library(icm42688_x5 SHARED IMPORTED GLOBAL)
set_target_properties(icm42688_x5 PROPERTIES
  IMPORTED_LOCATION "${ICM42688_SO}"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# 直接绑定 canonical major-2 ELF，由 SONAME 决定消费者的 DT_NEEDED。
add_library(sc132_v2 SHARED IMPORTED GLOBAL)
set_target_properties(sc132_v2 PROPERTIES
  IMPORTED_LOCATION "${SC132_SO}"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
add_library(prrtsp_v2 SHARED IMPORTED GLOBAL)
set_target_properties(prrtsp_v2 PROPERTIES
  IMPORTED_LOCATION "${PRRTSP_SO}"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
add_library(hbmem SHARED IMPORTED GLOBAL)
set_target_properties(hbmem PROPERTIES
  IMPORTED_LOCATION "${HBMEM_SO}"
)
if(X5_USR_X5_INCLUDE_DIR)
  set_target_properties(hbmem PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${X5_USR_X5_INCLUDE_DIR}"
  )
endif()


add_executable(imu_reader_demo
  src/imu_reader_demo.cpp
  src/frozen_system_clock.cpp
)

add_executable(serial_port_demo
  src/serial_port_demo.cpp
)

add_executable(cam_demo
  src/cam_demo.cpp
  src/cam_demo_common.cpp
  src/cam_demo_config.cpp
  src/cam_demo_pipeline.cpp
  src/cam_demo_rtsp.cpp
  src/frozen_system_clock.cpp
)

add_executable(sensor_demo
  src/sensor_demo.cpp
  src/imu_reader_demo.cpp
  src/cam_demo_common.cpp
  src/cam_demo_config.cpp
  src/sensor_demo_yaml_config.cpp
  src/cam_demo_pipeline.cpp
  src/cam_demo_rtsp.cpp
  src/frozen_system_clock.cpp
)
add_executable(mosaic_rtsp_demo
  src/mosaic_rtsp_demo.cpp
  src/mosaic_nv12.cpp
  src/cam_demo_common.cpp
  src/cam_demo_config.cpp
  src/frozen_system_clock.cpp
)


foreach(versioned_target IN ITEMS imu_reader_demo serial_port_demo cam_demo sensor_demo mosaic_rtsp_demo)
  target_compile_definitions(${versioned_target} PRIVATE
    ROBOBATON_RELEASE_VERSION="${ROBOBATON_RELEASE_VERSION}")
endforeach()

target_compile_options(imu_reader_demo PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

target_compile_options(serial_port_demo PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

target_compile_options(cam_demo PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

target_compile_options(sensor_demo PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)
target_compile_options(mosaic_rtsp_demo PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)
target_compile_definitions(sensor_demo PRIVATE SENSOR_DEMO_NO_MAIN)

target_link_libraries(imu_reader_demo PRIVATE icm42688_x5)
target_link_options(imu_reader_demo PRIVATE
  -static-libstdc++
  -static-libgcc
)

target_link_options(serial_port_demo PRIVATE
  -static-libstdc++
  -static-libgcc
)

target_include_directories(cam_demo PRIVATE
  ${SC132_INCLUDE_DIRS}
)
target_include_directories(sensor_demo PRIVATE
  ${SC132_INCLUDE_DIRS}
)
target_include_directories(mosaic_rtsp_demo PRIVATE
  ${SC132_INCLUDE_DIRS}
)
if(X5_HOBOT_LIB_DIR)
  # rpath-link 仅供链接器解析 producer 的 vendor 依赖，不增加 consumer 的直接依赖或 RUNPATH。
  target_link_options(cam_demo PRIVATE "LINKER:-rpath-link,${X5_HOBOT_LIB_DIR}")
  target_link_options(sensor_demo PRIVATE "LINKER:-rpath-link,${X5_HOBOT_LIB_DIR}")
  target_link_options(mosaic_rtsp_demo PRIVATE "LINKER:-rpath-link,${X5_HOBOT_LIB_DIR}")
endif()
target_link_libraries(cam_demo PRIVATE sc132_v2 prrtsp_v2 Threads::Threads)
target_link_libraries(sensor_demo PRIVATE icm42688_x5 sc132_v2 prrtsp_v2 Threads::Threads)
target_link_libraries(mosaic_rtsp_demo PRIVATE sc132_v2 prrtsp_v2 hbmem Threads::Threads)
target_link_options(cam_demo PRIVATE
  -static-libstdc++
  -static-libgcc
  "LINKER:--export-dynamic"
  "LINKER:--no-allow-shlib-undefined"
)
target_link_options(mosaic_rtsp_demo PRIVATE
  -static-libstdc++
  -static-libgcc
  "LINKER:--export-dynamic"
  "LINKER:--no-allow-shlib-undefined"
)
# libicm42688 已依赖共享 C++ 运行库，sensor_demo 复用同一映射，避免静态和动态运行库同时驻留。
target_link_options(sensor_demo PRIVATE
  "LINKER:--export-dynamic"
  "LINKER:--no-allow-shlib-undefined"
)

set_target_properties(imu_reader_demo PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib"
)

set_target_properties(cam_demo PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib;/usr/hobot/lib;/usr/hobot/lib/sensor;/usr/lib;/lib64:/lib"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib;/usr/hobot/lib;/usr/hobot/lib/sensor;/usr/lib;/lib64:/lib"
)

set_target_properties(sensor_demo PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib;/usr/hobot/lib;/usr/hobot/lib/sensor;/usr/lib;/lib64:/lib"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib;/usr/hobot/lib;/usr/hobot/lib/sensor;/usr/lib;/lib64:/lib"
)
set_target_properties(mosaic_rtsp_demo PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib;/usr/hobot/lib;/usr/hobot/lib/sensor;/usr/lib;/lib64:/lib"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../lib;\$ORIGIN/lib;/usr/hobot/lib;/usr/hobot/lib/sensor;/usr/lib;/lib64:/lib"
)


install(TARGETS imu_reader_demo serial_port_demo cam_demo sensor_demo mosaic_rtsp_demo
  RUNTIME DESTINATION .
  LIBRARY DESTINATION lib
)

install(FILES "${ICM42688_SO}" "${SC132_SO}" "${PRRTSP_SO}"
  DESTINATION lib
)

install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/include/icm42688_driver.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/sc132camera.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/prrtsp_v2.h"
  DESTINATION include
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/sensor_config.yaml"
  DESTINATION config
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DESTINATION .)
