#include "vb/protocol/handshake.hpp"

namespace vb::protocol {

using core::Err;
using core::ProtocolError;

namespace {

template <typename T>
Decoded<T> finish(ByteReader &r, T value) {
	r.expect_consumed();
	if (r.failed()) {
		return Err{ r.error() };
	}
	return value;
}

} // namespace

// --- C2SHello ---------------------------------------------------------------
void C2SHello::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.u16(engine_protocol_version);
	w.u64(client_nonce);
	w.string(client_version);
	w.u8(client_flags);
}

Decoded<C2SHello> C2SHello::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	C2SHello m;
	m.engine_protocol_version = r.u16();
	m.client_nonce = r.u64();
	m.client_version = r.string();
	m.client_flags = r.u8();
	return finish(r, std::move(m));
}

// --- S2CServerInfo ---------------------------------------------------------
void S2CServerInfo::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.string(pack_name);
	w.string(pack_version);
	w.u16(engine_protocol_version);
	w.u16(tick_rate);
	w.u32(view_distance);
	w.string(motd);
	w.u8(static_cast<std::uint8_t>(auth_mode));
	w.string(engine_version_req);
	w.boolean(third_person_allowed);
}

Decoded<S2CServerInfo> S2CServerInfo::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CServerInfo m;
	m.pack_name = r.string();
	m.pack_version = r.string();
	m.engine_protocol_version = r.u16();
	m.tick_rate = r.u16();
	m.view_distance = r.u32();
	m.motd = r.string();
	m.auth_mode = static_cast<AuthMode>(r.u8());
	if (!r.failed() && !valid(m.auth_mode)) {
		r.fail(ProtocolError::kBadEnum);
	}
	m.engine_version_req = r.string();
	if (!r.failed() && m.engine_version_req.size() > kMaxEngineVersionReqBytes) {
		r.fail(ProtocolError::kLengthExceeded);
	}
	m.third_person_allowed = r.boolean();
	return finish(r, std::move(m));
}

// --- C2SAuth -------------------------------------------------------------
void C2SAuth::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.string(player_name);
	w.string(token);
}

Decoded<C2SAuth> C2SAuth::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	C2SAuth m;
	m.player_name = r.string();
	m.token = r.string(kMaxAuthTokenBytes);
	return finish(r, std::move(m));
}

// --- S2CAuthChallenge -----------------------------------------------------
void S2CAuthChallenge::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.string(provider);
	w.string(display_name);
	w.string(issuer);
	w.string(client_id);
	w.varint(scopes.size());
	for (const auto &s : scopes) {
		w.string(s);
	}
	w.varint(params.size());
	for (const auto &[k, v] : params) {
		w.string(k);
		w.string(v);
	}
	w.string(nonce);
}

Decoded<S2CAuthChallenge> S2CAuthChallenge::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CAuthChallenge m;
	m.provider = r.string(kMaxAuthFieldBytes);
	m.display_name = r.string(kMaxAuthFieldBytes);
	m.issuer = r.string(kMaxAuthFieldBytes);
	m.client_id = r.string(kMaxAuthFieldBytes);
	const std::uint64_t ns = r.varint();
	if (!r.failed() && ns > kMaxAuthListEntries) {
		return Err{ ProtocolError::kLengthExceeded };
	}
	for (std::uint64_t i = 0; i < ns && !r.failed(); ++i) {
		m.scopes.push_back(r.string(kMaxAuthFieldBytes));
	}
	const std::uint64_t np = r.varint();
	if (!r.failed() && np > kMaxAuthListEntries) {
		return Err{ ProtocolError::kLengthExceeded };
	}
	for (std::uint64_t i = 0; i < np && !r.failed(); ++i) {
		std::string k = r.string(kMaxAuthFieldBytes);
		std::string v = r.string(kMaxAuthFieldBytes);
		m.params.emplace_back(std::move(k), std::move(v));
	}
	m.nonce = r.string(kMaxAuthNonceBytes);
	return finish(r, std::move(m));
}

// --- S2CReauthRequest / C2SReauth ----------------------------------------
void S2CReauthRequest::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.string(nonce);
	w.u16(grace_seconds);
}

Decoded<S2CReauthRequest> S2CReauthRequest::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CReauthRequest m;
	m.nonce = r.string(kMaxAuthNonceBytes);
	m.grace_seconds = r.u16();
	return finish(r, std::move(m));
}

void C2SReauth::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.string(token);
}

Decoded<C2SReauth> C2SReauth::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	C2SReauth m;
	m.token = r.string(kMaxAuthTokenBytes);
	return finish(r, std::move(m));
}

// --- S2CAuthResult -----------------------------------------------------
void S2CAuthResult::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.boolean(ok);
	w.string(reason);
	w.string(resolved_name);
}

Decoded<S2CAuthResult> S2CAuthResult::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CAuthResult m;
	m.ok = r.boolean();
	m.reason = r.string();
	m.resolved_name = r.string();
	return finish(r, std::move(m));
}

// --- C2SReady ---------------------------------------------------------
void C2SReady::encode(std::vector<std::byte> &out) const { (void)out; }

Decoded<C2SReady> C2SReady::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	return finish(r, C2SReady{});
}

// --- S2CJoinAccept ---------------------------------------------------
void S2CJoinAccept::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.u32(static_cast<std::uint32_t>(your_net_id));
	w.f64(spawn_pos.x);
	w.f64(spawn_pos.y);
	w.f64(spawn_pos.z);
	w.u64(world_seed);
	w.u32(time_of_day);
}

Decoded<S2CJoinAccept> S2CJoinAccept::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CJoinAccept m;
	m.your_net_id = static_cast<core::NetId>(r.u32());
	m.spawn_pos.x = r.f64();
	m.spawn_pos.y = r.f64();
	m.spawn_pos.z = r.f64();
	m.world_seed = r.u64();
	m.time_of_day = r.u32();
	return finish(r, std::move(m));
}

// --- S2CDisconnect -------------------------------------------------
void S2CDisconnect::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.u8(static_cast<std::uint8_t>(reason));
	w.string(message);
}

Decoded<S2CDisconnect> S2CDisconnect::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CDisconnect m;
	m.reason = static_cast<DisconnectReason>(r.u8());
	m.message = r.string();
	if (!r.failed() && !valid(m.reason)) {
		r.fail(ProtocolError::kBadEnum);
	}
	return finish(r, std::move(m));
}

} // namespace vb::protocol
