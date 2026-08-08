#pragma once

#include "config-utils/shared/config-utils.hpp"

DECLARE_CONFIG(ModConfig) {
    CONFIG_VALUE(RefreshToken, std::string, "refreshToken", "");
    CONFIG_VALUE(InstallationId, std::string, "installationId", "");
};
