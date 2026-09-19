#include "xrfg/game_profiles.hpp"
#include "xrfg/implicit_layer.hpp"
#include "xrfg/standalone_launcher.hpp"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(linker,                                                     \
    "\"/manifestdependency:type='win32' "                                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "           \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "          \
    "language='*'\"")

namespace {

constexpr wchar_t kWindowClass[] = L"OFXRBridgeTrayWindow";
constexpr wchar_t kApplicationName[] = L"OFXR Bridge";
constexpr wchar_t kCleanupArgument[] = L"--cleanup-manual-arm";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTrayId = 1;
constexpr UINT kArmPollMilliseconds = 250;
constexpr UINT_PTR kGameDetectionTimerId = 1;
constexpr UINT kGameDetectionPollMilliseconds = 3000;
constexpr std::uint32_t kImplementationVersion = XRFG_IMPLEMENTATION_VERSION;
constexpr wchar_t kDonateUrl[] = L"https://ko-fi.com/tig3rmast3r";

enum MenuCommand : UINT {
    toggle_arm = 90,
    backend_fidelity_fx = 110,
    backend_nvidia_slow = 111,
    backend_nvidia_medium = 112,
    backend_nvidia_fast = 113,
    toggle_nvidia_bidirectional = 114,
    nvidia_scale_full = 115,
    nvidia_scale_three_quarter = 116,
    nvidia_scale_half = 117,
    toggle_diagnostics = 120,
    overlay_off = 121,
    overlay_upper_left = 122,
    overlay_upper_right = 123,
    overlay_lower_left = 124,
    overlay_lower_right = 125,
    open_logs = 130,
    donate = 139,
    show_about = 140,
    exit_application = 150,
    add_game_profile = 160,
    manage_game_profiles = 161,
    // Reserved range for one dynamic entry per stored game profile
    // (game_profile_launch_base + index). Kept well clear of every other
    // fixed command ID above and below.
    game_profile_launch_base = 1000,
    game_profile_launch_max = 1999,
};

struct AppState {
    HWND window{};
    NOTIFYICONDATAW icon{};
    xrfg::standalone::LauncherSettings settings;
    std::filesystem::path executable_directory;
    std::filesystem::path local_directory;
    std::filesystem::path settings_path;
    std::filesystem::path game_profiles_path;
    xrfg::game_profiles::GameProfileStore game_profiles;
    // Name of the auto-detect profile currently believed to be running (its
    // mod launched and the bridge armed for it), or empty when none is
    // active. Only one auto-launch is tracked at a time -- concurrent VR
    // games are not a scenario this targets -- so a detected game process
    // is not relaunched every poll, and closing it triggers disarm.
    std::string auto_detected_profile_name;
    std::filesystem::path armed_manifest;
    xrfg::implicit_layer::RegistryScope armed_scope{
        xrfg::implicit_layer::RegistryScope::current_user};
    HANDLE arm_signal{};
    // Alternate namespace used by lifecycle tests, never read from user INI.
    std::wstring registry_subkey{xrfg::implicit_layer::kRegistrySubkey};
    HICON armed_icon{};
    HICON disarmed_icon{};
    UINT taskbar_created_message{};
    bool armed{};
};

[[nodiscard]] std::wstring last_error_message(std::wstring_view action) {
    const DWORD code = GetLastError();
    wchar_t* system_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr);
    std::wstring output(action);
    output += L" failed (" + std::to_wstring(code) + L")";
    if (length > 0 && system_message != nullptr) {
        output += L": ";
        output.append(system_message, length);
        LocalFree(system_message);
    }
    return output;
}

[[nodiscard]] std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    return std::filesystem::path(path.data()).parent_path();
}

[[nodiscard]] std::filesystem::path local_app_data() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value))) {
        throw std::runtime_error("SHGetKnownFolderPath failed");
    }
    const std::filesystem::path output(value);
    CoTaskMemFree(value);
    return output;
}

[[nodiscard]] std::filesystem::path runtime_directory(
    const std::filesystem::path& local_directory) {
    return xrfg::standalone::runtime_version_directory(
        local_directory,
        kImplementationVersion);
}

[[nodiscard]] bool write_text_atomic(
    const std::filesystem::path& path,
    std::string_view text,
    std::wstring* error) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream) {
                if (error) *error = L"Unable to write " + temporary.wstring();
                return false;
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (error) *error = last_error_message(L"Saving " + path.wstring());
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Unable to save " + path.wstring();
        return false;
    }
}

