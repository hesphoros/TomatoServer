#include <filesystem>
#include <iostream>
#include <vector>

#include <drogon/drogon.h>
#include <sodium.h>

#include "bootstrap/AppConfig.h"
#include "http/ApiServer.h"
#include "storage/sqlite/Migrator.h"

namespace {
tomato::bootstrap::AppConfig loadConfigFromKnownLocations() {
    const std::vector<std::string> candidates = {
        "config/server.dev.json",
        "../../config/server.dev.json"
    };
    for (const auto &path : candidates) {
        if (std::filesystem::exists(path)) {
            return tomato::bootstrap::loadAppConfig(path);
        }
    }
    throw std::runtime_error("Failed to open config file: config/server.dev.json");
}
}  // namespace

int main() {
    try {
        if (sodium_init() < 0) {
            throw std::runtime_error("Failed to initialize libsodium");
        }

        const auto config = loadConfigFromKnownLocations();
        std::filesystem::create_directories(std::filesystem::path(config.sqlitePath).parent_path());
        std::filesystem::create_directories(config.mediaRootDir);
        tomato::storage::sqlite::Migrator migrator(config.sqlitePath, config.migrationsDir);
        migrator.migrateAndSeed();
        tomato::http::ApiServer apiServer(config);

        drogon::app().setThreadNum(static_cast<size_t>(config.threadCount));
        drogon::app().addListener(config.bindAddress,
                                  config.httpsPort,
                                  true,
                                  config.tlsCertPath,
                                  config.tlsKeyPath);

        drogon::app().registerHandler(
            "/healthz",
            [](const drogon::HttpRequestPtr &,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                Json::Value payload;
                payload["status"] = "ok";
                auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
                callback(response);
            },
            {drogon::internal::HttpConstraint(drogon::Get)});
        apiServer.registerRoutes();

        std::cout << "TomatoServer is listening on https://" << config.bindAddress << ":"
                  << config.httpsPort << '\n';
        drogon::app().run();
    } catch (const std::exception &ex) {
        std::cerr << "Fatal startup error: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
