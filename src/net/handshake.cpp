#include "vb/net/handshake.hpp"

#include <algorithm>
#include <utility>

#include "vb/core/log.hpp"
#include "vb/core/version.hpp"
#include "vb/core/version_req.hpp"
#include "vb/protocol/input.hpp" // S2CKeybindRegistry

namespace vb::net {

using protocol::DisconnectReason;
using protocol::Frame;
using protocol::MessageType;

namespace {

OutgoingFrame disconnect_frame(DisconnectReason reason, std::string message) {
	protocol::S2CDisconnect msg;
	msg.reason = reason;
	msg.message = std::move(message);
	return frame_message(msg);
}

constexpr std::size_t kAssetChunkBytes = 48u * 1024u;

} // namespace

// ===========================================================================
// ServerHandshake
// ===========================================================================

ServerHandshake::ServerHandshake(HandshakeServerConfig config,
		HandshakeServerHost host) : config_(std::move(config)),
									host_(std::move(host)) {}

ServerHandshakeStep ServerHandshake::fail(DisconnectReason reason,
		const std::string &human_message) {
	VB_DEBUG("net", "handshake rejected: ", human_message);
	state_ = ServerHandshakeState::kClosed;
	ServerHandshakeStep step;
	step.send.push_back(disconnect_frame(reason, human_message));
	step.disconnect = true;
	step.disconnect_reason = reason;
	return step;
}

ServerHandshakeStep ServerHandshake::on_timeout() {
	if (state_ == ServerHandshakeState::kPlaying ||
			state_ == ServerHandshakeState::kClosed) {
		return {};
	}
	return fail(DisconnectReason::kTimeout, "handshake timed out");
}

double ServerHandshake::timeout_seconds() const {
	if (config_.auth_mode == protocol::AuthMode::kExternal &&
			(state_ == ServerHandshakeState::kAwaitingAuth ||
					state_ == ServerHandshakeState::kVerifyingAuth)) {
		return config_.auth_timeout_seconds;
	}
	return config_.handshake_timeout_seconds;
}

ServerHandshakeStep ServerHandshake::poll_auth() {
	if (state_ != ServerHandshakeState::kVerifyingAuth || !auth_ticket_) {
		return {};
	}
	const std::optional<AuthOutcome> outcome = auth_ticket_();
	if (!outcome) {
		return {};
	}
	auth_ticket_ = nullptr;
	return finish_external_auth(*outcome);
}

ServerHandshakeStep ServerHandshake::finish_external_auth(AuthOutcome outcome) {
	if (outcome.ok) {
		if (!outcome.login) {
			// A verifier that says "ok" without an identity is a bug; never
			// admit an anonymous player on an authenticating server.
			return finish_auth({ false, "authentication failed", {}, {} });
		}
		std::string name = host_.resolve_name(
				outcome.resolved_name.empty() ? outcome.login->name : outcome.resolved_name,
				*outcome.login);
		// The pack sees the final in-game name in both `name` and `login.name`.
		auto login = std::make_shared<LoginData>(*outcome.login);
		login->name = name;
		if (!host_.join_veto(name, login.get())) {
			return finish_auth({ false, "denied by pack", {}, {} });
		}
		login_ = std::move(login);
		outcome.resolved_name = std::move(name);
	}
	return finish_auth(outcome);
}

ServerHandshakeStep ServerHandshake::finish_auth(const AuthOutcome &outcome) {
	ServerHandshakeStep step;
	protocol::S2CAuthResult result;
	result.ok = outcome.ok;
	result.reason = outcome.reason;
	result.resolved_name = outcome.ok && !outcome.resolved_name.empty()
			? outcome.resolved_name
			: requested_name_;
	step.send.push_back(frame_message(result));
	if (!outcome.ok) {
		step.send.push_back(disconnect_frame(DisconnectReason::kAuthFailed,
				outcome.reason.empty() ? "authentication failed" : outcome.reason));
		step.disconnect = true;
		step.disconnect_reason = DisconnectReason::kAuthFailed;
		state_ = ServerHandshakeState::kClosed;
		return step;
	}
	player_name_ = result.resolved_name;
	state_ = ServerHandshakeState::kAwaitingAssetManifestRequest;
	return step;
}

ServerHandshakeStep ServerHandshake::pump_assets(int max_chunks) {
	ServerHandshakeStep step;
	if (state_ != ServerHandshakeState::kStreamingAssets) {
		return step;
	}

	for (int sent = 0; sent < max_chunks; ++sent) {
		if (asset_stream_.pending_idx >= asset_stream_.pending.size()) {
			state_ = ServerHandshakeState::kAwaitingReady;
			return step;
		}
		const core::AssetHash hash = asset_stream_.pending[asset_stream_.pending_idx];

		if (!asset_stream_.current_loaded) {
			auto bytes = host_.asset_file_bytes(hash);
			if (!bytes) {
				// Client asked for a hash outside the manifest we sent it --
				// misbehavior or corruption, not recoverable.
				return fail(DisconnectReason::kProtocolError,
						"requested asset hash not found");
			}
			asset_stream_.current_bytes = std::move(*bytes);
			asset_stream_.current_chunk_idx = 0;
			const std::size_t size = asset_stream_.current_bytes.size();
			asset_stream_.current_total_chunks = static_cast<std::uint32_t>(
					size == 0 ? 1 : (size + kAssetChunkBytes - 1) / kAssetChunkBytes);
			asset_stream_.current_loaded = true;
		}

		const std::size_t offset =
				static_cast<std::size_t>(asset_stream_.current_chunk_idx) * kAssetChunkBytes;
		const std::size_t remaining = asset_stream_.current_bytes.size() - offset;
		const std::size_t chunk_len = std::min(remaining, kAssetChunkBytes);

		protocol::S2CAssetData data;
		data.hash = hash;
		data.seq = asset_stream_.current_chunk_idx;
		data.total_chunks = asset_stream_.current_total_chunks;
		data.bytes.assign(asset_stream_.current_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
				asset_stream_.current_bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk_len));
		step.send.push_back(frame_message(data));

		++asset_stream_.current_chunk_idx;
		if (asset_stream_.current_chunk_idx >= asset_stream_.current_total_chunks) {
			++asset_stream_.pending_idx;
			asset_stream_.current_bytes.clear();
			asset_stream_.current_loaded = false;
			// Leave kStreamingAssets the moment the last chunk is queued, not
			// on the next call: if this chunk used up the tick's budget, the
			// client's Ready (sent as soon as it has every asset) could
			// otherwise arrive while we're still "streaming" and be rejected
			// as an unexpected message.
			if (asset_stream_.pending_idx >= asset_stream_.pending.size()) {
				state_ = ServerHandshakeState::kAwaitingReady;
				return step;
			}
		}
	}
	return step;
}

