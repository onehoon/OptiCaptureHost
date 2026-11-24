#pragma once

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace oc
{

struct Hotkey
{
    UINT mods = 0;
    UINT vk = 0;

    bool operator==(const Hotkey& other) const { return mods == other.mods && vk == other.vk; }
    bool operator!=(const Hotkey& other) const { return !(*this == other); }
};

struct Settings
{
    // 0 = leave the target window unchanged; 1..3 = 67%, 58%, and 50% of the monitor size.
    int targetScale = 0;
    Hotkey stop{ MOD_CONTROL | MOD_SHIFT, VK_F12 };
    Hotkey focus{ MOD_CONTROL | MOD_SHIFT, VK_F11 };
    Hotkey menu{ MOD_CONTROL | MOD_SHIFT, VK_F10 };
    Hotkey launcher{ MOD_CONTROL | MOD_SHIFT, VK_F9 };
};

inline std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value = path;
    const size_t cut = value.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : value.substr(0, cut + 1);
}

inline std::wstring SettingsPath()
{
    return ExeDir() + L"OptiCaptureHost.ini";
}

inline std::wstring StatusPath()
{
    return ExeDir() + L"OptiCaptureHost.status";
}

inline Hotkey ParseHotkey(const std::wstring& text, Hotkey fallback)
{
    wchar_t* end = nullptr;
    const unsigned long mods = wcstoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L',')
        return fallback;
    const unsigned long vk = wcstoul(end + 1, &end, 10);
    if (vk == 0 || vk > 254)
        return fallback;

    Hotkey result;
    result.mods = static_cast<UINT>(mods) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    result.vk = static_cast<UINT>(vk);
    return result;
}

inline std::wstring HotkeyToString(const Hotkey& hotkey)
{
    return std::to_wstring(hotkey.mods) + L"," + std::to_wstring(hotkey.vk);
}

inline void LoadSettings(Settings& settings)
{
    settings = Settings{};
    const std::wstring path = SettingsPath();
    settings.targetScale = GetPrivateProfileIntW(L"Capture", L"TargetScale", 0, path.c_str());
    if (settings.targetScale < 0 || settings.targetScale > 3)
        settings.targetScale = 0;

    auto readHotkey = [&](const wchar_t* key, const Hotkey& fallback) {
        wchar_t value[64] = {};
        GetPrivateProfileStringW(L"Shortcuts", key, L"", value, static_cast<DWORD>(std::size(value)), path.c_str());
        return value[0] ? ParseHotkey(value, fallback) : fallback;
    };
    settings.stop = readHotkey(L"Stop", settings.stop);
    settings.focus = readHotkey(L"Focus", settings.focus);
    settings.menu = readHotkey(L"Menu", settings.menu);
    settings.launcher = readHotkey(L"Launcher", settings.launcher);
}

inline void SaveSettings(const Settings& settings)
{
    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(L"Capture", L"TargetScale", std::to_wstring(settings.targetScale).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Stop", HotkeyToString(settings.stop).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Focus", HotkeyToString(settings.focus).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Menu", HotkeyToString(settings.menu).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Launcher", HotkeyToString(settings.launcher).c_str(), path.c_str());
}

struct LiveStats
{
    unsigned long long stamp = 0;
    unsigned pid = 0;
    double captureFps = 0.0;
    double presentFps = 0.0;
    double presentMs = 0.0;
    double waitMs = 0.0;
    int focus = 2;
};

inline void WriteLiveStats(const LiveStats& stats)
{
    const std::wstring path = StatusPath();
    const std::wstring temporary = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file)
        return;
    std::fprintf(file, "stamp=%llu\npid=%u\ncapture=%.3f\npresent=%.3f\npresentms=%.3f\nwaitms=%.3f\nfocus=%d\n", stats.stamp,
                 stats.pid, stats.captureFps, stats.presentFps, stats.presentMs, stats.waitMs, stats.focus);
    std::fclose(file);
    MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline bool ReadLiveStats(LiveStats& stats)
{
    stats = LiveStats{};
    FILE* file = nullptr;
    if (_wfopen_s(&file, StatusPath().c_str(), L"rb") != 0 || !file)
        return false;

    bool any = false;
    char line[160];
    while (std::fgets(line, sizeof(line), file))
    {
        char* equal = std::strchr(line, '=');
        if (!equal)
            continue;
        *equal = '\0';
        const char* key = line;
        const char* value = equal + 1;
        const double number = std::atof(value);
        if (!std::strcmp(key, "stamp")) stats.stamp = std::strtoull(value, nullptr, 10);
        else if (!std::strcmp(key, "pid")) stats.pid = static_cast<unsigned>(number);
        else if (!std::strcmp(key, "capture")) stats.captureFps = number;
        else if (!std::strcmp(key, "present")) stats.presentFps = number;
        else if (!std::strcmp(key, "presentms")) stats.presentMs = number;
        else if (!std::strcmp(key, "waitms")) stats.waitMs = number;
        else if (!std::strcmp(key, "focus")) stats.focus = static_cast<int>(number);
        else continue;
        any = true;
    }
    std::fclose(file);
    return any && stats.stamp != 0;
}

inline unsigned long long FileStamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return 0;
    return (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

} // namespace oc
