#include "vb/render/main_menu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <raygui.h>
#include <raylib.h>

#include "vb/core/server_address.hpp"
#include "vb/render/resources.hpp"

namespace vb::render {

namespace {

// Splits `text` into lines no wider than `width` pixels in raygui's current font (breaking at
// spaces; a single over-long word gets a line of its own).
std::vector<std::string> wrap_gui_text(std::string_view text, float width) {
	const Font font = GuiGetFont();
	const float size = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE));
	const float spacing = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SPACING));
	std::vector<std::string> lines;
	std::string line;
	std::size_t i = 0;
	while (i < text.size()) {
		const std::size_t end = std::min(text.find(' ', i), text.size());
		const std::string word(text.substr(i, end - i));
		const std::string candidate = line.empty() ? word : line + ' ' + word;
		if (!line.empty() && MeasureTextEx(font, candidate.c_str(), size, spacing).x > width) {
			lines.push_back(line);
			line = word;
		} else {
			line = candidate;
		}
		i = end + 1;
	}
	if (!line.empty()) {
		lines.push_back(line);
	}
	return lines;
}

// The shared dark rounded panel (#140E0A at ~85%, 1 px 14%-white border) with an optional title in
// its 24 px header band (content layouts below it assume that band, as GuiPanel's was).
void draw_panel(Rectangle r, const char *title) {
	DrawRectangleRounded(r, 0.04f, 8, Color{ 0x14, 0x0E, 0x0A, 217 });
	DrawRectangleRoundedLinesEx(r, 0.04f, 8, 1.0f, Color{ 255, 255, 255, 36 });
	if (title != nullptr && title[0] != '\0') {
		GuiLabel(Rectangle{ r.x + 12.0f, r.y + 2.0f, r.width - 24.0f, 22.0f }, title);
	}
}

// raygui draws a slider's text to the *left* of the bar in the bar's (dark) colour; on our dark
// panels that overlaps the neighbouring column and is unreadable, so draw the label and value
// ourselves: label column, bar, value column.
bool labeled_slider(float x, float y, float w, const char *label, const char *value_text, float *value,
		float min, float max) {
	const float label_w = 120.0f;
	const float value_w = 70.0f;
	GuiLabel(Rectangle{ x, y, label_w, 20.0f }, label);
	const float before = *value;
	GuiSlider(Rectangle{ x + label_w, y, w - label_w - value_w, 20.0f }, nullptr, nullptr, value, min, max);
	GuiLabel(Rectangle{ x + w - value_w + 8.0f, y, value_w - 8.0f, 20.0f }, value_text);
	return *value != before;
}

constexpr int kTextBufSize = 128;

// GuiTextBox needs a fixed-capacity, nul-terminated buffer; `s` is resized to
// kTextBufSize before the call and trimmed back to its real length after,
// mirroring UiRenderer::draw's kTextBox case.
bool text_box(Rectangle bounds, std::string &s, bool &edit) {
	s.resize(kTextBufSize, '\0');
	const bool toggled = GuiTextBox(bounds, s.data(), kTextBufSize, edit) != 0;
	if (toggled) {
		edit = !edit;
	}
	s.resize(std::strlen(s.c_str()));
	return toggled;
}

Rectangle centered(float w, float h, float y) {
	const float x = (static_cast<float>(GetScreenWidth()) - w) * 0.5f;
	return Rectangle{ x, y, w, h };
}

constexpr const char *kKeybindLabels[6] = { "Move forward", "Move back",
	"Move left", "Move right", "Jump", "Sprint" };

// Display-only lookup for the keybindings screen -- covers every key a
// player would plausibly rebind movement to (letters, digits, space, arrows,
// modifiers). Anything outside this set still binds and works correctly
// (the raw keycode round-trips through config just fine); it just prints as
// "Key <n>" rather than a friendly name.
std::string key_display_name(int key) {
	if (key >= KEY_A && key <= KEY_Z) {
		return std::string(1, static_cast<char>(key));
	}
	if (key >= KEY_ZERO && key <= KEY_NINE) {
		return std::string(1, static_cast<char>(key));
	}
	switch (key) {
		case KEY_SPACE:
			return "Space";
		case KEY_ENTER:
			return "Enter";
		case KEY_TAB:
			return "Tab";
		case KEY_ESCAPE:
			return "Escape";
		case KEY_UP:
			return "Up";
		case KEY_DOWN:
			return "Down";
		case KEY_LEFT:
			return "Left";
		case KEY_RIGHT:
			return "Right";
		case KEY_LEFT_SHIFT:
			return "Left Shift";
		case KEY_RIGHT_SHIFT:
			return "Right Shift";
		case KEY_LEFT_CONTROL:
			return "Left Ctrl";
		case KEY_RIGHT_CONTROL:
			return "Right Ctrl";
		case KEY_LEFT_ALT:
			return "Left Alt";
		case KEY_RIGHT_ALT:
			return "Right Alt";
		case KEY_CAPS_LOCK:
			return "Caps Lock";
		default:
			break;
	}
	return "Key " + std::to_string(key);
}

} // namespace

