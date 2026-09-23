cmake_minimum_required(VERSION 3.16)
project(cyra_core VERSION 0.0.0 LANGUAGES CXX)

# cyra_core is the ROS-free C++17 core: thread scheduling and the chain declaration file.
#   - standalone:  cmake -S . -B build   (needs libyaml-cpp-dev; exports find_package(cyra_core))
#   - colcon:      built as an ament_cmake package inside a ROS 2 workspace

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ament_cmake QUIET)
find_package(Threads REQUIRED)
# Under ament the vendor package points yaml-cpp_DIR at the copy ROS ships; standalone it is the
# system libyaml-cpp-dev.
if(ament_cmake_FOUND)
  find_package(yaml_cpp_vendor REQUIRED)
endif()
find_package(yaml-cpp REQUIRED)
# Jazzy's yaml-cpp config exports the namespaced target; Humble's (0.7) exports the bare one.
if(TARGET yaml-cpp::yaml-cpp)
  set(CYRA_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
  set(CYRA_YAML_CPP_TARGET yaml-cpp)
endif()
include(GNUInstallDirs)

add_library(cyra_core
  src/rt.cpp
  src/spec.cpp)
target_link_libraries(cyra_core PRIVATE ${CYRA_YAML_CPP_TARGET})
add_library(cyra::core ALIAS cyra_core)
# PIC so this static lib can be linked into shared objects (cyra_py's nanobind module).
set_target_properties(cyra_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(cyra_core PUBLIC cxx_std_17)
target_include_directories(cyra_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(cyra_core PRIVATE -Wall -Wextra -Wpedantic)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(cyra_core_test_srcs
  test/test_rt.cpp
  test/test_spec.cpp)

if(ament_cmake_FOUND)
  install(TARGETS cyra_core
    EXPORT export_cyra_core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include)
  ament_export_targets(export_cyra_core HAS_LIBRARY_TARGET)
  ament_export_include_directories(include)
  # A static library's PRIVATE link still lands in the exported interface, so a consumer needs
  # the yaml-cpp target findable at its own configure.
  ament_export_dependencies(yaml_cpp_vendor yaml-cpp)

  if(BUILD_TESTING)
    find_package(ament_cmake_gtest REQUIRED)
    ament_add_gtest(test_cyra_core ${cyra_core_test_srcs})
    if(TARGET test_cyra_core)
      target_link_libraries(test_cyra_core cyra_core Threads::Threads)
    endif()
  endif()

  ament_package()
else()
  include(CMakePackageConfigHelpers)
  install(TARGETS cyra_core
    EXPORT cyra_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT cyra_coreTargets
    FILE cyra_coreTargets.cmake
    NAMESPACE cyra::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cyra_core)
  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cyra_coreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cyra_coreConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cyra_core)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cyra_coreConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cyra_coreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cyra_coreConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cyra_core)

  include(CTest)
  if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(test_cyra_core ${cyra_core_test_srcs})
    target_link_libraries(test_cyra_core PRIVATE cyra_core GTest::gtest GTest::gtest_main Threads::Threads)
    add_test(NAME test_cyra_core COMMAND test_cyra_core)
  endif()
endif()
