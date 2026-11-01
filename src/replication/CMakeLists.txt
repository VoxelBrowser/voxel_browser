# vb/replication — interest management (spec §8.4, docs/replication.md).
# Compiled into vb_core. interest.cpp picks its backend at compile time via
# VB_WITH_REPLICATION (see src/core/CMakeLists.txt for the compile
# definition and cmake/Dependencies.cmake for the vb::librg target).

target_sources(vb_core PRIVATE
  interest.cpp
)

if(VB_WITH_REPLICATION)
  # librg is vendored C99; its LIBRG_IMPL translation unit must not see our
  # -Werror flags -- same isolation pattern as vb_raygui_impl
  # (src/render/CMakeLists.txt). Built as C11, not C99: zpl calls aligned_alloc(),
  # which newer glibc only declares in C11 mode.
  add_library(vb_librg_impl STATIC librg_impl.c)
  target_link_libraries(vb_librg_impl PUBLIC vb_librg)
  set_target_properties(vb_librg_impl PROPERTIES C_STANDARD 11)
  target_link_libraries(vb_core PRIVATE vb_librg_impl)
endif()