struct MainMenu::Assets {
	Texture2D backdrop{};
	Font ui_font{};
	Font title_font{};
	bool loaded = false;

	Assets() = default;
	Assets(const Assets &) = delete;
	Assets &operator=(const Assets &) = delete;
	~Assets() {
		// Skip the GL calls if the window is already gone (member destruction order).
		if (loaded && IsWindowReady()) {
			UnloadTexture(backdrop);
			UnloadFont(ui_font);
			UnloadFont(title_font);
		}
	}
};

MainMenu::MainMenu(const core::ClientConfig &config) :
		assets_(std::make_unique<Assets>()),
		address_("127.0.0.1"),
		address_text_("127.0.0.1") {
}

MainMenu::~MainMenu() = default;
MainMenu::MainMenu(MainMenu &&) noexcept = default;
MainMenu &MainMenu::operator=(MainMenu &&) noexcept = default;

void MainMenu::ensure_assets() {
	if (assets_->loaded || !IsWindowReady()) {
		return;
	}
	assets_->loaded = true;
	const auto dirt = resources::find("dirt.png");
	if (dirt.data != nullptr) {
		Image img = LoadImageFromMemory(".png", dirt.data, static_cast<int>(dirt.size));
		assets_->backdrop = LoadTextureFromImage(img);
		UnloadImage(img);
		SetTextureFilter(assets_->backdrop, TEXTURE_FILTER_POINT);
		SetTextureWrap(assets_->backdrop, TEXTURE_WRAP_REPEAT);
	}
	const auto load_font = [](const char *file, const char *ext, int size) {
		const auto blob = resources::find(file);
		if (blob.data == nullptr) {
			return GetFontDefault();
		}
		Font f = LoadFontFromMemory(ext, blob.data, static_cast<int>(blob.size), size, nullptr, 0);
		if (f.texture.id == 0) {
			return GetFontDefault();
		}
		SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
		return f;
	};
	assets_->ui_font = load_font("Inter-Regular.otf", ".otf", 32);
	assets_->title_font = load_font("PixelifySans-Regular.ttf", ".ttf", 64);
	GuiSetFont(assets_->ui_font);
	GuiSetStyle(DEFAULT, TEXT_SIZE, 15);
	// Labels sit directly on the dark panels, so they need light text.
	GuiSetStyle(LABEL, TEXT_COLOR_NORMAL, static_cast<int>(0xEDE6DCFFu));
	GuiSetStyle(LABEL, TEXT_COLOR_FOCUSED, static_cast<int>(0x8FC8F0FFu));
	// Text and value boxes sit on the dark panels: light text, a dark amber-bordered field while
	// editing instead of raygui's bright cyan.
	for (const int control : { static_cast<int>(TEXTBOX), static_cast<int>(VALUEBOX) }) {
		GuiSetStyle(control, TEXT_COLOR_NORMAL, static_cast<int>(0xEDE6DCFFu));
		GuiSetStyle(control, TEXT_COLOR_FOCUSED, static_cast<int>(0xFFFFFFFFu));
		GuiSetStyle(control, TEXT_COLOR_PRESSED, static_cast<int>(0xFFFFFFFFu));
		GuiSetStyle(control, BASE_COLOR_NORMAL, static_cast<int>(0x1E1610FFu));
		GuiSetStyle(control, BASE_COLOR_FOCUSED, static_cast<int>(0x2A2018FFu));
		GuiSetStyle(control, BASE_COLOR_PRESSED, static_cast<int>(0x2A2018FFu));
		GuiSetStyle(control, BORDER_COLOR_PRESSED, static_cast<int>(0xE0B060FFu));
	}
}

