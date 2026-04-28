#include "http/ApiServer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <cstring>

#include <drogon/MultiPart.h>
#include <sodium.h>

namespace tomato::http {

namespace {

std::string nowPlusHoursIso(uint64_t hours) {
    const auto now = std::chrono::system_clock::now();
    const auto expiry = now + std::chrono::hours(hours);
    const auto asTime = std::chrono::system_clock::to_time_t(expiry);

    std::tm tmValue{};
#ifdef _WIN32
    gmtime_s(&tmValue, &asTime);
#else
    gmtime_r(&asTime, &tmValue);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmValue, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string randomHexBytes(size_t bytes) {
    std::vector<unsigned char> buffer(bytes);
    randombytes_buf(buffer.data(), buffer.size());
    std::string out;
    out.reserve(bytes * 2);
    constexpr char kHex[] = "0123456789abcdef";
    for (const auto byte : buffer) {
        out.push_back(kHex[(byte >> 4) & 0xF]);
        out.push_back(kHex[byte & 0xF]);
    }
    return out;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string hashPasswordArgon2id(const std::string &plainPassword) {
    std::array<char, crypto_pwhash_STRBYTES> hash{};
    if (crypto_pwhash_str(hash.data(), plainPassword.c_str(), plainPassword.size(),
                          crypto_pwhash_OPSLIMIT_MODERATE,
                          crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
        throw std::runtime_error("Failed to hash password with argon2id");
    }
    return hash.data();
}

std::string trimWebWhitespace(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(0, 1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

/** Stored `media_assets.media_url` is always `/media/YYYY/mm/file.ext`; clients may send a full URL. */
std::string canonicalMediaUrlPath(const std::string &raw) {
    std::string s = trimWebWhitespace(raw);
    constexpr char kMarker[] = "/media/";
    const auto pos = s.find(kMarker);
    if (pos != std::string::npos) {
        return s.substr(pos);
    }
    return s;
}

drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode code, const std::string &message) {
    Json::Value payload;
    payload["error"] = message;
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(code);
    return response;
}

}  // namespace

ApiRepository::ApiRepository(std::string sqlitePath) : sqlitePath_(std::move(sqlitePath)) {
    open();
}

ApiRepository::~ApiRepository() {
    close();
}

void ApiRepository::open() {
    if (sqlite3_open(sqlitePath_.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("Failed to open sqlite for API repository");
    }
    exec("PRAGMA foreign_keys = ON;");
}

void ApiRepository::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void ApiRepository::exec(const std::string &sql) {
    char *error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string msg = error != nullptr ? error : "sqlite exec failed";
        sqlite3_free(error);
        throw std::runtime_error(msg);
    }
}

bool ApiRepository::hasUserProfileSchema() const {
    return usersTableHasProfileColumns();
}

bool ApiRepository::usersTableHasProfileColumns() const {
    if (usersProfileColumnsCached_) {
        return usersHasProfileColumns_;
    }
    sqlite3_stmt *pragmaStmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(users)", -1, &pragmaStmt, nullptr) != SQLITE_OK) {
        usersProfileColumnsCached_ = true;
        usersHasProfileColumns_ = false;
        return false;
    }
    bool hasBio = false;
    while (sqlite3_step(pragmaStmt) == SQLITE_ROW) {
        const char *colName = reinterpret_cast<const char *>(sqlite3_column_text(pragmaStmt, 1));
        if (colName != nullptr && std::strcmp(colName, "bio") == 0) {
            hasBio = true;
            break;
        }
    }
    sqlite3_finalize(pragmaStmt);
    usersProfileColumnsCached_ = true;
    usersHasProfileColumns_ = hasBio;
    return hasBio;
}

std::optional<std::pair<int64_t, std::string>> ApiRepository::verifyUserPassword(
    const std::string &username, const std::string &password) {
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT id, password_hash FROM users WHERE username = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    const int64_t userId = sqlite3_column_int64(stmt, 0);
    const auto *hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    const std::string hashText = hash != nullptr ? hash : "";
    sqlite3_finalize(stmt);

    if (crypto_pwhash_str_verify(hashText.c_str(), password.c_str(), password.size()) != 0) {
        return std::nullopt;
    }
    return std::make_pair(userId, username);
}

std::string ApiRepository::createAuthToken(int64_t userId, uint64_t ttlHours) {
    const std::string token = randomHexBytes(32);
    const std::string expiresAt = nowPlusHoursIso(ttlHours);

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "INSERT INTO auth_tokens(token, user_id, expires_at) VALUES(?, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare token insert");
    }
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, userId);
    sqlite3_bind_text(stmt, 3, expiresAt.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to insert token");
    }
    sqlite3_finalize(stmt);
    return token;
}

std::optional<int64_t> ApiRepository::validateToken(const std::string &token) {
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT user_id FROM auth_tokens WHERE token = ? AND datetime(expires_at) > datetime('now') LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    const int64_t userId = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return userId;
}

std::vector<FriendItem> ApiRepository::listFriends(int64_t userId) {
    const bool profileCols = usersTableHasProfileColumns();
    sqlite3_stmt *stmt = nullptr;
    const char *sql = profileCols ? "SELECT u.id, u.username, u.bio, u.avatar_media_url "
                                    "FROM friendships f JOIN users u ON u.id = f.friend_id "
                                    "WHERE f.user_id = ? AND f.status = 'accepted' ORDER BY u.username ASC"
                                  : "SELECT u.id, u.username "
                                    "FROM friendships f JOIN users u ON u.id = f.friend_id "
                                    "WHERE f.user_id = ? AND f.status = 'accepted' ORDER BY u.username ASC";
    std::vector<FriendItem> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_int64(stmt, 1, userId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FriendItem item;
        item.id = sqlite3_column_int64(stmt, 0);
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        item.username = name != nullptr ? name : "";
        if (profileCols) {
            const auto *bio = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            item.bio = bio != nullptr ? bio : "";
            if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
                const auto *av = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
                item.avatarMediaUrl = std::string(av != nullptr ? av : "");
            }
        } else {
            item.bio = "";
        }
        result.push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<int64_t> ApiRepository::findUserIdByUsername(const std::string &username) {
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT id FROM users WHERE username = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    const int64_t userId = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return userId;
}

std::optional<int64_t> ApiRepository::findDirectConversation(int64_t userA, int64_t userB) {
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT c.id FROM conversations c "
        "JOIN conversation_members m1 ON m1.conversation_id = c.id "
        "JOIN conversation_members m2 ON m2.conversation_id = c.id "
        "WHERE c.type = 'direct' AND m1.user_id = ? AND m2.user_id = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, userA);
    sqlite3_bind_int64(stmt, 2, userB);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    const int64_t conversationId = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return conversationId;
}

int64_t ApiRepository::ensureDirectConversation(int64_t userA, int64_t userB) {
    if (const auto existing = findDirectConversation(userA, userB); existing.has_value()) {
        return existing.value();
    }
    exec("INSERT INTO conversations(type) VALUES('direct')");
    const int64_t convId = sqlite3_last_insert_rowid(db_);

    sqlite3_stmt *stmt = nullptr;
    const char *memberSql =
        "INSERT OR IGNORE INTO conversation_members(conversation_id, user_id) VALUES(?, ?)";
    if (sqlite3_prepare_v2(db_, memberSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare conversation member insert");
    }
    sqlite3_bind_int64(stmt, 1, convId);
    sqlite3_bind_int64(stmt, 2, userA);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to insert member A");
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, convId);
    sqlite3_bind_int64(stmt, 2, userB);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to insert member B");
    }
    sqlite3_finalize(stmt);
    return convId;
}

std::optional<FriendItem> ApiRepository::addFriendByUsername(int64_t userId, const std::string &friendUsername) {
    const auto friendIdOpt = findUserIdByUsername(friendUsername);
    if (!friendIdOpt.has_value() || friendIdOpt.value() == userId) {
        return std::nullopt;
    }
    const int64_t friendId = friendIdOpt.value();

    sqlite3_stmt *stmt = nullptr;
    const char *friendSql =
        "INSERT OR IGNORE INTO friendships(user_id, friend_id, status) VALUES(?, ?, 'accepted')";
    if (sqlite3_prepare_v2(db_, friendSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, userId);
    sqlite3_bind_int64(stmt, 2, friendId);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, friendId);
    sqlite3_bind_int64(stmt, 2, userId);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_finalize(stmt);

    (void)ensureDirectConversation(userId, friendId);
    FriendItem item;
    item.id = friendId;
    item.username = friendUsername;
    return item;
}

bool ApiRepository::userInConversation(int64_t userId, int64_t conversationId) {
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT COUNT(1) FROM conversation_members WHERE conversation_id = ? AND user_id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, conversationId);
    sqlite3_bind_int64(stmt, 2, userId);
    const bool ok = (sqlite3_step(stmt) == SQLITE_ROW) && (sqlite3_column_int64(stmt, 0) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<ConversationItem> ApiRepository::listConversations(int64_t userId) {
    const bool profileCols = usersTableHasProfileColumns();
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        profileCols ? "SELECT c.id, u.id, u.username, u.bio, u.avatar_media_url, m.id, m.msg_type, m.content_text, "
                      "m.media_url, "
                      "m.sender_id, m.created_at "
                      "FROM conversations c "
                      "JOIN conversation_members me ON me.conversation_id = c.id AND me.user_id = ? "
                      "JOIN conversation_members other ON other.conversation_id = c.id AND other.user_id != ? "
                      "JOIN users u ON u.id = other.user_id "
                      "LEFT JOIN messages m ON m.id = (SELECT id FROM messages mm WHERE mm.conversation_id = c.id AND "
                      "mm.deleted = 0 "
                      "ORDER BY mm.id DESC LIMIT 1) "
                      "WHERE c.type = 'direct' ORDER BY c.id DESC"
                    : "SELECT c.id, u.id, u.username, m.id, m.msg_type, m.content_text, m.media_url, "
                      "m.sender_id, m.created_at "
                      "FROM conversations c "
                      "JOIN conversation_members me ON me.conversation_id = c.id AND me.user_id = ? "
                      "JOIN conversation_members other ON other.conversation_id = c.id AND other.user_id != ? "
                      "JOIN users u ON u.id = other.user_id "
                      "LEFT JOIN messages m ON m.id = (SELECT id FROM messages mm WHERE mm.conversation_id = c.id AND "
                      "mm.deleted = 0 "
                      "ORDER BY mm.id DESC LIMIT 1) "
                      "WHERE c.type = 'direct' ORDER BY c.id DESC";
    std::vector<ConversationItem> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_int64(stmt, 1, userId);
    sqlite3_bind_int64(stmt, 2, userId);
    const int msgIdCol = profileCols ? 5 : 3;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ConversationItem item;
        item.id = sqlite3_column_int64(stmt, 0);
        item.peerUserId = sqlite3_column_int64(stmt, 1);
        const auto *peer = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        item.peerUsername = peer != nullptr ? peer : "";
        if (profileCols) {
            const auto *peerBio = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            item.peerBio = peerBio != nullptr ? peerBio : "";
            if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
                const auto *av = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
                item.peerAvatarMediaUrl = std::string(av != nullptr ? av : "");
            }
        } else {
            item.peerBio = "";
        }
        if (sqlite3_column_type(stmt, msgIdCol) != SQLITE_NULL) {
            item.lastMessageId = sqlite3_column_int64(stmt, msgIdCol);
        }
        if (sqlite3_column_type(stmt, msgIdCol + 1) != SQLITE_NULL) {
            const auto *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, msgIdCol + 1));
            item.lastMessageType = std::string(v != nullptr ? v : "");
        }
        if (sqlite3_column_type(stmt, msgIdCol + 2) != SQLITE_NULL) {
            const auto *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, msgIdCol + 2));
            item.lastMessageText = std::string(v != nullptr ? v : "");
        }
        if (sqlite3_column_type(stmt, msgIdCol + 3) != SQLITE_NULL) {
            const auto *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, msgIdCol + 3));
            item.lastMessageMediaUrl = std::string(v != nullptr ? v : "");
        }
        if (sqlite3_column_type(stmt, msgIdCol + 4) != SQLITE_NULL) {
            item.lastSenderId = sqlite3_column_int64(stmt, msgIdCol + 4);
        }
        if (sqlite3_column_type(stmt, msgIdCol + 5) != SQLITE_NULL) {
            const auto *v = reinterpret_cast<const char *>(sqlite3_column_text(stmt, msgIdCol + 5));
            item.lastCreatedAt = std::string(v != nullptr ? v : "");
        }
        result.push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<MessageItem> ApiRepository::sendMessage(int64_t senderId,
                                                      int64_t conversationId,
                                                      const std::string &msgType,
                                                      const std::optional<std::string> &contentText,
                                                      const std::optional<std::string> &mediaUrl,
                                                      const std::optional<std::string> &clientMsgId) {
    if (!userInConversation(senderId, conversationId)) {
        return std::nullopt;
    }

    sqlite3_stmt *stmt = nullptr;
    const char *insertSql =
        "INSERT INTO messages(conversation_id, sender_id, msg_type, content_text, media_url, client_msg_id) "
        "VALUES(?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, conversationId);
    sqlite3_bind_int64(stmt, 2, senderId);
    sqlite3_bind_text(stmt, 3, msgType.c_str(), -1, SQLITE_TRANSIENT);
    if (contentText.has_value()) {
        sqlite3_bind_text(stmt, 4, contentText->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    if (mediaUrl.has_value()) {
        sqlite3_bind_text(stmt, 5, mediaUrl->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    if (clientMsgId.has_value()) {
        sqlite3_bind_text(stmt, 6, clientMsgId->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 6);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_finalize(stmt);
    const int64_t id = sqlite3_last_insert_rowid(db_);

    sqlite3_stmt *queryStmt = nullptr;
    const char *querySql =
        "SELECT id, conversation_id, sender_id, msg_type, content_text, media_url, client_msg_id, created_at "
        "FROM messages WHERE id = ?";
    if (sqlite3_prepare_v2(db_, querySql, -1, &queryStmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(queryStmt, 1, id);
    if (sqlite3_step(queryStmt) != SQLITE_ROW) {
        sqlite3_finalize(queryStmt);
        return std::nullopt;
    }
    MessageItem item;
    item.id = sqlite3_column_int64(queryStmt, 0);
    item.conversationId = sqlite3_column_int64(queryStmt, 1);
    item.senderId = sqlite3_column_int64(queryStmt, 2);
    item.msgType = reinterpret_cast<const char *>(sqlite3_column_text(queryStmt, 3));
    if (sqlite3_column_type(queryStmt, 4) != SQLITE_NULL) {
        item.contentText = std::string(reinterpret_cast<const char *>(sqlite3_column_text(queryStmt, 4)));
    }
    if (sqlite3_column_type(queryStmt, 5) != SQLITE_NULL) {
        item.mediaUrl = std::string(reinterpret_cast<const char *>(sqlite3_column_text(queryStmt, 5)));
    }
    if (sqlite3_column_type(queryStmt, 6) != SQLITE_NULL) {
        item.clientMsgId = std::string(reinterpret_cast<const char *>(sqlite3_column_text(queryStmt, 6)));
    }
    item.createdAt = reinterpret_cast<const char *>(sqlite3_column_text(queryStmt, 7));
    sqlite3_finalize(queryStmt);
    return item;
}

std::vector<MessageItem> ApiRepository::syncMessages(int64_t userId, int64_t conversationId, int64_t afterId, int limit) {
    std::vector<MessageItem> result;
    if (!userInConversation(userId, conversationId)) {
        return result;
    }

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT id, conversation_id, sender_id, msg_type, content_text, media_url, client_msg_id, created_at "
        "FROM messages WHERE conversation_id = ? AND id > ? AND deleted = 0 ORDER BY id ASC LIMIT ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_int64(stmt, 1, conversationId);
    sqlite3_bind_int64(stmt, 2, afterId);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MessageItem item;
        item.id = sqlite3_column_int64(stmt, 0);
        item.conversationId = sqlite3_column_int64(stmt, 1);
        item.senderId = sqlite3_column_int64(stmt, 2);
        item.msgType = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            item.contentText = std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)));
        }
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            item.mediaUrl = std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5)));
        }
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            item.clientMsgId = std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6)));
        }
        item.createdAt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
        result.push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
    return result;
}

