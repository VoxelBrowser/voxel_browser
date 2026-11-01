#include <doctest/doctest.h>

#include <ostream>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vb/net/loopback.hpp"
#include "vb/net/session.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/world/block.hpp"
#include "vb/world/world.hpp"
#include "vb/worldgen/generator.hpp"
#include "vb/worldgen/worker_pool.hpp"

using namespace vb::net;
using vb::core::Vec3d;
namespace wg = vb::worldgen;

// Server capacity stress tests: how many players can actually join, what
// happens to the ones past `max_players`, and how expensive a full server's
// tick is. soak_test.cpp covers long-running queue growth with a handful of
// clients; these cover the player-count axis instead. Everything runs over
// LoopbackNetwork with simulated time (no real sleeps, no real UDP), so it
// stays deterministic inside the default vb_tests run.
namespace {

struct CapClient {
	Transport *transport = nullptr;
	ConnId conn{};
	std::unique_ptr<ClientSession> session;
};

class CapacityHarness {
public:
	explicit CapacityHarness(std::uint32_t max_players) {
		HandshakeServerConfig cfg;
		cfg.world_seed = 7;
		cfg.max_players = max_players;
		server = std::make_unique<ServerSession>(net.server(), cfg);
		REQUIRE(net.server().listen(0));
	}

	CapClient &connect(const std::string &name) {
		Transport &t = net.create_client();
		auto conn = t.connect("x", 0);
		REQUIRE(conn);
		auto c = std::make_unique<CapClient>();
		c->transport = &t;
		c->conn = *conn;
		c->session = std::make_unique<ClientSession>(
				t, *conn, HandshakeClientConfig{ name, "", "v", 1 });
		clients.push_back(std::move(c));
		return *clients.back();
	}

	// Pumps server + every client; records the highest player_count() seen
	// at any point so an overfill that later "corrects itself" still fails.
	void pump(int rounds) {
		for (int r = 0; r < rounds; ++r) {
			server->tick(0.05);
			peak_players = std::max(peak_players, server->player_count());
			for (auto &c : clients) {
				c->session->tick(0.05);
			}
		}
	}

	std::size_t joined_count() const {
		return static_cast<std::size_t>(std::count_if(clients.begin(), clients.end(),
				[](const auto &c) { return c->session->joined(); }));
	}
	std::size_t failed_count() const {
		return static_cast<std::size_t>(std::count_if(clients.begin(), clients.end(),
				[](const auto &c) { return c->session->failed(); }));
	}

	LoopbackNetwork net;
	std::unique_ptr<ServerSession> server;
	std::vector<std::unique_ptr<CapClient>> clients;
	std::size_t peak_players = 0;
};

constexpr std::uint32_t kDefaultMaxPlayers = 16; // HandshakeServerConfig's default

} // namespace

TEST_CASE("capacity: clients joining one at a time fill the server to "
		  "max_players, the next is refused, and a freed slot is reusable") {
	CapacityHarness h(kDefaultMaxPlayers);

	for (std::uint32_t i = 0; i < kDefaultMaxPlayers; ++i) {
		CapClient &c = h.connect("p" + std::to_string(i));
		h.pump(10);
		REQUIRE_MESSAGE(c.session->joined(), "player ", i, " failed to join");
	}
	CHECK(h.server->player_count() == kDefaultMaxPlayers);
	CHECK(h.server->pending_count() == 0);

	CapClient &extra = h.connect("overflow");
	h.pump(10);
	CHECK(extra.session->failed());
	CHECK_FALSE(extra.session->joined());
	CHECK(h.server->player_count() == kDefaultMaxPlayers);

	// One player leaves; the server reclaims the slot and the refused
	// connection, and a newcomer can take that slot.
	h.clients.front()->transport->close(h.clients.front()->conn, "bye");
	h.pump(5);
	CHECK(h.server->player_count() == kDefaultMaxPlayers - 1);
	CHECK(h.server->pending_count() == 0);

	CapClient &late = h.connect("late");
	h.pump(10);
	CHECK(late.session->joined());
	CHECK(h.server->player_count() == kDefaultMaxPlayers);
	CHECK(h.peak_players == kDefaultMaxPlayers);
}

