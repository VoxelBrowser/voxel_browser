// ClientApp: the windowed client's whole per-frame state machine (menu ->
// connecting -> loading -> playing), extracted verbatim from src/client/main.cpp
// (phase E1 of docs/e2e-automation.md). `frame()` is one iteration of the old
// `while (!window.should_close())` body; behavior is unchanged.
#pragma once

#include <filesystem>
#include <optional>

#include "session_host.hpp"

#if defined(VB_WITH_AUTH)
#include "vb/auth/coordinator.hpp"
#include "vb/auth/http.hpp"
#endif

namespace vb::client {

enum class AppState { kMenu,
	kSettings,
	kKeybindings,
	kConnecting,
	kLoading,
	kPlaying,
	kError };

class ClientApp {
public:
	// `auto_connect`: skip the menu and connect right away (true =
	// singleplayer), mirroring `--singleplayer` / `--server` on the CLI.
	ClientApp(vb::core::ClientConfig config, std::string config_path,
			vb::render::Window &window, const std::string &cli_server, int cli_port,
			int configured_view_distance, std::optional<bool> auto_connect,
			bool render = true,
			std::optional<std::filesystem::path> auth_token_file = std::nullopt);

	// Headless only (render == false): blocks until the pending connection
	// joined or failed -- same fixed-tick / 10s-wall-clock waits the old
	// stand-alone run_headless() used -- then enters kPlaying. Prints the
	// "client: joined ..." / "client: join failed: ..." lines the smoke tests
	// match. Returns false on failure.
	bool connect_blocking();

	// Runs one frame (window.begin_frame() .. end_frame()). Returns false when
	// the player quit from the main menu.
	bool frame(const vb::render::InputFrame &input, double dt);

	AppState app_state() const { return state; }

	// Read-only views for the dev-only automation endpoint (and tests).
	static const char *app_state_name(AppState s);
	const vb::net::ClientSession *session() const { return client; }
	const std::deque<std::string> &chat() const { return chat_log; }
#if defined(VB_WITH_AUTH)
	const vb::auth::SignInCoordinator *sign_in_state() const { return sign_in.get(); }
#endif

#if defined(VB_WITH_AUTOMATION)
	// --- development-only automation access (docs/e2e-automation.md §5.1) ---
	// Compiled out of production builds with the rest of the automation code.
	// Everything here either reads state or pokes the same members the real
	// input path already drives; none of it is a protocol shortcut.

	// Headless clients skip drawing, and with it the modal-screen / HUD Lua
	// render calls that populate widget lists. Automation needs those lists
	// (ui.click, state.ui), so this makes frame() evaluate them even when
	// render == false. Off by default so plain --headless is unchanged.
	void set_headless_ui_eval(bool on) { headless_ui_eval = on; }

	double yaw() const { return controller.yaw(); }
	double pitch() const { return controller.pitch(); }
	vb::core::Vec3d eye() const { return controller.position(); }
	// Absolute look (the real path is mouse deltas; tests want exact aim).
	void set_look(double yaw_deg, double pitch_deg) { controller.set_look(yaw_deg, pitch_deg); }
	bool mouse_captured_state() const { return mouse_captured; }
	void set_mouse_captured(bool on) { mouse_captured = on; }
	std::uint8_t selected_hotbar_slot() const { return selected_slot; }
	bool chat_box_open() const { return chat_open; }
	const vb::render::MovementBindings &bindings() const { return movement_bindings; }

	vb::script::UiRuntime &ui() { return ui_runtime; }
	const vb::script::UiRuntime &ui() const { return ui_runtime; }
	// The HUD widget list from the most recent headless evaluation.
	const std::vector<vb::script::Widget> &hud_widgets() const { return hud_widget_cache; }

	// What the chat box does on Enter: send `text` over the real chat path.
	void submit_chat(std::string_view text);
	// Types into the chat box exactly as keyboard characters would (opening it first if
	// needed); pressing Enter then submits it through the normal frame logic. The raygui
	// box that draws the text only exists in a windowed client, but the buffer and the
	// Enter handling are shared, so this works headless too.
	bool type_chat(std::string_view text);

	// What a human (or a script) does, reported as meaning rather than as raw input, for the
	// recorder (docs/e2e-automation.md §8, E6). Called from the real frame logic, so a
	// player's clicks, typing and menu choices are seen exactly as the game acted on them.
	class Observer {
	public:
		virtual ~Observer() = default;
		virtual void on_chat_sent(std::string_view /*text*/) {}
		virtual void on_ui_click(const std::string & /*id*/, bool /*hud*/) {}
		virtual void on_ui_change(const std::string & /*id*/, const std::string & /*text*/, bool /*hud*/) {}
		virtual void on_ui_list(const std::string & /*id*/, int /*index*/, bool /*hud*/) {}
		virtual void on_menu_connect(const std::string & /*host*/, int /*port*/, const std::string & /*name*/) {}
		virtual void on_menu_singleplayer(const std::string & /*name*/) {}
	};
	void set_observer(Observer *o) { observer = o; }
	// The UI entry points the frame loop uses, exposed so automation commands take the
	// same path (and are observed the same way).
	void ui_click(const std::string &id, bool hud);
	void ui_change(const std::string &id, const std::string &text, bool hud);
	void ui_list(const std::string &id, int index, bool hud);

