#pragma once

#include <string>

// Settings remembered between runs, stored in %APPDATA%\VSRPlayer\settings.ini
// (read from the pre-rename %APPDATA%\DLSSVideoPlayer\settings.ini if that is all there is).
struct Settings
{
    // RTX Video HDR (TrueHDR) tuning; NVIDIA's defaults.
    unsigned thdrContrast = 100;   // 0..200
    unsigned thdrSaturation = 100; // 0..200
    unsigned thdrMiddleGray = 50;  // 10..100

    // Playback
    unsigned volumePercent = 100; // 0..100
    bool muted = false;

    void Load();       // missing file / keys keep the defaults
    void Save() const; // best effort
};

// Folder for VSRPlayer's per-user files: %APPDATA%\VSRPlayer (created if needed).
std::wstring AppDataFolder();