void MainMenu::draw_backdrop() {
	ensure_assets();
	const float w = static_cast<float>(GetScreenWidth());
	const float h = static_cast<float>(GetScreenHeight());
	if (assets_->backdrop.id != 0) {
		// The 16x16 texture drawn 4x, tiled across the window.
		const float tile = static_cast<float>(assets_->backdrop.width) * 4.0f;
		DrawTexturePro(assets_->backdrop, Rectangle{ 0.0f, 0.0f, w / tile * static_cast<float>(assets_->backdrop.width), h / tile * static_cast<float>(assets_->backdrop.height) },
				Rectangle{ 0.0f, 0.0f, w, h }, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
	}
	DrawRectangle(0, 0, static_cast<int>(w), static_cast<int>(h), Color{ 0, 0, 0, 140 }); // 55% black
}

void MainMenu::prefill(std::string_view address, int port) {
	address_ = std::string(address);
	port_ = port;
	address_text_ = core::format_server_address(address_, port_);
	address_error_.clear();
}

MainMenu::MainResult MainMenu::draw_main(const std::vector<std::string> &recent_servers) {
	draw_backdrop();
	MainResult result;

	const float screen_w = static_cast<float>(GetScreenWidth());
	const float screen_h = static_cast<float>(GetScreenHeight());
	const float field_w = std::min(520.0f, screen_w - 32.0f);
	const float button_w = 110.0f;
	const float row_h = 26.0f;
	const std::size_t shown = std::min<std::size_t>(recent_servers.size(), 3);

	// Stack: title, address row, error line, recent panel. Centred vertically so it fits the
	// 720x360 minimum window.
	const float title_size = 56.0f;
	const float title_h = title_size;
	const float panel_h = shown == 0 ? 0.0f : 24.0f + row_h * static_cast<float>(shown) + 30.0f;
	const float stack_h = title_h + 20.0f + 40.0f + 22.0f + panel_h;
	// Leave the bottom 60 px to the Settings button so the two never overlap at the minimum height.
	const float top = std::max(12.0f, (screen_h - 60.0f - stack_h) * 0.5f);
	const float x = (screen_w - field_w) * 0.5f;

	const char *title = "Voxel Browser";
	const Vector2 tsize = MeasureTextEx(assets_->title_font, title, title_size, 3.0f);
	DrawTextEx(assets_->title_font, title, Vector2{ (screen_w - tsize.x) * 0.5f, top }, title_size, 3.0f,
			Color{ 0xF4, 0xEC, 0xE0, 255 });

	float y = top + title_h + 20.0f;
	const float input_w = field_w - button_w - 10.0f;
	const bool toggled = text_box(Rectangle{ x, y, input_w, 40.0f }, address_text_, address_edit_);
	const bool enter = toggled && !address_edit_ && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER));
	if (enter) {
		address_edit_ = true; // keep the caret so a failed parse can be fixed right away
	}
	const bool clicked = GuiButton(Rectangle{ x + input_w + 10.0f, y, button_w, 40.0f }, "Continue");
	if (enter || clicked) {
		std::string why;
		if (const auto parsed = core::parse_server_address(address_text_, &why)) {
			address_ = parsed->host;
			port_ = parsed->port;
			address_error_.clear();
			result.connect = true;
		} else {
			address_error_ = why;
		}
	}
	y += 40.0f + 4.0f;
	if (!address_error_.empty()) {
		DrawTextEx(assets_->ui_font, address_error_.c_str(), Vector2{ x + 2.0f, y }, 14.0f, 0.0f,
				Color{ 0xFF, 0x8A, 0x7A, 255 });
	}
	y += 18.0f;

	if (shown != 0) {
		draw_panel(Rectangle{ x, y, field_w, panel_h }, "Recent servers");
		float ry = y + 26.0f;
		for (std::size_t i = 0; i < shown; ++i) {
			if (GuiLabelButton(Rectangle{ x + 12.0f, ry, field_w - 24.0f, row_h }, recent_servers[i].c_str())) {
				result.open_history = true;
				result.history_index = static_cast<int>(i);
			}
			ry += row_h;
		}
		if (GuiLabelButton(Rectangle{ x + 12.0f, ry + 2.0f, field_w - 24.0f, 24.0f }, "All history in Settings")) {
			result.open_history = true;
			result.history_index = -1;
		}
	}

	if (GuiButton(Rectangle{ 16.0f, screen_h - 16.0f - 34.0f, 110.0f, 34.0f }, "Settings")) {
		result.open_settings = true;
	}

	if (!signed_in_label_.empty()) {
		// Top-right normally; bottom-right when the title would overlap it (the 360 px minimum).
		const float w = 260.0f;
		const float sx = std::max(8.0f, screen_w - w - 12.0f);
		const float sy = screen_h < 480.0f ? screen_h - 12.0f - 58.0f : 10.0f;
		GuiLabel(Rectangle{ sx, sy, w, 20.0f }, ("Signed in to " + signed_in_label_).c_str());
		if (GuiButton(Rectangle{ sx, sy + 22.0f, 100.0f, 24.0f }, "Sign out")) {
			result.sign_out = true;
		}
	}

	return result;
}

