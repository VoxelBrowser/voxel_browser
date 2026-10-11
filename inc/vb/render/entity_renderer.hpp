#pragma once

#include <cstddef>
#include <memory>

#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"
#include "vb/protocol/world.hpp" // EntityVisualDef
#include "vb/render/texture_atlas.hpp" // VirtualFs

// Draws a Y-axis-billboarded sprite for each replicated remote entity (spec
// §11.3). Phase 3 ships a single hardcoded flat-tinted placeholder frame (no
// pack/Lua dependency); the direction-bucket + animation-clip machinery
// (vb/render/entity_visual.hpp) is already wired, so swapping in real
// per-entity-kind atlases later (Phase 4.2/4.4/5.1) only changes how a frame
// is looked up, not this class's structure.
//
// Like ChunkRenderer, only ever construct this when the window isn't
// headless -- the constructor uploads a GPU texture.

namespace vb::net {
class ClientSession;
}

namespace vb::render {

class ChunkRenderer;

struct CameraView {
	core::Vec3d position{};
	core::Vec3d target{};
};

class EntityRenderer {
public:
	EntityRenderer();
	~EntityRenderer();

	EntityRenderer(const EntityRenderer &) = delete;
	EntityRenderer &operator=(const EntityRenderer &) = delete;

	// Decodes `def.texture`'s bytes out of `vfs`, validates the sheet against
	// `def`'s declared layout (vb::render::build_entity_visual_layout), and
	// uploads it as a real GPU texture for `id`'s billboard to use going
	// forward. Missing bytes or a failed validation logs VB_WARN and leaves
	// this kind on the flat placeholder -- same "missing = default" posture
	// as ClientSession::entity_kind() itself. Call once per session, right
	// after construction, for every S2C_EntityKindRegistry record that set a
	// `visual` (mirrors how ChunkRenderer::set_atlas() is wired).
	void set_kind_visual(core::EntityKindId id,
			const protocol::EntityVisualDef &def, const VirtualFs &vfs);

	// Entity-management follow-up: keeps a copy of the synced/on-disk pack
	// filesystem so sync() can lazily decode a per-instance visual override's
	// texture the first time it sees one (an override's spawn time isn't
	// known up front the way every kind's `visual` is at join, so this can't
	// be a one-shot loop over a fixed list the way set_kind_visual's join-time
	// caller works). Assets don't change post-join (no manifest-staleness
	// story exists for mid-session pack writes -- see REMAINING_TASKS.md), so
	// one copy taken right after join stays valid for the whole session.
	void set_virtual_fs(VirtualFs vfs);

	// Dropped-item entities (net::ClientSession::entity_item) are drawn as a
	// small spinning cube coloured like the block they came from, using
	// `chunks`' block colours. Must outlive this renderer; nullptr (the
	// default) keeps drops on their kind's sprite.
	void set_block_colors(const ChunkRenderer *chunks);

	// Refresh per-entity animation clip + facing from `client`'s replicated
	// remote entities (spec §8.4's remote_entities()/interpolated_pos()). Call
	// once per frame, before draw().
	void sync(const net::ClientSession &client, const CameraView &camera,
			double dt_seconds);

	// Draw a billboard for every tracked entity. Call inside
	// BeginMode3D/EndMode3D.
	void draw(const CameraView &camera) const;

	// Draw every tracked entity's text label (S2C_EntityProps) as a
	// camera-facing billboard. Call inside BeginMode3D/EndMode3D after
	// everything else in the 3D pass: labels test depth (walls hide them
	// unless the label set through_walls) but never write it.
	void draw_labels(const CameraView &camera) const;

	std::size_t tracked_count() const;

private:
	core::Vec3d resolve_render_pos(const net::ClientSession &client, core::NetId id,
			const CameraView &camera, int depth) const;

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace vb::render
