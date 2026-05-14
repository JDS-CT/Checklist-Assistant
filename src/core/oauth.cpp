#include "core/oauth.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/logging.hpp"
#include "sqlite3.h"
#include "xxhash.h"

namespace {

using core::OAuthAuthorization;
using core::OAuthTokenMetadata;
using core::OAuthClientConfig;
using core::logging::LogWarn;
using core::logging::IsDebugEnabled;
using core::logging::LogDebug;
using core::logging::LogError;

int Prepare(sqlite3* db, const std::string& sql, sqlite3_stmt** stmt) {
  return sqlite3_prepare_v2(db, sql.c_str(), -1, stmt, nullptr);
}

void Finalize(sqlite3_stmt* stmt) {
  if (stmt) {
    sqlite3_finalize(stmt);
  }
}

void StepOrThrow(sqlite3_stmt* stmt, const std::string& context) {
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    throw std::runtime_error("SQLite failure during " + context + ": " +
                             std::string{sqlite3_errstr(sqlite3_errcode(sqlite3_db_handle(stmt)))});
  }
}

std::int64_t NowSeconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

std::string Join(const std::vector<std::string>& values, char delimiter) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      oss << delimiter;
    }
    oss << values[i];
  }
  return oss.str();
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (char ch : value) {
    if (ch == delimiter) {
      if (!current.empty()) {
        parts.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

bool IsExpired(std::int64_t epoch_seconds) {
  if (epoch_seconds <= 0) return false;
  return epoch_seconds <= NowSeconds();
}

std::string NormalizeScope(const std::string& scope) {
  std::vector<std::string> tokens = Split(scope, ' ');
  std::sort(tokens.begin(), tokens.end());
  tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
  return Join(tokens, ' ');
}

}  // namespace

namespace core {

std::string HashSecret(const std::string& value) {
  const XXH128_hash_t hash = XXH3_128bits(value.data(), value.size());
  XXH128_canonical_t canonical;
  XXH128_canonicalFromHash(&canonical, hash);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned char byte : canonical.digest) {
    oss << std::setw(2) << static_cast<int>(byte);
  }
  return oss.str();
}

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::string GenerateTokenString(std::size_t length) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(kAlphabet) - 2);
  std::string output;
  output.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    output.push_back(kAlphabet[dist(rng)]);
  }
  return output;
}

std::unique_lock<std::mutex> OAuthStore::AcquireLock() const {
  return std::unique_lock<std::mutex>(mutex_);
}

OAuthStore::OAuthStore(std::string db_path) : db_path_(std::move(db_path)) {}

OAuthStore::~OAuthStore() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

void OAuthStore::Initialize() {
  namespace fs = std::filesystem;
  const fs::path db_location(db_path_);
  if (db_location.has_parent_path()) {
    fs::create_directories(db_location.parent_path());
  }

  Reopen();
  EnsureSchema();
}

void OAuthStore::Reopen() const {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }

  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  const int rc = sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    throw std::runtime_error("Failed to open OAuth SQLite database at " + db_path_ + ": " +
                             sqlite3_errstr(rc));
  }

  char* errmsg = nullptr;
  sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to enable foreign_keys for OAuth store: " + message);
  }

  errmsg = nullptr;
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    LogError(std::string{"Could not set WAL journal mode for OAuth store: "} + errmsg);
    sqlite3_free(errmsg);
  }
}

