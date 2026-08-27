# GLFW and Dear ImGui.
#
# Both are fetched at configure time and pinned to a tag, so a fresh clone builds the
# same thing months from now. RGV_OFFLINE switches to find_package/system lookup for
# machines with no network or a vendoring policy.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- GLFW --------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
  set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
  # Wayland needs wayland-scanner and wayland-protocols at build time; it is opt-in so
  # a missing protocol package cannot break the default build. XWayland covers the
  # default case.
  set(GLFW_BUILD_WAYLAND ${RGV_WAYLAND} CACHE BOOL "" FORCE)
endif()

if(RGV_OFFLINE)
  find_package(glfw3 3.3 REQUIRED)
else()
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(glfw)
endif()

# --- Dear ImGui --------------------------------------------------------------
# ImGui ships no CMakeLists, so the target is declared here over its sources. Only the
# GLFW + OpenGL3 backends are compiled; the GL3 backend brings its own loader, which is
# why rgv's loader and ImGui's never collide.
if(RGV_OFFLINE)
  set(RGV_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH
      "Path to a checkout of Dear ImGui")
  if(NOT EXISTS ${RGV_IMGUI_DIR}/imgui.cpp)
    message(FATAL_ERROR
      "RGV_OFFLINE is set but no ImGui checkout was found at ${RGV_IMGUI_DIR}. "
      "Clone https://github.com/ocornut/imgui (v1.90.9) there, or set RGV_IMGUI_DIR.")
  endif()
else()
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(imgui)
  set(RGV_IMGUI_DIR ${imgui_SOURCE_DIR})
endif()

# Split in two. The core is platform-free and runs headless, which is what lets the
# UI layout tests drive real ImGui frames with no window and no GPU.
add_library(rgv_imgui_core STATIC
  ${RGV_IMGUI_DIR}/imgui.cpp
  ${RGV_IMGUI_DIR}/imgui_draw.cpp
  ${RGV_IMGUI_DIR}/imgui_tables.cpp
  ${RGV_IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(rgv_imgui_core PUBLIC ${RGV_IMGUI_DIR})

add_library(rgv_imgui STATIC
  ${RGV_IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${RGV_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(rgv_imgui PUBLIC ${RGV_IMGUI_DIR}/backends)
target_link_libraries(rgv_imgui PUBLIC rgv_imgui_core glfw)
if(UNIX AND NOT APPLE)
  target_link_libraries(rgv_imgui PUBLIC ${CMAKE_DL_LIBS})
endif()
