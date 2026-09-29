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

# thread / address / undefined. TSan is the one that matters here: the stage claims are a mutex
# and a thread_local that every applying thread touches.
set(CYRA_SANITIZER "" CACHE STRING "Build with -fsanitize=<value> (thread, address, undefined)")
if(CYRA_SANITIZER)
  add_compile_options(-fsanitize=${CYRA_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${CYRA_SANITIZER})
endif()

# CYRA_FUZZER=ON instruments the library too, so the fuzzer is guided by the parser's edges and
# not only the harness's. Clang only: cmake -DCYRA_FUZZER=ON -DCMAKE_CXX_COMPILER=clang++
set(CYRA_FUZZER OFF CACHE BOOL "Build the fuzz harness with libFuzzer (clang only)")
if(CYRA_FUZZER)
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(cyra_core
  src/record.cpp
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
  test/test_record.cpp
  test/test_rt.cpp
  test/test_spec.cpp)
# Tests write their records here rather than into the host's /dev/shm.
set(CYRA_TEST_RUN_DIR ${CMAKE_CURRENT_BINARY_DIR}/run)
file(MAKE_DIRECTORY ${CYRA_TEST_RUN_DIR})

# Without libFuzzer the harness links a replay main() that runs a corpus bent from valid files,
# so ctest keeps it compiled and exercised between fuzzing sessions.
if(CYRA_FUZZER)
  add_executable(fuzz_spec test/fuzz/fuzz_spec.cpp)
  target_link_options(fuzz_spec PRIVATE -fsanitize=fuzzer)
else()
  add_executable(fuzz_spec test/fuzz/fuzz_spec.cpp test/fuzz/fuzz_spec_replay_main.cpp)
endif()
target_link_libraries(fuzz_spec PRIVATE cyra_core)

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
    ament_add_gtest(test_cyra_core ${cyra_core_test_srcs} ENV CYRA_RUN_DIR=${CYRA_TEST_RUN_DIR})
    if(TARGET test_cyra_core)
      target_link_libraries(test_cyra_core cyra_core Threads::Threads)
      target_compile_definitions(test_cyra_core PRIVATE CYRA_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs")
    endif()
    if(NOT CYRA_FUZZER)
      add_test(NAME fuzz_spec_corpus COMMAND fuzz_spec)
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
    target_compile_definitions(test_cyra_core PRIVATE CYRA_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs")
    add_test(NAME test_cyra_core COMMAND test_cyra_core)
    set_tests_properties(test_cyra_core PROPERTIES ENVIRONMENT CYRA_RUN_DIR=${CYRA_TEST_RUN_DIR})
    if(NOT CYRA_FUZZER)
      add_test(NAME fuzz_spec_corpus COMMAND fuzz_spec)
    endif()
  endif()
endif()
