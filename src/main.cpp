#include "main.hpp"

#include "qlite/accsaber_api.hpp"
#include "qlite/mod_config.hpp"
#include "qlite/score_tracker.hpp"

#include "GlobalNamespace/MainMenuViewController.hpp"

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

MAKE_HOOK_MATCH(MainMenuViewController_DidActivate,
        &GlobalNamespace::MainMenuViewController::DidActivate, void,
        GlobalNamespace::MainMenuViewController* self, bool firstActivation,
        bool addedToHierarchy, bool screenSystemEnabling) {
    MainMenuViewController_DidActivate(self, firstActivation, addedToHierarchy,
            screenSystemEnabling);
    AccSaberQLite::API::EnsureSessionFromMainThread();
}

MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
    *info = modInfo.to_c();

    Paper::Logger::RegisterFileContextId(PaperLogger.tag);
    getModConfig().Init(modInfo);

    PaperLogger.info("Completed setup!");
}

MOD_EXTERN_FUNC void late_load() noexcept {
    il2cpp_functions::Init();

    PaperLogger.info("Installing hooks...");
    INSTALL_HOOK(PaperLogger, MainMenuViewController_DidActivate);
    AccSaberQLite::Tracker::InstallHooks();
    PaperLogger.info("Installed all hooks!");
}