void OAuthStore::EnsureSchema() const {
  const char* kSchema = R"sql(
    CREATE TABLE IF NOT EXISTS oauth_clients (
        client_id        TEXT PRIMARY KEY,
        secret_hash      TEXT NOT NULL,
        redirect_uris    TEXT NOT NULL,
        allowed_scopes   TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS oauth_auth_codes (
        code_hash     TEXT PRIMARY KEY,
        client_id     TEXT NOT NULL,
        user_id       TEXT NOT NULL,
        redirect_uri  TEXT NOT NULL,
        scope         TEXT NOT NULL,
        state         TEXT NOT NULL,
        created_at    INTEGER NOT NULL,
        expires_at    INTEGER NOT NULL,
        consumed_at   INTEGER,
        FOREIGN KEY(client_id) REFERENCES oauth_clients(client_id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS oauth_tokens (
        access_token_hash    TEXT PRIMARY KEY,
        refresh_token_hash   TEXT,
        client_id            TEXT NOT NULL,
        user_id              TEXT NOT NULL,
        scope                TEXT NOT NULL,
        created_at           INTEGER NOT NULL,
        access_expires_at    INTEGER NOT NULL,
        refresh_expires_at   INTEGER,
        revoked_at           INTEGER,
        FOREIGN KEY(client_id) REFERENCES oauth_clients(client_id) ON DELETE CASCADE
    );

    CREATE UNIQUE INDEX IF NOT EXISTS idx_oauth_refresh_hash
      ON oauth_tokens(refresh_token_hash)
      WHERE refresh_token_hash IS NOT NULL;
  )sql";

  char* errmsg = nullptr;
  const int rc = sqlite3_exec(db_, kSchema, nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string message = errmsg ? errmsg : "";
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to initialize OAuth schema: " + message);
  }
}

void OAuthStore::UpsertClient(const OAuthClientConfig& client) {
  auto lock = AcquireLock();
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO oauth_clients (client_id, secret_hash, redirect_uris, allowed_scopes) "
      "VALUES (?,?,?,?) "
      "ON CONFLICT(client_id) DO UPDATE SET secret_hash=excluded.secret_hash, "
      "redirect_uris=excluded.redirect_uris, allowed_scopes=excluded.allowed_scopes;";

  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare OAuth client upsert");
  }

  const std::string redirect_blob = Join(client.redirect_uris, '\n');
  const std::string scopes_blob = NormalizeScope(Join(client.allowed_scopes, ' '));

  sqlite3_bind_text(stmt, 1, client.client_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, client.secret_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, redirect_blob.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, scopes_blob.c_str(), -1, SQLITE_TRANSIENT);

  StepOrThrow(stmt, "oauth client upsert");
  Finalize(stmt);
}

std::optional<OAuthClientConfig> OAuthStore::GetClient(const std::string& client_id) const {
  auto lock = AcquireLock();
  if (IsDebugEnabled()) {
    LogDebug("OAuthStore::GetClient acquired lock for client_id=" + client_id);
  }

  sqlite3* local_db = nullptr;
  const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
  const int open_rc = sqlite3_open_v2(db_path_.c_str(), &local_db, flags, nullptr);
  if (open_rc != SQLITE_OK) {
    throw std::runtime_error("Failed to open SQLite database for GetClient at " + db_path_ + ": " +
                             sqlite3_errstr(open_rc));
  }
  auto db_guard = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(local_db, sqlite3_close);

  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT client_id, secret_hash, redirect_uris, allowed_scopes FROM oauth_clients "
      "WHERE client_id=?;";
  if (IsDebugEnabled()) {
    LogDebug("OAuthStore::GetClient preparing statement");
  }
  if (Prepare(local_db, sql, &stmt) != SQLITE_OK) {
    std::string message = sqlite3_errmsg(local_db);
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare OAuth client lookup: " + message);
  }
  if (IsDebugEnabled()) {
    LogDebug("OAuthStore::GetClient binding client_id=" + client_id);
  }
  sqlite3_bind_text(stmt, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);

  OAuthClientConfig client;
  if (IsDebugEnabled()) {
    LogDebug("OAuthStore::GetClient stepping for client_id=" + client_id);
  }
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    client.client_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    client.secret_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const auto redirect_blob = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const auto scopes_blob = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    client.redirect_uris = Split(redirect_blob ? redirect_blob : "", '\n');
    client.allowed_scopes = Split(scopes_blob ? scopes_blob : "", ' ');
  } else {
    Finalize(stmt);
    return std::nullopt;
  }

  Finalize(stmt);
  return client;
}

std::string OAuthStore::IssueAuthorizationCode(const std::string& client_id,
                                               const std::string& user_id,
                                               const std::string& redirect_uri,
                                               const std::string& scope,
                                               const std::string& state,
                                               std::chrono::seconds ttl) {
  const std::string code = GenerateTokenString(40);
  const std::string code_hash = HashSecret(code);
  const std::int64_t now = NowSeconds();
  const std::int64_t expires = now + ttl.count();

  auto lock = AcquireLock();
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO oauth_auth_codes (code_hash, client_id, user_id, redirect_uri, scope, state, "
      "created_at, expires_at) "
      "VALUES (?,?,?,?,?,?,?,?);";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare auth code insert");
  }

  sqlite3_bind_text(stmt, 1, code_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, client_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, user_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, redirect_uri.c_str(), -1, SQLITE_TRANSIENT);
  const std::string normalized_scope = NormalizeScope(scope);
  sqlite3_bind_text(stmt, 5, normalized_scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, now);
  sqlite3_bind_int64(stmt, 8, expires);

  StepOrThrow(stmt, "auth code insert");
  Finalize(stmt);
  return code;
}