void MainMenu::open_settings(const core::ClientConfig &config, SettingsSection section, int highlight) {
	s_width_ = static_cast<int>(config.window_width);
	s_height_ = static_cast<int>(config.window_height);
	s_vsync_ = config.vsync;
	s_fov_ = static_cast<float>(config.fov);
	s_render_distance_ = static_cast<float>(config.render_distance);
	s_sensitivity_ = static_cast<float>(config.mouse_sensitivity);
	s_cache_mb_ = static_cast<int>(config.asset_cache_mb);
	s_name_ = config.player_name;
	s_name_edit_ = false;
	settings_section_ = section;
	history_highlight_ = highlight;
	// Put the highlighted entry a row or two down from the top so it is visibly in view.
	history_scroll_ = std::max(0, highlight - 2);
}

MainMenu::SettingsResult MainMenu::draw_settings(core::ClientConfig &config) {
	draw_backdrop();
	SettingsResult result;

	const float screen_w = static_cast<float>(GetScreenWidth());
	const float screen_h = static_cast<float>(GetScreenHeight());
	const float panel_w = std::min(600.0f, screen_w - 24.0f);
	const float panel_h = std::min(420.0f, screen_h - 24.0f);
	const float px = (screen_w - panel_w) * 0.5f;
	const float py = (screen_h - panel_h) * 0.5f;
	draw_panel(Rectangle{ px, py, panel_w, panel_h }, "Settings");

	// Section list on the left.
	const float nav_w = 120.0f;
	const char *names[4] = { "General", "Video", "Controls", "History" };
	const SettingsSection sections[4] = { SettingsSection::kGeneral, SettingsSection::kVideo,
		SettingsSection::kControls, SettingsSection::kHistory };
	float ny = py + 34.0f;
	for (int i = 0; i < 4; ++i) {
		const bool active = settings_section_ == sections[i];
		if (active) {
			DrawRectangleRounded(Rectangle{ px + 10.0f, ny, nav_w, 30.0f }, 0.2f, 6, Color{ 255, 255, 255, 40 });
		}
		if (GuiLabelButton(Rectangle{ px + 18.0f, ny, nav_w - 16.0f, 30.0f }, names[i])) {
			settings_section_ = sections[i];
		}
		ny += 34.0f;
	}

	// Content area to the right of the section list, above the footer.
	const float cx = px + nav_w + 30.0f;
	const float cw = panel_w - nav_w - 50.0f;
	const float footer_h = 44.0f;
	const float content_top = py + 34.0f;
	const float content_bottom = py + panel_h - footer_h;
	float y = content_top;

	const bool name_ok = !s_name_.empty() && s_name_.size() <= core::kMaxPlayerNameLength;

	switch (settings_section_) {
		case SettingsSection::kGeneral: {
			GuiLabel(Rectangle{ cx, y, cw, 22.0f }, "Default player name");
			y += 24.0f;
			text_box(Rectangle{ cx, y, cw, 30.0f }, s_name_, s_name_edit_);
			y += 34.0f;
			if (!name_ok) {
				DrawTextEx(assets_->ui_font,
						s_name_.empty() ? "Enter a name" : "Name must be at most 32 characters",
						Vector2{ cx + 2.0f, y }, 14.0f, 0.0f, Color{ 0xFF, 0x8A, 0x7A, 255 });
			}
			y += 26.0f;
			GuiLabel(Rectangle{ cx, y, cw * 0.55f, 24.0f }, "Asset cache (MB)");
			if (GuiValueBox(Rectangle{ cx + cw * 0.6f, y, cw * 0.4f, 24.0f }, nullptr, &s_cache_mb_, 64, 8192,
						s_cache_edit_) != 0) {
				s_cache_edit_ = !s_cache_edit_;
			}
			break;
		}
		case SettingsSection::kVideo: {
			GuiLabel(Rectangle{ cx, y, cw * 0.5f, 24.0f }, "Window size (on restart)");
			if (GuiValueBox(Rectangle{ cx + cw * 0.55f, y, cw * 0.2f, 24.0f }, nullptr, &s_width_, 640, 3840,
						s_width_edit_) != 0) {
				s_width_edit_ = !s_width_edit_;
			}
			if (GuiValueBox(Rectangle{ cx + cw * 0.78f, y, cw * 0.2f, 24.0f }, nullptr, &s_height_, 480, 2160,
						s_height_edit_) != 0) {
				s_height_edit_ = !s_height_edit_;
			}
			y += 34.0f;
			GuiCheckBox(Rectangle{ cx, y, 20.0f, 20.0f }, "V-Sync (applies on restart)", &s_vsync_);
			y += 34.0f;
			char fov_label[32];
			std::snprintf(fov_label, sizeof(fov_label), "%.0f", static_cast<double>(s_fov_));
			labeled_slider(cx, y, cw, "FOV", fov_label, &s_fov_, 50.0f, 110.0f);
			y += 34.0f;
			char rd_label[32];
			std::snprintf(rd_label, sizeof(rd_label), "%.0f chunks", static_cast<double>(s_render_distance_));
			labeled_slider(cx, y, cw, "Render distance", rd_label, &s_render_distance_, 2.0f, 32.0f);
			break;
		}
		case SettingsSection::kControls: {
			char sens_label[32];
			std::snprintf(sens_label, sizeof(sens_label), "%.2f", static_cast<double>(s_sensitivity_));
			labeled_slider(cx, y + 4.0f, cw, "Sensitivity", sens_label, &s_sensitivity_, 0.02f, 0.6f);
			y += 44.0f;
			if (GuiButton(Rectangle{ cx, y, 160.0f, 32.0f }, "Keybindings...")) {
				result.open_keybindings = true;
			}
			break;
		}
		case SettingsSection::kHistory: {
			const float row_h = 30.0f;
			const std::size_t total = config.recent_servers.size();
			const float list_bottom = content_bottom - 34.0f; // Clear history sits underneath
			const int visible = std::max(1, static_cast<int>((list_bottom - content_top) / row_h));
			const Rectangle list_rect{ cx, content_top, cw, list_bottom - content_top };
			if (total == 0) {
				GuiLabel(Rectangle{ cx, content_top, cw, 24.0f }, "No servers yet.");
			}
			if (CheckCollisionPointRec(GetMousePosition(), list_rect)) {
				history_scroll_ -= static_cast<int>(GetMouseWheelMove());
			}
			const int max_scroll = std::max(0, static_cast<int>(total) - visible);
			history_scroll_ = std::clamp(history_scroll_, 0, max_scroll);
			const float btn_w = 78.0f;
			float ry = content_top;
			int remove_index = -1;
			for (int i = history_scroll_; i < static_cast<int>(total) && i < history_scroll_ + visible; ++i) {
				if (i == history_highlight_) {
					DrawRectangleRounded(Rectangle{ cx - 4.0f, ry, cw + 8.0f, row_h - 2.0f }, 0.2f, 6,
							Color{ 0xE0, 0xB0, 0x60, 60 });
				}
				const std::string &entry = config.recent_servers[static_cast<std::size_t>(i)];
				GuiLabel(Rectangle{ cx + 4.0f, ry, cw - 2.0f * btn_w - 16.0f, row_h - 2.0f }, entry.c_str());
				if (GuiButton(Rectangle{ cx + cw - 2.0f * btn_w - 6.0f, ry + 2.0f, btn_w, row_h - 6.0f }, "Connect")) {
					result.connect_history = i;
				}
				if (GuiButton(Rectangle{ cx + cw - btn_w, ry + 2.0f, btn_w, row_h - 6.0f }, "Remove")) {
					remove_index = i;
				}
				ry += row_h;
			}
			if (remove_index >= 0) {
				config.recent_servers.erase(config.recent_servers.begin() + remove_index);
				history_highlight_ = -1;
				result.history_changed = true;
			}
			if (total != 0 && GuiButton(Rectangle{ cx, content_bottom - 30.0f, 140.0f, 28.0f }, "Clear history")) {
				config.recent_servers.clear();
				history_highlight_ = -1;
				result.history_changed = true;
			}
			break;
		}
	}

	// Footer.
	const float fy = py + panel_h - footer_h + 6.0f;
	if (GuiButton(Rectangle{ px + panel_w - 2.0f * 110.0f - 22.0f, fy, 110.0f, 32.0f }, "Back")) {
		result.back = true;
	}
	if (!name_ok) {
		GuiDisable();
	}
	const bool save_clicked = GuiButton(Rectangle{ px + panel_w - 110.0f - 12.0f, fy, 110.0f, 32.0f }, "Save");
	GuiEnable();
	if (save_clicked && name_ok) {
		config.window_width = static_cast<std::uint32_t>(s_width_);
		config.window_height = static_cast<std::uint32_t>(s_height_);
		config.vsync = s_vsync_;
		config.fov = static_cast<double>(s_fov_);
		config.render_distance = static_cast<std::uint32_t>(s_render_distance_);
		config.mouse_sensitivity = static_cast<double>(s_sensitivity_);
		config.asset_cache_mb = static_cast<std::uint32_t>(s_cache_mb_);
		config.player_name = s_name_;
		result.save = true;
	}

	return result;
}

