#include "vb/auth/secret_store.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first: wincred.h relies on its types.
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace vb::auth {

namespace {

// Every backend files secrets under this service / prefix.
constexpr const char *kService = "voxel_browser";

bool disabled_by_env() {
	const char *v = std::getenv("VB_KEYCHAIN");
	return v != nullptr && (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0 || std::strcmp(v, "file") == 0);
}

#if defined(_WIN32)

std::wstring widen(const std::string &s) {
	if (s.empty()) {
		return {};
	}
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring out(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
	return out;
}

// Windows Credential Manager, generic credentials "voxel_browser/<account>".
class WindowsCredentialBackend final : public SecretBackend {
public:
	std::string_view name() const override { return "Windows Credential Manager"; }

	bool set(const std::string &account, const std::string &secret) override {
		if (secret.size() > static_cast<std::size_t>(CRED_MAX_CREDENTIAL_BLOB_SIZE)) {
			return false; // too big for a generic credential; the caller keeps it in a file
		}
		std::wstring target = target_for(account);
		std::wstring user = widen(kService);
		CREDENTIALW cred{};
		cred.Type = CRED_TYPE_GENERIC;
		cred.TargetName = target.data();
		cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
		cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secret.data()));
		cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
		cred.UserName = user.data();
		return CredWriteW(&cred, 0) != FALSE;
	}

	std::optional<std::string> get(const std::string &account) override {
		PCREDENTIALW cred = nullptr;
		const std::wstring target = target_for(account);
		if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) == FALSE || cred == nullptr) {
			return std::nullopt;
		}
		std::string out(reinterpret_cast<const char *>(cred->CredentialBlob), cred->CredentialBlobSize);
		CredFree(cred);
		return out;
	}

	bool erase(const std::string &account) override {
		const std::wstring target = target_for(account);
		return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
	}

private:
	static std::wstring target_for(const std::string &account) {
		return widen(std::string(kService) + "/" + account);
	}
};

#elif defined(__APPLE__)

// macOS Keychain: generic passwords with service "voxel_browser".
class MacKeychainBackend final : public SecretBackend {
public:
	std::string_view name() const override { return "macOS Keychain"; }

	bool set(const std::string &account, const std::string &secret) override {
		std::lock_guard<std::mutex> lock(mutex_);
		CFMutableDictionaryRef query = base_query(account);
		SecItemDelete(query); // replace, never duplicate
		CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(secret.data()),
				static_cast<CFIndex>(secret.size()));
		CFDictionarySetValue(query, kSecValueData, data);
		CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
		const OSStatus st = SecItemAdd(query, nullptr);
		CFRelease(data);
		CFRelease(query);
		return st == errSecSuccess;
	}

	std::optional<std::string> get(const std::string &account) override {
		std::lock_guard<std::mutex> lock(mutex_);
		CFMutableDictionaryRef query = base_query(account);
		CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
		CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
		CFTypeRef result = nullptr;
		const OSStatus st = SecItemCopyMatching(query, &result);
		CFRelease(query);
		if (st != errSecSuccess || result == nullptr) {
			return std::nullopt;
		}
		std::optional<std::string> out;
		if (CFGetTypeID(result) == CFDataGetTypeID()) {
			const auto data = static_cast<CFDataRef>(result);
			out = std::string(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
					static_cast<std::size_t>(CFDataGetLength(data)));
		}
		CFRelease(result);
		return out;
	}

	bool erase(const std::string &account) override {
		std::lock_guard<std::mutex> lock(mutex_);
		CFMutableDictionaryRef query = base_query(account);
		const OSStatus st = SecItemDelete(query);
		CFRelease(query);
		return st == errSecSuccess;
	}

private:
	static CFStringRef cf_string(const std::string &s) {
		return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(s.data()),
				static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
	}

	// Caller releases.
	static CFMutableDictionaryRef base_query(const std::string &account) {
		CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFStringRef service = cf_string(kService);
		CFStringRef acct = cf_string(account);
		CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
		CFDictionarySetValue(q, kSecAttrService, service);
		CFDictionarySetValue(q, kSecAttrAccount, acct);
		CFRelease(service);
		CFRelease(acct);
		return q;
	}

	std::mutex mutex_;
};

#else

// freedesktop Secret Service (GNOME Keyring, KWallet, KeePassXC, ...) through
// libsecret, loaded with dlopen so the client runs without it. The struct
// layouts below are libsecret's and GLib's public, stable ABI.
using gboolean = int;
using gchar = char;
using gpointer = void *;
struct GError {
	std::uint32_t domain;
	int code;
	gchar *message;
};
struct GHashTable;
using GHashFunc = unsigned (*)(const void *);
using GEqualFunc = gboolean (*)(const void *, const void *);

struct SecretSchemaAttribute {
	const gchar *name;
	int type; // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
};
struct SecretSchema {
	const gchar *name;
	int flags; // SECRET_SCHEMA_NONE = 0
	SecretSchemaAttribute attributes[32];
	int reserved;
	gpointer reserved1, reserved2, reserved3, reserved4, reserved5, reserved6, reserved7;
};

class LibsecretBackend final : public SecretBackend {
public:
	std::string_view name() const override { return "Secret Service"; }