	// --- main-menu injection (windowed clients; raygui can't be clicked by script) ---
	// The menu is drawn by raygui from raylib's real input state, so these set the same
	// fields a player would type into and hand kMenu the same result a click would.
	bool menu_active() const { return state == AppState::kMenu; }
	// The name this connection used (what the menu holds), not config.player_name, which only
	// follows it after a menu-driven connect.
	const std::string &player_name() const { return menu.player_name(); }
	void menu_set_name(std::string_view name) {
		const std::string address = menu.address(); // prefill() reassigns it: pass a copy
		menu.prefill(address, menu.port(), name);
	}
	void menu_connect(std::string_view host, int port, std::string_view name) {
		menu.prefill(host, port, name);
		pending_menu = vb::render::MainMenu::MainResult{ .connect = true };
	}
	void menu_singleplayer(std::string_view name) {
		const std::string address = menu.address();
		menu.prefill(address, menu.port(), name);
		pending_menu = vb::render::MainMenu::MainResult{ .singleplayer = true };
	}

	// Saves the framebuffer to `path` (PNG) at the end of the next frame. Windowed only.
	enum class Screenshot { kNone,
		kPending,
		kDone,
		kFailed };
	bool request_screenshot(std::string path);
	Screenshot screenshot_state() const { return screenshot_state_; }
	int screenshot_width() const { return screenshot_w_; }
	int screenshot_height() const { return screenshot_h_; }
#endif

private:
	void begin_connect(bool as_singleplayer);
	void enter_playing();

	vb::core::ClientConfig config;
	const std::string config_path;
	vb::render::Window &window;
	const int configured_view_distance;
	const float fov;
	// false = headless: same state machine/input/prediction/UI runtime, but no
	// menu, GL resources or draw calls (window may be a headless Window).
	const bool render;
	vb::render::MainMenu menu;
	AppState state = AppState::kMenu;
	std::string error_message;
	// The view distance actually in effect this connection -- starts at
	// configured_view_distance every time begin_connect() runs, then
	// enter_playing() clamps it down to a real server's own (possibly
	// smaller) S2CServerInfo::view_distance once that's known. Drives
	// kLoading's expected-chunk-count estimate and the default fog distance
	// below, so both always agree with whatever this connection can
	// actually stream in.
	int view_distance = 0;
	bool connecting_singleplayer = false;
	std::string connecting_target;
	int connect_ticks = 0;
	std::chrono::steady_clock::time_point connect_deadline;
	// Phase 7.1: hard cap on how long kLoading waits for the initial view-box
	// of chunks to stream in before letting the player through anyway (e.g. a
	// server whose own view_distance is smaller than this client guessed, or
	// a slow connection) -- getting out of the way beats blocking forever.
	// `loading_deadline` is a *stall* deadline, not a flat one: it's pushed
	// forward every time `loading_last_uploaded` (the previous frame's
	// uploaded_count) advances, so a large view distance that's genuinely
	// still meshing/uploading chunks -- just slowly -- isn't cut off mid-load
	// (an earlier flat 8s deadline handed off to kPlaying while most of the
	// default view_distance=8 box, ~2000 chunks, was still unmeshed). Only a
	// real stall -- e.g. a server whose own view_distance is smaller than
	// this client guessed, so `fraction` can never reach 1.0 -- lets it fire.
	// `loading_hard_deadline` is the absolute backstop against a pathological
	// server that trickles in just enough chunks each tick to keep resetting
	// the stall deadline forever.
	std::chrono::steady_clock::time_point loading_deadline;
	std::chrono::steady_clock::time_point loading_hard_deadline;
	std::size_t loading_last_uploaded = 0;
	// 2000ms measured as too tight in practice: `loading_deadline` starts
	// counting the instant kLoading is entered, before the server has sent a
	// single chunk -- the very first batch out of a cold worldgen pool (no
	// cached chunks yet, every one of the view box's chunks generated fresh)
	// can itself take longer than 2s, so `loaded_chunks` was still 0 when the
	// stall deadline fired and kPlaying was entered with (visibly) nothing
	// loaded -- reported as "the world does not finish loading when the
	// loading screen disappears". 5s gives the worldgen -> mesh -> upload
	// pipeline room to produce its first real batch without making a
	// genuinely dead connection (no server, wrong port) wait much longer
	// before `loading_hard_deadline` would have caught it anyway.
	static constexpr std::chrono::milliseconds kLoadingStallTimeout{ 5000 };
	static constexpr std::chrono::seconds kLoadingHardTimeout{ 30 };

