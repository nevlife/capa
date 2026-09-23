cmake_minimum_required(VERSION 3.16)
project(cyra_py VERSION 0.0.0 LANGUAGES CXX)

# cyra_py is the nanobind Python binding over cyra_core, plus cyra.flux, the pure-Python adapter
# for flux.ros.PartitionedExecutor. nanobind is vendored as a submodule at the repo root.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_python REQUIRED)
find_package(cyra_core REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)  # ament's PYTHON_INSTALL_DIR helper
find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)  # nanobind

# PYTHON_INSTALL_DIR is only populated during ament_package(); we install before that, so
# compute it now (same helper ament uses, so the install path and PYTHONPATH hook agree).
_ament_cmake_python_get_python_install_dir()
# Humble's ament resolves the dir to local/lib/.../dist-packages while colcon looks for
# lib/.../site-packages, so colcon writes no PYTHONPATH hook there. Register ament's own.
_ament_cmake_python_register_environment_hook()

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/nanobind/CMakeLists.txt)
  message(FATAL_ERROR
    "thirdparty/nanobind is empty. Run `git submodule update --init` in the cyra checkout.")
endif()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/nanobind
                 ${CMAKE_CURRENT_BINARY_DIR}/nanobind EXCLUDE_FROM_ALL)

nanobind_add_module(_cyra NB_STATIC src/cyra_py.cpp)
target_link_libraries(_cyra PRIVATE cyra_core::cyra_core)

install(TARGETS _cyra LIBRARY DESTINATION "${PYTHON_INSTALL_DIR}/cyra")
install(FILES cyra/__init__.py cyra/flux.py DESTINATION "${PYTHON_INSTALL_DIR}/cyra")

if(BUILD_TESTING)
  find_package(ament_cmake_pytest REQUIRED)
  ament_add_pytest_test(test_rt test/test_rt.py)
  ament_add_pytest_test(test_flux test/test_flux.py)
endif()

ament_package()
