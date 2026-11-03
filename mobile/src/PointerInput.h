// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// One stream of pointer samples (mouse, finger, pen) for the viewport, whatever the platform:
//   - Web: the browser's Pointer Events, read directly (pressure, tilt, and the coalesced samples
//     Apple Pencil delivers between frames). SDL's own touch / mouse emulation still feeds ImGui.
//   - Windows (development build): translated from SDL mouse / finger / pen events.
// Coordinates are window units (CSS pixels on the web), origin top-left.
#pragma once

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <vector>

enum class PointerKind : int { Mouse = 0, Touch = 1, Pen = 2 };
enum class PointerPhase : int { Down = 0, Move = 1, Up = 2, Cancel = 3, Hover = 4 };

struct PointerSample {
    PointerPhase phase = PointerPhase::Move;
    PointerKind kind = PointerKind::Mouse;
    int64_t id = 0;
    float x = 0, y = 0;
    float pressure = 0;      // 0..1; mouse reports 1 while a button is down
    float tiltX = 0, tiltY = 0; // degrees, -90..90
    bool eraser = false;
    int buttons = 0;
    double timeMs = 0;
    bool coalesced = false;  // an extra sample the browser merged into one event
};

namespace pointer {

// Starts listening (web: adds Pointer Event listeners to the canvas). Call once after the window exists.
void install(SDL_Window* window);

// Desktop: turns an SDL event into samples. Does nothing on the web (the browser stream is used).
void processSdlEvent(const SDL_Event& event, SDL_Window* window);

// Moves the samples gathered since the last call into `out` (cleared first).
void drain(std::vector<PointerSample>& out);

// "Pointer Events (web)" or "SDL".
const char* sourceName();

} // namespace pointer
