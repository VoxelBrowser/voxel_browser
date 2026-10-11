#pragma once

#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vb/protocol/world.hpp"

// Pure spritesheet layout math for a script entity kind's real per-kind
// visual (spec architecture_spec/rendering.md §11.3, entity-management
// follow-up). Sibling to vb/render/entity_visual.hpp (pose/clip selection)
// but depends on vb::protocol::EntityVisualDef, so it lives in its own
// header. No raylib dependency -- header-only, unit-tested without a GL
// context the same way entity_visual.hpp already is.

namespace vb::render {

// One clip's resolved column run within a pose row.
struct EntityClipLayout {
	std::string name;
	int frames = 0;
	float fps = 1.0f;
	int start_frame = 0; // running-sum column offset, in frames

	bool operator==(const EntityClipLayout &) const = default;
};

struct EntityVisualLayout {
	int frame_width = 0;
	int frame_height = 0;
	int facings = 8;
	bool mirror = true; // whether the engine flips the authored rows for the unauthored half
	int rows = 1; // mirror ? facings/2+1 : facings -- the pose-row count select_pose() indexes into
	float origin_x = 0.5f;
	float origin_y = 1.0f;
	std::vector<EntityClipLayout> clips;

	bool operator==(const EntityVisualLayout &) const = default;
};

// Computes the running per-clip column layout from `def.clips` and validates
// it against a decoded sheet's real pixel dimensions (rendering.md: "the
// required sheet size is derived and validated exactly at load time...
// a mismatch is a pack load error, not a silent misdraw"). nullopt on any
// mismatch -- bad pack data, not a silent misdraw.
inline std::optional<EntityVisualLayout> build_entity_visual_layout(
		const protocol::EntityVisualDef &def, int image_width, int image_height) {
	if (def.frame_width == 0 || def.frame_height == 0 || def.clips.empty()) {
		return std::nullopt;
	}

	EntityVisualLayout layout;
	layout.frame_width = def.frame_width;
	layout.frame_height = def.frame_height;
	layout.facings = def.facings;
	layout.mirror = def.mirror;
	layout.rows = def.mirror ? (def.facings / 2 + 1) : def.facings;
	layout.origin_x = def.origin_x;
	layout.origin_y = def.origin_y;

	int running = 0;
	layout.clips.reserve(def.clips.size());
	for (const auto &c : def.clips) {
		if (c.frames == 0 || c.fps <= 0.0f) {
			return std::nullopt;
		}
		layout.clips.push_back({ c.clip, c.frames, c.fps, running });
		running += c.frames;
	}

	const int required_width = layout.frame_width * running;
	const int required_height = layout.frame_height * layout.rows;
	if (image_width != required_width || image_height != required_height) {
		return std::nullopt;
	}
	return layout;
}

// Per-instance override merge (spec architecture_spec/rendering.md §11.3's
// "Per-instance override" -- a script entity's own
// protocol::EntityVisualOverride, field-by-field over its kind's
// protocol::EntityVisualDef). Every unset override field inherits the kind's
// own value unchanged; `frame_width`/`frame_height` and `origin_x`/
// `origin_y` are only ever set as pairs on EntityVisualOverride, so no
// half-pair inheritance case exists to handle here.
inline protocol::EntityVisualDef merge_visual_override(
		const protocol::EntityVisualDef &kind_default,
		const protocol::EntityVisualOverride &over) {
	protocol::EntityVisualDef out = kind_default;
	if (over.texture) {
		out.texture = *over.texture;
	}
	if (over.frame_width && over.frame_height) {
		out.frame_width = *over.frame_width;
		out.frame_height = *over.frame_height;
	}
	if (over.facings) {
		out.facings = *over.facings;
	}
	if (over.mirror) {
		out.mirror = *over.mirror;
	}
	if (over.origin_x && over.origin_y) {
		out.origin_x = *over.origin_x;
		out.origin_y = *over.origin_y;
	}
	if (over.clips) {
		out.clips = *over.clips;
	}
	if (over.layer) {
		out.layer = *over.layer;
	}
	if (over.through_walls) {
		out.through_walls = *over.through_walls;
	}
	if (over.layers) {
		out.layers = *over.layers;
	}
	return out;
}

// Paper-doll layers (protocol::EntityVisualLayer). A layer is drawn on pose
// row `row` unless its `rows` mask excludes it (0 = every row).
inline bool layer_applies_to_row(std::uint8_t rows_mask, int row) {
	return rows_mask == 0 || (row >= 0 && row < 8 && ((rows_mask >> row) & 1u) != 0);
}

// A layer sheet must be exactly the base sheet's size: it is sampled with the
// base frame's source rectangle, so any other size would misalign.
inline bool layer_sheet_matches(int base_width, int base_height, int layer_width,
		int layer_height) {
	return base_width == layer_width && base_height == layer_height;
}

// Draw order of a layered billboard on pose row `row`: indices into
// `layers` with -1 standing for the base sheet -- `below` layers first (in
// list order), then the base, then the rest. Layers that don't apply to the
// row are left out.
inline std::vector<int> layer_draw_order(
		const std::vector<protocol::EntityVisualLayer> &layers, int row) {
	std::vector<int> order;
	order.reserve(layers.size() + 1);
	for (std::size_t i = 0; i < layers.size(); ++i) {
		if (layers[i].below && layer_applies_to_row(layers[i].rows, row)) {
			order.push_back(static_cast<int>(i));
		}
	}
	order.push_back(-1);
	for (std::size_t i = 0; i < layers.size(); ++i) {
		if (!layers[i].below && layer_applies_to_row(layers[i].rows, row)) {
			order.push_back(static_cast<int>(i));
		}
	}
	return order;
}

// Exact-name lookup; falls back to the first declared clip if `name` isn't
// present (rendering.md: "falls back to the first declared clip rather than
// erroring at runtime"). `layout.clips` is never empty for a layout returned
// by build_entity_visual_layout() above.
inline const EntityClipLayout &resolve_clip(
		const EntityVisualLayout &layout, std::string_view name) {
	for (const auto &c : layout.clips) {
		if (c.name == name) {
			return c;
		}
	}
	return layout.clips.front();
}

} // namespace vb::render
