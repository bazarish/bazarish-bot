// Bazarish project (c) 2026
#include "MenuRules.hpp"

#include <bazarish/Bytes.hpp>

#include <cstdint>
#include <utility>

namespace bazarish::bot {

namespace {

constexpr std::size_t kMenuIdBytes = 4;
constexpr int kBitsPerByte = 8;

std::int64_t freshMenuId()
{
    std::int64_t id = 0;
    for (const std::uint8_t byte : randomBytes(kMenuIdBytes)) {
        id = (id << kBitsPerByte) | byte;
    }
    return id;
}

}  // namespace

bool owesMenu(const std::optional<client::BotMenu>& held, const client::IncomingMessage& message)
{
    return held && !message.sentByUs && client::carriesMenuId(message.contentType)
        && message.menuId != held->id;
}

std::optional<client::BotMenu> replacingMenu(
    const std::optional<client::BotMenu>& held, std::vector<client::BotMenuItem> items)
{
    if (items == (held ? held->items : std::vector<client::BotMenuItem>())) {
        return std::nullopt;
    }
    client::BotMenu menu{freshMenuId(), std::move(items)};
    client::requireBotMenu(menu);
    return menu;
}

}  // namespace bazarish::bot
