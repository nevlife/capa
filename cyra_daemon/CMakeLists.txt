cmake_minimum_required(VERSION 3.16)
project(cyra_daemon VERSION 0.0.0 LANGUAGES CXX)

# cyrad is a Rust crate built by cargo. cyra_core stays the one parser and judge; the crate reaches
# it through a cxx bridge (src/ffi.rs, src/bridge.cpp). CMake resolves cyra_core and what its static
# archive needs, hands the list to cargo, and installs what cargo built.

find_package(ament_cmake REQUIRED)
find_package(cyra_core REQUIRED)

# rustup's cargo, which reads rust-toolchain.toml. A distribution cargo ignores that file and is
# usually too old for the edition.
set(cargo_home $ENV{CARGO_HOME})
if(NOT cargo_home)
  set(cargo_home $ENV{HOME}/.cargo)
endif()
find_program(CARGO cargo PATHS ${cargo_home}/bin NO_DEFAULT_PATH)
find_program(CARGO cargo)
if(NOT CARGO)
  message(FATAL_ERROR "cargo not found: install rustup (docs/en/daemon.en.md, Building)")
endif()

if(TARGET yaml-cpp::yaml-cpp)
  set(yaml_cpp yaml-cpp::yaml-cpp)
else()
  set(yaml_cpp yaml-cpp)
endif()
set(CARGO_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(CARGO_ENV
  CARGO_TARGET_DIR=${CARGO_DIR}
  CYRA_CORE_LINK=$<TARGET_LINKER_FILE:cyra_core::cyra_core>:$<TARGET_LINKER_FILE:${yaml_cpp}>)

# Always runs; cargo decides what is out of date. The toolchain file is found from the working
# directory, not from --manifest-path. MAKEFLAGS and MFLAGS name make's jobserver pipes, which a custom
# target's command does not inherit, so cargo is left to size its own jobs.
add_custom_target(cyrad ALL
  COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS --unset=MFLAGS ${CARGO_ENV} ${CARGO} build --release --locked
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  USES_TERMINAL
  COMMENT "cargo build cyrad")
install(PROGRAMS ${CARGO_DIR}/release/cyrad DESTINATION bin)
install(FILES systemd/cyrad.service DESTINATION share/${PROJECT_NAME}/systemd)

# Never built: it gives clang-tidy a compile command for the C++ half of the bridge. The headers
# cxx generates are under ${CARGO_DIR}/cxxbridge once cargo has run.
add_library(cyrad_bridge_tidy OBJECT EXCLUDE_FROM_ALL src/bridge.cpp)
target_include_directories(cyrad_bridge_tidy PRIVATE src ${CARGO_DIR}/cxxbridge)
target_link_libraries(cyrad_bridge_tidy PRIVATE cyra_core::cyra_core)

if(BUILD_TESTING)
  find_package(ament_cmake_test REQUIRED)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run)
  ament_add_test(cyrad_cargo_test
    COMMAND ${CARGO} test --release --locked
    GENERATE_RESULT_FOR_RETURN_CODE_ZERO
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TIMEOUT 600
    ENV ${CARGO_ENV} CYRA_RUN_DIR=${CMAKE_CURRENT_BINARY_DIR}/run)
endif()

ament_package()
