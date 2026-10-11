#pragma once

#include "config-utils/shared/config-utils.hpp"

DECLARE_CONFIG(ModConfig) {
    CONFIG_VALUE(RefreshToken, std::string, "refreshToken", "");
    CONFIG_VALUE(AccessToken, std::string, "accessToken", "");
    CONFIG_VALUE(AccessExpiresAt, int, "accessExpiresAt", 0);
    CONFIG_VALUE(InstallationId, std::string, "installationId", "");
};
