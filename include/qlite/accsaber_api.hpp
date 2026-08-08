#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace AccSaberQLite::API {

struct ScorePayload {
    std::string mapDifficultyId;
    uint32_t score = 0;
    uint32_t scoreNoMods = 0;
    std::optional<int> maxCombo;
    std::optional<int> badCuts;
    std::optional<int> misses;
    std::optional<int> wallHits;
    std::optional<int> bombHits;
    std::optional<int> pauses;
    std::optional<int> streak115;
    std::string hmd;
    std::vector<std::string> modifierCodes;
    bool partial = false;
};

bool HasSession();
void EnsureSessionFromMainThread();
void BeginLoginFromMainThread(std::function<void(bool)> onDone);
void SubmitScore(ScorePayload payload, std::function<void(bool)> onDone);

void ComputeModifierMultiplier(std::vector<std::string> codes, std::function<void(std::optional<double>)> onDone);
void ResolveMapDifficulty(std::string songHash, std::string difficulty, std::string characteristic,
        std::function<void(std::optional<std::string>)> onDone);

}
