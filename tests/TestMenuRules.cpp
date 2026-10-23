// Bazarish project (c) 2026
#include "MenuRules.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

using namespace bazarish;
using namespace bazarish::bot;

namespace {

client::IncomingMessage from(const std::string& type, const std::optional<std::int64_t> menuId)
{
    client::IncomingMessage message;
    message.contentType = type;
    message.fromFingerprint = "bob";
    message.menuId = menuId;
    return message;
}

}  // namespace

int main()
{
    const std::vector<client::BotMenuItem> items = {{"Баланс", "balance"}, {"Help", "help"}};
    const std::optional<client::BotMenu> first = replacingMenu(std::nullopt, items);
    CHECK(first && first->items == items);
    CHECK(first->id >= 0 && first->id <= client::kMaxBotMenuId);
    CHECK(!replacingMenu(first, items));
    CHECK(!replacingMenu(std::nullopt, {}));
    const std::optional<client::BotMenu> gone = replacingMenu(first, {});
    CHECK(gone && gone->items.empty());
    const std::optional<client::BotMenu> other = replacingMenu(first, {{"Help", "help"}});
    CHECK(other && other->items.size() == 1);

    bool refused = false;
    try {
        (void)replacingMenu(first, {{"Help", "/help"}});
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    CHECK(owesMenu(first, from("text", std::nullopt)));
    CHECK(owesMenu(first, from("bot.command", first->id + 1)));
    CHECK(!owesMenu(first, from("text", first->id)));
    CHECK(!owesMenu(first, from("bot.callback", first->id)));
    CHECK(!owesMenu(std::nullopt, from("text", std::nullopt)));
    CHECK(!owesMenu(first, from("receipt", std::nullopt)));
    CHECK(!owesMenu(first, from("contact.request", std::nullopt)));
    client::IncomingMessage ours = from("text", std::nullopt);
    ours.sentByUs = true;
    CHECK(!owesMenu(first, ours));
    CHECK(owesMenu(gone, from("text", first->id)));

    std::printf("TestMenuRules: all checks passed\n");
    return 0;
}
