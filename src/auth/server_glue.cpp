#include "vb/auth/server_glue.hpp"

#include <optional>
#include <string>

#include "vb/core/log.hpp"

namespace vb::auth {

void apply_external_auth(net::HandshakeServerConfig &config, const AuthConfig &auth) {
	config.auth_mode = protocol::AuthMode::kExternal;
	config.reauth_interval_seconds = auth.reauth_interval_seconds;
	config.reauth_grace_seconds = auth.reauth_grace_seconds;
}

std::shared_ptr<AuthService> install_external_auth(net::HandshakeServerHost &host,
		const AuthConfig &auth, std::shared_ptr<HttpFetcher> http) {
	auto service = std::make_shared<AuthService>(auth, std::move(http));
	service->start();

	host.auth_challenge = [service, auth]() -> std::optional<protocol::S2CAuthChallenge> {
		protocol::S2CAuthChallenge c;
		c.nonce = service->new_nonce();
		if (c.nonce.empty()) {
			return std::nullopt; // no entropy: fail closed
		}
		c.provider = std::string(provider_name(auth.provider));
		c.display_name = auth.display_name;
		c.issuer = auth.issuer;
		c.client_id = auth.client_id;
		c.scopes = auth.scopes;
		if (auth.provider == Provider::kFirebase) {
			c.params.emplace_back("project_id", auth.project_id);
			c.params.emplace_back("api_key", auth.api_key);
			std::string methods;
			for (const auto m : auth.sign_in) {
				methods += methods.empty() ? "" : ",";
				methods += m == FirebaseSignIn::kPassword ? "password" : "google";
			}
			c.params.emplace_back("sign_in", methods);
			if (!auth.google_client_id.empty()) {
				c.params.emplace_back("google_client_id", auth.google_client_id);
				if (!auth.google_client_secret.empty()) {
					c.params.emplace_back("google_client_secret", auth.google_client_secret);
				}
			}
		}
		return c;
	};

	host.begin_authenticate = [service](std::string_view token,
									  std::string_view nonce) -> net::AuthTicket {
		auto pending = service->begin(std::string(token), std::string(nonce));
		return [pending]() -> std::optional<net::AuthOutcome> {
			const auto verdict = pending->poll();
			if (!verdict) {
				return std::nullopt;
			}
			if (!verdict->ok) {
				VB_WARN("auth", "sign-in rejected: ", verdict->detail);
				return net::AuthOutcome{ false, verdict->reason, {}, {} };
			}
			auto login = std::make_shared<net::LoginData>();
			login->provider = std::string(provider_name(verdict->login.provider));
			login->issuer = verdict->login.issuer;
			login->subject = verdict->login.subject;
			login->name = verdict->login.name;
			login->claims_json = verdict->login.claims_json;
			login->issued_at = verdict->login.issued_at;
			login->expires_at = verdict->login.expires_at;
			return net::AuthOutcome{ true, {}, verdict->login.name, login };
		};
	};
	return service;
}

} // namespace vb::auth
