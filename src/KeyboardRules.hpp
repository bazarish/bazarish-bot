// Bazarish project (c) 2026
#pragma once

#include <Session.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace bazarish::bot {

client::InlineKeyboard keyboardFromJson(const nlohmann::json& rows);

std::optional<client::InlineKeyboard> keyboardOfEdit(const nlohmann::json& body);

client::InlineKeyboard editedKeyboard(
    const std::optional<client::InlineKeyboard>& given, const std::string& heldJson);

std::string storedKeyboard(const client::InlineKeyboard& keyboard);

}  // namespace bazarish::bot
