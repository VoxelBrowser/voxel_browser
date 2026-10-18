// In-engine authentication, step 9.4 (architecture_spec/auth.md §5.5/§6): the
// verified login reaches pack scripts, names are made unique, a second
// sign-in of the same account kicks the older session. Driven over the
// loopback transport with a fake verifier (the real one is auth_verifier_test).

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "vb/net/loopback.hpp"
#include "vb/net/session.hpp"
#include "vb/script/pack_runtime.hpp"
#include "vb/world/block.hpp"

#if VB_WITH_LUA

using namespace vb::net;

namespace {

std::filesystem::path temp_storage(const char *name) {
	auto p = std::filesystem::temp_directory_path() /
			(std::string("vb_auth_login_") + name + ".json");
	std::filesystem::remove(p);
	return p;
}

// Token format of the fake IdP: "<name>|<subject>|<claims-json>[|<iat>]"; "bad" is
// rejected. Without an iat the login is stamped 0 (join does not check it).
AuthTicket fake_verify(std::string_view token, std::string_view nonce) {
	AuthOutcome out;
	if (token == "bad" || nonce != "nonce-1") {
		out = AuthOutcome{ false, "not accepted by this server", {}, {} };
	} else {
		const std::string t(token);
		const auto p1 = t.find('|');
		const auto p2 = t.find('|', p1 + 1);
		const auto p3 = t.find('|', p2 + 1);
		auto login = std::make_shared<LoginData>();
		login->provider = "keycloak";
		login->issuer = "https://idp.example";
		login->name = t.substr(0, p1);
		login->subject = t.substr(p1 + 1, p2 - p1 - 1);
		login->claims_json = t.substr(p2 + 1, p3 == std::string::npos ? std::string::npos : p3 - p2 - 1);
		if (p3 != std::string::npos) {
			login->issued_at = std::stoll(t.substr(p3 + 1));
		}
		out = AuthOutcome{ true, {}, login->name, login };
	}
	return [out] { return std::optional<AuthOutcome>(out); };
}

struct AuthRig {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	std::unique_ptr<vb::script::PackRuntime> rt;
	std::unique_ptr<ServerSession> server;
	std::vector<std::unique_ptr<ClientSession>> clients;
	std::vector<std::pair<Transport *, ConnId>> links;

	static constexpr std::int64_t kBase = 1'800'000'000;
	double elapsed = 0.0;
	std::int64_t now() const { return kBase + static_cast<std::int64_t>(elapsed); }

	AuthRig(const char *name, const std::string &pack, bool external,
			std::uint32_t reauth_interval = 0, std::uint32_t reauth_grace = 3) {
		rt = std::make_unique<vb::script::PackRuntime>(net.server(), registry,
				temp_storage(name));
		rt->set_auth_required(external);
		REQUIRE(rt->load_pack_file(pack));
		rt->freeze();
		HandshakeServerConfig cfg;
		cfg.world_seed = 7;
		cfg.reauth_interval_seconds = reauth_interval;
		cfg.reauth_grace_seconds = reauth_grace;
		HandshakeServerHost host;
		host.unix_time = [this] { return now(); };
		if (external) {
			cfg.auth_mode = vb::protocol::AuthMode::kExternal;
			host.auth_challenge = [] {
				vb::protocol::S2CAuthChallenge c;
				c.provider = "keycloak";
				c.nonce = "nonce-1";
				return std::optional<vb::protocol::S2CAuthChallenge>(c);
			};
			host.begin_authenticate = fake_verify;
		}
		rt->install_join_veto(host);
		server = std::make_unique<ServerSession>(net.server(), cfg, host);
		rt->attach_session(*server);
		REQUIRE(net.server().listen(0));
	}

	ClientSession &connect(const std::string &name, const std::string &token) {
		Transport &t = net.create_client();
		auto id = t.connect("x", 0);
		REQUIRE(id);
		HandshakeClientConfig cc;
		cc.player_name = name;
		cc.token = token;
		cc.client_nonce = clients.size() + 1;
		clients.push_back(std::make_unique<ClientSession>(t, *id, cc));
		links.emplace_back(&t, *id);
		return *clients.back();
	}

	void disconnect(const ClientSession &c) {
		for (std::size_t i = 0; i < clients.size(); ++i) {
			if (clients[i].get() == &c) {
				links[i].first->close(links[i].second, "bye");
			}
		}
	}

