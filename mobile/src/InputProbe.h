// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// Input probe (a developer panel): shows what the device really delivers (finger / pen / mouse, pressure, tilt,
// sample rate, coalesced samples) and draws pen strokes as dots sized by pressure.
#pragma once

#include "PointerInput.h"

#include <deque>
#include <string>
#include <vector>

struct ProbeInfo {
    std::string platform, glVersion, glRenderer, glslVersion, extra;
    int windowW = 0, windowH = 0, pixelW = 0, pixelH = 0;
    float pixelDensity = 1, contentScale = 1, fps = 0;
    int systemRamMB = 0, triangles = 0;
    bool cjkFont = false;
};

class InputProbe {
public:
    void onSample(const PointerSample& sample);

    // Draws the panel and the stroke dots. Returns the actions the user picked.
    struct Actions {
        bool resetView = false;
    };
    Actions draw(const ProbeInfo& info);

    // True when (x, y) lies on the probe panel (so the viewport ignores that touch).
    bool hitsPanel(float x, float y) const;

private:
    struct KindStats {
        long long samples = 0, coalesced = 0, downs = 0, hovers = 0;
        std::deque<double> recent; // sample times (ms) for the rate over the last second
        float lastPressure = 0, minPressure = 1, maxPressure = 0, tiltX = 0, tiltY = 0;
        bool sawEraser = false;
        double lastTime = 0;
    };
    struct Dot {
        float x, y, pressure;
        int kind;
    };
    KindStats stats_[3];
    std::vector<float> pressureHistory_;
    std::vector<Dot> dots_;
    std::vector<std::pair<long long, PointerSample>> active_;
    std::string lastEvent_;
    bool showDots_ = false; // off: the pen sculpts now, dots would cover the model
    float panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;
};
