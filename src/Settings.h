#pragma once

// Settings remembered between runs, stored in %APPDATA%\VSRPlayer\settings.ini
// (read from the pre-rename %APPDATA%\DLSSVideoPlayer\settings.ini if that is all there is).
struct Settings
{
    // RTX Video HDR (TrueHDR) tuning; NVIDIA's defaults.
    unsigned thdrContrast = 100;   // 0..200
    unsigned thdrSaturation = 100; // 0..200
    unsigned thdrMiddleGray = 50;  // 10..100

    void Load();       // missing file / keys keep the defaults
    void Save() const; // best effort
};