	void pump(int n = 30) {
		for (int i = 0; i < n; ++i) {
			server->tick(0.05);
			for (auto &c : clients) {
				c->tick(0.05);
			}
			for (const auto &j : server->take_joins()) {
				rt->dispatch_player_join_completed(j);
			}
			for (const auto &c : server->take_login_changes()) {
				rt->dispatch_login_changed(c);
			}
			for (const auto &l : server->take_leaves()) {
				rt->dispatch_player_leave(l);
			}
			elapsed += 0.05;
		}
	}
};

constexpr const char *kRecordingPack = R"(
	join_calls = {}
	leave_logins = {}
	vb.on("player_join", function(name, login)
		join_calls[#join_calls + 1] = { name = name, login = login }
	end)
	vb.on("player_leave", function(player)
		local l = player:get_login()
		leave_logins[#leave_logins + 1] = (l == nil) and "nil" or l.subject
	end)
)";

} // namespace

TEST_CASE("auth pack: player_join and get_login see the verified, frozen login") {
	AuthRig rig("frozen", kRecordingPack, /*external=*/true);
	auto &alice = rig.connect("ignored-client-name",
			R"(alice|sub-a|{"email":"a@x.io","groups":["mod","dev"]})");
	rig.pump();
	REQUIRE(alice.joined());

	REQUIRE(rig.rt->load_pack_file(R"(
		assert(vb.auth.required() == true)
		assert(#join_calls == 1)
		local c = join_calls[1]
		-- The in-game name comes from the verified claim, not the client's.
		assert(c.name == "alice")
		local l = c.login
		assert(l ~= nil)
		assert(l.provider == "keycloak" and l.subject == "sub-a" and l.name == "alice")
		assert(l.claims.email == "a@x.io" and l.claims.groups[2] == "dev")
		-- Frozen: no writes, no swapping the metatable, no engine-internal fields.
		assert(not pcall(function() l.name = "root" end))
		assert(not pcall(function() l.claims.email = "x" end))
		assert(not pcall(function() l.claims.groups[1] = "admin" end))
		assert(getmetatable(l) == false)
		assert(l.issuer == nil and l.token == nil and l.expires_at == nil)
		local n = 0
		for k in pairs(l) do n = n + 1 end
		assert(n == 4)
	)"));

	// Every Player the script can obtain has a non-nil login, even while it
	// leaves: player_leave runs after the session dropped the connection.
	rig.disconnect(alice);
	rig.pump();
	REQUIRE(rig.rt->load_pack_file(R"(
		assert(#leave_logins == 1 and leave_logins[1] == "sub-a")
	)"));
}

TEST_CASE("plain pack: login is nil everywhere and vb.auth.required() is false") {
	AuthRig rig("plain", kRecordingPack, /*external=*/false);
	auto &bob = rig.connect("Bob", "");
	rig.pump();
	REQUIRE(bob.joined());
	rig.disconnect(bob);
	rig.pump();
	REQUIRE(rig.rt->load_pack_file(R"(
		assert(vb.auth.required() == false)
		assert(#join_calls == 1 and join_calls[1].name == "Bob" and join_calls[1].login == nil)
		assert(#leave_logins == 1 and leave_logins[1] == "nil")
	)"));
}

TEST_CASE("auth pack: a rejected token never reaches the asset manifest or the world") {
	AuthRig rig("rejected", kRecordingPack, true);
	auto &mallory = rig.connect("Mallory", "bad");
	rig.pump();
	CHECK(mallory.failed());
	CHECK_FALSE(mallory.joined());
	CHECK(rig.server->player_count() == 0);
	REQUIRE(rig.rt->load_pack_file("assert(#join_calls == 0)"));
}

TEST_CASE("auth pack: player_join can veto on the login") {
	AuthRig rig("veto", R"(
		vb.on("player_join", function(name, login)
			return login.claims.verified == true
		end)
	)",
			true);
	auto &ok = rig.connect("x", R"(good|s1|{"verified":true})");
	auto &no = rig.connect("x", R"(unverified|s2|{"verified":false})");
	rig.pump();
	CHECK(ok.joined());
	CHECK(no.failed());
}

TEST_CASE("auth pack: two people called alex get alex and alex#2") {
	AuthRig rig("names", kRecordingPack, true);
	auto &a = rig.connect("c", "alex|sub-1|{}");
	rig.pump();
	auto &b = rig.connect("c", "alex|sub-2|{}");
	rig.pump();
	REQUIRE(a.joined());
	REQUIRE(b.joined());
	// Each client learns the name it actually got (S2C_JoinAccept.your_name, v33).
	CHECK(a.join_accept()->your_name == "alex");
	CHECK(b.join_accept()->your_name == "alex#2");
	REQUIRE(rig.rt->load_pack_file(R"(
		assert(#join_calls == 2)
		assert(join_calls[1].name == "alex" and join_calls[1].login.name == "alex")
		assert(join_calls[2].name == "alex#2" and join_calls[2].login.name == "alex#2")
		assert(join_calls[2].login.subject == "sub-2")
	)"));
}

TEST_CASE("auth pack: the same account signing in again kicks the older session") {
	AuthRig rig("dup", kRecordingPack, true);
	auto &first = rig.connect("c", "carol|sub-c|{}");
	rig.pump();
	REQUIRE(first.joined());
	CHECK(rig.server->player_count() == 1);

	auto &second = rig.connect("c", "carol|sub-c|{}");
	rig.pump();
	CHECK(second.joined()); // the newcomer is never refused
	CHECK(rig.server->player_count() == 1);
	REQUIRE(rig.rt->load_pack_file(R"(
		-- The older session left; the newcomer keeps the plain name (the kicked
		-- session does not make it "carol#2").
		assert(#leave_logins == 1 and leave_logins[1] == "sub-c")
		assert(join_calls[2].name == "carol")
	)"));
}

// ---------------------------------------------------------------------------
// Per-IP sign-in attempt limit (auth.md §8)
// ---------------------------------------------------------------------------

namespace {

// Delegates to a LoopbackTransport but reports every peer as one IP, as a real
// network transport would for several connections from the same host.
class FixedIpTransport final : public Transport {
public:
	explicit FixedIpTransport(Transport &inner) : inner_(inner) {}
	vb::core::Status<vb::core::NetError> listen(std::uint16_t port) override { return inner_.listen(port); }
	vb::core::Result<ConnId, vb::core::NetError> connect(std::string_view h, std::uint16_t p) override {
		return inner_.connect(h, p);
	}
	void send(ConnId c, vb::protocol::Lane l, std::span<const std::byte> f) override { inner_.send(c, l, f); }
	void close(ConnId c, std::string_view r) override { inner_.close(c, r); }
	void poll(std::vector<TransportEvent> &out) override { inner_.poll(out); }
	bool is_server() const override { return inner_.is_server(); }
	std::size_t connection_count() const override { return inner_.connection_count(); }
	std::optional<std::string> remote_address(ConnId) const override { return "203.0.113.9"; }

private:
	Transport &inner_;
};

} // namespace

TEST_CASE("auth pack: a client cannot hammer sign-in attempts from one IP") {
	LoopbackNetwork net;
	FixedIpTransport server_side(net.server());
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	cfg.auth_mode = vb::protocol::AuthMode::kExternal;
	HandshakeServerHost host;
	host.auth_challenge = [] {
		vb::protocol::S2CAuthChallenge c;
		c.nonce = "nonce-1";
		return std::optional<vb::protocol::S2CAuthChallenge>(c);
	};
	host.begin_authenticate = fake_verify;
	ServerSession server(server_side, cfg, host);
	server.set_max_auth_attempts_per_minute_per_ip(3);
	REQUIRE(net.server().listen(0));

	std::vector<std::unique_ptr<ClientSession>> clients;
	std::vector<std::string> reasons;
	for (int i = 0; i < 5; ++i) {
		Transport &t = net.create_client();
		auto id = t.connect("x", 0);
		REQUIRE(id);
		HandshakeClientConfig cc;
		cc.player_name = "x";
		cc.token = "bad"; // every attempt is rejected, so each one is a failed attempt
		cc.client_nonce = static_cast<std::uint64_t>(i + 1);
		clients.push_back(std::make_unique<ClientSession>(t, *id, cc));
		for (int k = 0; k < 20; ++k) {
			server.tick(0.05);
			clients.back()->tick(0.05);
		}
		REQUIRE(clients.back()->failed());
		reasons.push_back(clients.back()->failure_reason());
	}
	// The first three were verified (and rejected as bad tokens); the rest were
	// stopped before any verification work was done.
	CHECK(reasons[0] == "not accepted by this server");
	CHECK(reasons[2] == "not accepted by this server");
	CHECK(reasons[3] == "too many sign-in attempts, try again later");
	CHECK(reasons[4] == "too many sign-in attempts, try again later");

	// A minute later the window has slid and the IP may try again.
	for (int k = 0; k < 1300; ++k) { // 65 s of server ticks
		server.tick(0.05);
	}
	Transport &t = net.create_client();
	auto id = t.connect("x", 0);
	REQUIRE(id);
	HandshakeClientConfig cc;
	cc.token = "later|sub-l|{}";
	ClientSession later(t, *id, cc);
	for (int k = 0; k < 40; ++k) {
		server.tick(0.05);
		later.tick(0.05);
	}
	CHECK(later.joined());
}

// ---------------------------------------------------------------------------
// Periodic live re-authentication (auth.md §5.6)
// ---------------------------------------------------------------------------

namespace {

constexpr const char *kReauthPack = R"(
	changes = {}
	vb.on("login_changed", function(player, login)
		changes[#changes + 1] = { subject = login.subject, groups = login.claims.groups, name = login.name }
	end)
	leaves = 0
	vb.on("player_leave", function(player) leaves = leaves + 1 end)
)";

// The client's re-auth hook: answers every request with `answer()` immediately.
void answer_reauth(ClientSession &c, std::function<TokenPoll()> answer) {
	c.set_reauth_provider([answer](const vb::protocol::S2CReauthRequest &) -> TokenTicket {
		return [answer] { return answer(); };
	});
}

TokenPoll token_poll(const std::string &t) {
	TokenPoll p;
	p.done = true;
	p.token = t;
	return p;
}
TokenPoll refused_poll() {
	TokenPoll p;
	p.done = true;
	p.error = "refresh refused";
	return p;
}

} // namespace

TEST_CASE("re-auth: a live session answers repeated requests and is never kicked") {
	AuthRig rig("reauth_ok", kReauthPack, true, /*interval=*/10, /*grace=*/3);
	auto &c = rig.connect("c", "dana|sub-d|{}");
	int answered = 0;
	answer_reauth(c, [&] {
		++answered;
		return token_poll("dana|sub-d|{}|" + std::to_string(rig.now()));
	});
	rig.pump(10);
	REQUIRE(c.joined());
	rig.pump(260); // ~13 s: one interval (10 s ±10%) has passed
	CHECK(answered == 1);
	CHECK(rig.server->player_count() == 1);
	rig.pump(260);
	CHECK(answered == 2);
	CHECK(rig.server->player_count() == 1);
	REQUIRE(rig.rt->load_pack_file("assert(#changes == 0 and leaves == 0)"));
}

TEST_CASE("re-auth: a changed allowlisted claim fires login_changed and swaps get_login") {
	AuthRig rig("reauth_claims", kReauthPack, true, 10, 3);
	auto &c = rig.connect("c", R"(erin|sub-e|{"groups":["user"]})");
	answer_reauth(c, [&] {
		return token_poll(R"(erin|sub-e|{"groups":["mod"]}|)" + std::to_string(rig.now()));
	});
	rig.pump(10);
	REQUIRE(c.joined());
	rig.pump(260);
	CHECK(rig.server->player_count() == 1);
	REQUIRE(rig.rt->load_pack_file(R"(
		assert(#changes == 1)
		assert(changes[1].subject == "sub-e" and changes[1].groups[1] == "mod")
		assert(changes[1].name == "erin") -- the in-game name is fixed for the session
	)"));
	// An identical claim set on the next round does not fire it again.
	rig.pump(260);
	REQUIRE(rig.rt->load_pack_file("assert(#changes == 1)"));
}

TEST_CASE("re-auth: a revoked login (no valid answer) is kicked when the grace runs out") {
	AuthRig rig("reauth_revoked", kReauthPack, true, 10, 3);
	auto &c = rig.connect("c", "fay|sub-f|{}");
	answer_reauth(c, [] { return refused_poll(); });
	rig.pump(10);
	REQUIRE(c.joined());
	rig.pump(240); // interval (≤ 11 s) elapsed: request sent, grace (3 s) still running
	rig.pump(80);
	CHECK(rig.server->player_count() == 0);
	REQUIRE(rig.rt->load_pack_file("assert(leaves == 1)"));
}

TEST_CASE("re-auth: a stale token (issued before the request) is rejected") {
	AuthRig rig("reauth_stale", kReauthPack, true, 10, 3);
	auto &c = rig.connect("c", "gus|sub-g|{}");
	const std::int64_t old_iat = rig.now() - 600;
	answer_reauth(c, [&] { return token_poll("gus|sub-g|{}|" + std::to_string(old_iat)); });
	rig.pump(10);
	REQUIRE(c.joined());
	rig.pump(300);
	CHECK(rig.server->player_count() == 0);
}

TEST_CASE("re-auth: a different account answering is kicked") {
	AuthRig rig("reauth_other", kReauthPack, true, 10, 3);
	auto &c = rig.connect("c", "hal|sub-h|{}");
	answer_reauth(c, [&] {
		return token_poll("mallory|sub-OTHER|{}|" + std::to_string(rig.now()));
	});
	rig.pump(10);
	REQUIRE(c.joined());
	rig.pump(260);
	CHECK(rig.server->player_count() == 0);
}

TEST_CASE("re-auth: disabled (interval 0) or no auth.lua never sends a request") {
	AuthRig rig("reauth_off", kReauthPack, true, /*interval=*/0, 3);
	auto &c = rig.connect("c", "ivy|sub-i|{}");
	int asked = 0;
	answer_reauth(c, [&] {
		++asked;
		return refused_poll();
	});
	rig.pump(600);
	CHECK(asked == 0);
	CHECK(rig.server->player_count() == 1);

	AuthRig plain("reauth_plain", kReauthPack, false, 10, 3);
	auto &p = plain.connect("Bob", "");
	answer_reauth(p, [&] {
		++asked;
		return refused_poll();
	});
	plain.pump(400);
	CHECK(asked == 0);
	CHECK(plain.server->player_count() == 1);
}

#endif // VB_WITH_LUA