void MainMenu::open_keybindings(const core::ClientConfig &config) {
	k_keys_[0] = config.key_forward;
	k_keys_[1] = config.key_back;
	k_keys_[2] = config.key_left;
	k_keys_[3] = config.key_right;
	k_keys_[4] = config.key_jump;
	k_keys_[5] = config.key_sprint;
	k_rebinding_ = -1;
}

MainMenu::KeybindingsResult MainMenu::draw_keybindings(core::ClientConfig &config,
		const InputFrame &input) {
	draw_backdrop();
	KeybindingsResult result;

	const float panel_w = 420.0f;
	draw_panel(centered(panel_w, 380.0f, 80.0f), "Keybindings");

	float y = 130.0f;
	const float x = (static_cast<float>(GetScreenWidth()) - panel_w) * 0.5f + 20.0f;
	const float field_w = panel_w - 40.0f;

	if (k_rebinding_ >= 0) {
		const int pressed = input.key_pressed_first;
		if (pressed == KEY_ESCAPE) {
			k_rebinding_ = -1;
		} else if (pressed != 0) {
			k_keys_[k_rebinding_] = pressed;
			k_rebinding_ = -1;
		}
	}

	for (int i = 0; i < 6; ++i) {
		GuiLabel(Rectangle{ x, y, field_w * 0.45f, 28.0f }, kKeybindLabels[i]);
		const std::string label = (k_rebinding_ == i)
				? "Press any key... (Esc to cancel)"
				: key_display_name(k_keys_[i]);
		if (GuiButton(Rectangle{ x + field_w * 0.48f, y, field_w * 0.52f, 28.0f },
					label.c_str()) &&
				k_rebinding_ < 0) {
			k_rebinding_ = i;
		}
		y += 34.0f;
	}
	y += 10.0f;

	if (GuiButton(Rectangle{ x, y, field_w * 0.48f, 32.0f }, "Save")) {
		config.key_forward = k_keys_[0];
		config.key_back = k_keys_[1];
		config.key_left = k_keys_[2];
		config.key_right = k_keys_[3];
		config.key_jump = k_keys_[4];
		config.key_sprint = k_keys_[5];
		result.save = true;
	}
	if (GuiButton(Rectangle{ x + field_w * 0.52f, y, field_w * 0.48f, 32.0f }, "Back")) {
		result.back = true;
	}

	return result;
}

