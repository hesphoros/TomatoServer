#include "bootstrap/AppConfig.h"

#include <fstream>
#include <stdexcept>

#include <json/json.h>

namespace tomato::bootstrap {

namespace {

template <typename Numeric>
Numeric readNumber(const Json::Value &node, const char *key, Numeric defaultValue) {
    if (!node.isMember(key)) {
        return defaultValue;
    }
    return static_cast<Numeric>(node[key].asLargestUInt());
}

}  // namespace

AppConfig loadAppConfig(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open config file: " + path);
    }

    Json::Value root;
    in >> root;

    AppConfig config;
    config.bindAddress = root.get("bindAddress", config.bindAddress).asString();
    config.httpsPort = readNumber<uint16_t>(root, "httpsPort", config.httpsPort);
    config.tlsCertPath = root.get("tlsCertPath", config.tlsCertPath).asString();
    config.tlsKeyPath = root.get("tlsKeyPath", config.tlsKeyPath).asString();
    config.sqlitePath = root.get("sqlitePath", config.sqlitePath).asString();
    config.migrationsDir = root.get("migrationsDir", config.migrationsDir).asString();
    config.mediaRootDir = root.get("mediaRootDir", config.mediaRootDir).asString();
    config.maxUploadBytes = readNumber<uint64_t>(root, "maxUploadBytes", config.maxUploadBytes);
    config.tokenTtlHours = readNumber<uint64_t>(root, "tokenTtlHours", config.tokenTtlHours);
    config.registerApiKey = root.get("registerApiKey", config.registerApiKey).asString();
    config.dbConnectionCount =
        readNumber<size_t>(root, "dbConnectionCount", config.dbConnectionCount);
    config.threadCount = readNumber<size_t>(root, "threadCount", config.threadCount);

    return config;
}

}  // namespace tomato::bootstrap
