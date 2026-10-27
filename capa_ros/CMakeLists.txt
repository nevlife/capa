cmake_minimum_required(VERSION 3.16)
project(capa_ros VERSION 0.0.0 LANGUAGES CXX)

# capa_ros applies capa declarations to ROS 2 callback group threads: those of a flux
# PartitionedExecutor, through its on_thread_start() hook, and those of capa_component_container.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ament_cmake REQUIRED)
find_package(capa_core REQUIRED)
find_package(flux_components REQUIRED)
find_package(flux_cpp REQUIRED)
find_package(rclcpp REQUIRED)

add_library(capa_ros src/flux.cpp)
target_include_directories(capa_ros PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(capa_ros PUBLIC capa_core::capa_core flux_cpp::flux_cpp)
ament_target_dependencies(capa_ros PUBLIC rclcpp)
target_compile_options(capa_ros PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(capa_ros PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The container's manager, shared by the executable and its test. Not installed.
add_library(capa_component_manager STATIC src/component_manager.cpp)
target_include_directories(capa_component_manager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(capa_component_manager PUBLIC capa_ros flux_components::flux_components)
target_compile_options(capa_component_manager PRIVATE -Wall -Wextra -Wpedantic)

add_executable(capa_component_container src/component_container.cpp)
target_link_libraries(capa_component_container capa_component_manager)
target_compile_options(capa_component_container PRIVATE -Wall -Wextra -Wpedantic)

install(DIRECTORY include/ DESTINATION include)
install(TARGETS capa_ros
  EXPORT export_capa_ros
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include)
install(TARGETS capa_component_container DESTINATION lib/${PROJECT_NAME})
ament_export_targets(export_capa_ros HAS_LIBRARY_TARGET)
ament_export_include_directories(include)
ament_export_dependencies(capa_core flux_cpp rclcpp)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(composition_interfaces REQUIRED)
  find_package(rclcpp_components REQUIRED)
  find_package(std_msgs REQUIRED)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run)
  ament_add_gtest(test_capa_ros test/test_schedule.cpp test/test_chain.cpp
    ENV CAPA_RUN_DIR=${CMAKE_CURRENT_BINARY_DIR}/run)
  if(TARGET test_capa_ros)
    target_link_libraries(test_capa_ros capa_ros)
    ament_target_dependencies(test_capa_ros rclcpp std_msgs)
  endif()

  add_library(capa_ros_test_components SHARED test/components.cpp)
  target_link_libraries(capa_ros_test_components
    flux_cpp::flux_cpp rclcpp::rclcpp rclcpp_components::component ${std_msgs_TARGETS})
  ament_add_gtest(test_component_container test/test_component_container.cpp TIMEOUT 120
    ENV CAPA_RUN_DIR=${CMAKE_CURRENT_BINARY_DIR}/run)
  if(TARGET test_component_container)
    target_link_libraries(test_component_container capa_component_manager
      ${composition_interfaces_TARGETS} ${std_msgs_TARGETS})
    target_compile_definitions(test_component_container PRIVATE
      CAPA_TEST_COMPONENTS="$<TARGET_FILE:capa_ros_test_components>")
    add_dependencies(test_component_container capa_ros_test_components)
  endif()
endif()

ament_package()
