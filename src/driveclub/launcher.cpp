// SPDX-FileCopyrightText: Copyright 2026 Driveclub-PC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "driveclub/launch_check.h"
#include "driveclub/launcher.h"
#include "driveclub/widescreen.h"

#ifndef _WIN32
#include <bit>
#include <cfloat>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <tuple>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "imgui/big_picture/imgui_impl_sdl3_big_picture.h"
#include "imgui/big_picture/imgui_impl_sdlrenderer3.h"
#include "imgui/renderer/font_stack.h"
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must precede the control and shell headers.
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace DriveclubPC::Launcher {

namespace {

namespace fs = std::filesystem;

fs::path TextPath(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

struct Settings {
    std::string game_dir;
    std::string save_dir; // driveclub-pc: empty uses the existing emulator home, saves only
    int target_fps = 60;
    bool fullscreen = true;
    int width = 0; ///< 0 = the primary display's resolution
    int height = 0;
    bool fsr = true;
    int sharpening = 1; ///< 0 off, 1 normal, 2 strong
    int readbacks = 2;  ///< driveclub-pc: 0 off, 1 relaxed, 2 precise (default: Relaxed broke
                        ///< shadows and vegetation in the owner's races)
    // driveclub-pc: image quality and hybrid CPU settings.
    int af = 16;
    int aa = 1;
    bool pcore_pin = true;
    // driveclub-pc: the game's own render scaling (render_res.h): 1080, 1440 or 2160 lines.
    int render_res = 1080;
    bool async_shaders = true; ///< shader translation and pipeline creation off-thread
    bool frame_generation = false;
    bool show_fps = false;
    bool deferred_recording = false;
    bool classic_vblank = false;
    bool show_launcher = true;
};

fs::path IniPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "driveclub_pc.ini";
}

Settings Load() {
    Settings s;
    std::ifstream in(IniPath());
    std::map<std::string, std::string> kv;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) {
            line.erase(0, 3); // UTF-8 byte order mark
        }
        const auto eq = line.find('=');
        if (eq != std::string::npos && line[0] != '#') {
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    const auto num = [&](const char* key, int fallback) {
        const auto it = kv.find(key);
        return it == kv.end() ? fallback : std::atoi(it->second.c_str());
    };
    if (kv.contains("game_dir")) {
        s.game_dir = kv["game_dir"];
    }
    if (kv.contains("save_dir")) {
        s.save_dir = kv["save_dir"];
    }
    s.target_fps = num("target_fps", s.target_fps);
    s.fullscreen = num("fullscreen", s.fullscreen) != 0;
    s.width = num("width", s.width);
    s.height = num("height", s.height);
    s.fsr = num("fsr", s.fsr) != 0;
    s.sharpening = std::clamp(num("sharpening", s.sharpening), 0, 2);
    s.readbacks = std::clamp(num("readbacks", s.readbacks), 0, 2);
    s.af = num("af", s.af);
    if (s.af != 2 && s.af != 4 && s.af != 8 && s.af != 16)
        s.af = 0;
    s.aa = std::clamp(num("aa", s.aa), 0, 2);
    s.pcore_pin = num("pcore_pin", 1) != 0;
    s.render_res = num("render_res", s.render_res);
    if (s.render_res != 1440 && s.render_res != 2160) {
        s.render_res = 1080;
    }
    s.async_shaders = num("async_shaders", 1) != 0;
    s.frame_generation = num("frame_generation", 0) != 0;
    s.show_fps = num("show_fps", s.show_fps) != 0;
    s.deferred_recording = num("deferred_recording", s.deferred_recording) != 0;
    s.classic_vblank = num("classic_vblank", s.classic_vblank) != 0;
    s.show_launcher = num("show_launcher", s.show_launcher) != 0;
    if (s.target_fps != 30 && s.target_fps != 60 && s.target_fps != 120) {
        s.target_fps = 60;
    }
    // driveclub-pc: FG is the dedicated 60-FPS-base mode in the launcher.
    s.frame_generation = s.frame_generation && s.target_fps == 60;
    return s;
}

void Save(const Settings& s) {
    std::ofstream out(IniPath());
    out << "# Driveclub-PC launcher settings\n"
        << "game_dir=" << s.game_dir << "\n"
        << "save_dir=" << s.save_dir << "\n"
        << "target_fps=" << s.target_fps << "\n"
        << "fullscreen=" << int(s.fullscreen) << "\n"
        << "width=" << s.width << "\n"
        << "height=" << s.height << "\n"
        << "fsr=" << int(s.fsr) << "\n"
        << "sharpening=" << s.sharpening << "\n"
        << "readbacks=" << s.readbacks << "\n"
        << "af=" << s.af << "\n"
        << "aa=" << s.aa << "\n"
        << "pcore_pin=" << int(s.pcore_pin) << "\n"
        << "render_res=" << s.render_res << "\n"
        << "async_shaders=" << int(s.async_shaders) << "\n"
        << "frame_generation=" << int(s.frame_generation) << "\n"
        << "show_fps=" << int(s.show_fps) << "\n"
        << "deferred_recording=" << int(s.deferred_recording) << "\n"
        << "classic_vblank=" << int(s.classic_vblank) << "\n"
        << "show_launcher=" << int(s.show_launcher) << "\n";
}

std::pair<int, int> DisplaySize() {
    int width = 1920, height = 1080;
#ifdef _WIN32
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth > 0) {
        width = static_cast<int>(mode.dmPelsWidth);
        height = static_cast<int>(mode.dmPelsHeight);
    }
#else
    // driveclub-pc (linux): the desktop size of the primary display, through SDL.
    if (SDL_Init(SDL_INIT_VIDEO)) {
        const SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display)) {
            if (mode->w > 0 && mode->h > 0) {
                width = mode->w;
                height = mode->h;
            }
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
#endif
    return {width, height};
}

/// driveclub-pc: the game's output size: the render resolution lines at the display's aspect
/// in fullscreen or the window's aspect in windowed mode (wider than 16:9: widescreen.h).
Widescreen::Output TargetOutput(const Settings& s) {
    auto [w, h] = DisplaySize();
    if (!s.fullscreen && s.width > 0 && s.height > 0) {
        w = s.width;
        h = s.height;
    }
    return Widescreen::ComputeOutput(w, h, static_cast<u32>(s.render_res));
}

/// driveclub-pc: extra video memory the game needs for its render targets at this size.
int ExtraVramMb(const Widescreen::Output& out) {
    const u64 pixels = u64{out.width} * out.height;
    return pixels <= 1920u * 1080u ? 0 : pixels <= 2560u * 1440u ? 1024 : 1536;
}

/// Writes the graphics settings into the per-game emulator config, keeping its other keys.
void WriteGameConfig(const Settings& s) {
    const auto title_id = ReadGameTitleId(TextPath(s.game_dir)).value_or(std::string(TitleId));
    EnsureDefaultGameConfig(title_id);
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
                      fmt::format("{}.json", title_id);
    nlohmann::json json = nlohmann::json::object();
    {
        std::ifstream in(path);
        if (in) {
            json = nlohmann::json::parse(in, nullptr, false);
            if (json.is_discarded() || !json.is_object()) {
                json = nlohmann::json::object();
            }
        }
    }
    const auto [dw, dh] = DisplaySize();
    auto& gpu = json["GPU"];
    gpu["full_screen"] = s.fullscreen;
    gpu["full_screen_mode"] = s.fullscreen ? "Fullscreen (Borderless)" : "Windowed";
    gpu["window_width"] = s.width > 0 ? s.width : dw;
    gpu["window_height"] = s.height > 0 ? s.height : dh;
    gpu["fsr_enabled"] = s.fsr;
    gpu["rcas_enabled"] = s.sharpening != 0;
    gpu["rcas_attenuation"] = s.sharpening == 2 ? 100 : 250;
    gpu["readbacks_mode"] = s.readbacks;
    // driveclub-pc: a higher render resolution needs a bigger video-memory budget in the game
    // (DCPC_VRAM_EXTRA_MB below), backed by extra direct memory.
    json["General"]["extra_dmem_in_mbytes"] = ExtraVramMb(TargetOutput(s));
    std::ofstream out(path);
    out << json.dump(2) << "\n";
}

void SetEnvDefault(const char* name, const std::string& value) {
    // A value already in the environment (test scripts) wins.
    if (std::getenv(name) == nullptr) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 0);
#endif
    }
}

