#include "xrfg/game_profiles.hpp"

#include <iostream>
#include <string>

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

    std::cout << "OFXR game profile tests passed\n";
    return 0;
}
