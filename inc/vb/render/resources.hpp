#pragma once

#include <cstddef>
#include <string_view>

// Menu assets baked into vb_render at build time (cmake/EmbedFiles.cmake, src/render/resources/):
// the dirt backdrop texture and the two UI fonts. Keys are file names under resources/.

namespace vb::render::resources {

struct EmbeddedFile {
	const char *path; // nullptr terminates the table
	const unsigned char *data;
	std::size_t size;
};

extern const EmbeddedFile kEmbeddedFiles[];
extern const std::size_t kEmbeddedFileCount;

// The embedded file named `path`, or {nullptr, 0} when there is none.
struct Blob {
	const unsigned char *data = nullptr;
	std::size_t size = 0;
};
Blob find(std::string_view path);

} // namespace vb::render::resources
