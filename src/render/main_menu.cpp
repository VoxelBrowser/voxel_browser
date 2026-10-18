#include "vb/render/main_menu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <raygui.h>
#include <raylib.h>

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

MainMenu::MainMenu(const core::ClientConfig &config) :
		address_("127.0.0.1"), port_text_("27015"), player_name_(config.player_name) {
}

void MainMenu::prefill(std::string_view address, int port, std::string_view player_name) {
	address_ = std::string(address);
	port_ = port;
	player_name_ = std::string(player_name);
}

MainMenu::MainResult MainMenu::draw_main(const std::vector<std::string> &recent_servers) {
	MainResult result;

	const float panel_w = 420.0f;
	GuiPanel(centered(panel_w, 420.0f, 80.0f), "voxel_browser");

	float y = 130.0f;
	const float x = (static_cast<float>(GetScreenWidth()) - panel_w) * 0.5f + 20.0f;
	const float field_w = panel_w - 40.0f;

	GuiLabel(Rectangle{ x, y, field_w, 20.0f }, "Player name");
	y += 22.0f;
	text_box(Rectangle{ x, y, field_w, 28.0f }, player_name_, name_edit_);
	y += 38.0f;

	GuiLabel(Rectangle{ x, y, field_w, 20.0f }, "Server address");
	y += 22.0f;
	text_box(Rectangle{ x, y, field_w * 0.65f, 28.0f }, address_, address_edit_);
	if (GuiValueBox(Rectangle{ x + field_w * 0.68f, y, field_w * 0.32f, 28.0f },
				nullptr, &port_, 1, 65535, port_edit_) != 0) {
		port_edit_ = !port_edit_;
	}
	y += 40.0f;

	if (GuiButton(Rectangle{ x, y, field_w, 32.0f }, "Connect")) {
		result.connect = true;
	}
	y += 40.0f;
	if (GuiButton(Rectangle{ x, y, field_w, 32.0f }, "Play Singleplayer")) {
		result.singleplayer = true;
	}
	y += 40.0f;

	if (!recent_servers.empty()) {
		GuiLabel(Rectangle{ x, y, field_w, 20.0f }, "Recent servers");
		y += 22.0f;
		std::vector<const char *> items;
		items.reserve(recent_servers.size());
		for (const auto &s : recent_servers) {
			items.push_back(s.c_str());
		}
		const float list_h = 84.0f;
		const int before = recent_active_;
		GuiListView(Rectangle{ x, y, field_w, list_h }, nullptr, &recent_scroll_,
				&recent_active_);
		if (recent_active_ != before && recent_active_ >= 0 &&
				static_cast<std::size_t>(recent_active_) < recent_servers.size()) {
			const std::string &picked = recent_servers[static_cast<std::size_t>(recent_active_)];
			const auto colon = picked.rfind(':');
			if (colon != std::string::npos) {
				address_ = picked.substr(0, colon);
				try {
					port_ = std::stoi(picked.substr(colon + 1));
				} catch (...) {
				}
			} else {
				address_ = picked;
			}
		}
		y += list_h + 10.0f;
	}

	if (GuiButton(Rectangle{ x, y, field_w * 0.48f, 32.0f }, "Settings")) {
		result.open_settings = true;
	}
	if (GuiButton(Rectangle{ x + field_w * 0.52f, y, field_w * 0.48f, 32.0f }, "Quit")) {
		result.quit = true;
	}

	if (!signed_in_label_.empty()) {
		const float w = 260.0f;
		const float sx = std::max(8.0f, static_cast<float>(GetScreenWidth()) - w - 12.0f);
		GuiLabel(Rectangle{ sx, 10.0f, w, 20.0f }, ("Signed in to " + signed_in_label_).c_str());
		if (GuiButton(Rectangle{ sx, 32.0f, 100.0f, 24.0f }, "Sign out")) {
			result.sign_out = true;
		}
	}

	return result;
}

void MainMenu::open_settings(const core::ClientConfig &config) {
	s_width_ = static_cast<int>(config.window_width);
	s_height_ = static_cast<int>(config.window_height);
	s_vsync_ = config.vsync;
	s_fov_ = static_cast<float>(config.fov);
	s_render_distance_ = static_cast<float>(config.render_distance);
	s_sensitivity_ = static_cast<float>(config.mouse_sensitivity);
	s_cache_mb_ = static_cast<int>(config.asset_cache_mb);
}