std::optional<OAuthAuthorization> OAuthStore::ConsumeAuthorizationCode(
    const std::string& code, const std::string& client_id, const std::string& redirect_uri) {
  const std::string code_hash = HashSecret(code);
  const std::int64_t now = NowSeconds();

  auto lock = AcquireLock();
  sqlite3_stmt* select_stmt = nullptr;
  sqlite3_stmt* update_stmt = nullptr;

  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for code consume: " + message);
  }

  try {
    const std::string select_sql =
        "SELECT client_id, user_id, redirect_uri, scope, state, expires_at, consumed_at "
        "FROM oauth_auth_codes WHERE code_hash=?;";
    if (Prepare(db_, select_sql, &select_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare auth code lookup");
    }
    sqlite3_bind_text(select_stmt, 1, code_hash.c_str(), -1, SQLITE_TRANSIENT);

    OAuthAuthorization auth;
    bool found = false;
    if (sqlite3_step(select_stmt) == SQLITE_ROW) {
      auth.client_id = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 0));
      auth.user_id = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 1));
      auth.redirect_uri = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 2));
      auth.scope = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 3));
      auth.state = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 4));
      const std::int64_t expires_at = sqlite3_column_int64(select_stmt, 5);
      const bool consumed = sqlite3_column_type(select_stmt, 6) != SQLITE_NULL;
      if (auth.client_id != client_id || auth.redirect_uri != redirect_uri ||
          IsExpired(expires_at) || consumed) {
        found = false;
      } else {
        found = true;
        auth.expires_at = std::chrono::system_clock::time_point{std::chrono::seconds{
            expires_at}};
      }
    }
    Finalize(select_stmt);
    select_stmt = nullptr;

    if (!found) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return std::nullopt;
    }

    const std::string update_sql =
        "UPDATE oauth_auth_codes SET consumed_at=? WHERE code_hash=?;";
    if (Prepare(db_, update_sql, &update_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare auth code consume update");
    }
    sqlite3_bind_int64(update_stmt, 1, now);
    sqlite3_bind_text(update_stmt, 2, code_hash.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(update_stmt, "auth code consume");

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    Finalize(update_stmt);
    update_stmt = nullptr;
    return auth;
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    Finalize(select_stmt);
    Finalize(update_stmt);
    throw;
  }
}

IssuedTokenPair OAuthStore::IssueTokens(const std::string& client_id, const std::string& user_id,
                                        const std::string& scope,
                                        std::chrono::seconds access_ttl,
                                        std::optional<std::chrono::seconds> refresh_ttl) {
  const std::string access_token = GenerateTokenString(48);
  const std::string access_hash = HashSecret(access_token);
  const auto normalized_scope = NormalizeScope(scope);
  const std::int64_t now = NowSeconds();
  const std::int64_t access_expires = now + access_ttl.count();

  std::optional<std::string> refresh_token;
  std::string refresh_hash;
  std::optional<std::int64_t> refresh_expires;
  if (refresh_ttl && refresh_ttl->count() > 0) {
    refresh_token = GenerateTokenString(64);
    refresh_hash = HashSecret(*refresh_token);
    refresh_expires = now + refresh_ttl->count();
  }

  auto lock = AcquireLock();
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO oauth_tokens (access_token_hash, refresh_token_hash, client_id, user_id, scope, "
      "created_at, access_expires_at, refresh_expires_at) "
      "VALUES (?,?,?,?,?,?,?,?);";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare token insert");
  }

  sqlite3_bind_text(stmt, 1, access_hash.c_str(), -1, SQLITE_TRANSIENT);
  if (refresh_token) {
    sqlite3_bind_text(stmt, 2, refresh_hash.c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 2);
  }
  sqlite3_bind_text(stmt, 3, client_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, user_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, normalized_scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, now);
  sqlite3_bind_int64(stmt, 7, access_expires);
  if (refresh_expires) {
    sqlite3_bind_int64(stmt, 8, *refresh_expires);
  } else {
    sqlite3_bind_null(stmt, 8);
  }

  StepOrThrow(stmt, "token insert");
  Finalize(stmt);

  IssuedTokenPair pair;
  pair.access_token = access_token;
  pair.refresh_token = refresh_token;
  pair.access_expires_in = access_ttl;
  pair.refresh_expires_in = refresh_ttl;
  pair.scope = normalized_scope;
  return pair;
}

