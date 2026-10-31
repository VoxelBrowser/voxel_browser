// voxel_browser — the client ("browser").
//
// Phase 5.3: an engine-level main menu (raylib window always opens first) —
// connect screen, connecting/progress screen, error screen, settings screen,
// recent servers — wraps the streamed/meshed voxel gameplay from earlier
// phases. `--loopback` (alias `--singleplayer`) runs an in-process server (worldgen + replication)
// over a loopback transport and joins it (spec §3 — one code path for single-
// and multiplayer). `--headless` skips the menu entirely and connects
// immediately from CLI args/config (CI and integration tests depend on this
// exact behaviour, see `singleplayer_smoke`/`client_smoke`).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <raygui.h>
#include <raylib.h>

#include "vb/assetsync/cache.hpp"
#include "vb/core/build_info.hpp"
#include "vb/core/cli.hpp"
#include "vb/core/config.hpp"
#include "vb/core/ids.hpp" // kChunkDim
#include "vb/core/paths.hpp"
#include "vb/net/gns_transport.hpp"
#include "vb/net/loopback.hpp"
#include "vb/net/session.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/physics/movement.hpp"
#include "vb/protocol/input.hpp"
#include "vb/protocol/world.hpp"
#include "vb/render/camera.hpp"
#include "vb/render/chunk_renderer.hpp"
#include "vb/render/crack_atlas.hpp"
#include "vb/render/crack_overlay.hpp"
#include "vb/render/entity_renderer.hpp"
#include "vb/render/input.hpp"
#include "vb/render/main_menu.hpp"
#include "vb/render/ui_renderer.hpp"
#include "vb/render/window.hpp"
#include "vb/script/pack_loader.hpp"
#include "vb/script/pack_runtime.hpp"
#include "vb/script/ui_runtime.hpp"
#include "vb/world/block.hpp"
#include "vb/world/daynight.hpp"
#include "vb/world/raycast.hpp"
#include "vb/world/region_store.hpp"
#include "vb/world/world.hpp"
#include "vb/worldgen/generator.hpp"
#include "vb/worldgen/worker_pool.hpp"

#include "client_app.hpp"

#if defined(VB_WITH_AUTOMATION)
#include "automation_endpoint.hpp"
#include "recorder.hpp"
#include "vb/automation/host.hpp"
#endif

using namespace vb::client;

