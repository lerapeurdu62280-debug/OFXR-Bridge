#include "xrfg/game_profiles.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace xrfg::game_profiles {
namespace {

using xrfg::standalone::FlowBackend;
using xrfg::standalone::NvidiaInputScale;
using xrfg::standalone::NvidiaPerformancePreset;

[[nodiscard]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), required, nullptr,
            nullptr) != required) {
        throw std::runtime_error("WideCharToMultiByte returned a short result");
    }
    return output;
}

[[nodiscard]] std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), required) !=
        required) {
        return {};
    }
    return output;
}

[[nodiscard]] std::string escape_json(std::string_view value) {
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() + 16);
    for (const unsigned char character : value) {
        switch (character) {
        case '\"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                output += "\\u00";
                output.push_back(hexadecimal[(character >> 4) & 0x0F]);
                output.push_back(hexadecimal[character & 0x0F]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return output;
}

// Unescapes a JSON string body (no surrounding quotes). Malformed escapes
// are passed through literally rather than rejected -- callers treat a
// corrupt store as empty, not as a hard error.
[[nodiscard]] std::string unescape_json(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            output.push_back(value[i]);
            continue;
        }
        const char next = value[++i];
        switch (next) {
        case '\"': output.push_back('\"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u':
            // \uXXXX (BMP only) -- narrow non-ASCII to '?' rather than
            // pulling in a full UTF-16 surrogate decoder for a local store
            // whose values are always round-tripped through this same code.
            if (i + 4 < value.size()) {
                i += 4;
                output.push_back('?');
            }
            break;
        default:
            output.push_back(next);
            break;
        }
    }
    return output;
}

[[nodiscard]] std::string path_to_json_string(const std::filesystem::path& path) {
    return escape_json(wide_to_utf8(path.native()));
}

[[nodiscard]] std::filesystem::path json_string_to_path(std::string_view value) {
    return std::filesystem::path(utf8_to_wide(unescape_json(value)));
}

// Extracts the raw text of the string value for `"key": "..."` starting the
// search at `offset`. Returns npos in `.second` when not found.
[[nodiscard]] std::pair<std::string, std::size_t> find_string_field(
    std::string_view object, std::string_view key, std::size_t offset) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_position = object.find(needle, offset);
    if (key_position == std::string_view::npos) {
        return {{}, std::string_view::npos};
    }
    std::size_t cursor = object.find(':', key_position + needle.size());
    if (cursor == std::string_view::npos) {
        return {{}, std::string_view::npos};
    }
    ++cursor;
    while (cursor < object.size() &&
           std::isspace(static_cast<unsigned char>(object[cursor])) != 0) {
        ++cursor;
    }
    if (cursor >= object.size() || object[cursor] != '\"') {
        return {{}, std::string_view::npos};
    }
    ++cursor;
    const std::size_t value_start = cursor;
    while (cursor < object.size()) {
        if (object[cursor] == '\\') {
            cursor += 2;
            continue;
        }
        if (object[cursor] == '\"') {
            break;
        }
        ++cursor;
    }
    if (cursor >= object.size()) {
        return {{}, std::string_view::npos};
    }
    return {std::string(object.substr(value_start, cursor - value_start)),
            key_position};
}

[[nodiscard]] std::string field(
    const std::string& name,
    const std::filesystem::path& value) {
    return "\"" + name + "\": \"" + path_to_json_string(value) + "\"";
}

[[nodiscard]] std::string field(
    const std::string& name, const std::string& value) {
    return "\"" + name + "\": \"" + escape_json(value) + "\"";
}

[[nodiscard]] std::string field(const std::string& name, bool value) {
    return "\"" + name + "\": " + (value ? "true" : "false");
}

