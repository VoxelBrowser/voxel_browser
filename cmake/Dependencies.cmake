# cmake/Dependencies.cmake
#
# One place for every third-party dependency. Each block tries find_package()
# first (distro / vcpkg / system install) and falls back to a pinned
# FetchContent checkout, mirroring the raylib pattern from the original skeleton.
#
# Heavy or spike-blocked dependencies are only *declared* here and are pulled in
# lazily by the VB_WITH_* option that owns them:
#
#   VB_WITH_NET         -> GameNetworkingSockets   (Phase 1.2 transport)
#   VB_WITH_REPLICATION -> librg + zpl             (Phase 1.4 replication spike)
#   VB_WITH_WORLDGEN    -> FastNoise2              (Phase 2.2 terrain)
#   VB_WITH_COMPRESSION -> lz4 + xxHash            (Phase 2.4 / 4.4 codecs)
#   VB_WITH_LUA         -> Lua 5.4 + sol2          (Phase 4 scripting)
#
# Keep the pinned tags in sync with docs/protocol.md / STATE.md when bumped.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Some pinned dependencies declare very old cmake_minimum_required() floors
# that CMake >= 4.0 rejects outright (e.g. lz4 v1.9.4 declares VERSION 2.8.12).
# doctest was bumped past v2.4.11 (which declared VERSION 3.0) specifically to
# get off this list, but lz4 still needs the floor, so the shim stays; revisit
# per-dependency as each one is bumped past whatever old floor it still pins.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# ---------------------------------------------------------------------------
# vb_fetch(<name> TAG <git-tag> REPO <url> [SUBDIR <dir>])
#   Declares a dependency and makes it available now. SUBDIR points
#   FetchContent at a nested CMakeLists.txt (used for the Lua wrapper).
# ---------------------------------------------------------------------------
function(vb_fetch NAME)
  cmake_parse_arguments(ARG "FULL_CLONE" "TAG;REPO;SUBDIR" "" ${ARGN})
  set(_extra "")
  if(ARG_SUBDIR)
    set(_extra SOURCE_SUBDIR ${ARG_SUBDIR})
  endif()
  # A commit hash (for a dependency that publishes no tags) cannot be
  # shallow-cloned by name, so FULL_CLONE turns shallow off.
  set(_shallow TRUE)
  if(ARG_FULL_CLONE)
    set(_shallow FALSE)
  endif()
  FetchContent_Declare(${NAME}
    GIT_REPOSITORY ${ARG_REPO}
    GIT_TAG ${ARG_TAG}
    GIT_SHALLOW ${_shallow}
    GIT_PROGRESS TRUE
    ${_extra}
  )
  FetchContent_MakeAvailable(${NAME})
endfunction()

if(VB_BUILD_CLIENT)

