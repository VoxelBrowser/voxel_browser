#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// OS credential storage for sign-in refresh tokens (auth.md §7): the macOS
// Keychain, the Windows Credential Manager, or the freedesktop Secret Service
// through libsecret on Linux (loaded at run time, so the client never needs
// it installed). SessionStore uses it when one is available and falls back to
// owner-only (0600) files when not.

namespace vb::auth {

class SecretBackend {
public:
	virtual ~SecretBackend() = default;

	virtual std::string_view name() const = 0; // "macOS Keychain", for logs and the menu
	// Each call reports failure instead of throwing; a failed set() makes the
	// caller keep the secret in its file instead.
	virtual bool set(const std::string &account, const std::string &secret) = 0;
	virtual std::optional<std::string> get(const std::string &account) = 0;
	virtual bool erase(const std::string &account) = 0;
};

// The platform backend, or nullptr when there is none (no libsecret, no
// Secret Service on the session bus, or VB_KEYCHAIN=off in the environment --
// the e2e harness sets that so test runs never touch a developer's keychain).
std::shared_ptr<SecretBackend> os_secret_backend();

// For tests: secrets in a map, plus a switch that makes every call fail.
class MemorySecretBackend final : public SecretBackend {
public:
	std::string_view name() const override { return "memory"; }
	bool set(const std::string &account, const std::string &secret) override {
		if (fail) {
			return false;
		}
		secrets[account] = secret;
		return true;
	}
	std::optional<std::string> get(const std::string &account) override {
		auto it = secrets.find(account);
		if (fail || it == secrets.end()) {
			return std::nullopt;
		}
		return it->second;
	}
	bool erase(const std::string &account) override {
		return !fail && secrets.erase(account) > 0;
	}

	bool fail = false;
	std::map<std::string, std::string> secrets;
};

} // namespace vb::auth
