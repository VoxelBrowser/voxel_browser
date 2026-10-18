#pragma once

// Shared contract of the fuzz targets in tests/fuzz/ (libFuzzer-style).
//
// Every target defines the two functions below. Built with
// -DVB_BUILD_FUZZERS=ON (Clang) a target links libFuzzer; every other build
// links replay_main.cpp instead, which runs the target's seeds plus a fixed
// number of deterministic mutations of them, so the same code runs under
// ctest (and the sanitizer legs) on every platform.

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

// Valid inputs to start from (written out with `--write-seeds <dir>` for a
// libFuzzer corpus).
std::vector<std::vector<std::uint8_t>> vb_fuzz_seeds();

// A crash is the failure signal for both drivers; a property violation
// (e.g. decode(encode(x)) != x) aborts.
#define VB_FUZZ_CHECK(cond)                          \
	do {                                             \
		if (!(cond)) {                               \
			vb_fuzz_fail(#cond, __FILE__, __LINE__); \
		}                                            \
	} while (0)

[[noreturn]] void vb_fuzz_fail(const char *what, const char *file, int line);