ServerHandshakeStep ServerHandshake::on_frame(const Frame &frame) {
	const MessageType type = frame.header.type;

	switch (state_) {
		case ServerHandshakeState::kAwaitingHello: {
			if (type != MessageType::kC2SHello) {
				return fail(DisconnectReason::kBadHandshake, "expected Hello");
			}
			auto hello = protocol::C2SHello::decode(frame.payload);
			if (!hello) {
				return fail(DisconnectReason::kProtocolError, "malformed Hello");
			}
			if (hello->engine_protocol_version != kEngineProtocolVersion) {
				return fail(DisconnectReason::kProtocolMismatch,
						"engine protocol version mismatch");
			}

			if ((hello->client_flags & protocol::kClientFlagAutomation) != 0 &&
					!config_.accept_automation_clients) {
				// Refused before ServerInfo, so before auth and asset sync:
				// a rejected dev client costs this server almost nothing.
				return fail(DisconnectReason::kBadHandshake,
						"automation clients are not accepted by this server");
			}

			protocol::S2CServerInfo info;
			info.pack_name = config_.pack_name;
			info.pack_version = config_.pack_version;
			info.engine_protocol_version = kEngineProtocolVersion;
			info.tick_rate = config_.tick_rate;
			info.view_distance = config_.view_distance;
			info.motd = config_.motd;
			info.auth_mode = config_.auth_mode;
			info.engine_version_req = config_.engine_version_req;
			info.third_person_allowed = config_.third_person_allowed;

			ServerHandshakeStep step;
			if (config_.auth_mode == protocol::AuthMode::kExternal) {
				auto challenge = host_.auth_challenge();
				if (!challenge || challenge->nonce.empty()) {
					// Fail closed: never fall back to unauthenticated play.
					return fail(DisconnectReason::kAuthFailed,
							"server cannot authenticate players right now");
				}
				auth_nonce_ = challenge->nonce;
				step.send.push_back(frame_message(info));
				step.send.push_back(frame_message(*challenge));
			} else {
				step.send.push_back(frame_message(info));
			}
			state_ = ServerHandshakeState::kAwaitingAuth;
			return step;
		}

		case ServerHandshakeState::kAwaitingAuth: {
			if (type != MessageType::kC2SAuth) {
				return fail(DisconnectReason::kBadHandshake, "expected Auth");
			}
			auto auth = protocol::C2SAuth::decode(frame.payload);
			if (!auth) {
				return fail(DisconnectReason::kProtocolError, "malformed Auth");
			}
			if (host_.current_player_count() >= config_.max_players) {
				return fail(DisconnectReason::kServerFull, "server is full");
			}

			requested_name_ = auth->player_name;
			if (config_.auth_mode == protocol::AuthMode::kExternal) {
				if (!host_.begin_authenticate) {
					return fail(DisconnectReason::kAuthFailed,
							"server cannot authenticate players right now");
				}
				auth_ticket_ = host_.begin_authenticate(auth->token, auth_nonce_);
				if (!auth_ticket_) {
					return fail(DisconnectReason::kAuthFailed,
							"server cannot authenticate players right now");
				}
				state_ = ServerHandshakeState::kVerifyingAuth;
				return poll_auth(); // resolves immediately on the fast path
			}
			return finish_auth(host_.authenticate(auth->player_name, auth->token));
		}

		case ServerHandshakeState::kVerifyingAuth:
			// The verdict is still pending; the client must wait for it.
			return fail(DisconnectReason::kBadHandshake,
					"unexpected message while verifying sign-in");

		case ServerHandshakeState::kAwaitingAssetManifestRequest: {
			if (type != MessageType::kC2SAssetManifestRequest) {
				return fail(DisconnectReason::kBadHandshake,
						"expected AssetManifestRequest");
			}
			auto req = protocol::C2SAssetManifestRequest::decode(frame.payload);
			if (!req) {
				return fail(DisconnectReason::kProtocolError,
						"malformed AssetManifestRequest");
			}

			manifest_ = host_.asset_manifest(); // may be null (opt-out / disabled)
			protocol::S2CAssetManifest reply;
			if (manifest_) {
				reply.manifest_hash = manifest_->manifest_hash;
				reply.total_bytes = manifest_->total_bytes;
				if (req->known_manifest_hash != manifest_->manifest_hash) {
					reply.entries.reserve(manifest_->entries.size());
					for (const auto &e : manifest_->entries) {
						reply.entries.push_back({ e.path, e.hash, e.size,
								static_cast<protocol::AssetKind>(e.kind) });
					}
				}
				// else: reconnect fast path -- entries stay empty, client
				// already has everything for this manifest_hash.
			}

			state_ = ServerHandshakeState::kAwaitingAssetRequest;
			ServerHandshakeStep step;
			step.send.push_back(frame_message(reply));
			return step;
		}

		case ServerHandshakeState::kAwaitingAssetRequest: {
			if (type != MessageType::kC2SAssetRequest) {
				return fail(DisconnectReason::kBadHandshake, "expected AssetRequest");
			}
			auto req = protocol::C2SAssetRequest::decode(frame.payload);
			if (!req) {
				return fail(DisconnectReason::kProtocolError, "malformed AssetRequest");
			}

			asset_stream_ = AssetStreamState{};
			asset_stream_.pending = std::move(req->missing);
			if (asset_stream_.pending.empty()) {
				state_ = ServerHandshakeState::kAwaitingReady;
				return {};
			}
			state_ = ServerHandshakeState::kStreamingAssets;
			return {}; // first bytes go out from the next pump_assets() tick
		}

		case ServerHandshakeState::kStreamingAssets:
			// No client message is expected while streaming; the server
			// paces itself via pump_assets(), called from ServerSession's
			// tick loop, not in response to a frame.
			return fail(DisconnectReason::kBadHandshake,
					"unexpected message while streaming assets");

		case ServerHandshakeState::kAwaitingReady: {
			if (type != MessageType::kC2SReady) {
				return fail(DisconnectReason::kBadHandshake, "expected Ready");
			}
			if (!protocol::C2SReady::decode(frame.payload)) {
				return fail(DisconnectReason::kProtocolError, "malformed Ready");
			}

			grant_ = host_.on_ready(player_name_);

			state_ = ServerHandshakeState::kPlaying;
			ServerHandshakeStep step;
			if (auto records = host_.block_registry()) {
				step.send.push_back(
						frame_message(protocol::S2CBlockRegistry{ std::move(*records) }));
			}
			if (auto names = host_.keybind_registry()) {
				step.send.push_back(
						frame_message(protocol::S2CKeybindRegistry{ std::move(*names) }));
			}
			if (auto kinds = host_.entity_kind_registry()) {
				step.send.push_back(frame_message(
						protocol::S2CEntityKindRegistry{ std::move(*kinds) }));
			}
			if (auto mp = host_.move_params()) {
				step.send.push_back(frame_message(*mp));
			}
			if (auto dnc = host_.day_night_curve()) {
				step.send.push_back(
						frame_message(protocol::S2CDayNightCurve{ std::move(*dnc) }));
			}
			if (auto fp = host_.fog_params()) {
				step.send.push_back(frame_message(*fp));
			}

			protocol::S2CJoinAccept accept;
			accept.your_net_id = grant_.net_id;
			accept.spawn_pos = grant_.spawn_pos;
			accept.world_seed = grant_.world_seed;
			accept.time_of_day = grant_.time_of_day;
			step.send.push_back(frame_message(accept));
			step.completed = true;
			step.player_name = player_name_;
			return step;
		}

		case ServerHandshakeState::kPlaying:
		case ServerHandshakeState::kClosed:
			return fail(DisconnectReason::kBadHandshake,
					"unexpected message after handshake");
	}
	return {};
}

