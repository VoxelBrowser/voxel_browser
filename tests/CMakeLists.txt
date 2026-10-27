# vb_tests — unit + integration tests (doctest).
# Integration tests (headless client <-> server) are added from Phase 1.

add_executable(vb_tests
  main.cpp
  unit/auth_config_test.cpp
  unit/auth_keycloak_test.cpp
  unit/auth_keycloak_client_test.cpp
  unit/auth_verifier_test.cpp
  unit/auth_login_test.cpp
  unit/auth_signin_test.cpp
  unit/assetsync_cache_test.cpp
  unit/assetsync_manifest_test.cpp
  unit/biome_selector_test.cpp
  unit/chunk_lifecycle_test.cpp
  unit/block_damage_tracker_test.cpp
  unit/blockedit_test.cpp
  unit/block_damage_test.cpp
  unit/block_registry_test.cpp
  unit/chunk_codec_test.cpp
  unit/cli_test.cpp
  unit/config_test.cpp
  unit/content_base_behaviour_test.cpp
  unit/content_base_data_test.cpp
  unit/content_base_ui_test.cpp
  unit/content_pack_test.cpp
  unit/data_script_test.cpp
  unit/core_test.cpp
  unit/daynight_test.cpp
  unit/editor_model_test.cpp
  unit/entity_visual_layout_test.cpp
  unit/entity_visual_test.cpp
  unit/frustum_test.cpp
  unit/gns_transport_test.cpp
  unit/item_drops_test.cpp
  unit/kitchen_sink_pack_test.cpp
  unit/lua_api_surface_test.cpp
  unit/lighting_test.cpp
  unit/mesher_test.cpp
  unit/netcode_test.cpp
  unit/noise_graph_test.cpp
  unit/pack_runtime_integration_test.cpp
  unit/pack_runtime_test.cpp
  unit/perf_budget_test.cpp
  unit/physics_test.cpp
  unit/protocol_test.cpp
  unit/net_test.cpp
  unit/region_store_test.cpp
  unit/world_registry_test.cpp
  unit/script_test.cpp
  unit/replication_test.cpp
  unit/render_test.cpp
  unit/script_db_test.cpp
  unit/server_time_estimator_test.cpp
  unit/sha256_test.cpp
  unit/structure_placement_test.cpp
  unit/structure_test.cpp
  unit/soak_test.cpp
  unit/ui_runtime_test.cpp
  unit/world_replication_test.cpp
  unit/version_req_test.cpp
  unit/world_test.cpp
  unit/worldgen_test.cpp
)