	std::unique_ptr<Singleplayer> sp;
#if defined(VB_WITH_AUTH)
	// External authentication (auth.md §7). Declared before `remote` so it is
	// destroyed after it: the session's sign-in ticket points back here.
	std::optional<std::filesystem::path> auth_token_file_;
	std::unique_ptr<vb::auth::SignInCoordinator> sign_in;
	std::shared_ptr<vb::auth::SessionStore> auth_store;
	bool reauth_panel_open = false; // the in-game "sign in again" overlay
	int menu_frames = 0;
	std::string signed_in_server_; // "host:port" the menu's sign-in label is for
	void install_sign_in(vb::net::ClientSession &session, const std::string &server_id);
#if defined(VB_WITH_AUTOMATION)
	bool headless_browser_started_ = false;
	void headless_sign_in_step();
#endif
	void draw_sign_in();
	void draw_reauth_prompt();
	void refresh_signed_in_label();
#endif
	std::unique_ptr<RemoteConnection> remote;
	vb::net::ClientSession *client = nullptr;
	vb::core::Vec3d spawn{ 0.0, 72.0, 0.0 };
	std::string status;

	vb::script::UiRuntime ui_runtime;
	vb::render::UiRenderer ui_renderer;
	// A separate UiRenderer instance for the always-on HUD (below): drawing
	// both the modal screen and the HUD through one UiRenderer would thrash
	// its per-widget-id text/list edit caches every frame (it clears them
	// whenever the drawn ui_name changes, which "hud" vs. the modal name
	// would do twice a frame).
	vb::render::UiRenderer hud_renderer;
	vb::render::FirstPersonController controller;
	// Render-only step-up smoothing (Phase 3's "physics is exact but visually
	// abrupt" gap) -- reset alongside `controller` in enter_playing() below so
	// a fresh connection/respawn never inherits a stale in-flight smoothing
	// state from a previous life.
	vb::render::EyeHeightSmoother eye_smoother;
	vb::physics::MoveParams move_params;
	std::uint32_t input_seq = 0;
	std::unique_ptr<vb::render::ChunkRenderer> chunk_renderer;
	std::unique_ptr<vb::render::EntityRenderer> entity_renderer;
	// REMAINING_TASKS.md 6.5's last piece: real crack-stage art + per-block
	// crack_texture override, replacing the old flat translucent-cube
	// overlay. Built once per session right alongside chunk_renderer's own
	// texture atlas, below.
	std::unique_ptr<vb::render::CrackOverlay> crack_overlay;
	vb::render::CrackAtlas crack_atlas;
	bool mouse_captured = false;
	// A pack asked for capture (UiRuntime::take_capture_request) that waits
	// for the screen/chat box to close; Tab/Escape or a release cancel it.
	bool pending_capture_ = false;
	// Set when a pack-requested capture lands: `primary` is masked out until
	// the left button is released, so the closing click never punches.
	bool suppress_primary_ = false;
	// Entity-management follow-up (held item / hotbar selection): which
	// inventory slot (0-based) number keys 1-9 have selected, persisted
	// across frames like input_seq above -- sample_input_cmd only ever reads
	// this, number-key handling lives in the kPlaying loop below, gated on
	// mouse_captured the same way movement/break/place input already is (so
	// typing "1" into an open chat box never changes it).
	std::uint8_t selected_slot = 0;

	// Phase 6.17: the client-local physical-key-to-action map for movement +
	// break/place (see vb::render::MovementBindings' own comment above). One instance,
	// same defaults every frame -- a future settings screen would mutate
	// this instead of inventing a second mechanism.
	vb::render::MovementBindings movement_bindings{ config.key_forward, config.key_back,
		config.key_left, config.key_right, config.key_jump, config.key_sprint };

	// HUD chat (spec §5.4): a small scrolling log + an Enter-to-open text
	// box, plain raygui like MainMenu -- no Lua, no dependency on the pack's
	// UiRuntime chat concept (there isn't one).
	static constexpr std::size_t kChatLogLimit = 8;
	static constexpr int kChatBufferSize = 256;
	std::deque<std::string> chat_log;
	std::string chat_buf;
	bool chat_open = false;
	// F5 third-person view (see the playing loop); off on every join.
	bool third_person = false;
	static constexpr double kThirdPersonDistance = 4.0;
#if defined(VB_WITH_AUTOMATION)
	bool headless_ui_eval = false;
	std::vector<vb::script::Widget> hud_widget_cache;
	std::optional<vb::render::MainMenu::MainResult> pending_menu;
	Observer *observer = nullptr;
	std::string screenshot_path_;
	Screenshot screenshot_state_ = Screenshot::kNone;
	int screenshot_w_ = 0, screenshot_h_ = 0;
#endif
};

} // namespace vb::client
