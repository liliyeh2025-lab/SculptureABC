// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "PointerInput.h"

#include <SDL3/SDL.h>

#include <unordered_map>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

std::vector<PointerSample> gQueue;

void push(const PointerSample& sample) {
    if (gQueue.size() < 100000) gQueue.push_back(sample);
}

} // namespace

#ifdef __EMSCRIPTEN__

extern "C" EMSCRIPTEN_KEEPALIVE void sculpture_pointer(int phase, int kind, double id, double x, double y,
                                                       double pressure, double tiltX, double tiltY, int eraser,
                                                       int buttons, double timeMs, int coalesced) {
    PointerSample s;
    s.phase = PointerPhase(phase);
    s.kind = PointerKind(kind);
    s.id = int64_t(id);
    s.x = float(x);
    s.y = float(y);
    s.pressure = float(pressure);
    s.tiltX = float(tiltX);
    s.tiltY = float(tiltY);
    s.eraser = eraser != 0;
    s.buttons = buttons;
    s.timeMs = timeMs;
    s.coalesced = coalesced != 0;
    push(s);
}

// Listens on the canvas. No preventDefault here: SDL's own listeners still need the events (ImGui),
// and the page CSS (touch-action: none) already stops the browser's pinch / scroll gestures.
EM_JS(void, sculpture_install_pointer_listeners, (), {
    const canvas = Module['canvas'];
    if (!canvas || canvas.__sculpturePointer) return;
    canvas.__sculpturePointer = true;
    const kindOf = (e) => (e.pointerType === 'pen' ? 2 : e.pointerType === 'touch' ? 1 : 0);
    const send = (phase, e, coalesced) => {
        const r = canvas.getBoundingClientRect();
        // Mouse reports pressure 0.5 while pressed; use 1 so it reads as a full press.
        let p = e.pressure;
        if (e.pointerType === 'mouse') p = e.buttons ? 1 : 0;
        const eraser = (e.buttons & 32) ? 1 : 0;
        // iPad (WebKit) reports Apple Pencil angles as altitude / azimuth and leaves tiltX / tiltY at 0:
        // convert with the Pointer Events spec formula.
        let tx = e.tiltX || 0, ty = e.tiltY || 0;
        if (tx === 0 && ty === 0 && typeof e.altitudeAngle === 'number' && typeof e.azimuthAngle === 'number' &&
            e.altitudeAngle < Math.PI / 2 - 1e-4) {
            const t = Math.tan(e.altitudeAngle);
            tx = Math.atan(Math.cos(e.azimuthAngle) / t) * 180 / Math.PI;
            ty = Math.atan(Math.sin(e.azimuthAngle) / t) * 180 / Math.PI;
        }
        _sculpture_pointer(phase, kindOf(e), e.pointerId, e.clientX - r.left, e.clientY - r.top, p,
                           tx, ty, eraser, e.buttons, e.timeStamp, coalesced ? 1 : 0);
    };
    canvas.addEventListener('pointerdown', (e) => {
        try { canvas.setPointerCapture(e.pointerId); } catch (err) {}
        send(0, e, false);
    });
    canvas.addEventListener('pointermove', (e) => {
        const phase = (e.buttons !== 0 || (e.pointerType === 'touch')) ? 1 : 4;
        const list = e.getCoalescedEvents ? e.getCoalescedEvents() : [];
        if (list.length > 1) {
            for (let i = 0; i < list.length; ++i) send(phase, list[i], i < list.length - 1);
        } else {
            send(phase, e, false);
        }
    });
    canvas.addEventListener('pointerup', (e) => send(2, e, false));
    canvas.addEventListener('pointercancel', (e) => send(3, e, false));
    // Safari's own pinch gesture events: keep the page from zooming.
    document.addEventListener('gesturestart', (e) => e.preventDefault());
    document.addEventListener('gesturechange', (e) => e.preventDefault());
});

namespace pointer {
void install(SDL_Window*) { sculpture_install_pointer_listeners(); }
void processSdlEvent(const SDL_Event&, SDL_Window*) {}
const char* sourceName() { return "Pointer Events (web)"; }
} // namespace pointer

#else // desktop: SDL events

namespace {

struct PenState {
    float pressure = 0, tiltX = 0, tiltY = 0;
    bool down = false, eraser = false;
};
std::unordered_map<SDL_PenID, PenState> gPens;
bool gMouseDown = false;

double toMs(Uint64 ns) { return double(ns) / 1.0e6; }

} // namespace

