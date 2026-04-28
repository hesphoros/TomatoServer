#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tomato::bootstrap {

struct AppConfig {
    std::string bindAddress = "0.0.0.0";
    uint16_t httpsPort = 8443;
    std::string tlsCertPath = "config/dev-cert.pem";
    std::string tlsKeyPath = "config/dev-key.pem";
    std::string sqlitePath = "storage/db/tomato.db";
    std::string migrationsDir = "migrations";
    std::string mediaRootDir = "uploads/media";
    uint64_t maxUploadBytes = 10ULL * 1024ULL * 1024ULL;
    uint64_t tokenTtlHours = 168;
    /** If empty, POST /api/v1/auth/register is disabled. Set locally for operator-only registration. */
    std::string registerApiKey;
    size_t dbConnectionCount = 1;
    size_t threadCount = 1;
};

AppConfig loadAppConfig(const std::string &path);

}  // namespace tomato::bootstrap
