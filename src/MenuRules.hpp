// Bazarish project (c) 2026
#pragma once

#include <BotMenu.hpp>
#include <Session.hpp>

#include <optional>
#include <vector>

namespace bazarish::bot {

// A message from the peer that names no menu, or another one than the menu held for them.
bool owesMenu(const std::optional<client::BotMenu>& held, const client::IncomingMessage& message);

// The menu to send under a fresh id; none when the items are the ones already held.
std::optional<client::BotMenu> replacingMenu(
    const std::optional<client::BotMenu>& held, std::vector<client::BotMenuItem> items);

}  // namespace bazarish::bot
