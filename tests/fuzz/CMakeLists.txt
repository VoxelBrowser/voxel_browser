# Fuzz targets for the decoders that parse untrusted bytes (fuzz.hpp has the contract).
#
# Every build: fuzz_<name>_replay runs the target's seeds plus deterministic mutations, registered
# with ctest as fuzz_<name> so the sanitizer legs exercise it too.
# -DVB_BUILD_FUZZERS=ON (Clang): fuzz_<name> is a real libFuzzer binary as well. Start a corpus with
#   ./fuzz_<name>_replay --write-seeds corpus/<name> && ./fuzz_<name> corpus/<name> -max_total_time=60

set(VB_FUZZ_MUTATIONS 20000 CACHE STRING "Mutations each fuzz_* ctest entry runs")

function(vb_add_fuzz short)
  cmake_parse_arguments(F "" "" "SOURCES;LIBS" ${ARGN})
  set(name fuzz_${short})
  add_executable(${name}_replay ${F_SOURCES} replay_main.cpp)
  target_link_libraries(${name}_replay PRIVATE ${F_LIBS} vb_warnings vb_sanitizers)
  set_target_properties(${name}_replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  # regressions/<short>/ holds inputs that once crashed a target; they replay on every run.
  add_test(NAME ${name} COMMAND ${name}_replay --mutations ${VB_FUZZ_MUTATIONS}
           ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${short})
  set_tests_properties(${name} PROPERTIES LABELS fuzz TIMEOUT 600)

  if(VB_BUILD_FUZZERS)
    add_executable(${name} ${F_SOURCES} libfuzzer_support.cpp)
    target_link_libraries(${name} PRIVATE ${F_LIBS} vb_warnings)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  endif()
endfunction()

vb_add_fuzz(protocol SOURCES fuzz_protocol.cpp LIBS vb_core)

if(VB_WITH_AUTH)
  vb_add_fuzz(auth SOURCES fuzz_auth.cpp LIBS vb_auth)
endif()
