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
    eLeft,
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
    eButtonLeave,
    eButtonContactOn,
    eButtonContactOff,
    eButtonSettings,
    eSettingsHint,
    eBlocked,
};

inline constexpr const char* kGroupLanguages[] = {"en", "ru", "es", "zh", "ar"};

std::string copyText(const std::string& language, Copy key, const std::vector<std::string>& values = {});
bool copyComplete();

}  // namespace bazarish::bot
