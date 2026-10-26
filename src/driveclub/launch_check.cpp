// SPDX-FileCopyrightText: Copyright 2026 Driveclub-PC contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>

#include <SDL3/SDL_messagebox.h>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "driveclub/launch_check.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace DriveclubPC {

namespace {

std::optional<std::string> ReadSfoString(const std::filesystem::path& folder,
                                         std::string_view key) {
    // driveclub-pc: the upstream PSF parser assumes complete input; check folder selections first.
    const auto path = folder / "sce_sys" / "param.sfo";
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < sizeof(PSFHeader) || size > 1024 * 1024) {
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input) {
        return std::nullopt;
    }
    PSFHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    const size_t entries = header.index_table_entries;
    const size_t keys = header.key_table_offset;
    const size_t data = header.data_table_offset;
    if (entries > (size - sizeof(header)) / sizeof(PSFRawEntry) ||
        keys < sizeof(header) + entries * sizeof(PSFRawEntry) || keys > data || data > size) {
        return std::nullopt;
    }
    for (size_t i = 0; i < entries; ++i) {
        PSFRawEntry entry;
        std::memcpy(&entry, bytes.data() + sizeof(header) + i * sizeof(entry), sizeof(entry));
        const size_t key_offset = keys + static_cast<size_t>(entry.key_offset);
        const size_t value_offset = data + static_cast<size_t>(entry.data_offset);
        const size_t length = entry.param_len;
        const auto format = static_cast<PSFEntryFmt>(entry.param_fmt.Raw());
        if (key_offset >= data ||
            std::memchr(bytes.data() + key_offset, 0, data - key_offset) == nullptr ||
            value_offset > size || length > size - value_offset ||
            (format != PSFEntryFmt::Text && format != PSFEntryFmt::Integer &&
             format != PSFEntryFmt::Binary) ||
            (format == PSFEntryFmt::Integer && length != sizeof(s32)) ||
            (format == PSFEntryFmt::Text &&
             (length == 0 || bytes[value_offset + length - 1] != 0))) {
            return std::nullopt;
        }
    }
    PSF psf;
    if (!psf.Open(bytes)) {
        return std::nullopt;
    }
    if (const auto value = psf.GetString(key)) {
        return std::string(*value);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> ReadGameTitleId(const std::filesystem::path& eboot_or_folder) {
    auto folder = eboot_or_folder;
    std::error_code ec;
    if (std::filesystem::is_regular_file(folder, ec)) {
        folder = folder.parent_path();
    }
    if (const auto base = Core::FileSys::BaseGameFromOverlay(folder)) {
        folder = *base;
    }
    return ReadSfoString(folder, "TITLE_ID");
}

std::string GameCompatibilityNote(std::string_view title_id) {
    return title_id == TitleId
               ? fmt::format("Driveclub {}. Tested with update 1.28.", title_id)
               : fmt::format("Driveclub {}: experimental region. Tested only with {} v1.28.",
                             title_id, TitleId);
}

std::optional<std::filesystem::path> ResolveAndValidateGame(const std::filesystem::path& arg,
                                                            std::string& error) {
    std::error_code ec;
    std::filesystem::path folder = arg;
    if (std::filesystem::is_regular_file(folder, ec)) {
        folder = folder.parent_path();
    }
    if (!std::filesystem::is_directory(folder, ec)) {
        error = fmt::format("The game folder does not exist:\n{}", arg.string());
        return std::nullopt;
    }
    if (const auto base = Core::FileSys::BaseGameFromOverlay(folder)) {
        folder = *base;
    }
    const auto eboot = folder / "eboot.bin";
    if (!std::filesystem::is_regular_file(eboot, ec)) {
        error = fmt::format("No eboot.bin in\n{}\n\nSelect the Driveclub base game folder ({}).",
                            folder.string(), TitleId);
        return std::nullopt;
    }
    const auto title_id = ReadSfoString(folder, "TITLE_ID");
    if (!title_id) {
        error = fmt::format("sce_sys/param.sfo is missing or unreadable in\n{}", folder.string());
        return std::nullopt;
    }
    // driveclub-pc: allow known regions and other Driveclub editions identified by their SFO.
    auto title = ReadSfoString(folder, "TITLE").value_or("");
    std::transform(title.begin(), title.end(), title.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool valid_id = title_id->size() == 9 && title_id->starts_with("CUSA") &&
                          std::all_of(title_id->begin() + 4, title_id->end(),
                                      [](unsigned char c) { return c >= '0' && c <= '9'; });
    const bool known_region =
        *title_id == TitleId || *title_id == "CUSA00003" || *title_id == "CUSA00879";
    if (!valid_id || (!known_region && (title.find("driveclub") == std::string::npos ||
                                        title.find("bikes") != std::string::npos))) {
        error = fmt::format("This folder holds {}, not a supported Driveclub base game:\n{}",
                            *title_id, folder.string());
        return std::nullopt;
    }

    std::optional<std::filesystem::path> update;
    for (const auto suffix : {"-UPDATE", "-patch"}) {
        const auto candidate = Core::FileSys::OverlayPath(folder, suffix);
        if (std::filesystem::is_directory(candidate, ec)) {
            const auto update_id = ReadSfoString(candidate, "TITLE_ID");
            if (!update_id || *update_id != *title_id) {
                error =
                    fmt::format("The update region ({}) does not match the base game ({}):\n{}",
                                update_id.value_or("unreadable"), *title_id, candidate.string());
                return std::nullopt;
            }
            update = candidate;
            break;
        }
    }
    const auto base_version = ReadSfoString(folder, "APP_VER").value_or("?");
    const auto update_version =
        update ? ReadSfoString(*update, "APP_VER").value_or("?") : std::string("none");
    LOG_INFO(Loader, "{}: {} base {} (APP_VER {}), update {} (APP_VER {})", AppName, *title_id,
             folder.string(), base_version, update ? update->string() : "not found",
             update_version);
    if (*title_id != TitleId) {
        LOG_WARNING(Loader, "{}", GameCompatibilityNote(*title_id));
    }
    // The installed version is the update's when there is an update folder, otherwise the base
    // game's (a dump that already is 1.28 needs no separate update folder).
    const auto installed_version = update ? update_version : base_version;
    if (installed_version != TargetAppVersion) {
        LOG_WARNING(Loader,
                    "{}: tested with update {}; this installation runs {}. Expect differences.",
                    AppName, TargetAppVersion, installed_version);
    }
    return eboot;
}

FirmwareReport InspectSysModules(std::string_view title_id) {
    // driveclub-pc: modules observed in successful v1.28 races, cross-checked against
    // sysmodule_internal.cpp. Keep the existing libc requirement; the other required modules
    // have no HLE fallback. The five optional modules do have one.
    struct Module {
        const char* name;
        bool required;
    };
    constexpr Module modules[] = {{"libSceLibcInternal.sprx", true}, {"libSceJpegDec.sprx", true},
                                  {"libSceFreeTypeOt.sprx", true},   {"libSceAudiodec.sprx", true},
                                  {"libSceRtc.sprx", false},         {"libScePngDec.sprx", false},
                                  {"libSceFont.sprx", false},        {"libSceFontFt.sprx", false},
                                  {"libSceAvPlayer.sprx", false}};
    const auto dir = EmulatorSettings.GetSysModulesDir();
    FirmwareReport report;
    std::string missing, invalid, fallback;
    int found = 0;
    for (const auto& module : modules) {
        auto path = dir / module.name;
        std::error_code ec;
        const auto regional = dir / title_id / module.name;
        // libc is loaded by the linker from the global directory, not a regional override.
        if (std::string_view(module.name) != "libSceLibcInternal.sprx" &&
            std::filesystem::exists(regional, ec)) {
            path = regional;
        }
        if (!std::filesystem::exists(path, ec)) {
            (module.required ? missing : fallback) += fmt::format("\n  {}", module.name);
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        std::array<unsigned char, 64> header{};
        in.read(reinterpret_cast<char*>(header.data()), header.size());
        const bool elf = header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' &&
                         header[3] == 'F' && header[4] == 2 && header[5] == 1;
        const bool self =
            header[0] == 0x4f && header[1] == 0x15 && header[2] == 0x3d && header[3] == 0x1d;
        if (!in || (!elf && !self)) {
            invalid += fmt::format("\n  {}", module.name);
        } else {
            ++found;
        }
    }
    report.ready = missing.empty() && invalid.empty();
    report.summary = report.ready ? fmt::format("Firmware ready: {}/9 modules found{}.", found,
                                                fallback.empty() ? "" : "; HLE fallbacks in use")
                                  : "Firmware incomplete or unreadable. See Details.";
    report.details = fmt::format("Firmware directory:\n{}\nRegion: {}\n", dir.string(), title_id);
    if (!missing.empty()) {
        report.details += "\nMissing required modules:" + missing + "\n";
    }
    if (!invalid.empty()) {
        report.details += "\nUnreadable files or invalid ELF/SELF headers:" + invalid + "\n";
    }
    if (!fallback.empty()) {
        report.details += "\nNot present (emulator HLE fallback available):" + fallback + "\n";
    }
    report.details += "\nUse decrypted modules from your own PS4 firmware dump. Header checks do "
                      "not verify decryption, firmware version or full file integrity.";
    return report;
}

bool ValidateSysModules(std::string& error, std::string_view title_id) {
    const auto report = InspectSysModules(title_id);
    error = report.ready ? std::string{} : report.details;
    return report.ready;
}

void EnsureDefaultGameConfig(std::string_view title_id) {
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
                      fmt::format("{}.json", title_id);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    // Borderless fullscreen at the desktop resolution of the primary display; the game renders at
    // its native 1920x1080 and the presenter scales it with FSR 1 + RCAS.
    int width = 1920;
    int height = 1080;
#ifdef _WIN32
    // Physical pixels of the primary display (GetSystemMetrics would be DPI-scaled here).
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth > 0 &&
        mode.dmPelsHeight > 0) {
        width = static_cast<int>(mode.dmPelsWidth);
        height = static_cast<int>(mode.dmPelsHeight);
    }
#endif
    std::ofstream out(path);
    out << fmt::format("{{\n  \"GPU\": {{\n"
                       "    \"readbacks_mode\": 2,\n"
                       "    \"readback_offload\": true,\n"
                       "    \"fsr_enabled\": true,\n"
                       "    \"rcas_enabled\": true,\n"
                       "    \"full_screen\": true,\n"
                       "    \"full_screen_mode\": \"Fullscreen (Borderless)\",\n"
                       "    \"window_width\": {},\n"
                       "    \"window_height\": {}\n"
                       "  }}\n}}\n",
                       width, height);
    LOG_INFO(Loader, "{}: wrote default Driveclub settings to {} (borderless {}x{}, FSR 1)",
             AppName, path.string(), width, height);
}

void ShowError(std::string_view message) {
    LOG_CRITICAL(Loader, "{}", message);
    Common::Log::Flush();
    fmt::print(stderr, "{}: {}\n", AppName, message);
    std::fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, std::string(AppName).c_str(),
                             std::string(message).c_str(), nullptr);
}

std::string UsageText() {
    return fmt::format("{} runs the PS4 version of Driveclub ({}, update {}).\n\n"
                       "Usage: {}.exe <Driveclub base game folder>\n\n"
                       "The update must sit next to it as <folder>-UPDATE or <folder>-patch.\n"
                       "Other Driveclub regions are experimental. Only {} v1.28 was tested.\n"
                       "Settings are kept in 'user'; the launcher can select a custom save root.",
                       AppName, TitleId, TargetAppVersion, AppName, TitleId);
}

} // namespace DriveclubPC
