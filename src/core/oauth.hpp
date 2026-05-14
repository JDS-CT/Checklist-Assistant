#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace core {

struct OAuthClientConfig {
  std::string client_id;
  std::string secret_hash;
  std::vector<std::string> redirect_uris;
  std::vector<std::string> allowed_scopes;
};

struct OAuthAuthorization {
  std::string client_id;
  std::string user_id;
  std::string redirect_uri;
  std::string scope;
  std::string state;
  std::chrono::system_clock::time_point expires_at;
};

struct OAuthTokenMetadata {
  std::string client_id;
  std::string user_id;
  std::string scope;
  std::chrono::system_clock::time_point access_expires_at;
  std::optional<std::chrono::system_clock::time_point> refresh_expires_at;
  std::string refresh_token_hash;
  bool revoked = false;
};

struct IssuedTokenPair {
  std::string access_token;
  std::optional<std::string> refresh_token;
  std::chrono::seconds access_expires_in;
  std::optional<std::chrono::seconds> refresh_expires_in;
  std::string scope;
};

class OAuthStore {
 public:
  explicit OAuthStore(std::string db_path);
  ~OAuthStore();

  OAuthStore(const OAuthStore&) = delete;
  OAuthStore& operator=(const OAuthStore&) = delete;

  void Initialize();
  void UpsertClient(const OAuthClientConfig& client);
  std::optional<OAuthClientConfig> GetClient(const std::string& client_id) const;

  std::string IssueAuthorizationCode(const std::string& client_id, const std::string& user_id,
                                     const std::string& redirect_uri, const std::string& scope,
                                     const std::string& state, std::chrono::seconds ttl);

  std::optional<OAuthAuthorization> ConsumeAuthorizationCode(const std::string& code,
                                                             const std::string& client_id,
                                                             const std::string& redirect_uri);

  IssuedTokenPair IssueTokens(const std::string& client_id, const std::string& user_id,
                              const std::string& scope, std::chrono::seconds access_ttl,
                              std::optional<std::chrono::seconds> refresh_ttl);

  std::optional<OAuthTokenMetadata> FindAccessToken(const std::string& raw_token) const;
  std::optional<OAuthTokenMetadata> FindRefreshToken(const std::string& raw_token) const;
 std::optional<IssuedTokenPair> RotateRefreshToken(
      const std::string& raw_refresh_token, std::chrono::seconds access_ttl,
      std::optional<std::chrono::seconds> refresh_ttl);

 private:
  void EnsureSchema() const;
  void Reopen() const;
  std::optional<OAuthAuthorization> LoadAuthorization(const std::string& code_hash) const;
  std::optional<OAuthTokenMetadata> LoadTokenByAccessHash(const std::string& hash) const;
  std::optional<OAuthTokenMetadata> LoadTokenByRefreshHash(const std::string& hash) const;
  std::unique_lock<std::mutex> AcquireLock() const;

  std::string db_path_;
  mutable std::mutex mutex_;
  mutable sqlite3* db_ = nullptr;
};

std::string HashSecret(const std::string& value);
bool ConstantTimeEquals(const std::string& a, const std::string& b);
std::string GenerateTokenString(std::size_t length);

}  // namespace core
