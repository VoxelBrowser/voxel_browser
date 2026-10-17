#include "vb/render/ui_renderer.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

#include <raygui.h>

#include "vb/core/log.hpp"

namespace vb::render {

namespace {

constexpr int kTextBoxBufferSize = 256;

} // namespace

UiRenderer::~UiRenderer() {
	unload_sprite_textures();
}

void UiRenderer::unload_sprite_textures() {
	// No window means no GL context, and nothing was ever uploaded.
	if (IsWindowReady()) {
		for (auto &[path, tex] : sprite_textures_) {
			(void)path;
			if (tex && tex->id != 0) {
				UnloadTexture(*tex);
			}
		}
	}
	sprite_textures_.clear();
}

void UiRenderer::set_pack_source(VirtualFs vfs, std::filesystem::path disk_root) {
	unload_sprite_textures();
	pack_vfs_ = std::move(vfs);
	pack_disk_root_ = std::move(disk_root);
}

const Texture2D *UiRenderer::sprite_texture(const std::string &path) {
	if (const auto it = sprite_textures_.find(path); it != sprite_textures_.end()) {
		return it->second ? &*it->second : nullptr;
	}
	std::vector<unsigned char> bytes;
	if (const auto it = pack_vfs_.find(path); it != pack_vfs_.end()) {
		bytes.assign(reinterpret_cast<const unsigned char *>(it->second.data()),
				reinterpret_cast<const unsigned char *>(it->second.data()) + it->second.size());
	} else if (!pack_disk_root_.empty() && path.find("..") == std::string::npos) {
		std::ifstream f(pack_disk_root_ / path, std::ios::binary);
		if (f) {
			bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
	}
	std::optional<Texture2D> loaded;
	if (bytes.empty()) {
		VB_WARN("render", "ui sprite: texture '", path, "' not found in the pack");
	} else {
		Image img = LoadImageFromMemory(".png", bytes.data(), static_cast<int>(bytes.size()));
		if (img.data == nullptr) {
			VB_WARN("render", "ui sprite: failed to decode '", path, "'");
		} else {
			loaded = LoadTextureFromImage(img);
			UnloadImage(img);
			SetTextureFilter(*loaded, TEXTURE_FILTER_POINT); // pixel art
		}
	}
	const auto [it, inserted] = sprite_textures_.emplace(path, std::move(loaded));
	(void)inserted;
	return it->second ? &*it->second : nullptr;
}

UiFrameResult UiRenderer::draw(std::string_view ui_name,
		const std::vector<script::Widget> &widgets, const ChunkRenderer *atlas_source) {
	if (ui_name != last_ui_name_) {
		text_buffers_.clear();
		edit_mode_.clear();
		list_active_.clear();
		list_scroll_.clear();
		last_ui_name_ = std::string(ui_name);
	}

	UiFrameResult result;

	for (const auto &w : widgets) {
		const Rectangle bounds{ w.x, w.y, w.w, w.h };
		switch (w.type) {
			case script::WidgetType::kLabel:
				GuiLabel(bounds, w.text.c_str());
				break;
			case script::WidgetType::kPanel:
				GuiPanel(bounds, w.text.c_str());
				break;
			case script::WidgetType::kButton:
				if (GuiButton(bounds, w.text.c_str())) {
					result.clicked.push_back(w.id);
				}
				break;
			case script::WidgetType::kTextBox: {
				auto buf_it = text_buffers_.find(w.id);
				if (buf_it == text_buffers_.end()) {
					buf_it = text_buffers_.emplace(w.id, w.text).first;
				}
				std::string &buf = buf_it->second;
				buf.resize(kTextBoxBufferSize, '\0');
				bool &edit = edit_mode_[w.id];
				const bool was_editing = edit;
				if (GuiTextBox(bounds, buf.data(), kTextBoxBufferSize, edit)) {
					edit = !edit;
				}
				buf.resize(std::strlen(buf.c_str()));
				if (was_editing && !edit) {
					result.changed_text.emplace_back(w.id, buf);
				}
				break;
			}
			case script::WidgetType::kRect: {
				// The one non-interactive, no-baked-in-meaning primitive
				// (Phase 6.16): plain raylib rectangle draws, no raygui
				// control involved -- Lua decides what this rectangle
				// *means* (a progress bar fill, a divider, a health bar
				// segment, ...), the engine just draws a box.
				DrawRectangle(static_cast<int>(w.x), static_cast<int>(w.y),
						static_cast<int>(w.w), static_cast<int>(w.h),
						Color{ w.fill_r, w.fill_g, w.fill_b, w.fill_a });
				if (w.border_a > 0) {
					DrawRectangleLines(static_cast<int>(w.x), static_cast<int>(w.y),
							static_cast<int>(w.w), static_cast<int>(w.h),
							Color{ w.border_r, w.border_g, w.border_b, w.border_a });
				}
				break;
			}
			case script::WidgetType::kText: {
				// Raw, non-raygui text draw (Phase 6.16 follow-up): the only
				// primitive here that needs raylib's MeasureText, which is
				// exactly why it lives in this raylib-linked layer rather
				// than vb::script::UiRuntime -- `x` is the anchor `align`
				// is relative to, not always the left edge Lua would
				// otherwise have to guess by pre-measuring text width itself.
				const int text_w = MeasureText(w.text.c_str(), w.font_size);
				float draw_x = w.x;
				if (w.align == script::TextAlign::kRight) {
					draw_x = w.x - static_cast<float>(text_w);
				} else if (w.align == script::TextAlign::kCenter) {
					draw_x = w.x - static_cast<float>(text_w) / 2.0f;
				}
				DrawText(w.text.c_str(), static_cast<int>(draw_x), static_cast<int>(w.y),
						w.font_size, Color{ w.fill_r, w.fill_g, w.fill_b, w.fill_a });
				break;
			}
			case script::WidgetType::kIcon: {
				// Real block texture from the shared chunk atlas when one's
				// been uploaded (entity-management follow-up: an "item"
				// primitive, not a baked-in "item grid" concept -- content
				// composes a real inventory grid out of these plus
				// kRect/kText, same posture Phase 6.16 gave kRect for
				// progress bars). No atlas yet falls back to the same flat
				// placeholder color a chunk mesh itself would use.
				if (atlas_source != nullptr && atlas_source->has_atlas()) {
					const AtlasRect &r = atlas_source->atlas_rect_for(w.item);
					const Texture2D tex = atlas_source->atlas_texture();
					const Rectangle src{ r.u0 * static_cast<float>(tex.width),
						r.v0 * static_cast<float>(tex.height),
						(r.u1 - r.u0) * static_cast<float>(tex.width),
						(r.v1 - r.v0) * static_cast<float>(tex.height) };
					DrawTexturePro(tex, src, bounds, Vector2{ 0, 0 }, 0.0f,
							Color{ w.fill_r, w.fill_g, w.fill_b, w.fill_a });
				} else {
					DrawRectangle(static_cast<int>(w.x), static_cast<int>(w.y),
							static_cast<int>(w.w), static_cast<int>(w.h),
							fallback_color_for(static_cast<std::uint32_t>(w.item)));
				}
				break;
			}
			case script::WidgetType::kSprite: {
				if (const Texture2D *tex = sprite_texture(w.texture)) {
					const Rectangle src{ w.src_x, w.src_y,
						w.src_w > 0.0f ? w.src_w : static_cast<float>(tex->width),
						w.src_h > 0.0f ? w.src_h : static_cast<float>(tex->height) };
					DrawTexturePro(*tex, src, bounds, Vector2{ 0, 0 }, 0.0f,
							Color{ w.fill_r, w.fill_g, w.fill_b, w.fill_a });
				}
				break;
			}
			case script::WidgetType::kList: {
				std::vector<const char *> items;
				items.reserve(w.items.size());
				for (const auto &item : w.items) {
					items.push_back(item.c_str());
				}
				auto active_it = list_active_.find(w.id);
				if (active_it == list_active_.end()) {
					active_it = list_active_.emplace(w.id, w.list_index).first;
				}
				int &scroll = list_scroll_[w.id];
				int focus = -1;
				const int before = active_it->second;
				GuiListViewEx(bounds, items.data(), static_cast<int>(items.size()),
						&scroll, &active_it->second, &focus);
				if (active_it->second != before) {
					result.changed_list.emplace_back(w.id, active_it->second);
				}
				break;
			}
		}
	}

	return result;
}

} // namespace vb::render