// ===========================================================================
// ClientHandshake
// ===========================================================================

ClientHandshake::ClientHandshake(HandshakeClientConfig config,
		HandshakeClientHost host) : config_(std::move(config)),
									host_(std::move(host)) {}

ClientHandshakeStep ClientHandshake::fail(std::string reason) {
	status_ = ClientHandshakeStatus::kFailed;
	ClientHandshakeStep step;
	step.failed = true;
	step.failure_reason = std::move(reason);
	return step;
}

ClientHandshakeStep ClientHandshake::send_auth(std::string token) {
	if (token.size() > protocol::kMaxAuthTokenBytes) {
		return fail("sign-in token too large");
	}
	protocol::C2SAuth auth;
	auth.player_name = config_.player_name;
	auth.token = std::move(token);
	status_ = ClientHandshakeStatus::kAuthenticating;
	ClientHandshakeStep step;
	step.send.push_back(frame_message(auth));
	return step;
}

ClientHandshakeStep ClientHandshake::poll() {
	if (status_ != ClientHandshakeStatus::kSigningIn || !sign_in_ticket_) {
		return {};
	}
	TokenPoll p = sign_in_ticket_();
	if (!p.done) {
		return {};
	}
	sign_in_ticket_ = nullptr;
	if (!p.error.empty() || p.token.empty()) {
		return fail(p.error.empty() ? "sign-in cancelled" : p.error);
	}
	return send_auth(std::move(p.token));
}

