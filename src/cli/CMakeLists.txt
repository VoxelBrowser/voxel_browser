# vb — developer CLI (architecture_spec/dev-cli.md). `vb_cli_lib` holds all the
# logic so vb_tests can exercise it without spawning the binary.

include(${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake)
set(_vb_embedded_cpp "${CMAKE_BINARY_DIR}/generated/vb_embedded_files.cpp")
vb_embed_files("${_vb_embedded_cpp}"
  ROOT "${CMAKE_SOURCE_DIR}"
  GLOBS "templates/pack/*" "sdk/lua/library/*.lua" "sdk/lua/luarc.template.json" "sdk/lua/luarc.ui.template.json")

add_library(vb_cli_lib STATIC
  ${_vb_embedded_cpp}
  embedded.cpp
  help.cpp
  pack.cpp
  commands.cpp
  compat.cpp
  completions.cpp
  installer.cpp
  instance.cpp
  layout.cpp
  lock.cpp
  portcheck.cpp
  process.cpp
  release_manifest.cpp
  self_update.cpp
  server_config.cpp
  service.cpp
  shim.cpp
  signature.cpp
  source.cpp
  store.cpp
  version.cpp
  watch.cpp
  zip.cpp
)

# GCC 13 reports -Wmaybe-uninitialized from inside libstdc++'s <regex> (std::function in
# _State) under ASan at -O2; with -Werror that fails the asan CI build. A false positive in a
# system header, so silence just that warning for just this file (same as src/automation).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(pack.cpp PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()

target_include_directories(vb_cli_lib PUBLIC "${CMAKE_SOURCE_DIR}/inc")

target_link_libraries(vb_cli_lib
  PUBLIC
    vb_core
  PRIVATE
    vb_editor_model
    tomlplusplus::tomlplusplus
    CURL::libcurl
    nlohmann_json::nlohmann_json
    vb_ed25519
    miniz
    vb_warnings
    vb_sanitizers
)

# Release signing keys (release_keys.txt) are compiled into vb.
file(STRINGS "${CMAKE_SOURCE_DIR}/release_keys.txt" _vb_release_keys REGEX "^[0-9a-fA-F]+$")
list(JOIN _vb_release_keys "," _vb_release_keys_joined)
target_compile_definitions(vb_cli_lib PRIVATE VB_RELEASE_PUBLIC_KEYS="${_vb_release_keys_joined}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/release_keys.txt")

add_executable(vb main.cpp)
target_link_libraries(vb PRIVATE vb_cli_lib vb_warnings vb_sanitizers)
set_target_properties(vb PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(WIN32)
  target_link_libraries(vb_cli_lib PRIVATE ws2_32) # portcheck.cpp
endif()

# miniz's own include dir is not SYSTEM; keep -Werror away from third-party code.
get_target_property(_vb_miniz_inc miniz INTERFACE_INCLUDE_DIRECTORIES)
if(_vb_miniz_inc)
  target_include_directories(vb_cli_lib SYSTEM PRIVATE ${_vb_miniz_inc})
endif()