void ApplyRuntime(const Settings& s) {
    EmulatorSettings.SetSaveDataHome(TextPath(s.save_dir));
    SetEnvDefault("DCPC_AF", std::to_string(s.af));
    SetEnvDefault("DCPC_AA", std::to_string(s.aa));
    SetEnvDefault("DCPC_PCORE_PIN", s.pcore_pin ? "1" : "0");
    SetEnvDefault("DCPC_FRAME_GEN", s.frame_generation ? "1" : "0");
    SetEnvDefault("DCPC_ASYNC_SHADERS", s.async_shaders ? "1" : "0");
    SetEnvDefault("DCPC_ASYNC_PIPELINES", s.async_shaders ? "1" : "0");
    const auto out = TargetOutput(s);
    if (out.height != 1080) {
        // The engine's own -resolutionscale switch scales its render targets (width aligned to
        // 128, height = width * 9 / 16, or table height * scale with widescreen).
        SetEnvDefault("DCPC_GAME_ARGS",
                      out.height == 2160 ? "-resolutionscale 2" : "-resolutionscale 1.3333");
    }
    if (out.wide || out.height != 1080) {
        // The game's display buffers follow DCPC_RENDER_RES.
        SetEnvDefault("DCPC_VRAM_EXTRA_MB", std::to_string(ExtraVramMb(out)));
        SetEnvDefault("DCPC_RENDER_RES", fmt::format("{}x{}", out.width, out.height));
    }
    if (out.wide) {
        // Aspect, Hor+ FOV and a wide scene (widescreen.h).
        SetEnvDefault("DCPC_WIDESCREEN", fmt::format("{}x{}", out.width, out.height));
    }
    SetEnvDefault("DCPC_TARGET_FPS", std::to_string(s.target_fps));
    SetEnvDefault("DCPC_DEFERRED_RECORDING", s.deferred_recording ? "1" : "0");
    SetEnvDefault("DCPC_CLASSIC_VBLANK", s.classic_vblank ? "1" : "0");
    if (s.show_fps) {
        EmulatorSettings.SetShowFpsCounter(true);
    }
}

fs::path SaveRoot(const Settings& s) {
    return s.save_dir.empty() ? EmulatorSettings.GetHomeDir() : TextPath(s.save_dir);
}