void load_settings(AppState& state) {
    std::ifstream stream(state.settings_path, std::ios::binary);
    if (!stream) {
        return;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    state.settings = xrfg::standalone::parse_settings(text.str());
}

void save_settings(const AppState& state) {
    std::wstring ignored;
    static_cast<void>(write_text_atomic(
        state.settings_path,
        xrfg::standalone::serialize_settings(state.settings),
        &ignored));
}

void load_game_profiles(AppState& state) {
    std::ifstream stream(state.game_profiles_path, std::ios::binary);
    if (!stream) {
        return;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    state.game_profiles = xrfg::game_profiles::parse_store(text.str());
}

void save_game_profiles(const AppState& state) {
    std::wstring ignored;
    static_cast<void>(write_text_atomic(
        state.game_profiles_path,
        xrfg::game_profiles::serialize_store(state.game_profiles),
        &ignored));
}

// Prompts for two file paths (the game and its external VR mod) with the
// standard Open dialog, and appends a new profile if both are chosen and a
// name is available. Returns false without any error dialog if the user
// simply cancels.
[[nodiscard]] bool prompt_new_game_profile(AppState& state, std::wstring* error) {
    wchar_t game_path[MAX_PATH]{};
    OPENFILENAMEW game_dialog{};
    game_dialog.lStructSize = sizeof(game_dialog);
    game_dialog.hwndOwner = state.window;
    game_dialog.lpstrFilter = L"Executable\0*.exe\0All files\0*.*\0";
    game_dialog.lpstrFile = game_path;
    game_dialog.nMaxFile = static_cast<DWORD>(std::size(game_path));
    game_dialog.lpstrTitle = L"Select the game executable";
    game_dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&game_dialog)) {
        return false;
    }

    wchar_t mod_path[MAX_PATH]{};
    OPENFILENAMEW mod_dialog{};
    mod_dialog.lStructSize = sizeof(mod_dialog);
    mod_dialog.hwndOwner = state.window;
    mod_dialog.lpstrFilter = L"Executable\0*.exe\0All files\0*.*\0";
    mod_dialog.lpstrFile = mod_path;
    mod_dialog.nMaxFile = static_cast<DWORD>(std::size(mod_path));
    mod_dialog.lpstrTitle = L"Select the external VR mod/injector to launch";
    mod_dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&mod_dialog)) {
        return false;
    }

    xrfg::game_profiles::GameProfile profile;
    profile.game_executable = game_path;
    profile.mod_executable = mod_path;
    profile.name = profile.game_executable.stem().wstring().empty()
        ? "Game"
        : xrfg::game_profiles::to_utf8(profile.game_executable.stem().wstring());
    profile.settings = state.settings;
    profile.auto_detect = MessageBoxW(
        state.window,
        L"Automatically launch the mod and arm the bridge whenever this "
        L"game's process is detected running, instead of only from this "
        L"menu?\r\n\r\nYou can change this later by editing the profiles "
        L"file (\"Open profiles file\").",
        L"Auto-detect this game?",
        MB_YESNO | MB_ICONQUESTION) == IDYES;
    state.game_profiles.profiles.push_back(profile);
    save_game_profiles(state);
    static_cast<void>(error);
    return true;
}

