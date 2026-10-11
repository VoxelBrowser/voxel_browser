// ClientApp implementation -- the windowed loop's body moved out of
// src/client/main.cpp (phase E1 of docs/e2e-automation.md), no behavior change.

#include "client_app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>

#if defined(VB_WITH_AUTOMATION)
#include <rlgl.h>
#endif

// Notifies the recorder (automation builds only); a no-op in production builds.
#if defined(VB_WITH_AUTOMATION)
#define VB_OBSERVE(call)           \
	do {                           \
		if (observer != nullptr) { \
			observer->call;        \
		}                          \
	} while (0)
#else
#define VB_OBSERVE(call) ((void)0)
#endif

namespace vb::client {

namespace {

// Damaged blocks farther than this from the camera get no crack overlay:
// the crack art is unreadable past a few blocks, and the server replicates
// damage for every loaded chunk, not just ones within reach.
constexpr float kCrackDrawDistance = 24.0f;

// How long a connection attempt may take before the client gives up. 10 s in every shipped
// build. Development builds let the test harness stretch it (VB_CONNECT_TIMEOUT_SECONDS), as it
// stretches its own waits, so a sanitized client behind a simulated bad network isn't judged
// by a clock meant for a healthy one.
std::chrono::seconds connect_timeout() {
#if defined(VB_WITH_AUTOMATION)
	if (const char *v = std::getenv("VB_CONNECT_TIMEOUT_SECONDS")) {
		const long s = std::strtol(v, nullptr, 10);
		if (s >= 1 && s <= 600) {
			return std::chrono::seconds(s);
		}
	}
#endif
	return std::chrono::seconds(10);
}

} // namespace

ClientApp::ClientApp(vb::core::ClientConfig config_in, std::string config_path_in,
		vb::render::Window &window_in, const std::string &cli_server, int cli_port,
		int configured_view_distance_in, std::optional<bool> auto_connect, bool render_in,
		std::optional<std::filesystem::path> auth_token_file) :
		config(std::move(config_in)),
		config_path(std::move(config_path_in)),
		window(window_in),
		configured_view_distance(configured_view_distance_in),
		fov(static_cast<float>(config.fov)),
		render(render_in),
		menu(config),
		view_distance(configured_view_distance_in),
		movement_bindings{ config.key_forward, config.key_back, config.key_left,
			config.key_right, config.key_jump, config.key_sprint } {
#if defined(VB_WITH_AUTH)
	auth_token_file_ = std::move(auth_token_file);
#else
	(void)auth_token_file;
#endif
	menu.prefill(cli_server, cli_port, config.player_name);
	if (auto_connect) {
		begin_connect(*auto_connect);
	}
}

void ClientApp::begin_connect(bool as_singleplayer) {
	connecting_singleplayer = as_singleplayer;
	connect_ticks = 0;
	sp.reset();
	remote.reset();
#if defined(VB_WITH_AUTH)
	sign_in.reset(); // after `remote`: the session's ticket points at it
#endif
	client = nullptr;
	// Undo any clamp a previous connection's enter_playing() applied --
	// a fresh connection (even a retry of the same server) starts back
	// at the player's own full preference until this one's own
	// S2CServerInfo says otherwise.
	view_distance = configured_view_distance;
	if (as_singleplayer) {
		sp = std::make_unique<Singleplayer>(7, menu.player_name(), view_distance);
		connecting_target = "singleplayer";
		if (!sp->world_error.empty()) {
			error_message = sp->world_error;
			std::cout << "client: " << error_message << '\n';
			sp.reset();
			state = AppState::kError;
			return;
		}
#if defined(VB_WITH_AUTH)
		install_sign_in(sp->client(), connecting_target);
#endif
		state = AppState::kConnecting;
	} else {
		connecting_target = menu.address() + ':' + std::to_string(menu.port());
		std::cout << "client: connecting to " << connecting_target << "...\n";
		remote = std::make_unique<RemoteConnection>(menu.address(),
				static_cast<std::uint16_t>(menu.port()), menu.player_name(), config);
		connect_deadline = std::chrono::steady_clock::now() + connect_timeout();
		if (!remote->session) {
			error_message = "Could not connect to " + connecting_target +
					" (bad address, or built without VB_WITH_NET)";
			// Headless output is matched by the client_smoke CTest regex.
			std::cout << "client: "
					  << (render ? error_message
								 : "could not connect to " + connecting_target +
												 " (bad address, or built without VB_WITH_NET)")
					  << '\n';
			state = AppState::kError;
		} else {
#if defined(VB_WITH_AUTH)
			install_sign_in(*remote->session, connecting_target);
#endif
			state = AppState::kConnecting;
		}
	}
}

#if defined(VB_WITH_AUTH)
// Always installed: it only ever runs if the server answers with an
// S2C_AuthChallenge (a server without auth.lua never does). Singleplayer uses
// the same path, so a pack that declares auth.lua signs in for real there too.
void ClientApp::install_sign_in(vb::net::ClientSession &session, const std::string &server_id) {
	if (!auth_store) {
		auth_store = std::make_shared<vb::auth::SessionStore>(vb::core::user_config_dir() / "auth");
	}
	vb::auth::SignInCoordinator::Options opts;
	opts.http = std::shared_ptr<vb::auth::HttpFetcher>(vb::auth::make_curl_fetcher());
	opts.token_file = auth_token_file_;
	opts.store = auth_store;
	opts.server_id = server_id;
#if defined(VB_WITH_AUTOMATION)
	// Dev/test only: instead of launching a browser, write the authorization
	// URL here so a test can play the user's browser (tests/e2e/test_auth.py).
	if (const char *url_file = std::getenv("VB_AUTH_URL_FILE"); url_file != nullptr && *url_file != '\0') {
		opts.open_browser = [path = std::string(url_file)](const std::string &url) {
			std::ofstream out(path, std::ios::trunc);
			out << url;
			return static_cast<bool>(out);
		};
	}
	headless_browser_started_ = false;
#endif
	sign_in = std::make_unique<vb::auth::SignInCoordinator>(std::move(opts));
	session.set_sign_in_provider(sign_in->provider());
	session.set_reauth_provider(sign_in->reauth_provider());
	reauth_panel_open = false;
}

#if defined(VB_WITH_AUTOMATION)
// Headless automation has no window to click in: accept the trust prompt and
// start the browser flow by itself (the test then plays the browser). Compiled
// out of production builds -- auto-trusting a server would defeat the prompt.
void ClientApp::headless_sign_in_step() {
	if (render || !sign_in || auth_token_file_) {
		return;
	}
	using Phase = vb::auth::SignInCoordinator::Phase;
	if (sign_in->needs_trust()) {
		sign_in->trust();
	}
	if (!headless_browser_started_ && sign_in->phase() == Phase::kChoosing &&
			!sign_in->needs_trust() && sign_in->supports_browser()) {
		headless_browser_started_ = true;
		sign_in->start_browser();
	} else if (headless_browser_started_ && sign_in->phase() == Phase::kChoosing &&
			!sign_in->last_error().empty()) {
		// No window to retry in: a failed browser attempt (e.g. no browser could be
		// opened) ends the join instead of waiting out the server's auth timeout.
		sign_in->cancel();
	} else if (!sign_in->supports_browser() && !sign_in->needs_trust() &&
			sign_in->phase() == Phase::kChoosing) {
		sign_in->cancel(); // firebase password etc.: nothing a headless client can do
	}
}
#endif

// The engine-drawn sign-in screen (auth.md §7). The player may take minutes in
// a browser tab, so the client-side connect deadline is held off while it is
// up; the server's own auth_timeout_seconds is the real bound.
void ClientApp::draw_sign_in() {
	using Phase = vb::auth::SignInCoordinator::Phase;
	connect_deadline = std::chrono::steady_clock::now() + connect_timeout();
	if (!render || !sign_in) {
		return; // headless: a token file (or nothing) drives it
	}
	const auto challenge = sign_in->challenge();
	vb::render::MainMenu::SigningInView view;
	const std::string title = challenge ? challenge->display_name : std::string();
	view.title = title;
	view.server = connecting_target;
	std::string host = challenge ? challenge->issuer : std::string();
	if (const auto p = host.find("//"); p != std::string::npos) {
		host = host.substr(p + 2);
	}
	host = host.substr(0, host.find('/'));
	view.provider_host = host;
	view.needs_trust = sign_in->needs_trust();
	view.offer_browser = sign_in->supports_browser();
	view.offer_password = sign_in->supports_password();
	view.working = sign_in->phase() == Phase::kWorking;
	const std::string last_error = sign_in->last_error();
	view.error = last_error;
	auto ui = menu.draw_signing_in(view);
	if (ui.cancel) {
		sign_in->cancel();
		sp.reset(); // before sign_in: its session holds a ticket pointing at it
		remote.reset();
		sign_in.reset();
		client = nullptr;
		state = AppState::kMenu;
	} else if (ui.trust) {
		sign_in->trust();
	} else if (ui.browser) {
		sign_in->start_browser();
	} else if (ui.submit_password) {
		sign_in->start_password(ui.email, ui.password);
		std::fill(ui.password.begin(), ui.password.end(), '\0');
	}
}
// Periodic re-auth (auth.md §5.6): the silent refresh happens without any UI;
// only when it fails does this non-blocking banner appear over the running
// game. The player can ignore it, but the server kicks once its grace runs out.
void ClientApp::draw_reauth_prompt() {
	if (!render || !sign_in || !sign_in->reauth_prompt_active()) {
		reauth_panel_open = false;
		return;
	}
	const float w = 420.0f;
	const float x = (static_cast<float>(GetScreenWidth()) - w) * 0.5f;
	if (!reauth_panel_open) {
		GuiPanel(Rectangle{ x, 8.0f, w, 40.0f }, nullptr);
		GuiLabel(Rectangle{ x + 10.0f, 16.0f, w - 150.0f, 24.0f }, "Your sign-in expired.");
		if (GuiButton(Rectangle{ x + w - 130.0f, 14.0f, 120.0f, 28.0f }, "Sign in again")) {
			reauth_panel_open = true;
			if (mouse_captured) {
				mouse_captured = false;
				EnableCursor();
			}
		}
		return;
	}
	const auto challenge = sign_in->challenge();
	vb::render::MainMenu::SigningInView view;
	view.server = connecting_target;
	std::string host = challenge ? challenge->issuer : std::string();
	if (const auto p = host.find("//"); p != std::string::npos) {
		host = host.substr(p + 2);
	}
	host = host.substr(0, host.find('/'));
	view.provider_host = host;
	view.offer_browser = sign_in->supports_browser();
	view.offer_password = sign_in->supports_password();
	using Phase = vb::auth::SignInCoordinator::Phase;
	view.working = sign_in->phase() == Phase::kWorking;
	const std::string last_error = sign_in->last_error();
	view.error = last_error;
	auto ui = menu.draw_signing_in(view);
	if (ui.cancel) {
		reauth_panel_open = false; // only closes the panel; the request stays open
	} else if (ui.browser) {
		sign_in->start_browser();
	} else if (ui.submit_password) {
		sign_in->start_password(ui.email, ui.password);
		std::fill(ui.password.begin(), ui.password.end(), '\0');
	}
}

// The saved login for the server currently in the connect fields: logins
// are per server (vb::auth::SessionStore), so there is no engine-wide
// "signed in" state to show.
void ClientApp::refresh_signed_in_label() {
	if (!auth_store) {
		auth_store = std::make_shared<vb::auth::SessionStore>(
				vb::core::user_config_dir() / "auth");
	}
	signed_in_server_ = menu.address() + ':' + std::to_string(menu.port());
	const auto sessions = auth_store->list_for(signed_in_server_);
	if (sessions.empty()) {
		menu.set_signed_in_label({});
		return;
	}
	const std::string &account = sessions.front().label;
	menu.set_signed_in_label(signed_in_server_ + " as " + (account.empty() ? std::string("account") : account));
}
#endif

void ClientApp::enter_playing() {
	client = connecting_singleplayer ? &sp->client() : &*remote->session;
	// Bind this connection's effective view distance to whatever the
	// server actually just told us (S2CServerInfo::view_distance,
	// always present, unlike the opt-in fog/move-params messages) --
	// never wider than the player's own configured_view_distance, so a
	// server advertising a larger box than the player asked for doesn't
	// silently raise their own setting. A no-op for singleplayer:
	// sp_server_config() above already echoes this same
	// configured_view_distance back as the server's own.
	if (const auto &info = client->server_info()) {
		view_distance = std::min(configured_view_distance,
				static_cast<int>(info->view_distance));
	}
	const auto &accept = *client->join_accept();
	const vb::core::NetId net_id = accept.your_net_id;
	spawn = accept.spawn_pos;
	status = (connecting_singleplayer ? std::string("singleplayer")
									  : ("connected to " + connecting_target)) +
			" — net id " + std::to_string(static_cast<std::uint32_t>(net_id)) +
			", seed " + std::to_string(accept.world_seed);
	std::cout << "client: joined " << status << '\n';

	// Client UI VM (spec §10.4, Phase 4.5): a second, restricted Lua VM,
	// separate from PackRuntime's server-side one. `ui/*.lua` travels over
	// Asset Sync like any other pack file (Phase 4.4) for a real
	// multiplayer connection; `--singleplayer` never asset-syncs (no
	// PackRuntime/manifest on that in-process path, REMAINING_TASKS.md
	// 4.3), so it instead reads `ui/*.lua` directly off disk from the
	// same `kSingleplayerContentPack` the integrated server's PackRuntime
	// already loads (client and server share one machine/filesystem
	// there, so there's nothing to "sync") -- otherwise the HUD below
	// (and every other Lua-defined screen) would silently never load in
	// the most common dev/test path.
	ui_runtime = vb::script::UiRuntime{};
	ui_runtime.attach_session(*client);
	std::vector<std::pair<std::string, std::string>> ui_sources;
	if (connecting_singleplayer) {
		const std::filesystem::path ui_dir =
				std::filesystem::path(kSingleplayerContentPack) / "ui";
		std::error_code ec;
		if (std::filesystem::is_directory(ui_dir, ec)) {
			for (const auto &entry : std::filesystem::directory_iterator(ui_dir, ec)) {
				if (entry.path().extension() != ".lua") {
					continue;
				}
				std::ifstream f(entry.path(), std::ios::binary);
				if (!f) {
					continue;
				}
				std::ostringstream ss;
				ss << f.rdbuf();
				ui_sources.emplace_back(
						"ui/" + entry.path().filename().string(), ss.str());
			}
		}
	} else {
		for (const auto &[path, bytes] : client->virtual_pack_fs()) {
			if (path.rfind("ui/", 0) != 0 || path.size() < 4 ||
					path.substr(path.size() - 4) != ".lua") {
				continue;
			}
			ui_sources.emplace_back(path,
					std::string(reinterpret_cast<const char *>(bytes.data()),
							bytes.size()));
		}
	}
	// Directory/map iteration order isn't guaranteed; sort so a file like
	// `ui/_style.lua` always loads before the screens that use it.
	std::sort(ui_sources.begin(), ui_sources.end());
	for (const auto &[path, source] : ui_sources) {
		const vb::script::ScriptResult result =
				ui_runtime.load_pack_file(source, path);
		if (!result.ok && result.error != vb::core::ScriptError::kDisabled) {
			std::cerr << "client: ui pack file '" << path
					  << "' failed to load: " << result.message << '\n';
		}
	}

	controller = vb::render::FirstPersonController{};
	controller.set_position({ spawn.x, spawn.y + 1.7, spawn.z });
	controller.set_look(0.0, -20.0);
	controller.set_sensitivity(config.mouse_sensitivity);
	eye_smoother.reset(spawn.y + 1.7);

	// Phase 6.7: read back what's already applied (S2C_MoveParams arrives
	// alongside S2C_JoinAccept, so it's already in the session by now)
	// instead of stomping it back to the engine default.
	move_params = client->move_params();
	client->set_local_feet(spawn);
	input_seq = 0;
	// Everything below builds GPU resources; a render=false (headless) app has no GL context.
	if (render) {
		chunk_renderer = std::make_unique<vb::render::ChunkRenderer>();
		// Real texture/atlas system: built once per session, right after the
		// block registry (S2C_BlockRegistry, already applied by now -- see
		// client->move_params() above reading back another join-time
		// message the same way) and every referenced texture's bytes are
		// available, and before kLoading starts streaming/meshing any chunk
		// -- so every chunk mesh this session uploads already gets real
		// atlas UVs from its very first upload, no re-upload-on-atlas-
		// arrival case to handle. `remote` resolves texture paths against
		// its already-synced Asset Sync virtual FS; `sp` (--singleplayer)
		// has no asset sync at all (client + server share one in-process
		// registry/content pack), so it reads the same
		// `kSingleplayerContentPack` the integrated server's PackRuntime
		// loaded from, straight off disk instead.
		{
			const vb::render::VirtualFs vfs = remote ? remote->asset_cache.virtual_fs()
													 : load_textures_from_disk(client->chunk_store().registry(),
															   kSingleplayerContentPack);
			vb::render::TextureAtlas atlas =
					vb::render::TextureAtlas::build(client->chunk_store().registry(), vfs);
			std::vector<vb::render::AtlasRect> rects;
			std::vector<Color> averages;
			std::vector<bool> translucent;
			rects.reserve(atlas.block_count());
			averages.reserve(atlas.block_count());
			for (std::size_t i = 0; i < atlas.block_count(); ++i) {
				const auto id = static_cast<vb::core::BlockId>(i);
				rects.push_back(atlas.rect_for(id));
				averages.push_back(atlas.average_color_for(id));
				translucent.push_back(atlas.is_translucent(id));
			}
			chunk_renderer->set_atlas(atlas.upload(), std::move(rects), std::move(averages), std::move(translucent));

			// REMAINING_TASKS.md 6.5's last piece: same join-time, same vfs
			// -- a block's crack_texture (if any) is synced/on-disk exactly
			// like its regular texture.
			crack_atlas = vb::render::CrackAtlas::build(client->chunk_store().registry(), vfs);
			crack_overlay = std::make_unique<vb::render::CrackOverlay>();
			crack_overlay->set_texture(crack_atlas.upload());
		}
		entity_renderer = std::make_unique<vb::render::EntityRenderer>();
		entity_renderer->set_block_colors(chunk_renderer.get());
		// Entity-management follow-up: build any registered kind's real
		// spritesheet (S2C_EntityKindRegistry.visual) the same session the
		// block texture atlas above was built -- both are one-shot,
		// join-time setup reading from the same synced/disk content pack. A
		// kind that never set `visual = {...}` is untouched, keeping its
		// flat placeholder billboard exactly as before this existed.
		{
			vb::render::VirtualFs entity_vfs = remote
					? remote->asset_cache.virtual_fs()
					: load_entity_textures_from_disk(client->entity_kind_registry(), kSingleplayerContentPack);
			const auto &entity_kinds = client->entity_kind_registry();
			for (std::size_t i = 0; i < entity_kinds.size(); ++i) {
				const auto &rec = entity_kinds[i];
				if (rec.visual) {
					entity_renderer->set_kind_visual(
							static_cast<vb::core::EntityKindId>(i + 1), *rec.visual, entity_vfs);
				}
			}
			// Entity-management follow-up: kept for the rest of the session
			// so sync() can lazily decode a per-instance visual_override's
			// texture whenever one shows up (unlike the kind visuals above,
			// an override's owning entity can spawn at any later time, not
			// just during this one join-time pass).
			entity_renderer->set_virtual_fs(std::move(entity_vfs));
			if (!remote) {
				entity_renderer->set_disk_fallback(kSingleplayerContentPack);
			}
		}
	}
	mouse_captured = false;
	pending_capture_ = false;
	suppress_primary_ = false;
	chat_log.clear();
	chat_buf.clear();
	chat_open = false;
	third_person = false;

	if (!connecting_singleplayer) {
		auto &recents = config.recent_servers;
		recents.erase(std::remove(recents.begin(), recents.end(), connecting_target),
				recents.end());
		recents.insert(recents.begin(), connecting_target);
		if (recents.size() > 8) {
			recents.resize(8);
		}
	}
	config.player_name = menu.player_name();
	// Headless never touched client.toml (CI / tests must not rewrite it).
	if (render) {
		if (auto saved = vb::core::save_client_config(config_path, config); !saved) {
			std::cerr << "client: could not save '" << config_path
					  << "': " << vb::core::message(saved.error()) << '\n';
		}
	}

	// Phase 7.1: a loading screen between "joined" and "first playable
	// frame" instead of dropping straight into kPlaying -- the first
	// frames after join are otherwise an emptier-than-usual world (chunks
	// still streaming in), rendered with no indication that's expected.
	loading_deadline = std::chrono::steady_clock::now() + kLoadingStallTimeout;
	loading_hard_deadline = std::chrono::steady_clock::now() + kLoadingHardTimeout;
	loading_last_uploaded = 0;
	// No renderer means nothing to wait on: go straight to playing.
	state = render ? AppState::kLoading : AppState::kPlaying;
}

const char *ClientApp::app_state_name(AppState s) {
	switch (s) {
		case AppState::kMenu:
			return "menu";
		case AppState::kSettings:
			return "settings";
		case AppState::kKeybindings:
			return "keybindings";
		case AppState::kConnecting:
			return "connecting";
		case AppState::kLoading:
			return "loading";
		case AppState::kPlaying:
			return "playing";
		case AppState::kError:
			return "error";
	}
	return "unknown";
}

bool ClientApp::connect_blocking() {
	if (state == AppState::kError || (!sp && !remote)) {
		return false;
	}
	if (connecting_singleplayer) {
		vb::net::ClientSession &c = sp->client();
		for (int i = 0; i < 128 && !c.joined() && !c.failed(); ++i) {
			sp->tick(0.05);
		}
#if defined(VB_WITH_AUTH) && defined(VB_WITH_AUTOMATION)
		// A pack with auth.lua makes the integrated server ask this client to sign in, which
		// takes as long as the (test-played) browser takes, and the server verifies the token on
		// a worker thread: from here on wait in real time like for a remote server (automation
		// builds only; a production headless client has no way to answer and keeps failing fast).
		if (sign_in && !c.joined() && !c.failed() && c.status() != vb::net::ClientHandshakeStatus::kConnecting) {
			auto deadline = std::chrono::steady_clock::now() + connect_timeout();
			while (!c.joined() && !c.failed() && std::chrono::steady_clock::now() < deadline) {
				if (c.status() == vb::net::ClientHandshakeStatus::kSigningIn) {
					deadline = std::chrono::steady_clock::now() + connect_timeout();
					headless_sign_in_step();
				}
				sp->tick(0.05);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
#endif
		client = &c;
	} else {
		client = &*remote->session;
		auto deadline = std::chrono::steady_clock::now() + connect_timeout();
		while (!client->joined() && !client->failed() &&
				std::chrono::steady_clock::now() < deadline) {
#if defined(VB_WITH_AUTH)
			if (sign_in && client->status() == vb::net::ClientHandshakeStatus::kSigningIn) {
				// A sign-in takes as long as it takes; the server's own auth timeout bounds it.
				deadline = std::chrono::steady_clock::now() + connect_timeout();
#if defined(VB_WITH_AUTOMATION)
				headless_sign_in_step();
#endif
			}
#endif
			client->tick(0.05);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	if (!client->joined()) {
		std::cout << "client: join failed: "
				  << (client->failed() ? client->failure_reason() : "timed out")
				  << '\n';
		state = AppState::kError;
		return false;
	}
	enter_playing();
	return true;
}

bool ClientApp::frame(const vb::render::InputFrame &input, double dt) {
	window.begin_frame();

	switch (state) {
		case AppState::kMenu: {
#if defined(VB_WITH_AUTH)
			const bool server_changed =
					signed_in_server_ != menu.address() + ':' + std::to_string(menu.port());
			if (render && ((menu_frames++ % 120) == 0 || server_changed)) {
				refresh_signed_in_label(); // cheap: a handful of tiny files
			}
#endif
			auto result = menu.draw_main(config.recent_servers);
#if defined(VB_WITH_AUTOMATION)
			if (pending_menu) { // automation: the click a player would have made
				result = *pending_menu;
				pending_menu.reset();
			}
#endif
#if defined(VB_WITH_AUTH)
			if (result.sign_out && auth_store) {
				auth_store->sign_out(signed_in_server_); // this server only
				refresh_signed_in_label();
			}
#endif
			if (result.connect) {
				VB_OBSERVE(on_menu_connect(menu.address(), menu.port(), menu.player_name()));
				begin_connect(false);
			} else if (result.singleplayer) {
				VB_OBSERVE(on_menu_singleplayer(menu.player_name()));
				begin_connect(true);
			} else if (result.open_settings) {
				menu.open_settings(config);
				state = AppState::kSettings;
			} else if (result.quit) {
				window.end_frame();
				std::cout << "client: exited from menu\n";
				return false;
			}
			break;
		}
		case AppState::kSettings: {
			const auto result = menu.draw_settings(config);
			if (result.save) {
				vb::core::save_client_config(config_path, config);
				state = AppState::kMenu;
			} else if (result.back) {
				state = AppState::kMenu;
			} else if (result.open_keybindings) {
				menu.open_keybindings(config);
				state = AppState::kKeybindings;
			}
			break;
		}
		case AppState::kKeybindings: {
			const auto result = menu.draw_keybindings(config, input);
			if (result.save) {
				movement_bindings = vb::render::MovementBindings{ config.key_forward,
					config.key_back, config.key_left, config.key_right,
					config.key_jump, config.key_sprint };
				vb::core::save_client_config(config_path, config);
				state = AppState::kSettings;
			} else if (result.back) {
				state = AppState::kSettings;
			}
			break;
		}
		case AppState::kConnecting: {
			bool timed_out = false;
			if (connecting_singleplayer) {
				vb::net::ClientSession &sp_client = sp->client();
#if defined(VB_WITH_AUTH)
				// Signing in takes as long as the player takes: advance the
				// integrated server in real time (its own auth timeout counts
				// simulated seconds) and don't let the 128-tick budget expire.
				if (sign_in && sp_client.status() == vb::net::ClientHandshakeStatus::kSigningIn) {
					sp->tick(dt);
					connect_ticks = 0;
					draw_sign_in();
					break;
				}
#endif
				for (int i = 0; i < 8 && !sp_client.joined() && !sp_client.failed() &&
						connect_ticks < 128;
						++i, ++connect_ticks) {
					sp->tick(0.05);
				}
				if (connect_ticks >= 128 && !sp_client.joined() && !sp_client.failed()) {
					timed_out = true;
				}
				client = &sp_client;
			} else {
				client = &*remote->session;
				client->tick(dt);
				if (std::chrono::steady_clock::now() > connect_deadline) {
					timed_out = true;
				}
			}

			if (client->joined()) {
				enter_playing();
			} else if (client->failed() || timed_out) {
				error_message = client->failed() ? client->failure_reason()
												 : std::string("connection timed out");
				std::cout << "client: join failed: " << error_message << '\n';
				state = AppState::kError;
			} else {
				// Real byte-progress only exists once actually streaming
				// assets, and only for a real remote session (singleplayer
				// has no ClientAssetCache -- both accessors default to 0
				// there, so the total>0 guard alone would suffice, but the
				// status check documents *why* rather than relying on it).
#if defined(VB_WITH_AUTH)
				if (!connecting_singleplayer && sign_in &&
						client->status() == vb::net::ClientHandshakeStatus::kSigningIn) {
					draw_sign_in();
					break;
				}
#endif
				float fraction = -1.0f;
				if (!connecting_singleplayer &&
						client->status() == vb::net::ClientHandshakeStatus::kSyncingAssets) {
					const std::uint64_t total = client->asset_sync_total_bytes();
					if (total > 0) {
						fraction = static_cast<float>(client->asset_sync_received_bytes()) /
								static_cast<float>(total);
					}
				}
				const auto ui = render
						? menu.draw_connecting(connecting_singleplayer
										  ? "Starting singleplayer world..."
										  : connecting_status_text(client->status()),
								  fraction)
						: vb::render::MainMenu::ConnectingResult{};
				if (ui.cancel) {
					sp.reset();
					remote.reset();
					client = nullptr;
					state = AppState::kMenu;
				}
			}
			break;
		}
		case AppState::kLoading: {
			// Phase 7.1: keep pumping the session/server so the initial
			// view-box of chunks actually streams in and gets meshed --
			// this screen isn't a passive wait, it's what's advancing the
			// load. A higher submit/upload budget than kPlaying's steady-
			// state 8/frame (spec: get through this quickly).
			if (connecting_singleplayer) {
				sp->tick(dt);
			} else {
				client->tick(dt);
			}
			if (chunk_renderer) {
				chunk_renderer->sync(client->chunk_store(), /*submit*/ 64, /*upload*/ 16);
			}

			// Expected chunk count mirrors WorldReplicator's own
			// chunks_in_view box (radius=view_distance, vertical
			// radius=3, see src/net/world_replicator.cpp /
			// Singleplayer's own construction above) -- an approximation
			// for a real server (whose own view_distance this client
			// doesn't know ahead of time), clamped to 1.0 below so a
			// smaller real box still reads as "done", not stuck.
			const std::size_t expected = static_cast<std::size_t>(2 * view_distance + 1) *
					static_cast<std::size_t>(2 * view_distance + 1) * 7u;
			// Progress must track what's actually visible on screen, not
			// just chunk data having arrived (client->chunk_store().size()
			// reaches `expected` well before ChunkRenderer has meshed and
			// GPU-uploaded that many chunks, since meshing is async and
			// upload is budgeted -- using store size here let the loading
			// screen hit 100% and hand off to kPlaying while the world
			// behind it was still sky-colored, unmeshed chunks).
			const std::size_t loaded_chunks =
					chunk_renderer ? chunk_renderer->uploaded_count() : 0;
			const float fraction = expected == 0
					? 1.0f
					: static_cast<float>(loaded_chunks) / static_cast<float>(expected);

			std::string operator_title;
			if (const auto &info = client->server_info()) {
				operator_title = info->motd;
			}
			menu.draw_loading(fraction, operator_title);

			const auto now = std::chrono::steady_clock::now();
			if (loaded_chunks > loading_last_uploaded) {
				loading_last_uploaded = loaded_chunks;
				loading_deadline = now + kLoadingStallTimeout;
			}
			if (fraction >= 1.0f || now > loading_deadline ||
					now > loading_hard_deadline) {
				state = AppState::kPlaying;
			}
			break;
		}
		case AppState::kError: {
			const auto result = menu.draw_error(error_message);
			if (result.back) {
				state = AppState::kMenu;
			}
			break;
		}
		case AppState::kPlaying: {
			if (auto opened = client->take_open_ui()) {
				ui_runtime.open(opened->ui_name, opened->ctx_json);
			}

			for (std::string &line : client->take_chat_messages()) {
				chat_log.push_back(std::move(line));
			}
			while (chat_log.size() > kChatLogLimit) {
				chat_log.pop_front();
			}

			if (chat_open) {
				if (input.key_pressed(KEY_ESCAPE)) {
					chat_open = false;
					chat_buf.clear();
				} else if (input.key_pressed(KEY_ENTER) || input.key_pressed(KEY_KP_ENTER)) {
					if (!chat_buf.empty()) {
						VB_OBSERVE(on_chat_sent(chat_buf));
						client->send_chat(chat_buf);
					}
					chat_buf.clear();
					chat_open = false;
				}
			} else if (!ui_runtime.is_open() &&
					(input.key_pressed(KEY_ENTER) || input.key_pressed(KEY_KP_ENTER))) {
				chat_open = true;
			}

			// F5: third-person camera, to see your own appearance (layers,
			// player:set_visual_override). A pack can forbid it
			// (vb.render.set_third_person(false) -> S2CServerInfo).
			const bool third_person_allowed =
					!client->server_info() || client->server_info()->third_person_allowed;
			if (!chat_open && !ui_runtime.is_open() && input.key_pressed(KEY_F5)) {
				third_person = !third_person;
			}
			if (!third_person_allowed) {
				third_person = false;
			}

			// EnableCursor()/DisableCursor() each warp the OS cursor to
			// screen center as a side effect (raylib's
			// rcore_desktop_glfw.c), so they must only fire on the
			// mouse_captured transition, not every frame it holds a
			// value -- calling EnableCursor() every frame the inventory
			// stayed open re-centered the cursor 60+ times a second,
			// making it look stuck in the middle of the screen.
			//
			// A pack can also ask for capture/release (client.capture_mouse,
			// ui.close{ capture_mouse = true }, capture_mouse_on_close). A
			// release applies now; a capture waits until no screen and no
			// chat box need the cursor, and Tab/Escape cancel it.
			const bool user_release = input.key_pressed(KEY_TAB) || input.key_pressed(KEY_ESCAPE);
			bool pack_release = false;
			if (const auto request = ui_runtime.take_capture_request()) {
				pack_release = !*request;
				pending_capture_ = *request;
			}
			if (user_release || pack_release) {
				pending_capture_ = false;
			}
			if (ui_runtime.is_open() || chat_open || user_release || pack_release) {
				if (mouse_captured) {
					mouse_captured = false;
					if (render) {
						EnableCursor();
					}
				}
			} else if (pending_capture_) {
				pending_capture_ = false;
				if (!mouse_captured) {
					mouse_captured = true;
					// The click that closed the screen may still be held: don't
					// let it reach the server as a punch.
					suppress_primary_ = true;
					if (render) {
						DisableCursor();
					}
				}
			} else if (input.mouse_button_pressed(MOUSE_BUTTON_LEFT) && !mouse_captured) {
				mouse_captured = true;
				if (render) {
					DisableCursor();
				}
			}

			// Hotbar selection (entity-management follow-up): keys 1-9 pick
			// inventory slot 0-8. Gated on mouse_captured, same as
			// movement/break/place below, so typing a digit into an open
			// chat box or UI never changes it.
			if (mouse_captured) {
				for (int i = 0; i < 9; ++i) {
					if (input.key_pressed(KEY_ONE + i)) {
						selected_slot = static_cast<std::uint8_t>(i);
						break;
					}
				}
			}

			// Look only — position is authoritative, driven by input commands
			// and corrected by the server via prediction/reconciliation
			// (spec §8.4).
			vb::render::LookMoveInput look_in;
			if (mouse_captured) {
				look_in.look_delta = { input.mouse_dx, input.mouse_dy };
			}
			controller.update(look_in, dt);

			{
				vb::protocol::InputCmd cmd = vb::render::sample_input_cmd(input, ++input_seq, dt,
						controller.yaw(), controller.pitch(), mouse_captured,
						movement_bindings, client->registered_keybinds(),
						selected_slot);
				// After a pack-requested capture: hold back `primary` until the
				// click that closed the screen is released.
				if (suppress_primary_) {
					if (input.mouse_button_down(MOUSE_BUTTON_LEFT)) {
						cmd.buttons = static_cast<std::uint8_t>(
								cmd.buttons & ~vb::protocol::kInputPrimary);
					} else {
						suppress_primary_ = false;
					}
				}
				client->push_input(cmd);
				// Singleplayer ticks the whole embedded game (client + server,
				// over loopback); a real connection just pumps this client's
				// GnsTransport -- the dedicated server ticks itself.
				if (connecting_singleplayer) {
					sp->tick(dt);
				} else {
					client->tick(dt);
				}
				const vb::core::Vec3d feet = client->predicted_feet();
				const double smoothed_eye_y = eye_smoother.update(
						feet.y + move_params.eye_height, dt);
				controller.set_position({ feet.x, smoothed_eye_y, feet.z });
			}

			// Phase 6.17/6.20: neither breaking nor placing is a client-
			// authoritative hardcoded action any more -- the client only
			// reports raw input (buttons.primary/secondary, set above in
			// sample_input_cmd) and does its own raycast purely for the
			// crosshair-highlight visual below; content/base/mechanics.lua
			// decides *when* and *what* (player:break_block()/
			// player:place_block()) server-side, once a pack opts in at
			// all -- neither is an engine default any more.
			vb::world::VoxelRayHit look_hit;
			if (mouse_captured) {
				look_hit = vb::world::raycast_voxel(client->chunk_store(),
						controller.position(), controller.forward(), 5.0);
			}

			// Raw state only -- "engine provides raw state, Lua deals
			// with presentation". client.break_progress() reports the
			// currently-looked-at block's live damage fraction, now that
			// S2C_BlockDamage (Phase 6.5's deferred half, closed
			// 2026-09-27) replicates the server's real punch count for
			// any block a nearby player has damaged -- not just this
			// client's own punches. nullopt whenever nothing's targeted,
			// the block has no damage on record, or it's an instant-break
			// block (max_damage == 0, no fraction to report).
			std::optional<float> break_progress;
			if (look_hit.hit) {
				const auto &damage = client->block_damage();
				if (const auto it = damage.find(look_hit.voxel); it != damage.end()) {
					const vb::core::BlockId block =
							client->chunk_store().block_at(look_hit.voxel);
					const auto &registry = client->chunk_store().registry();
					if (registry.contains(block) &&
							registry.get(block).max_damage > 0) {
						break_progress = static_cast<float>(it->second) /
								static_cast<float>(registry.get(block).max_damage);
					}
				}
			}
			ui_runtime.set_break_progress(break_progress);
			const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
			ui_runtime.set_clock(std::chrono::duration<double>(since_epoch).count());
			if (render) {
				ui_runtime.set_screen_size(GetScreenWidth(), GetScreenHeight());
				const Vector2 mouse = GetMousePosition();
				ui_runtime.set_mouse_position(mouse.x, mouse.y);
			}

			// Same posture, for the 3 pieces of always-on HUD content
			// that used to be drawn directly by this file (Phase 6.16
			// follow-up, closes REMAINING_TASKS' "player list / chat box
			// / hotbar are still hardcoded C++" item) -- content/base/
			// ui/hud.lua now decides how (or whether) to show these.
			{
				std::vector<std::string> other_names;
				other_names.reserve(client->players().size());
				for (const auto &[id, name] : client->players()) {
					(void)id;
					other_names.push_back(name);
				}
				ui_runtime.set_player_list(config.player_name, std::move(other_names));
			}
			ui_runtime.set_chat({ chat_log.begin(), chat_log.end() }, chat_open);
			ui_runtime.set_mouse_captured(mouse_captured);
			if (const auto &st = client->player_status()) {
				ui_runtime.set_player_status(vb::script::UiRuntime::StatusView{
						st->health, st->max_health, st->hunger, st->max_hunger });
			} else {
				ui_runtime.set_player_status(std::nullopt);
			}
			{
				const auto &inv = client->inventory();
				const auto &registry = client->chunk_store().registry();
				std::vector<vb::script::UiRuntime::InventorySlotView> slots;
				slots.reserve(inv.size());
				for (const auto &slot : inv) {
					std::string name = registry.contains(slot.item)
							? registry.get(slot.item).name
							: "?";
					slots.push_back({ std::move(name), slot.count,
							static_cast<std::uint32_t>(slot.item) });
				}
				ui_runtime.set_inventory(std::move(slots), selected_slot + 1);
			}

#if defined(VB_WITH_AUTOMATION)
			// Automation needs the widget lists a windowed client would get as
			// a side effect of drawing; evaluate them without drawing.
			if (!render && headless_ui_eval) {
				if (ui_runtime.is_open()) {
					ui_runtime.render_frame();
				}
				hud_widget_cache = ui_runtime.render_hud();
			}
#endif

			if (!render) {
				break; // everything below is GL drawing
			}

			std::size_t chunk_count = 0;
			std::size_t entity_count = 0;
			if (chunk_renderer) {
				chunk_renderer->sync(client->chunk_store(), /*budget*/ 8);
				chunk_count = chunk_renderer->uploaded_count();
			}
			// The view camera: the eye in first person; in third person pulled
			// back along the look direction (stopping short of solid blocks)
			// and looking at the eye, with the local player drawn.
			const vb::core::Vec3d look_dir = controller.forward();
			vb::core::Vec3d view_pos = controller.position();
			if (third_person) {
				const vb::core::Vec3d eye = controller.position();
				double back = 0.0;
				for (double t = 0.25; t <= kThirdPersonDistance + 1e-9; t += 0.25) {
					const auto cell = [&](double e, double d) {
						return static_cast<int>(std::floor(e - d * t));
					};
					const vb::core::IVec3 v{ cell(eye.x, look_dir.x), cell(eye.y, look_dir.y),
						cell(eye.z, look_dir.z) };
					if (client->chunk_store().solid_at(v)) {
						break;
					}
					back = std::max(0.0, t - 0.2);
				}
				view_pos = { eye.x - look_dir.x * back, eye.y - look_dir.y * back,
					eye.z - look_dir.z * back };
			}
			const vb::core::Vec3d view_target{ view_pos.x + look_dir.x,
				view_pos.y + look_dir.y, view_pos.z + look_dir.z };
			const vb::render::CameraView view{ view_pos, view_target };
			if (entity_renderer) {
				entity_renderer->set_draw_local_player(third_person);
				const vb::render::CameraView camera_view = view;
				entity_renderer->sync(*client, camera_view, dt);
				entity_count = entity_renderer->tracked_count();
			}

			// Day/night sky (spec §5.4): a simple gradient driven by the
			// server's time_of_day clock (S2C_JoinAccept's initial value,
			// kept current by periodic S2C_TimeOfDay updates). Overwrites
			// window.begin_frame()'s flat dark clear for this state only.
			const vb::world::SkyColor sky = vb::world::sky_color_for_time(
					client->time_of_day(), client->day_night_curve());
			ClearBackground(Color{ sky.r, sky.g, sky.b, 255 });

			// Phase 7.2: distance fog, always blending into the same sky
			// color computed above (never an independently drifting
			// tint -- see REMAINING_TASKS.md Phase 7.2). A pack's
			// vb.render.set_fog{start=, end=} overrides the distances;
			// absent that, the default matches this client's own
			// view_distance so fog fades in right around where chunks
			// stop streaming in, rather than at an arbitrary distance.
			if (chunk_renderer) {
				float fog_start;
				float fog_end;
				if (const auto &fog = client->fog_override()) {
					fog_start = fog->fog_start;
					fog_end = fog->fog_end;
				} else {
					fog_end = static_cast<float>(view_distance * vb::core::kChunkDim);
					fog_start = fog_end * 0.6f;
				}
				// A pack's vb.render.set_fog override can name any
				// distance it likes -- nothing about it is checked
				// against how far this client actually keeps chunks
				// loaded. An override longer than that reach would
				// show a hard, unfogged edge right where the world
				// stops rendering instead of the soft fade fog exists
				// to provide; bind the two together by clamping
				// fog_end to the real view distance regardless of
				// source (the engine default above is already exactly
				// at that bound, so this is a no-op for it -- only an
				// override can ever be pulled in). fog_start is
				// clamped to match so it can't end up past a
				// just-lowered fog_end.
				const float max_fog_distance = static_cast<float>(view_distance * vb::core::kChunkDim);
				fog_end = std::min(fog_end, max_fog_distance);
				fog_start = std::min(fog_start, fog_end);
				// Phase 7.3: "underwater" is the same sky-color fog
				// mechanism, just a much closer distance preset -- no
				// separate tint/color system (REMAINING_TASKS.md 7.3's
				// decision). Triggered whenever the camera's own eye
				// voxel is a liquid block, overriding whichever
				// fog_start/fog_end were picked above (default or a
				// pack's vb.render.set_fog override alike) so surfacing
				// always restores normal visibility immediately.
				const vb::core::Vec3d eye = controller.position();
				const vb::core::IVec3 eye_voxel{
					static_cast<int>(std::floor(eye.x)),
					static_cast<int>(std::floor(eye.y)),
					static_cast<int>(std::floor(eye.z))
				};
				const vb::core::BlockId eye_block =
						client->chunk_store().block_at(eye_voxel);
				vb::world::SkyColor fog_color = sky;
				if (client->chunk_store().registry().is_liquid(eye_block)) {
					fog_end = 8.0f;
					fog_start = 2.0f;
					// Phase 7.5: underwater fog defaults to the submerged
					// liquid's own texture's average color instead of
					// echoing the sky -- water should tint the murk
					// itself, not whatever time of day it happens to be.
					// A pack's vb.render.set_fog{underwater_tint=} (7.5's
					// override half) replaces that default outright when
					// present.
					if (const auto &fog = client->fog_override();
							fog && fog->has_underwater_tint) {
						fog_color = vb::world::SkyColor{
							fog->underwater_tint_r, fog->underwater_tint_g,
							fog->underwater_tint_b
						};
					} else {
						const Color tint = chunk_renderer->underwater_tint(eye_block);
						fog_color = vb::world::SkyColor{ tint.r, tint.g, tint.b };
					}
				}
				chunk_renderer->set_fog(controller.position(), fog_color, fog_start, fog_end);
			}

			Camera3D camera = to_camera(controller, fov);
			camera.position = { static_cast<float>(view_pos.x), static_cast<float>(view_pos.y),
				static_cast<float>(view_pos.z) };
			camera.target = { static_cast<float>(view_target.x), static_cast<float>(view_target.y),
				static_cast<float>(view_target.z) };
			BeginMode3D(camera);
			DrawGrid(64, 4.0f);
			if (chunk_renderer) {
				chunk_renderer->draw(camera);
			}
			if (entity_renderer) {
				entity_renderer->draw(view);
			}
			if (look_hit.hit) {
				const Vector3 hit_center{
					static_cast<float>(look_hit.voxel.x) + 0.5f,
					static_cast<float>(look_hit.voxel.y) + 0.5f,
					static_cast<float>(look_hit.voxel.z) + 0.5f
				};
				DrawCubeWires(hit_center, 1.02f, 1.02f, 1.02f, BLACK);
			}
			// Real crack-stage overlay (REMAINING_TASKS.md 6.5, closed
			// 2026-09-27): a textured cube sampling each damaged block's
			// own crack stage from crack_atlas -- its pack-set
			// crack_texture override if one decoded validly at join time,
			// else the engine's shared built-in procedural crack pattern.
			// Stage picks progressively more damaged art as the damage
			// fraction climbs toward 1.0; alpha darkens in step with it.
			// Drawn for every damaged block the server replicates (they
			// stay damaged until healed), not only the one under the
			// crosshair: tying it to the aim made the crack blink on and
			// off whenever the look ray grazed a neighbouring block.
			if (crack_overlay && !client->block_damage().empty()) {
				const auto &registry = client->chunk_store().registry();
				const auto opaque_at = [&](vb::core::IVec3 v) {
					return registry.is_opaque(client->chunk_store().block_at(v));
				};
				crack_overlay->begin();
				for (const auto &[voxel, punches] : client->block_damage()) {
					const vb::core::BlockId block = client->chunk_store().block_at(voxel);
					if (!registry.contains(block) || registry.get(block).max_damage == 0) {
						continue;
					}
					const Vector3 center{ static_cast<float>(voxel.x) + 0.5f,
						static_cast<float>(voxel.y) + 0.5f,
						static_cast<float>(voxel.z) + 0.5f };
					const float dx = center.x - camera.position.x;
					const float dy = center.y - camera.position.y;
					const float dz = center.z - camera.position.z;
					if (dx * dx + dy * dy + dz * dz > kCrackDrawDistance * kCrackDrawDistance) {
						continue;
					}
					const float fraction = vb::core::clamp(static_cast<float>(punches) /
									static_cast<float>(registry.get(block).max_damage),
							0.0f, 1.0f);
					const int stage = std::min(
							static_cast<int>(fraction * vb::render::CrackAtlas::kStages),
							vb::render::CrackAtlas::kStages - 1);
					using Face = vb::render::CrackOverlay::Face;
					struct Side {
						vb::core::IVec3 offset;
						Face face;
					};
					static constexpr Side kSides[] = {
						{ { 1, 0, 0 }, Face::kPosX },
						{ { -1, 0, 0 }, Face::kNegX },
						{ { 0, 1, 0 }, Face::kPosY },
						{ { 0, -1, 0 }, Face::kNegY },
						{ { 0, 0, 1 }, Face::kPosZ },
						{ { 0, 0, -1 }, Face::kNegZ },
					};
					std::uint8_t hidden = 0;
					for (const Side &side : kSides) {
						if (opaque_at({ voxel.x + side.offset.x, voxel.y + side.offset.y,
									voxel.z + side.offset.z })) {
							hidden = static_cast<std::uint8_t>(hidden | side.face);
						}
					}
					const auto alpha = static_cast<unsigned char>(fraction * 220.0f + 35.0f);
					crack_overlay->draw(center, camera.position,
							crack_atlas.rect_for(block, stage), alpha, hidden);
				}
				crack_overlay->end();
			}
			if (entity_renderer) {
				entity_renderer->draw_labels(view);
			}
			EndMode3D();
			draw_overlay(controller, status, chunk_count, entity_count,
					mouse_captured, client->time_of_day());

			// The HUD (spec §5.4-adjacent, Phase 6.16): an always-on,
			// pack-defined overlay drawn every frame regardless of
			// whether a modal ui_runtime screen is also open. Today this
			// is how the hold-to-break progress bar is drawn --
			// content/base/ui/hud.lua reads client.break_progress() (set
			// above) and decides whether/how to show it; the engine
			// itself no longer draws a single pixel of it.
			const auto hud_result =
					hud_renderer.draw("hud", ui_runtime.render_hud(), chunk_renderer.get());
			for (const auto &id : hud_result.clicked) {
				VB_OBSERVE(on_ui_click(id, true));
				ui_runtime.report_hud_click(id);
			}
			for (const auto &[id, text] : hud_result.changed_text) {
				VB_OBSERVE(on_ui_change(id, text, true));
				ui_runtime.report_hud_change(id, text);
			}
			for (const auto &[id, idx] : hud_result.changed_list) {
				VB_OBSERVE(on_ui_list(id, idx, true));
				ui_runtime.report_hud_list_change(id, idx);
			}

			// The player list, chat log, and hotbar used to be drawn
			// directly here; they're now content/base/ui/hud.lua widgets
			// (client.players()/client.chat_log()/client.inventory(),
			// set above), drawn by the render_hud() call right above.
			// Only the chat *input box* stays here: real keyboard
			// text-entry capture, plain raygui like MainMenu, never a
			// UiRuntime widget (spec §5.4's own deliberate scope).
			if (chat_open) {
				const int line_h = 18;
				const int box_bottom = GetScreenHeight() - 16;
				chat_buf.resize(kChatBufferSize, '\0');
				GuiTextBox(Rectangle{ 12.0f, static_cast<float>(box_bottom - line_h),
								   360.0f, static_cast<float>(line_h + 4) },
						chat_buf.data(), kChatBufferSize, true);
				chat_buf.resize(std::strlen(chat_buf.c_str()));
			}

			if (ui_runtime.is_open()) {
				const auto &widgets = ui_runtime.render_frame();
				const auto ui_result = ui_renderer.draw(
						ui_runtime.current_name(), widgets, chunk_renderer.get());
				for (const auto &id : ui_result.clicked) {
					VB_OBSERVE(on_ui_click(id, false));
					ui_runtime.report_click(id);
				}
				for (const auto &[id, text] : ui_result.changed_text) {
					VB_OBSERVE(on_ui_change(id, text, false));
					ui_runtime.report_change(id, text);
				}
				for (const auto &[id, idx] : ui_result.changed_list) {
					VB_OBSERVE(on_ui_list(id, idx, false));
					ui_runtime.report_list_change(id, idx);
				}
			}
#if defined(VB_WITH_AUTH)
			draw_reauth_prompt();
#endif
			break;
		}
	}

#if defined(VB_WITH_AUTOMATION)
	if (screenshot_state_ == Screenshot::kPending) {
		// Before end_frame(): the frame has been drawn but not yet swapped out. raylib
		// batches draw calls until EndDrawing(), so flush the batch first or the read
		// returns only the clear colour. (raylib's TakeScreenshot() is avoided: it strips
		// the directory from the path.)
		rlDrawRenderBatchActive();
		Image shot = LoadImageFromScreen();
		screenshot_w_ = shot.width;
		screenshot_h_ = shot.height;
		screenshot_state_ = (shot.data != nullptr && ExportImage(shot, screenshot_path_.c_str()))
				? Screenshot::kDone
				: Screenshot::kFailed;
		UnloadImage(shot);
	}
#endif

	window.end_frame();
	return true;
}

} // namespace vb::client

#if defined(VB_WITH_AUTOMATION)
void vb::client::ClientApp::submit_chat(std::string_view text) {
	if (client != nullptr && !text.empty()) {
		VB_OBSERVE(on_chat_sent(text));
		client->send_chat(text);
	}
}

void vb::client::ClientApp::ui_click(const std::string &id, bool hud) {
	VB_OBSERVE(on_ui_click(id, hud));
	hud ? ui_runtime.report_hud_click(id) : ui_runtime.report_click(id);
}

void vb::client::ClientApp::ui_change(const std::string &id, const std::string &text, bool hud) {
	VB_OBSERVE(on_ui_change(id, text, hud));
	hud ? ui_runtime.report_hud_change(id, text) : ui_runtime.report_change(id, text);
}

void vb::client::ClientApp::ui_list(const std::string &id, int index, bool hud) {
	VB_OBSERVE(on_ui_list(id, index, hud));
	hud ? ui_runtime.report_hud_list_change(id, index) : ui_runtime.report_list_change(id, index);
}

bool vb::client::ClientApp::type_chat(std::string_view text) {
	if (state != AppState::kPlaying || ui_runtime.is_open()) {
		return false; // a player can't type into the chat box here either
	}
	chat_open = true;
	if (chat_buf.size() + text.size() >= static_cast<std::size_t>(kChatBufferSize)) {
		return false; // would overflow the box's fixed buffer
	}
	chat_buf.append(text);
	return true;
}

bool vb::client::ClientApp::request_screenshot(std::string path) {
	if (!render || screenshot_state_ == Screenshot::kPending) {
		return false;
	}
	screenshot_path_ = std::move(path);
	screenshot_state_ = Screenshot::kPending;
	return true;
}
#endif
