// Bazarish project (c) 2026
#pragma once

#include <string>
#include <vector>

namespace bazarish::bot {

enum class Copy {
    eAskName,
    eNameTaken,
    eNameRefused,
    eAwaitingApproval,
    eWelcome,
    eDeclined,
    eExcluded,
    eWantsToJoin,
    eAllowedBy,
    eDeclinedBy,
    eAllowed,
    eDeclinedCard,
    eAsksYou,
    eContactSent,
    eAdminGranted,
    eAdminRevoked,
    ePostingClosed,
    eTooLarge,
    eButtonAllow,
    eButtonDecline,
    eButtonContactOn,
    eButtonContactOff,
    eButtonSettings,
    eSettingsHint,
    eButtonRename,
    eAskNewName,
    eRenamed,
    eBlocked,
};

inline constexpr const char* kGroupLanguages[] = {"en", "ru", "es", "zh", "ar"};

std::string copyText(const std::string& language, Copy key, const std::vector<std::string>& values = {});
bool copyComplete();

}  // namespace bazarish::bot