MainMenu::ConnectingResult MainMenu::draw_connecting(std::string_view status_text,
		float fraction) {
	draw_backdrop();
	ConnectingResult result;
	// A known fraction (asset-sync byte progress) grows the panel by one row
	// for a real GuiProgressBar; unknown (-1, every other handshake stage)
	// keeps the original text-only layout exactly as before this gap closed.
	const bool has_fraction = fraction >= 0.0f;
	const float panel_w = 420.0f;
	const float panel_h = has_fraction ? 178.0f : 140.0f;
	// Vertically centered on actual screen height, not a hardcoded y: a
	// fixed y=220 (its value before this fix) put this panel's bottom edge
	// at 220+178=398px, past the bottom of the engine's own enforced
	// minimum window height (360px, kMinWindowHeight) whenever asset-sync
	// progress grew the panel by its extra row -- the Cancel button sat
	// clipped off-screen. Clamped so it can never go negative either, same
	// posture as `centered()`'s x.
	const float panel_y = std::max(20.0f,
			(static_cast<float>(GetScreenHeight()) - panel_h) * 0.5f);
	draw_panel(centered(panel_w, panel_h, panel_y), "Connecting");
	const float x = (static_cast<float>(GetScreenWidth()) - panel_w) * 0.5f + 20.0f;
	GuiLabel(Rectangle{ x, panel_y + 50.0f, panel_w - 40.0f, 24.0f },
			std::string(status_text).c_str());
	float button_y = panel_y + 90.0f;
	if (has_fraction) {
		float value = fraction < 1.0f ? fraction : 1.0f;
		GuiProgressBar(Rectangle{ x, panel_y + 84.0f, panel_w - 40.0f, 20.0f }, nullptr,
				nullptr, &value, 0.0f, 1.0f);
		button_y = panel_y + 116.0f;
	}
	if (GuiButton(Rectangle{ x, button_y, panel_w - 40.0f, 32.0f }, "Cancel")) {
		result.cancel = true;
	}
	return result;
}

