// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// Tablet shell entry point (web and Windows). A model made by upstream's core and drawn by upstream's renderer
// (through render/GLCompat.h), sculpted with eight of upstream's brushes; the touch shell (Shell.h) round it and the
// pointer gestures of ViewportInput.h. Interface text in Traditional Chinese (I18n.h, mobile/i18n/zh-TW.json).
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include "FileStore.h"
#include "I18n.h"
#include "InputProbe.h"
#include "MobileGL.h"
#include "PointerInput.h"
#include "SculptView.h"
#include "Shell.h"
#include "ViewportInput.h"
#include "render/GLCompat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
EM_JS_DEPS(sculpture_main, "$stringToNewUTF8,$UTF8ToString");
EM_JS(char*, sculpture_browser_info, (), {
    const n = navigator;
    const parts = [
        'Browser: ' + n.userAgent,
        'Cores ' + (n.hardwareConcurrency || '?') + ', device memory ' + (n.deviceMemory ? n.deviceMemory + ' GB' : '?'),
        'Secure ' + window.isSecureContext + ', cross-origin isolated (threads) ' + (window.crossOriginIsolated === true),
        'Standalone (home screen) ' + (window.matchMedia('(display-mode: standalone)').matches || n.standalone === true),
        'Max touch points ' + (n.maxTouchPoints || 0)
    ];
    return stringToNewUTF8(parts.join('\n'));
});
#endif

#ifdef __EMSCRIPTEN__
// A page address option (?model=lion&subdiv=2&bench=1), or "" when absent.
EM_JS(char*, sculpture_url_option, (const char* name), {
    const v = new URLSearchParams(window.location.search).get(UTF8ToString(name));
    return v === null ? 0 : stringToNewUTF8(v);
});
#endif

#ifndef SCULPTURE_ASSET_DIR
#define SCULPTURE_ASSET_DIR "/assets"
#endif
#ifndef SCULPTURE_MOBILE_DIR
#define SCULPTURE_MOBILE_DIR "/mobile"
#endif

