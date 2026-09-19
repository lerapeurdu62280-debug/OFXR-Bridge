#include "xrfg/game_profiles.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] bool contains(std::string_view text, std::string_view value) {
    return text.find(value) != std::string_view::npos;
}

} // namespace

int main() {
    using namespace xrfg::game_profiles;
    using xrfg::standalone::FlowBackend;
    using xrfg::standalone::NvidiaInputScale;
    using xrfg::standalone::NvidiaPerformancePreset;

    // Empty store round-trips to an empty (but well-formed) array.
    {
        const GameProfileStore empty;
        const std::string serialized = serialize_store(empty);
        if (!contains(serialized, "[") || !contains(serialized, "]")) {
            std::cerr << "empty store serialization failed\n";
            return 1;
        }
        if (!parse_store(serialized).profiles.empty()) {
            std::cerr << "empty store round-trip failed\n";
            return 1;
        }
    }

    // A single profile round-trips every field, including a path with a
    // space and an argument string that itself needs quoting.
    {
        GameProfileStore store;
        GameProfile profile;
        profile.name = "Forza Horizon 6";
        profile.game_executable =
            LR"(C:\Program Files\ForzaHorizon6\ForzaHorizon6.exe)";
        profile.mod_executable = LR"(C:\Mods\VRMod\VRMod.exe)";
        profile.mod_arguments = "--target \"ForzaHorizon6.exe\"";
        profile.settings.backend = FlowBackend::nvidia;
        profile.settings.nvidia_preset = NvidiaPerformancePreset::medium;
        profile.settings.nvidia_input_scale = NvidiaInputScale::half;
        profile.settings.nvidia_bidirectional = true;
        store.profiles.push_back(profile);

        const std::string serialized = serialize_store(store);
        const GameProfileStore parsed = parse_store(serialized);
        if (parsed.profiles.size() != 1) {
            std::cerr << "single profile count mismatch\n";
            return 1;
        }
        const GameProfile& round_tripped = parsed.profiles.front();
        if (round_tripped.name != profile.name ||
            round_tripped.game_executable != profile.game_executable ||
            round_tripped.mod_executable != profile.mod_executable ||
            round_tripped.mod_arguments != profile.mod_arguments ||
            round_tripped.settings.backend != profile.settings.backend ||
            round_tripped.settings.nvidia_preset !=
                profile.settings.nvidia_preset ||
            round_tripped.settings.nvidia_input_scale !=
                profile.settings.nvidia_input_scale ||
            round_tripped.settings.nvidia_bidirectional !=
                profile.settings.nvidia_bidirectional) {
            std::cerr << "single profile round-trip failed\n";
            return 1;
        }
    }

    // Several profiles preserve order and stay independent of each other.
    {
        GameProfileStore store;
        for (const std::string& name : {"Game A", "Game B", "Game C"}) {
            GameProfile profile;
            profile.name = name;
            profile.mod_executable = L"C:\\Mods\\mod.exe";
            store.profiles.push_back(profile);
        }
        const GameProfileStore parsed = parse_store(serialize_store(store));
        if (parsed.profiles.size() != 3 ||
            parsed.profiles[0].name != "Game A" ||
            parsed.profiles[1].name != "Game B" ||
            parsed.profiles[2].name != "Game C") {
            std::cerr << "multi-profile ordering failed\n";
            return 1;
        }
    }

    // Malformed input degrades to an empty store instead of throwing.
    {
        if (!parse_store("not json at all").profiles.empty() ||
            !parse_store("").profiles.empty() ||
            !parse_store("{\"not\": \"an array\"}").profiles.empty()) {
            std::cerr << "malformed input handling failed\n";
            return 1;
        }
    }

    // The launch command quotes the executable and appends raw arguments.
    {
        GameProfile profile;
        profile.mod_executable = L"C:\\My Mods\\VRMod.exe";
        profile.mod_arguments = "--fast";
        const std::wstring command = build_launch_command(profile);
        if (command != L"\"C:\\My Mods\\VRMod.exe\" --fast") {
            std::wcerr << L"unexpected launch command: " << command << L'\n';
            return 1;
        }

        GameProfile no_arguments;
        no_arguments.mod_executable = L"C:\\Mods\\VRMod.exe";
        if (build_launch_command(no_arguments) != L"C:\\Mods\\VRMod.exe") {
            std::cerr << "launch command without arguments failed\n";
            return 1;
        }
    }

    if (store_path(L"C:\\Users\\Test\\OFXR Bridge").filename() !=
        L"game_profiles.json") {
        std::cerr << "store path failed\n";
        return 1;
    }

    // auto_detect defaults to false and round-trips through the store.
    {
        GameProfile profile;
        profile.name = "Detected Game";
        profile.mod_executable = L"C:\\Mods\\mod.exe";
        if (profile.auto_detect) {
            std::cerr << "auto_detect default should be false\n";
            return 1;
        }
        profile.auto_detect = true;
        GameProfileStore store;
        store.profiles.push_back(profile);
        const GameProfileStore parsed = parse_store(serialize_store(store));
        if (parsed.profiles.size() != 1 || !parsed.profiles.front().auto_detect) {
            std::cerr << "auto_detect round-trip failed\n";
            return 1;
        }
        store.profiles.front().auto_detect = false;
        const GameProfileStore parsed_off =
            parse_store(serialize_store(store));
        if (parsed_off.profiles.front().auto_detect) {
            std::cerr << "auto_detect false round-trip failed\n";
            return 1;
        }
    }

    // Detection matches by executable file name only, case-insensitively,
    // and never matches when the profile has no game path configured.
    {
        GameProfile profile;
        profile.game_executable =
            LR"(C:\XboxGames\Forza Horizon 6\Content\ForzaHorizon6.exe)";
        const std::vector<std::wstring> running_mixed_case = {
            L"explorer.exe", L"FORZAHORIZON6.EXE", L"steam.exe"};
        if (!game_process_running(profile, running_mixed_case)) {
            std::cerr << "case-insensitive process match failed\n";
            return 1;
        }
        const std::vector<std::wstring> running_without_game = {
            L"explorer.exe", L"steam.exe"};
        if (game_process_running(profile, running_without_game)) {
            std::cerr << "process match false positive\n";
            return 1;
        }
        GameProfile no_path;
        if (game_process_running(no_path, running_mixed_case)) {
            std::cerr << "empty game path should never match\n";
            return 1;
        }
    }

    // running_executable_names() should at least see this test's own
    // process (or degrade to an empty list, never throw).
    {
        const auto running = running_executable_names();
        const bool sees_test_binary = std::any_of(
            running.begin(), running.end(), [](const std::wstring& name) {
                return name.find(L"xrfg_game_profiles_tests") !=
                    std::wstring::npos;
            });
        if (!running.empty() && !sees_test_binary) {
            std::cerr << "process snapshot did not include this process\n";
            return 1;
        }
    }

    std::cout << "OFXR game profile tests passed\n";
    return 0;
}
