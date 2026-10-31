// Bazarish project (c) 2026
#pragma once

#include <BotMenu.hpp>
#include <Session.hpp>

#include <optional>
#include <vector>

namespace bazarish::bot {

bool owesMenu(const std::optional<client::BotMenu>& held, const client::IncomingMessage& message);

std::optional<client::BotMenu> replacingMenu(
    const std::optional<client::BotMenu>& held, std::vector<client::BotMenuItem> items);

}  // namespace bazarish::bot
