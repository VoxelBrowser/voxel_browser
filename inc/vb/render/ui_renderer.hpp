#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vb/render/chunk_renderer.hpp" // atlas texture + per-block UV rects, for kIcon widgets
#include "vb/render/texture_atlas.hpp" // VirtualFs
#include "vb/script/ui_runtime.hpp" // script::Widget

// Draws a vb::script::UiRuntime's widget list with raygui (spec §10.4,
// Phase 4.5). No sol2 here -- Widget is plain data; UiRuntime (vb_core) and
// UiRenderer (vb_render) are the two halves of the client UI VM split
// across the core/render boundary, same as ClientChunkStore/ChunkRenderer.

namespace vb::render {

struct UiFrameResult {
	std::vector<std::string> clicked;
	std::vector<std::pair<std::string, std::string>> changed_text;
	std::vector<std::pair<std::string, int>> changed_list;
};

class UiRenderer {
public:
	UiRenderer() = default;
	~UiRenderer();
	UiRenderer(const UiRenderer &) = delete;
	UiRenderer &operator=(const UiRenderer &) = delete;

	// Where `sprite` widgets find their textures: the synced pack (`vfs`,
	// path -> PNG bytes) first, then, if non-empty, `disk_root / path`
	// (singleplayer reads the content pack straight off disk). Drops any
	// textures cached from a previous session.
	void set_pack_source(VirtualFs vfs, std::filesystem::path disk_root = {});

	// Draws `widgets` via raygui. Call between Window::begin_frame()/
	// end_frame(), outside any BeginMode3D/EndMode3D block (raygui is a 2D
	// immediate-mode overlay). Resets per-widget text/list edit state when
	// `ui_name` differs from the previous call (a different or reopened UI).
	// `atlas_source` (optional) is the session's `ChunkRenderer` -- only
	// consulted for `kIcon` widgets, to draw a real block texture from the
	// same atlas chunk meshes already use; nullptr (or one with no atlas
	// set yet) falls back to that block's flat placeholder color.
	UiFrameResult draw(std::string_view ui_name,
			const std::vector<script::Widget> &widgets,
			const ChunkRenderer *atlas_source = nullptr);

private:
	// nullptr id == tried and failed (warned once), so it isn't retried per frame.
	const Texture2D *sprite_texture(const std::string &path);
	void unload_sprite_textures();

	VirtualFs pack_vfs_;
	std::filesystem::path pack_disk_root_;
	std::unordered_map<std::string, std::optional<Texture2D>> sprite_textures_;
	std::string last_ui_name_;
	std::unordered_map<std::string, std::string> text_buffers_;
	std::unordered_map<std::string, bool> edit_mode_;
	std::unordered_map<std::string, int> list_active_;
	std::unordered_map<std::string, int> list_scroll_;
};

} // namespace vb::render
