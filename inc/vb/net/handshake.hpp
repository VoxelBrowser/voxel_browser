#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vb/assetsync/manifest.hpp" // assetsync::Manifest
#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"
#include "vb/net/login.hpp"
#include "vb/protocol/assetsync.hpp" // AssetEntryRecord, S2CAssetData
#include "vb/protocol/compression.hpp"
#include "vb/protocol/handshake.hpp"
#include "vb/protocol/message.hpp"
#include "vb/protocol/world.hpp" // BlockRegistryRecord

// Connection handshake state machines (spec §8.3), transport-agnostic: feed them
// decoded frames, get back frames to send plus a terminal outcome. The server
// runs one ServerHandshake per inbound connection; the client runs one
// ClientHandshake for its outbound connection.
//
// Phase 1 covers Hello -> ServerInfo -> Auth -> AuthResult -> Ready ->
// JoinAccept. The asset-manifest and block-registry steps (§8.3, §9) slot in
// between Auth and Ready in Phase 4.

namespace vb::net {

struct OutgoingFrame {
	protocol::Lane lane = protocol::Lane::kControl;
	std::vector<std::byte> bytes;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

enum class ServerHandshakeState : std::uint8_t {
	kAwaitingHello,
	kAwaitingAuth,
	kVerifyingAuth, // external auth: token received, verifier ticket pending (auth.md §5.2)
	kAwaitingAssetManifestRequest, // spec §9: sent AuthResult(ok), awaiting C2S_AssetManifestRequest
	kAwaitingAssetRequest, // sent S2C_AssetManifest, awaiting C2S_AssetRequest
	kStreamingAssets, // sending S2C_AssetData across ticks; no frame expected here
	kAwaitingReady,
	kPlaying,
	kClosed,
};

struct HandshakeServerConfig {
	std::string pack_name = "base";
	std::string pack_version = "0.0.0";
	std::uint16_t tick_rate = 20;
	// Sent verbatim as S2CServerInfo::view_distance (see that field's own
	// comment) -- the operator's real server.toml view_distance, not a
	// per-pack override; there's no opt-in pattern to mirror here.
	std::uint32_t view_distance = 8;
	std::string motd;
	// Sent verbatim as S2CServerInfo::engine_version_req (pack.toml's
	// engine_version_req; empty = no requirement).
	std::string engine_version_req;
	// Sent as S2CServerInfo::third_person_allowed (the pack's
	// vb.render.set_third_person).
	bool third_person_allowed = true;
	protocol::AuthMode auth_mode = protocol::AuthMode::kNone;
	std::uint32_t max_players = 16;
	double handshake_timeout_seconds = 10.0;
	// auth_mode == kExternal only: how long a client may spend signing in and
	// being verified (kAwaitingAuth / kVerifyingAuth) before it is dropped.
	// The player is in a browser here, so this is far longer than the
	// handshake timeout that covers every other state.
	double auth_timeout_seconds = 300.0;
	// External auth: periodic live re-authentication (auth.md §5.6). Every
	// `reauth_interval_seconds` (±10% per player) a playing player is asked to
	// re-prove their login; no valid answer within `reauth_grace_seconds` kicks
	// them. 0 disables (the login is trusted until disconnect).
	std::uint32_t reauth_interval_seconds = 0;
	std::uint32_t reauth_grace_seconds = 120;
	std::uint64_t world_seed = 0; // used for JoinAccept when the host grant is 0
	// Whether to accept clients that set protocol::kClientFlagAutomation in
	// C2SHello. Defaults to "only if this binary itself was built with
	// VB_WITH_AUTOMATION", so a production server refuses dev/test clients
	// (docs/e2e-automation.md §7.4). Tests override it explicitly.
#if defined(VB_WITH_AUTOMATION)
	bool accept_automation_clients = true;
#else
	bool accept_automation_clients = false;
#endif
};

struct AuthOutcome {
	bool ok = true;
	std::string reason; // shown to the player; keep coarse
	// Name the server will use for the player. Empty = use C2SAuth's
	// player_name (auth_mode none/token); external auth sets it from the
	// verified name_claim.
	std::string resolved_name;
	// External auth only: the verified identity (user data, no token). Set by
	// the verifier host on success; the FSM keeps it for the session.
	std::shared_ptr<const LoginData> login;
};

// A pending token verification (external auth). Polled once per server tick;
// nullopt = still working (e.g. waiting on a JWKS fetch), a value = final.
using AuthTicket = std::function<std::optional<AuthOutcome>()>;

struct JoinGrant {
	core::NetId net_id = core::NetId::kInvalid;
	core::Vec3d spawn_pos{ 0.0, 64.0, 0.0 };
	std::uint64_t world_seed = 0;
	std::uint32_t time_of_day = 0;
};

// Host hooks the FSM calls back into. Defaults make auth_mode=none "just work".
struct HandshakeServerHost {
	std::function<std::uint32_t()> current_player_count = [] { return 0u; };
	std::function<AuthOutcome(std::string_view name, std::string_view token)>
			authenticate =
					[](std::string_view name, std::string_view) -> AuthOutcome {
		if (name.empty() || name.size() > 32) {
			return { false, "invalid player name", {}, {} };
		}
		return { true, {}, {}, {} };
	};
	// auth_mode == kExternal only. `auth_challenge` builds the S2C_AuthChallenge
	// for a new connection (including a fresh random nonce); `nullopt` means
	// the server cannot authenticate right now and the join fails closed.
	// `begin_authenticate` starts verifying `token` against that nonce and
	// returns a ticket the FSM polls (the fast path returns a ticket that is
	// already resolved). When unset, `authenticate` above is used
	// synchronously, which is exactly the auth_mode none/token behaviour.
	std::function<std::optional<protocol::S2CAuthChallenge>()> auth_challenge =
			[] { return std::optional<protocol::S2CAuthChallenge>{}; };
	std::function<AuthTicket(std::string_view token, std::string_view nonce)>
			begin_authenticate;
	// External auth only, called by the FSM once the token verified, before the
	// player is admitted: `resolve_name` picks the final in-game name (the
	// session suffixes collisions: alex -> alex#2); `join_veto` is the pack's
	// `player_join(name, login)` veto. Defaults admit under the verified name.
	// Wall clock (unix seconds) for re-auth freshness checks; tests override.
	std::function<std::int64_t()> unix_time = [] {
		return static_cast<std::int64_t>(std::time(nullptr));
	};
	std::function<std::string(std::string_view name, const LoginData &login)>
			resolve_name = [](std::string_view name, const LoginData &) {
		return std::string(name);
	};
	std::function<bool(std::string_view name, const LoginData *login)> join_veto =
			[](std::string_view, const LoginData *) { return true; };
	std::function<JoinGrant(std::string_view name)> on_ready =
			[](std::string_view) { return JoinGrant{}; };