# ===========================================================================
# raylib — window / GL context / input (needed by vb_render only)
# ===========================================================================
find_package(raylib 5.5 QUIET)
if(NOT raylib_FOUND)
  set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(BUILD_GAMES OFF CACHE INTERNAL "")
  set(CUSTOMIZE_BUILD ON CACHE INTERNAL "")
  set(SUPPORT_MODULE_RAUDIO OFF CACHE INTERNAL "") # no audio subsystem in v0
  vb_fetch(raylib TAG 5.5 REPO https://github.com/raysan5/raylib.git)
endif()
if(TARGET raylib)
  get_target_property(_vb_raylib_inc raylib INTERFACE_INCLUDE_DIRECTORIES)
  if(_vb_raylib_inc)
    set_target_properties(raylib PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_vb_raylib_inc}")
  endif()
  unset(_vb_raylib_inc)
endif()

# ===========================================================================
# raygui — immediate-mode UI (header-only; implementation TU lives in vb_render)
# ===========================================================================
if(NOT TARGET vb_raygui)
  FetchContent_Declare(raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 4.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(raygui)
  add_library(vb_raygui INTERFACE)
  target_include_directories(vb_raygui SYSTEM INTERFACE "${raygui_SOURCE_DIR}/src")
  target_link_libraries(vb_raygui INTERFACE raylib)
  add_library(vb::raygui ALIAS vb_raygui)
endif()

endif() # VB_BUILD_CLIENT

# ===========================================================================
# EnTT — ECS (header-only, used by vb_core)
# ===========================================================================
find_package(EnTT QUIET)
if(NOT EnTT_FOUND AND NOT TARGET EnTT::EnTT)
  vb_fetch(entt TAG v3.13.2 REPO https://github.com/skypjack/entt.git)
  if(TARGET EnTT)
    get_target_property(_vb_entt_inc EnTT INTERFACE_INCLUDE_DIRECTORIES)
    if(_vb_entt_inc)
      set_target_properties(EnTT PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_vb_entt_inc}")
    endif()
    unset(_vb_entt_inc)
  endif()
endif()

# ===========================================================================
# tomlplusplus — server.toml / client.toml config loader (Phase 1.1)
# ===========================================================================
find_package(tomlplusplus QUIET)
if(NOT tomlplusplus_FOUND AND NOT TARGET tomlplusplus::tomlplusplus)
  vb_fetch(tomlplusplus TAG v3.4.0
    REPO https://github.com/marzer/tomlplusplus.git)
endif()

# ===========================================================================
# vb CLI downloads: libcurl (HTTPS) + miniz (zip) — only for VB_BUILD_CLI
#   libcurl: system package on Linux (libcurl4-openssl-dev) and macOS (SDK);
#   on Windows (or anywhere find_package fails) a pinned FetchContent build
#   using the OS TLS stack on Windows (Schannel — no OpenSSL to ship).
#   miniz: single-file zip reader; release archives are .zip on every OS.
# ===========================================================================
if(VB_BUILD_CLI OR VB_WITH_AUTH)
  if(NOT WIN32)
    find_package(CURL QUIET)
  endif()
  if(NOT TARGET CURL::libcurl)
    set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
    set(BUILD_TESTING OFF CACHE INTERNAL "")
    set(BUILD_LIBCURL_DOCS OFF CACHE INTERNAL "")
    set(BUILD_MISC_DOCS OFF CACHE INTERNAL "")
    set(ENABLE_CURL_MANUAL OFF CACHE INTERNAL "")
    set(CURL_DISABLE_INSTALL ON CACHE INTERNAL "")
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE INTERNAL "")
    set(HTTP_ONLY ON CACHE INTERNAL "")
    set(CURL_USE_LIBPSL OFF CACHE INTERNAL "")
    set(USE_LIBIDN2 OFF CACHE INTERNAL "")
    set(CURL_USE_LIBSSH2 OFF CACHE INTERNAL "")
    set(CURL_BROTLI OFF CACHE INTERNAL "")
    set(CURL_ZSTD OFF CACHE INTERNAL "")
    if(WIN32)
      set(CURL_USE_SCHANNEL ON CACHE INTERNAL "")
    elseif(APPLE)
      set(CURL_USE_SECTRANSP ON CACHE INTERNAL "")
    else()
      set(CURL_USE_OPENSSL ON CACHE INTERNAL "")
    endif()
    set(_vb_saved_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    vb_fetch(curl TAG curl-8_10_1 REPO https://github.com/curl/curl.git)
    set(BUILD_SHARED_LIBS ${_vb_saved_shared})
  endif()

endif()

if(VB_BUILD_CLI)
  if(NOT TARGET miniz)
    set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(BUILD_FUZZERS OFF CACHE INTERNAL "")
    set(BUILD_TESTS OFF CACHE INTERNAL "")
    set(INSTALL_PROJECT OFF CACHE INTERNAL "")
    set(AMALGAMATE_SOURCES OFF CACHE INTERNAL "")
    vb_fetch(miniz TAG 3.0.2 REPO https://github.com/richgel999/miniz.git)
  endif()
endif()

# ---------------------------------------------------------------------------
# In-engine authentication (architecture_spec/auth.md §5.4): Mbed TLS 3.6 LTS
# for RSA PKCS#1 v1.5 / ECDSA P-256 signature verification and SHA-256. HTTPS
# for discovery/JWKS reuses the libcurl block above.
# ---------------------------------------------------------------------------
if(VB_WITH_AUTH)
  set(ENABLE_PROGRAMS OFF CACHE INTERNAL "")
  set(ENABLE_TESTING OFF CACHE INTERNAL "")
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE INTERNAL "")
  set(GEN_FILES OFF CACHE INTERNAL "")
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE INTERNAL "")
  set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE INTERNAL "")
  set(_vb_saved_shared ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  vb_fetch(mbedtls TAG mbedtls-3.6.2 REPO https://github.com/Mbed-TLS/mbedtls.git)
  set(BUILD_SHARED_LIBS ${_vb_saved_shared})
endif()

# ---------------------------------------------------------------------------
# Ed25519 for release.toml signature checks (vb CLI, dev-cli.md section 3.3).
#   orlp/ed25519: public domain / zlib, five small C files, RFC 8032 compatible
#   (verified against OpenSSL-produced signatures -- see tests/unit/
#   dev_cli_signing_test.cpp). It publishes no tags, so a commit is pinned.
#   Built as its own target so the project's -Werror flags never see it.
#   Keys are in release_keys.txt at the repo root (compiled into vb).
# ---------------------------------------------------------------------------
if(VB_BUILD_CLI)
  vb_fetch(ed25519 TAG b1f19fab4aebe607805620d25a5e42566ce46a0e
    REPO https://github.com/orlp/ed25519.git FULL_CLONE)
  FetchContent_GetProperties(ed25519 SOURCE_DIR ed25519_SOURCE_DIR)
  add_library(vb_ed25519 STATIC
    "${ed25519_SOURCE_DIR}/src/fe.c" "${ed25519_SOURCE_DIR}/src/ge.c"
    "${ed25519_SOURCE_DIR}/src/sc.c" "${ed25519_SOURCE_DIR}/src/sha512.c"
    "${ed25519_SOURCE_DIR}/src/verify.c")
  target_include_directories(vb_ed25519 SYSTEM PUBLIC "${ed25519_SOURCE_DIR}/src")
  set_target_properties(vb_ed25519 PROPERTIES POSITION_INDEPENDENT_CODE ON)
  # Signing is only needed by tests (they sign fixtures at run time).
  add_library(vb_ed25519_signing STATIC
    "${ed25519_SOURCE_DIR}/src/keypair.c" "${ed25519_SOURCE_DIR}/src/sign.c")
  target_link_libraries(vb_ed25519_signing PUBLIC vb_ed25519)

  # GameNetworkingSockets bundles its own Ed25519 under the same global names
  # (ed25519_sign, ...). ELF/Mach-O linkers tolerate that; MSVC fails with
  # LNK2005 as soon as a binary pulls in both (vb_tests does). Prefix every
  # global symbol this vendored copy defines so it can never collide.
  set(_vb_ed25519_api ed25519_verify ed25519_sign ed25519_create_keypair)
  set(_vb_ed25519_internal
    fe_0 fe_1 fe_add fe_cmov fe_copy fe_cswap fe_frombytes fe_invert
    fe_isnegative fe_isnonzero fe_mul fe_mul121666 fe_neg fe_pow22523 fe_sq
    fe_sq2 fe_sub fe_tobytes
    ge_add ge_double_scalarmult_vartime ge_frombytes_negate_vartime ge_madd
    ge_msub ge_p1p1_to_p2 ge_p1p1_to_p3 ge_p2_0 ge_p2_dbl ge_p3_0 ge_p3_dbl
    ge_p3_to_cached ge_p3_to_p2 ge_p3_tobytes ge_scalarmult_base ge_sub
    ge_tobytes
    sc_muladd sc_reduce
    sha512 sha512_final sha512_init sha512_update)
  foreach(_sym IN LISTS _vb_ed25519_api)
    # PUBLIC: callers (signature.cpp, the signing tests) must see the new name too.
    target_compile_definitions(vb_ed25519 PUBLIC ${_sym}=vb_${_sym})
  endforeach()
  foreach(_sym IN LISTS _vb_ed25519_internal)
    # The signing sources call the same internals, so both libraries rename them.
    target_compile_definitions(vb_ed25519 PRIVATE ${_sym}=vb_ed25519_${_sym})
    target_compile_definitions(vb_ed25519_signing PRIVATE ${_sym}=vb_ed25519_${_sym})
  endforeach()
endif()

# ===========================================================================
# doctest — test framework (Phase 0 test target)
# ===========================================================================
if(VB_BUILD_TESTS)
  find_package(doctest QUIET)
  if(NOT doctest_FOUND AND NOT TARGET doctest::doctest)
    set(DOCTEST_WITH_TESTS OFF CACHE INTERNAL "")
    set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")
    vb_fetch(doctest TAG v2.5.3 REPO https://github.com/doctest/doctest.git)
  endif()
endif()

# ===========================================================================
# GameNetworkingSockets — reliable/unreliable UDP transport (Phase 1.2)
#   Transitive: protobuf (GNS's own CMakeLists does a plain
#   find_package(Protobuf REQUIRED) — it vendors nothing) + a crypto backend.
#   Protobuf must be a real *installed* package (headers + built libs + a
#   discoverable CMake config) — FetchContent-ing its source doesn't work:
#   protobuf only emits its CMake config as part of an install step, and the
#   library doesn't exist yet at configure time. So, unlike every other
#   dependency here, protobuf (and OpenSSL where BCrypt isn't available) come
#   from a package manager instead of FetchContent:
#     Windows: vcpkg (`vcpkg install protobuf`), pass
#       -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake.
#       GitHub's windows-latest runners ship vcpkg pre-installed.
#     Linux:   apt-get install protobuf-compiler libprotobuf-dev libssl-dev
#     macOS:   brew install protobuf openssl
#   Crypto: Windows uses BCrypt (built into the OS — no OpenSSL needed there);
#   Linux/macOS use GNS's default, system OpenSSL.
#   GNS itself is still FetchContent'd (its own CMakeLists builds cleanly from
#   source once protobuf/crypto are satisfied — the fragile link was only
#   protobuf). ICE/WebRTC (P2P NAT punching) is off: this engine only ever
#   dials a known dedicated-server address, and ICE pulls a multi-hundred-MB
#   webrtc submodule for a feature we'd never use.
# ===========================================================================
if(VB_WITH_NET)
  find_package(GameNetworkingSockets QUIET)
  if(NOT GameNetworkingSockets_FOUND)
    # Deliberately NOT calling find_package(Protobuf REQUIRED) here ourselves:
    # GNS's own src/CMakeLists.txt already does, and calling it a second time
    # in the same configure run is unsafe with some protobuf installs (seen
    # with Homebrew's on macOS) -- protobuf's generated config fatal-errors
    # ("Some (but not all) targets in this export set were already defined")
    # because the two find_package() calls request slightly different
    # component sets. Let GNS's own call be the only one; it already fails
    # with a clear "Could NOT find Protobuf" message if it's missing.

    message(STATUS "vb: fetching GameNetworkingSockets")
    set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(BUILD_TESTS OFF CACHE INTERNAL "")
    set(BUILD_SHARED_LIB OFF CACHE INTERNAL "") # we only ever link GameNetworkingSockets::static
    set(ENABLE_ICE OFF CACHE INTERNAL "")
    set(USE_STEAMWEBRTC OFF CACHE INTERNAL "")
    if(WIN32)
      set(USE_CRYPTO "BCrypt" CACHE STRING "" FORCE)
    endif()

    FetchContent_Declare(gamenetworkingsockets
      GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
      GIT_TAG v1.6.0
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
      # Skip the webrtc submodule (only needed by ICE, which is off above).
      GIT_SUBMODULES "src/external/abseil;src/external/vjson")
    FetchContent_MakeAvailable(gamenetworkingsockets)
    # v1.6.0's minbase_identify.h only recognises x86 under MSVC and hits
    # `#error "Cannot determine endianness of platform!"` on Windows/ARM64.
    # ARM64 Windows is little-endian; say so for the GNS target (and whatever
    # links it) rather than patching the fetched sources.
    if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" AND TARGET GameNetworkingSockets_s)
      target_compile_definitions(GameNetworkingSockets_s PUBLIC VALVE_LITTLE_ENDIAN=1)
    endif()
  endif()
endif()

# ===========================================================================
# librg — interest management / entity streaming (spec §18 Q3, docs/replication.md)
#   v7.4.0 is a single self-contained header (code/librg.h bundles its own zpl —
#   no separate zpl dependency). LIBRG_IMPL goes in exactly one TU, built as its
#   own target so the project warning flags don't touch the C code.
# ===========================================================================
if(VB_WITH_REPLICATION AND NOT TARGET vb_librg)
  FetchContent_Declare(librg
    GIT_REPOSITORY https://github.com/zpl-c/librg.git
    GIT_TAG v7.4.0 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(librg)
  add_library(vb_librg INTERFACE)
  target_include_directories(vb_librg SYSTEM INTERFACE "${librg_SOURCE_DIR}/code")
  add_library(vb::librg ALIAS vb_librg)
endif()

# ===========================================================================
# FastNoise2 — world generation noise trees (Phase 2)
# ===========================================================================
if(VB_WITH_WORLDGEN)
  find_package(FastNoise2 QUIET)
  if(NOT FastNoise2_FOUND)
    set(FASTNOISE2_NOISETOOL OFF CACHE INTERNAL "")
    set(FASTNOISE2_TESTS OFF CACHE INTERNAL "")
    # NOTE: the real tag is "v0.10.0-alpha" -- there is no plain "v0.10.0" in
    # Auburn/FastNoise2 (confirmed via `git ls-remote --tags`, Phase 6.14).
    # The bare "v0.10.0" pin was apparently never actually exercised before
    # this phase (VB_WITH_WORLDGEN had zero call sites anywhere until now),
    # so the typo went unnoticed.
    vb_fetch(fastnoise2 TAG v0.10.0-alpha REPO https://github.com/Auburn/FastNoise2.git)
  endif()
endif()

# ===========================================================================
# lz4 + xxHash — chunk / asset compression + content hashing (Phase 2/4)
# ===========================================================================
if(VB_WITH_COMPRESSION)
  # A universal macOS build (CMAKE_OSX_ARCHITECTURES "arm64;x86_64") can't link
  # Homebrew's single-arch lz4 (x86_64 slice: undefined LZ4_* symbols), so
  # build it from source there.
  if(NOT (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";"))
    find_package(lz4 QUIET)
  endif()
  if(NOT lz4_FOUND AND NOT TARGET LZ4::lz4)
    set(LZ4_BUILD_CLI OFF CACHE INTERNAL "")
    set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE INTERNAL "")
    FetchContent_Declare(lz4
      GIT_REPOSITORY https://github.com/lz4/lz4.git
      GIT_TAG v1.9.4 GIT_SHALLOW TRUE
      SOURCE_SUBDIR build/cmake)
    FetchContent_MakeAvailable(lz4)
  endif()

  find_package(xxHash QUIET)
  if(NOT xxHash_FOUND AND NOT TARGET xxHash::xxhash)
    set(XXHASH_BUILD_XXHSUM OFF CACHE INTERNAL "")
    FetchContent_Declare(xxhash
      GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
      GIT_TAG v0.8.2 GIT_SHALLOW TRUE
      SOURCE_SUBDIR cmake_unofficial)
    FetchContent_MakeAvailable(xxhash)
  endif()
endif()

# ===========================================================================
# Lua 5.4 + sol2 — scripting (Phase 4). Binding layer decision: sol2 (§18 Q1).
#   PUC-Lua ships no CMake, so cmake/lua/CMakeLists.txt wraps it into a
#   static `lua_static` target with a `lua::lua` alias.
# ===========================================================================
if(VB_WITH_LUA)
  find_package(Lua 5.4 QUIET)
  if(NOT LUA_FOUND AND NOT TARGET lua::lua)
    FetchContent_Declare(lua_src
      GIT_REPOSITORY https://github.com/lua/lua.git
      GIT_TAG v5.4.6 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(lua_src)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/cmake/lua" "${CMAKE_BINARY_DIR}/lua_wrapper")
  endif()

  find_package(sol2 QUIET)
  if(NOT sol2_FOUND AND NOT TARGET sol2::sol2)
    set(SOL2_BUILD_LUA OFF CACHE INTERNAL "")
    # v3.3.0's bundled "better optional" doesn't compile under Clang >= 18
    # (no member 'construct' in optional<T&>); v3.5.0 fixes it.
    vb_fetch(sol2 TAG v3.5.0 REPO https://github.com/ThePhD/sol2.git)
  endif()
  if(TARGET sol2::sol2)
    # Real, previously-unfixed engine bug (REMAINING_TASKS.md's Cross-Cutting
    # "PlayerHandle stashed across ticks" item): sol2's default (this macro
    # OFF) pushes a *non-const lvalue reference* to a registered usertype
    # argument as an `as_reference_tag` -- a raw pointer into the caller's own
    # C++ stack frame, not a copy (see sol2's stack_core.hpp,
    # stack_detail::push_reference<T>) -- purely as a perf optimization for
    # the common case where a Lua handler only uses the argument during the
    # call. Every PlayerHandle dispatch call site in pack_runtime.cpp
    # constructs a named local (`PlayerHandle p{...}`) and passes it into a
    # sol2 call (`fn(p, ...)`, `fire(event, p, ...)`) -- an lvalue every
    # time -- so a pack script that stores that argument anywhere (a global,
    # a table field, a closure upvalue) beyond the call it was received in
    # ends up holding a pointer to a C++ local that's already been popped off
    # the stack by the time it's read back, reliably corrupted (not a race)
    # the moment a *different* call path reuses that stack address for
    # something else. Confirmed empirically (temporary instrumentation
    # printed the extracted `self` pointer and matched it byte-for-byte
    # against `&p` of the constructing call's own local). PlayerHandle is the
    # only usertype this project registers, is a stateless proxy (no method
    # ever mutates net_id/rt in place), and its own doc comment already
    # assumes value semantics ("constructed fresh per dispatch call, so it
    # can never dangle") -- this define makes sol2 actually honor that: it
    # forces every registered-usertype function-call argument to push as an
    # owned copy regardless of value category, restoring the invariant the
    # type was designed around. Applied to the sol2::sol2 INTERFACE target
    # (not a single TU's #define) so every translation unit that ever pushes
    # a usertype through a Lua call sees the same behavior.
    # `sol2::sol2` is normally an ALIAS (sol2's own CMakeLists.txt aliases its
    # real `sol2` INTERFACE target) -- target_compile_definitions() rejects
    # alias targets, so resolve to the real one first. A find_package()-found
    # sol2 might expose sol2::sol2 as a real (non-alias) target instead, so
    # fall back to it directly if there's no ALIASED_TARGET.
    get_target_property(_vb_sol2_real_target sol2::sol2 ALIASED_TARGET)
    if(NOT _vb_sol2_real_target)
      set(_vb_sol2_real_target sol2::sol2)
    endif()
    target_compile_definitions(${_vb_sol2_real_target} INTERFACE SOL_FUNCTION_CALL_VALUE_SEMANTICS=1)
    unset(_vb_sol2_real_target)
  endif()
endif()

# nlohmann/json — vb.storage persistence + player:open_ui ctx serialization
# (Phase 4.2), the development-only automation protocol (e2e design), and the
# vb CLI's --json output / GitHub release listing (8.5). Header-only.
if(VB_WITH_LUA OR VB_WITH_AUTOMATION OR VB_BUILD_CLI OR VB_WITH_AUTH)
  find_package(nlohmann_json QUIET)
  if(NOT nlohmann_json_FOUND AND NOT TARGET nlohmann_json::nlohmann_json)
    vb_fetch(nlohmann_json TAG v3.11.3 REPO https://github.com/nlohmann/json.git)
  endif()
endif()