# Lets content_pack_test.cpp find content/base regardless of the test
# binary's working directory (which ctest sets to the build dir, not the
# source tree).
target_compile_definitions(vb_tests PRIVATE
  VB_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(vb_tests
  PRIVATE
    vb_core
    vb_editor_model
    vb_auth
    doctest::doctest
    vb_warnings
    vb_sanitizers
)

if(VB_WITH_AUTH)
  target_link_libraries(vb_tests PRIVATE nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(vb_tests PRIVATE ws2_32) # auth_signin_test plays the browser over Winsock
  endif()
endif()

if(VB_WITH_LUA AND TARGET vb_pack_check)
  target_sources(vb_tests PRIVATE unit/check_pack_test.cpp)
  target_link_libraries(vb_tests PRIVATE vb_pack_check)
endif()

if(VB_BUILD_CLI)
  target_sources(vb_tests PRIVATE unit/dev_cli_test.cpp)
  target_sources(vb_tests PRIVATE unit/dev_cli_install_test.cpp)
  target_sources(vb_tests PRIVATE unit/dev_cli_host_test.cpp)
  target_sources(vb_tests PRIVATE unit/dev_cli_polish_test.cpp)
  target_sources(vb_tests PRIVATE unit/dev_cli_hardening_test.cpp)
  target_sources(vb_tests PRIVATE unit/dev_cli_structure_test.cpp)
  target_sources(vb_tests PRIVATE unit/dev_cli_pack_test.cpp)
  target_sources(vb_tests PRIVATE unit/dev_cli_help_test.cpp)
  target_compile_definitions(vb_tests PRIVATE VB_TEST_VB_EXE="$<TARGET_FILE:vb>")
  add_dependencies(vb_tests vb)
  if(VB_BUILD_SERVER)
    # Integration tests drive the real server through `vb` (stop file, detach).
    target_compile_definitions(vb_tests PRIVATE
      VB_TEST_SERVER_EXE="$<TARGET_FILE:voxel_browser_server>")
    add_dependencies(vb_tests voxel_browser_server)
  endif()
  target_link_libraries(vb_tests PRIVATE vb_cli_lib miniz nlohmann_json::nlohmann_json vb_ed25519_signing)
endif()

# TextureAtlas (real texture/atlas system) needs raylib's Image functions,
# which -- like vb_render itself -- only exist when VB_BUILD_CLIENT is ON
# (cmake/Dependencies.cmake gates the raylib fetch behind it).
if(VB_BUILD_CLIENT)
  target_sources(vb_tests PRIVATE unit/texture_atlas_test.cpp unit/crack_atlas_test.cpp unit/input_test.cpp)
  target_link_libraries(vb_tests PRIVATE vb_render)
endif()

# Development-only automation module (docs/e2e-automation.md); absent from
# default builds.
if(VB_WITH_AUTOMATION)
  target_sources(vb_tests PRIVATE unit/automation_test.cpp)
  target_link_libraries(vb_tests PRIVATE vb_automation)
endif()

# doctest.h itself trips /W4 /WX on MSVC — treat its headers as SYSTEM so the
# project warning flags don't apply to third-party code.
get_target_property(_vb_doctest_inc doctest::doctest INTERFACE_INCLUDE_DIRECTORIES)
if(_vb_doctest_inc)
  target_include_directories(vb_tests SYSTEM PRIVATE ${_vb_doctest_inc})
endif()

set_target_properties(vb_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_test(NAME vb_tests COMMAND vb_tests --no-intro --force-colors=off)

# Fuzz targets (tests/fuzz/): libFuzzer executables with VB_BUILD_FUZZERS, corpus-replay tests otherwise.
add_subdirectory(fuzz)

# Smoke tests: the binaries start, print, and exit cleanly headless.
if(VB_BUILD_SERVER)
  add_test(NAME server_smoke COMMAND voxel_browser_server --ticks 2 --tick-rate 60)
endif()
if(VB_BUILD_CLIENT)
  # No --loopback / --server given -> the client tries to reach the
  # default 127.0.0.1:27015, where nothing is listening. That's now a real
  # connection attempt (Phase 1.2's GnsTransport), so this smoke-tests the
  # graceful-failure path instead of a successful run: without VB_WITH_NET it
  # fails immediately (no backend); with it, ConnectByIPAddress "succeeds"
  # locally and the client only discovers no one answered after the join-wait
  # timeout (up to 10s) -- hence the generous TIMEOUT here.
  add_test(NAME client_smoke COMMAND voxel_browser --headless --frames 2)
  set_tests_properties(client_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "client: (could not connect|join failed)"
    TIMEOUT 30)
  add_test(NAME singleplayer_smoke
    COMMAND voxel_browser --headless --frames 2 --loopback --name CiBot
            --render-distance 2)
  set_tests_properties(singleplayer_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "joined singleplayer" TIMEOUT 60)
endif()

# The structure editor: load the real content/base pack headless and print a
# summary (--check opens no window).
if(VB_BUILD_EDITOR)
  add_test(NAME editor_check
    COMMAND vb_structure_editor "${CMAKE_SOURCE_DIR}/content/base/data/blocks.lua" --check)
  set_tests_properties(editor_check PROPERTIES
    PASS_REGULAR_EXPRESSION "pack 'base' at .*: [0-9]+ blocks, [0-9]+ structure files, 0 errors")
endif()

# A build without VB_WITH_AUTOMATION must reject --automation loudly instead of
# silently ignoring it (docs/e2e-automation.md §7.2).
if(NOT VB_WITH_AUTOMATION)
  if(VB_BUILD_SERVER)
    add_test(NAME server_rejects_automation COMMAND voxel_browser_server --automation stdio)
    set_tests_properties(server_rejects_automation PROPERTIES WILL_FAIL TRUE)
    add_test(NAME server_rejects_net_sim COMMAND voxel_browser_server --net-sim lag_ms=1 --ticks 1)
    set_tests_properties(server_rejects_net_sim PROPERTIES WILL_FAIL TRUE)
    add_test(NAME server_rejects_automation_tcp COMMAND voxel_browser_server --automation tcp --ticks 1)
    set_tests_properties(server_rejects_automation_tcp PROPERTIES WILL_FAIL TRUE)
  endif()
  if(VB_BUILD_CLIENT)
    add_test(NAME client_rejects_automation COMMAND voxel_browser --automation stdio)
    set_tests_properties(client_rejects_automation PROPERTIES WILL_FAIL TRUE)
    add_test(NAME client_rejects_net_sim COMMAND voxel_browser --net-sim lag_ms=1 --headless --frames 1)
    set_tests_properties(client_rejects_net_sim PROPERTIES WILL_FAIL TRUE)
    add_test(NAME client_rejects_automation_tcp COMMAND voxel_browser --automation tcp --headless --frames 1)
    set_tests_properties(client_rejects_automation_tcp PROPERTIES WILL_FAIL TRUE)
    add_test(NAME client_rejects_automation_record
      COMMAND voxel_browser --automation-record ${CMAKE_BINARY_DIR}/never.py --headless --frames 1)
    set_tests_properties(client_rejects_automation_record PROPERTIES WILL_FAIL TRUE)
    # Auth sign-in via a file is a dev/CI shortcut around the sign-in screen (auth.md §7).
    add_test(NAME client_rejects_auth_token_file
      COMMAND voxel_browser --auth-token-file ${CMAKE_BINARY_DIR}/never.txt --headless --frames 1)
    set_tests_properties(client_rejects_auth_token_file PROPERTIES WILL_FAIL TRUE)
  endif()
endif()

# End-to-end multiplayer tests (docs/e2e-automation.md §8): real voxel_browser +
# voxel_browser_server processes driven over the automation channel by pytest
# (tests/e2e/). Registered only in a build that can run one: automation (the driver),
# NET (a real UDP server/client pair), LUA (the base pack's mechanics and UI) and
# COMPRESSION (asset sync, which is how clients get the pack's UI scripts), plus a
# Python that can import pytest. Select/skip with `ctest -L e2e` / `ctest -LE e2e`.
if(VB_WITH_AUTOMATION AND VB_WITH_NET AND VB_WITH_LUA AND VB_WITH_COMPRESSION
    AND VB_BUILD_CLIENT AND VB_BUILD_SERVER)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import pytest"
      RESULT_VARIABLE _vb_pytest_rc OUTPUT_QUIET ERROR_QUIET)
    if(_vb_pytest_rc EQUAL 0)
      add_test(NAME e2e
        COMMAND ${Python3_EXECUTABLE} -m pytest -p no:cacheprovider -q -m "not slow"
                ${CMAKE_SOURCE_DIR}/tests/e2e
                --vb-build-dir ${CMAKE_BINARY_DIR}
                --vb-artifacts ${CMAKE_BINARY_DIR}/e2e-artifacts
                --junitxml=${CMAKE_BINARY_DIR}/e2e.xml)
      set_tests_properties(e2e PROPERTIES LABELS e2e TIMEOUT 900)
      # The tests that wait on a real clock (marker `slow`: the 60 s re-auth interval, the 60 s
      # unknown-key refresh limit) get their own entry so neither run nears its timeout. Its label
      # is not "e2e" on purpose: `ctest -L e2e` (a regex) must still select exactly one test.
      add_test(NAME e2e_wallclock
        COMMAND ${Python3_EXECUTABLE} -m pytest -p no:cacheprovider -q -m slow
                ${CMAKE_SOURCE_DIR}/tests/e2e
                --vb-build-dir ${CMAKE_BINARY_DIR}
                --vb-artifacts ${CMAKE_BINARY_DIR}/e2e-artifacts
                --junitxml=${CMAKE_BINARY_DIR}/e2e-wallclock.xml)
      set_tests_properties(e2e_wallclock PROPERTIES LABELS wallclock TIMEOUT 600)
      # Sanitizer builds run several times slower; vbtest scales every wait by this.
      if(VB_ENABLE_ASAN OR VB_ENABLE_UBSAN OR VB_ENABLE_TSAN)
        set_property(TEST e2e e2e_wallclock APPEND PROPERTY ENVIRONMENT VB_E2E_TIMEOUT_SCALE=3)
      endif()
    else()
      message(STATUS "e2e tests not registered: ${Python3_EXECUTABLE} cannot import pytest "
                     "(pip install -r tests/e2e/requirements.txt)")
    endif()
  else()
    message(STATUS "e2e tests not registered: no Python 3 interpreter found")
  endif()
endif()