	// Snapshot of the world's current block registry, sent as
	// S2C_BlockRegistry between C2S_Ready and S2C_JoinAccept (spec §8.3,
	// Phase 4.3). `nullopt` (default) sends no frame at all -- the client
	// already assumes vb::world::BlockRegistry::base() until told otherwise,
	// so hosts/tests that don't care about this get zero behavior change.
	std::function<std::optional<std::vector<protocol::BlockRegistryRecord>>()>
			block_registry = [] {
		return std::optional<std::vector<protocol::BlockRegistryRecord>>{};
	};

	// Effective physics::MoveParams, sent as S2C_MoveParams alongside
	// block_registry above (spec §7.3, Phase 6.7) so client-side prediction
	// uses the exact same tunables as the server's authoritative simulation
	// instead of silently drifting from vb::physics::MoveParams's hardcoded
	// defaults. `nullopt` (default) sends no frame at all -- the client keeps
	// whatever MoveParams it was already constructed with, so hosts/tests
	// that don't care about this see zero behavior change.
	std::function<std::optional<protocol::S2CMoveParams>()> move_params = [] {
		return std::optional<protocol::S2CMoveParams>{};
	};

	// A pack-overridden day/night gradient, sent as S2C_DayNightCurve
	// alongside move_params above (spec §5.4, Phase 6.8). `nullopt` (default)
	// sends no frame at all -- the client keeps rendering
	// vb::world::default_day_night_curve(), so hosts/tests that don't care
	// about this see zero behavior change.
	std::function<std::optional<std::vector<protocol::DayNightKeyframeRecord>>()>
			day_night_curve = [] {
		return std::optional<std::vector<protocol::DayNightKeyframeRecord>>{};
	};

