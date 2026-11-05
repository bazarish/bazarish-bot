// Bazarish project (c) 2026
#pragma once

#include <string>
#include <vector>

namespace bazarish::bot {

enum class Copy {
    eAskName,
    eNameTaken,
    eNameRefused,
    eOfferedNameTaken,
    eOfferedNameRefused,
    eAwaitingApproval,
    eDeclined,
    eExcluded,
    eWantsToJoin,
    eAllowedBy,
    eDeclinedBy,
    eAllowed,
    eDeclinedCard,
    eAsksYou,
    eContactSent,
    eContactRefused,
    eAdminGranted,
    eAdminRevoked,
    ePostingClosed,
    eTooLarge,
    eButtonAllow,
    eButtonDecline,
    eButtonContactOn,
    eButtonContactOff,
    eSettings,
    eButtonRename,
    eAskNewName,
    eRenamed,
    eBlocked,
};

inline constexpr const char* kGroupLanguages[] = {"en", "ru", "es", "zh", "ar"};

std::string copyText(const std::string& language, Copy key, const std::vector<std::string>& values = {});
bool copyComplete();

}  // namespace bazarish::bot