namespace pointer {

void install(SDL_Window*) {}

const char* sourceName() { return "SDL"; }

void processSdlEvent(const SDL_Event& e, SDL_Window* window) {
    PointerSample s;
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.which == SDL_TOUCH_MOUSEID || e.button.which == SDL_PEN_MOUSEID) return;
        if (e.button.button != SDL_BUTTON_LEFT) return;
        gMouseDown = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        s.phase = gMouseDown ? PointerPhase::Down : PointerPhase::Up;
        s.kind = PointerKind::Mouse;
        s.x = e.button.x;
        s.y = e.button.y;
        s.pressure = gMouseDown ? 1.0f : 0.0f;
        s.buttons = gMouseDown ? 1 : 0;
        s.timeMs = toMs(e.button.timestamp);
        push(s);
        return;
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.which == SDL_TOUCH_MOUSEID || e.motion.which == SDL_PEN_MOUSEID) return;
        s.phase = gMouseDown ? PointerPhase::Move : PointerPhase::Hover;
        s.kind = PointerKind::Mouse;
        s.x = e.motion.x;
        s.y = e.motion.y;
        s.pressure = gMouseDown ? 1.0f : 0.0f;
        s.buttons = gMouseDown ? 1 : 0;
        s.timeMs = toMs(e.motion.timestamp);
        push(s);
        return;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        s.phase = e.type == SDL_EVENT_FINGER_DOWN     ? PointerPhase::Down
                  : e.type == SDL_EVENT_FINGER_MOTION ? PointerPhase::Move
                  : e.type == SDL_EVENT_FINGER_UP     ? PointerPhase::Up
                                                      : PointerPhase::Cancel;
        s.kind = PointerKind::Touch;
        s.id = int64_t(e.tfinger.fingerID);
        s.x = e.tfinger.x * float(w);
        s.y = e.tfinger.y * float(h);
        s.pressure = e.tfinger.pressure;
        s.timeMs = toMs(e.tfinger.timestamp);
        push(s);
        return;
    }
    case SDL_EVENT_PEN_AXIS: {
        PenState& pen = gPens[e.paxis.which];
        if (e.paxis.axis == SDL_PEN_AXIS_PRESSURE) pen.pressure = e.paxis.value;
        else if (e.paxis.axis == SDL_PEN_AXIS_XTILT) pen.tiltX = e.paxis.value;
        else if (e.paxis.axis == SDL_PEN_AXIS_YTILT) pen.tiltY = e.paxis.value;
        else return;
        // Pressure changes without motion are samples too (a pen pressing harder in place).
        s.phase = pen.down ? PointerPhase::Move : PointerPhase::Hover;
        s.kind = PointerKind::Pen;
        s.id = int64_t(e.paxis.which);
        s.x = e.paxis.x;
        s.y = e.paxis.y;
        s.pressure = pen.pressure;
        s.tiltX = pen.tiltX;
        s.tiltY = pen.tiltY;
        s.eraser = pen.eraser;
        s.timeMs = toMs(e.paxis.timestamp);
        push(s);
        return;
    }
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP: {
        PenState& pen = gPens[e.ptouch.which];
        pen.down = e.type == SDL_EVENT_PEN_DOWN;
        pen.eraser = e.ptouch.eraser;
        s.phase = pen.down ? PointerPhase::Down : PointerPhase::Up;
        s.kind = PointerKind::Pen;
        s.id = int64_t(e.ptouch.which);
        s.x = e.ptouch.x;
        s.y = e.ptouch.y;
        s.pressure = pen.down ? pen.pressure : 0.0f;
        s.tiltX = pen.tiltX;
        s.tiltY = pen.tiltY;
        s.eraser = pen.eraser;
        s.timeMs = toMs(e.ptouch.timestamp);
        push(s);
        return;
    }
    case SDL_EVENT_PEN_MOTION: {
        PenState& pen = gPens[e.pmotion.which];
        s.phase = pen.down ? PointerPhase::Move : PointerPhase::Hover;
        s.kind = PointerKind::Pen;
        s.id = int64_t(e.pmotion.which);
        s.x = e.pmotion.x;
        s.y = e.pmotion.y;
        s.pressure = pen.down ? pen.pressure : 0.0f;
        s.tiltX = pen.tiltX;
        s.tiltY = pen.tiltY;
        s.eraser = pen.eraser;
        s.timeMs = toMs(e.pmotion.timestamp);
        push(s);
        return;
    }
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        gPens.erase(e.pproximity.which);
        return;
    default:
        return;
    }
}

} // namespace pointer

#endif

namespace pointer {
void drain(std::vector<PointerSample>& out) {
    out.clear();
    out.swap(gQueue);
}
} // namespace pointer