namespace {

void print_usage() {
	std::cout << "Usage: voxel_browser [options]\n"
				 "\n"
				 "  --config <path>   client.toml to load (default client.toml)\n"
				 "  --server <addr>   server address to connect to (default 127.0.0.1)\n"
				 "  --port <n>        server port (default 27015)\n"
				 "  --name <name>     player name override\n"
				 "  --fov <deg>       vertical field of view override\n"
				 "  --render-distance <n>  view distance override (chunks)\n"
				 "  --asset-cache-dir <path>  asset cache directory override\n"
				 "  --content-pack <dir>  --loopback: content pack to load (default content/base,\n"
				 "                        else <exe dir>/content/base)\n"
				 "  --world-dir <dir>     --loopback: where the world is saved (default world_singleplayer)\n"
				 "  --headless        run without a window (no rendering, skips the menu)\n"
				 "  --frames <n>      headless: run n frames then exit (default 3)\n"
#if defined(VB_WITH_AUTOMATION)
				 "  --automation stdio|tcp[:PORT]  drive via JSON lines on stdin/stdout, or on a token-protected\n"
				 "                    127.0.0.1-only socket (dev builds; windowed needs a display, e.g. xvfb-run)\n"
				 "  --automation-token <t>    fixed token for tcp (default: random)\n"
				 "  --automation-info <file>  write {host,port,token,pid} here for tcp (default: print to stderr)\n"
				 "  --automation-clock <real|manual>  manual: frames advance only on `step`\n"
				 "  --net-sim <spec>  fake lag/jitter/loss on sent packets, e.g. lag_ms=100,loss_pct=2 (dev builds only)\n"
				 "  --automation-record <file.py>  write what you do as a vbtest script (dev builds only)\n"
				 "  --auth-token-file <file>  sign in with the ID token in this file instead of the UI (dev builds only)\n"
#endif
#if !defined(VB_DISTRIBUTION)
				 "  --insecure-skip-auth  DEV ONLY: --loopback ignores the pack's auth.lua (no sign-in)\n"
				 "  --ignore-engine-req   DEV ONLY: --loopback loads the pack despite its engine_version_req\n"
#endif
				 "  --version        print build info and exit\n"
				 "  --help           show this help\n"
				 "\n"
				 "Diagnostics:\n"
				 "  --loopback        run an in-process server and join it over a loopback link. Isolates\n"
				 "                    rendering, worldgen, pack Lua, physics and saves from the network:\n"
				 "                    bypasses GNS, asset sync and compression negotiation. Not a play\n"
				 "                    mode (--singleplayer is a deprecated alias)\n"
				 "\n"
				 "Without --headless a window opens to the main menu; --loopback or\n"
				 "--server on the command line skips the menu and connects immediately\n"
				 "(dropping back to the menu on failure instead of exiting).\n";
}

// --headless (CI / integration-test smoke path): the same ClientApp the
// windowed client runs, with render=false -- no menu, GL resources or draws.
#if defined(VB_WITH_AUTOMATION)
// Development-only: same ClientApp, but driven by the automation channel
// instead of a fixed frame count. Runs until `quit` or the harness closes stdin.
int run_automated(ClientApp &app, vb::automation::Host &host, vb::render::Window *window = nullptr,
		vb::client::Recorder *recorder = nullptr) {
	ClientAutomationEndpoint endpoint(app, host);
	using Clock = std::chrono::steady_clock;
	constexpr auto kFrame = std::chrono::microseconds(16667);
	auto next = Clock::now();
	while (host.pump(endpoint) && (window == nullptr || !window->should_close())) {
		if (!host.frame_allowed()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		// Actions queue this frame's synthetic input, the real ClientApp frame
		// consumes it, then finished actions answer their requests.
		endpoint.begin_frame();
		const vb::render::InputFrame input = endpoint.input().poll();
		const bool keep_running = app.frame(input, 1.0 / 60.0);
		if (recorder != nullptr) {
			recorder->observe_frame(app, input);
		}
		endpoint.end_frame();
		if (!keep_running) {
			break;
		}
		host.frame_done();
		if (!host.manual_clock()) {
			next += kFrame;
			std::this_thread::sleep_until(next);
		}
	}
	if (recorder != nullptr) {
		recorder->write();
	}
	std::cout << "client: automation session ended\n";
	return EXIT_SUCCESS;
}
#endif

int run_headless(const vb::core::ClientConfig &config, const vb::core::Args &args,
		const std::string &server, int port, bool singleplayer, int view_distance,
		const std::optional<std::filesystem::path> &auth_token_file
#if defined(VB_WITH_AUTOMATION)
		,
		vb::automation::Host *automation, vb::client::Recorder *recorder
#endif
) {
	vb::render::WindowConfig wcfg;
	wcfg.headless = true;
	wcfg.width = static_cast<int>(config.window_width);
	wcfg.height = static_cast<int>(config.window_height);
	wcfg.vsync = config.vsync;
	wcfg.title = "voxel_browser";
	wcfg.headless_frame_limit = static_cast<std::uint64_t>(args.int_or("frames", 3));
	vb::render::Window window(wcfg);

	ClientApp app(config, "", window, server, port, view_distance, singleplayer,
			/*render*/ false, auth_token_file, args.value_or("name", ""));
	if (!app.connect_blocking()) {
		return EXIT_FAILURE;
	}
#if defined(VB_WITH_AUTOMATION)
	if (automation != nullptr) {
		app.set_observer(recorder);
		return run_automated(app, *automation, nullptr, recorder);
	}
#endif
	while (!window.should_close()) {
		app.frame(vb::render::InputFrame{}, 1.0 / 60.0);
	}
	std::cout << "client: exited after " << window.frame_count() << " frames\n";
	return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
	const vb::core::Args args(argc, argv);

	if (args.has("help", 'h')) {
		print_usage();
		return EXIT_SUCCESS;
	}
	if (args.has("version", 'v')) {
		std::cout << vb::core::describe_build() << '\n';
		return EXIT_SUCCESS;
	}

	const bool loopback_requested = args.has("loopback") || args.has("singleplayer");
	if (args.has("singleplayer")) {
		std::cerr << "client: --singleplayer is deprecated, use --loopback (a netcode-isolation diagnostic)\n";
	}

	// An explicit --content-pack is taken literally; the default also looks
	// beside the executable (see kSingleplayerContentPack's comment).
	if (const auto pack = args.value("content-pack"); pack && !pack->empty()) {
		kSingleplayerContentPack = *pack;
	} else {
		kSingleplayerContentPack =
				vb::core::resolve_beside_program(kSingleplayerContentPack, args.program()).string();
	}
	kSingleplayerWorldDir = args.value_or("world-dir", kSingleplayerWorldDir);
	if (args.has("insecure-skip-auth")) {
#if defined(VB_DISTRIBUTION)
		std::cerr << "client: --insecure-skip-auth is not available in this build\n";
		return EXIT_FAILURE;
#else
		// Dev only: --loopback ignores the pack's auth.lua (get_login() is nil).
		kSingleplayerSkipAuth = true;
#endif
	}

	if (args.has("ignore-engine-req")) {
#if defined(VB_DISTRIBUTION)
		std::cerr << "client: --ignore-engine-req is not available in this build\n";
		return EXIT_FAILURE;
#else
		// Dev only: --loopback loads the pack despite its engine_version_req.
		kSingleplayerIgnoreEngineReq = true;
#endif
	}

	std::optional<std::filesystem::path> auth_token_file; // set below, automation builds only
#if defined(VB_WITH_AUTOMATION)
	std::unique_ptr<vb::automation::Host> automation;
	if (args.has("automation")) {
		const std::string clock = args.value_or("automation-clock", "real");
		if (clock != "real" && clock != "manual") {
			std::cerr << "client: --automation-clock must be 'real' or 'manual'\n";
			return EXIT_FAILURE;
		}
		std::string automation_error;
		automation = vb::automation::Host::open_spec(args.value_or("automation", ""),
				args.value_or("automation-token", ""), args.value_or("automation-info", ""), automation_error);
		if (!automation) {
			std::cerr << "client: " << automation_error << '\n';
			return EXIT_FAILURE;
		}
		automation->set_manual_clock(clock == "manual");
	}
	if (args.has("net-sim")) {
		std::string error;
		const auto sim = vb::net::parse_net_sim(args.value_or("net-sim", ""), error);
		if (!sim) {
			std::cerr << "client: " << error << '\n';
			return EXIT_FAILURE;
		}
		if (loopback_requested) {
			std::cerr << "client: --net-sim applies to real (GNS) connections, not --loopback\n";
			return EXIT_FAILURE;
		}
		if (!vb::net::GnsTransport::set_net_sim(*sim)) {
			std::cerr << "client: --net-sim needs a build with VB_WITH_NET\n";
			return EXIT_FAILURE;
		}
	}
	// Headless/automation sign-in (auth.md §7): supply the ID token directly,
	// re-read for every sign-in. Dev/CI only, so it is compiled out with the
	// rest of the automation surface.
	if (args.has("auth-token-file")) {
		const std::string path = args.value_or("auth-token-file", "");
		if (path.empty()) {
			std::cerr << "client: --auth-token-file needs a path\n";
			return EXIT_FAILURE;
		}
		auth_token_file = std::filesystem::path(path);
	}
	std::unique_ptr<vb::client::Recorder> recorder;
	if (args.has("automation-record")) {
		const std::string path = args.value_or("automation-record", "");
		if (path.empty()) {
			std::cerr << "client: --automation-record needs an output path\n";
			return EXIT_FAILURE;
		}
		if (args.has("headless") && !automation) {
			std::cerr << "client: --automation-record needs a window (or --automation): there is nothing to watch\n";
			return EXIT_FAILURE;
		}
		recorder = std::make_unique<vb::client::Recorder>(path);
		if (!recorder->write()) {
			std::cerr << "client: cannot write '" << path << "'\n";
			return EXIT_FAILURE;
		}
	}
#else
	if (args.has("automation") || args.has("net-sim") || args.has("automation-record") ||
			args.has("auth-token-file")) {
		// Never silently ignored: a misconfigured test setup must fail loudly.
		std::cerr << "client: built without VB_WITH_AUTOMATION\n";
		return EXIT_FAILURE;
	}
#endif

	const std::string config_path = args.value_or("config", "client.toml");
	auto loaded = vb::core::load_client_config(config_path);
	if (!loaded) {
		std::cerr << "client: bad config: " << vb::core::message(loaded.error())
				  << '\n';
		return EXIT_FAILURE;
	}
	vb::core::ClientConfig config = *loaded;
	vb::core::apply_cli_overrides(config, args);

	const std::string cli_server = args.value_or("server", "127.0.0.1");
	const int cli_port = args.int_or("port", 27015);
	const bool singleplayer = loopback_requested;
	const bool headless = args.has("headless") || VB_HEADLESS_DEFAULT;
	// The player's own preference (Settings screen / client.toml), never
	// mutated. `view_distance` below starts equal to it but may be clamped
	// down per-connection once a real server's own (possibly smaller) view
	// distance is known -- see begin_connect/enter_playing in the windowed
	// path. Singleplayer always uses this unclamped value: the client IS
	// the server there, so there's nothing external to clamp against.
	const int configured_view_distance =
			static_cast<int>(config.render_distance < 2 ? 2 : config.render_distance);

	std::cout << vb::core::describe_build() << '\n'
			  << "client: " << (headless ? "headless" : "windowed") << " mode\n";

	if (headless) {
		return run_headless(config, args, cli_server, cli_port, singleplayer, configured_view_distance,
				auth_token_file
#if defined(VB_WITH_AUTOMATION)
				,
				automation.get(), recorder.get()
#endif
		);
	}

	// --- windowed: main menu first (spec §5.3) -----------------------------

	vb::render::WindowConfig wcfg;
	wcfg.headless = false;
	wcfg.width = static_cast<int>(config.window_width);
	wcfg.height = static_cast<int>(config.window_height);
	wcfg.vsync = config.vsync;
	wcfg.title = "voxel_browser";
	vb::render::Window window(wcfg);

	std::optional<bool> auto_connect;
	if (singleplayer || args.has("server")) {
		auto_connect = singleplayer;
	}
	ClientApp app(config, config_path, window, cli_server, cli_port,
			configured_view_distance, auto_connect, /*render*/ true, auth_token_file, args.value_or("name", ""));
	vb::render::RaylibInput raylib_input;

#if defined(VB_WITH_AUTOMATION)
	if (automation) {
		// A windowed client driven over the channel: starts on the menu (or connects
		// straight away with --server/--loopback) and takes synthetic input.
		app.set_observer(recorder.get());
		return run_automated(app, *automation, &window, recorder.get());
	}
	app.set_observer(recorder.get());
#endif
	while (!window.should_close()) {
		const vb::render::InputFrame input = raylib_input.poll();
		const double dt = static_cast<double>(GetFrameTime());
		const bool keep_running = app.frame(input, dt);
#if defined(VB_WITH_AUTOMATION)
		if (recorder) {
			recorder->observe_frame(app, input);
		}
#endif
		if (!keep_running) {
			break;
		}
	}
#if defined(VB_WITH_AUTOMATION)
	if (recorder) {
		recorder->write();
		std::cout << "client: recorded " << recorder->steps().size() << " steps to "
				  << args.value_or("automation-record", "") << '\n';
		return EXIT_SUCCESS;
	}
#endif

	std::cout << "client: exited after " << window.frame_count() << " frames\n";
	return EXIT_SUCCESS;
}
