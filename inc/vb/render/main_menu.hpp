#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vb/core/config.hpp"
#include "vb/render/input.hpp"

// Engine-level main menu / connect / settings / error screens (spec §11.6),
// drawn with raygui. Not pack content -- no Lua involved here, unlike
// vb::script::UiRuntime (Phase 4.5) which is what a *pack* uses to draw its
// own screens once joined. src/client/main.cpp owns the actual app-state
// machine (menu -> connecting -> playing / error) and which struct
// (Singleplayer / RemoteConnection) is alive; MainMenu only draws the
// non-gameplay screens and reports back which button fired this frame.

namespace vb::render {

class MainMenu {
public:
	explicit MainMenu(const core::ClientConfig &config);
	~MainMenu();
	MainMenu(MainMenu &&) noexcept;
	MainMenu &operator=(MainMenu &&) noexcept;

	// Pre-fills the connect fields (e.g. from CLI overrides) before the first
	// draw_main() call.
	void prefill(std::string_view address, int port, std::string_view player_name);
	// The parsed host / port of the address field (the last valid parse; prefill() values until
	// the player edits it).

	// --- main screen ---------------------------------------------------
	struct MainResult {
		bool connect = false;
		bool open_settings = false;
		// A recent server was clicked (or the "All history" link): open Settings > History.
		// `history_index` is the clicked entry, or -1 for the link.
		bool open_history = false;
		int history_index = -1;
		bool sign_out = false; // "Sign out" of the selected server (only shown while signed_in_label is set)
	};
	// The search-engine-style home page (Phase 12.2): title, one address field + Continue (Enter
	// submits), the first three `recent_servers` ("host:port" strings, most-recent first) and
	// Settings bottom-left. Clicking a recent entry neither connects nor fills the field.
	MainResult draw_main(const std::vector<std::string> &recent_servers);

	// Phase 9.6: shown top-right as "Signed in to <label>" with a Sign out
	// button when non-empty (e.g. "play.example.com:27015 as alice": logins
	// are per server). Empty = nothing drawn.
	void set_signed_in_label(std::string label) { signed_in_label_ = std::move(label); }

	const std::string &address() const { return address_; }
	int port() const { return port_; }
	const std::string &player_name() const { return player_name_; }

	// --- settings screen -------------------------------------------------
	// Sections down the left; Back / Save in the footer (Phase 12.3).
	enum class SettingsSection { kGeneral,
		kVideo,
		kControls,
		kHistory };
	struct SettingsResult {
		bool save = false;
		bool back = false;
		bool open_keybindings = false;
		// History > Remove / Clear history edited config.recent_servers in place; persist it.
		bool history_changed = false;
		// History > Connect on this entry (index into config.recent_servers), else -1.
		int connect_history = -1;
	};
	// `config` is read on entry (call once when opening the screen) and, on
	// `save`, filled in with the edited values for the caller to persist. `highlight` is the
	// History entry to mark (-1 none); opening on kHistory scrolls it into view.
	void open_settings(const core::ClientConfig &config,
			SettingsSection section = SettingsSection::kGeneral, int highlight = -1);
	SettingsResult draw_settings(core::ClientConfig &config);

	// --- keybindings screen (5.3) ----------------------------------------
	struct KeybindingsResult {
		bool save = false;
		bool back = false;
	};
	// Reached from Settings. `config` is read on entry the same way
	// open_settings() is; on `save`, the 6 MovementBindings-equivalent
	// key_* fields are filled in with the edited values for the caller to
	// persist. Rebinding is "click the action's key button, then press any
	// physical key" -- Esc cancels a rebind in progress without changing it.
	void open_keybindings(const core::ClientConfig &config);
	KeybindingsResult draw_keybindings(core::ClientConfig &config, const InputFrame &input);

	// --- connecting screen -------------------------------------------------
	struct ConnectingResult {
		bool cancel = false;
	};
	// `fraction < 0.0f` (the default) means "no known byte-progress" -- draws
	// just the status text as before, no bar. Callers pass a real 0.0-1.0
	// value once asset-sync byte counts are available (see
	// ClientSession::asset_sync_total_bytes()/asset_sync_received_bytes()).
	ConnectingResult draw_connecting(std::string_view status_text,
			float fraction = -1.0f);