MainMenu::SettingsResult MainMenu::draw_settings(core::ClientConfig &config) {
	SettingsResult result;

	const float panel_w = 460.0f;
	GuiPanel(centered(panel_w, 420.0f, 80.0f), "Settings");

	float y = 130.0f;
	const float x = (static_cast<float>(GetScreenWidth()) - panel_w) * 0.5f + 20.0f;
	const float field_w = panel_w - 40.0f;

	GuiLabel(Rectangle{ x, y, field_w * 0.5f, 24.0f }, "Window width (applies on restart)");
	if (GuiValueBox(Rectangle{ x + field_w * 0.55f, y, field_w * 0.2f, 24.0f }, nullptr,
				&s_width_, 640, 3840, s_width_edit_) != 0) {
		s_width_edit_ = !s_width_edit_;
	}
	if (GuiValueBox(Rectangle{ x + field_w * 0.78f, y, field_w * 0.2f, 24.0f }, nullptr,
				&s_height_, 480, 2160, s_height_edit_) != 0) {
		s_height_edit_ = !s_height_edit_;
	}
	y += 32.0f;

	GuiCheckBox(Rectangle{ x, y, 20.0f, 20.0f }, "V-Sync (applies on restart)", &s_vsync_);
	y += 34.0f;

	char fov_label[32];
	std::snprintf(fov_label, sizeof(fov_label), "%.0f", static_cast<double>(s_fov_));
	GuiSlider(Rectangle{ x, y, field_w, 20.0f }, "FOV", fov_label, &s_fov_, 50.0f, 110.0f);
	y += 32.0f;

	char rd_label[32];
	std::snprintf(rd_label, sizeof(rd_label), "%.0f chunks",
			static_cast<double>(s_render_distance_));
	GuiSlider(Rectangle{ x, y, field_w, 20.0f }, "Render distance", rd_label,
			&s_render_distance_, 2.0f, 32.0f);
	y += 32.0f;

	char sens_label[32];
	std::snprintf(sens_label, sizeof(sens_label), "%.2f", static_cast<double>(s_sensitivity_));
	GuiSlider(Rectangle{ x, y, field_w, 20.0f }, "Sensitivity", sens_label, &s_sensitivity_,
			0.02f, 0.6f);
	y += 32.0f;

	GuiLabel(Rectangle{ x, y, field_w * 0.55f, 24.0f }, "Asset cache (MB)");
	if (GuiValueBox(Rectangle{ x + field_w * 0.6f, y, field_w * 0.4f, 24.0f }, nullptr,
				&s_cache_mb_, 64, 8192, s_cache_edit_) != 0) {
		s_cache_edit_ = !s_cache_edit_;
	}
	y += 44.0f;

	if (GuiButton(Rectangle{ x, y, field_w * 0.3f, 32.0f }, "Keybindings")) {
		result.open_keybindings = true;
	}
	if (GuiButton(Rectangle{ x + field_w * 0.34f, y, field_w * 0.3f, 32.0f }, "Save")) {
		config.window_width = static_cast<std::uint32_t>(s_width_);
		config.window_height = static_cast<std::uint32_t>(s_height_);
		config.vsync = s_vsync_;
		config.fov = static_cast<double>(s_fov_);
		config.render_distance = static_cast<std::uint32_t>(s_render_distance_);
		config.mouse_sensitivity = static_cast<double>(s_sensitivity_);
		config.asset_cache_mb = static_cast<std::uint32_t>(s_cache_mb_);
		result.save = true;
	}
	if (GuiButton(Rectangle{ x + field_w * 0.68f, y, field_w * 0.32f, 32.0f }, "Back")) {
		result.back = true;
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
	KeybindingsResult result;

	const float panel_w = 420.0f;
	GuiPanel(centered(panel_w, 380.0f, 80.0f), "Keybindings");

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
	GuiPanel(centered(panel_w, panel_h, panel_y), "Connecting");
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
	GuiPanel(centered(panel_w, panel_h, panel_y), title.c_str());
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
	GuiPanel(centered(panel_w, panel_h, panel_y), std::string(title).c_str());
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
