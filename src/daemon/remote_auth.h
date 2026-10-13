// Remote access to the web panel (remote_access = true): who may use the
// control API, and keeping remote_password out of what remote clients see.
//
// Requests from this computer (127.0.0.0/8) need no password, so Virgil
// Control and the local panel work as before. Any other source must send
// HTTP Basic credentials (any user name) with remote_password. After
// kMaxFailures wrong passwords from one address it is locked out for
// kLockoutMs.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace virgil {

// What GET /api/config shows remote clients instead of the password. A
// posted configuration with this value keeps the current password.
extern const char* const kPasswordMask;

bool is_loopback(uint32_t addr);  // host byte order
bool constant_time_equal(const std::string& a, const std::string& b);
// The password of an "Authorization: Basic <base64(user:password)>" header
// value. False if it is not Basic auth or not valid base64.
bool basic_auth_password(const std::string& authorization, std::string* password);

class RemoteAuth {
 public:
  enum class Result {
    Allowed,
    NeedLogin,  // 401 with WWW-Authenticate: no or a wrong password
    LockedOut,  // 429: too many wrong passwords from this address lately
    Refused,    // 403: remote access is off
  };
  static constexpr int kMaxFailures = 5;
  static constexpr int64_t kLockoutMs = 10000;

  // `peer` in host byte order; `now_ms` from a monotonic clock.
  Result check(uint32_t peer, bool remote_enabled, const std::string& password, const std::string& authorization,
               int64_t now_ms);
  // Seconds until a locked-out address may try again (at least 1).
  int retry_after_s(uint32_t peer, int64_t now_ms) const;

 private:
  struct Peer {
    int failures = 0;
    int64_t locked_until = 0, last = 0;
  };
  mutable std::mutex m_;
  std::map<uint32_t, Peer> peers_;
};

// `conf` with the value of remote_password (if any) replaced by kPasswordMask.
std::string mask_remote_password(const std::string& conf);

// A configuration text posted by a client, checked against the `current`
// one: a remote_password equal to kPasswordMask becomes the current password,
// and clients that are not on this computer (`local` false) may not change
// remote_access or remote_password. On success `out` is the text to save.
bool resolve_posted_config(const std::string& posted, const std::string& current, bool local, std::string* out,
                           std::string* error);

}  // namespace virgil
