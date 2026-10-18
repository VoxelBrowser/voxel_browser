#include "vb/auth/coordinator.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "vb/auth/firebase.hpp"
#include "vb/auth/jwt.hpp"
#include "vb/auth/oidc_client.hpp"

namespace vb::auth {

std::optional<std::string> read_token_file(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return std::nullopt;
	}
	std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (data.size() > kMaxJwtBytes + 64) {
		return std::nullopt;
	}
	auto is_space = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
	while (!data.empty() && is_space(data.back())) {
		data.pop_back();
	}
	std::size_t b = 0;
	while (b < data.size() && is_space(data[b])) {
		++b;
	}
	data.erase(0, b);
	if (data.empty() || data.size() > kMaxJwtBytes) {
		return std::nullopt;
	}
	return data;
}

namespace {

constexpr int kPromptAfterTransientFailures = 3;

std::string param(const protocol::S2CAuthChallenge &c, const char *key) {
	for (const auto &[k, v] : c.params) {
		if (k == key) {
			return v;
		}
	}
	return {};
}

bool is_oidc(const protocol::S2CAuthChallenge &c) {
	return c.provider == "oidc" || c.provider == "keycloak";
}

} // namespace

SignInResult silent_sign_in(HttpFetcher &http, const protocol::S2CAuthChallenge &challenge,
		const std::string &refresh_token) {
	SignInResult r;
	r.done = true;
	if (refresh_token.empty()) {
		r.error = "Sign in again";
		return r;
	}
	if (challenge.provider == "firebase") {
		r = firebase_refresh(http, param(challenge, "api_key"), refresh_token);
	} else if (is_oidc(challenge)) {
		std::string err;
		const auto ep = oidc_discover(http, challenge.issuer, &err);
		if (!ep) {
			// Discovery failing says nothing about the refresh token: keep it.
			r.error = "Could not reach the identity provider";
			r.retryable = true;
			return r;
		}
		const TokenResponse t = refresh_id_token(http, *ep, challenge.client_id, refresh_token);
		r.ok = t.ok;
		r.id_token = t.id_token;
		r.refresh_token = t.refresh_token;
		r.retryable = !t.ok && t.retryable;
		r.error = t.ok ? std::string()
					   : (t.retryable ? "Could not reach the identity provider" : "Sign in again");
	} else {
		r.error = "Sign in again";
	}
	if (r.ok && r.refresh_token.empty()) {
		r.refresh_token = refresh_token; // the IdP did not rotate it
	}
	return r;
}

SignInCoordinator::SignInCoordinator(Options options) :
		options_(std::move(options)) {}
SignInCoordinator::~SignInCoordinator() = default;

std::function<net::TokenTicket(const protocol::S2CAuthChallenge &)>
SignInCoordinator::provider() {
	return [this](const protocol::S2CAuthChallenge &ch) -> net::TokenTicket {
		{
			std::lock_guard lock(mu_);
			challenge_ = ch;
			active_nonce_ = ch.nonce;
			phase_ = Phase::kChoosing;
			error_.clear();
			token_.clear();
			token_ready_ = false;
			cancelled_ = false;
			reauth_ = false;
			needs_trust_ = options_.store && !options_.token_file &&
					!options_.store->is_trusted(options_.server_id, ch.issuer);
			if (!needs_trust_ && !options_.token_file) {
				begin_silent_locked();
			}
		}
		if (options_.token_file) {
			const auto path = *options_.token_file;
			return [this, path]() -> net::TokenPoll {
				net::TokenPoll p;
				p.done = true;
				if (auto t = read_token_file(path)) {
					p.token = std::move(*t);
					std::lock_guard lock(mu_);
					subject_ = subject_from_id_token(p.token);
				} else {
					p.error = "could not read the sign-in token file";
				}
				return p;
			};
		}
		return [this] { return poll_ticket(); };
	};
}

