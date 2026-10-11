#pragma once

// EntityVisualLayer's wire codec, shared by world.cpp (EntityVisualDef in
// S2C_EntityKindRegistry) and snapshot.cpp (EntityVisualOverride). Private to
// src/protocol/.

#include <cstdint>
#include <vector>

#include "vb/protocol/byte_buffer.hpp"
#include "vb/protocol/world.hpp"

namespace vb::protocol::detail {

inline void write_visual_layers(ByteWriter &w, const std::vector<EntityVisualLayer> &layers) {
	w.varint(layers.size());
	for (const auto &l : layers) {
		w.string(l.texture);
		w.boolean(l.below);
		w.u8(l.rows);
		for (std::uint8_t c : l.tint) {
			w.u8(c);
		}
	}
}

inline std::vector<EntityVisualLayer> read_visual_layers(ByteReader &r) {
	std::vector<EntityVisualLayer> layers;
	const std::uint64_t n = r.varint();
	if (n > kMaxEntityVisualLayers) {
		r.fail(core::ProtocolError::kLengthExceeded);
		return layers;
	}
	layers.reserve(static_cast<std::size_t>(n));
	for (std::uint64_t i = 0; i < n && !r.failed(); ++i) {
		EntityVisualLayer l;
		l.texture = r.string();
		l.below = r.boolean();
		l.rows = r.u8();
		for (std::uint8_t &c : l.tint) {
			c = r.u8();
		}
		layers.push_back(std::move(l));
	}
	return layers;
}

} // namespace vb::protocol::detail
