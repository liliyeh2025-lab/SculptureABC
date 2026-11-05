# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#
# Tablet shell: the web build (Emscripten -> WebAssembly + WebGL2) is the product; the Windows build is for
# day-to-day development and debugging. Separate from the upstream SculptForge CMakeLists.txt so upstream
# merges never touch it.
cmake_minimum_required(VERSION 3.24)
project(SculptureABC VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(SCULPTURE_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

option(SCULPTURE_BUILD_TESTS "Also build upstream's unit tests (sculptforge_tests; the web build runs them under Node.js)" OFF)
# Multithreaded web build (-pthread): needs a cross-origin isolated page (HTTPS + COOP/COEP, mobile/web/_headers).
option(SCULPTURE_WEB_THREADS "Web build with real threads (SharedArrayBuffer) instead of inline ones" OFF)

if(EMSCRIPTEN)
  # Upstream catches exceptions (file readers, JSON, malformed input): WebAssembly's own exception handling
  # (Safari 15.2+, Chrome 95+, Node 17+), on everything so the whole program agrees.
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions)
  if(SCULPTURE_WEB_THREADS)
    # Every object, SDL and ImGui included, needs atomics for the shared memory.
    add_compile_options(-pthread)
    add_link_options(-pthread)
  endif()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Same versions as upstream (cmake/Dependencies.cmake) so code moves over unchanged.
FetchContent_Declare(glm
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.30/SDL3-3.2.30.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(sculpture_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(sculpture_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(sculpture_imgui PUBLIC SDL3::SDL3-static)
if(EMSCRIPTEN)
  # WebGL2 = OpenGL ES 3.0: the ImGui backend uses the GLES 3 headers Emscripten provides.
  target_compile_definitions(sculpture_imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
endif()

# --- Upstream's sculpting core (sculptforge_core): its definition is read from the root CMakeLists.txt ---
set(SCULPTFORGE_BUILD_APP OFF) # the upstream sections below: no desktop-app pieces
include(cmake/UpstreamSections.cmake)
sculpture_upstream_section("# --- Core library" "# --- Bring-your-own-AI")
target_compile_definitions(sculptforge_core PUBLIC SCULPTURE_MOBILE=1)
if(EMSCRIPTEN AND SCULPTURE_WEB_THREADS)
  # Upstream's loops use up to 16 threads; a tablet has at most 8 cores (src/platform/web_cores.js).
  target_link_options(sculptforge_core INTERFACE --js-library ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web_cores.js)
  target_compile_options(sculptforge_core PRIVATE -msimd128)
elseif(EMSCRIPTEN)
  # Single-threaded web build: "threads" run inline (src/platform/InlineThreads.c). An INTERFACE source, so it is
  # compiled into every program that links the core (a library member could lose to Emscripten's weak stand-ins).
  target_sources(sculptforge_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/InlineThreads.c)
  target_link_options(sculptforge_core INTERFACE --js-library ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/single_core.js)
  # xatlas (Auto UV) keeps worker threads waiting for tasks, which inline "threads" can't do: its own switch
  # turns them off.
  set_property(SOURCE ${SCULPTURE_REPO_ROOT}/third_party/xatlas/xatlas.cpp APPEND PROPERTY COMPILE_DEFINITIONS XA_MULTITHREADED=0)
  target_compile_options(sculptforge_core PRIVATE -msimd128)
endif()

if(SCULPTURE_BUILD_TESTS)
  # The brush assistant / HTTP and phone-link libraries the test program links (no WebRTC download).
  set(SCULPTFORGE_WEBRTC OFF)
  sculpture_upstream_section("# --- Bring-your-own-AI" "# --- Headless command-line tool")
  set(SCULPTFORGE_BUILD_TESTS ON)
  if(EMSCRIPTEN)
    set(SCULPTURE_TEST_DATA_DIR /repo/tests/data)
  endif()
  enable_testing()
  # The section's own `if(SCULPTFORGE_BUILD_TESTS)` is closed here: the Python- and app-driven tests after the
  # cut are not ours to run.
  sculpture_upstream_section("# --- Tests ---" "  # Tests that launch our programs from Python" "endif()")
  if(EMSCRIPTEN)
    # Node.js: the repository is mounted at /repo (tools/node-mount.js) so the tests find tests/data.
    target_link_options(sculptforge_tests PRIVATE
      -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sEXIT_RUNTIME=1
      -lnodefs.js --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/tools/node-mount.js)
    set_property(TARGET sculptforge_tests APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/node-mount.js)
  endif()
endif()

# --- Upstream's renderer (src/render): every source file there, as it is (no list copied: a file upstream adds is
# picked up at the next configure). It reaches GL only through render/GL.h's function table, which
# src/render/GLCompat.cpp fills with GLES 3.0 / WebGL2 stand-ins where desktop GL 3.3 has more.
file(GLOB SCULPTURE_UPSTREAM_RENDER CONFIGURE_DEPENDS ${SCULPTURE_REPO_ROOT}/src/render/*.cpp)
add_library(sculpture_render STATIC ${SCULPTURE_UPSTREAM_RENDER} src/render/GLCompat.cpp)
target_include_directories(sculpture_render PUBLIC ${SCULPTURE_REPO_ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sculpture_render PUBLIC sculptforge_core SDL3::SDL3-static glm::glm)
if(EMSCRIPTEN)
  target_compile_options(sculpture_render PRIVATE -msimd128)
endif()

add_executable(SculptureABC WIN32
  src/main.cpp
  src/MobileGL.cpp
  src/PointerInput.cpp
  src/Brushes.cpp
  src/SculptView.cpp
  src/render/GroupOverlay.cpp
  src/Shell.cpp
  src/ViewportInput.cpp
  src/FileStore.cpp
  src/I18n.cpp
  src/InputProbe.cpp)

# Upstream headers (render/GL.h for GL types and constants; later the whole core).
target_include_directories(SculptureABC PRIVATE src ${SCULPTURE_REPO_ROOT}/src ${SCULPTURE_REPO_ROOT}/third_party/lucide)
target_link_libraries(SculptureABC PRIVATE sculpture_render sculptforge_core sculpture_imgui SDL3::SDL3-static glm::glm)
target_compile_definitions(SculptureABC PRIVATE SCULPTURE_MOBILE=1 SCULPTURE_VERSION="${PROJECT_VERSION}")

if(EMSCRIPTEN)
  set_target_properties(SculptureABC PROPERTIES SUFFIX ".html")
  target_compile_options(SculptureABC PRIVATE -msimd128)
  target_compile_options(sculpture_imgui PRIVATE -msimd128)
  target_link_options(SculptureABC PRIVATE
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sGL_ENABLE_GET_PROC_ADDRESS=1
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=4GB
    -sENVIRONMENT=$<IF:$<BOOL:${SCULPTURE_WEB_THREADS}>,web$<COMMA>worker,web>
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
    # Autosave (OPFS), downloads, the file picker and drops (src/FileStore.cpp calls into it).
    --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/web/files.js
    # (SHELL: keeps CMake from merging the repeated --embed-file flags into one)
    "SHELL:--embed-file ${SCULPTURE_REPO_ROOT}/assets/fonts/Inter-Regular.ttf@/assets/fonts/Inter-Regular.ttf"
    "SHELL:--embed-file ${SCULPTURE_REPO_ROOT}/assets/fonts/lucide.ttf@/assets/fonts/lucide.ttf"
    "SHELL:--embed-file ${SCULPTURE_REPO_ROOT}/assets/demo/lion-head.glb@/assets/demo/lion-head.glb"
    # Ours: the Chinese font subset and the interface text (mobile/tools/make-font-subset.py, mobile/i18n).
    "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansTC-Subset.ttf@/mobile/assets/fonts/NotoSansTC-Subset.ttf"
    "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/i18n/zh-TW.json@/mobile/i18n/zh-TW.json")
  set_property(TARGET SculptureABC APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n/zh-TW.json ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansTC-Subset.ttf)
  if(SCULPTURE_WEB_THREADS)
    # Workers started at load: the main thread joins threads at once, and a worker made on demand would only start
    # after the main thread yields (a deadlock). The ThreadPool keeps cores - 1 of them; parallelFor uses up to
    # cores - 1 more at a time.
    target_link_options(SculptureABC PRIVATE -sPTHREAD_POOL_SIZE=16 -sMAXIMUM_MEMORY=2GB)
  endif()
  set_property(TARGET SculptureABC APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
                                                               ${CMAKE_CURRENT_SOURCE_DIR}/web/files.js)
else()
  # Development build reads the fonts straight from the repository.
  target_compile_definitions(SculptureABC PRIVATE SCULPTURE_ASSET_DIR="${SCULPTURE_REPO_ROOT}/assets"
                                                 SCULPTURE_MOBILE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(MSVC)
  target_compile_options(SculptureABC PRIVATE /utf-8 /W3)
  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SculptureABC)
endif()
