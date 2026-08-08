#include "qlite/score_tracker.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "main.hpp"
#include "qlite/accsaber_api.hpp"

#include "custom-types/shared/delegate.hpp"
#include "metacore/shared/game.hpp"

#include "GlobalNamespace/BadCutScoringElement.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapObjectManager.hpp"
#include "GlobalNamespace/GameplayCoreInstaller.hpp"
#include "GlobalNamespace/GameplayCoreSceneSetupData.hpp"
#include "GlobalNamespace/GameplayModifiers.hpp"
#include "GlobalNamespace/IReadonlyBeatmapData.hpp"
#include "GlobalNamespace/LevelCompletionResults.hpp"
#include "GlobalNamespace/MissScoringElement.hpp"
#include "GlobalNamespace/NoteController.hpp"
#include "GlobalNamespace/NoteCutInfo.hpp"
#include "GlobalNamespace/NoteData.hpp"
#include "GlobalNamespace/OVRPlugin.hpp"
#include "GlobalNamespace/ObstacleController.hpp"
#include "GlobalNamespace/PauseMenuManager.hpp"
#include "GlobalNamespace/PlayerHeadAndObstacleInteraction.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "GlobalNamespace/ScoreController.hpp"
#include "GlobalNamespace/ScoringElement.hpp"
#include "GlobalNamespace/StandardLevelScenesTransitionSetupDataSO.hpp"
#include "System/Action_1.hpp"

using namespace GlobalNamespace;