ClientHandshakeStep ClientHandshake::cancel_sign_in() {
	if (status_ != ClientHandshakeStatus::kSigningIn) {
		return {};
	}
	sign_in_ticket_ = nullptr; // destroys the ticket, which cancels its task
	return fail("sign-in cancelled");
}

ClientHandshakeStep ClientHandshake::start() {
	protocol::C2SHello hello;
	hello.engine_protocol_version = kEngineProtocolVersion;
	hello.client_nonce = config_.client_nonce;
	hello.client_version = config_.client_version;
	hello.client_flags = config_.client_flags;
#if defined(VB_WITH_AUTOMATION)
	hello.client_flags |= protocol::kClientFlagAutomation;
#endif

	status_ = ClientHandshakeStatus::kConnecting;
	ClientHandshakeStep step;
	step.send.push_back(frame_message(hello));
	return step;
}

ClientHandshakeStep ClientHandshake::on_frame(const Frame &frame) {
	const MessageType type = frame.header.type;

	if (type == MessageType::kS2CDisconnect) {
		auto msg = protocol::S2CDisconnect::decode(frame.payload);
		return fail(msg ? msg->message : "disconnected");
	}

	switch (status_) {
		case ClientHandshakeStatus::kConnecting: {
			if (type != MessageType::kS2CServerInfo) {
				return fail("expected ServerInfo");
			}
			auto info = protocol::S2CServerInfo::decode(frame.payload);
			if (!info) {
				return fail("malformed ServerInfo");
			}
			if (info->engine_protocol_version != kEngineProtocolVersion) {
				return fail("engine protocol version mismatch");
			}
			if (!info->engine_version_req.empty()) {
				// A pack's ui/*.lua runs on *this* engine, and two releases can
				// share a protocol version while differing in the UI API, so the
				// requirement is checked before any asset is downloaded. An
				// unparseable requirement fails closed.
				std::string req_error;
				const auto req = core::VersionReq::parse(info->engine_version_req, &req_error);
				const core::SemVer mine = core::engine_version();
				if (!req) {
					return fail("this server's pack has an invalid engine_version_req (" +
							req_error + ")");
				}
				if (!req->matches(mine)) {
					const auto lower = req->lower_bound();
					return fail("this server's pack needs Voxel Browser " + req->to_string() +
							"; you have " + core::to_string(mine) +
							(lower ? " -- run `vb update`" : ""));
				}
			}
			server_info_ = std::move(*info);

			if (server_info_->auth_mode == protocol::AuthMode::kExternal) {
				status_ = ClientHandshakeStatus::kAwaitingChallenge;
				return {};
			}

			protocol::C2SAuth auth;
			auth.player_name = config_.player_name;
			auth.token = config_.token;

			status_ = ClientHandshakeStatus::kAuthenticating;
			ClientHandshakeStep step;
			step.send.push_back(frame_message(auth));
			return step;
		}

		case ClientHandshakeStatus::kAwaitingChallenge: {
			if (type != MessageType::kS2CAuthChallenge) {
				return fail("expected AuthChallenge");
			}
			auto challenge = protocol::S2CAuthChallenge::decode(frame.payload);
			if (!challenge) {
				return fail("malformed AuthChallenge");
			}
			challenge_ = *challenge;
			if (host_.begin_sign_in) {
				sign_in_ticket_ = host_.begin_sign_in(*challenge);
				if (!sign_in_ticket_) {
					return fail("sign-in is unavailable");
				}
				status_ = ClientHandshakeStatus::kSigningIn;
				return poll(); // a token file resolves on the first poll
			}
			std::optional<std::string> token;
			if (host_.obtain_token) {
				token = host_.obtain_token(*challenge);
			} else if (!config_.token.empty()) {
				token = config_.token;
			}
			if (!token || token->empty()) {
				return fail("sign-in required but cancelled or unavailable");
			}
			return send_auth(std::move(*token));
		}

		case ClientHandshakeStatus::kSigningIn:
			// The server sends nothing while the player signs in.
			return fail("unexpected message while signing in");

		case ClientHandshakeStatus::kAuthenticating: {
			if (type != MessageType::kS2CAuthResult) {
				return fail("expected AuthResult");
			}
			auto result = protocol::S2CAuthResult::decode(frame.payload);
			if (!result) {
				return fail("malformed AuthResult");
			}
			if (!result->ok) {
				return fail(result->reason.empty() ? "authentication rejected"
												   : result->reason);
			}

			resolved_name_ = result->resolved_name;
			status_ = ClientHandshakeStatus::kAwaitingAssetManifest;
			ClientHandshakeStep step;
			protocol::C2SAssetManifestRequest req;
			req.known_manifest_hash = host_.last_known_manifest_hash();
			step.send.push_back(frame_message(req));
			return step;
		}

		case ClientHandshakeStatus::kAwaitingAssetManifest: {
			if (type != MessageType::kS2CAssetManifest) {
				return fail("expected AssetManifest");
			}
			auto m = protocol::S2CAssetManifest::decode(frame.payload);
			if (!m) {
				return fail("malformed AssetManifest");
			}
			std::vector<core::AssetHash> missing = host_.assets_missing(*m);

			ClientHandshakeStep step;
			step.send.push_back(frame_message(protocol::C2SAssetRequest{ missing }));
			if (missing.empty()) {
				// Nothing to receive (reconnect fast path or an empty pack) --
				// proceed straight to Ready, mirroring the server's shortcut.
				status_ = ClientHandshakeStatus::kSyncing;
				step.send.push_back(frame_message(protocol::C2SReady{}));
			} else {
				status_ = ClientHandshakeStatus::kSyncingAssets;
			}
			return step;
		}

		case ClientHandshakeStatus::kSyncingAssets: {
			if (type != MessageType::kS2CAssetData) {
				return fail("expected AssetData");
			}
			auto d = protocol::S2CAssetData::decode(frame.payload);
			if (!d) {
				return fail("malformed AssetData");
			}
			if (!host_.on_asset_chunk(*d)) {
				return fail("asset transfer failed (hash mismatch or size cap)");
			}
			if (!host_.assets_all_received()) {
				return {}; // more chunks still expected
			}
			status_ = ClientHandshakeStatus::kSyncing;
			ClientHandshakeStep step;
			step.send.push_back(frame_message(protocol::C2SReady{}));
			return step;
		}

		case ClientHandshakeStatus::kSyncing: {
			if (type != MessageType::kS2CJoinAccept) {
				return fail("expected JoinAccept");
			}
			auto accept = protocol::S2CJoinAccept::decode(frame.payload);
			if (!accept) {
				return fail("malformed JoinAccept");
			}
			join_accept_ = std::move(*accept);

			status_ = ClientHandshakeStatus::kJoined;
			ClientHandshakeStep step;
			step.completed = true;
			return step;
		}

		case ClientHandshakeStatus::kJoined:
		case ClientHandshakeStatus::kFailed:
			return {};
	}
	return {};
}

} // namespace vb::net