int ApiRepository::clearConversationMessages(int64_t userId, int64_t conversationId) {
    if (!userInConversation(userId, conversationId)) {
        return -1;
    }
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "UPDATE messages SET deleted = 1 WHERE conversation_id = ? AND deleted = 0";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, conversationId);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes;
}

std::optional<int64_t> ApiRepository::createRegisteredUser(const std::string &username,
                                                             const std::string &passwordPlain) {
    if (username.empty() || passwordPlain.empty()) {
        return std::nullopt;
    }
    std::string hash;
    try {
        hash = hashPasswordArgon2id(passwordPlain);
    } catch (...) {
        return std::nullopt;
    }
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "INSERT INTO users(username, password_hash) VALUES(?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(db_);
}

std::optional<UserProfile> ApiRepository::getUserProfileRow(int64_t userId) {
    const bool profileCols = usersTableHasProfileColumns();
    sqlite3_stmt *stmt = nullptr;
    const char *sql = profileCols ? "SELECT id, username, bio, avatar_media_url FROM users WHERE id = ? LIMIT 1"
                                  : "SELECT id, username FROM users WHERE id = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, userId);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    UserProfile profile;
    profile.id = sqlite3_column_int64(stmt, 0);
    const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    profile.username = name != nullptr ? name : "";
    if (profileCols) {
        const auto *bio = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        profile.bio = bio != nullptr ? bio : "";
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            const auto *av = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            profile.avatarMediaUrl = std::string(av != nullptr ? av : "");
        }
    } else {
        profile.bio = "";
    }
    sqlite3_finalize(stmt);
    return profile;
}

