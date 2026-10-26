cmake_minimum_required(VERSION 3.14)
project(capa_cli NONE)

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_python REQUIRED)

ament_python_install_package(${PROJECT_NAME})

# Installed to bin: a command a person types, which needs no ROS and so is not behind `ros2 run`.
install(PROGRAMS scripts/capa DESTINATION bin)

if(BUILD_TESTING)
  find_package(ament_cmake_pytest REQUIRED)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run)
  ament_add_pytest_test(test_capa_cli test ENV CAPA_RUN_DIR=${CMAKE_CURRENT_BINARY_DIR}/run)
endif()

ament_package()