TEST_CASE("capacity: a simultaneous burst of more than max_players clients "
		  "never overfills the server") {
	// Every client connects before the server ticks once, so all of them run
	// their handshakes interleaved. The cap check must hold even though none
	// of them is "playing" yet when the others pass their Auth step.
	constexpr std::uint32_t kBurst = kDefaultMaxPlayers + 8;
	CapacityHarness h(kDefaultMaxPlayers);
	for (std::uint32_t i = 0; i < kBurst; ++i) {
		h.connect("burst" + std::to_string(i));
	}
	h.pump(20);

	INFO("joined=", h.joined_count(), " failed=", h.failed_count(),
			" peak=", h.peak_players);
	CHECK(h.peak_players <= kDefaultMaxPlayers);
	CHECK(h.server->player_count() <= kDefaultMaxPlayers);
	CHECK(h.joined_count() == h.server->player_count());
	// Nobody is left hanging: each client either got in or was told no.
	CHECK(h.joined_count() + h.failed_count() == kBurst);
	CHECK(h.server->pending_count() == 0);

	for (auto &c : h.clients) {
		c->transport->close(c->conn, "done");
	}
	h.pump(5);
	CHECK(h.server->player_count() == 0);
	CHECK(h.server->pending_count() == 0);
}

TEST_CASE("capacity: a full server's tick stays inside the tick budget") {
	// max_players clients, all joined, with world replication on, moving
	// around a shared area. The gate is the 20 Hz tick budget (50 ms) on the
	// median steady-state server tick -- generous for 16 players in a small
	// area, so it only trips on a real per-player blow-up (e.g. accidental
	// O(n^2) per-tick work), not on CI noise. Initial chunk generation is
	// excluded: it happens synchronously here and is perf_budget_test.cpp's
	// concern, not this one's.
	vb::world::World world{ vb::world::BlockRegistry::base() };
	wg::WorldGenWorkerPool pool{ wg::WorldGenerator(wg::WorldGenParams{},
										 vb::world::BlockRegistry::base()),
		wg::WorldGenWorkerPool::kSynchronous };

	CapacityHarness h(kDefaultMaxPlayers);
	h.server->set_world_replicator(std::make_unique<WorldReplicator>(
			world, pool, vb::world::BlockRegistry::base(), /*view*/ 1, /*vview*/ 1));

	for (std::uint32_t i = 0; i < kDefaultMaxPlayers; ++i) {
		h.connect("t" + std::to_string(i));
	}
	h.pump(20);
	REQUIRE(h.joined_count() == kDefaultMaxPlayers);

	std::vector<vb::core::NetId> ids;
	for (auto &c : h.clients) {
		ids.push_back(c->session->join_accept()->your_net_id);
	}
	auto place = [&](int tick) {
		for (std::size_t i = 0; i < ids.size(); ++i) {
			// A small circle-ish walk inside one shared area, deterministic.
			const double x = static_cast<double>(i % 4) * 4.0 + (tick % 8);
			const double z = static_cast<double>(i / 4) * 4.0 + ((tick / 8) % 8);
			h.server->set_player_state(ids[i], Vec3d{ x, 40.0, z });
		}
	};

	// Warm-up: let the shared area's chunks generate and stream out.
	for (int tick = 0; tick < 40; ++tick) {
		place(tick);
		h.pump(1);
	}

	constexpr int kMeasuredTicks = 60;
	std::vector<double> server_ms;
	server_ms.reserve(kMeasuredTicks);
	for (int tick = 40; tick < 40 + kMeasuredTicks; ++tick) {
		place(tick);
		const auto t0 = std::chrono::steady_clock::now();
		h.server->tick(0.05);
		const auto t1 = std::chrono::steady_clock::now();
		server_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		for (auto &c : h.clients) {
			c->session->tick(0.05);
		}
	}

	std::sort(server_ms.begin(), server_ms.end());
	const double median = server_ms[server_ms.size() / 2];
	INFO(kDefaultMaxPlayers, " players: median server tick ", median, " ms, worst ",
			server_ms.back(), " ms");
	CHECK(h.server->player_count() == kDefaultMaxPlayers);
	CHECK(median < 50.0);
}
