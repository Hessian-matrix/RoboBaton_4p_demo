cmake_minimum_required(VERSION 3.16)

project(x5_imu_uart_open_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ICM42688_X5_SO "${CMAKE_CURRENT_SOURCE_DIR}/lib/libicm42688_x5.so")
if(NOT EXISTS "${ICM42688_X5_SO}")
  message(FATAL_ERROR "Missing ${ICM42688_X5_SO}. Copy libicm42688_x5.so into open_source_demo/lib first.")
endif()

add_library(icm42688_x5 SHARED IMPORTED GLOBAL)
set_target_properties(icm42688_x5 PROPERTIES
  IMPORTED_LOCATION "${ICM42688_X5_SO}"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_executable(imu_reader_demo
  src/imu_reader_demo.cpp
)

add_executable(serial_port_demo
  src/serial_port_demo.cpp
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

target_link_libraries(imu_reader_demo PRIVATE icm42688_x5)

set_target_properties(imu_reader_demo PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/lib"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/lib"
)

install(TARGETS imu_reader_demo serial_port_demo
  RUNTIME DESTINATION .
)

install(FILES "${ICM42688_X5_SO}"
  DESTINATION lib
)