std::optional<OAuthAuthorization> OAuthStore::LoadAuthorization(
    const std::string& code_hash) const {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT client_id, user_id, redirect_uri, scope, state, expires_at, consumed_at "
      "FROM oauth_auth_codes WHERE code_hash=?;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare auth code lookup");
  }
  sqlite3_bind_text(stmt, 1, code_hash.c_str(), -1, SQLITE_TRANSIENT);

  OAuthAuthorization auth;
  bool found = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    auth.client_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    auth.user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    auth.redirect_uri = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    auth.scope = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    auth.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const std::int64_t expires_at = sqlite3_column_int64(stmt, 5);
    const bool consumed = sqlite3_column_type(stmt, 6) != SQLITE_NULL;
    if (!IsExpired(expires_at) && !consumed) {
      found = true;
      auth.expires_at =
          std::chrono::system_clock::time_point{std::chrono::seconds{expires_at}};
    }
  }
  Finalize(stmt);
  if (!found) {
    return std::nullopt;
  }
  return auth;
}

std::optional<OAuthTokenMetadata> OAuthStore::LoadTokenByAccessHash(
    const std::string& hash) const {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT client_id, user_id, scope, access_expires_at, refresh_expires_at, revoked_at, "
      "refresh_token_hash "
      "FROM oauth_tokens WHERE access_token_hash=?;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare access token lookup");
  }
  sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);

  OAuthTokenMetadata meta;
  bool found = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    meta.client_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    meta.user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    meta.scope = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const std::int64_t access_expires_at = sqlite3_column_int64(stmt, 3);
    const auto refresh_expires_raw = sqlite3_column_type(stmt, 4) == SQLITE_NULL
                                         ? std::optional<std::int64_t>{}
                                         : std::optional<std::int64_t>{
                                               sqlite3_column_int64(stmt, 4)};
    const bool revoked = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
    const auto refresh_hash_text = sqlite3_column_text(stmt, 6);
    meta.access_expires_at =
        std::chrono::system_clock::time_point{std::chrono::seconds{access_expires_at}};
    if (refresh_expires_raw) {
      meta.refresh_expires_at = std::chrono::system_clock::time_point{
          std::chrono::seconds{*refresh_expires_raw}};
    }
    meta.revoked = revoked;
    meta.refresh_token_hash = refresh_hash_text ? reinterpret_cast<const char*>(refresh_hash_text)
                                                : "";
    found = true;
  }
  Finalize(stmt);
  if (!found) {
    return std::nullopt;
  }
  return meta;
}

std::optional<OAuthTokenMetadata> OAuthStore::LoadTokenByRefreshHash(
    const std::string& hash) const {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT client_id, user_id, scope, access_expires_at, refresh_expires_at, revoked_at, "
      "refresh_token_hash "
      "FROM oauth_tokens WHERE refresh_token_hash=?;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare refresh token lookup");
  }
  sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);

  OAuthTokenMetadata meta;
  bool found = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    meta.client_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    meta.user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    meta.scope = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const std::int64_t access_expires_at = sqlite3_column_int64(stmt, 3);
    const auto refresh_expires_raw = sqlite3_column_type(stmt, 4) == SQLITE_NULL
                                         ? std::optional<std::int64_t>{}
                                         : std::optional<std::int64_t>{
                                               sqlite3_column_int64(stmt, 4)};
    const bool revoked = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
    meta.refresh_token_hash = hash;
    meta.access_expires_at =
        std::chrono::system_clock::time_point{std::chrono::seconds{access_expires_at}};
    if (refresh_expires_raw) {
      meta.refresh_expires_at = std::chrono::system_clock::time_point{
          std::chrono::seconds{*refresh_expires_raw}};
    }
    meta.revoked = revoked;
    found = true;
  }
  Finalize(stmt);
  if (!found) {
    return std::nullopt;
  }
  return meta;
}

std::optional<OAuthTokenMetadata> OAuthStore::FindAccessToken(
    const std::string& raw_token) const {
  const std::string hash = HashSecret(raw_token);
  auto lock = AcquireLock();
  const auto meta = LoadTokenByAccessHash(hash);
  if (!meta) return std::nullopt;
  const auto now = std::chrono::system_clock::now();
  if (meta->revoked || meta->access_expires_at <= now) {
    return std::nullopt;
  }
  return meta;
}