	// --- sign-in screen (Phase 9.5, architecture_spec/auth.md §7) ------------
	// Engine-drawn, never pack content: the server's `auth.lua` only picks the
	// provider. The password is masked, kept only in this object while typing,
	// and wiped as soon as it is handed back.
	struct SigningInView {
		std::string_view title; // auth.lua display_name (may be empty)
		std::string_view server; // "host:port"
		std::string_view provider_host; // host of the issuer, so the player sees who they sign in with
		bool needs_trust = false; // first use of this (server, issuer): ask before contacting the IdP
		bool offer_browser = false; // oidc/keycloak, or firebase "google": a browser sign-in
		std::string_view browser_label = "Sign in with your browser"; // the button text
		bool offer_password = false; // firebase e-mail/password form
		bool working = false; // a sign-in is in flight; inputs disabled
		std::string_view error; // previous attempt's failure, empty if none
	};
	struct SigningInResult {
		bool trust = false; // "Continue" on the first-use prompt
		bool cancel = false;
		bool browser = false;
		bool submit_password = false;
		std::string email;
		std::string password;
	};
	SigningInResult draw_signing_in(const SigningInView &view);

	// --- error screen --------------------------------------------------------
	struct ErrorResult {
		bool back = false;
	};
	ErrorResult draw_error(std::string_view reason, std::string_view title = "Could not connect");

	// --- loading screen (Phase 7.1) ---------------------------------------
	// The window between "joined" and "first playable frame" -- initial chunk
	// streaming. `fraction` is [0,1] (clamp before calling); `operator_title`
	// is stage 2's optional operator branding (server.toml's `motd`, once the
	// handshake has actually delivered it -- empty until then, drawn under
	// the generic bar rather than blocking it). Deliberately no Lua/pack
	// involvement and no cancel button -- this screen's only job is getting
	// out of the way quickly, not hosting interaction.
	void draw_loading(float fraction, std::string_view operator_title);

private:
	// Embedded dirt backdrop + UI fonts (src/render/resources), loaded lazily on the first draw
	// because they need a live GL context. Opaque here so this header stays raylib-free.
	struct Assets;
	std::unique_ptr<Assets> assets_;
	void ensure_assets();
	// Tiled dirt texture under a dark scrim; every draw_* starts with it (Phase 12.1).
	void draw_backdrop();

	std::string address_; // host of the last valid parse of address_text_
	std::string address_text_; // the field as typed: host, host:port or [v6]:port
	std::string address_error_; // shown under the field after a failed Continue
	int port_ = 27015;
	std::string player_name_;
	bool address_edit_ = true; // focused on entry so Enter works immediately
	bool name_edit_ = false;
	std::string signed_in_label_;
	// Sign-in screen form state.
	std::string si_email_;
	std::string si_password_;
	bool si_email_edit_ = false;
	bool si_password_edit_ = false;

	// Settings screen staging values (raygui widgets bind to these directly).
	int s_width_ = 1280;
	int s_height_ = 720;
	bool s_vsync_ = true;
	float s_fov_ = 70.0f;
	float s_render_distance_ = 8.0f;
	float s_sensitivity_ = 0.12f;
	int s_cache_mb_ = 512;
	std::string s_name_ = "Player";
	bool s_name_edit_ = false;
	SettingsSection settings_section_ = SettingsSection::kGeneral;
	int history_highlight_ = -1;
	int history_scroll_ = 0; // first visible History row
	bool s_width_edit_ = false;
	bool s_height_edit_ = false;
	bool s_cache_edit_ = false;

	// Keybindings screen staging values, in the same order the screen draws
	// them (forward/back/left/right/jump/sprint).
	int k_keys_[6] = { 87, 83, 65, 68, 32, 340 };
	// Index into k_keys_ currently waiting for a key press, or -1 if none.
	int k_rebinding_ = -1;
};

} // namespace vb::render
