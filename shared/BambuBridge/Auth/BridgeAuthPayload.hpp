#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r::BambuBridge {

inline bool looks_like_change_user_data(const nlohmann::json& j)
{
    if (!j.is_object())
        return false;
    return j.contains("token") ||
           j.contains("access_token") ||
           j.contains("refresh_token") ||
           j.contains("code") ||
           j.contains("user") ||
           j.contains("user_id") ||
           j.contains("uidStr");
}

inline nlohmann::json normalize_change_user_payload(const nlohmann::json& input)
{
    if (!input.is_object())
        return input;

    const auto command = input.value("command", std::string());
    if ((command == "user_login" || command == "login_token") && input.contains("data") && input["data"].is_object())
        return input;

    auto data_it = input.find("data");
    if (data_it != input.end() && looks_like_change_user_data(*data_it))
        return {
            {"command", command.empty() ? "user_login" : command},
            {"data", *data_it},
        };

    if (looks_like_change_user_data(input))
        return {
            {"command", "user_login"},
            {"data", input},
        };

    return input;
}

inline std::string normalize_change_user_payload_string(const std::string& input)
{
    try {
        const auto parsed = nlohmann::json::parse(input);
        return normalize_change_user_payload(parsed).dump();
    } catch (...) {
        return input;
    }
}

}
