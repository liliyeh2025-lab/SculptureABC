// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// What the pointers do on the 3D view:
//   pen on the model            sculpt (pressure scales the strength); off the model it turns the view
//   one finger on the model     sculpt too, until a pen shows up (then fingers only move the view; a setting)
//   one finger off the model    turn the view
//   two fingers                 slide and pinch-zoom (a stroke the first finger began is taken back)
//   two-finger / three-finger tap   undo / redo
//   mouse                       left button sculpts on the model, turns off it; right / middle slides; wheel zooms
#pragma once

#include "PointerInput.h"

#include <cstdint>
#include <functional>
#include <map>

class SculptView;

class ViewportInput {
public:
    // `overUi(x, y)`: the point lies on the shell's bars or panels (that pointer is left to the interface).
    void handle(const PointerSample& s, SculptView& view, float w, float h, const std::function<bool(float, float)>& overUi);

    // Fingers sculpt on the model. Turned off by itself the first time a pen is seen (the settings can turn it on).
    bool fingersSculpt = true;
    bool penSeen() const { return penSeen_; }

    // Where to draw the brush circle (pen hover, the mouse, or the stroking pointer), if anywhere.
    bool cursor(float& x, float& y, double nowMs) const;
    bool busy() const { return strokeKey_ >= 0 || !fingers_.empty() || mouseDrag_ != 0; }

private:
    struct Finger {
        float x, y;   // last position
        float x0, y0; // where it landed
    };
    void fingerSample(const PointerSample& s, SculptView& view, float w, float h, const std::function<bool(float, float)>& overUi);

    bool penSeen_ = false;
    int64_t strokeKey_ = -1; // the pointer sculpting right now (id * 4 + kind)
    std::map<int64_t, Finger> fingers_; // fingers that landed on the view
    bool multiTouch_ = false; // two or more fingers came down since the view was last free of fingers
    int tapFingers_ = 0;      // most fingers down at once in this touch
    bool tapMoved_ = false;
    double tapStartMs_ = 0;
    int mouseDrag_ = 0; // 1 turning, 2 sliding
    float lastX_ = 0, lastY_ = 0; // mouse / pen turning
    float cursorX_ = 0, cursorY_ = 0;
    double cursorMs_ = -1e9;
    bool cursorMouse_ = false;
};