namespace {

void append_utf8(std::string &s, int cp) {
	if (cp < 0x80) {
		s.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

// Masked single-line field (raygui's GuiTextBox cannot hide its text). Returns
// true when Enter is pressed while focused.
bool password_box(Rectangle bounds, std::string &value, bool &focused) {
	const bool hovered = CheckCollisionPointRec(GetMousePosition(), bounds);
	if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		focused = hovered;
	}
	bool enter = false;
	if (focused) {
		for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed()) {
			if (cp >= 32 && cp != 127 && value.size() < 120) {
				append_utf8(value, cp);
			}
		}
		if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
			while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80) {
				value.pop_back();
			}
			if (!value.empty()) {
				value.pop_back();
			}
		}
		enter = IsKeyPressed(KEY_ENTER);
	}
	DrawRectangleRec(bounds, Color{ 30, 30, 36, 255 });
	DrawRectangleLinesEx(bounds, 1.0f, focused ? Color{ 120, 170, 255, 255 } : Color{ 90, 90, 100, 255 });
	std::string dots;
	for (const char ch : value) {
		if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) {
			dots.push_back('*');
		}
	}
	DrawText(dots.c_str(), static_cast<int>(bounds.x) + 8, static_cast<int>(bounds.y) + 6, 18, RAYWHITE);
	return enter;
}

} // namespace

MainMenu::SigningInResult MainMenu::draw_signing_in(const SigningInView &view) {
	draw_backdrop();
	SigningInResult result;
	const float panel_w = 460.0f;
	float panel_h = 150.0f;
	if (view.needs_trust) {
		panel_h = 190.0f;
	}
	if (view.offer_password) {
		panel_h += 110.0f;
	}
	if (view.offer_browser) {
		panel_h += 44.0f;
	}
	if (!view.error.empty()) {
		panel_h += 26.0f;
	}
	const float panel_y = std::max(20.0f,
			(static_cast<float>(GetScreenHeight()) - panel_h) * 0.5f);
	const std::string title = view.title.empty() ? std::string("Sign in")
												 : "Sign in - " + std::string(view.title); // ASCII: raylib's default font has no em dash
	draw_panel(centered(panel_w, panel_h, panel_y), title.c_str());
	const float x = (static_cast<float>(GetScreenWidth()) - panel_w) * 0.5f + 20.0f;
	const float w = panel_w - 40.0f;
	float y = panel_y + 40.0f;
	const std::string who = std::string(view.server) + " wants you to sign in with " +
			std::string(view.provider_host);
	GuiLabel(Rectangle{ x, y, w, 24.0f }, who.c_str());
	y += 30.0f;

	if (view.needs_trust) {
		GuiLabel(Rectangle{ x, y, w, 40.0f },
				"Only continue if you trust this server with\nyour sign-in. It will be remembered.");
		if (GuiButton(Rectangle{ x, panel_y + panel_h - 84.0f, w, 32.0f }, "Continue")) {
			result.trust = true;
		}
		if (GuiButton(Rectangle{ x, panel_y + panel_h - 44.0f, w, 32.0f }, "Cancel")) {
			result.cancel = true;
		}
		return result;
	}

	const bool enabled = !view.working;
	if (!enabled) {
		GuiLabel(Rectangle{ x, y, w, 24.0f },
				view.offer_browser ? "Finish signing in in your web browser..." : "Signing in...");
		y += 30.0f;
	} else {
		if (view.offer_browser) {
			const std::string label(view.browser_label);
			if (GuiButton(Rectangle{ x, y, w, 32.0f }, label.c_str())) {
				result.browser = true;
			}
			y += 44.0f;
		}
		if (view.offer_password) {
			GuiLabel(Rectangle{ x, y, w, 20.0f }, "E-mail");
			text_box(Rectangle{ x, y + 20.0f, w, 28.0f }, si_email_, si_email_edit_);
			GuiLabel(Rectangle{ x, y + 52.0f, w, 20.0f }, "Password");
			const bool enter = password_box(Rectangle{ x, y + 72.0f, w, 28.0f }, si_password_,
					si_password_edit_);
			y += 110.0f;
			if (GuiButton(Rectangle{ x, y - 8.0f, w, 32.0f }, "Sign in") || enter) {
				result.submit_password = true;
				result.email = si_email_;
				result.password = si_password_;
				// The password does not outlive this call.
				std::fill(si_password_.begin(), si_password_.end(), '\0');
				si_password_.clear();
			}
			y += 36.0f;
		}
	}
	if (!view.error.empty()) {
		GuiLabel(Rectangle{ x, y, w, 24.0f }, std::string(view.error).c_str());
		y += 26.0f;
	}
	if (GuiButton(Rectangle{ x, panel_y + panel_h - 44.0f, w, 32.0f }, "Cancel")) {
		result.cancel = true;
		std::fill(si_password_.begin(), si_password_.end(), '\0');
		si_password_.clear();
	}
	return result;
}