[[nodiscard]] GameProfile parse_object(std::string_view object) {
    GameProfile profile;
    if (auto [value, found] = find_string_field(object, "name", 0);
        found != std::string_view::npos) {
        profile.name = unescape_json(value);
    }
    if (auto [value, found] = find_string_field(object, "game_executable", 0);
        found != std::string_view::npos) {
        profile.game_executable = json_string_to_path(value);
    }
    if (auto [value, found] = find_string_field(object, "mod_executable", 0);
        found != std::string_view::npos) {
        profile.mod_executable = json_string_to_path(value);
    }
    if (auto [value, found] = find_string_field(object, "mod_arguments", 0);
        found != std::string_view::npos) {
        profile.mod_arguments = unescape_json(value);
    }
    if (auto [value, found] = find_string_field(object, "backend", 0);
        found != std::string_view::npos) {
        profile.settings.backend = value == "nvidia"
            ? FlowBackend::nvidia
            : FlowBackend::fidelity_fx;
    }
    if (auto [value, found] = find_string_field(object, "nvidia_preset", 0);
        found != std::string_view::npos) {
        profile.settings.nvidia_preset = value == "slow"
            ? NvidiaPerformancePreset::slow
            : value == "fast" ? NvidiaPerformancePreset::fast
                               : NvidiaPerformancePreset::medium;
    }
    if (auto [value, found] =
            find_string_field(object, "nvidia_input_scale", 0);
        found != std::string_view::npos) {
        profile.settings.nvidia_input_scale = value == "75"
            ? NvidiaInputScale::three_quarter
            : value == "50" ? NvidiaInputScale::half : NvidiaInputScale::full;
    }
    const std::size_t bidirectional_key = object.find("\"nvidia_bidirectional\"");
    if (bidirectional_key != std::string_view::npos) {
        profile.settings.nvidia_bidirectional =
            object.find("true", bidirectional_key) != std::string_view::npos &&
            (object.find("true", bidirectional_key) <
             object.find_first_of(",}", bidirectional_key));
    }
    return profile;
}

} // namespace

GameProfileStore parse_store(std::string_view json) {
    GameProfileStore store;
    const std::size_t array_start = json.find('[');
    if (array_start == std::string_view::npos) {
        return store;
    }
    std::size_t depth = 0;
    std::size_t object_start = std::string_view::npos;
    for (std::size_t i = array_start; i < json.size(); ++i) {
        const char character = json[i];
        if (character == '{') {
            if (depth == 0) {
                object_start = i;
            }
            ++depth;
        } else if (character == '}') {
            if (depth == 0) {
                continue;
            }
            --depth;
            if (depth == 0 && object_start != std::string_view::npos) {
                store.profiles.push_back(
                    parse_object(json.substr(object_start, i - object_start + 1)));
                object_start = std::string_view::npos;
            }
        } else if (character == ']' && depth == 0) {
            break;
        }
    }
    return store;
}

std::string serialize_store(const GameProfileStore& store) {
    std::ostringstream output;
    output << "[\n";
    for (std::size_t i = 0; i < store.profiles.size(); ++i) {
        const GameProfile& profile = store.profiles[i];
        output << "  {\n"
               << "    " << field("name", profile.name) << ",\n"
               << "    " << field("game_executable", profile.game_executable)
               << ",\n"
               << "    " << field("mod_executable", profile.mod_executable)
               << ",\n"
               << "    " << field("mod_arguments", profile.mod_arguments)
               << ",\n"
               << "    "
               << field(
                      "backend",
                      xrfg::standalone::backend_ini_value(profile.settings.backend))
               << ",\n"
               << "    "
               << field(
                      "nvidia_preset",
                      xrfg::standalone::nvidia_preset_ini_value(
                          profile.settings.nvidia_preset))
               << ",\n"
               << "    "
               << field(
                      "nvidia_input_scale",
                      xrfg::standalone::nvidia_input_scale_ini_value(
                          profile.settings.nvidia_input_scale))
               << ",\n"
               << "    "
               << field("nvidia_bidirectional", profile.settings.nvidia_bidirectional)
               << "\n"
               << "  }" << (i + 1 < store.profiles.size() ? "," : "") << "\n";
    }
    output << "]\n";
    return output.str();
}

std::filesystem::path store_path(const std::filesystem::path& local_directory) {
    return local_directory / L"game_profiles.json";
}

std::string to_utf8(std::wstring_view value) {
    return wide_to_utf8(value);
}

std::wstring to_wide(std::string_view value) {
    return utf8_to_wide(value);
}

std::wstring build_launch_command(const GameProfile& profile) {
    std::wstring command =
        xrfg::standalone::quote_windows_argument(profile.mod_executable.wstring());
    if (!profile.mod_arguments.empty()) {
        command += L" ";
        command += utf8_to_wide(profile.mod_arguments);
    }
    return command;
}

} // namespace xrfg::game_profiles