	bool set(const std::string &account, const std::string &secret) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!load()) {
			return false;
		}
		GHashTable *attrs = attributes(account);
		GError *err = nullptr;
		const std::string label = std::string("Voxel Browser sign-in (") + account.substr(0, 12) + ")";
		const gboolean ok = store_(&schema_, attrs, "default", label.c_str(), secret.c_str(), nullptr, &err);
		hash_unref_(attrs);
		return check(err) && ok != 0;
	}

	std::optional<std::string> get(const std::string &account) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!load()) {
			return std::nullopt;
		}
		GHashTable *attrs = attributes(account);
		GError *err = nullptr;
		gchar *pw = lookup_(&schema_, attrs, nullptr, &err);
		hash_unref_(attrs);
		if (!check(err) || pw == nullptr) {
			return std::nullopt;
		}
		std::string out(pw);
		password_free_(pw);
		return out;
	}

	bool erase(const std::string &account) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!load()) {
			return false;
		}
		GHashTable *attrs = attributes(account);
		GError *err = nullptr;
		const gboolean ok = clear_(&schema_, attrs, nullptr, &err);
		hash_unref_(attrs);
		return check(err) && ok != 0;
	}

	// No session bus means no Secret Service, and libsecret would otherwise
	// try (and slowly fail) to autolaunch one.
	static bool session_bus_present() {
		if (std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr) {
			return true;
		}
		const char *runtime = std::getenv("XDG_RUNTIME_DIR");
		if (runtime == nullptr) {
			return false;
		}
		const std::string bus = std::string(runtime) + "/bus";
		return access(bus.c_str(), F_OK) == 0;
	}

private:
	// Lazily dlopen()s libsecret on first use, so a client that never signs
	// in never touches it. Once a call fails, the backend stays off for this
	// process (the store falls back to files) instead of retrying a broken
	// or locked service on every save.
	bool load() {
		if (broken_) {
			return false;
		}
		if (store_ != nullptr) {
			return true;
		}
		void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
		void *secret = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
		if (glib == nullptr || secret == nullptr) {
			broken_ = true;
			return false;
		}
		hash_new_ = reinterpret_cast<GHashTable *(*)(GHashFunc, GEqualFunc)>(dlsym(glib, "g_hash_table_new"));
		hash_insert_ = reinterpret_cast<gboolean (*)(GHashTable *, gpointer, gpointer)>(dlsym(glib, "g_hash_table_insert"));
		hash_unref_ = reinterpret_cast<void (*)(GHashTable *)>(dlsym(glib, "g_hash_table_unref"));
		str_hash_ = reinterpret_cast<GHashFunc>(dlsym(glib, "g_str_hash"));
		str_equal_ = reinterpret_cast<GEqualFunc>(dlsym(glib, "g_str_equal"));
		error_free_ = reinterpret_cast<void (*)(GError *)>(dlsym(glib, "g_error_free"));
		auto *store = reinterpret_cast<gboolean (*)(const SecretSchema *, GHashTable *, const gchar *,
				const gchar *, const gchar *, void *, GError **)>(dlsym(secret, "secret_password_storev_sync"));
		lookup_ = reinterpret_cast<gchar *(*)(const SecretSchema *, GHashTable *, void *, GError **)>(
				dlsym(secret, "secret_password_lookupv_sync"));
		clear_ = reinterpret_cast<gboolean (*)(const SecretSchema *, GHashTable *, void *, GError **)>(
				dlsym(secret, "secret_password_clearv_sync"));
		password_free_ = reinterpret_cast<void (*)(gchar *)>(dlsym(secret, "secret_password_free"));
		if (hash_new_ == nullptr || hash_insert_ == nullptr || hash_unref_ == nullptr ||
				str_hash_ == nullptr || str_equal_ == nullptr || error_free_ == nullptr ||
				store == nullptr || lookup_ == nullptr || clear_ == nullptr || password_free_ == nullptr) {
			broken_ = true;
			return false;
		}
		schema_ = SecretSchema{};
		schema_.name = "io.github.voxelbrowser.SignIn";
		schema_.attributes[0] = { "service", 0 };
		schema_.attributes[1] = { "account", 0 };
		store_ = store;
		return true;
	}

	GHashTable *attributes(const std::string &account) {
		account_ = account;
		GHashTable *t = hash_new_(str_hash_, str_equal_);
		hash_insert_(t, const_cast<char *>("service"), const_cast<char *>(kService));
		hash_insert_(t, const_cast<char *>("account"), account_.data());
		return t;
	}

	bool check(GError *err) {
		if (err == nullptr) {
			return true;
		}
		error_free_(err);
		broken_ = true;
		return false;
	}

	std::mutex mutex_;
	bool broken_ = false;
	std::string account_; // backs the "account" attribute while a call runs
	SecretSchema schema_{};
	GHashTable *(*hash_new_)(GHashFunc, GEqualFunc) = nullptr;
	gboolean (*hash_insert_)(GHashTable *, gpointer, gpointer) = nullptr;
	void (*hash_unref_)(GHashTable *) = nullptr;
	GHashFunc str_hash_ = nullptr;
	GEqualFunc str_equal_ = nullptr;
	void (*error_free_)(GError *) = nullptr;
	gboolean (*store_)(const SecretSchema *, GHashTable *, const gchar *, const gchar *, const gchar *,
			void *, GError **) = nullptr;
	gchar *(*lookup_)(const SecretSchema *, GHashTable *, void *, GError **) = nullptr;
	gboolean (*clear_)(const SecretSchema *, GHashTable *, void *, GError **) = nullptr;
	void (*password_free_)(gchar *) = nullptr;
};

#endif

} // namespace

std::shared_ptr<SecretBackend> os_secret_backend() {
	if (disabled_by_env()) {
		return nullptr;
	}
#if defined(_WIN32)
	return std::make_shared<WindowsCredentialBackend>();
#elif defined(__APPLE__)
	return std::make_shared<MacKeychainBackend>();
#else
	if (!LibsecretBackend::session_bus_present()) {
		return nullptr;
	}
	return std::make_shared<LibsecretBackend>();
#endif
}

} // namespace vb::auth
