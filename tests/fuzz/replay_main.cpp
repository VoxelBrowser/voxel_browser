// Corpus-replay driver for the fuzz targets (see fuzz.hpp).
//
//   fuzz_x_replay [--mutations N] [--write-seeds DIR] [FILE|DIR ...]
//
// Runs the target's built-in seeds plus every file under the given paths; with
// --mutations it also runs N deterministic mutations (byte flips, truncations, insertions,
// splices) of every input. --write-seeds writes the seeds as files and exits
// (to start a libFuzzer corpus).

#include "fuzz.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

void vb_fuzz_fail(const char *what, const char *file, int line) {
	std::fprintf(stderr, "fuzz property violated: %s (%s:%d)\n", what, file, line);
	std::abort();
}

namespace {

std::vector<std::uint8_t> read_file(const fs::path &p) {
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void collect(const fs::path &p, std::vector<std::vector<std::uint8_t>> &out) {
	if (fs::is_directory(p)) {
		for (const auto &e : fs::recursive_directory_iterator(p)) {
			if (e.is_regular_file()) {
				out.push_back(read_file(e.path()));
			}
		}
	} else {
		out.push_back(read_file(p));
	}
}

struct Rng {
	std::uint32_t s = 0x9e3779b9u;
	std::uint32_t next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	std::size_t below(std::size_t n) { return n == 0 ? 0 : next() % n; }
};

std::vector<std::uint8_t> mutate(const std::vector<std::vector<std::uint8_t>> &inputs, Rng &rng) {
	std::vector<std::uint8_t> m = inputs[rng.below(inputs.size())];
	const int edits = 1 + static_cast<int>(rng.below(4));
	for (int i = 0; i < edits; ++i) {
		switch (rng.below(5)) {
			case 0: // flip a byte
				if (!m.empty()) {
					m[rng.below(m.size())] ^= static_cast<std::uint8_t>(1u + rng.below(255));
				}
				break;
			case 1: // set an "interesting" value
				if (!m.empty()) {
					static const std::uint8_t kInteresting[] = { 0x00, 0x01, 0x7f, 0x80, 0xff };
					m[rng.below(m.size())] = kInteresting[rng.below(sizeof(kInteresting))];
				}
				break;
			case 2: // truncate
				m.resize(rng.below(m.size() + 1));
				break;
			case 3: // insert random bytes
			{
				const std::size_t at = rng.below(m.size() + 1);
				const std::size_t n = 1 + rng.below(8);
				for (std::size_t k = 0; k < n; ++k) {
					m.insert(m.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::uint8_t>(rng.next()));
				}
				break;
			}
			default: // splice the tail of another input
			{
				const auto &other = inputs[rng.below(inputs.size())];
				const std::size_t cut = rng.below(m.size() + 1);
				const std::size_t from = rng.below(other.size() + 1);
				m.resize(cut);
				m.insert(m.end(), other.begin() + static_cast<std::ptrdiff_t>(from), other.end());
				break;
			}
		}
	}
	return m;
}

void run(const std::vector<std::uint8_t> &in) {
	// Copy into an exactly-sized heap buffer so ASan sees reads past the end.
	auto *buf = new std::uint8_t[in.empty() ? 1 : in.size()];
	if (!in.empty()) {
		std::memcpy(buf, in.data(), in.size());
	}
	LLVMFuzzerTestOneInput(buf, in.size());
	delete[] buf;
}

} // namespace

int main(int argc, char **argv) {
	long mutations = 0;
	std::vector<std::vector<std::uint8_t>> inputs = vb_fuzz_seeds();
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--mutations" && i + 1 < argc) {
			mutations = std::strtol(argv[++i], nullptr, 10);
		} else if (a == "--write-seeds" && i + 1 < argc) {
			const fs::path dir = argv[++i];
			fs::create_directories(dir);
			int n = 0;
			for (const auto &s : vb_fuzz_seeds()) {
				std::ofstream out(dir / ("seed-" + std::to_string(n++)), std::ios::binary);
				out.write(reinterpret_cast<const char *>(s.data()), static_cast<std::streamsize>(s.size()));
			}
			std::printf("wrote %d seeds to %s\n", n, dir.string().c_str());
			return 0;
		} else {
			collect(a, inputs);
		}
	}
	if (inputs.empty()) {
		inputs.emplace_back();
	}
	for (const auto &in : inputs) {
		run(in);
	}
	Rng rng;
	for (long i = 0; i < mutations; ++i) {
		run(mutate(inputs, rng));
	}
	std::printf("ran %zu inputs + %ld mutations\n", inputs.size(), mutations);
	return 0;
}