std::function<net::TokenTicket(const protocol::S2CReauthRequest &)>
SignInCoordinator::reauth_provider() {
	return [this](const protocol::S2CReauthRequest &req) -> net::TokenTicket {
		if (options_.token_file) {
			// Re-read for every re-auth so a test can rotate or withhold it.
			const auto path = *options_.token_file;
			return [this, path]() -> net::TokenPoll {
				net::TokenPoll p;
				p.done = true;
				if (auto t = read_token_file(path)) {
					p.token = std::move(*t);
					std::lock_guard lock(mu_);
					subject_ = subject_from_id_token(p.token);
				} else {
					p.error = "could not read the sign-in token file";
				}
				return p;
			};
		}
		{
			std::lock_guard lock(mu_);
			active_nonce_ = req.nonce;
			reauth_ = true;
			retry_pending_ = false;
			transient_failures_ = 0;
			phase_ = Phase::kChoosing;
			error_.clear();
			token_.clear();
			token_ready_ = false;
			cancelled_ = false;
			begin_silent_locked(); // no UI if the cached refresh token still works
		}
		return [this] { return poll_ticket(); };
	};
}

void SignInCoordinator::begin_silent_locked() {
	if (!options_.store || !options_.http || !challenge_ || task_) {
		return;
	}
	const auto stored = options_.store->load(options_.server_id, challenge_->issuer, challenge_->client_id);
	if (!stored) {
		return;
	}
	const protocol::S2CAuthChallenge ch = *challenge_;
	const std::string refresh = stored->refresh_token;
	auto http = options_.http;
	silent_ = true;
	phase_ = Phase::kWorking;
	task_ = std::make_unique<SignInTask>([http, ch, refresh](const std::atomic<bool> &) {
		return silent_sign_in(*http, ch, refresh);
	});
}

void SignInCoordinator::save_session_locked(const SignInResult &r) {
	if (!options_.store || !challenge_ || r.refresh_token.empty()) {
		return;
	}
	StoredSession s;
	s.server = options_.server_id;
	s.issuer = challenge_->issuer;
	s.client_id = challenge_->client_id;
	s.provider = challenge_->provider;
	s.api_key = param(*challenge_, "api_key");
	s.refresh_token = r.refresh_token;
	s.label = label_from_id_token(r.id_token);
	options_.store->save(s);
}

void SignInCoordinator::update_locked() {
	if (!task_ && retry_pending_ && reauth_ && !cancelled_ &&
			std::chrono::steady_clock::now() >= retry_at_) {
		retry_pending_ = false;
		begin_silent_locked();
	}
	if (!task_) {
		return;
	}
	const SignInResult r = task_->poll();
	if (!r.done) {
		return;
	}
	task_.reset();
	const bool was_silent = silent_;
	silent_ = false;
	if (r.ok) {
		save_session_locked(r);
		transient_failures_ = 0;
		retry_pending_ = false;
		token_ = r.id_token;
		token_ready_ = true;
		phase_ = Phase::kFinished;
		reauth_ = false;
	} else if (was_silent && r.retryable) {
		// The IdP could not be reached (or answered 5xx): the cached refresh token may
		// well be fine, so keep it and, for a re-auth, quietly try again until the
		// server's grace runs out.
		error_ = r.error;
		phase_ = Phase::kChoosing;
		if (reauth_) {
			++transient_failures_;
			retry_pending_ = true;
			retry_at_ = std::chrono::steady_clock::now() + options_.reauth_retry_interval;
		}
	} else {
		retry_pending_ = false;
		transient_failures_ = 0;
		if (was_silent && options_.store && challenge_) {
			// The cached refresh token is no good (expired, revoked): forget it
			// and fall back to an interactive sign-in.
			options_.store->erase(options_.server_id, challenge_->issuer, challenge_->client_id);
			error_ = "Your saved sign-in expired";
		} else {
			error_ = r.error;
		}
		phase_ = Phase::kChoosing; // retryable
	}
}

net::TokenPoll SignInCoordinator::poll_ticket() {
	std::lock_guard lock(mu_);
	update_locked();
	net::TokenPoll p;
	if (cancelled_) {
		p.done = true;
		p.error = "sign-in cancelled";
	} else if (token_ready_) {
		p.done = true;
		subject_ = subject_from_id_token(token_);
		p.token = std::move(token_);
		token_.clear();
		token_ready_ = false;
	}
	return p;
}

SignInCoordinator::Phase SignInCoordinator::phase() const {
	std::lock_guard lock(mu_);
	const_cast<SignInCoordinator *>(this)->update_locked();
	return phase_;
}

std::optional<protocol::S2CAuthChallenge> SignInCoordinator::challenge() const {
	std::lock_guard lock(mu_);
	return challenge_;
}

std::string SignInCoordinator::last_error() const {
	std::lock_guard lock(mu_);
	return error_;
}

std::string SignInCoordinator::signed_in_subject() const {
	std::lock_guard lock(mu_);
	return subject_;
}

