# vb_render — client-only rendering. Links raylib + raygui.
# Never linked by the server. Builds in headless mode too (window.cpp no-ops).

# raygui implementation isolated so -Werror / /WX never touches third-party code.
add_library(vb_raygui_impl STATIC raygui_impl.c)
target_link_libraries(vb_raygui_impl PUBLIC vb_raygui)
set_target_properties(vb_raygui_impl PROPERTIES C_STANDARD 99)

# Menu backdrop texture + fonts (src/render/resources), baked in so the menu works with no pack
# loaded and can't fail on a missing file. Licence texts ship alongside (release bundle).
include(${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake)
set(_vb_render_resources_cpp "${CMAKE_BINARY_DIR}/generated/vb_render_resources.cpp")
vb_embed_files("${_vb_render_resources_cpp}"
  ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources"
  GLOBS "dirt.png" "*.otf" "*.ttf"
  NAMESPACE "vb::render::resources"
  HEADER "vb/render/resources.hpp")

add_library(vb_render STATIC
  ${_vb_render_resources_cpp}
  resources.cpp
  chunk_renderer.cpp
  crack_atlas.cpp
  crack_overlay.cpp
  entity_renderer.cpp
  input.cpp
  main_menu.cpp
  texture_atlas.cpp
  ui_renderer.cpp
  window.cpp
)
add_library(vb::render ALIAS vb_render)

target_include_directories(vb_render PUBLIC "${CMAKE_SOURCE_DIR}/inc")
target_compile_features(vb_render PUBLIC cxx_std_20)

target_link_libraries(vb_render
  PUBLIC
    vb_core
    raylib
    vb_raygui
    vb_raygui_impl
  PRIVATE
    vb_warnings
    vb_sanitizers
)
