#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"
#include "vb/core/result.hpp"
#include "vb/protocol/byte_buffer.hpp"
#include "vb/protocol/message.hpp"

// Connection handshake payloads (spec §8.3). Every struct has:
//   void encode(std::vector<std::byte>&) const;   // appends the payload
//   static Result<T, ProtocolError> decode(std::span<const std::byte>);
// and a round-trip test in tests/unit/protocol_test.cpp.

namespace vb::protocol {

template <typename T>
using Decoded = core::Result<T, core::ProtocolError>;

enum class AuthMode : std::uint8_t {
	kNone = 0,
	kToken = 1,
	// Pack-level auth.lua: the client signs in with an external identity
	// provider and the server verifies the ID token (docs/auth.md design:
	// architecture_spec/auth.md). Followed by S2C_AuthChallenge.
	kExternal = 2,
};
constexpr bool valid(AuthMode m) {
	return m == AuthMode::kNone || m == AuthMode::kToken ||
			m == AuthMode::kExternal;
}

// Wire bounds for the external-auth messages (auth.md §5.2).
inline constexpr std::size_t kMaxAuthTokenBytes = 16u * 1024u;
inline constexpr std::size_t kMaxAuthFieldBytes = 2048;
inline constexpr std::size_t kMaxAuthNonceBytes = 256;
inline constexpr std::size_t kMaxAuthListEntries = 16;

enum class DisconnectReason : std::uint8_t {
	kUnknown = 0,
	kServerFull,
	kProtocolMismatch,
	kAuthFailed,
	kServerShutdown,
	kKicked,
	kTimeout,
	kProtocolError,
	kBadHandshake,
};
constexpr bool valid(DisconnectReason r) {
	return static_cast<std::uint8_t>(r) <= static_cast<std::uint8_t>(DisconnectReason::kBadHandshake);
}

// C2SHello::client_flags bits. Unknown bits are ignored by the receiver.
// kClientFlagAutomation is set by clients built with VB_WITH_AUTOMATION
// (docs/e2e-automation.md §7.4): a server built without it refuses them so a
// dev/test client can never join a production server by accident.
inline constexpr std::uint8_t kClientFlagAutomation = 1u << 0;

struct C2SHello {
	static constexpr MessageType kType = MessageType::kC2SHello;
	std::uint16_t engine_protocol_version = 0;
	std::uint64_t client_nonce = 0;
	std::string client_version;
	std::uint8_t client_flags = 0; // kClientFlag* bits (protocol v27+)

	void encode(std::vector<std::byte> &out) const;
	static Decoded<C2SHello> decode(std::span<const std::byte> in);
};

// Cap on S2CServerInfo::engine_version_req (a short comparator list).
inline constexpr std::size_t kMaxEngineVersionReqBytes = 128;

struct S2CServerInfo {
	static constexpr MessageType kType = MessageType::kS2CServerInfo;
	std::string pack_name;
	std::string pack_version;
	std::uint16_t engine_protocol_version = 0;
	std::uint16_t tick_rate = 20;
	// The server's own configured chunk view distance (server.toml's
	// view_distance) -- unlike fog_params/move_params, there's no opt-in
	// "nullopt means unset" here, the server always knows this about itself.
	// Lets a client clamp its own (possibly larger) render_distance so its
	// default fog distance never reaches past chunks the server will never
	// actually stream to it (see src/client/main.cpp's enter_playing).
	std::uint32_t view_distance = 8;
	std::string motd;
	AuthMode auth_mode = AuthMode::kNone;
	// The pack's `engine_version_req` (pack.toml; protocol v30+), e.g. ">=0.6.0".
	// Empty = no requirement. The client refuses to continue when its own
	// version does not satisfy it, before downloading any asset.
	std::string engine_version_req;
	// `vb.render.set_third_person(false)` (protocol v32): whether the client
	// may switch to its third-person camera (F5).
	bool third_person_allowed = true;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CServerInfo> decode(std::span<const std::byte> in);
};

struct C2SAuth {
	static constexpr MessageType kType = MessageType::kC2SAuth;
	std::string player_name;
	std::string token; // empty when auth_mode == kNone

	void encode(std::vector<std::byte> &out) const;
	static Decoded<C2SAuth> decode(std::span<const std::byte> in);
};

// Sent right after S2C_ServerInfo when auth_mode == kExternal: tells the
// client which identity provider to sign in with and the server's one-time
// nonce, which the client must bind into the sign-in request so the resulting
// ID token cannot be replayed against another connection.
struct S2CAuthChallenge {
	static constexpr MessageType kType = MessageType::kS2CAuthChallenge;
	std::string provider; // "oidc" | "keycloak" | "firebase"
	std::string display_name;
	std::string issuer;
	std::string client_id;
	std::vector<std::string> scopes;
	std::vector<std::pair<std::string, std::string>> params; // preset extras
	std::string nonce; // base64url, <= kMaxAuthNonceBytes

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CAuthChallenge> decode(std::span<const std::byte> in);
};

// Periodic live re-authentication (wire format only until Phase 9.6).
struct S2CReauthRequest {
	static constexpr MessageType kType = MessageType::kS2CReauthRequest;
	std::string nonce;
	std::uint16_t grace_seconds = 0;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CReauthRequest> decode(std::span<const std::byte> in);
};

struct C2SReauth {
	static constexpr MessageType kType = MessageType::kC2SReauth;
	std::string token;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<C2SReauth> decode(std::span<const std::byte> in);
};

struct S2CAuthResult {
	static constexpr MessageType kType = MessageType::kS2CAuthResult;
	bool ok = false;
	std::string reason;
	std::string resolved_name; // name the server will use (kExternal: from name_claim)

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CAuthResult> decode(std::span<const std::byte> in);
};

struct C2SReady {
	static constexpr MessageType kType = MessageType::kC2SReady;
	void encode(std::vector<std::byte> &out) const;
	static Decoded<C2SReady> decode(std::span<const std::byte> in);
};

struct S2CJoinAccept {
	static constexpr MessageType kType = MessageType::kS2CJoinAccept;
	core::NetId your_net_id = core::NetId::kInvalid;
	core::Vec3d spawn_pos{};
	std::uint64_t world_seed = 0;
	std::uint32_t time_of_day = 0; // ticks into the day cycle

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CJoinAccept> decode(std::span<const std::byte> in);
};

struct S2CDisconnect {
	static constexpr MessageType kType = MessageType::kS2CDisconnect;
	DisconnectReason reason = DisconnectReason::kUnknown;
	std::string message;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CDisconnect> decode(std::span<const std::byte> in);
};

} // namespace vb::protocol