	// A pack-overridden fog distance, sent as S2C_FogParams alongside
	// day_night_curve above (spec §7.2, Phase 7.2). `nullopt` (default) sends
	// no frame at all -- the client computes its own default fog distance
	// from its own view_distance config, so hosts/tests that don't care about
	// this see zero behavior change (there's no server-side universal
	// default to send unprompted, unlike move_params/day_night_curve).
	std::function<std::optional<protocol::S2CFogParams>()> fog_params = [] {
		return std::optional<protocol::S2CFogParams>{};
	};

	// Pack-registered custom keybind names, sent as S2C_KeybindRegistry
	// alongside block_registry above (spec §10.6, Phase 6.3) -- `names[i]`
	// becomes bit i of every InputCmd::keybinds from then on. `nullopt`
	// (default) sends no frame at all: no host/test that doesn't use this
	// channel sees any behavior change.
	std::function<std::optional<std::vector<std::string>>()> keybind_registry =
			[] { return std::optional<std::vector<std::string>>{}; };

	// Pack-registered `vb.register_entity{...}` kinds, sent as
	// S2C_EntityKindRegistry alongside block_registry above (entity-management
	// follow-up to Phase 6.1). `kinds[i]` describes EntityKindId `i + 1`.
	// `nullopt` (default) sends no frame at all: no host/test that doesn't use
	// this channel sees any behavior change, and every remote entity keeps
	// rendering as the flat placeholder billboard it always has.
	std::function<std::optional<std::vector<protocol::EntityKindRegistryRecord>>()>
			entity_kind_registry = [] {
		return std::optional<std::vector<protocol::EntityKindRegistryRecord>>{};
	};

	// Built once at server startup (assetsync::build_manifest over the
	// content pack) and handed to every connection by reference -- never
	// rebuilt per connection. `nullptr` (default) skips asset sync entirely
	// (also what a VB_WITH_COMPRESSION-disabled build gets): the server
	// replies with an empty S2C_AssetManifest and moves straight on.
	std::function<std::shared_ptr<const assetsync::Manifest>()> asset_manifest =
			[] { return std::shared_ptr<const assetsync::Manifest>{}; };

	// Raw bytes of one pack file by hash, called lazily once per hash a
	// client actually requests (the common reconnect case -- nothing
	// missing -- never touches disk). `nullopt` should only happen for a
	// hash outside the manifest we just sent (client misbehavior/corruption)
	// and is treated as a hard disconnect.
	std::function<std::optional<std::vector<std::byte>>(core::AssetHash)>
			asset_file_bytes = [](core::AssetHash) { return std::nullopt; };
};

struct ServerHandshakeStep {
	std::vector<OutgoingFrame> send;
	bool completed = false; // promote the connection to Playing
	bool disconnect = false;
	protocol::DisconnectReason disconnect_reason =
			protocol::DisconnectReason::kUnknown;
	std::string player_name; // valid once completed
};

class ServerHandshake {
public:
	ServerHandshake(HandshakeServerConfig config, HandshakeServerHost host);

	ServerHandshakeState state() const { return state_; }
	const std::string &player_name() const { return player_name_; }
	// External auth: the verified identity once the token was accepted (null
	// under auth_mode none/token, and before verification completes).
	const std::shared_ptr<const LoginData> &login() const { return login_; }
	// Valid once state() == kPlaying: the grant sent in S2C_JoinAccept.
	const JoinGrant &grant() const { return grant_; }

	// Feed one decoded frame from this connection.
	ServerHandshakeStep on_frame(const protocol::Frame &frame);

	// Call when `elapsed_seconds` since connect exceeds the timeout.
	ServerHandshakeStep on_timeout();

	// Seconds the connection may sit in its current state before on_timeout()
	// should be called (measured from the last state change).
	double timeout_seconds() const;

	// Call once per tick while state() == kVerifyingAuth: resolves the pending
	// ticket. A no-op step in any other state or while the ticket is pending.
	ServerHandshakeStep poll_auth();