MainMenu::ErrorResult MainMenu::draw_error(std::string_view reason, std::string_view title) {
	draw_backdrop();
	ErrorResult result;
	const float panel_w = 460.0f;
	// Long reasons (a pack's engine_version_req, auth errors) wrap instead of running off the panel.
	const float line_h = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE)) + 6.0f;
	std::vector<std::string> lines = wrap_gui_text(reason, panel_w - 40.0f);
	if (lines.size() > 8) {
		lines.resize(8);
		lines.back() += " ...";
	}
	const float text_h = std::max(48.0f, line_h * static_cast<float>(lines.size()));
	const float panel_h = 112.0f + text_h;
	// Same vertical-centering fix as draw_connecting() above, and for the
	// same reason: a hardcoded y=220 put this panel's bottom (220+160=380)
	// past the 360px minimum window height too.
	const float panel_y = std::max(20.0f,
			(static_cast<float>(GetScreenHeight()) - panel_h) * 0.5f);
	draw_panel(centered(panel_w, panel_h, panel_y), std::string(title).c_str());
	const float x = (static_cast<float>(GetScreenWidth()) - panel_w) * 0.5f + 20.0f;
	for (std::size_t i = 0; i < lines.size(); ++i) {
		GuiLabel(Rectangle{ x, panel_y + 50.0f + line_h * static_cast<float>(i), panel_w - 40.0f, line_h },
				lines[i].c_str());
	}
	if (GuiButton(Rectangle{ x, panel_y + 62.0f + text_h, panel_w - 40.0f, 32.0f }, "Back to menu")) {
		result.back = true;
	}
	return result;
}

void MainMenu::draw_loading(float fraction, std::string_view operator_title) {
	draw_backdrop();
	// Stage 1: a fully generic bar, no data dependency -- drawn every call
	// regardless of whether stage 2's operator_title has arrived yet, so it
	// never waits on anything network-derived.
	if (fraction < 0.0f) {
		fraction = 0.0f;
	} else if (fraction > 1.0f) {
		fraction = 1.0f;
	}
	const float panel_w = 460.0f;
	const float screen_h = static_cast<float>(GetScreenHeight());
	const float bar_y = screen_h * 0.5f;
	const float x = (static_cast<float>(GetScreenWidth()) - panel_w) * 0.5f;

	// Stage 2: operator branding (server.toml's `motd`), drawn above the bar
	// only once the handshake has actually delivered it -- text only, no
	// color knob exists yet (see REMAINING_TASKS.md Phase 7.1).
	if (!operator_title.empty()) {
		const int font_size = 28;
		const int text_w = MeasureText(std::string(operator_title).c_str(), font_size);
		DrawText(std::string(operator_title).c_str(),
				(GetScreenWidth() - text_w) / 2, static_cast<int>(bar_y) - 60,
				font_size, RAYWHITE);
	}

	float value = fraction;
	GuiProgressBar(Rectangle{ x, bar_y, panel_w, 28.0f }, nullptr, nullptr,
			&value, 0.0f, 1.0f);

	char pct[16];
	std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(fraction * 100.0f + 0.5f));
	const int pct_w = MeasureText(pct, 18);
	DrawText(pct, (GetScreenWidth() - pct_w) / 2, static_cast<int>(bar_y) + 36, 18,
			Color{ 200, 200, 210, 230 });
}

} // namespace vb::render
