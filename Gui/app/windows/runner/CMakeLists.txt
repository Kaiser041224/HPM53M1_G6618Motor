cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# === Rust bridge (gui-frb) ===
# The generated Dart loader (frb_generated.dart) looks up a dynamic library with
# stem "gui_frb", so build the crate's cdylib via cargo and stage it next to the
# runner. This mirrors the flutter_rust_bridge Cargokit CMake helper on Windows.
find_program(CARGO_EXECUTABLE NAMES cargo
  HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR "cargo not found; it is required to build the gui-frb Rust bridge")
endif()

set(GUI_FRB_WORKSPACE_DIR "${CMAKE_SOURCE_DIR}/../..")
set(GUI_FRB_MANIFEST "${GUI_FRB_WORKSPACE_DIR}/frb/Cargo.toml")
set(GUI_FRB_CARGO_TARGET_DIR "${GUI_FRB_WORKSPACE_DIR}/target")
set(GUI_FRB_CARGO_PROFILE "$<IF:$<CONFIG:Debug>,dev,release>")
set(GUI_FRB_OUTPUT_DIR "$<IF:$<CONFIG:Debug>,debug,release>")
set(GUI_FRB_DLL "${GUI_FRB_CARGO_TARGET_DIR}/${GUI_FRB_OUTPUT_DIR}/gui_frb.dll")

add_custom_target(gui_frb_cargo ALL
  COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${GUI_FRB_CARGO_TARGET_DIR}"
          "${CARGO_EXECUTABLE}" build --manifest-path "${GUI_FRB_MANIFEST}"
          --package gui-frb "--profile=${GUI_FRB_CARGO_PROFILE}"
  WORKING_DIRECTORY "${GUI_FRB_WORKSPACE_DIR}"
  COMMENT "Building gui-frb Rust bridge (cargo, $<CONFIG>)"
  VERBATIM)

add_dependencies(${BINARY_NAME} gui_frb_cargo)

# Stage the freshly built cdylib next to the runner through the same install
# mechanism Flutter uses for flutter_windows.dll. The install step runs as part
# of the default VS build, so the deployed DLL is refreshed even when only Rust
# sources changed and the runner itself is not relinked (a POST_BUILD copy on
# the runner would be skipped in that case, and referencing the runner target
# from gui_frb_cargo would create a target dependency cycle).
install(FILES "${GUI_FRB_DLL}"
  DESTINATION "$<TARGET_FILE_DIR:${BINARY_NAME}>"
  COMPONENT Runtime)
