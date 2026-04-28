#pragma once

#include <string>
#include <vector>

struct sqlite3;

namespace tomato::storage::sqlite {

class Migrator {
  public:
    Migrator(std::string sqlitePath, std::string migrationsDir);
    ~Migrator();

    void migrateAndSeed();

  private:
    std::string sqlitePath_;
    std::string migrationsDir_;
    sqlite3 *db_{nullptr};

    void ensureMigrationTable();
    void applyPendingMigrations();
    void seedDefaultData();
    void open();
    void close();
    void exec(const std::string &sql);
    int64_t queryInt64(const std::string &sql);
    int64_t queryInt64(const std::string &sql, const std::string &param);
    void execWithTwoTextParams(const std::string &sql, const std::string &a, const std::string &b);
    void execWithTwoIntParams(const std::string &sql, int64_t a, int64_t b);

    std::vector<std::string> listMigrationFiles() const;
    std::vector<std::string> splitStatements(const std::string &sqlContent) const;
    bool isMigrationApplied(const std::string &name);
    void markMigrationApplied(const std::string &name);

    int64_t ensureUser(const std::string &username, const std::string &plainPassword);
    void ensureFriendship(int64_t userId, int64_t friendId);
    int64_t ensureDirectConversation(int64_t userA, int64_t userB);
};

}  // namespace tomato::storage::sqlite