// Launches a profile's external mod, then applies and arms the bridge with
// the settings saved for it. This never touches the game process itself --
// it only starts the tool the user configured and waits for it to expose
// its own OpenXR session, exactly as it would if launched by hand.
[[nodiscard]] bool launch_game_profile(
    AppState& state,
    const xrfg::game_profiles::GameProfile& profile,
    std::wstring* error) {
    if (!std::filesystem::is_regular_file(profile.mod_executable)) {
        if (error) {
            *error = L"The configured mod executable is missing:\r\n" +
                     profile.mod_executable.wstring();
        }
        return false;
    }
    std::wstring command = xrfg::game_profiles::build_launch_command(profile);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(
        profile.mod_executable.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        profile.mod_executable.parent_path().c_str(),
        &startup,
        &process);
    if (!started) {
        if (error) *error = last_error_message(L"Starting the external VR mod");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    state.settings = profile.settings;
    save_settings(state);
    return true;
}

void show_error(HWND owner, const std::wstring& error) {
    MessageBoxW(owner, error.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
}

void remove_manifest_file(const std::filesystem::path& manifest) {
    std::error_code ignored;
    std::filesystem::remove(manifest, ignored);
}

void log_lifecycle(const std::filesystem::path& local_directory,
                   std::wstring_view action, std::wstring_view error = {}) noexcept {
    try {
        std::filesystem::create_directories(local_directory);
        const auto path = local_directory / L"tray-lifecycle.log";
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        std::wofstream stream(path, !size_error && size > 262144
            ? std::ios::trunc : std::ios::app);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        stream << time.wYear << L'-' << time.wMonth << L'-' << time.wDay << L' '
               << time.wHour << L':' << time.wMinute << L':' << time.wSecond
               << L" pid=" << GetCurrentProcessId() << L" V" << kImplementationVersion
               << L' ' << action << (error.empty() ? L" OK" : L" FAILED: ") << error << L'\n';
    } catch (...) { /* Cleanup must not depend on diagnostic I/O. */ }
}

[[nodiscard]] bool write_runtime_configuration(
    const AppState& state,
    std::wstring* error,
    const std::filesystem::path& arm_manifest = {}) {
    const auto& manifest = arm_manifest.empty() ? state.armed_manifest : arm_manifest;
    std::string configuration = xrfg::standalone::build_runtime_ini(state.settings);
    if (!manifest.empty()) {
        const auto control = xrfg::implicit_layer::arm_signal_name(manifest);
        std::string ascii_control;
        for (const wchar_t c : control) ascii_control.push_back(static_cast<char>(c));
        configuration.insert(std::string("[ofxr]\r\n").size(),
            "control_event=" + ascii_control + "\r\n");
    }
    return write_text_atomic(
        runtime_directory(state.local_directory) / L"ofxr_bridge.ini",
        configuration,
        error);
}

[[nodiscard]] bool prepare_runtime_layer(
    AppState& state,
    std::filesystem::path* manifest,
    std::wstring* error) {
    try {
        const auto source_dll = state.executable_directory / L"ofxr" /
            L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        if (!std::filesystem::is_regular_file(source_dll)) {
            if (error) {
                *error = L"The bundled OFXR layer is missing:\r\n" +
                         source_dll.wstring();
            }
            return false;
        }
        const auto directory = runtime_directory(state.local_directory);
        std::filesystem::create_directories(directory);
        const auto runtime_dll = directory /
            L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        if (!xrfg::standalone::install_runtime_layer_dll(
                source_dll, runtime_dll)) {
            if (error) *error = last_error_message(L"Installing the runtime layer");
            return false;
        }

        static std::uint64_t last_arm_id = 0;
        last_arm_id = std::max<std::uint64_t>(last_arm_id + 1, GetTickCount64());
        const std::wstring manifest_name =
            std::wstring(xrfg::implicit_layer::kManifestPrefix) +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(last_arm_id) +
            xrfg::implicit_layer::kManifestSuffix;
        const auto generated_manifest = directory / manifest_name;
        *manifest = generated_manifest;
        state.arm_signal = xrfg::implicit_layer::create_arm_signal(generated_manifest, error);
        if (!state.arm_signal) return false;
        if (!write_text_atomic(
                generated_manifest,
                xrfg::standalone::build_implicit_layer_manifest(
                    runtime_dll, kImplementationVersion),
                error) ||
            !write_runtime_configuration(state, error, generated_manifest)) {
            remove_manifest_file(generated_manifest);
            return false;
        }
        *manifest = generated_manifest;
        return true;
    } catch (...) {
        if (error) *error = L"Unable to prepare the manual OpenXR layer.";
        return false;
    }
}

[[nodiscard]] bool spawn_cleanup_helper(
    const AppState& state,
    const std::filesystem::path& manifest,
    xrfg::implicit_layer::RegistryScope scope,
    std::wstring* error) {
    std::wstring command = xrfg::standalone::quote_windows_argument(
        (state.executable_directory / L"OFXRBridgeTray.exe").wstring());
    command += L" ";
    command += kCleanupArgument;
    command += L" ";
    command += xrfg::standalone::quote_windows_argument(manifest.wstring());
    command += L" ";
    command += std::to_wstring(GetCurrentProcessId());
    command += L" ";
    command += xrfg::implicit_layer::registry_scope_name(scope);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto tray_executable =
        state.executable_directory / L"OFXRBridgeTray.exe";
    if (!CreateProcessW(
            tray_executable.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            state.executable_directory.c_str(),
            &startup,
            &process)) {
        if (error) *error = last_error_message(L"Starting the cleanup watchdog");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

[[nodiscard]] bool cleanup_all_owned_registrations(
    const AppState& state,
    std::wstring* error) {
    if (error) error->clear();
    bool success = true;
    for (const auto scope : {
             xrfg::implicit_layer::RegistryScope::current_user,
             xrfg::implicit_layer::RegistryScope::local_machine}) {
        std::wstring detail;
        if (!xrfg::implicit_layer::cleanup_owned_registrations(
                state.local_directory, scope, &detail, state.registry_subkey)) {
            success = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += xrfg::implicit_layer::registry_scope_name(scope);
                *error += L": ";
                *error += detail;
            }
        }
    }
    return success;
}

[[nodiscard]] std::wstring tray_tooltip(const AppState& state) {
    std::wstring tooltip = state.armed ? L"OFXR Bridge ARMED - " : L"OFXR Bridge - ";
    if (state.settings.backend == xrfg::standalone::FlowBackend::nvidia) {
        switch (state.settings.nvidia_preset) {
        case xrfg::standalone::NvidiaPerformancePreset::fast:
            tooltip += L"NVIDIA Fast (test)";
            break;
        case xrfg::standalone::NvidiaPerformancePreset::slow:
            tooltip += L"NVIDIA Slow";
            break;
        case xrfg::standalone::NvidiaPerformancePreset::medium:
        default:
            tooltip += L"NVIDIA Medium";
            break;
        }
        tooltip += state.settings.nvidia_bidirectional
            ? L" + bidirectional"
            : L" + forward";
        switch (state.settings.nvidia_input_scale) {
        case xrfg::standalone::NvidiaInputScale::three_quarter:
            tooltip += L" @ 75%";
            break;
        case xrfg::standalone::NvidiaInputScale::half:
            tooltip += L" @ 50%";
            break;
        case xrfg::standalone::NvidiaInputScale::full:
        default:
            tooltip += L" @ 100%";
            break;
        }
    } else {
        tooltip += L"FidelityFX";
    }
    if (state.settings.diagnostics) {
        tooltip += L" - recorder on";
    }
    return tooltip;
}

void refresh_tray_icon(AppState& state) {
    const std::wstring tooltip = tray_tooltip(state);
    wcsncpy_s(state.icon.szTip, tooltip.c_str(), _TRUNCATE);
    state.icon.hIcon = state.armed ? state.armed_icon : state.disarmed_icon;
    state.icon.uFlags = NIF_ICON | NIF_TIP;
    static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &state.icon));
}

void show_balloon(
    AppState& state,
    std::wstring_view title,
    std::wstring_view message,
    DWORD flags = NIIF_INFO) {
    wcsncpy_s(state.icon.szInfoTitle, std::wstring(title).c_str(), _TRUNCATE);
    wcsncpy_s(state.icon.szInfo, std::wstring(message).c_str(), _TRUNCATE);
    state.icon.dwInfoFlags = flags;
    state.icon.uFlags = NIF_INFO;
    static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &state.icon));
}

