// vb_loadtest -- real-UDP server capacity benchmark (development tool).
//
// Spawns N headless bot players in this one process, each with its own
// GnsTransport connection to a running voxel_browser_server, and drives them
// like real players: join, then walk around sending a movement input every
// tick. Reports how many got in, how long joining took, round-trip time and
// per-bot bandwidth, plus how long this process's own tick took (if that
// gets near the tick period, the bot process -- not the server -- is the
// bottleneck and the numbers stop meaning much).
//
// The server's own side (tick work time, achieved tick rate) comes from its
// --status-file; scripts/loadtest.py runs both and steps N up to find where
// the server stops keeping up. See docs/loadtest.md.
//
// Bots skip asset download (ClientSession with no asset cache), so per-bot
// numbers cover gameplay traffic only, not a first join's pack download.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "vb/core/cli.hpp"
#include "vb/core/log.hpp"
#include "vb/net/gns_transport.hpp"
#include "vb/net/session.hpp"
#include "vb/net/transport.hpp"
#include "vb/protocol/input.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using vb::net::ConnId;
using vb::net::TransportEvent;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// Forwards to a real transport and counts bytes both ways. ClientSession only
// ever sees the Transport interface, so this sits transparently in between.
class CountingTransport final : public vb::net::Transport {
public:
	explicit CountingTransport(vb::net::Transport &inner) : inner_(inner) {}

	vb::core::Status<vb::core::NetError> listen(std::uint16_t port) override {
		return inner_.listen(port);
	}
	vb::core::Result<ConnId, vb::core::NetError> connect(std::string_view host,
			std::uint16_t port) override {
		return inner_.connect(host, port);
	}
	void send(ConnId conn, vb::protocol::Lane lane,
			std::span<const std::byte> frame) override {
		tx_bytes += frame.size();
		inner_.send(conn, lane, frame);
	}
	void close(ConnId conn, std::string_view reason) override { inner_.close(conn, reason); }
	void poll(std::vector<TransportEvent> &out) override {
		const std::size_t first = out.size();
		inner_.poll(out);
		for (std::size_t i = first; i < out.size(); ++i) {
			if (out[i].kind == TransportEvent::Kind::kMessage) {
				rx_bytes += out[i].frame.size();
			} else if (out[i].kind == TransportEvent::Kind::kDisconnected) {
				disconnected = true;
				disconnect_reason = out[i].reason;
			}
		}
	}
	bool is_server() const override { return inner_.is_server(); }
	std::size_t connection_count() const override { return inner_.connection_count(); }
	std::optional<std::string> remote_address(ConnId conn) const override {
		return inner_.remote_address(conn);
	}
	std::optional<double> round_trip_time_seconds(ConnId conn) const override {
		return inner_.round_trip_time_seconds(conn);
	}

	std::uint64_t rx_bytes = 0;
	std::uint64_t tx_bytes = 0;
	bool disconnected = false;
	std::string disconnect_reason;

private:
	vb::net::Transport &inner_;
};

struct Bot {
	std::string name;
	std::unique_ptr<vb::net::GnsTransport> gns;
	std::unique_ptr<CountingTransport> counter;
	ConnId conn{};
	std::unique_ptr<vb::net::ClientSession> session;
	Clock::time_point connect_at;
	std::optional<double> join_seconds;
	bool failed = false;
	std::string failure;
	std::uint32_t input_seq = 0;
	float yaw = 0.0f;
	double next_turn = 0.0;
	// Bytes at the start of the measurement window (once everyone is in).
	std::uint64_t rx_mark = 0;
	std::uint64_t tx_mark = 0;
};

double percentile(std::vector<double> v, double p) {
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	const auto idx = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
	return v[idx];
}

