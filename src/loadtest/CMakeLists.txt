# vb_loadtest -- real-UDP server capacity benchmark (docs/loadtest.md). A
# development tool, built next to the server; without VB_WITH_NET it builds
# but every connect fails (GnsTransport's kBackendUnavailable stub).

add_executable(vb_loadtest main.cpp)

target_link_libraries(vb_loadtest
  PRIVATE
    vb_core
    vb_warnings
    vb_sanitizers
)

set_target_properties(vb_loadtest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