std::optional<OAuthTokenMetadata> OAuthStore::FindRefreshToken(
    const std::string& raw_token) const {
  const std::string hash = HashSecret(raw_token);
  auto lock = AcquireLock();
  const auto meta = LoadTokenByRefreshHash(hash);
  if (!meta) return std::nullopt;
  const auto now = std::chrono::system_clock::now();
  if (meta->revoked) return std::nullopt;
  if (meta->refresh_expires_at && *meta->refresh_expires_at <= now) {
    return std::nullopt;
  }
  return meta;
}

std::optional<IssuedTokenPair> OAuthStore::RotateRefreshToken(
    const std::string& raw_refresh_token, std::chrono::seconds access_ttl,
    std::optional<std::chrono::seconds> refresh_ttl) {
  const std::string refresh_hash = HashSecret(raw_refresh_token);
  const std::int64_t now = NowSeconds();

  auto lock = AcquireLock();
  sqlite3_stmt* select_stmt = nullptr;
  sqlite3_stmt* revoke_stmt = nullptr;
  sqlite3_stmt* insert_stmt = nullptr;

  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for refresh rotate: " + message);
  }

  try {
    const std::string select_sql =
        "SELECT client_id, user_id, scope, access_expires_at, refresh_expires_at, revoked_at "
        "FROM oauth_tokens WHERE refresh_token_hash=?;";
    if (Prepare(db_, select_sql, &select_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare refresh token lookup");
    }
    sqlite3_bind_text(select_stmt, 1, refresh_hash.c_str(), -1, SQLITE_TRANSIENT);

    std::string client_id;
    std::string user_id;
    std::string scope;
    bool found = false;
    bool revoked = false;
    std::optional<std::int64_t> refresh_expires;
    if (sqlite3_step(select_stmt) == SQLITE_ROW) {
      client_id = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 0));
      user_id = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 1));
      scope = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 2));
      const auto refresh_expires_raw = sqlite3_column_type(select_stmt, 4) == SQLITE_NULL
                                           ? std::optional<std::int64_t>{}
                                           : std::optional<std::int64_t>{
                                                 sqlite3_column_int64(select_stmt, 4)};
      revoked = sqlite3_column_type(select_stmt, 5) != SQLITE_NULL;
      refresh_expires = refresh_expires_raw;
      found = true;
    }
    Finalize(select_stmt);
    select_stmt = nullptr;

    if (!found || revoked || (refresh_expires && *refresh_expires <= now) || !refresh_ttl ||
        refresh_ttl->count() <= 0) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return std::nullopt;
    }

    const std::string revoke_sql =
        "UPDATE oauth_tokens SET revoked_at=? WHERE refresh_token_hash=?;";
    if (Prepare(db_, revoke_sql, &revoke_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare refresh revoke");
    }
    sqlite3_bind_int64(revoke_stmt, 1, now);
    sqlite3_bind_text(revoke_stmt, 2, refresh_hash.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(revoke_stmt, "refresh revoke");
    Finalize(revoke_stmt);
    revoke_stmt = nullptr;

    const std::string access_token = GenerateTokenString(48);
    const std::string access_hash = HashSecret(access_token);
    const std::string normalized_scope = NormalizeScope(scope);
    const std::int64_t access_expires = now + access_ttl.count();

    const std::string new_refresh_token = GenerateTokenString(64);
    const std::string new_refresh_hash = HashSecret(new_refresh_token);
    const std::int64_t new_refresh_expires = now + refresh_ttl->count();

    const std::string insert_sql =
        "INSERT INTO oauth_tokens (access_token_hash, refresh_token_hash, client_id, user_id, scope, "
        "created_at, access_expires_at, refresh_expires_at) VALUES (?,?,?,?,?,?,?,?);";
    if (Prepare(db_, insert_sql, &insert_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare rotated token insert");
    }
    sqlite3_bind_text(insert_stmt, 1, access_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_stmt, 2, new_refresh_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_stmt, 3, client_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_stmt, 4, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_stmt, 5, normalized_scope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_stmt, 6, now);
    sqlite3_bind_int64(insert_stmt, 7, access_expires);
    sqlite3_bind_int64(insert_stmt, 8, new_refresh_expires);
    StepOrThrow(insert_stmt, "rotated token insert");
    Finalize(insert_stmt);
    insert_stmt = nullptr;

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    IssuedTokenPair pair;
    pair.access_token = access_token;
    pair.refresh_token = new_refresh_token;
    pair.access_expires_in = access_ttl;
    pair.refresh_expires_in = refresh_ttl;
    pair.scope = normalized_scope;
    return pair;
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    Finalize(select_stmt);
    Finalize(revoke_stmt);
    Finalize(insert_stmt);
    throw;
  }
}

}  // namespace core
