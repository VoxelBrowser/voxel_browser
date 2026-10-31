#include "vb/render/resources.hpp"

namespace vb::render::resources {

Blob find(std::string_view path) {
	for (std::size_t i = 0; i < kEmbeddedFileCount; ++i) {
		if (path == kEmbeddedFiles[i].path) {
			return Blob{ kEmbeddedFiles[i].data, kEmbeddedFiles[i].size };
		}
	}
	return Blob{};
}

} // namespace vb::render::resources