namespace {

// Start-up options, for tests and the speed test on a tablet: the page address (?model=lion&subdiv=2&bench=1) on the
// web, --model lion --subdiv 2 --bench 1 on Windows.
std::string startOption(int argc, char* argv[], const char* name) {
#ifdef __EMSCRIPTEN__
    (void)argc;
    (void)argv;
    std::string value;
    if (char* v = sculpture_url_option(name)) {
        value = v;
        std::free(v);
    }
    return value;
#else
    const std::string flag = std::string("--") + name;
    for (int i = 1; i + 1 < argc; ++i)
        if (flag == argv[i]) return argv[i + 1];
    return "";
#endif
}

struct AppState {
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    SculptView view;
    Shell shell;
    ViewportInput input;
    InputProbe probe;
    ProbeInfo info;
    std::vector<PointerSample> samples;
    std::set<int64_t> swallowed; // pointers whose touch closed a panel (ignored until they lift)
    std::string status; // the last error of an action
    std::string notice; // a passing message (Shell: ShellContext::notify)
    double noticeUntil = 0.0;
    files::Autosaver autosaver;
    bool wasBenchmarking = false;
    std::string benchOut; // --benchout FILE (Windows): write the speed test's results there and quit
    std::string exportOut; // --export FILE (Windows, for tests): once the start-up files are open, export and quit
    int diagIn = -1;      // frames until the diagnostics run (-1: not asked for)
    std::string brushesSaved; // the brush settings as last saved (BrushBook JSON)
    double brushesCheckedMs = 0.0;
    Uint64 frameCounterStart = 0;
    int frames = 0;
};

AppState* gApp = nullptr; // (for the page's "going to the background" call)

double nowMs() { return double(SDL_GetTicksNS()) * 1e-6; }

// The brush settings (Brushes.h) and the chosen tool, kept between visits.
const char* const kBrushSettings = "brushes.json";
std::string brushSettingsText(const SculptView& view) {
    sf::Json j = view.brushes.toJson();
    j.set("tool", view.toolDef().key);
    return j.dump();
}
void loadBrushSettings(AppState& app) {
    const std::string text = files::loadSetting(kBrushSettings);
    sf::Json j;
    std::string error;
    if (!text.empty() && sf::Json::parse(text, j, error)) {
        app.view.brushes.applyJson(j);
        if (j["tool"].isString())
            if (const int t = toolIndex(j["tool"].asString().c_str()); t >= 0) app.view.tool = t;
    } else if (!text.empty()) {
        SDL_Log("brush settings: %s", error.c_str());
    }
    app.brushesSaved = brushSettingsText(app.view);
}
// Saves the brush settings when they changed (checked about once a second; the speed test's are its own).
void saveBrushSettings(AppState& app, bool now) {
    if (app.view.benchmarking() || (!now && nowMs() - app.brushesCheckedMs < 1000.0)) return;
    app.brushesCheckedMs = nowMs();
    std::string text = brushSettingsText(app.view);
    if (text == app.brushesSaved) return;
    files::saveSetting(kBrushSettings, text);
    app.brushesSaved = std::move(text);
}

void notify(AppState& app, const std::string& text) {
    app.notice = text;
    app.noticeUntil = nowMs() + 5000.0;
}

// Files that arrived (the autosave read back, a picked or dropped file): opened between frames.
void openIncoming(AppState& app) {
    files::Incoming in;
    while (files::takeIncoming(in)) {
        std::string error;
        const bool restore = in.kind == files::Kind::Restore;
        if (app.view.openFile(in.path, error)) {
            app.status.clear();
            if (restore) {
                app.autosaver.markSaved(app.view.revision()); // (it is the autosave)
                notify(app, T("Picked up where you left off"));
            } else {
                const size_t slash = in.path.find_last_of("/\\");
                notify(app, i18n::format(T("Opened %s"), in.path.substr(slash == std::string::npos ? 0 : slash + 1).c_str()));
            }
        } else {
            SDL_Log("open %s: %s", in.path.c_str(), error.c_str());
            app.status = i18n::format(restore ? T("Could not restore the last work: %s") : T("Could not open the file: %s"),
                                      error.c_str());
        }
        files::release(in);
    }
    const std::string notice = files::takeNotice();
    if (!notice.empty()) app.status = notice;
}

std::string glString(GLenum name) {
    const GLubyte* s = mgl::GetString(name);
    return s ? reinterpret_cast<const char*>(s) : "?";
}

bool createContext(AppState& app, std::string& error) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    app.window = SDL_CreateWindow("SculptureABC", 1280, 800, flags);
    if (!app.window) {
        error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }
#ifdef __EMSCRIPTEN__
    // The page goes full screen itself (Settings > Full screen, files.js): to SDL that is only a window resize. Left
    // to its own fullscreenchange handler, SDL marks the window full screen and from then on ignores the canvas's
    // size, so the picture kept its old size, cut off at the top (iPad).
    emscripten_set_fullscreenchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, 0, nullptr);
#endif
    app.gl = SDL_GL_CreateContext(app.window);
#ifndef __EMSCRIPTEN__
    if (!app.gl) {
        // No GLES driver on this desktop: fall back to desktop OpenGL 3.3 core (same shaders, other header).
        SDL_Log("GLES 3.0 context failed (%s); trying OpenGL 3.3 core", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        app.gl = SDL_GL_CreateContext(app.window);
    }
#endif
    if (!app.gl) {
        error = std::string("SDL_GL_CreateContext: ") + SDL_GetError();
        return false;
    }
    SDL_GL_MakeCurrent(app.window, app.gl);
    SDL_GL_SetSwapInterval(1);
    if (const char* missing = mgl::load([](const char* name) {
            return reinterpret_cast<mgl::GenericProc>(SDL_GL_GetProcAddress(name));
        })) {
        error = std::string("missing GL function ") + missing;
        return false;
    }
    // Upstream's renderer: its own function table, with GLES 3.0 / WebGL2 stand-ins (render/GLCompat.h).
    if (const char* missing = sculpture::loadUpstreamGL([](const char* name) {
            return reinterpret_cast<gl::GenericProc>(SDL_GL_GetProcAddress(name));
        }, mgl::isES())) {
        error = std::string("missing GL function ") + missing;
        return false;
    }
    return true;
}

