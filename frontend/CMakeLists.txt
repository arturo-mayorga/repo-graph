cmake_minimum_required(VERSION 3.20)
project(rgv LANGUAGES C CXX)

# --- options -----------------------------------------------------------------
option(RGV_BUILD_APP   "Build the OpenGL application (needs GLFW + a GL 3.3 driver)" ON)
option(RGV_BUILD_TESTS "Build the contract test suite" ON)
option(RGV_OFFLINE     "Never fetch dependencies; require them to be installed" OFF)
option(RGV_WAYLAND     "Build GLFW with the Wayland backend as well as X11" OFF)
option(RGV_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# The app is the whole point of the project, so it lands somewhere predictable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_library(rgv_warnings INTERFACE)
if(MSVC)
  target_compile_options(rgv_warnings INTERFACE /W4 $<$<BOOL:${RGV_WARNINGS_AS_ERRORS}>:/WX>)
else()
  target_compile_options(rgv_warnings INTERFACE
    -Wall -Wextra -Wno-unused-parameter
    $<$<BOOL:${RGV_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# Fixtures are located at runtime. Baking the source path in makes `./bin/rgv` work
# straight out of the build tree with no arguments and no install step.
set(RGV_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures" CACHE PATH
    "Directory scanned for fixture sets")
cmake_path(NORMAL_PATH RGV_FIXTURE_DIR)

# --- vendored header-only dependencies ---------------------------------------
add_library(rgv_json INTERFACE)
target_include_directories(rgv_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)

add_library(rgv_entt INTERFACE)
target_include_directories(rgv_entt INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/entt)

# --- rgv_core ----------------------------------------------------------------
# Contract, store, fixtures, and the impact simulation. No GL, no ImGui, no ECS:
# this is the layer that has to stay buildable and testable on a headless machine.
# Scene, layout, and the colour rules live here too. They depend only on EnTT, and
# keeping them out of the app target is what lets the tests assert things like "a
# stale impact conclusion must not be styled as current" without a GPU.
add_library(rgv_core STATIC
  # contract, store, fixtures
  src/contract/Types.cpp
  src/model/GraphStore.cpp
  src/fixture/Json.cpp
  src/fixture/FixtureSource.cpp
  src/live/LiveSource.cpp
  src/sim/ImpactSim.cpp
  src/analysis/Reach.cpp
  src/analysis/Specificity.cpp
  src/config/Settings.cpp

  # the ECS itself
  src/ecs/System.cpp
  src/ecs/Resources.cpp

  # view policy: pure functions shared by rendering, picking, and layout
  src/view/SemanticZoom.cpp
  src/view/CameraFit.cpp
  src/view/HoverLinks.cpp
  src/view/LabelLayout.cpp
  src/platform/OpenPath.cpp
  src/ui/Theme.cpp

  # systems that need neither a GPU nor a window, and are therefore testable headless
  src/systems/SourceSystem.cpp
  src/systems/CommandSystem.cpp
  src/systems/SpecificitySystem.cpp
  src/systems/SceneSyncSystem.cpp
  src/systems/ImpactStateSystem.cpp
  src/systems/SelectionSystem.cpp
  src/systems/LayoutSystem.cpp
  src/systems/StyleSystem.cpp
  src/systems/LabelSystem.cpp
  src/systems/PickingSystem.cpp
  src/systems/NavigationSystem.cpp
  src/systems/TransportSystem.cpp
)
target_include_directories(rgv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rgv_core PUBLIC rgv_warnings rgv_entt PRIVATE rgv_json)

# ImGui and GLFW are only needed for the app, but the UI layout tests need ImGui too,
# so the dependencies are resolved before either target is declared.
if(RGV_BUILD_APP)
  include(cmake/Dependencies.cmake)
endif()

# --- rgv-replay --------------------------------------------------------------
add_executable(rgv-replay tools/rgv_replay.cpp)
target_link_libraries(rgv-replay PRIVATE rgv_core)

# --- tests -------------------------------------------------------------------
if(RGV_BUILD_TESTS)
  enable_testing()
  add_executable(rgv-tests
    tests/test_main.cpp
    tests/test_store.cpp
    tests/test_timeline.cpp
    tests/test_scene.cpp
    tests/test_live.cpp
    tests/test_settings.cpp
    tests/test_specificity.cpp
    tests/test_schedule.cpp
    tests/test_packages.cpp
    tests/test_open_path.cpp
    tests/test_ts_imports.cpp
    tests/test_label_layout.cpp
    tests/test_python_imports.cpp
    tests/test_provider_impact.cpp
    # The manifest readers are the provider's, not the frontend's. Compiled into the
    # tests directly rather than linked, because a provider shares no library with the
    # frontend -- only the wire format.
    ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/Packages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/PythonImports.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/TsImports.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/Impact.cpp
  )
  target_include_directories(rgv-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json
                                               ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch)
  target_compile_definitions(rgv-tests PRIVATE RGV_FIXTURE_DIR="${RGV_FIXTURE_DIR}")
  target_link_libraries(rgv-tests PRIVATE rgv_core)

  add_test(NAME contract COMMAND rgv-tests)

  # Layout invariants for the panels. Needs ImGui, which is only present when the app
  # is being built -- but needs no window and no GPU, so it still runs in CI.
  if(RGV_BUILD_APP)
    add_executable(rgv-ui-tests
      tests/test_main.cpp
      tests/test_ui_layout.cpp
      src/ui/Panels.cpp
      src/ui/ImGuiStyle.cpp
    )
    target_include_directories(rgv-ui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(rgv-ui-tests PRIVATE RGV_FIXTURE_DIR="${RGV_FIXTURE_DIR}")
    target_link_libraries(rgv-ui-tests PRIVATE rgv_core rgv_imgui_core)
    add_test(NAME ui_layout COMMAND rgv-ui-tests)
  endif()
  # Fixture validation is a test too: a scenario whose authored impact disagrees with
  # traversal is a broken fixture, and it should fail CI rather than mislead a reviewer.
  file(GLOB RGV_FIXTURE_SETS LIST_DIRECTORIES true ${RGV_FIXTURE_DIR}/*)
  foreach(dir ${RGV_FIXTURE_SETS})
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/snapshot.json)
      get_filename_component(name ${dir} NAME)
      add_test(NAME fixture.${name} COMMAND rgv-replay ${dir} --check)
    endif()
  endforeach()
endif()

# --- application -------------------------------------------------------------
if(RGV_BUILD_APP)
  # Only the systems that genuinely need a window, a GPU, or ImGui live here.
  add_executable(rgv
    app/main.cpp
    app/Options.cpp
    app/FixtureBootstrap.cpp
    app/LiveBootstrap.cpp
    src/render/GL.cpp
    src/render/GraphRenderer.cpp
    src/systems/WindowSystem.cpp
    src/systems/GraphRenderSystem.cpp
    src/systems/UiSystem.cpp
    src/ui/Panels.cpp
    src/ui/ImGuiStyle.cpp
  )
  target_compile_definitions(rgv PRIVATE RGV_FIXTURE_DIR="${RGV_FIXTURE_DIR}")
  target_link_libraries(rgv PRIVATE rgv_core rgv_imgui glfw)
  if(UNIX AND NOT APPLE)
    target_link_libraries(rgv PRIVATE ${CMAKE_DL_LIBS})
  endif()

  message(STATUS "rgv: app enabled, fixtures at ${RGV_FIXTURE_DIR}")
else()
  message(STATUS "rgv: app disabled (headless build: rgv_core, rgv-replay, tests)")
endif()

# -- providers ----------------------------------------------------------------
#
# A provider is a separate process that writes the contract to stdout (§6). It links
# nothing from the frontend: the wire format is the only thing they share, which is what
# lets the next one be written in another language entirely.
# The tests run the real provider, so a provider that emits a token the parser rejects
# fails the build rather than producing a silently empty graph at runtime.
add_compile_definitions(RGV_WATCH_BIN="${CMAKE_BINARY_DIR}/bin/rgv-watch"
                        RGV_TEST_TMP="${CMAKE_BINARY_DIR}/test-tmp")

add_executable(rgv-watch
  ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/Packages.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/PythonImports.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/TsImports.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/Impact.cpp)
target_include_directories(rgv-watch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
target_compile_features(rgv-watch PRIVATE cxx_std_20)
target_link_libraries(rgv-watch PRIVATE rgv_warnings)
