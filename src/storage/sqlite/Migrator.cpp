#include "storage/sqlite/Migrator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <sqlite3.h>
#include <sodium.h>

namespace tomato::storage::sqlite {

namespace {

std::string trim(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
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

}  // namespace

Migrator::Migrator(std::string sqlitePath, std::string migrationsDir)
    : sqlitePath_(std::move(sqlitePath)), migrationsDir_(std::move(migrationsDir)) {}

Migrator::~Migrator() {
    close();
}

void Migrator::migrateAndSeed() {
    open();
    ensureMigrationTable();
    applyPendingMigrations();
    seedDefaultData();
}

void Migrator::open() {
    if (db_ != nullptr) {
        return;
    }
    if (sqlite3_open(sqlitePath_.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("Failed to open sqlite database: " + sqlitePath_);
    }
    exec("PRAGMA foreign_keys = ON;");
}

void Migrator::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Migrator::exec(const std::string &sql) {
    char *errorMessage = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errorMessage) != SQLITE_OK) {
        const std::string message = errorMessage != nullptr ? errorMessage : "sqlite exec failed";
        sqlite3_free(errorMessage);
        throw std::runtime_error(message + " | sql: " + sql);
    }
}

int64_t Migrator::queryInt64(const std::string &sql) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare failed");
    }
    const int step = sqlite3_step(stmt);
    int64_t value = 0;
    if (step == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

int64_t Migrator::queryInt64(const std::string &sql, const std::string &param) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare failed");
    }
    sqlite3_bind_text(stmt, 1, param.c_str(), -1, SQLITE_TRANSIENT);
    const int step = sqlite3_step(stmt);
    int64_t value = 0;
    if (step == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

void Migrator::execWithTwoTextParams(const std::string &sql,
                                     const std::string &a,
                                     const std::string &b) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare failed");
    }
    sqlite3_bind_text(stmt, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, b.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite step failed");
    }
    sqlite3_finalize(stmt);
}

void Migrator::execWithTwoIntParams(const std::string &sql, int64_t a, int64_t b) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare failed");
    }
    sqlite3_bind_int64(stmt, 1, a);
    sqlite3_bind_int64(stmt, 2, b);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite step failed");
    }
    sqlite3_finalize(stmt);
}

void Migrator::ensureMigrationTable() {
    exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "name TEXT PRIMARY KEY,"
        "applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ")");
}

std::vector<std::string> Migrator::listMigrationFiles() const {
    std::vector<std::string> files;
    for (const auto &entry : std::filesystem::directory_iterator(migrationsDir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sql") {
            files.push_back(entry.path().filename().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> Migrator::splitStatements(const std::string &sqlContent) const {
    std::vector<std::string> statements;
    std::string current;
    for (char ch : sqlContent) {
        current.push_back(ch);
        if (ch == ';') {
            auto statement = trim(current);
            if (!statement.empty()) {
                statements.push_back(statement);
            }
            current.clear();
        }
    }

    auto remainder = trim(current);
    if (!remainder.empty()) {
        statements.push_back(remainder);
    }
    return statements;
}

bool Migrator::isMigrationApplied(const std::string &name) {
    return queryInt64("SELECT COUNT(1) FROM schema_migrations WHERE name = ?", name) > 0;
}

void Migrator::markMigrationApplied(const std::string &name) {
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "INSERT INTO schema_migrations(name) VALUES(?)";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare failed");
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("failed to mark migration");
    }
    sqlite3_finalize(stmt);
}

void Migrator::applyPendingMigrations() {
    const auto files = listMigrationFiles();
    for (const auto &file : files) {
        if (isMigrationApplied(file)) {
            continue;
        }

        const auto path = std::filesystem::path(migrationsDir_) / file;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error("Failed to open migration: " + path.string());
        }
        const std::string sql((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

        const auto statements = splitStatements(sql);
        for (const auto &statement : statements) {
            exec(statement);
        }
        markMigrationApplied(file);
    }
}

int64_t Migrator::ensureUser(const std::string &username, const std::string &plainPassword) {
    int64_t userId = queryInt64("SELECT id FROM users WHERE username = ? LIMIT 1", username);
    if (userId > 0) {
        return userId;
    }

    const auto hash = hashPasswordArgon2id(plainPassword);
    execWithTwoTextParams("INSERT INTO users(username, password_hash) VALUES(?, ?)",
                          username,
                          hash);

    userId = queryInt64("SELECT id FROM users WHERE username = ? LIMIT 1", username);
    if (userId <= 0) {
        throw std::runtime_error("Failed to create default user: " + username);
    }
    return userId;
}

void Migrator::ensureFriendship(int64_t userId, int64_t friendId) {
    sqlite3_stmt *stmt = nullptr;
    const std::string sql =
        "INSERT OR IGNORE INTO friendships(user_id, friend_id, status) VALUES(?, ?, 'accepted')";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare failed");
    }
    sqlite3_bind_int64(stmt, 1, userId);
    sqlite3_bind_int64(stmt, 2, friendId);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("failed to insert friendship");
    }
    sqlite3_finalize(stmt);
}

int64_t Migrator::ensureDirectConversation(int64_t userA, int64_t userB) {
    sqlite3_stmt *stmt = nullptr;
    const std::string query =
        "SELECT c.id FROM conversations c "
        "JOIN conversation_members m1 ON m1.conversation_id = c.id "
        "JOIN conversation_members m2 ON m2.conversation_id = c.id "
        "WHERE c.type = 'direct' AND m1.user_id = ? AND m2.user_id = ? "
        "LIMIT 1";
    if (sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare failed");
    }
    sqlite3_bind_int64(stmt, 1, userA);
    sqlite3_bind_int64(stmt, 2, userB);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    sqlite3_finalize(stmt);

    exec("INSERT INTO conversations(type) VALUES('direct')");
    const int64_t conversationId = queryInt64("SELECT last_insert_rowid()");

    execWithTwoIntParams(
        "INSERT OR IGNORE INTO conversation_members(conversation_id, user_id) VALUES(?, ?)",
        conversationId,
        userA);
    execWithTwoIntParams(
        "INSERT OR IGNORE INTO conversation_members(conversation_id, user_id) VALUES(?, ?)",
        conversationId,
        userB);
    return conversationId;
}

void Migrator::seedDefaultData() {
    const int64_t hesphorosId = ensureUser("hesphoros", "hesphoros");
    const int64_t ruansiqiId = ensureUser("ruansiqi", "ruansiqi");

    ensureFriendship(hesphorosId, ruansiqiId);
    ensureFriendship(ruansiqiId, hesphorosId);
    (void)ensureDirectConversation(hesphorosId, ruansiqiId);
}

}  // namespace tomato::storage::sqlite
