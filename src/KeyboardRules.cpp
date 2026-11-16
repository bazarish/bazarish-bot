// Bazarish project (c) 2026
#include "KeyboardRules.hpp"

#include <bazarish/JsonRead.hpp>

#include <stdexcept>
#include <utility>
#include <vector>

namespace bazarish::bot {

client::InlineKeyboard keyboardFromJson(const nlohmann::json& rows)
{
    if (!rows.is_array()) {
        throw std::invalid_argument("a \"keyboard\" is an array of rows of buttons");
    }
    client::InlineKeyboard keyboard;
    for (const nlohmann::json& row : rows) {
        if (!row.is_array()) {
            throw std::invalid_argument("a keyboard row is an array of buttons");
        }
        std::vector<client::InlineButton> buttons;
        for (const nlohmann::json& button : row) {
            if (!button.is_object()) {
                throw std::invalid_argument(
                    "a button is a \"text\" with a \"data\" or a \"command\"");
            }
            buttons.push_back({button.value("text", std::string()),
                button.value("data", std::string()), button.value("command", std::string())});
        }
        keyboard.push_back(std::move(buttons));
    }
    return keyboard;
}

std::optional<client::InlineKeyboard> keyboardOfEdit(const nlohmann::json& body)
{
    if (!body.contains("keyboard")) {
        return std::nullopt;
    }
    return keyboardFromJson(body.at("keyboard"));
}

client::InlineKeyboard editedKeyboard(
    const std::optional<client::InlineKeyboard>& given, const std::string& heldJson)
{
    if (given.has_value()) {
        return *given;
    }
    if (heldJson.empty()) {
        return {};
    }
    return keyboardFromJson(bazarish::parseJson(heldJson));
}

std::string storedKeyboard(const client::InlineKeyboard& keyboard)
{
    return keyboard.empty() ? std::string() : client::inlineKeyboardJson(keyboard);
}

}  // namespace bazarish::bot