std::optional<UserProfile> ApiRepository::getUserProfileForViewer(int64_t viewerId, int64_t targetUserId) {
    if (viewerId == targetUserId) {
        return getUserProfileRow(targetUserId);
    }
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT 1 FROM friendships WHERE status = 'accepted' AND "
        "((user_id = ? AND friend_id = ?) OR (user_id = ? AND friend_id = ?)) LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, viewerId);
    sqlite3_bind_int64(stmt, 2, targetUserId);
    sqlite3_bind_int64(stmt, 3, targetUserId);
    sqlite3_bind_int64(stmt, 4, viewerId);
    const bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!ok) {
        return std::nullopt;
    }
    return getUserProfileRow(targetUserId);
}

bool ApiRepository::userOwnsMediaUrl(int64_t userId, const std::string &mediaUrl) {
    const std::string path = canonicalMediaUrlPath(mediaUrl);
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT 1 FROM media_assets WHERE owner_user_id = ? AND media_url = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, userId);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return ok;
}

bool ApiRepository::updateOwnUserProfile(int64_t userId,
                                         const std::optional<std::string> &bio,
                                         const std::optional<std::string> &avatarMediaUrl) {
    if (!bio.has_value() && !avatarMediaUrl.has_value()) {
        return false;
    }
    if (!usersTableHasProfileColumns()) {
        return false;
    }
    if (avatarMediaUrl.has_value() && !avatarMediaUrl->empty() &&
        !userOwnsMediaUrl(userId, *avatarMediaUrl)) {
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (bio.has_value() && avatarMediaUrl.has_value()) {
        const char *sql = "UPDATE users SET bio = ?, avatar_media_url = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, bio->c_str(), -1, SQLITE_TRANSIENT);
        if (avatarMediaUrl->empty()) {
            sqlite3_bind_null(stmt, 2);
        } else {
            const std::string canon = canonicalMediaUrlPath(*avatarMediaUrl);
            sqlite3_bind_text(stmt, 2, canon.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(stmt, 3, userId);
    } else if (bio.has_value()) {
        const char *sql = "UPDATE users SET bio = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, bio->c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, userId);
    } else {
        const char *sql = "UPDATE users SET avatar_media_url = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        if (avatarMediaUrl->empty()) {
            sqlite3_bind_null(stmt, 1);
        } else {
            const std::string canon = canonicalMediaUrlPath(*avatarMediaUrl);
            sqlite3_bind_text(stmt, 1, canon.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(stmt, 2, userId);
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool ApiRepository::saveMediaMeta(int64_t ownerUserId,
                                  const std::string &originalName,
                                  const std::string &storedPath,
                                  const std::string &mediaUrl,
                                  const std::string &contentType,
                                  int64_t fileSize) {
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT INTO media_assets(owner_user_id, original_name, stored_path, media_url, content_type, file_size) "
        "VALUES(?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, ownerUserId);
    sqlite3_bind_text(stmt, 2, originalName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, storedPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, mediaUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, contentType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, fileSize);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool ApiRepository::findMediaPath(const std::string &mediaUrl, std::string &storedPath) {
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT stored_path FROM media_assets WHERE media_url = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, mediaUrl.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    storedPath = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return true;
}

ApiServer::ApiServer(const tomato::bootstrap::AppConfig &config)
    : config_(config), repository_(config.sqlitePath) {}

std::optional<std::string> ApiServer::readBearerToken(const drogon::HttpRequestPtr &req) {
    auto auth = req->getHeader("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.size() <= prefix.size() || auth.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    return auth.substr(prefix.size());
}

std::optional<int64_t> ApiServer::authenticate(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &callback) {
    const auto token = readBearerToken(req);
    if (!token.has_value()) {
        callback(jsonError(drogon::k401Unauthorized, "Missing bearer token"));
        return std::nullopt;
    }
    const auto userId = repository_.validateToken(token.value());
    if (!userId.has_value()) {
        callback(jsonError(drogon::k401Unauthorized, "Invalid or expired token"));
        return std::nullopt;
    }
    return userId;
}

std::string ApiServer::randomHex(size_t bytes) {
    return randomHexBytes(bytes);
}

void ApiServer::registerRoutes() {
    drogon::app().registerHandler(
        "/api/v1/auth/login",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            const auto body = req->getJsonObject();
            if (!body || body->get("username", "").asString().empty() ||
                body->get("password", "").asString().empty()) {
                callback(jsonError(drogon::k400BadRequest, "username and password are required"));
                return;
            }
            const auto username = body->get("username", "").asString();
            const auto password = body->get("password", "").asString();
            const auto user = repository_.verifyUserPassword(username, password);
            if (!user.has_value()) {
                callback(jsonError(drogon::k401Unauthorized, "Invalid credentials"));
                return;
            }
            const auto token = repository_.createAuthToken(user->first, config_.tokenTtlHours);
            Json::Value payload;
            payload["token"] = token;
            payload["user"]["id"] = Json::Int64(user->first);
            payload["user"]["username"] = user->second;
            if (const auto profile = repository_.getUserProfileRow(user->first); profile.has_value()) {
                payload["user"]["bio"] = profile->bio;
                if (profile->avatarMediaUrl.has_value()) {
                    payload["user"]["avatarMediaUrl"] = *profile->avatarMediaUrl;
                } else {
                    payload["user"]["avatarMediaUrl"] = "";
                }
            }
            payload["expiresInHours"] = Json::UInt64(config_.tokenTtlHours);
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/auth/register",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            if (config_.registerApiKey.empty()) {
                callback(jsonError(drogon::k403Forbidden, "Registration is disabled on this server"));
                return;
            }
            const auto headerKey = req->getHeader("x-tomato-register-key");
            if (headerKey != config_.registerApiKey) {
                callback(jsonError(drogon::k403Forbidden, "Invalid registration key"));
                return;
            }
            const auto body = req->getJsonObject();
            if (!body || body->get("username", "").asString().empty() ||
                body->get("password", "").asString().empty()) {
                callback(jsonError(drogon::k400BadRequest, "username and password are required"));
                return;
            }
            const auto username = body->get("username", "").asString();
            const auto password = body->get("password", "").asString();
            const auto createdId = repository_.createRegisteredUser(username, password);
            if (!createdId.has_value()) {
                callback(jsonError(drogon::k409Conflict, "Username already exists or registration failed"));
                return;
            }
            Json::Value payload;
            payload["id"] = Json::Int64(createdId.value());
            payload["username"] = username;
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/friends",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            const auto friends = repository_.listFriends(userId.value());
            Json::Value payload(Json::arrayValue);
            for (const auto &friendItem : friends) {
                Json::Value item;
                item["id"] = Json::Int64(friendItem.id);
                item["username"] = friendItem.username;
                item["bio"] = friendItem.bio;
                if (friendItem.avatarMediaUrl.has_value()) {
                    item["avatarMediaUrl"] = *friendItem.avatarMediaUrl;
                } else {
                    item["avatarMediaUrl"] = "";
                }
                payload.append(item);
            }
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/friends/add",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            const auto body = req->getJsonObject();
            const auto username = body ? body->get("username", "").asString() : "";
            if (username.empty()) {
                callback(jsonError(drogon::k400BadRequest, "username is required"));
                return;
            }
            const auto friendItem = repository_.addFriendByUsername(userId.value(), username);
            if (!friendItem.has_value()) {
                callback(jsonError(drogon::k404NotFound, "Friend user not found or invalid"));
                return;
            }
            const auto conversationId =
                repository_.ensureDirectConversation(userId.value(), friendItem->id);
            Json::Value payload;
            payload["friend"]["id"] = Json::Int64(friendItem->id);
            payload["friend"]["username"] = friendItem->username;
            payload["conversationId"] = Json::Int64(conversationId);
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/conversations",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            const auto list = repository_.listConversations(userId.value());
            Json::Value payload(Json::arrayValue);
            for (const auto &conversation : list) {
                Json::Value item;
                item["id"] = Json::Int64(conversation.id);
                item["type"] = "direct";
                item["peer"]["id"] = Json::Int64(conversation.peerUserId);
                item["peer"]["username"] = conversation.peerUsername;
                item["peer"]["bio"] = conversation.peerBio;
                if (conversation.peerAvatarMediaUrl.has_value()) {
                    item["peer"]["avatarMediaUrl"] = *conversation.peerAvatarMediaUrl;
                } else {
                    item["peer"]["avatarMediaUrl"] = "";
                }
                if (conversation.lastMessageId.has_value()) {
                    item["lastMessage"]["id"] = Json::Int64(*conversation.lastMessageId);
                    item["lastMessage"]["msgType"] =
                        conversation.lastMessageType.value_or("");
                    item["lastMessage"]["contentText"] =
                        conversation.lastMessageText.value_or("");
                    item["lastMessage"]["mediaUrl"] =
                        conversation.lastMessageMediaUrl.value_or("");
                    item["lastMessage"]["senderId"] =
                        Json::Int64(conversation.lastSenderId.value_or(0));
                    item["lastMessage"]["createdAt"] =
                        conversation.lastCreatedAt.value_or("");
                }
                payload.append(item);
            }
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/messages/send",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            const auto body = req->getJsonObject();
            if (!body || !body->isMember("conversationId") || !body->isMember("msgType")) {
                callback(jsonError(drogon::k400BadRequest, "conversationId and msgType are required"));
                return;
            }
            const int64_t conversationId = body->get("conversationId", 0).asInt64();
            const std::string msgType = toLower(body->get("msgType", "").asString());
            if (msgType != "text" && msgType != "image" && msgType != "sticker") {
                callback(jsonError(drogon::k400BadRequest, "msgType must be text/image/sticker"));
                return;
            }

            std::optional<std::string> contentText;
            if (body->isMember("contentText")) {
                const auto value = body->get("contentText", "").asString();
                if (!value.empty()) {
                    contentText = value;
                }
            }
            std::optional<std::string> mediaUrl;
            if (body->isMember("mediaUrl")) {
                const auto value = body->get("mediaUrl", "").asString();
                if (!value.empty()) {
                    mediaUrl = value;
                }
            }
            std::optional<std::string> clientMsgId;
            if (body->isMember("clientMsgId")) {
                const auto value = body->get("clientMsgId", "").asString();
                if (!value.empty()) {
                    clientMsgId = value;
                }
            }

            if (msgType == "text" && !contentText.has_value()) {
                callback(jsonError(drogon::k400BadRequest, "text message requires contentText"));
                return;
            }
            if ((msgType == "image" || msgType == "sticker") && !mediaUrl.has_value()) {
                callback(jsonError(drogon::k400BadRequest, "media message requires mediaUrl"));
                return;
            }

            const auto saved = repository_.sendMessage(userId.value(),
                                                       conversationId,
                                                       msgType,
                                                       contentText,
                                                       mediaUrl,
                                                       clientMsgId);
            if (!saved.has_value()) {
                callback(jsonError(drogon::k403Forbidden, "Conversation unavailable"));
                return;
            }

            Json::Value payload;
            payload["id"] = Json::Int64(saved->id);
            payload["conversationId"] = Json::Int64(saved->conversationId);
            payload["senderId"] = Json::Int64(saved->senderId);
            payload["msgType"] = saved->msgType;
            payload["contentText"] = saved->contentText.value_or("");
            payload["mediaUrl"] = saved->mediaUrl.value_or("");
            payload["clientMsgId"] = saved->clientMsgId.value_or("");
            payload["createdAt"] = saved->createdAt;
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/messages/sync",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            const auto conversationId = std::stoll(req->getParameter("conversationId").empty()
                                                       ? "0"
                                                       : req->getParameter("conversationId"));
            const auto afterId = std::stoll(req->getParameter("afterId").empty()
                                                ? "0"
                                                : req->getParameter("afterId"));
            int limit = 50;
            if (!req->getParameter("limit").empty()) {
                limit = std::stoi(req->getParameter("limit"));
            }
            limit = std::clamp(limit, 1, 200);

            const auto messages = repository_.syncMessages(userId.value(), conversationId, afterId, limit);
            Json::Value payload;
            payload["conversationId"] = Json::Int64(conversationId);
            Json::Value rows(Json::arrayValue);
            int64_t cursor = afterId;
            for (const auto &message : messages) {
                Json::Value item;
                item["id"] = Json::Int64(message.id);
                item["conversationId"] = Json::Int64(message.conversationId);
                item["senderId"] = Json::Int64(message.senderId);
                item["msgType"] = message.msgType;
                item["contentText"] = message.contentText.value_or("");
                item["mediaUrl"] = message.mediaUrl.value_or("");
                item["clientMsgId"] = message.clientMsgId.value_or("");
                item["createdAt"] = message.createdAt;
                rows.append(item);
                cursor = (std::max)(cursor, message.id);
            }
            payload["messages"] = rows;
            payload["nextAfterId"] = Json::Int64(cursor);
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/messages/clear",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            const auto body = req->getJsonObject();
            if (!body || !body->isMember("conversationId")) {
                callback(jsonError(drogon::k400BadRequest, "conversationId is required"));
                return;
            }
            const int64_t conversationId = body->get("conversationId", 0).asInt64();
            const int cleared = repository_.clearConversationMessages(userId.value(), conversationId);
            if (cleared < 0) {
                callback(jsonError(drogon::k403Forbidden, "Conversation unavailable"));
                return;
            }
            Json::Value payload;
            payload["conversationId"] = Json::Int64(conversationId);
            payload["clearedCount"] = cleared;
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/profile/me",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            if (req->method() == drogon::Get) {
                const auto profile = repository_.getUserProfileRow(userId.value());
                if (!profile.has_value()) {
                    callback(jsonError(drogon::k404NotFound, "User not found"));
                    return;
                }
                Json::Value payload;
                payload["id"] = Json::Int64(profile->id);
                payload["username"] = profile->username;
                payload["bio"] = profile->bio;
                if (profile->avatarMediaUrl.has_value()) {
                    payload["avatarMediaUrl"] = *profile->avatarMediaUrl;
                } else {
                    payload["avatarMediaUrl"] = "";
                }
                callback(drogon::HttpResponse::newHttpJsonResponse(payload));
                return;
            }
            if (req->method() == drogon::Patch) {
                const auto body = req->getJsonObject();
                if (!body) {
                    callback(jsonError(drogon::k400BadRequest, "JSON body required"));
                    return;
                }
                std::optional<std::string> bio;
                std::optional<std::string> avatarMediaUrl;
                if (body->isMember("bio")) {
                    bio = body->get("bio", "").asString();
                }
                if (body->isMember("avatarMediaUrl")) {
                    const auto v = body->get("avatarMediaUrl", "").asString();
                    if (!v.empty()) {
                        avatarMediaUrl = canonicalMediaUrlPath(v);
                    } else {
                        avatarMediaUrl = std::string("");
                    }
                }
                if (!bio.has_value() && !avatarMediaUrl.has_value()) {
                    callback(jsonError(drogon::k400BadRequest, "JSON body must include bio and/or avatarMediaUrl"));
                    return;
                }
                if (!repository_.hasUserProfileSchema()) {
                    callback(jsonError(
                        drogon::k503ServiceUnavailable,
                        "Database missing profile columns (users.bio / avatar_media_url). Apply "
                        "migrations/003_user_profile.sql and restart."));
                    return;
                }
                if (!repository_.updateOwnUserProfile(userId.value(), bio, avatarMediaUrl)) {
                    callback(jsonError(drogon::k400BadRequest, "Nothing to update or invalid avatarMediaUrl"));
                    return;
                }
                const auto profile = repository_.getUserProfileRow(userId.value());
                Json::Value payload;
                if (profile.has_value()) {
                    payload["id"] = Json::Int64(profile->id);
                    payload["username"] = profile->username;
                    payload["bio"] = profile->bio;
                    payload["avatarMediaUrl"] = profile->avatarMediaUrl.value_or("");
                }
                callback(drogon::HttpResponse::newHttpJsonResponse(payload));
                return;
            }
            callback(jsonError(drogon::k405MethodNotAllowed, "Method not allowed"));
        },
        {drogon::Get, drogon::Patch});

    drogon::app().registerHandler(
        "/api/v1/users/{}",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback,
               const std::string &targetUserIdStr) {
            auto viewerId = authenticate(req, callback);
            if (!viewerId.has_value()) {
                return;
            }
            if (req->method() != drogon::Get) {
                callback(jsonError(drogon::k405MethodNotAllowed, "Method not allowed"));
                return;
            }
            int64_t targetUserId = 0;
            try {
                targetUserId = std::stoll(targetUserIdStr);
            } catch (...) {
                callback(jsonError(drogon::k400BadRequest, "Invalid user id"));
                return;
            }
            const auto profile = repository_.getUserProfileForViewer(viewerId.value(), targetUserId);
            if (!profile.has_value()) {
                callback(jsonError(drogon::k404NotFound, "Profile not found"));
                return;
            }
            Json::Value payload;
            payload["id"] = Json::Int64(profile->id);
            payload["username"] = profile->username;
            payload["bio"] = profile->bio;
            payload["avatarMediaUrl"] = profile->avatarMediaUrl.value_or("");
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/media/upload",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            drogon::MultiPartParser parser;
            if (parser.parse(req) != 0 || parser.getFiles().empty()) {
                callback(jsonError(drogon::k400BadRequest, "multipart file is required"));
                return;
            }

            const auto &file = parser.getFiles().front();
            if (file.fileLength() == 0 || file.fileLength() > config_.maxUploadBytes) {
                callback(jsonError(drogon::k400BadRequest, "file size invalid"));
                return;
            }
            auto extension = toLower(std::string(file.getFileExtension()));
            if (!extension.empty() && extension[0] == '.') {
                extension = extension.substr(1);
            }
            const std::array<std::string, 5> allowed = {"png", "jpg", "jpeg", "webp", "gif"};
            if (std::find(allowed.begin(), allowed.end(), extension) == allowed.end()) {
                callback(jsonError(drogon::k400BadRequest, "only image file types are supported"));
                return;
            }

            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tmValue{};
#ifdef _WIN32
            localtime_s(&tmValue, &time);
#else
            localtime_r(&time, &tmValue);
#endif
            std::ostringstream folderBuilder;
            folderBuilder << std::put_time(&tmValue, "%Y/%m");
            const std::string ym = folderBuilder.str();
            const auto outputDir = std::filesystem::path(config_.mediaRootDir) / ym;
            std::filesystem::create_directories(outputDir);

            std::string filename = randomHex(16) + "." + extension;
            auto fullPath = outputDir / filename;
            std::ofstream out(fullPath, std::ios::binary);
            out.write(file.fileData(), static_cast<std::streamsize>(file.fileLength()));
            out.close();
            if (!out) {
                callback(jsonError(drogon::k500InternalServerError, "failed to persist file"));
                return;
            }

            const std::string mediaUrl = "/media/" + ym + "/" + filename;
            if (!repository_.saveMediaMeta(userId.value(),
                                           file.getFileName(),
                                           fullPath.string(),
                                           mediaUrl,
                                           "image/" + extension,
                                           static_cast<int64_t>(file.fileLength()))) {
                callback(jsonError(drogon::k500InternalServerError, "failed to save media meta"));
                return;
            }

            Json::Value payload;
            payload["mediaUrl"] = mediaUrl;
            payload["size"] = Json::UInt64(file.fileLength());
            payload["originalName"] = file.getFileName();
            callback(drogon::HttpResponse::newHttpJsonResponse(payload));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/media/{}/{}/{}",
        [this](const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback,
               const std::string &year,
               const std::string &month,
               const std::string &file) {
            auto userId = authenticate(req, callback);
            if (!userId.has_value()) {
                return;
            }
            const std::string mediaUrl = "/media/" + year + "/" + month + "/" + file;
            std::string storedPath;
            if (!repository_.findMediaPath(mediaUrl, storedPath) || !std::filesystem::exists(storedPath)) {
                callback(jsonError(drogon::k404NotFound, "media not found"));
                return;
            }
            callback(drogon::HttpResponse::newFileResponse(storedPath, "", drogon::CT_NONE, "", req));
        },
        {drogon::Get});
}

}  // namespace tomato::http
