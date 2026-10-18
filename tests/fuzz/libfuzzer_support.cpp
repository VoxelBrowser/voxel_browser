// libFuzzer builds only: the failure hook from fuzz.hpp (replay_main.cpp has
// its own). libFuzzer reports the abort as a crash and saves the input.

#include "fuzz.hpp"

#include <cstdio>
#include <cstdlib>

void vb_fuzz_fail(const char *what, const char *file, int line) {
	std::fprintf(stderr, "fuzz property violated: %s (%s:%d)\n", what, file, line);
	std::abort();
}
