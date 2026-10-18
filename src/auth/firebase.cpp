#include "vb/auth/firebase.hpp"

#include <nlohmann/json.hpp>

#include "vb/auth/oidc_client.hpp"

namespace vb::auth {

namespace {

using nlohmann::json;

SignInResult failure(std::string error) {
	SignInResult r;
	r.done = true;
	r.error = std::move(error);
	return r;
}

std::string error_message(const std::string &body) {
	const json doc = json::parse(body, nullptr, false);
	if (doc.is_object() && doc.contains("error") && doc["error"].is_object() &&
			doc["error"].contains("message") && doc["error"]["message"].is_string()) {
		return doc["error"]["message"].get<std::string>();
	}
	return {};
}

} // namespace

std::string firebase_error_text(const std::string &message) {
	// Identity Toolkit may append detail after " : ".
	const std::string code = message.substr(0, message.find(" :"));
	if (code == "INVALID_PASSWORD" || code == "EMAIL_NOT_FOUND" ||
			code == "INVALID_LOGIN_CREDENTIALS" || code == "INVALID_EMAIL") {
		return "Wrong e-mail or password";
	}
	if (code == "USER_DISABLED") {
		return "This account has been disabled";
	}
	if (code == "TOO_MANY_ATTEMPTS_TRY_LATER") {
		return "Too many attempts, try again later";
	}
	return "Sign-in failed";
}

SignInResult firebase_password_sign_in(HttpFetcher &http, const std::string &api_key,
		const std::string &email, const std::string &password) {
	if (api_key.empty() || email.empty() || password.empty()) {
		return failure("Enter your e-mail and password");
	}
	const json body = { { "email", email }, { "password", password },
		{ "returnSecureToken", true } };
	const HttpResult res = http.post(
			"https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" +
					url_encode(api_key),
			"application/json", body.dump());
	if (res.status == 0) {
		return failure("Could not reach the sign-in service");
	}
	if (res.status != 200) {
		return failure(firebase_error_text(error_message(res.body)));
	}
	const json doc = json::parse(res.body, nullptr, false);
	if (!doc.is_object() || !doc.contains("idToken") || !doc["idToken"].is_string()) {
		return failure("Sign-in failed");
	}
	SignInResult r;
	r.done = true;
	r.ok = true;
	r.id_token = doc["idToken"].get<std::string>();
	if (doc.contains("refreshToken") && doc["refreshToken"].is_string()) {
		r.refresh_token = doc["refreshToken"].get<std::string>();
	}
	return r;
}

SignInResult firebase_idp_sign_in(HttpFetcher &http, const std::string &api_key,
		const std::string &provider_id, const std::string &idp_id_token) {
	if (api_key.empty() || idp_id_token.empty()) {
		return failure("Sign-in failed");
	}
	// requestUri only has to be a valid http(s) URL for an id_token post body.
	const json body = { { "postBody", form_encode({ { "id_token", idp_id_token }, { "providerId", provider_id } }) },
		{ "requestUri", "http://localhost" }, { "returnSecureToken", true },
		{ "returnIdpCredential", true } };
	const HttpResult res = http.post(
			"https://identitytoolkit.googleapis.com/v1/accounts:signInWithIdp?key=" + url_encode(api_key),
			"application/json", body.dump());
	if (res.status == 0) {
		return failure("Could not reach the sign-in service");
	}
	if (res.status != 200) {
		const std::string code = error_message(res.body);
		if (code.rfind("OPERATION_NOT_ALLOWED", 0) == 0) {
			return failure("Google sign-in is not enabled for this game");
		}
		return failure(firebase_error_text(code));
	}
	const json doc = json::parse(res.body, nullptr, false);
	if (!doc.is_object()) {
		return failure("Sign-in failed");
	}
	// Account-linking conflicts come back as 200 with an error and no token.
	if (doc.contains("needConfirmation") && doc["needConfirmation"].is_boolean() &&
			doc["needConfirmation"].get<bool>()) {
		return failure("This e-mail already has an account with another sign-in method");
	}
	if (!doc.contains("idToken") || !doc["idToken"].is_string()) {
		return failure("Sign-in failed");
	}
	SignInResult r;
	r.done = true;
	r.ok = true;
	r.id_token = doc["idToken"].get<std::string>();
	if (doc.contains("refreshToken") && doc["refreshToken"].is_string()) {
		r.refresh_token = doc["refreshToken"].get<std::string>();
	}
	return r;
}

SignInResult firebase_refresh(HttpFetcher &http, const std::string &api_key,
		const std::string &refresh_token) {
	if (api_key.empty() || refresh_token.empty()) {
		return failure("Sign in again");
	}
	const HttpResult res = http.post(
			"https://securetoken.googleapis.com/v1/token?key=" + url_encode(api_key),
			"application/x-www-form-urlencoded",
			form_encode({ { "grant_type", "refresh_token" },
					{ "refresh_token", refresh_token } }));
	if (res.status == 0 || res.status >= 500 || res.status == 429) {
		SignInResult r = failure("Could not reach the sign-in service");
		r.retryable = true; // the refresh token is not at fault
		return r;
	}
	const json doc = json::parse(res.body, nullptr, false);
	if (res.status != 200 || !doc.is_object() || !doc.contains("id_token") ||
			!doc["id_token"].is_string()) {
		return failure("Sign in again");
	}
	SignInResult r;
	r.done = true;
	r.ok = true;
	r.id_token = doc["id_token"].get<std::string>();
	if (doc.contains("refresh_token") && doc["refresh_token"].is_string()) {
		r.refresh_token = doc["refresh_token"].get<std::string>();
	}
	return r;
}

} // namespace vb::auth
