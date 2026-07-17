cmake_minimum_required(VERSION 3.16)

project(x5_imu_uart_open_demo LANGUAGES C CXX)

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

add_executable(imu_reader_demo
  src/imu_reader_demo.cpp
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
)

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
if(X5_HOBOT_LIB_DIR)
  # rpath-link 仅供链接器解析 producer 的 vendor 依赖，不增加 consumer 的直接依赖或 RUNPATH。
  target_link_options(cam_demo PRIVATE "LINKER:-rpath-link,${X5_HOBOT_LIB_DIR}")
endif()
target_link_libraries(cam_demo PRIVATE sc132_v2 prrtsp_v2 Threads::Threads)
target_link_options(cam_demo PRIVATE
  -static-libstdc++
  -static-libgcc
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

install(TARGETS imu_reader_demo serial_port_demo cam_demo
  RUNTIME DESTINATION .
  LIBRARY DESTINATION lib
)

install(FILES "${ICM42688_SO}" "${SC132_SO}" "${PRRTSP_SO}"
  DESTINATION lib
)

install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/include/icm42688_driver.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/sc132camera.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/pr_venc.h"
  DESTINATION include
)
