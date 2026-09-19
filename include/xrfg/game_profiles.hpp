#pragma once
#include "xrfg/standalone_launcher.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xrfg::game_profiles {

// A user-defined shortcut: launch an external VR injector/mod for a game
// that does not natively expose OpenXR (e.g. a per-game mod such as the
// Forza Horizon 6 VRMod), then arm the bridge with settings tuned for it.
//
// This never performs injection itself -- it only starts a process the
// user already installed and points it at the values they supplied. The
// bridge has no knowledge of how that external tool works.
struct GameProfile {
    // Display name shown in the tray menu, e.g. "Forza Horizon 6".
    std::string name;
    // Path to the game's own executable. Currently informational/for the
    // user's own reference (matched loosely, never executed by us) -- no
    // process-detection loop is implemented yet.
    std::filesystem::path game_executable;
    // Path to the external VR mod/injector executable to launch.
    std::filesystem::path mod_executable;
    // Optional command-line arguments passed to the mod executable, as a
    // single pre-quoted string (the caller is responsible for quoting).
    std::string mod_arguments;
    // Bridge settings applied automatically before arming for this game.
    xrfg::standalone::LauncherSettings settings;
    // When set, the tray watches for game_executable's process name and
    // launches the mod + arms the bridge as soon as it appears, without
    // requiring a menu click. Off by default: a newly added profile should
    // not silently start behaving differently from a manual launch.
    bool auto_detect{};
};

struct GameProfileStore {
    std::vector<GameProfile> profiles;
};

// Minimal JSON reader for the flat, array-of-objects shape this store
// writes -- not a general-purpose JSON parser. Malformed or unrecognized
// input yields an empty store rather than throwing, matching how
// xrfg::standalone::parse_settings degrades on unexpected INI content.
[[nodiscard]] GameProfileStore parse_store(std::string_view json);

[[nodiscard]] std::string serialize_store(const GameProfileStore& store);

[[nodiscard]] std::filesystem::path store_path(
    const std::filesystem::path& local_directory);

// Builds the command line (module path + arguments) used to launch a
// profile's external mod executable via CreateProcessW. Exposed standalone
// so it can be unit-tested without spawning a real process.
[[nodiscard]] std::wstring build_launch_command(const GameProfile& profile);

// Converts a wide string to UTF-8, e.g. for deriving a default profile name
// from a filesystem path stem. A plain per-character narrowing cast would
// corrupt any non-ASCII character instead of encoding it.
[[nodiscard]] std::string to_utf8(std::wstring_view value);

// The inverse of to_utf8, e.g. for displaying a stored profile name (UTF-8)
// in a Win32 wide-character menu.
[[nodiscard]] std::wstring to_wide(std::string_view value);

// Snapshots the currently running processes' executable file names (e.g.
// "ForzaHorizon6.exe", case preserved as reported by Windows). Returns an
// empty list on failure (e.g. the snapshot API is unavailable) rather than
// throwing -- a detection poll that finds nothing this tick is expected
// behavior, not an error.
[[nodiscard]] std::vector<std::wstring> running_executable_names();

// True when `profile.game_executable`'s file name matches one of
// `running_names`, case-insensitively (Windows process names are not
// case-sensitive). An empty game_executable never matches, so a profile
// added without a game path is inert for detection rather than matching
// everything.
[[nodiscard]] bool game_process_running(
    const GameProfile& profile,
    const std::vector<std::wstring>& running_names);

} // namespace xrfg::game_profiles
