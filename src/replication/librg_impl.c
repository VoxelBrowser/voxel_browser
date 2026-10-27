// Sole translation unit for librg's implementation (docs/replication.md).
// Built as its own target (see CMakeLists.txt) so the project's -Werror
// warning flags never touch this vendored C99 code, same pattern as
// vb_raygui_impl in src/render/CMakeLists.txt.

// zpl (bundled in librg) seeds its RNG with MSVC's x86-only __rdtsc(), which
// does not exist on Windows/ARM64. The ARM64 equivalent is the virtual
// counter register; zpl only uses the value as entropy.
#if defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>
#define __rdtsc() ((unsigned __int64)_ReadStatusReg(ARM64_CNTVCT))
#endif

// zpl calls aligned_alloc() without including <stdlib.h> on POSIX; newer
// compilers reject the implicit declaration.
#include <stdlib.h>

#define LIBRG_IMPL
#include <librg.h>