bool SignInCoordinator::needs_trust() const {
	std::lock_guard lock(mu_);
	return needs_trust_;
}

void SignInCoordinator::trust() {
	std::lock_guard lock(mu_);
	if (!needs_trust_ || !challenge_ || !options_.store) {
		return;
	}
	options_.store->trust(options_.server_id, challenge_->issuer);
	needs_trust_ = false;
	begin_silent_locked();
}

bool SignInCoordinator::reauth_prompt_active() const {
	std::lock_guard lock(mu_);
	const_cast<SignInCoordinator *>(this)->update_locked();
	// Quietly retrying a transient failure is not "your sign-in expired": the prompt
	// waits for a few misses in a row.
	const bool quietly_retrying = retry_pending_ && transient_failures_ < kPromptAfterTransientFailures;
	return reauth_ && !cancelled_ && phase_ != Phase::kFinished && !(silent_ && task_) &&
			!quietly_retrying;
}

bool SignInCoordinator::supports_browser() const {
	std::lock_guard lock(mu_);
	return challenge_ && is_oidc(*challenge_);
}

bool SignInCoordinator::supports_password() const {
	std::lock_guard lock(mu_);
	if (!challenge_ || challenge_->provider != "firebase") {
		return false;
	}
	return ("," + param(*challenge_, "sign_in") + ",").find(",password,") != std::string::npos;
}

bool SignInCoordinator::supports_google() const {
	std::lock_guard lock(mu_);
	if (!challenge_ || challenge_->provider != "firebase" || param(*challenge_, "google_client_id").empty()) {
		return false;
	}
	return ("," + param(*challenge_, "sign_in") + ",").find(",google,") != std::string::npos;
}

void SignInCoordinator::start_google() {
	std::lock_guard lock(mu_);
	if (phase_ != Phase::kChoosing || !challenge_ || task_ || needs_trust_ || !options_.http ||
			challenge_->provider != "firebase") {
		return;
	}
	OidcSignInParams p;
	p.http = options_.http;
	p.issuer = options_.google_issuer;
	p.client_id = param(*challenge_, "google_client_id");
	p.client_secret = param(*challenge_, "google_client_secret");
	p.scopes = { "openid", "email", "profile" };
	p.nonce = active_nonce_; // ends up in the Google token only; Firebase tokens carry none
	p.open_browser = options_.open_browser;
	if (p.client_id.empty()) {
		return;
	}
	const std::string api_key = param(*challenge_, "api_key");
	error_.clear();
	phase_ = Phase::kWorking;
	task_ = std::make_unique<SignInTask>([p, api_key](const std::atomic<bool> &c) {
		SignInResult google = oidc_browser_sign_in(p, c);
		if (!google.ok) {
			return google;
		}
		return firebase_idp_sign_in(*p.http, api_key, "google.com", google.id_token);
	});
}

void SignInCoordinator::start_browser() {
	std::lock_guard lock(mu_);
	if (phase_ != Phase::kChoosing || !challenge_ || task_ || needs_trust_ || !is_oidc(*challenge_)) {
		return;
	}
	OidcSignInParams p;
	p.http = options_.http;
	p.issuer = challenge_->issuer;
	p.client_id = challenge_->client_id;
	p.scopes = challenge_->scopes;
	p.nonce = active_nonce_; // the join challenge's, or the open re-auth request's
	p.open_browser = options_.open_browser;
	error_.clear();
	phase_ = Phase::kWorking;
	task_ = std::make_unique<SignInTask>(
			[p](const std::atomic<bool> &c) { return oidc_browser_sign_in(p, c); });
}

void SignInCoordinator::start_password(const std::string &email, const std::string &password) {
	std::lock_guard lock(mu_);
	if (phase_ != Phase::kChoosing || !challenge_ || task_ || needs_trust_ || !options_.http) {
		return;
	}
	const std::string api_key = param(*challenge_, "api_key");
	error_.clear();
	phase_ = Phase::kWorking;
	auto http = options_.http;
	task_ = std::make_unique<SignInTask>(
			[http, api_key, email, password](const std::atomic<bool> &) {
				return firebase_password_sign_in(*http, api_key, email, password);
			});
}

void SignInCoordinator::cancel() {
	std::lock_guard lock(mu_);
	cancelled_ = true;
	if (task_) {
		task_->cancel();
		parked_.push_back(std::move(task_));
	}
	phase_ = Phase::kFinished;
}

} // namespace vb::auth
