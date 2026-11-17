#pragma once

#include <dxgi1_5.h>

#include <string>

struct AVFrame;

// HDR capabilities of the monitor the window is (mostly) on.
struct DisplayInfo
{
    bool hdr = false;             // Windows HDR ("Use HDR") is on for this monitor
    float maxNits = 0.0f;         // display peak luminance reported by Windows (0 = unknown)
    float sdrWhiteNits = 200.0f;  // Windows "SDR content brightness" level (UI / SDR white in HDR)
    std::wstring deviceName;      // e.g. \\.\DISPLAY1
};

// Queries with a fresh DXGI factory, so it reflects HDR being toggled while the player runs.
// Call on events (file open, window moved, display change), not per frame.
DisplayInfo QueryDisplayInfo(HWND hwnd);

// True if the decoded frame is HDR10: PQ transfer in a 10-bit (P010) surface.
bool IsHdr10Frame(const AVFrame* frame, DXGI_FORMAT surfaceFormat);

// HDR10 static metadata (mastering display + content light level) from the frame's side
// data, with BT.2020 / 1000-nit defaults for anything the file doesn't provide.
DXGI_HDR_METADATA_HDR10 HdrMetadataFromFrame(const AVFrame* frame);