std::string PathText(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// driveclub-pc: validate before launching; never silently fall back to a different save root.
bool PrepareSaveRoot(const Settings& s, std::string& error) {
    if (s.save_dir.empty()) {
        return true;
    }
    const auto root = SaveRoot(s);
    if (!root.is_absolute()) {
        error = "The save data root must be an absolute path. Choose a folder in the launcher.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec || !fs::is_directory(root, ec)) {
        error = fmt::format("Cannot use save data root:\n{}\n{}", PathText(root), ec.message());
        return false;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto probe = root / fmt::format(".dcpc-write-check-{}", stamp);
    // Creating a uniquely named empty directory checks write access without touching save files.
    if (!fs::create_directory(probe, ec)) {
        error =
            fmt::format("Save data root is not writable:\n{}\n{}", PathText(root), ec.message());
        return false;
    }
    fs::remove(probe, ec);
    if (ec) {
        error = fmt::format("Cannot remove save directory write check:\n{}\n{}", PathText(probe),
                            ec.message());
        return false;
    }
    LOG_INFO(Loader, "driveclub-pc: custom save data root {} (existing saves are not moved)",
             PathText(root));
    return true;
}

std::string FirmwareStatus(bool& ok, std::string_view title_id = TitleId) {
    const auto report = InspectSysModules(title_id);
    ok = report.ready;
    return report.summary;
}

#ifdef _WIN32

std::wstring Widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

enum Id : int {
    IdGameEdit = 100,
    IdBrowse,
    IdGameStatus,
    IdFirmwareStatus,
    IdImport,
    IdFps,
    IdDisplay,
    IdResolution,
    IdRenderRes,
    IdUpscaling,
    IdSharpening,
    IdReadbacks,
    IdAf,
    IdAa,
    IdPcore,
    IdAsyncShaders,
    IdShowFps,
    IdDeferred,
    IdClassic,
    IdShowLauncher,
    IdPlay,
    IdQuit,
    IdSavePath,
    IdSaveBrowse,
    IdSaveOpen,
    IdSaveDefault,
    IdFirmwareDetails,
};

struct Ui {
    Settings settings;
    HWND window{};
    HFONT font{}, title_font{};
    std::map<int, HWND> controls;
    bool game_ok = false;
    bool firmware_ok = false;
    bool play = false;
    float scale = 1.0f;
    std::vector<std::pair<int, int>> resolutions;
    int scroll = 0;
    int content_height = 0;
};

Ui* g_ui = nullptr;

int Px(float dip) {
    return static_cast<int>(dip * g_ui->scale + 0.5f);
}

std::optional<std::wstring> PickFolder(HWND owner, const wchar_t* title) {
    std::optional<std::wstring> result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return result;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::string GetText(int id) {
    const HWND h = g_ui->controls[id];
    const int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(n), L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    return Narrow(w);
}

void SetText(int id, const std::string& text) {
    SetWindowTextW(g_ui->controls[id], Widen(text).c_str());
}

void RefreshStatus() {
    auto& ui = *g_ui;
    const std::string dir = GetText(IdGameEdit);
    std::string message;
    if (dir.empty()) {
        ui.game_ok = false;
        message = "Choose a Driveclub base game folder. Tested: CUSA00093 v1.28.";
    } else {
        std::string error;
        const auto eboot = ResolveAndValidateGame(TextPath(dir), error);
        ui.game_ok = eboot.has_value();
        message =
            ui.game_ok
                ? GameCompatibilityNote(ReadGameTitleId(*eboot).value_or(std::string(TitleId)))
                : error;
    }
    std::replace(message.begin(), message.end(), '\n', ' ');
    SetText(IdGameStatus, message);
    bool fw = false;
    const auto title_id = ReadGameTitleId(TextPath(dir)).value_or(std::string(TitleId));
    std::string fw_text = FirmwareStatus(fw, title_id);
    std::replace(fw_text.begin(), fw_text.end(), '\n', ' ');
    ui.firmware_ok = fw;
    SetText(IdFirmwareStatus, fw_text);
    EnableWindow(ui.controls[IdPlay], ui.game_ok && ui.firmware_ok);
    InvalidateRect(ui.window, nullptr, TRUE);
}

void RefreshSavePath() {
    SetText(IdSavePath, PathText(SaveRoot(g_ui->settings)));
}

void OpenSaveFolder() {
    std::string error;
    if (!PrepareSaveRoot(g_ui->settings, error)) {
        MessageBoxW(g_ui->window, Widen(error).c_str(), L"Driveclub-PC save data",
                    MB_OK | MB_ICONERROR);
        return;
    }
    const auto path = SaveRoot(g_ui->settings);
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec || reinterpret_cast<INT_PTR>(ShellExecuteW(g_ui->window, L"open", path.c_str(), nullptr,
                                                      nullptr, SW_SHOWNORMAL)) <= 32) {
        MessageBoxW(g_ui->window,
                    L"Cannot open the save data folder. Check the path and permissions.",
                    L"Driveclub-PC", MB_OK | MB_ICONERROR);
    }
}

void ImportFirmware() {
    const auto source = PickFolder(g_ui->window, L"Folder with your PS4 firmware modules (*.sprx)");
    if (!source) {
        return;
    }
    const fs::path dest = EmulatorSettings.GetSysModulesDir();
    std::error_code ec;
    fs::create_directories(dest, ec);
    int copied = 0, skipped = 0, failed = 0;
    for (const auto& entry : fs::directory_iterator(*source, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sprx") {
            std::error_code copy_ec;
            if (fs::copy_file(entry.path(), dest / entry.path().filename(),
                              fs::copy_options::skip_existing, copy_ec)) {
                ++copied;
            } else if (copy_ec) {
                ++failed;
            } else {
                ++skipped;
            }
        }
    }
    MessageBoxW(g_ui->window,
                Widen(fmt::format("Copied {} module(s), kept {} existing, {} failed.\n{}\n\n"
                                  "See Details for missing or invalid modules. Existing files "
                                  "are never overwritten.",
                                  copied, skipped, failed, PathText(dest)))
                    .c_str(),
                L"Driveclub-PC", MB_OK | MB_ICONINFORMATION);
    RefreshStatus();
}

int ComboIndex(int id) {
    return static_cast<int>(SendMessageW(g_ui->controls[id], CB_GETCURSEL, 0, 0));
}

bool Checked(int id) {
    return SendMessageW(g_ui->controls[id], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void CollectSettings() {
    auto& s = g_ui->settings;
    s.game_dir = GetText(IdGameEdit);
    static constexpr int fps_values[] = {30, 60, 60, 120};
    const int frame_mode = std::clamp(ComboIndex(IdFps), 0, 3);
    s.target_fps = fps_values[frame_mode];
    s.frame_generation = frame_mode == 2;
    s.fullscreen = ComboIndex(IdDisplay) == 0;
    const int res =
        std::clamp(ComboIndex(IdResolution), 0, static_cast<int>(g_ui->resolutions.size()) - 1);
    s.width = g_ui->resolutions[res].first;
    s.height = g_ui->resolutions[res].second;
    s.fsr = ComboIndex(IdUpscaling) == 0;
    s.sharpening = std::clamp(ComboIndex(IdSharpening), 0, 2);
    s.readbacks = 2 - std::clamp(ComboIndex(IdReadbacks), 0, 2);
    static constexpr int af_values[] = {0, 2, 4, 8, 16};
    s.af = af_values[std::clamp(ComboIndex(IdAf), 0, 4)];
    s.aa = std::clamp(ComboIndex(IdAa), 0, 2);
    s.pcore_pin = Checked(IdPcore);
    static constexpr int render_values[] = {1080, 1440, 2160};
    s.render_res = render_values[std::clamp(ComboIndex(IdRenderRes), 0, 2)];
    s.async_shaders = Checked(IdAsyncShaders);
    s.show_fps = Checked(IdShowFps);
    s.deferred_recording = Checked(IdDeferred);
    s.classic_vblank = Checked(IdClassic);
    s.show_launcher = Checked(IdShowLauncher);
}

/// driveclub-pc: the window size only applies in windowed mode.
void UpdateWindowSizeEnabled() {
    const auto it = g_ui->controls.find(IdResolution);
    if (it != g_ui->controls.end()) {
        EnableWindow(it->second, ComboIndex(IdDisplay) == 1);
    }
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, float x, float y, float w, float h,
          int id, DWORD ex_style = 0) {
    HWND hwnd =
        CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(w),
                        Px(h), g_ui->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui->font), TRUE);
    if (id > 0) {
        g_ui->controls[id] = hwnd;
    }
    return hwnd;
}

HWND Combo(int id, float x, float y, float w, const std::vector<std::wstring>& items, int sel) {
    HWND h = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 200, id);
    for (const auto& item : items) {
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    SendMessageW(h, CB_SETCURSEL, sel, 0);
    return h;
}

void Label(const wchar_t* text, float x, float y, float w, float h = 20) {
    Make(L"STATIC", text, SS_LEFT, x, y + 3, w, h, 0);
}

void BuildControls() {
    auto& ui = *g_ui;
    const auto& s = ui.settings;
    constexpr float L = 24, C = 170, W = 400, Row = 36;
    float y = 72;

    Label(L"Game folder", L, y, 140);
    Make(L"EDIT", Widen(s.game_dir).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, C, y, W - 96, 24,
         IdGameEdit, WS_EX_CLIENTEDGE);
    Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, C + W - 88, y - 1, 88, 26, IdBrowse);
    y += 28;
    Make(L"STATIC", L"", SS_LEFT, C, y, W, 34, IdGameStatus);
    y += 40;

    Label(L"Firmware", L, y, 140);
    Make(L"STATIC", L"", SS_LEFT, C, y + 3, W, 34, IdFirmwareStatus);
    y += 38;
    Make(L"BUTTON", L"Import...", BS_PUSHBUTTON | WS_TABSTOP, C, y, 96, 26, IdImport);
    Make(L"BUTTON", L"Details...", BS_PUSHBUTTON | WS_TABSTOP, C + 104, y, 96, 26,
         IdFirmwareDetails);
    y += 38;

    Label(L"Save data root", L, y, 140);
    Make(L"EDIT", Widen(PathText(SaveRoot(s))).c_str(), ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP,
         C, y, W, 24, IdSavePath, WS_EX_CLIENTEDGE);
    y += 30;
    Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, C, y, 96, 26, IdSaveBrowse);
    Make(L"BUTTON", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, C + 104, y, 104, 26, IdSaveOpen);
    Make(L"BUTTON", L"Use default", BS_PUSHBUTTON | WS_TABSTOP, C + 216, y, 104, 26, IdSaveDefault);
    y += 30;
    Label(L"Saves: <root>\\<user-id>\\savedata\\<game-region>.\nExisting saves are not moved.", C,
          y, W, 36);
    y += 46;

    Label(L"Frame rate", L, y, 140);
    Combo(IdFps, C, y, W,
          {L"30 FPS (as on PS4)", L"60 FPS (recommended)", L"60 FPS + FG (2x, ~120 output)",
           L"120 FPS (experimental)"},
          s.target_fps == 30    ? 0
          : s.target_fps == 120 ? 3
          : s.frame_generation ? 2
                                : 1);
    y += Row;

    Label(L"Display", L, y, 140);
    Combo(IdDisplay, C, y, W, {L"Fullscreen (borderless)", L"Window"}, s.fullscreen ? 0 : 1);
    y += Row;

    const auto [dw, dh] = DisplaySize();
    // driveclub-pc: the three ultrawide sizes get a matching wide output (widescreen.h).
    ui.resolutions = {{0, 0},       {5120, 1440}, {3840, 2160}, {3440, 1440},
                      {2560, 1600}, {2560, 1440}, {2560, 1080}, {1920, 1200},
                      {1920, 1080}, {1600, 900},  {1280, 720}};
    std::vector<std::wstring> res_items;
    int res_sel = 0;
    for (size_t i = 0; i < ui.resolutions.size(); ++i) {
        const auto [w, h] = ui.resolutions[i];
        res_items.push_back(i == 0 ? Widen(fmt::format("Same as the display ({}x{})", dw, dh))
                                   : Widen(fmt::format("{}x{}", w, h)));
        if (w == s.width && h == s.height) {
            res_sel = static_cast<int>(i);
        }
    }
    // driveclub-pc: borderless fullscreen always covers the whole display; this is only the size
    // of the window in windowed mode.
    Label(L"Window size", L, y, 140);
    Combo(IdResolution, C, y, W, res_items, res_sel);
    UpdateWindowSizeEnabled();
    y += Row;

    // driveclub-pc: lines; the width follows the display (or window) aspect, see widescreen.h.
    Label(L"Render resolution", L, y, 140);
    Combo(IdRenderRes, C, y, W,
          {L"1080p (native, fastest)", L"1440p (sharper, about 10-15% slower)",
           L"2160p (very demanding)"},
          s.render_res == 2160   ? 2
          : s.render_res == 1440 ? 1
                                 : 0);
    y += Row;

    Label(L"Upscaling", L, y, 140);
    Combo(IdUpscaling, C, y, W, {L"AMD FSR 1 (game image to window size)", L"Bilinear"},
          s.fsr ? 0 : 1);
    y += Row;

    Label(L"Sharpening", L, y, 140);
    Combo(IdSharpening, C, y, W, {L"Off", L"Normal (RCAS)", L"Strong (RCAS)"}, s.sharpening);
    y += Row;

    Label(L"Anisotropic filter", L, y, 140);
    Combo(IdAf, C, y, W, {L"Off (game setting)", L"2x", L"4x", L"8x", L"16x"},
          s.af == 16  ? 4
          : s.af == 8 ? 3
          : s.af == 4 ? 2
          : s.af == 2 ? 1
                      : 0);
    y += Row;
    Label(L"Anti-aliasing", L, y, 140);
    Combo(IdAa, C, y, W, {L"Off", L"FXAA (default)", L"SMAA 1x (High)"}, s.aa);
    y += Row;

    Label(L"GPU readbacks", L, y, 140);
    Combo(IdReadbacks, C, y, W,
          {L"Precise (default)", L"Relaxed (faster, shadow/vegetation glitches)",
           L"Off (fastest, effects may break)"},
          2 - s.readbacks);
    y += Row + 6;

    Make(L"BUTTON", L"Prefer performance cores (hybrid CPUs)", BS_AUTOCHECKBOX | WS_TABSTOP, C, y,
         W, 22, IdPcore);
    SendMessageW(ui.controls[IdPcore], BM_SETCHECK, s.pcore_pin ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 26;
    Make(L"BUTTON", L"Compile shaders in the background (less stutter)",
         BS_AUTOCHECKBOX | WS_TABSTOP, C, y, W, 22, IdAsyncShaders);
    SendMessageW(ui.controls[IdAsyncShaders], BM_SETCHECK,
                 s.async_shaders ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 26;
    Make(L"BUTTON", L"Show FPS counter", BS_AUTOCHECKBOX | WS_TABSTOP, C, y, W, 22, IdShowFps);
    SendMessageW(ui.controls[IdShowFps], BM_SETCHECK, s.show_fps ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 26;
    Make(L"BUTTON", L"Experimental: record Vulkan commands on a second thread",
         BS_AUTOCHECKBOX | WS_TABSTOP, C, y, W, 22, IdDeferred);
    SendMessageW(ui.controls[IdDeferred], BM_SETCHECK,
                 s.deferred_recording ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 26;
    Make(L"BUTTON", L"Classic PS4 frame pacing (fallback if timing looks wrong)",
         BS_AUTOCHECKBOX | WS_TABSTOP, C, y, W, 22, IdClassic);
    SendMessageW(ui.controls[IdClassic], BM_SETCHECK,
                 s.classic_vblank ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 26;
    Make(L"BUTTON", L"Show this window at start (hold Shift to open it anyway)",
         BS_AUTOCHECKBOX | WS_TABSTOP, C, y, W, 22, IdShowLauncher);
    SendMessageW(ui.controls[IdShowLauncher], BM_SETCHECK,
                 s.show_launcher ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 30;
    Label(L"Quit game: Left Ctrl + Left Shift + End (default shortcut).", C, y, W, 20);
    y += 38;

    Make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, C + W - 196, y, 96, 32, IdPlay);
    Make(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, C + W - 92, y, 92, 32, IdQuit);
    ui.content_height = Px(y + 52);
}

void ScrollTo(int position) {
    RECT client;
    GetClientRect(g_ui->window, &client);
    const int next = std::clamp(
        position, 0, std::max(0, g_ui->content_height - static_cast<int>(client.bottom)));
    const int delta = g_ui->scroll - next;
    g_ui->scroll = next;
    ScrollWindowEx(g_ui->window, 0, delta, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    SetScrollPos(g_ui->window, SB_VERT, next, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &si);
        int next = g_ui->scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP:
            next -= Px(36);
            break;
        case SB_LINEDOWN:
            next += Px(36);
            break;
        case SB_PAGEUP:
            next -= static_cast<int>(si.nPage);
            break;
        case SB_PAGEDOWN:
            next += static_cast<int>(si.nPage);
            break;
        case SB_THUMBTRACK:
            next = si.nTrackPos;
            break;
        case SB_TOP:
            next = 0;
            break;
        case SB_BOTTOM:
            next = si.nMax;
            break;
        }
        ScrollTo(next);
        return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollTo(g_ui->scroll - GET_WHEEL_DELTA_WPARAM(wp) * Px(72) / WHEEL_DELTA);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetBkMode(dc, TRANSPARENT);
        if (id == IdGameStatus) {
            SetTextColor(dc, g_ui->game_ok ? RGB(20, 120, 40) : RGB(170, 30, 30));
        } else if (id == IdFirmwareStatus) {
            SetTextColor(dc, g_ui->firmware_ok ? RGB(20, 120, 40) : RGB(170, 30, 30));
        }
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wp);
        if (msg == WM_PRINTCLIENT) {
            RECT client;
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
        }
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g_ui->title_font);
        RECT r{Px(24), Px(14) - g_ui->scroll, Px(600), Px(44) - g_ui->scroll};
        DrawTextW(dc, L"Driveclub-PC", -1, &r, DT_LEFT | DT_SINGLELINE);
        SelectObject(dc, g_ui->font);
        SetTextColor(dc, RGB(90, 90, 90));
        RECT r2{Px(24), Px(44) - g_ui->scroll, Px(580), Px(64) - g_ui->scroll};
        DrawTextW(dc, L"Tested: CUSA00093 v1.28. Other Driveclub regions are experimental.", -1,
                  &r2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        if (msg == WM_PAINT) {
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    case WM_COMMAND:
        // driveclub-pc: keyboard navigation must reveal controls below the visible area too.
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == BN_SETFOCUS || HIWORD(wp) == CBN_SETFOCUS) {
            RECT control;
            GetWindowRect(reinterpret_cast<HWND>(lp), &control);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&control), 2);
            RECT client;
            GetClientRect(hwnd, &client);
            if (control.top < 0) {
                ScrollTo(g_ui->scroll + control.top - Px(8));
            } else if (control.bottom > client.bottom) {
                ScrollTo(g_ui->scroll + control.bottom - client.bottom + Px(8));
            }
        }
        switch (LOWORD(wp)) {
        case IdBrowse:
            if (const auto dir = PickFolder(hwnd, L"Driveclub base game folder (any region)")) {
                SetWindowTextW(g_ui->controls[IdGameEdit], dir->c_str());
            }
            return 0;
        case IdGameEdit:
            if (HIWORD(wp) == EN_CHANGE) {
                RefreshStatus();
            }
            return 0;
        case IdImport:
            ImportFirmware();
            return 0;
        case IdFirmwareDetails: {
            const auto title_id =
                ReadGameTitleId(TextPath(GetText(IdGameEdit))).value_or(std::string(TitleId));
            const auto report = InspectSysModules(title_id);
            MessageBoxW(hwnd, Widen(report.details).c_str(), L"Driveclub-PC firmware",
                        MB_OK | (report.ready ? MB_ICONINFORMATION : MB_ICONWARNING));
            return 0;
        }
        case IdSaveBrowse:
            if (const auto dir =
                    PickFolder(hwnd, L"Save data root (existing saves are not moved)")) {
                g_ui->settings.save_dir = Narrow(*dir);
                RefreshSavePath();
            }
            return 0;
        case IdSaveDefault:
            g_ui->settings.save_dir.clear();
            RefreshSavePath();
            return 0;
        case IdSaveOpen:
            OpenSaveFolder();
            return 0;
        case IdDisplay:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                UpdateWindowSizeEnabled();
            }
            return 0;
        case IdPlay:
            CollectSettings();
            {
                std::string error;
                if (!PrepareSaveRoot(g_ui->settings, error)) {
                    MessageBoxW(hwnd, Widen(error).c_str(), L"Driveclub-PC save data",
                                MB_OK | MB_ICONERROR);
                    return 0;
                }
            }
            g_ui->play = true;
            DestroyWindow(hwnd);
            return 0;
        case IdQuit:
            CollectSettings();
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        CollectSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/// Returns true when the user pressed Play; `s` holds the chosen settings either way.
bool ShowWindow(Settings& s) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    Ui ui;
    ui.settings = s;
    g_ui = &ui;
    ui.scale = static_cast<float>(GetDpiForSystem()) / 96.0f;

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -Px(14);
    ui.font = CreateFontIndirectW(&lf);
    lf.lfHeight = -Px(24);
    lf.lfWeight = FW_SEMIBOLD;
    ui.title_font = CreateFontIndirectW(&lf);

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"DriveclubPCLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT rect{0, 0, Px(600), std::min(Px(920), static_cast<int>(work.bottom - work.top) - Px(60))};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL;
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    ui.window = CreateWindowExW(
        0, wc.lpszClassName, L"Driveclub-PC", style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    BuildControls();
    RECT client;
    GetClientRect(ui.window, &client);
    SCROLLINFO si{sizeof(si),
                  SIF_RANGE | SIF_PAGE,
                  0,
                  ui.content_height - 1,
                  static_cast<UINT>(client.bottom),
                  0,
                  0};
    SetScrollInfo(ui.window, SB_VERT, &si, TRUE);
    RefreshStatus();
    ::ShowWindow(ui.window, SW_SHOW);
    SetForegroundWindow(ui.window);
    if (const char* snapshot = std::getenv("DCPC_LAUNCHER_SNAPSHOT")) {
        if (std::getenv("DCPC_LAUNCHER_SNAPSHOT_BOTTOM")) {
            ScrollTo(ui.content_height);
        }
        // Test hook: save an image of this window only (24-bit BMP) and close it.
        for (int i = 0; i < 40; ++i) {
            MSG pending;
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            Sleep(10);
        }
        RECT rc;
        GetWindowRect(ui.window, &rc);
        const int sw = rc.right - rc.left, sh = rc.bottom - rc.top;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), sw, -sh, 1, 24, BI_RGB};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        SendMessageW(ui.window, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                     PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        const DWORD stride = static_cast<DWORD>((sw * 3 + 3) & ~3);
        BITMAPFILEHEADER fh{
            0x4D42,
            static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + stride * sh),
            0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
        std::ofstream f(snapshot, std::ios::binary);
        f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
        f.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(bi.bmiHeader));
        f.write(static_cast<const char*>(bits), static_cast<std::streamsize>(stride) * sh);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        DestroyWindow(ui.window);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(ui.window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    s = ui.settings;
    DeleteObject(ui.font);
    DeleteObject(ui.title_font);
    UnregisterClassW(wc.lpszClassName, inst);
    g_ui = nullptr;
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return ui.play;
}

#endif

} // namespace

#ifndef _WIN32

// driveclub-pc (linux): the settings window, drawn with SDL3 + Dear ImGui (the same stack as the
// Big Picture mode). It mirrors the Windows window: game folder, firmware import, frame rate,
// display, render resolution, upscaling, image quality and the runtime options.
namespace {

struct FolderPick {
    std::mutex mutex;
    bool finished = false;
    bool has_path = false;
    std::string path;
};

// SDL calls this from another thread when the folder dialog closes.
void SDLCALL OnFolderPicked(void* userdata, const char* const* list, int /*filter*/) {
    auto* pick = static_cast<FolderPick*>(userdata);
    std::lock_guard lock{pick->mutex};
    if (list != nullptr && list[0] != nullptr) {
        pick->path = list[0];
        pick->has_path = true;
    }
    pick->finished = true;
}

std::string ImportFirmwareFrom(const fs::path& source) {
    const fs::path dest = EmulatorSettings.GetSysModulesDir();
    std::error_code ec;
    fs::create_directories(dest, ec);
    int copied = 0;
    for (const auto& entry : fs::directory_iterator(source, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sprx") {
            std::error_code copy_ec;
            fs::copy_file(entry.path(), dest / entry.path().filename(),
                          fs::copy_options::skip_existing, copy_ec);
            copied += copy_ec ? 0 : 1;
        }
    }
    return fmt::format("Copied {} module(s) into {}", copied, dest.string());
}

} // namespace

namespace {

// ---- game patches: list them, switch them on and off, save (like the Patches tab of shadPS4) -----
// A patch folder (for example ~/.local/share/shadPS4/patches/shadPS4/) holds a files.json that maps
// each XML file to the title ids it is for, plus the XML files. The emulator applies the patches
// whose <Metadata> has isEnabled="true"; this part only edits that attribute.
struct PatchEntry {
    std::string name;
    std::string note;
    bool enabled = false;
    bool locked = false; // Always="true": not meant to be switched by the user
};

struct PatchFileInfo {
    fs::path path;     // the XML file
    std::string file;  // file name, for example Driveclub.xml
    std::string repo;  // folder name, for example shadPS4
};

std::vector<PatchFileInfo> FindPatchFiles(const fs::path& patches_dir,
                                          const std::set<std::string>& title_ids) {
    std::vector<PatchFileInfo> result;
    std::error_code ec;
    for (const auto& repo : fs::directory_iterator(patches_dir, ec)) {
        if (!repo.is_directory(ec)) {
            continue;
        }
        std::ifstream index(repo.path() / "files.json");
        if (!index) {
            continue;
        }
        const auto json = nlohmann::json::parse(index, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            continue;
        }
        for (const auto& [file, serials] : json.items()) {
            if (!serials.is_array()) {
                continue;
            }
            const bool match = std::any_of(serials.begin(), serials.end(), [&](const auto& id) {
                return id.is_string() && title_ids.count(id.template get<std::string>()) > 0;
            });
            const fs::path xml = repo.path() / file;
            if (match && fs::is_regular_file(xml, ec)) {
                result.push_back({xml, file, repo.path().filename().string()});
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.repo, a.file) < std::tie(b.repo, b.file);
    });
    return result;
}

std::vector<PatchEntry> ReadPatchEntries(const fs::path& xml_path) {
    std::vector<PatchEntry> entries;
    pugi::xml_document doc;
    if (!doc.load_file(xml_path.c_str())) {
        return entries;
    }
    for (const pugi::xml_node meta : doc.child("Patch").children("Metadata")) {
        PatchEntry entry;
        entry.name = meta.attribute("Name").as_string();
        entry.note = meta.attribute("Note").as_string();
        entry.enabled = std::string_view(meta.attribute("isEnabled").as_string()) == "true";
        entry.locked = std::string_view(meta.attribute("Always").as_string()) == "true";
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool SavePatchEntries(const fs::path& xml_path, const std::vector<PatchEntry>& entries,
                      std::string& error) {
    pugi::xml_document doc;
    if (!doc.load_file(xml_path.c_str(), pugi::parse_full)) {
        error = "Cannot read " + xml_path.string();
        return false;
    }
    size_t index = 0;
    for (pugi::xml_node meta : doc.child("Patch").children("Metadata")) {
        if (index >= entries.size()) {
            break;
        }
        if (!entries[index].locked) {
            pugi::xml_attribute attribute = meta.attribute("isEnabled");
            if (!attribute) {
                attribute = meta.append_attribute("isEnabled");
            }
            attribute.set_value(entries[index].enabled ? "true" : "false");
        }
        ++index;
    }
    const fs::path temp = xml_path.string() + ".tmp";
    if (!doc.save_file(temp.c_str(), "    ")) {
        error = "Cannot write " + temp.string();
        return false;
    }
    std::error_code ec;
    fs::rename(temp, xml_path, ec);
    if (ec) {
        error = "Cannot replace " + xml_path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool DeletePatchFile(const PatchFileInfo& info, std::string& error) {
    std::error_code ec;
    fs::remove(info.path, ec);
    if (ec) {
        error = "Cannot delete " + info.path.string() + ": " + ec.message();
        return false;
    }
    const fs::path index_path = info.path.parent_path() / "files.json";
    nlohmann::json json;
    {
        std::ifstream in(index_path);
        if (in) {
            json = nlohmann::json::parse(in, nullptr, false);
        }
    }
    if (json.is_object()) {
        json.erase(info.file);
        std::ofstream out(index_path);
        out << json.dump(2) << "\n";
    }
    return true;
}

// Patches shipped inside the AppImage live in <APPDIR>/usr/share/driveclub-pc/patches.
fs::path BundledPatchesDir() {
    if (const char* custom = std::getenv("DCPC_BUNDLED_PATCHES"); custom != nullptr && *custom) {
        return fs::path(custom);
    }
    if (const char* appdir = std::getenv("APPDIR"); appdir != nullptr && *appdir) {
        return fs::path(appdir) / "usr" / "share" / "driveclub-pc" / "patches";
    }
    return {};
}

// Copies the bundled patch files into the user's patches folder once. A file is never overwritten
// (the user may have switched patches on or off in it) and a file the user deleted does not come
// back: what was already copied is remembered in .dcpc-seeded.json.
void SeedBundledPatches(const fs::path& bundled, const fs::path& patches_dir) {
    std::error_code ec;
    if (bundled.empty() || !fs::is_directory(bundled, ec)) {
        return;
    }
    fs::create_directories(patches_dir, ec);
    const fs::path seeded_path = patches_dir / ".dcpc-seeded.json";
    std::set<std::string> seeded;
    {
        std::ifstream in(seeded_path);
        const auto json = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
        if (json.is_array()) {
            for (const auto& item : json) {
                if (item.is_string()) {
                    seeded.insert(item.get<std::string>());
                }
            }
        }
    }
    bool seeded_changed = false;
    for (const auto& repo : fs::directory_iterator(bundled, ec)) {
        if (!repo.is_directory(ec)) {
            continue;
        }
        const std::string repo_name = repo.path().filename().string();
        nlohmann::json bundled_index = nlohmann::json::object();
        {
            std::ifstream in(repo.path() / "files.json");
            const auto json = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
            if (json.is_object()) {
                bundled_index = json;
            }
        }
        const fs::path dest = patches_dir / repo.path().filename();
        const fs::path dest_index_path = dest / "files.json";
        nlohmann::json dest_index = nlohmann::json::object();
        {
            std::ifstream in(dest_index_path);
            const auto json = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
            if (json.is_object()) {
                dest_index = json;
            }
        }
        bool index_changed = false;
        for (const auto& item : fs::directory_iterator(repo.path(), ec)) {
            const std::string file = item.path().filename().string();
            const std::string key = repo_name + "/" + file;
            if (!item.is_regular_file(ec) || file == "files.json" || seeded.count(key) > 0) {
                continue;
            }
            fs::create_directories(dest, ec);
            if (!fs::exists(dest / file, ec)) {
                fs::copy_file(item.path(), dest / file, ec);
            }
            if (bundled_index.contains(file) && !dest_index.contains(file)) {
                dest_index[file] = bundled_index[file];
                index_changed = true;
            }
            seeded.insert(key);
            seeded_changed = true;
        }
        if (index_changed) {
            std::ofstream out(dest_index_path);
            out << dest_index.dump(2) << "\n";
        }
    }
    if (seeded_changed) {
        std::ofstream out(seeded_path);
        out << nlohmann::json(seeded).dump(2) << "\n";
    }
}

struct PatchPanelState {
    fs::path dir;
    std::set<std::string> title_ids;
    std::vector<PatchFileInfo> files;
    int selected = -1;
    std::vector<PatchEntry> entries;
    bool dirty = false;
    bool confirm_delete = false;
    std::string message;

    void Select(int index) {
        selected = index;
        dirty = false;
        confirm_delete = false;
        entries = (index >= 0 && index < static_cast<int>(files.size()))
                      ? ReadPatchEntries(files[static_cast<size_t>(index)].path)
                      : std::vector<PatchEntry>{};
    }

    void Refresh() {
        const fs::path previous = (selected >= 0 && selected < static_cast<int>(files.size()))
                                      ? files[static_cast<size_t>(selected)].path
                                      : fs::path();
        files = FindPatchFiles(dir, title_ids);
        int index = files.empty() ? -1 : 0;
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i].path == previous) {
                index = static_cast<int>(i);
            }
        }
        Select(index);
    }
};

// Draws the patches panel; call it inside a child window that has the room for it.
void DrawPatchPanel(PatchPanelState& state) {
    ImGui::TextUnformatted("Game patches");
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("This build already has its own 60 FPS, widescreen and render resolution "
                        "options. Patches that do the same can conflict with them.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    ImGui::TextUnformatted("Patch file");
    if (ImGui::BeginListBox("##patchfiles",
                            ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 4.5f))) {
        for (int i = 0; i < static_cast<int>(state.files.size()); ++i) {
            const auto& info = state.files[static_cast<size_t>(i)];
            const std::string label = info.file + " | " + info.repo;
            if (ImGui::Selectable(label.c_str(), i == state.selected) && i != state.selected) {
                state.Select(i);
            }
        }
        ImGui::EndListBox();
    }
    if (state.files.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No patch files found in %s. Put a patch folder there (a files.json "
                            "and the XML files), or use the patches bundled with the AppImage.",
                            state.dir.string().c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();

    const float below = ImGui::GetFrameHeightWithSpacing() * 3.0f;
    ImGui::BeginChild("##patchentries", ImVec2(0.0f, -below), true);
    for (size_t i = 0; i < state.entries.size(); ++i) {
        PatchEntry& entry = state.entries[i];
        ImGui::BeginDisabled(entry.locked);
        const std::string label = entry.name + "##patch" + std::to_string(i);
        if (ImGui::Checkbox(label.c_str(), &entry.enabled)) {
            state.dirty = true;
            state.message.clear();
        }
        ImGui::EndDisabled();
        if (!entry.note.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(entry.note.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    ImGui::EndChild();

    const bool have_file = state.selected >= 0 && state.selected < static_cast<int>(state.files.size());
    ImGui::BeginDisabled(!state.dirty || !have_file);
    if (ImGui::Button("Save")) {
        std::string error;
        if (SavePatchEntries(state.files[static_cast<size_t>(state.selected)].path, state.entries,
                             error)) {
            state.dirty = false;
            state.message = "Saved.";
        } else {
            state.message = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.dirty || !have_file);
    if (ImGui::Button("Revert")) {
        state.Select(state.selected);
        state.message.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_file);
    if (ImGui::Button(state.confirm_delete ? "Really delete?" : "Delete file")) {
        if (state.confirm_delete) {
            std::string error;
            const bool deleted = DeletePatchFile(state.files[static_cast<size_t>(state.selected)], error);
            state.message = deleted ? "File deleted." : error;
            state.Refresh();
        } else {
            state.confirm_delete = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        state.Refresh();
        state.message.clear();
    }
    if (!state.message.empty()) {
        ImGui::TextWrapped("%s", state.message.c_str());
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Folder: %s", state.dir.string().c_str());
    ImGui::PopTextWrapPos();
}

} // namespace

static bool ShowWindowImGui(Settings& s) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        ShowError(fmt::format("Cannot open the settings window ({}).\nEdit user/driveclub_pc.ini "
                              "instead.",
                              SDL_GetError()));
        return false;
    }
    int window_w = 1240; // settings on the left, game patches on the right
    int window_h = 940;
    {
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 &&
            usable.h > 0) {
            window_w = std::min(window_w, std::max(480, usable.w - 40));
            window_h = std::min(window_h, std::max(420, usable.h - 90));
        }
    }
    SDL_InitSubSystem(SDL_INIT_GAMEPAD); // optional: lets a controller drive the window
    SDL_Window* window =
        SDL_CreateWindow("Driveclub-PC", window_w, window_h, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (window == nullptr || renderer == nullptr) {
        const std::string reason = SDL_GetError();
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        ShowError(fmt::format("Cannot open the settings window ({}).\nEdit user/driveclub_pc.ini "
                              "instead.",
                              reason));
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini in the working directory
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    io.FontDefault = ImGui::FontStack::AddPrimaryUiFont(
        io.Fonts, 32.0f, EmulatorSettings.GetConsoleLanguage(), font_cfg, false);
    io.FontGlobalScale = 0.5f;
    const auto max_dim = SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                                               SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 8192);
    const int atlas_max = static_cast<int>(std::bit_floor(std::max<u64>(max_dim, 512)));
    io.Fonts->TexMaxWidth = atlas_max;
    io.Fonts->TexMaxHeight = atlas_max;
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.WindowRounding = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 9.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // ---- state -------------------------------------------------------------------------------
    static FolderPick pick; // static: the SDL dialog may call back after this function returns
    int pick_target = 0;    // 0 = game folder, 1 = firmware folder
    bool pick_pending = false;

    char game_buf[2048]{};
    std::snprintf(game_buf, sizeof(game_buf), "%s", s.game_dir.c_str());
    char save_buf[2048]{}; // optional save data root ("" = the emulator's own folder)
    std::snprintf(save_buf, sizeof(save_buf), "%s", s.save_dir.c_str());
    bool game_ok = false;
    bool firmware_ok = false;
    std::string game_msg;
    std::string fw_msg;
    std::string notice;

    PatchPanelState patches;
    patches.dir = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir);
    std::string current_title_id(TitleId); // the region of the chosen game
    const auto refresh = [&] {
        const std::string dir = game_buf;
        current_title_id = std::string(TitleId);
        if (dir.empty()) {
            game_ok = false;
            game_msg = "Choose a Driveclub base game folder. Tested: CUSA00093 v1.28.";
        } else {
            std::string error;
            const auto eboot = ResolveAndValidateGame(TextPath(dir), error);
            game_ok = eboot.has_value();
            current_title_id = ReadGameTitleId(TextPath(dir)).value_or(current_title_id);
            game_msg = game_ok ? GameCompatibilityNote(current_title_id) : error;
        }
        fw_msg = FirmwareStatus(firmware_ok, current_title_id);
        patches.title_ids = {current_title_id};
        patches.Refresh();
        std::replace(game_msg.begin(), game_msg.end(), '\n', ' ');
        std::replace(fw_msg.begin(), fw_msg.end(), '\n', ' ');
    };
    refresh();

    const auto [display_w, display_h] = DisplaySize();
    const std::vector<std::pair<int, int>> resolutions = {
        {0, 0},       {5120, 1440}, {3840, 2160}, {3440, 1440}, {2560, 1600}, {2560, 1440},
        {2560, 1080}, {1920, 1200}, {1920, 1080}, {1600, 900},  {1280, 800}, {1280, 720}};
    std::vector<std::string> res_items;
    int res_i = 0;
    for (size_t i = 0; i < resolutions.size(); ++i) {
        const auto [w, h] = resolutions[i];
        res_items.push_back(i == 0 ? fmt::format("Same as the display ({}x{})", display_w,
                                                 display_h)
                                   : fmt::format("{}x{}", w, h));
        if (w == s.width && h == s.height) {
            res_i = static_cast<int>(i);
        }
    }
    const std::vector<std::string> fps_items = {"30 FPS (as on PS4)", "60 FPS (recommended)",
                                                "60 FPS + FG (2x, ~120 output)",
                                                "120 FPS (experimental)"};
    const std::vector<std::string> display_items = {"Fullscreen (borderless)", "Window"};
    const std::vector<std::string> render_items = {"1080p (native, fastest)",
                                                   "1440p (sharper, about 10-15% slower)",
                                                   "2160p (very demanding)"};
    const std::vector<std::string> upscale_items = {"AMD FSR 1 (game image to window size)",
                                                    "Bilinear"};
    const std::vector<std::string> sharp_items = {"Off", "Normal (RCAS)", "Strong (RCAS)"};
    const std::vector<std::string> af_items = {"Off (game setting)", "2x", "4x", "8x", "16x"};
    const std::vector<std::string> aa_items = {"Off", "FXAA (default)", "SMAA 1x (High)"};
    const std::vector<std::string> rb_items = {"Precise (default)",
                                               "Relaxed (faster, shadow/vegetation glitches)",
                                               "Off (fastest, effects may break)"};

    int fps_i = s.target_fps == 30 ? 0 : s.target_fps == 120 ? 3 : s.frame_generation ? 2 : 1;
    int display_i = s.fullscreen ? 0 : 1;
    int render_i = s.render_res == 2160 ? 2 : s.render_res == 1440 ? 1 : 0;
    int upscale_i = s.fsr ? 0 : 1;
    int sharp_i = std::clamp(s.sharpening, 0, 2);
    int af_i = s.af == 16 ? 4 : s.af == 8 ? 3 : s.af == 4 ? 2 : s.af == 2 ? 1 : 0;
    int aa_i = std::clamp(s.aa, 0, 2);
    int rb_i = 2 - std::clamp(s.readbacks, 0, 2);
    bool pcore = s.pcore_pin;
    bool async_shaders = s.async_shaders;
    bool show_fps = s.show_fps;
    bool deferred = s.deferred_recording;
    bool classic = s.classic_vblank;
    bool show_launcher = s.show_launcher;

    constexpr float label_w = 170.0f;
    const ImVec4 green(0.35f, 0.80f, 0.45f, 1.0f);
    const ImVec4 red(0.95f, 0.40f, 0.40f, 1.0f);

    const auto combo = [&](const char* label, const char* id, int& index,
                           const std::vector<std::string>& items) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, items[static_cast<size_t>(index)].c_str())) {
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const bool selected = i == index;
                if (ImGui::Selectable(items[static_cast<size_t>(i)].c_str(), selected)) {
                    index = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    };
    const auto check = [&](const char* label, bool& value) {
        ImGui::SetCursorPosX(label_w);
        ImGui::Checkbox(label, &value);
    };
    const auto status = [&](const std::string& message, bool ok) {
        ImGui::Indent(label_w);
        ImGui::PushStyleColor(ImGuiCol_Text, ok ? green : red);
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent(label_w);
    };
    const auto start_pick = [&](int target) {
        pick_target = target;
        pick_pending = true;
        {
            std::lock_guard lock{pick.mutex};
            pick.finished = false;
            pick.has_path = false;
            pick.path.clear();
        }
        SDL_ShowOpenFolderDialog(OnFolderPicked, &pick, window, nullptr, false);
    };

    // ---- main loop ---------------------------------------------------------------------------
    bool done = false;
    bool play = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
        }

        if (pick_pending) {
            bool finished = false;
            bool has_path = false;
            std::string picked;
            {
                std::lock_guard lock{pick.mutex};
                finished = pick.finished;
                has_path = pick.has_path;
                picked = pick.path;
            }
            if (finished) {
                pick_pending = false;
                if (has_path) {
                    if (pick_target == 0) {
                        std::snprintf(game_buf, sizeof(game_buf), "%s", picked.c_str());
                        notice.clear();
                    } else if (pick_target == 2) {
                        std::snprintf(save_buf, sizeof(save_buf), "%s", picked.c_str());
                    } else {
                        notice = ImportFirmwareFrom(picked);
                    }
                    refresh();
                }
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Driveclub-PC", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);

        ImGui::TextUnformatted("Driveclub-PC settings");
        ImGui::Separator();
        const float footer_h = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f +
                               ImGui::GetStyle().WindowPadding.y;
        const float left_w = std::max(480.0f, ImGui::GetContentRegionAvail().x * 0.54f);
        ImGui::BeginChild("##settings", ImVec2(left_w, -footer_h), false);
        ImGui::Spacing();

        // Game folder
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Game folder");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-110.0f);
        ImGui::InputText("##game", game_buf, sizeof(game_buf));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            refresh();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pick_pending);
        if (ImGui::Button("Browse...##game")) {
            start_pick(0);
        }
        ImGui::EndDisabled();
        status(game_msg, game_ok);
        ImGui::Spacing();

        // Save data folder (optional)
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Save folder");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-110.0f);
        ImGui::InputText("##save", save_buf, sizeof(save_buf));
        ImGui::SameLine();
        ImGui::BeginDisabled(pick_pending);
        if (ImGui::Button("Browse...##save")) {
            start_pick(2);
        }
        ImGui::EndDisabled();
        {
            Settings preview;
            preview.save_dir = save_buf;
            ImGui::Indent(label_w);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Saves go to: %s", PathText(SaveRoot(preview)).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent(label_w);
        }
        ImGui::Spacing();

        // Firmware
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Firmware");
        ImGui::SameLine(label_w);
        ImGui::BeginDisabled(pick_pending);
        if (ImGui::Button("Import...##fw")) {
            start_pick(1);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Details##fw")) {
            notice = InspectSysModules(current_title_id).details;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(folder with your PS4 *.sprx modules)");
        status(fw_msg, firmware_ok);
        if (!notice.empty()) {
            ImGui::Indent(label_w);
            ImGui::TextWrapped("%s", notice.c_str());
            ImGui::Unindent(label_w);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        combo("Frame rate", "##fps", fps_i, fps_items);
        combo("Display", "##display", display_i, display_items);
        ImGui::BeginDisabled(display_i == 0); // the window size only applies in windowed mode
        combo("Window size", "##resolution", res_i, res_items);
        ImGui::EndDisabled();
        combo("Render resolution", "##render", render_i, render_items);
        combo("Upscaling", "##upscale", upscale_i, upscale_items);
        combo("Sharpening", "##sharp", sharp_i, sharp_items);
        combo("Anisotropic filter", "##af", af_i, af_items);
        combo("Anti-aliasing", "##aa", aa_i, aa_items);
        combo("GPU readbacks", "##readbacks", rb_i, rb_items);
        ImGui::Spacing();

        check("Prefer performance cores (hybrid CPUs)", pcore);
        check("Compile shaders in the background (less stutter)", async_shaders);
        check("Show FPS counter", show_fps);
        check("Experimental: record Vulkan commands on a second thread", deferred);
        check("Classic PS4 frame pacing (fallback if timing looks wrong)", classic);
        check("Show this window at start", show_launcher);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##patches", ImVec2(0.0f, -footer_h), false);
        DrawPatchPanel(patches);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::SetCursorPosX(label_w);
        ImGui::BeginDisabled(!(game_ok && firmware_ok));
        if (ImGui::Button("Play", ImVec2(120.0f, 0.0f))) {
            if (patches.dirty && patches.selected >= 0 &&
                patches.selected < static_cast<int>(patches.files.size())) {
                std::string patch_error; // unsaved patch choices are saved when the game starts
                SavePatchEntries(patches.files[static_cast<size_t>(patches.selected)].path,
                                 patches.entries, patch_error);
            }
            play = true;
            done = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(120.0f, 0.0f))) {
            done = true;
        }

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // ---- collect the choices (also when the window is closed, so the ini keeps them) --------
    s.game_dir = game_buf;
    static constexpr int fps_values[] = {30, 60, 60, 120};
    const int frame_mode = std::clamp(fps_i, 0, 3);
    s.target_fps = fps_values[frame_mode];
    s.frame_generation = frame_mode == 2;
    s.save_dir = save_buf;
    s.fullscreen = display_i == 0;
    const auto [chosen_w, chosen_h] = resolutions[static_cast<size_t>(res_i)];
    s.width = chosen_w;
    s.height = chosen_h;
    s.fsr = upscale_i == 0;
    s.sharpening = std::clamp(sharp_i, 0, 2);
    s.readbacks = 2 - std::clamp(rb_i, 0, 2);
    static constexpr int af_values[] = {0, 2, 4, 8, 16};
    s.af = af_values[std::clamp(af_i, 0, 4)];
    s.aa = std::clamp(aa_i, 0, 2);
    static constexpr int render_values[] = {1080, 1440, 2160};
    s.render_res = render_values[std::clamp(render_i, 0, 2)];
    s.pcore_pin = pcore;
    s.async_shaders = async_shaders;
    s.show_fps = show_fps;
    s.deferred_recording = deferred;
    s.classic_vblank = classic;
    s.show_launcher = show_launcher;

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return play;
}

#endif // !_WIN32

std::optional<fs::path> Run() {
    Settings s = Load();
    std::string error;
    const bool game_ok = !s.game_dir.empty() && ResolveAndValidateGame(TextPath(s.game_dir), error);
    bool firmware_ok = false;
    const auto title_id = ReadGameTitleId(TextPath(s.game_dir)).value_or(std::string(TitleId));
    FirmwareStatus(firmware_ok, title_id);
#ifdef _WIN32
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (s.show_launcher || shift || !game_ok || !firmware_ok) {
        const bool play = ShowWindow(s);
        Save(s);
        if (!play) {
            return std::nullopt;
        }
    }
#else
    // Patches shipped inside the AppImage are copied to the user's patches folder once.
    SeedBundledPatches(BundledPatchesDir(),
                       Common::FS::GetUserPath(Common::FS::PathType::PatchesDir));
    // driveclub-pc (linux): DCPC_LAUNCHER=1 opens the window even when it is turned off.
    if (s.show_launcher || !game_ok || !firmware_ok || std::getenv("DCPC_LAUNCHER") != nullptr) {
        const bool play = ShowWindowImGui(s);
        Save(s);
        if (!play) {
            return std::nullopt;
        }
    }
#endif
    if (!PrepareSaveRoot(s, error)) {
        ShowError(error);
        return std::nullopt;
    }
    WriteGameConfig(s);
    ApplyRuntime(s);
    const auto out = TargetOutput(s);
    LOG_INFO(Loader, "Launcher: {} FPS, {} {}x{}, {}, sharpening {}, output {}x{}{}", s.target_fps,
             s.fullscreen ? "fullscreen" : "window", s.width, s.height,
             s.fsr ? "FSR 1" : "bilinear", s.sharpening, out.width, out.height,
             out.wide ? " (widescreen)" : "");
    return TextPath(s.game_dir);
}

bool ApplySavedRuntimeOptions(bool check_only) {
    std::error_code ec;
    if (fs::exists(IniPath(), ec)) {
        const auto settings = Load();
        std::string error;
        if (!PrepareSaveRoot(settings, error)) {
            if (check_only) {
                fmt::print(stderr, "{}\n", error);
            } else {
                ShowError(error);
            }
            return false;
        }
        ApplyRuntime(settings);
    }
    return true;
}

} // namespace DriveclubPC::Launcher
