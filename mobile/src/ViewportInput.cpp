// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "ViewportInput.h"

#include "SculptView.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTapSlop = 12.0f;   // window units a tap may wander
constexpr double kTapMs = 350.0;    // longest touch that still counts as a tap
constexpr double kHoverMs = 400.0;  // the brush circle stays this long after the last hover sample
// (SDL's clock, as the shell's; a sample's own time stamp counts from elsewhere on the web.)
double nowMs() { return double(SDL_GetTicksNS()) * 1e-6; }
} // namespace

bool ViewportInput::cursor(float& x, float& y, double nowMs) const {
    if (strokeKey_ < 0 && !cursorMouse_ && nowMs - cursorMs_ > kHoverMs) return false;
    x = cursorX_;
    y = cursorY_;
    return true;
}

void ViewportInput::handle(const PointerSample& s, SculptView& view, float w, float h,
                           const std::function<bool(float, float)>& overUi) {
    if (s.kind == PointerKind::Touch) {
        fingerSample(s, view, w, h, overUi);
        return;
    }
    const int64_t key = s.id * 4 + int(s.kind);
    const bool pen = s.kind == PointerKind::Pen;
    const float pressure = pen ? s.pressure : 1.0f;
    const bool digKeys = (SDL_GetModState() & (SDL_KMOD_ALT | SDL_KMOD_CTRL)) != 0; // held: the other way
    if (pen && !penSeen_) {
        penSeen_ = true;
        fingersSculpt = false; // a pen draws, fingers handle the view (as Nomad does once it sees a Pencil)
    }

    switch (s.phase) {
    case PointerPhase::Hover:
        cursorX_ = s.x, cursorY_ = s.y, cursorMs_ = nowMs();
        cursorMouse_ = !pen && !overUi(s.x, s.y);
        return;
    case PointerPhase::Down: {
        if (strokeKey_ >= 0 || mouseDrag_ != 0 || overUi(s.x, s.y)) return;
        if (!fingers_.empty()) return; // fingers are moving the view
        cursorX_ = s.x, cursorY_ = s.y, cursorMs_ = nowMs();
        const bool slide = !pen && ((s.buttons & 6) != 0 || (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
        if (!slide) {
            const bool saved = view.invert;
            if (digKeys) view.invert = !saved;
            const bool began = view.beginStroke(s.x, s.y, w, h, pressure);
            view.invert = saved;
            if (began) {
                strokeKey_ = key;
                return;
            }
        }
        mouseDrag_ = slide ? 2 : 1; // off the model: turn (or slide) the view
        lastX_ = s.x, lastY_ = s.y;
        return;
    }
    case PointerPhase::Move:
        cursorX_ = s.x, cursorY_ = s.y, cursorMs_ = nowMs();
        if (!pen) cursorMouse_ = !overUi(s.x, s.y) || strokeKey_ == key;
        if (strokeKey_ == key) {
            const bool saved = view.invert;
            if (digKeys) view.invert = !saved;
            view.strokeTo(s.x, s.y, w, h, pressure);
            view.invert = saved;
        } else if (mouseDrag_ == 1) {
            view.rotate(s.x - lastX_, s.y - lastY_, h);
        } else if (mouseDrag_ == 2) {
            view.pan(s.x - lastX_, s.y - lastY_, h);
        }
        lastX_ = s.x, lastY_ = s.y;
        return;
    case PointerPhase::Up:
    case PointerPhase::Cancel:
        if (strokeKey_ == key) {
            view.endStroke();
            strokeKey_ = -1;
        }
        mouseDrag_ = 0;
        return;
    }
}

void ViewportInput::fingerSample(const PointerSample& s, SculptView& view, float w, float h,
                                 const std::function<bool(float, float)>& overUi) {
    const int64_t key = s.id * 4 + int(PointerKind::Touch);
    if (s.phase == PointerPhase::Down) {
        // A pen stroke or a mouse drag owns the view; fingers on the bars belong to the interface.
        if ((strokeKey_ >= 0 && strokeKey_ != key && fingers_.empty()) || mouseDrag_ != 0) return;
        if (fingers_.empty() && overUi(s.x, s.y)) return;
        if (fingers_.empty()) {
            multiTouch_ = false;
            tapFingers_ = 0;
            tapMoved_ = false;
            tapStartMs_ = nowMs();
        }
        fingers_[s.id] = {s.x, s.y, s.x, s.y};
        tapFingers_ = std::max(tapFingers_, int(fingers_.size()));
        if (fingers_.size() == 1) {
            if (fingersSculpt && view.beginStroke(s.x, s.y, w, h, 1.0f)) {
                strokeKey_ = key;
                cursorX_ = s.x, cursorY_ = s.y;
            }
        } else {
            multiTouch_ = true;
            if (strokeKey_ >= 0) { // the first finger's stroke was the start of a gesture
                view.cancelStroke();
                strokeKey_ = -1;
            }
        }
        return;
    }

    auto it = fingers_.find(s.id);
    if (it == fingers_.end()) return;
    Finger& f = it->second;

    if (s.phase == PointerPhase::Up || s.phase == PointerPhase::Cancel) {
        if (strokeKey_ == key) {
            view.endStroke();
            strokeKey_ = -1;
        }
        fingers_.erase(it);
        if (fingers_.empty() && s.phase == PointerPhase::Up && !tapMoved_ && nowMs() - tapStartMs_ < kTapMs) {
            if (tapFingers_ == 2) view.undo();
            if (tapFingers_ == 3) view.redo();
        }
        return;
    }
    if (s.phase != PointerPhase::Move) return;

    if (std::hypot(s.x - f.x0, s.y - f.y0) > kTapSlop) tapMoved_ = true;
    if (strokeKey_ == key) {
        cursorX_ = s.x, cursorY_ = s.y;
        view.strokeTo(s.x, s.y, w, h, 1.0f);
    } else if (fingers_.size() == 1 && !multiTouch_) {
        view.rotate(s.x - f.x, s.y - f.y, h);
    } else if (fingers_.size() == 2) {
        // This finger moved, the other stayed: slide by the midpoint's move, zoom by the spread.
        auto other = fingers_.begin();
        if (other->first == s.id) ++other;
        const Finger& o = other->second;
        const float before = std::hypot(f.x - o.x, f.y - o.y), after = std::hypot(s.x - o.x, s.y - o.y);
        view.pan((s.x - f.x) * 0.5f, (s.y - f.y) * 0.5f, h);
        if (before > 1.0f && after > 1.0f) view.zoom(after / before);
    }
    f.x = s.x;
    f.y = s.y;
}