	// Call once per tick (not just when a frame arrives) while
	// state() == kStreamingAssets: sends up to `max_chunks` more
	// S2C_AssetData frames (spec §9.3's per-tick pacing), transitioning to
	// kAwaitingReady once every requested hash has been fully sent. A no-op
	// step in any other state.
	ServerHandshakeStep pump_assets(int max_chunks);

private:
	ServerHandshakeStep fail(protocol::DisconnectReason reason,
			const std::string &human_message);
	ServerHandshakeStep finish_auth(const AuthOutcome &outcome);
	ServerHandshakeStep finish_external_auth(AuthOutcome outcome);

	HandshakeServerConfig config_;
	HandshakeServerHost host_;
	ServerHandshakeState state_ = ServerHandshakeState::kAwaitingHello;
	std::string player_name_;
	std::string requested_name_; // C2SAuth::player_name, used when resolved_name is empty
	std::string auth_nonce_; // external auth: nonce sent in S2C_AuthChallenge
	AuthTicket auth_ticket_;
	std::shared_ptr<const LoginData> login_;
	JoinGrant grant_;

	std::shared_ptr<const assetsync::Manifest> manifest_;
	struct AssetStreamState {
		std::vector<core::AssetHash> pending;
		std::size_t pending_idx = 0;
		std::vector<std::byte> current_bytes; // fetched lazily via host_.asset_file_bytes
		std::uint32_t current_chunk_idx = 0;
		std::uint32_t current_total_chunks = 0;
		bool current_loaded = false;
	};
	AssetStreamState asset_stream_;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

enum class ClientHandshakeStatus : std::uint8_t {
	kConnecting, // sent Hello, awaiting ServerInfo
	kAwaitingChallenge, // external auth: awaiting S2C_AuthChallenge
	kSigningIn, // external auth: the player is signing in (async; poll())
	kAuthenticating, // sent Auth, awaiting AuthResult
	kAwaitingAssetManifest, // sent C2S_AssetManifestRequest, awaiting S2C_AssetManifest
	kSyncingAssets, // sent C2S_AssetRequest, receiving S2C_AssetData frames
	kSyncing, // sent Ready, awaiting JoinAccept
	kJoined,
	kFailed,
};

// Result of polling an asynchronous sign-in (client, external auth).
// `done == false` = still in progress. When done, a non-empty `error` (or an
// empty `token`) means the sign-in failed or was cancelled.
struct TokenPoll {
	bool done = false;
	std::string token;
	std::string error;
};
using TokenTicket = std::function<TokenPoll()>;

// Asset-sync side of the client handshake (parallel to HandshakeServerHost).
// ClientHandshake has no filesystem access itself -- these hooks push the
// actual cache mechanics (compute-missing / verify / assemble) out to
// ClientSession's ClientAssetCache. Defaults behave as "I already have
// everything" (asset sync is skipped), so every existing
// ClientHandshake(config) call site keeps compiling unchanged.
struct HandshakeClientHost {
	std::function<core::AssetHash()> last_known_manifest_hash = [] {
		return core::AssetHash{};
	};
	// Gets the whole S2C_AssetManifest: on the reconnect fast path its entry
	// list is empty and only manifest_hash says which files the client needs.
	std::function<std::vector<core::AssetHash>(const protocol::S2CAssetManifest &)>
			assets_missing = [](const protocol::S2CAssetManifest &) {
		return std::vector<core::AssetHash>{};
	};
	std::function<bool(const protocol::S2CAssetData &)> on_asset_chunk =
			[](const protocol::S2CAssetData &) { return true; };
	std::function<bool()> assets_all_received = [] { return true; };

	// auth_mode == kExternal only: turns the server's challenge into an ID
	// token (the nonce must be bound into the sign-in request). Returning
	// nullopt/empty cancels the join. When unset, HandshakeClientConfig::token is used.
	// Synchronous for now; Phase 9.5 adds the interactive sign-in screen.
	std::function<std::optional<std::string>(const protocol::S2CAuthChallenge &)>
			obtain_token;
	// Preferred over obtain_token when set: starts an asynchronous sign-in
	// (browser round trip, a form, a token file) and returns a ticket the
	// handshake polls once per tick (ClientHandshakeStatus::kSigningIn).
	std::function<TokenTicket(const protocol::S2CAuthChallenge &)> begin_sign_in;
};

struct HandshakeClientConfig {
	std::string player_name = "Player";
	std::string token;
	std::string client_version = "voxel_browser";
	std::uint64_t client_nonce = 0;
	// Extra protocol::kClientFlag* bits to send in C2SHello. A build with
	// VB_WITH_AUTOMATION always adds kClientFlagAutomation on top of this;
	// tests set it directly to exercise the server policy in any build.
	std::uint8_t client_flags = 0;
};

struct ClientHandshakeStep {
	std::vector<OutgoingFrame> send;
	bool completed = false;
	bool failed = false;
	std::string failure_reason;
};

class ClientHandshake {
public:
	explicit ClientHandshake(HandshakeClientConfig config,
			HandshakeClientHost host = {});

