// Bazarish project (c) 2026
#include "KeyboardRules.hpp"
#include "TestUtil.hpp"

#include <bazarish/JsonRead.hpp>

#include <cstdio>
#include <exception>
#include <optional>
#include <string>

using namespace bazarish;
using namespace bazarish::bot;

int main()
try {
    const client::InlineKeyboard held = keyboardFromJson(
        parseJson(R"([[{"text":"Ping","data":"ping"},{"text":"Help","command":"help"}]])"));
    CHECK(held.size() == 1 && held.front().size() == 2);
    CHECK(held.front().front().data == "ping");
    CHECK(held.front().back().command == "help");
    const std::string heldJson = storedKeyboard(held);
    CHECK(!heldJson.empty());

    CHECK_THROWS(keyboardFromJson(parseJson(R"("ping")")));
    CHECK_THROWS(keyboardFromJson(parseJson(R"([{"text":"Ping","data":"ping"}])")));
    CHECK_THROWS(keyboardFromJson(parseJson(R"([["Ping"]])")));
    CHECK_THROWS(keyboardOfEdit(parseJson(R"({"text":"t","keyboard":{}})")));

    const std::optional<client::InlineKeyboard> absent = keyboardOfEdit(parseJson(R"({"text":"t"})"));
    CHECK(!absent.has_value());
    const client::InlineKeyboard kept = editedKeyboard(absent, heldJson);
    CHECK(storedKeyboard(kept) == heldJson);
    CHECK(editedKeyboard(absent, {}).empty());

    const std::optional<client::InlineKeyboard> cleared
        = keyboardOfEdit(parseJson(R"({"text":"t","keyboard":[]})"));
    CHECK(cleared.has_value());
    CHECK(editedKeyboard(cleared, heldJson).empty());
    CHECK(storedKeyboard(editedKeyboard(cleared, heldJson)).empty());

    const client::InlineKeyboard replaced = editedKeyboard(
        keyboardOfEdit(parseJson(R"({"text":"t","keyboard":[[{"text":"Done","data":"done"}]]})")),
        heldJson);
    CHECK(replaced.size() == 1 && replaced.front().size() == 1);
    CHECK(replaced.front().front().data == "done");

    std::printf("TestKeyboardRules ok\n");
    return 0;
} catch (...) {
    return testutil::failed();
}
