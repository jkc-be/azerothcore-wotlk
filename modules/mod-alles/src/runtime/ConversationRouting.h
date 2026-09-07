/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_CONVERSATION_ROUTING_H
#define MOD_ALLES_CONVERSATION_ROUTING_H
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace Alles
{
inline std::optional<std::string> AddressedBot(std::string const& text, std::vector<std::string> const& audience)
{
    auto lower = [](std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return value;
    };
    auto value = lower(text);
    auto start = value.find_first_not_of(" \t@");
    if (start == std::string::npos)
        return std::nullopt;
    value.erase(0, start);
    for (auto const& greeting : {"hey ", "hello ", "hi "})
        if (value.starts_with(greeting))
            value.erase(0, std::char_traits<char>::length(greeting));
    for (auto const& name : audience)
    {
        auto match = lower(name);
        if (value.starts_with(match) &&
            (value.size() == match.size() ||
             std::string_view(" ,:!?\t").find(value[match.size()]) != std::string_view::npos))
            return name;
    }
    return std::nullopt;
}

} // namespace Alles
#endif
