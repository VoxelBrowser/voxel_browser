#include <doctest/doctest.h>

#include <ostream>

#include "vb/net/gns_transport.hpp"
#include "vb/net/handshake.hpp"
#include "vb/net/session.hpp"

#if !VB_WITH_NET

TEST_CASE("GnsTransport reports kBackendUnavailable without VB_WITH_NET") {
	vb::net::GnsTransport t;
	const auto status = t.listen(0);
	CHECK_FALSE(status);
	CHECK(status.error() == vb::core::NetError::kBackendUnavailable);
	const auto conn = t.connect("127.0.0.1", 27015);
	CHECK_FALSE(conn);
	CHECK_FALSE(t.round_trip_time_seconds(vb::net::ConnId{ 1 }).has_value());
}

#else

#include <chrono>
#include <thread>
#include <vector>

#include "vb/protocol/message.hpp"

using namespace vb::net;

namespace {

// Real UDP: pump until `pred()` is true or the attempt budget runs out.
template <typename Pred>
bool pump_until(int attempts, Pred pred) {
	for (int i = 0; i < attempts; ++i) {
		if (pred()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return pred();
}

} // namespace

TEST_CASE("GnsTransport: connect, exchange a message, and disconnect over real UDP") {
	// GNS's direct-UDP listen path rejects port 0 ("Must specify local
	// port."), unlike a plain BSD socket -- it has no ephemeral-port
	// allocation, so tests must pick a concrete port themselves.
	constexpr std::uint16_t kTestPort = 27201;

	GnsTransport server;
	REQUIRE(server.listen(kTestPort));
	REQUIRE(server.bound_port() == kTestPort);
	CHECK(server.is_server());

	GnsTransport client;
	const auto conn = client.connect("127.0.0.1", server.bound_port());
	REQUIRE(conn);
	CHECK_FALSE(client.is_server());

	bool client_connected = false;
	bool server_connected = false;
	ConnId server_side_conn = ConnId::kInvalid;

	const bool both_connected = pump_until(500, [&] {
		std::vector<TransportEvent> ev;
		client.poll(ev);
		for (const auto &e : ev) {
			if (e.kind == TransportEvent::Kind::kConnected) {
				client_connected = true;
			}
		}
		ev.clear();
		server.poll(ev);
		for (const auto &e : ev) {
			if (e.kind == TransportEvent::Kind::kConnected) {
				server_connected = true;
				server_side_conn = e.conn;
			}
		}
		return client_connected && server_connected;
	});
	REQUIRE(both_connected);
	CHECK(server.connection_count() == 1);
	CHECK(client.connection_count() == 1);

	const std::vector<std::byte> payload{ std::byte{ 1 }, std::byte{ 2 },
		std::byte{ 3 }, std::byte{ 4 } };
	client.send(*conn, vb::protocol::Lane::kControl, payload);

	std::vector<std::byte> received;
	const bool got_message = pump_until(400, [&] {
		std::vector<TransportEvent> ev;
		server.poll(ev);
		for (const auto &e : ev) {
			if (e.kind == TransportEvent::Kind::kMessage && e.conn == server_side_conn) {
				received = e.frame;
			}
		}
		return !received.empty();
	});
	REQUIRE(got_message);
	CHECK(received == payload);

	// Client-initiated close: the client sees it immediately (synthesized),
	// the server learns about it asynchronously via its own poll().
	client.close(*conn, "bye");
	CHECK(client.connection_count() == 0);

	bool server_saw_disconnect = false;
	const bool disconnected = pump_until(400, [&] {
		std::vector<TransportEvent> ev;
		server.poll(ev);
		for (const auto &e : ev) {
			if (e.kind == TransportEvent::Kind::kDisconnected &&
					e.conn == server_side_conn) {
				server_saw_disconnect = true;
			}
		}
		return server_saw_disconnect;
	});
	CHECK(disconnected);
	CHECK(server.connection_count() == 0);
}

// Block damage rides Lane::kFeedback, which maps to its own higher-priority
// GNS lane: it must not wait behind bulk world data already queued on the
// connection. GNS sends at ~1 MB/s by default, so before this a damage update
// behind a burst of chunks waited for the whole burst.
TEST_CASE("GnsTransport: a kFeedback message overtakes queued kWorld data") {
	constexpr std::uint16_t kTestPort = 27207;
	GnsTransport server;
	REQUIRE(server.listen(kTestPort));
	GnsTransport client;
	const auto conn = client.connect("127.0.0.1", server.bound_port());
	REQUIRE(conn);
	ConnId server_side_conn = ConnId::kInvalid;
	bool client_connected = false;
	REQUIRE(pump_until(500, [&] {
		std::vector<TransportEvent> ev;
		client.poll(ev);
		for (const auto &e : ev) {
			client_connected = client_connected || e.kind == TransportEvent::Kind::kConnected;
		}
		ev.clear();
		server.poll(ev);
		for (const auto &e : ev) {
			if (e.kind == TransportEvent::Kind::kConnected) {
				server_side_conn = e.conn;
			}
		}
		return client_connected && server_side_conn != ConnId::kInvalid;
	}));

	// ~1 MiB of chunk-sized world messages, then one small feedback message.
	constexpr int kWorldMessages = 16;
	const std::vector<std::byte> world(64 * 1024, std::byte{ 0xAA });
	for (int i = 0; i < kWorldMessages; ++i) {
		server.send(server_side_conn, vb::protocol::Lane::kWorld, world);
	}
	const std::vector<std::byte> feedback(16, std::byte{ 0xFB });
	server.send(server_side_conn, vb::protocol::Lane::kFeedback, feedback);

	int world_received = 0;
	int world_before_feedback = -1;
	REQUIRE(pump_until(2000, [&] {
		std::vector<TransportEvent> ev;
		client.poll(ev);
		for (const auto &e : ev) {
			if (e.kind != TransportEvent::Kind::kMessage) {
				continue;
			}
			if (e.frame == feedback) {
				world_before_feedback = world_received;
			} else if (e.frame.size() == world.size()) {
				++world_received;
			}
		}
		return world_before_feedback >= 0 && world_received == kWorldMessages;
	}));
	// Sent ahead of the queue, not after all of it (one shared lane would give 16).
	CHECK(world_before_feedback < kWorldMessages / 2);
}

// REMAINING_TASKS.md Phase 3's "wall-clock server_time_est" gap needs a
// real transport-level RTT to feed it (the whole reason it was gated on
// GnsTransport in the first place -- LoopbackTransport has nothing to
// measure). GNS only starts reporting a real ping after enough real
// packets have actually round-tripped, so this polls a little longer than
// the bare connect tests above before asserting on it.
TEST_CASE("GnsTransport::round_trip_time_seconds reports a real, small RTT "
		"over localhost once connected") {
	constexpr std::uint16_t kTestPort = 27206;

	GnsTransport server;
	REQUIRE(server.listen(kTestPort));
	GnsTransport client;
	const auto conn = client.connect("127.0.0.1", kTestPort);
	REQUIRE(conn);

	// Whether RTT is measured yet right after connect() is an unobservable
	// race against GNS's own internal service thread, not a contract of our
	// wrapper -- over loopback it can already have a real (near-zero)
	// estimate before this test thread gets to ask. What actually matters
	// (checked below) is that a real, small RTT is reported once the
	// connection is up and has exchanged real traffic.
	const bool connected = pump_until(500, [&] {
		std::vector<TransportEvent> cev;
		client.poll(cev);
		std::vector<TransportEvent> sev;
		server.poll(sev);
		return client.connection_count() == 1;
	});
	REQUIRE(connected);

	// Exchange a little traffic both ways so GNS has real round trips to
	// measure from, not just the connection handshake itself.
	const std::vector<std::byte> ping{ std::byte{ 1 } };
	std::optional<double> rtt;
	pump_until(400, [&] {
		client.send(*conn, vb::protocol::Lane::kControl, ping);
		std::vector<TransportEvent> cev;
		client.poll(cev);
		std::vector<TransportEvent> sev;
		server.poll(sev);
		rtt = client.round_trip_time_seconds(*conn);
		return rtt.has_value();
	});

	REQUIRE(rtt.has_value());
	CHECK(*rtt >= 0.0);
	// Real localhost traffic, not a stalled/misrouted connection.
	CHECK(*rtt < 1.0);

	// An unknown/never-connected id: still nullopt, not a crash.
	CHECK_FALSE(client.round_trip_time_seconds(static_cast<ConnId>(0xDEADBEEF))
					.has_value());
}

// §8.3 hardening (REMAINING_TASKS.md 1.3): remote_address() is what
// ServerSession::set_max_connections_per_ip is built on -- confirmed here at
// the transport level, then end-to-end through a real ServerSession/
// ClientSession pair below.
TEST_CASE("GnsTransport::remote_address returns the real peer IP over UDP") {
	constexpr std::uint16_t kTestPort = 27202;

	GnsTransport server;
	REQUIRE(server.listen(kTestPort));
	GnsTransport client;
	const auto conn = client.connect("127.0.0.1", kTestPort);
	REQUIRE(conn);

	ConnId server_side_conn = ConnId::kInvalid;
	const bool connected = pump_until(500, [&] {
		std::vector<TransportEvent> cev;
		client.poll(cev);
		std::vector<TransportEvent> sev;
		server.poll(sev);
		for (const auto &e : sev) {
			if (e.kind == TransportEvent::Kind::kConnected) {
				server_side_conn = e.conn;
			}
		}
		return server_side_conn != ConnId::kInvalid;
	});
	REQUIRE(connected);

	const auto addr = server.remote_address(server_side_conn);
	REQUIRE(addr.has_value());
	CHECK(*addr == "127.0.0.1");

	// An unknown/never-connected id: nullopt, not a crash.
	CHECK_FALSE(server.remote_address(static_cast<ConnId>(0xDEADBEEF)).has_value());
}

TEST_CASE("ServerSession rejects a connection beyond the per-IP cap over real UDP") {
	constexpr std::uint16_t kTestPort = 27203;

	GnsTransport server_transport;
	REQUIRE(server_transport.listen(kTestPort));
	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(server_transport, cfg);
	server.set_max_connections_per_ip(1);

	GnsTransport client1_transport;
	const auto conn1 = client1_transport.connect("127.0.0.1", kTestPort);
	REQUIRE(conn1);
	ClientSession client1(client1_transport, *conn1,
			HandshakeClientConfig{ "A", "", "v", 1 });

	const bool first_joined = pump_until(500, [&] {
		server.tick(0.05);
		client1.tick(0.05);
		return client1.joined();
	});
	REQUIRE(first_joined);

	// A second connection from the same address (127.0.0.1): the cap is
	// already at 1, so the server-side Transport::close()s it before any
	// handshake traffic even starts -- the client sees a failed connection,
	// not a rejected auth (this is a transport-level policy, not
	// HandshakeServerConfig::max_players' application-level one).
	GnsTransport client2_transport;
	const auto conn2 = client2_transport.connect("127.0.0.1", kTestPort);
	REQUIRE(conn2);
	ClientSession client2(client2_transport, *conn2,
			HandshakeClientConfig{ "B", "", "v", 1 });

	pump_until(500, [&] {
		server.tick(0.05);
		client1.tick(0.05);
		client2.tick(0.05);
		return client2.joined() || client2.failed();
	});

	CHECK_FALSE(client2.joined());
	CHECK(client2.failed());
	CHECK(client1.joined()); // the first connection is unaffected
}

// REMAINING_TASKS.md 1.3 polish: SteamNetworkingIPAddr::ParseString() only
// ever accepts numeric IP literals -- "localhost" used to fail connect()
// outright before GnsTransport::connect() gained a getaddrinfo() fallback.
TEST_CASE("GnsTransport::connect resolves a real hostname (\"localhost\"), "
		"not just numeric IP literals") {
	constexpr std::uint16_t kTestPort = 27204;

	GnsTransport server;
	REQUIRE(server.listen(kTestPort));

	GnsTransport client;
	const auto conn = client.connect("localhost", kTestPort);
	REQUIRE(conn);

	bool server_connected = false;
	const bool connected = pump_until(500, [&] {
		std::vector<TransportEvent> cev;
		client.poll(cev);
		std::vector<TransportEvent> sev;
		server.poll(sev);
		for (const auto &e : sev) {
			if (e.kind == TransportEvent::Kind::kConnected) {
				server_connected = true;
			}
		}
		return server_connected;
	});
	CHECK(connected);

	// A genuinely unresolvable name still fails cleanly, same as before.
	const auto bad = client.connect(
			"this-hostname-should-never-resolve.invalid", kTestPort);
	CHECK_FALSE(bad);
}

// REMAINING_TASKS.md Phase 1 polish: "the two-client replication test runs
// over LoopbackTransport only; re-run over GnsTransport" -- the exact same
// scenario as replication_test.cpp's "two clients within interest range
// replicate to each other", but over real UDP instead of the in-process
// loopback, since the interest/replication logic itself is transport-
// agnostic (it only ever talks to Transport, never LoopbackTransport
// directly) and this was the one path never exercised over the real backend.
TEST_CASE("two clients within interest range replicate to each other, over "
		"real UDP (GnsTransport)") {
	constexpr std::uint16_t kTestPort = 27205;

	GnsTransport server_transport;
	REQUIRE(server_transport.listen(kTestPort));
	HandshakeServerConfig cfg;
	cfg.world_seed = 3;
	ServerSession server(server_transport, cfg);
	server.set_interest_radius_cells(1);

	GnsTransport a_transport;
	const auto conn_a = a_transport.connect("127.0.0.1", kTestPort);
	REQUIRE(conn_a);
	ClientSession a(a_transport, *conn_a, HandshakeClientConfig{ "A", "", "v", 1 });

	GnsTransport b_transport;
	const auto conn_b = b_transport.connect("127.0.0.1", kTestPort);
	REQUIRE(conn_b);
	ClientSession b(b_transport, *conn_b, HandshakeClientConfig{ "B", "", "v", 2 });

	const bool both_joined = pump_until(500, [&] {
		server.tick(0.05);
		a.tick(0.05);
		b.tick(0.05);
		return a.joined() && b.joined();
	});
	REQUIRE(both_joined);
	const auto a_id = a.join_accept()->your_net_id;
	const auto b_id = b.join_accept()->your_net_id;

	// Both near the origin (default 32 m cells, radius 1 -> same/adjacent cell).
	server.set_player_state(a_id, vb::core::Vec3d{ 0, 64, 0 });
	server.set_player_state(b_id, vb::core::Vec3d{ 8, 64, 0 });

	// Don't stop at the first tick each sees the other -- over real UDP the
	// "entered interest" event and the position-8 snapshot can land in
	// separate packets, so the earliest visible position may still be
	// whatever was current at connect time (0,0,0), not yet the explicit
	// set_player_state() above. Wait for the position to actually converge,
	// not just for presence.
	const bool both_see_each_other = pump_until(500, [&] {
		server.tick(0.05);
		a.tick(0.05);
		b.tick(0.05);
		return a.remote_entities().count(b_id) == 1 &&
				b.remote_entities().count(a_id) == 1 &&
				a.remote_entities().at(b_id).pos.x == doctest::Approx(8.0);
	});
	REQUIRE(both_see_each_other);
	CHECK(a.remote_entities().at(b_id).pos.x == doctest::Approx(8.0));

	// Move B far away -> each side should get a removal. Wait for both: over real UDP the two
	// removals travel in separate packets, so A seeing B go says nothing about B seeing A go yet.
	server.set_player_state(b_id, vb::core::Vec3d{ 5000, 64, 0 });
	const bool both_left = pump_until(500, [&] {
		server.tick(0.05);
		a.tick(0.05);
		b.tick(0.05);
		return a.remote_entities().count(b_id) == 0 && b.remote_entities().count(a_id) == 0;
	});
	CHECK(both_left);
	CHECK(a.remote_entities().count(b_id) == 0);
	CHECK(b.remote_entities().count(a_id) == 0);
}

#endif // VB_WITH_NET