namespace AccSaberQLite::Tracker {

namespace {

constexpr float COMPLETION_THRESHOLD = 0.75f;
constexpr int MIN_TOTAL_NOTES = 115;
constexpr std::string_view CUSTOM_LEVEL_PREFIX = "custom_level_";

bool tracking = false;
int totalNotes = 0;
int notes = 0;
int misses = 0;
int badCuts = 0;
int bombHits = 0;
int wallHits = 0;
int pauses = 0;
int streak115 = 0;
int current115Streak = 0;

bool AtEndsOfMap() {
    return notes == 0 || notes == totalNotes;
}

void ResetState() {
    tracking = false;
    totalNotes = 0;
    notes = 0;
    misses = 0;
    badCuts = 0;
    bombHits = 0;
    wallHits = 0;
    pauses = 0;
    streak115 = 0;
    current115Streak = 0;
}

void OnScoringElementFinished(ScoringElement* element) {
    if (!tracking || !element) return;

    NoteData* noteData = element->noteData;
    if (!noteData || noteData->gameplayType == NoteData_GameplayType::Bomb) return;

    NoteData_ScoringType scoringType = noteData->scoringType;
    if (scoringType == NoteData_ScoringType::Ignore) return;

    notes++;

    bool miss = false;
    if (il2cpp_utils::try_cast<MissScoringElement>(element)) {
        misses++;
        miss = true;
    } else if (il2cpp_utils::try_cast<BadCutScoringElement>(element)) {
        badCuts++;
        miss = true;
    }

    if (scoringType == NoteData_ScoringType::NoScore || miss) {
        current115Streak = 0;
        return;
    }

    if (element->cutScore == 115) {
        current115Streak++;
    } else if (current115Streak > 0) {
        streak115 = std::max(streak115, current115Streak);
        current115Streak = 0;
    }
}

void OnWallHit(UnityW<ObstacleController> obstacle) {
    if (!tracking || AtEndsOfMap()) return;
    wallHits++;
}

std::string HeadsetName() {
    switch (OVRPlugin::GetSystemHeadsetType()) {
        case OVRPlugin::SystemHeadset::Oculus_Quest:
            return "Oculus Quest";
        case OVRPlugin::SystemHeadset::Oculus_Quest_2:
            return "Oculus Quest 2";
        case OVRPlugin::SystemHeadset::Meta_Quest_Pro:
            return "Meta Quest Pro";
        case OVRPlugin::SystemHeadset::Meta_Quest_3:
            return "Meta Quest 3";
        default:
            return "Oculus Quest";
    }
}

std::vector<std::string> ModifierCodes(GameplayModifiers* mods, bool failed) {
    std::vector<std::string> codes;
    if (!mods) return codes;
    if (mods->noFailOn0Energy && failed) codes.emplace_back("NF");
    if (mods->enabledObstacleType == GameplayModifiers_EnabledObstacleType::NoObstacles) codes.emplace_back("NO");
    if (mods->noBombs) codes.emplace_back("NB");
    switch (mods->songSpeed) {
        case GameplayModifiers_SongSpeed::Slower:
            codes.emplace_back("SS");
            break;
        case GameplayModifiers_SongSpeed::Faster:
            codes.emplace_back("FS");
            break;
        case GameplayModifiers_SongSpeed::SuperFast:
            codes.emplace_back("SF");
            break;
        default:
            break;
    }
    if (mods->ghostNotes) codes.emplace_back("GN");
    if (mods->disappearingArrows) codes.emplace_back("DA");
    if (mods->proMode) codes.emplace_back("PM");
    if (mods->smallCubes) codes.emplace_back("SC");
    if (mods->instaFail) codes.emplace_back("IF");
    return codes;
}

std::string DifficultyName(int difficulty) {
    switch (difficulty) {
        case 0: return "EASY";
        case 1: return "NORMAL";
        case 2: return "HARD";
        case 3: return "EXPERT";
        case 4: return "EXPERT_PLUS";
        default: return "";
    }
}

std::string ExtractHash(StringW levelId) {
    std::string id = static_cast<std::string>(levelId);
    std::transform(id.begin(), id.end(), id.begin(),
            [](unsigned char c) { return std::tolower(c); });
    if (!id.starts_with(CUSTOM_LEVEL_PREFIX)) return "";
    return id.substr(CUSTOM_LEVEL_PREFIX.size());
}

void FinishAndSubmit(StandardLevelScenesTransitionSetupDataSO* transition,
        LevelCompletionResults* results) {
    if (!tracking || !results) return;
    tracking = false;

    if (transition->practiceSettings) {
        PaperLogger.info("No submit: practice mode");
        return;
    }
    std::string gameMode = static_cast<std::string>(transition->gameMode);
    if (gameMode != "Solo") {
        PaperLogger.info("No submit: game mode {} is not allowed", gameMode);
        return;
    }
    if (MetaCore::Game::IsScoreSubmissionDisabled()) {
        PaperLogger.info("No submit: score submission disabled by another mod");
        return;
    }
    if (totalNotes < MIN_TOTAL_NOTES || notes > totalNotes) {
        PaperLogger.info("No submit: note counts out of bounds ({}/{})", notes, totalNotes);
        return;
    }
    float completion = static_cast<float>(notes) / static_cast<float>(totalNotes);
    if (completion < COMPLETION_THRESHOLD) {
        PaperLogger.info("No submit: completion {:.1f}% below threshold", completion * 100.0f);
        return;
    }
    int multipliedScore = results->multipliedScore;
    if (multipliedScore <= 0) {
        PaperLogger.info("No submit: score was 0");
        return;
    }

    if (!transition->beatmapLevel || !transition->beatmapKey.beatmapCharacteristic ||
            !transition->gameplayModifiers) {
        PaperLogger.warn("No submit: transition data incomplete");
        return;
    }
    std::string hash = ExtractHash(transition->beatmapLevel->levelID);
    if (hash.empty()) {
        PaperLogger.info("No submit: not a custom level");
        return;
    }
    std::string difficulty = DifficultyName(transition->beatmapKey.difficulty.value__);
    if (difficulty.empty()) {
        PaperLogger.warn("No submit: unknown difficulty value");
        return;
    }
    std::string characteristic =
            static_cast<std::string>(transition->beatmapKey.beatmapCharacteristic->serializedName);

    bool failed = results->energy <= 0.0f && transition->gameplayModifiers->noFailOn0Energy;
    bool partial = results->levelEndAction != LevelCompletionResults_LevelEndAction::None ||
            results->levelEndStateType != LevelCompletionResults_LevelEndStateType::Cleared;

    streak115 = std::max(streak115, current115Streak);

    API::ScorePayload payload;
    payload.scoreNoMods = static_cast<uint32_t>(multipliedScore);
    payload.maxCombo = results->maxCombo;
    payload.badCuts = badCuts;
    payload.misses = misses;
    payload.wallHits = wallHits;
    payload.bombHits = bombHits;
    payload.pauses = pauses;
    payload.streak115 = streak115;
    payload.hmd = HeadsetName();
    payload.modifierCodes = ModifierCodes(transition->gameplayModifiers, failed);
    payload.partial = partial;

    API::ComputeModifierMultiplier(payload.modifierCodes,
            [payload = std::move(payload), hash = std::move(hash), difficulty = std::move(difficulty),
                    characteristic = std::move(characteristic),
                    multipliedScore](std::optional<double> multiplier) mutable {
                if (!multiplier.has_value()) {
                    PaperLogger.error("No submit: modifier multipliers unavailable");
                    return;
                }
                payload.score = static_cast<uint32_t>(
                        std::lround(static_cast<double>(multipliedScore) * *multiplier));
                API::ResolveMapDifficulty(hash, difficulty, characteristic,
                        [payload = std::move(payload)](std::optional<std::string> difficultyId) mutable {
                            if (!difficultyId.has_value()) {
                                PaperLogger.info("No submit: map difficulty not known to AccSaber");
                                return;
                            }
                            payload.mapDifficultyId = std::move(*difficultyId);
                            API::SubmitScore(std::move(payload), [](bool ok) {
                                if (!ok) PaperLogger.warn("Score submission did not succeed");
                            });
                        });
            });
}

}

MAKE_HOOK_MATCH(QLite_GameplayCoreInstall, &GameplayCoreInstaller::InstallBindings, void,
        GameplayCoreInstaller* self) {
    QLite_GameplayCoreInstall(self);

    ResetState();
    auto sceneSetupData = self->_sceneSetupData;
    if (!sceneSetupData || !sceneSetupData->transformedBeatmapData) return;

    totalNotes = sceneSetupData->transformedBeatmapData->get_cuttableNotesCount();
    tracking = true;
}

MAKE_HOOK_MATCH(QLite_ScoreControllerStart, &ScoreController::Start, void, ScoreController* self) {
    QLite_ScoreControllerStart(self);
    if (!tracking) return;

    auto scoreDelegate = custom_types::MakeDelegate<System::Action_1<ScoringElement*>*>(
            std::function<void(ScoringElement*)>(OnScoringElementFinished));
    self->add_scoringForNoteFinishedEvent(scoreDelegate);

    auto wallDelegate = custom_types::MakeDelegate<System::Action_1<UnityW<ObstacleController>>*>(
            std::function<void(UnityW<ObstacleController>)>(OnWallHit));
    self->_playerHeadAndObstacleInteraction->add_headDidEnterObstacleEvent(wallDelegate);
}

MAKE_HOOK_MATCH(QLite_BombCut, &BeatmapObjectManager::HandleNoteControllerNoteWasCut, void,
        BeatmapObjectManager* self, NoteController* noteController,
        ByRef<NoteCutInfo> noteCutInfo) {
    QLite_BombCut(self, noteController, noteCutInfo);

    if (!tracking || AtEndsOfMap()) return;
    if (!noteController->noteData ||
            noteController->noteData->gameplayType != NoteData_GameplayType::Bomb) {
        return;
    }
    bombHits++;
}

MAKE_HOOK_MATCH(QLite_Unpause, &PauseMenuManager::HandleResumeFromPauseAnimationDidFinish, void,
        PauseMenuManager* self) {
    QLite_Unpause(self);
    if (!tracking || AtEndsOfMap()) return;
    pauses++;
}

MAKE_HOOK_MATCH(QLite_LevelFinish, &StandardLevelScenesTransitionSetupDataSO::Finish, void,
        StandardLevelScenesTransitionSetupDataSO* self, LevelCompletionResults* levelCompletionResults) {
    QLite_LevelFinish(self, levelCompletionResults);
    FinishAndSubmit(self, levelCompletionResults);
}

void InstallHooks() {
    INSTALL_HOOK(PaperLogger, QLite_GameplayCoreInstall);
    INSTALL_HOOK(PaperLogger, QLite_ScoreControllerStart);
    INSTALL_HOOK(PaperLogger, QLite_BombCut);
    INSTALL_HOOK(PaperLogger, QLite_Unpause);
    INSTALL_HOOK(PaperLogger, QLite_LevelFinish);
}

}