void usage() {
	std::cout << "vb_loadtest -- connect N bot players to a running voxel_browser_server over real UDP\n\n"
				 "  --host <addr>        server address (default 127.0.0.1)\n"
				 "  --port <n>           server UDP port (default 27015)\n"
				 "  --bots <n>           number of bots (default 16)\n"
				 "  --ramp <n>           bots connected per second while ramping up (default 20)\n"
				 "  --duration <s>       seconds to keep all bots playing after the ramp (default 30)\n"
				 "  --join-timeout <s>   give up on bots not joined this long after the ramp (default 30)\n"
				 "  --tick-rate <hz>     bot input rate, match the server's tick_rate (default 20)\n"
				 "  --idle               stand still instead of walking (walking spreads bots out,\n"
				 "                       so the server has to generate and stream more terrain)\n"
				 "  --name-prefix <s>    bot name prefix (default bot)\n"
				 "  --seed <n>           RNG seed for walk directions (default 1)\n"
				 "  --json <path>        also write the summary as JSON here\n"
				 "  --quiet              no per-second progress lines\n";
}

} // namespace

int main(int argc, char **argv) {
	const vb::core::Args args(argc, argv);
	if (args.has("help", 'h')) {
		usage();
		return EXIT_SUCCESS;
	}
	vb::core::log::set_level(vb::core::LogLevel::kWarn);

	const std::string host = args.value_or("host", "127.0.0.1");
	const auto port = static_cast<std::uint16_t>(args.int_or("port", 27015));
	const int bot_count = std::max(1, args.int_or("bots", 16));
	const int ramp_per_second = std::max(1, args.int_or("ramp", 20));
	const int duration_s = std::max(1, args.int_or("duration", 30));
	const int join_timeout_s = std::max(1, args.int_or("join-timeout", 30));
	const int tick_rate = std::clamp(args.int_or("tick-rate", 20), 1, 240);
	const bool walk = !args.has("idle");
	const std::string prefix = args.value_or("name-prefix", "bot");
	const auto seed = static_cast<std::uint32_t>(args.int_or("seed", 1));
	const std::string json_path = args.value_or("json", "");
	const bool quiet = args.has("quiet");

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	const double dt = 1.0 / tick_rate;
	const auto tick_period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(dt));
	std::mt19937 rng(seed);
	std::uniform_real_distribution<float> any_yaw(0.0f, 360.0f);
	std::uniform_real_distribution<double> turn_after(2.0, 6.0);

	std::vector<std::unique_ptr<Bot>> bots;
	bots.reserve(static_cast<std::size_t>(bot_count));

	const auto start = Clock::now();
	auto next = start;
	double sim_time = 0.0;
	std::optional<double> all_joined_at; // sim time when the last bot joined/failed
	std::optional<double> measure_from;
	std::vector<double> self_tick_ms;
	double last_report = 0.0;
	bool connect_error = false;

	enum class Phase { kRamp,
		kPlay,
		kDone } phase = Phase::kRamp;
	double ramp_ended_at = 0.0;

	while (!g_stop && phase != Phase::kDone) {
		const auto work_start = Clock::now();

		// Ramp: connect bots at the requested rate.
		const auto wanted = std::min<int>(bot_count,
				static_cast<int>(std::floor(sim_time * ramp_per_second)) + 1);
		while (static_cast<int>(bots.size()) < wanted) {
			auto b = std::make_unique<Bot>();
			b->name = prefix + std::to_string(bots.size());
			b->gns = std::make_unique<vb::net::GnsTransport>();
			b->counter = std::make_unique<CountingTransport>(*b->gns);
			auto conn = b->counter->connect(host, port);
			if (!conn) {
				std::cerr << "vb_loadtest: connect failed: " << vb::core::message(conn.error()) << '\n';
				connect_error = true;
				break;
			}
			b->conn = *conn;
			vb::net::HandshakeClientConfig cfg;
			cfg.player_name = b->name;
			cfg.client_version = "vb_loadtest";
			cfg.client_nonce = rng();
			b->session = std::make_unique<vb::net::ClientSession>(*b->counter, b->conn, cfg);
			b->connect_at = Clock::now();
			b->yaw = any_yaw(rng);
			b->next_turn = sim_time + turn_after(rng);
			bots.push_back(std::move(b));
		}
		if (connect_error) {
			break;
		}

		for (auto &b : bots) {
			if (b->failed) {
				continue;
			}
			const bool was_joined = b->session->joined();
			if (was_joined) {
				if (walk && sim_time >= b->next_turn) {
					b->yaw = any_yaw(rng);
					b->next_turn = sim_time + turn_after(rng);
				}
				vb::protocol::InputCmd cmd;
				cmd.seq = ++b->input_seq;
				cmd.dt = static_cast<float>(dt);
				cmd.move = walk ? vb::core::Vec3f{ 0.0f, 0.0f, 1.0f } : vb::core::Vec3f{};
				cmd.yaw = b->yaw;
				b->session->push_input(cmd);
			}
			b->session->tick(dt);
			if (!was_joined && b->session->joined()) {
				b->join_seconds = std::chrono::duration<double>(Clock::now() - b->connect_at).count();
				if (const auto &ja = b->session->join_accept()) {
					b->session->set_local_feet(ja->spawn_pos);
				}
			}
			if (b->session->failed() || b->counter->disconnected) {
				b->failed = true;
				b->failure = !b->session->failure_reason().empty() ? b->session->failure_reason()
						: !b->counter->disconnect_reason.empty()   ? b->counter->disconnect_reason
																   : "disconnected";
			}
		}

		const auto settled = std::count_if(bots.begin(), bots.end(),
				[](const auto &b) { return b->failed || b->session->joined(); });
		const auto joined = std::count_if(bots.begin(), bots.end(),
				[](const auto &b) { return !b->failed && b->session->joined(); });

		if (phase == Phase::kRamp && static_cast<int>(bots.size()) == bot_count) {
			if (!all_joined_at && settled == bot_count) {
				all_joined_at = sim_time;
			}
			if (ramp_ended_at == 0.0) {
				ramp_ended_at = sim_time;
			}
			if (all_joined_at || sim_time - ramp_ended_at >= join_timeout_s) {
				phase = Phase::kPlay;
				measure_from = sim_time;
				for (auto &b : bots) {
					b->rx_mark = b->counter->rx_bytes;
					b->tx_mark = b->counter->tx_bytes;
				}
			}
		} else if (phase == Phase::kPlay && sim_time - *measure_from >= duration_s) {
			phase = Phase::kDone;
		}

		const double work_ms = std::chrono::duration<double, std::milli>(Clock::now() - work_start).count();
		if (phase == Phase::kPlay) {
			self_tick_ms.push_back(work_ms);
		}

		if (!quiet && sim_time - last_report >= 1.0) {
			last_report = sim_time;
			std::cout << std::fixed << std::setprecision(1) << "[" << std::setw(6) << sim_time << "s] "
					  << (phase == Phase::kRamp ? "ramp" : "play") << "  connected " << bots.size()
					  << "/" << bot_count << "  joined " << joined << "  failed "
					  << std::count_if(bots.begin(), bots.end(), [](const auto &b) { return b->failed; })
					  << "  bot-tick " << std::setprecision(2) << work_ms << " ms" << std::defaultfloat
					  << std::endl;
		}

		sim_time += dt;
		next += tick_period;
		const auto now = Clock::now();
		if (now - next > 5 * tick_period) {
			next = now; // fell behind: don't replay a backlog of ticks back to back
		}
		std::this_thread::sleep_until(next);
	}

	// --- summary --------------------------------------------------------
	const double window = measure_from ? sim_time - *measure_from : 0.0;
	std::vector<double> join_s;
	std::vector<double> rtt_ms;
	std::vector<double> rx_kbps;
	std::vector<double> tx_kbps;
	std::map<std::string, int> failures;
	int joined = 0;
	for (const auto &b : bots) {
		if (b->join_seconds) {
			join_s.push_back(*b->join_seconds);
		}
		if (b->failed) {
			++failures[b->failure];
			continue;
		}
		if (!b->session->joined()) {
			++failures["still joining at end"];
			continue;
		}
		++joined;
		if (auto rtt = b->counter->round_trip_time_seconds(b->conn)) {
			rtt_ms.push_back(*rtt * 1000.0);
		}
		if (window > 0.0) {
			rx_kbps.push_back(static_cast<double>(b->counter->rx_bytes - b->rx_mark) / 1024.0 / window);
			tx_kbps.push_back(static_cast<double>(b->counter->tx_bytes - b->tx_mark) / 1024.0 / window);
		}
	}
	auto mean = [](const std::vector<double> &v) {
		double s = 0.0;
		for (double x : v) {
			s += x;
		}
		return v.empty() ? 0.0 : s / static_cast<double>(v.size());
	};
	const double total_rx_kbps = mean(rx_kbps) * static_cast<double>(rx_kbps.size());

	std::ostringstream os;
	os << std::fixed << std::setprecision(2);
	os << "\n== vb_loadtest summary ==\n"
	   << "bots requested     " << bot_count << "\n"
	   << "bots joined        " << joined << "\n";
	for (const auto &[reason, n] : failures) {
		os << "  not in: " << n << " x \"" << reason << "\"\n";
	}
	os << "join time (s)      p50 " << percentile(join_s, 0.5) << "  p95 " << percentile(join_s, 0.95)
	   << "  max " << percentile(join_s, 1.0) << "\n"
	   << "rtt (ms)           p50 " << percentile(rtt_ms, 0.5) << "  p95 " << percentile(rtt_ms, 0.95)
	   << "  max " << percentile(rtt_ms, 1.0) << "\n"
	   << "per-bot rx (KiB/s) mean " << mean(rx_kbps) << "  max " << percentile(rx_kbps, 1.0) << "\n"
	   << "per-bot tx (KiB/s) mean " << mean(tx_kbps) << "\n"
	   << "server tx total    " << total_rx_kbps << " KiB/s (sum of bot rx)\n"
	   << "measured window    " << window << " s\n"
	   << "bot-process tick   p50 " << percentile(self_tick_ms, 0.5) << " ms  p95 "
	   << percentile(self_tick_ms, 0.95) << " ms  (period " << dt * 1000.0 << " ms)\n";
	if (percentile(self_tick_ms, 0.95) > dt * 1000.0 * 0.8) {
		os << "WARNING: the bot process itself is near its tick budget; split bots across\n"
			  "         several vb_loadtest processes/machines for trustworthy numbers.\n";
	}
	std::cout << os.str();

	if (!json_path.empty()) {
		std::ofstream js(json_path);
		js << std::fixed << std::setprecision(3) << "{\n"
		   << "  \"bots_requested\": " << bot_count << ",\n"
		   << "  \"bots_joined\": " << joined << ",\n"
		   << "  \"failures\": {";
		bool first = true;
		for (const auto &[reason, n] : failures) {
			std::string esc;
			for (char c : reason) {
				if (c == '"' || c == '\\') {
					esc += '\\';
				}
				esc += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
			}
			js << (first ? "" : ", ") << "\"" << esc << "\": " << n;
			first = false;
		}
		js << "},\n"
		   << "  \"join_s_p50\": " << percentile(join_s, 0.5) << ",\n"
		   << "  \"join_s_max\": " << percentile(join_s, 1.0) << ",\n"
		   << "  \"rtt_ms_p50\": " << percentile(rtt_ms, 0.5) << ",\n"
		   << "  \"rtt_ms_p95\": " << percentile(rtt_ms, 0.95) << ",\n"
		   << "  \"rx_kib_s_per_bot\": " << mean(rx_kbps) << ",\n"
		   << "  \"tx_kib_s_per_bot\": " << mean(tx_kbps) << ",\n"
		   << "  \"rx_kib_s_total\": " << total_rx_kbps << ",\n"
		   << "  \"window_s\": " << window << ",\n"
		   << "  \"bot_tick_ms_p50\": " << percentile(self_tick_ms, 0.5) << ",\n"
		   << "  \"bot_tick_ms_p95\": " << percentile(self_tick_ms, 0.95) << "\n"
		   << "}\n";
	}

	for (auto &b : bots) {
		b->counter->close(b->conn, "vb_loadtest done");
	}
	// Give the close a moment to reach the server instead of timing out there.
	for (int i = 0; i < 5; ++i) {
		std::vector<TransportEvent> ev;
		for (auto &b : bots) {
			ev.clear();
			b->gns->poll(ev);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	if (connect_error) {
		return EXIT_FAILURE;
	}
	return joined == bot_count ? EXIT_SUCCESS : 2;
}