// A pointer sample for the 3D view. A touch that lands off an open panel only closes it (as Nomad's panels do).
void viewportSample(AppState& app, const PointerSample& s) {
    int w = 0, h = 0;
    SDL_GetWindowSize(app.window, &w, &h);
    const int64_t key = s.id * 4 + int(s.kind);
    if (s.phase == PointerPhase::Down && app.shell.panelOpen() && !app.shell.hits(s.x, s.y) && !app.input.busy()) {
        app.shell.closePanel();
        app.swallowed.insert(key);
        return;
    }
    if (app.swallowed.count(key)) {
        if (s.phase == PointerPhase::Up || s.phase == PointerPhase::Cancel) app.swallowed.erase(key);
        return;
    }
    const auto overUi = [&](float x, float y) {
        return app.shell.hits(x, y) || (app.shell.showProbe && app.probe.hitsPanel(x, y)) ||
               (s.kind == PointerKind::Mouse && ImGui::GetIO().WantCaptureMouse);
    };
    app.input.handle(s, app.view, float(w), float(h), overUi);
}

} // namespace

#ifdef __EMSCRIPTEN__
// mobile/web/files.js calls this when the page goes to the background or away (visibilitychange, pagehide): save
// now, as the browser may stop the page any moment after.
extern "C" EMSCRIPTEN_KEEPALIVE void sculpture_page_hidden() {
    if (!gApp) return;
    saveBrushSettings(*gApp, true);
    gApp->autosaver.saveNow(gApp->view, nowMs());
}
#endif

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    SDL_SetAppMetadata("SculptureABC", SCULPTURE_VERSION, "com.sculptureabc.app");
    // Pens stay pens (no fake touches); fingers and pens still move the mouse for ImGui.
    SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    auto* app = new AppState();
    *appstate = app;
    gApp = app;

    std::string error;
    if (!createContext(*app, error)) {
        SDL_Log("%s", error.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SculptureABC", error.c_str(), nullptr);
        return SDL_APP_FAILURE;
    }

    ProbeInfo& info = app->info;
    info.platform = SDL_GetPlatform();
    info.glVersion = glString(GL_VERSION);
    info.glRenderer = glString(GL_RENDERER);
    info.glslVersion = glString(GL_SHADING_LANGUAGE_VERSION) + (mgl::isES() ? "  (GLES path)" : "  (desktop GL path)");
    info.systemRamMB = SDL_GetSystemRAM();
    SDL_Log("GL %s | %s | %s", info.glVersion.c_str(), info.glRenderer.c_str(), info.glslVersion.c_str());
#ifdef __EMSCRIPTEN__
    if (char* text = sculpture_browser_info()) {
        info.extra = text;
        std::free(text);
    }
#endif

    if (!app->view.init(error)) {
        SDL_Log("sphere: %s", error.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SculptureABC shader", error.c_str(), nullptr);
        return SDL_APP_FAILURE;
    }

    if (startOption(argc, argv, "brushes") != "default") loadBrushSettings(*app); // (?brushes=default: as shipped)
    else app->brushesSaved = brushSettingsText(app->view);
    if (const int t = toolIndex(startOption(argc, argv, "tool").c_str()); t >= 0) app->view.tool = t; // ?tool=Clay

    const std::string model = startOption(argc, argv, "model");
    if (model == "lion" && !app->view.loadLionHead(app->status)) SDL_Log("lion head: %s", app->status.c_str());
    if (model.rfind("head", 0) == 0) app->view.newBenchHead(size_t(std::atof(model.c_str() + 4) * 1e6)); // head1 = 1M
    for (int i = 0, n = std::atoi(startOption(argc, argv, "subdiv").c_str()); i < n && i < 4; ++i)
        if (!app->view.subdivide(app->status)) SDL_Log("subdivide: %s", app->status.c_str());
    if (startOption(argc, argv, "wire") == "1") app->view.settings.wireframe = true;
    if (startOption(argc, argv, "bench") == "1") app->view.startBenchmark();
    app->benchOut = startOption(argc, argv, "benchout");
    if (startOption(argc, argv, "diag") == "1") app->diagIn = 30; // after the page has settled

    // Pick up the last work (the autosave), unless a start option asked for a model (?model=, ?bench=1, ?fresh=1):
    // then that model counts as saved, so it replaces the autosave only once it is sculpted.
    const bool restore = model.empty() && startOption(argc, argv, "bench") != "1" && startOption(argc, argv, "fresh") != "1";
    files::init(app->window, restore);
    app->autosaver.markSaved(app->view.revision());
#ifndef __EMSCRIPTEN__
    // Tests: --open FILE opens a file as if it were dropped on the window; --export FILE exports and quits.
    if (const std::string open = startOption(argc, argv, "open"); !open.empty()) files::dropFile(open);
    app->exportOut = startOption(argc, argv, "export");
#endif

    // Interface text: Traditional Chinese unless ?lang=en / --lang en.
    {
        std::string i18nError;
        if (!i18n::load(SCULPTURE_MOBILE_DIR "/i18n/zh-TW.json", i18nError)) SDL_Log("i18n: %s", i18nError.c_str());
        if (startOption(argc, argv, "lang") == "en") i18n::setChinese(false);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // (A page that loads while hidden can have a canvas of size 0: the density comes out 0 or NaN, and every font
    // and ImGui coordinate with it. Then fall back to 1.)
    float pixelDensity = SDL_GetWindowPixelDensity(app->window);
    if (!std::isfinite(pixelDensity) || pixelDensity < 0.25f) pixelDensity = 1;
    float displayScale = SDL_GetWindowDisplayScale(app->window);
    float uiScale = std::isfinite(displayScale) && displayScale > 0 ? displayScale / pixelDensity : 1.0f;
    if (!std::isfinite(uiScale) || uiScale < 0.25f) uiScale = 1.0f;
    SDL_Log("pixel density %.2f, display scale %.2f, ui scale %.2f", double(pixelDensity), double(displayScale), double(uiScale));
    info.pixelDensity = pixelDensity;
    info.contentScale = uiScale;
    app->shell.applyStyle(uiScale);
    app->shell.loadFonts(uiScale, pixelDensity, SCULPTURE_ASSET_DIR, SCULPTURE_MOBILE_DIR);
    info.cjkFont = true;
    app->shell.openPanel(startOption(argc, argv, "panel"));
    ImGui_ImplSDL3_InitForOpenGL(app->window, app->gl);
    ImGui_ImplOpenGL3_Init(mgl::isES() ? "#version 300 es" : "#version 330 core");

    pointer::install(app->window);
    app->frameCounterStart = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto* app = static_cast<AppState*>(appstate);
    ImGui_ImplSDL3_ProcessEvent(event);
    pointer::processSdlEvent(*event, app->window);
    switch (event->type) {
    case SDL_EVENT_QUIT:
        saveBrushSettings(*app, true);
#ifdef __EMSCRIPTEN__
        // (The page is unloading: SDL says so before the page's own pagehide, which saves the quick way, see
        // mobile/web/files.js. Tearing the program down here would leave that save nothing to save.)
        break;
#else
        app->autosaver.saveNow(app->view, nowMs());
        return SDL_APP_SUCCESS;
#endif
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        app->autosaver.saveNow(app->view, nowMs());
        break;
#ifndef __EMSCRIPTEN__
    case SDL_EVENT_DROP_FILE:
        if (event->drop.data) files::dropFile(event->drop.data);
        break;
#endif
    case SDL_EVENT_MOUSE_WHEEL:
        if (!ImGui::GetIO().WantCaptureMouse && !app->shell.hits(event->wheel.mouse_x, event->wheel.mouse_y))
            app->view.zoom(std::pow(1.15f, event->wheel.y));
        break;
    case SDL_EVENT_KEY_DOWN:
        if ((event->key.mod & SDL_KMOD_CTRL) && !ImGui::GetIO().WantTextInput) {
            const bool shift = (event->key.mod & SDL_KMOD_SHIFT) != 0;
            if (event->key.key == SDLK_Z) shift ? app->view.redo() : app->view.undo();
            if (event->key.key == SDLK_Y) app->view.redo();
        }
        break;
    case SDL_EVENT_LOW_MEMORY:
        SDL_Log("low memory warning");
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto* app = static_cast<AppState*>(appstate);

    openIncoming(*app);
    if (!app->exportOut.empty()) {
        std::string error;
        if (!app->view.exportFile(app->exportOut, error)) SDL_Log("export %s: %s", app->exportOut.c_str(), error.c_str());
        return SDL_APP_SUCCESS; // (the autosave is left alone: quitting this way doesn't save)
    }
    pointer::drain(app->samples);
    for (const PointerSample& s : app->samples) {
        app->probe.onSample(s);
        viewportSample(*app, s);
    }

    ProbeInfo& info = app->info;
    SDL_GetWindowSize(app->window, &info.windowW, &info.windowH);
    SDL_GetWindowSizeInPixels(app->window, &info.pixelW, &info.pixelH);
    info.triangles = app->view.triangleCount();
    ++app->frames;
    Uint64 now = SDL_GetTicksNS();
    if (now - app->frameCounterStart >= 500000000ull) {
        info.fps = float(app->frames) * 1.0e9f / float(now - app->frameCounterStart);
        app->frames = 0;
        app->frameCounterStart = now;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ShellContext shellContext{app->view,   app->input,  app->probe, info, app->status, app->diagIn, double(now) * 1e-6,
                              app->notice, app->noticeUntil};
    app->shell.draw(shellContext);
    ImGui::Render();

    mgl::Viewport(0, 0, info.pixelW, info.pixelH);
    mgl::ClearColor(0.16f, 0.17f, 0.19f, 1.0f);
    mgl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (app->view.benchmarking() && !app->view.benchmarkStep(info.pixelW, info.pixelH) && !app->benchOut.empty()) {
        if (SDL_IOStream* f = SDL_IOFromFile(app->benchOut.c_str(), "w")) {
            SDL_IOprintf(f, "GL %s | %s\n", info.glVersion.c_str(), info.glRenderer.c_str());
            for (const std::string& line : app->view.benchmarkLines()) SDL_IOprintf(f, "%s\n", line.c_str());
            SDL_CloseIO(f);
        }
        return SDL_APP_SUCCESS;
    }
    if (app->diagIn > 0) --app->diagIn;
    else if (app->diagIn == 0) {
        app->diagIn = -1;
        app->view.runDiagnostics(info.pixelW, info.pixelH);
    }
    // The speed test replaced the model with test heads: bring the work back from the autosave.
    if (app->wasBenchmarking && !app->view.benchmarking()) {
        app->autosaver.markSaved(app->view.revision());
        files::restoreAgain();
    }
    app->wasBenchmarking = app->view.benchmarking();
    app->autosaver.tick(app->view, double(now) * 1e-6);
    saveBrushSettings(*app, false);
    app->view.draw(info.pixelW, info.pixelH);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(app->window);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    auto* app = static_cast<AppState*>(appstate);
    gApp = nullptr;
    if (!app) return;
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    app->view.shutdown();
    if (app->gl) SDL_GL_DestroyContext(app->gl);
    if (app->window) SDL_DestroyWindow(app->window);
    delete app;
}
