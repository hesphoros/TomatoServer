#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <drogon/drogon.h>
#include <sqlite3.h>

#include "bootstrap/AppConfig.h"

namespace tomato::http {

struct FriendItem {
    int64_t id{0};
    std::string username;
    std::string bio;
    std::optional<std::string> avatarMediaUrl;
};

struct UserProfile {
    int64_t id{0};
    std::string username;
    std::string bio;
    std::optional<std::string> avatarMediaUrl;
};

struct ConversationItem {
    int64_t id{0};
    int64_t peerUserId{0};
    std::string peerUsername;
    std::string peerBio;
    std::optional<std::string> peerAvatarMediaUrl;
    std::optional<int64_t> lastMessageId;
    std::optional<std::string> lastMessageType;
    std::optional<std::string> lastMessageText;
    std::optional<std::string> lastMessageMediaUrl;
    std::optional<int64_t> lastSenderId;
    std::optional<std::string> lastCreatedAt;
};

struct MessageItem {
    int64_t id{0};
    int64_t conversationId{0};
    int64_t senderId{0};
    std::string msgType;
    std::optional<std::string> contentText;
    std::optional<std::string> mediaUrl;
    std::optional<std::string> clientMsgId;
    std::string createdAt;
};

class ApiRepository {
  public:
    explicit ApiRepository(std::string sqlitePath);
    ~ApiRepository();

    ApiRepository(const ApiRepository &) = delete;
    ApiRepository &operator=(const ApiRepository &) = delete;

    std::optional<std::pair<int64_t, std::string>> verifyUserPassword(const std::string &username,
                                                                       const std::string &password);
    std::string createAuthToken(int64_t userId, uint64_t ttlHours);
    std::optional<int64_t> validateToken(const std::string &token);

    std::vector<FriendItem> listFriends(int64_t userId);
    std::optional<FriendItem> addFriendByUsername(int64_t userId, const std::string &friendUsername);
    std::vector<ConversationItem> listConversations(int64_t userId);

    std::optional<MessageItem> sendMessage(int64_t senderId,
                                           int64_t conversationId,
                                           const std::string &msgType,
                                           const std::optional<std::string> &contentText,
                                           const std::optional<std::string> &mediaUrl,
                                           const std::optional<std::string> &clientMsgId);
    std::vector<MessageItem> syncMessages(int64_t userId, int64_t conversationId, int64_t afterId, int limit);
    int clearConversationMessages(int64_t userId, int64_t conversationId);

    std::optional<int64_t> createRegisteredUser(const std::string &username, const std::string &passwordPlain);
    std::optional<UserProfile> getUserProfileRow(int64_t userId);
    std::optional<UserProfile> getUserProfileForViewer(int64_t viewerId, int64_t targetUserId);
    bool updateOwnUserProfile(int64_t userId,
                              const std::optional<std::string> &bio,
                              const std::optional<std::string> &avatarMediaUrl);
    bool userOwnsMediaUrl(int64_t userId, const std::string &mediaUrl);
    /** True when migration 003 has been applied (`users.bio` exists). */
    bool hasUserProfileSchema() const;

    std::optional<int64_t> findUserIdByUsername(const std::string &username);
    std::optional<int64_t> findDirectConversation(int64_t userA, int64_t userB);
    int64_t ensureDirectConversation(int64_t userA, int64_t userB);
    bool userInConversation(int64_t userId, int64_t conversationId);

    bool saveMediaMeta(int64_t ownerUserId,
                       const std::string &originalName,
                       const std::string &storedPath,
                       const std::string &mediaUrl,
                       const std::string &contentType,
                       int64_t fileSize);
    bool findMediaPath(const std::string &mediaUrl, std::string &storedPath);

  private:
    std::string sqlitePath_;
    sqlite3 *db_{nullptr};
    /** Cached: whether `users` has `bio` (implies profile migration 003 applied). */
    mutable bool usersProfileColumnsCached_{false};
    mutable bool usersHasProfileColumns_{false};

    void open();
    void close();
    void exec(const std::string &sql);
    bool usersTableHasProfileColumns() const;
};

class ApiServer {
  public:
    explicit ApiServer(const tomato::bootstrap::AppConfig &config);
    void registerRoutes();

  private:
    tomato::bootstrap::AppConfig config_;
    ApiRepository repository_;

    std::optional<int64_t> authenticate(const drogon::HttpRequestPtr &req,
                                        std::function<void(const drogon::HttpResponsePtr &)> &callback);

    static std::string randomHex(size_t bytes);
    static std::optional<std::string> readBearerToken(const drogon::HttpRequestPtr &req);
};

}  // namespace tomato::http