[[nodiscard]] bool add_tray_icon(AppState& state) {
    state.icon = {};
    state.icon.cbSize = sizeof(state.icon);
    state.icon.hWnd = state.window;
    state.icon.uID = kTrayId;
    state.icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    state.icon.uCallbackMessage = kTrayMessage;
    state.icon.hIcon = state.armed ? state.armed_icon : state.disarmed_icon;
    const std::wstring tooltip = tray_tooltip(state);
    wcsncpy_s(state.icon.szTip, tooltip.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(NIM_ADD, &state.icon) != FALSE;
}

[[nodiscard]] bool disarm_bridge(
    AppState& state,
    std::wstring* error) {
    // Signal the active DLL BEFORE registry/file cleanup or window teardown.
    std::wstring signal_error;
    if (state.arm_signal && !SetEvent(state.arm_signal))
        signal_error = last_error_message(L"Stopping the loaded OFXR layer");
    std::wstring detail;
    const bool cleaned = cleanup_all_owned_registrations(state, &detail);
    if (!signal_error.empty()) {
        if (!detail.empty()) detail += L"\r\n";
        detail += signal_error;
    }
    if (!cleaned || !signal_error.empty()) {
        log_lifecycle(state.local_directory, L"disarm-all", detail);
        if (error) *error = detail;
        return false;
    }
    log_lifecycle(state.local_directory, L"disarm-all");
    if (state.arm_signal) {
        CloseHandle(state.arm_signal);
        state.arm_signal = nullptr;
    }
    state.armed = false;
    state.armed_manifest.clear();
    state.armed_scope = xrfg::implicit_layer::RegistryScope::current_user;
    refresh_tray_icon(state);
    return true;
}

[[nodiscard]] bool arm_bridge(AppState& state, std::wstring* error) {
    // Reconcile disk/registry state, not just this tray process's memory.
    if (!disarm_bridge(state, error)) return false;

    std::filesystem::path manifest;
    const auto scope = xrfg::implicit_layer::preferred_registry_scope();
    if (!prepare_runtime_layer(state, &manifest, error) ||
        !xrfg::implicit_layer::register_manifest(
            manifest, scope, error, state.registry_subkey)) {
        if (!manifest.empty()) {
            std::wstring cleanup_error;
            if (!xrfg::implicit_layer::retire_manifest(
                    manifest, scope, &cleanup_error, state.registry_subkey))
                log_lifecycle(state.local_directory, L"arm-failure-cleanup", cleanup_error);
        }
        if (state.arm_signal) {
            SetEvent(state.arm_signal);
            CloseHandle(state.arm_signal);
            state.arm_signal = nullptr;
        }
        return false;
    }
    if (!spawn_cleanup_helper(state, manifest, scope, error)) {
        std::wstring ignored;
        static_cast<void>(xrfg::implicit_layer::retire_manifest(
            manifest, scope, &ignored, state.registry_subkey));
        if (state.arm_signal) {
            SetEvent(state.arm_signal);
            CloseHandle(state.arm_signal);
            state.arm_signal = nullptr;
        }
        return false;
    }
    state.armed = true;
    state.armed_manifest = manifest;
    state.armed_scope = scope;
    log_lifecycle(state.local_directory,
        scope == xrfg::implicit_layer::RegistryScope::local_machine
            ? L"arm-hklm"
            : L"arm-hkcu");
    refresh_tray_icon(state);
    show_balloon(
        state,
        L"OFXR Bridge armed",
        L"The bridge will remain enabled for OpenXR applications until you disarm it or exit the tray.");
    return true;
}

void update_runtime_options(AppState& state, bool overlay_change = false) {
    save_settings(state);
    if (state.armed) {
        std::wstring error;
        if (!write_runtime_configuration(state, &error)) {
            show_error(state.window, error);
        }
    }
    refresh_tray_icon(state);
    if (state.armed) {
        show_balloon(
            state,
            L"OFXR options saved",
            overlay_change ? L"The FPS overlay position updates in running applications."
                : L"The new optical-flow settings will be used by the next OpenXR session.");
    }
}

// Polled every kGameDetectionPollMilliseconds via WM_TIMER. Starts the mod
// and arms the bridge for the first auto_detect profile whose game process
// just appeared, and disarms when the previously detected game's process
// disappears. Never touches a profile the user launched by hand from the
// menu -- auto_detected_profile_name only ever holds a name this function
// itself set.
void poll_game_detection(AppState& state) {
    const auto running = xrfg::game_profiles::running_executable_names();

    if (!state.auto_detected_profile_name.empty()) {
        const auto active = std::find_if(
            state.game_profiles.profiles.begin(),
            state.game_profiles.profiles.end(),
            [&](const xrfg::game_profiles::GameProfile& profile) {
                return profile.name == state.auto_detected_profile_name;
            });
        const bool still_running = active !=
            state.game_profiles.profiles.end() &&
            xrfg::game_profiles::game_process_running(*active, running);
        if (!still_running) {
            std::wstring error;
            if (state.armed && !disarm_bridge(state, &error)) {
                log_lifecycle(state.local_directory, L"auto-detect-disarm", error);
            }
            state.auto_detected_profile_name.clear();
        }
        return; // One tracked game at a time; wait for it to close first.
    }

    for (const auto& profile : state.game_profiles.profiles) {
        if (!profile.auto_detect) {
            continue;
        }
        if (!xrfg::game_profiles::game_process_running(profile, running)) {
            continue;
        }
        std::wstring error;
        if (!launch_game_profile(state, profile, &error)) {
            log_lifecycle(state.local_directory, L"auto-detect-launch", error);
            continue; // Try again on the next poll rather than looping now.
        }
        if (!arm_bridge(state, &error)) {
            log_lifecycle(state.local_directory, L"auto-detect-arm", error);
        }
        state.auto_detected_profile_name = profile.name;
        break; // Only one auto-launch tracked at a time.
    }
}

void show_context_menu(AppState& state) {
    HMENU menu = CreatePopupMenu();
    HMENU backend_menu = CreatePopupMenu();
    HMENU nvidia_scale_menu = CreatePopupMenu();
    if (menu == nullptr || backend_menu == nullptr ||
        nvidia_scale_menu == nullptr) {
        if (nvidia_scale_menu) DestroyMenu(nvidia_scale_menu);
        if (backend_menu) DestroyMenu(backend_menu);
        if (menu) DestroyMenu(menu);
        return;
    }

    AppendMenuW(
        menu,
        MF_STRING | (state.armed ? MF_CHECKED : MF_UNCHECKED),
        toggle_arm,
        state.armed
            ? L"Disarm bridge"
            : L"Arm bridge until manual disarm");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.backend == xrfg::standalone::FlowBackend::fidelity_fx
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        backend_fidelity_fx,
        L"FidelityFX");
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.backend == xrfg::standalone::FlowBackend::nvidia &&
                     state.settings.nvidia_preset ==
                         xrfg::standalone::NvidiaPerformancePreset::fast
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        backend_nvidia_fast,
        L"NVIDIA Fast (test)");
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.backend == xrfg::standalone::FlowBackend::nvidia &&
                     state.settings.nvidia_preset ==
                         xrfg::standalone::NvidiaPerformancePreset::medium
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        backend_nvidia_medium,
        L"NVIDIA Medium");
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.backend == xrfg::standalone::FlowBackend::nvidia &&
                     state.settings.nvidia_preset ==
                         xrfg::standalone::NvidiaPerformancePreset::slow
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        backend_nvidia_slow,
        L"NVIDIA Slow (best quality)");
    AppendMenuW(backend_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.nvidia_bidirectional ? MF_CHECKED : MF_UNCHECKED),
        toggle_nvidia_bidirectional,
        L"NVIDIA bidirectional consistency");
    AppendMenuW(
        menu,
        MF_POPUP,
        reinterpret_cast<UINT_PTR>(backend_menu),
        L"Optical flow backend");
    AppendMenuW(
        nvidia_scale_menu,
        MF_STRING |
            (state.settings.nvidia_input_scale ==
                     xrfg::standalone::NvidiaInputScale::full
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        nvidia_scale_full,
        L"100% (full resolution)");
    AppendMenuW(
        nvidia_scale_menu,
        MF_STRING |
            (state.settings.nvidia_input_scale ==
                     xrfg::standalone::NvidiaInputScale::three_quarter
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        nvidia_scale_three_quarter,
        L"75%");
    AppendMenuW(
        nvidia_scale_menu,
        MF_STRING |
            (state.settings.nvidia_input_scale ==
                     xrfg::standalone::NvidiaInputScale::half
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        nvidia_scale_half,
        L"50%");
    AppendMenuW(
        menu,
        MF_POPUP,
        reinterpret_cast<UINT_PTR>(nvidia_scale_menu),
        L"NVIDIA OFA resolution");
    AppendMenuW(
        menu,
        MF_STRING | (state.settings.diagnostics ? MF_CHECKED : MF_UNCHECKED),
        toggle_diagnostics,
        L"Bridge flight recorder");
    HMENU overlay_menu = CreatePopupMenu();
    if (overlay_menu) {
        const struct { UINT command; xrfg::FpsOverlayPosition position; const wchar_t* text; } entries[]{
            {overlay_upper_left, xrfg::FpsOverlayPosition::upper_left, L"Upper left"},
            {overlay_upper_right, xrfg::FpsOverlayPosition::upper_right, L"Upper right"},
            {overlay_lower_left, xrfg::FpsOverlayPosition::lower_left, L"Lower left"},
            {overlay_lower_right, xrfg::FpsOverlayPosition::lower_right, L"Lower right"},
            {overlay_off, xrfg::FpsOverlayPosition::off, L"Off"}};
        for (const auto& entry : entries) {
            if (entry.position == xrfg::FpsOverlayPosition::off) AppendMenuW(overlay_menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(overlay_menu, MF_STRING |
                (state.settings.overlay_position == entry.position ? MF_CHECKED : MF_UNCHECKED),
                entry.command, entry.text);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(overlay_menu), L"FPS overlay");
    }
    AppendMenuW(menu, MF_STRING, open_logs, L"Open bridge logs");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU games_menu = CreatePopupMenu();
    if (games_menu) {
        UINT command_id = game_profile_launch_base;
        for (const auto& profile : state.game_profiles.profiles) {
            if (command_id > game_profile_launch_max) {
                break; // Reserved range exhausted; extra profiles are unreachable from here.
            }
            std::wstring label = profile.name.empty()
                ? L"(unnamed game)"
                : xrfg::game_profiles::to_wide(profile.name);
            if (profile.auto_detect) {
                label += L" (auto-detect)";
            }
            AppendMenuW(games_menu, MF_STRING, command_id, label.c_str());
            ++command_id;
        }
        if (!state.game_profiles.profiles.empty()) {
            AppendMenuW(games_menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(games_menu, MF_STRING, add_game_profile, L"Add game...");
        AppendMenuW(games_menu, MF_STRING, manage_game_profiles, L"Open profiles file");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(games_menu),
            L"Games (launch VR mod)");
    }
    AppendMenuW(menu, MF_STRING, donate, L"Donate");
    AppendMenuW(menu, MF_STRING, show_about, L"About");
    AppendMenuW(menu, MF_STRING, exit_application, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(state.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, state.window, nullptr);
    PostMessageW(state.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void handle_command(AppState& state, UINT command) {
    switch (command) {
    case toggle_arm:
        if (state.armed) {
            std::wstring error;
            if (!disarm_bridge(state, &error)) {
                show_error(state.window, error);
            }
        } else {
            std::wstring error;
            if (!arm_bridge(state, &error)) {
                show_error(state.window, error);
            }
        }
        break;
    case backend_fidelity_fx:
        state.settings.backend = xrfg::standalone::FlowBackend::fidelity_fx;
        update_runtime_options(state);
        break;
    case backend_nvidia_fast:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::fast;
        update_runtime_options(state);
        break;
    case backend_nvidia_slow:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::slow;
        update_runtime_options(state);
        break;
    case backend_nvidia_medium:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::medium;
        update_runtime_options(state);
        break;
    case toggle_nvidia_bidirectional:
        state.settings.nvidia_bidirectional =
            !state.settings.nvidia_bidirectional;
        update_runtime_options(state);
        break;
    case nvidia_scale_full:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::full;
        update_runtime_options(state);
        break;
    case nvidia_scale_three_quarter:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::three_quarter;
        update_runtime_options(state);
        break;
    case nvidia_scale_half:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::half;
        update_runtime_options(state);
        break;
    case overlay_off:
    case overlay_upper_left:
    case overlay_upper_right:
    case overlay_lower_left:
    case overlay_lower_right:
        state.settings.overlay_position = command == overlay_off ? xrfg::FpsOverlayPosition::off
            : command == overlay_upper_left ? xrfg::FpsOverlayPosition::upper_left
            : command == overlay_lower_left ? xrfg::FpsOverlayPosition::lower_left
            : command == overlay_lower_right ? xrfg::FpsOverlayPosition::lower_right
            : xrfg::FpsOverlayPosition::upper_right;
        update_runtime_options(state, true);
        break;
    case toggle_diagnostics:
        state.settings.diagnostics = !state.settings.diagnostics;
        update_runtime_options(state);
        break;
    case open_logs: {
        const auto directory = runtime_directory(state.local_directory);
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        ShellExecuteW(
            state.window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case donate:
        ShellExecuteW(
            state.window, L"open", kDonateUrl, nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case show_about: {
        wchar_t version_label[64]{};
        swprintf_s(version_label, L"OFXR Bridge V%03u", kImplementationVersion);
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = state.window;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                         TDF_ENABLE_HYPERLINKS |
                         TDF_SIZE_TO_CONTENT |
                         TDF_USE_HICON_MAIN;
        dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        dialog.pszWindowTitle = kApplicationName;
        dialog.pszMainInstruction = version_label;
        dialog.pszContent =
            L"Licensed under LGPL-3.0-or-later.\r\n\r\n"
            L"<a href=\"https://github.com/tig3rmast3r/OFXR-Bridge\">"
            L"github.com/tig3rmast3r/OFXR-Bridge</a>";
        dialog.hMainIcon = state.disarmed_icon;
        dialog.pfCallback = [](
            HWND window,
            UINT notification,
            WPARAM,
            LPARAM parameter,
            LONG_PTR) -> HRESULT {
                if (notification == TDN_HYPERLINK_CLICKED) {
                    ShellExecuteW(
                        window,
                        L"open",
                        reinterpret_cast<LPCWSTR>(parameter),
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
                }
                return S_OK;
            };
        if (FAILED(TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr))) {
            const std::wstring message = std::wstring(version_label) +
                L"\r\n\r\nLicense: LGPL-3.0-or-later\r\n"
                L"https://github.com/tig3rmast3r/OFXR-Bridge";
            MessageBoxW(
                state.window,
                message.c_str(),
                kApplicationName,
                MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case exit_application:
        SendMessageW(state.window, WM_CLOSE, 0, 0);
        break;
    case add_game_profile: {
        std::wstring error;
        if (!prompt_new_game_profile(state, &error)) {
            if (!error.empty()) show_error(state.window, error);
        }
        break;
    }
    case manage_game_profiles: {
        std::error_code ignored;
        std::filesystem::create_directories(state.local_directory, ignored);
        if (!std::filesystem::is_regular_file(state.game_profiles_path)) {
            save_game_profiles(state);
        }
        ShellExecuteW(state.window, L"open", state.game_profiles_path.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    default:
        if (command >= game_profile_launch_base &&
            command <= game_profile_launch_max) {
            const std::size_t index = command - game_profile_launch_base;
            if (index < state.game_profiles.profiles.size()) {
                const auto& profile = state.game_profiles.profiles[index];
                std::wstring error;
                if (!launch_game_profile(state, profile, &error)) {
                    show_error(state.window, error);
                    break;
                }
                if (!arm_bridge(state, &error)) {
                    show_error(state.window, error);
                }
            }
        }
        break;
    }
}

LRESULT CALLBACK window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == state->taskbar_created_message) {
        static_cast<void>(add_tray_icon(*state));
        return 0;
    }
    switch (message) {
    case kTrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            show_context_menu(*state);
        } else if (lparam == WM_LBUTTONDBLCLK) {
            handle_command(*state, toggle_arm);
        }
        return 0;
    case WM_COMMAND:
        handle_command(*state, LOWORD(wparam));
        return 0;
    case WM_TIMER:
        if (wparam == kGameDetectionTimerId) {
            poll_game_detection(*state);
        }
        return 0;
    case WM_CLOSE: {
        std::wstring error;
        if (!disarm_bridge(*state, &error)) {
            show_error(window, error);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    }
    case WM_QUERYENDSESSION: {
        std::wstring error;
        if (!disarm_bridge(*state, &error)) {
            ShutdownBlockReasonCreate(window, L"OFXR Bridge could not disable its OpenXR registrations.");
            return FALSE;
        }
        ShutdownBlockReasonDestroy(window);
        return TRUE;
    }
    case WM_ENDSESSION:
        if (wparam != FALSE) {
            std::wstring error;
            if (!disarm_bridge(*state, &error))
                log_lifecycle(state->local_directory, L"end-session-cleanup", error);
            DestroyWindow(window);
        } else {
            // A cancelled shutdown remains disarmed; never silently re-arm.
            ShutdownBlockReasonDestroy(window);
        }
        return 0;
    case WM_DESTROY: {
        std::wstring error;
        if (!disarm_bridge(*state, &error))
            log_lifecycle(state->local_directory, L"destroy-cleanup", error);
        Shell_NotifyIconW(NIM_DELETE, &state->icon);
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

[[nodiscard]] bool watch_registered_arm(
    HANDLE parent, const std::filesystem::path& manifest,
    const std::filesystem::path& local_directory, std::wstring* error,
    xrfg::implicit_layer::RegistryScope scope,
    std::wstring_view registry_subkey = xrfg::implicit_layer::kRegistrySubkey) {
    if (!xrfg::implicit_layer::owned_registration_path(manifest, local_directory)) {
        if (error) *error = L"Cleanup watchdog refused an unowned manifest.";
        return false;
    }
    while (xrfg::implicit_layer::manifest_registered(manifest, scope, registry_subkey)) {
        if (parent == nullptr || WaitForSingleObject(parent, kArmPollMilliseconds) != WAIT_TIMEOUT)
            break;
    }
    // Scoped to this arm only: an old helper must never revoke a newer arm.
    return xrfg::implicit_layer::retire_manifest(
        manifest, scope, error, registry_subkey);
}

[[nodiscard]] std::optional<int> run_cleanup_helper() {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) {
        return EXIT_FAILURE;
    }
    if (argument_count < 2 || _wcsicmp(arguments[1], kCleanupArgument) != 0) {
        LocalFree(arguments);
        return std::nullopt;
    }
    if (argument_count != 5) {
        LocalFree(arguments);
        return EXIT_FAILURE;
    }
    const std::filesystem::path manifest(arguments[2]);
    wchar_t* end = nullptr;
    const unsigned long parsed_pid = std::wcstoul(arguments[3], &end, 10);
    const bool valid_pid = end != arguments[3] && end != nullptr && *end == L'\0' &&
                           parsed_pid > 0 && parsed_pid <= MAXDWORD;
    const std::wstring_view scope_argument(arguments[4]);
    const auto scope = _wcsicmp(scope_argument.data(), L"HKLM") == 0
        ? xrfg::implicit_layer::RegistryScope::local_machine
        : xrfg::implicit_layer::RegistryScope::current_user;
    const bool valid_scope = _wcsicmp(scope_argument.data(), L"HKLM") == 0 ||
                             _wcsicmp(scope_argument.data(), L"HKCU") == 0;
    LocalFree(arguments);
    if (!valid_pid || !valid_scope) {
        return EXIT_FAILURE;
    }

    std::filesystem::path directory;
    try {
        directory = runtime_directory(local_app_data() / L"OFXR Bridge");
    } catch (...) {
        return EXIT_FAILURE;
    }
    if (!xrfg::implicit_layer::owned_manifest_path(manifest, directory)) {
        return EXIT_FAILURE;
    }

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parsed_pid));
    std::wstring error;
    const bool cleaned = watch_registered_arm(parent, manifest,
        local_app_data() / L"OFXR Bridge", &error, scope);
    if (parent != nullptr) {
        CloseHandle(parent);
    }
    log_lifecycle(local_app_data() / L"OFXR Bridge", L"watchdog-cleanup", error);
    return cleaned ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (const auto helper_result = run_cleanup_helper()) {
        return *helper_result;
    }

    HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"Local\\OFXRBridgeTray");
    if (single_instance == nullptr) {
        return EXIT_FAILURE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge is already running in the notification area.",
            kApplicationName,
            MB_OK | MB_ICONINFORMATION);
        CloseHandle(single_instance);
        return EXIT_SUCCESS;
    }

    AppState state;
    try {
        state.executable_directory = executable_directory();
        state.local_directory = local_app_data() / L"OFXR Bridge";
        state.settings_path = state.local_directory / L"tray.ini";
        state.game_profiles_path =
            xrfg::game_profiles::store_path(state.local_directory);
        std::wstring cleanup_error;
        if (!cleanup_all_owned_registrations(state, &cleanup_error)) {
            log_lifecycle(state.local_directory, L"startup-cleanup", cleanup_error);
            MessageBoxW(nullptr, cleanup_error.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
            CloseHandle(single_instance);
            return EXIT_FAILURE;
        }
        log_lifecycle(state.local_directory, L"startup-cleanup");
        load_settings(state);
        load_game_profiles(state);
    } catch (...) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge could not initialize its local configuration.",
            kApplicationName,
            MB_OK | MB_ICONERROR);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }
    state.taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    state.armed_icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_OFXR_ARMED));
    state.disarmed_icon = LoadIconW(
        instance, MAKEINTRESOURCEW(IDI_OFXR_DISARMED));
    if (state.armed_icon == nullptr || state.disarmed_icon == nullptr) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge could not load its notification icons.",
            kApplicationName,
            MB_OK | MB_ICONERROR);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hIcon = state.disarmed_icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    window_class.hIconSm = state.disarmed_icon;
    if (RegisterClassExW(&window_class) == 0) {
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }

    const HWND window = CreateWindowExW(
        0,
        kWindowClass,
        kApplicationName,
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        instance,
        &state);
    if (window == nullptr || !add_tray_icon(state)) {
        if (window) DestroyWindow(window);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }
    SetTimer(window, kGameDetectionTimerId, kGameDetectionPollMilliseconds, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    KillTimer(window, kGameDetectionTimerId);
    std::wstring cleanup_error;
    const bool clean_exit = disarm_bridge(state, &cleanup_error);
    if (!clean_exit) log_lifecycle(state.local_directory, L"message-loop-exit", cleanup_error);
    CloseHandle(single_instance);
    return clean_exit ? static_cast<int>(message.wParam) : EXIT_FAILURE;
}