	ClientHandshakeStatus status() const { return status_; }

	// Frames to send immediately once the transport reports Connected.
	ClientHandshakeStep start();

	ClientHandshakeStep on_frame(const protocol::Frame &frame);

	// Call once per tick: resolves a pending sign-in (kSigningIn). A no-op in
	// every other status.
	ClientHandshakeStep poll();
	// Aborts a pending sign-in: the join fails with "sign-in cancelled".
	ClientHandshakeStep cancel_sign_in();
	// Installs the async sign-in hook after construction (the challenge only
	// arrives once connected, so any time before connect works).
	void set_sign_in_provider(
			std::function<TokenTicket(const protocol::S2CAuthChallenge &)> provider) {
		host_.begin_sign_in = std::move(provider);
	}
	const std::optional<protocol::S2CAuthChallenge> &auth_challenge() const {
		return challenge_;
	}

	const std::optional<protocol::S2CServerInfo> &server_info() const {
		return server_info_;
	}
	const std::optional<protocol::S2CJoinAccept> &join_accept() const {
		return join_accept_;
	}
	// Name the server accepted (S2C_AuthResult.resolved_name); empty if it
	// did not override the requested one.
	const std::string &resolved_name() const { return resolved_name_; }

private:
	ClientHandshakeStep fail(std::string reason);
	ClientHandshakeStep send_auth(std::string token);

	HandshakeClientConfig config_;
	HandshakeClientHost host_;
	ClientHandshakeStatus status_ = ClientHandshakeStatus::kConnecting;
	std::optional<protocol::S2CServerInfo> server_info_;
	std::optional<protocol::S2CJoinAccept> join_accept_;
	std::string resolved_name_;
	std::optional<protocol::S2CAuthChallenge> challenge_;
	TokenTicket sign_in_ticket_;
};

// Payloads smaller than this never get LZ4-framed even when
// VB_WITH_COMPRESSION is on: LZ4's own block format (plus the 4-byte
// original-size prefix compress_lz4() adds) has enough fixed overhead that a
// small message (an input batch, a single-block edit result) would come out
// the same size or bigger, all for a wasted round trip through the
// compressor on both ends.
inline constexpr std::size_t kCompressionThresholdBytes = 128;

// Shared helper: frame a message into an OutgoingFrame on its natural lane.
// Above kCompressionThresholdBytes, and only when compressing genuinely
// shrinks the payload (never assumed -- an already-dense payload, e.g. a
// mostly-solid chunk's RLE'd blob past a certain point, can come back the
// same size or larger), this LZ4-frames the payload and sets
// MessageFlag::kCompressed so the receiver reverses it before decoding (see
// ARCHITECTURE_SPEC.md §18 Q4). Resolves generically for every message type
// through this one shared helper, not just chunk messages specifically --
// S2C_ChunkAdd/S2C_ChunkDelta are simply the frequent, large-payload case
// that motivated it.
template <typename Msg>
OutgoingFrame frame_message(const Msg &msg, std::uint16_t flags = 0) {
	std::vector<std::byte> payload;
	msg.encode(payload);
#if VB_WITH_COMPRESSION
	if (payload.size() >= kCompressionThresholdBytes) {
		std::vector<std::byte> compressed = protocol::compress_lz4(payload);
		if (compressed.size() < payload.size()) {
			payload = std::move(compressed);
			flags |= static_cast<std::uint16_t>(protocol::MessageFlag::kCompressed);
		}
	}
#endif
	OutgoingFrame out;
	out.lane = protocol::lane_for(Msg::kType);
	protocol::write_frame(out.bytes, Msg::kType, payload, flags);
	return out;
}

} // namespace vb::net
