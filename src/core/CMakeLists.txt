# vb_core — engine core. No rendering, no window; must build and run headless.
# Later phases add sources here (net/, world/, worldgen/, ecs/, replication/,
# assetsync/, script/, protocol/) under this same target.

add_library(vb_core STATIC
  build_info.cpp
  cli.cpp
  config.cpp
  log.cpp
  paths.cpp
  server_address.cpp
  sha256.cpp
  version_req.cpp
)
add_library(vb::core ALIAS vb_core)

# Sibling modules attach their sources to vb_core via target_sources()
# (see src/CMakeLists.txt add_subdirectory order).

target_include_directories(vb_core PUBLIC
  "${CMAKE_SOURCE_DIR}/inc"
  "${VB_GENERATED_INCLUDE_DIR}"
)

target_compile_features(vb_core PUBLIC cxx_std_20)

target_link_libraries(vb_core
  PUBLIC
    EnTT::EnTT
  PRIVATE
    tomlplusplus::tomlplusplus
    vb_warnings
    vb_sanitizers
)

if(VB_WITH_NET)
  # Static: avoids shipping/locating a GNS shared lib next to the exe (every
  # other dependency here is static or header-only; keep it that way).
  target_link_libraries(vb_core PUBLIC GameNetworkingSockets::static)
  target_compile_definitions(vb_core PUBLIC VB_WITH_NET=1)

  # GNS's public headers trip several of our -Werror flags (old-style casts,
  # sign conversion, __int64 as a language extension) — same fix as
  # doctest/raygui/toml++: re-add its include dir as SYSTEM so the warning
  # flags don't apply to third-party code.
  get_target_property(_vb_gns_inc GameNetworkingSockets::static
    INTERFACE_INCLUDE_DIRECTORIES)
  if(_vb_gns_inc)
    target_include_directories(vb_core SYSTEM PRIVATE ${_vb_gns_inc})
  endif()
endif()

if(VB_WITH_REPLICATION)
  target_link_libraries(vb_core PUBLIC vb_librg)
  target_compile_definitions(vb_core PUBLIC VB_WITH_REPLICATION=1)
endif()

if(VB_WITH_WORLDGEN)
  target_link_libraries(vb_core PUBLIC FastNoise2)
  target_compile_definitions(vb_core PUBLIC VB_WITH_WORLDGEN=1)
endif()

if(VB_WITH_COMPRESSION)
  # xxHash linked BEFORE lz4, deliberately: lz4's own lib/ directory (which
  # its CMake target publicly exposes for lz4.h) also vendors a private,
  # older xxhash.h/.c (used internally for lz4 frame checksums) with no XXH3
  # API at all. `#include <xxhash.h>` (Phase 4.4's manifest hashing) resolves
  # against whichever -I entry appears first, so xxHash's real header must be
  # added to the include path ahead of lz4's.
  if(TARGET xxHash::xxhash)
    target_link_libraries(vb_core PUBLIC xxHash::xxhash)
  elseif(TARGET xxhash)
    target_link_libraries(vb_core PUBLIC xxhash)
  endif()
  if(TARGET LZ4::lz4)
    target_link_libraries(vb_core PUBLIC LZ4::lz4)
  elseif(TARGET lz4_static)
    target_link_libraries(vb_core PUBLIC lz4_static)
  endif()
  target_compile_definitions(vb_core PUBLIC VB_WITH_COMPRESSION=1)
endif()

if(VB_WITH_LUA)
  target_link_libraries(vb_core PUBLIC lua::lua sol2::sol2 nlohmann_json::nlohmann_json)
  target_compile_definitions(vb_core PUBLIC VB_WITH_LUA=1)
endif()

# Visible to build_info.cpp so --version reports "+automation" (§7.3). Defined
# publicly so every consumer agrees; the automation module itself is separate.
if(VB_WITH_AUTOMATION)
  target_compile_definitions(vb_core PUBLIC VB_WITH_AUTOMATION=1)
endif()
