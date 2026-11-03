// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "FileStore.h"

#include "SculptView.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <system_error>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace files {

namespace {

std::string takeString(char* text) {
    std::string out;
    if (text) {
        out = text;
        std::free(text);
    }
    return out;
}

} // namespace

#ifdef __EMSCRIPTEN__
// --- Web: mobile/web/files.js does the work -----------------------------------------------------------------------

EM_JS_DEPS(sculpture_files, "$stringToNewUTF8,$UTF8ToString");
EM_JS(void, js_files_init, (int restore), { SculptureFiles.init(!!restore); });
EM_JS(void, js_files_restore, (), { SculptureFiles.restore(); });
EM_JS(int, js_files_restoring, (), { return SculptureFiles.restoring() ? 1 : 0; });
EM_JS(char*, js_files_take, (), {
    const t = SculptureFiles.take();
    return t === null ? 0 : stringToNewUTF8(t);
});
EM_JS(char*, js_files_notice, (), {
    const t = SculptureFiles.notice();
    return t === null ? 0 : stringToNewUTF8(t);
});
EM_JS(void, js_files_remove, (const char* path), { SculptureFiles.remove(UTF8ToString(path)); });
EM_JS(int, js_files_available, (), { return SculptureFiles.available() ? 1 : 0; });
EM_JS(char*, js_files_status, (), {
    const s = SculptureFiles.state();
    return stringToNewUTF8(s === "saved" ? SculptureFiles.savedAt() : s === "writing" ? "saving" : s === "error" ? "error" : "");
});
EM_JS(void, js_files_commit, (const char* path), { SculptureFiles.commit(UTF8ToString(path)); });
EM_JS(void, js_files_deliver, (const char* path, const char* name, const char* ok, const char* cancel), {
    SculptureFiles.deliver(UTF8ToString(path), UTF8ToString(name), UTF8ToString(ok), UTF8ToString(cancel));
});
EM_JS(void, js_files_pick, (const char* message, const char* ok, const char* cancel), {
    SculptureFiles.pick(UTF8ToString(message), UTF8ToString(ok), UTF8ToString(cancel));
});

void init(SDL_Window*, bool restore) {
    std::error_code ec;
    std::filesystem::create_directories("/tmp/files", ec);
    js_files_init(restore ? 1 : 0);
}
void restoreAgain() { js_files_restore(); }
bool restoring() { return js_files_restoring() != 0; }

bool takeIncoming(Incoming& out) {
    const std::string t = takeString(js_files_take());
    const size_t nl = t.find('\n');
    if (nl == std::string::npos) return false;
    out.kind = t.compare(0, nl, "restore") == 0 ? Kind::Restore : Kind::Import;
    out.path = t.substr(nl + 1);
    return true;
}
void release(const Incoming& in) { js_files_remove(in.path.c_str()); }
std::string takeNotice() { return takeString(js_files_notice()); }

bool autosaveAvailable() { return js_files_available() != 0; }
std::string autosaveWritePath() { return "/tmp/files/autosave.sfproj"; }
void commitAutosave(const std::string& path) { js_files_commit(path.c_str()); }
std::string autosaveStatus() { return takeString(js_files_status()); }
std::string autosaveLocation() { return ""; }

std::string exportPath(const std::string& fileName) {
    std::error_code ec;
    std::filesystem::create_directories("/tmp/files/out", ec);
    return "/tmp/files/out/" + fileName;
}
std::string deliverExport(const std::string& path, const std::string& fileName, const char* okLabel,
                          const char* cancelLabel) {
    js_files_deliver(path.c_str(), fileName.c_str(), okLabel, cancelLabel);
    return "";
}

void pickFile(const char* message, const char* okLabel, const char* cancelLabel) {
    js_files_pick(message, okLabel, cancelLabel);
}
void dropFile(const std::string&) {} // (files.js reads drops on the page itself)

EM_JS(int, js_fullscreen_available, (), { return SculptureFiles.fullscreenAvailable() ? 1 : 0; });
EM_JS(int, js_fullscreen_on, (), { return SculptureFiles.isFullscreen() ? 1 : 0; });
EM_JS(void, js_fullscreen_toggle, (const char* message, const char* ok, const char* cancel), {
    SculptureFiles.toggleFullscreen(UTF8ToString(message), UTF8ToString(ok), UTF8ToString(cancel));
});
EM_JS(int, js_running_as_app, (), { return SculptureFiles.runningAsApp() ? 1 : 0; });
bool fullscreenAvailable() { return js_fullscreen_available() != 0; }
bool runningAsApp() { return js_running_as_app() != 0; }
bool isFullscreen() { return js_fullscreen_on() != 0; }
void toggleFullscreen(const char* message, const char* okLabel, const char* cancelLabel) {
    js_fullscreen_toggle(message, okLabel, cancelLabel);
}

// Settings live in the site's localStorage (small, synchronous; it may be unavailable: then nothing is kept).
EM_JS(char*, js_setting_load, (const char* name), {
    try {
        const v = window.localStorage.getItem('sculptureabc.' + UTF8ToString(name));
        return v === null ? 0 : stringToNewUTF8(v);
    } catch (e) {
        return 0;
    }
});
EM_JS(void, js_setting_save, (const char* name, const char* text), {
    try {
        window.localStorage.setItem('sculptureabc.' + UTF8ToString(name), UTF8ToString(text));
    } catch (e) {
    }
});
std::string loadSetting(const std::string& name) { return takeString(js_setting_load(name.c_str())); }
void saveSetting(const std::string& name, const std::string& text) { js_setting_save(name.c_str(), text.c_str()); }

#else
// --- Windows (and other desktops): plain files ---------------------------------------------------------------------

namespace {

std::mutex gLock; // (the file dialog may answer from another thread)
std::deque<Incoming> gQueue;
std::deque<std::string> gNotices;
SDL_Window* gWindow = nullptr;
std::string gStatus;

// UTF-8 text to a path (a plain std::string would be read in the system code page).
std::filesystem::path fsPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::string prefDir() {
    static std::string dir = takeString(SDL_GetPrefPath("SculptureABC", "SculptureABC")); // (ends with a separator)
    return dir;
}
std::string autosaveFile() { return prefDir() + "autosave.sfproj"; }

void queueRestore() {
    std::error_code ec;
    if (prefDir().empty() || !std::filesystem::exists(fsPath(autosaveFile()), ec)) return;
    std::lock_guard<std::mutex> lock(gLock);
    gQueue.push_back({Kind::Restore, autosaveFile()});
}

void SDLCALL dialogDone(void*, const char* const* list, int) {
    std::lock_guard<std::mutex> lock(gLock);
    if (!list) {
        gNotices.push_back(std::string("file dialog: ") + SDL_GetError());
        return;
    }
    if (list[0]) gQueue.push_back({Kind::Import, list[0]});
}

} // namespace

void init(SDL_Window* window, bool restore) {
    gWindow = window;
    if (restore) queueRestore();
}
void restoreAgain() { queueRestore(); }
bool restoring() { return false; } // (read at once)

bool takeIncoming(Incoming& out) {
    std::lock_guard<std::mutex> lock(gLock);
    if (gQueue.empty()) return false;
    out = std::move(gQueue.front());
    gQueue.pop_front();
    return true;
}
void release(const Incoming&) {} // (the files are the user's own)

std::string takeNotice() {
    std::lock_guard<std::mutex> lock(gLock);
    if (gNotices.empty()) return "";
    std::string text = std::move(gNotices.front());
    gNotices.pop_front();
    return text;
}

bool autosaveAvailable() { return !prefDir().empty(); }
std::string autosaveWritePath() { return prefDir() + "autosave-new.sfproj"; }
void commitAutosave(const std::string& path) {
    std::error_code ec;
    std::filesystem::rename(fsPath(path), fsPath(autosaveFile()), ec); // (replaces the old one in one step)
    if (ec) {
        gStatus = "error";
        std::lock_guard<std::mutex> lock(gLock);
        gNotices.push_back("autosave: " + ec.message());
        return;
    }
    const std::time_t now = std::time(nullptr);
    char text[16];
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(text, sizeof text, "%H:%M", &local);
    gStatus = text;
}
std::string autosaveStatus() { return gStatus; }
std::string autosaveLocation() { return autosaveFile(); }

std::string exportPath(const std::string& fileName) {
    std::string dir = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS) ? SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS) : prefDir();
    // "Name.glb", else "Name (2).glb"... (never over an existing file)
    const size_t dot = fileName.find_last_of('.');
    const std::string stem = fileName.substr(0, dot), ext = dot == std::string::npos ? "" : fileName.substr(dot);
    std::error_code ec;
    std::string path = dir + fileName;
    for (int n = 2; std::filesystem::exists(fsPath(path), ec) && n < 1000; ++n)
        path = dir + stem + " (" + std::to_string(n) + ")" + ext;
    return path;
}
std::string deliverExport(const std::string& path, const std::string&, const char*, const char*) { return path; }

void pickFile(const char*, const char*, const char*) {
    static const SDL_DialogFileFilter filters[] = {
        {"Models and projects (sfproj, glb, gltf, obj, stl)", "sfproj;glb;gltf;obj;stl"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(dialogDone, nullptr, gWindow, filters, 2, nullptr, false);
}

void dropFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(gLock);
    gQueue.push_back({Kind::Import, path});
}

bool fullscreenAvailable() { return gWindow != nullptr; }
bool runningAsApp() { return false; }
bool isFullscreen() { return gWindow && (SDL_GetWindowFlags(gWindow) & SDL_WINDOW_FULLSCREEN) != 0; }
void toggleFullscreen(const char*, const char*, const char*) {
    if (gWindow) SDL_SetWindowFullscreen(gWindow, !isFullscreen());
}

std::string loadSetting(const std::string& name) {
    std::string out;
    if (SDL_IOStream* f = SDL_IOFromFile((prefDir() + name).c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = SDL_ReadIO(f, buf, sizeof buf)) > 0) out.append(buf, n);
        SDL_CloseIO(f);
    }
    return out;
}

void saveSetting(const std::string& name, const std::string& text) {
    // Written aside, then put in place (a crash mid-write leaves the old file).
    const std::string path = prefDir() + name, part = path + ".part";
    SDL_IOStream* f = SDL_IOFromFile(part.c_str(), "wb");
    if (!f) return;
    const bool ok = SDL_WriteIO(f, text.data(), text.size()) == text.size();
    SDL_CloseIO(f);
    std::error_code ec;
    if (ok) std::filesystem::rename(fsPath(part), fsPath(path), ec);
}

#endif

// --- When to autosave ---------------------------------------------------------------------------------------------

void Autosaver::tick(SculptView& view, double nowMs) {
    if (view.revision() != seen_) {
        seen_ = view.revision();
        changedAt_ = nowMs;
    }
    if (!enabled || view.revision() == saved_ || view.stroking() || view.benchmarking() || restoring()) return;
    // Still for a moment: half a second, longer for a model that takes long to save (up to 3 s), so a save rarely
    // lands between two quick strokes. (A page closed or reloaded within that moment loses the last stroke: the
    // browser stops the write when the page goes; one sent to the background keeps it.)
    if (nowMs - changedAt_ < std::clamp(saveMs_ * 5.0, 500.0, 3000.0)) return;
    if (nowMs - savedAt_ < std::max(2000.0, saveMs_ * 20.0)) return; // a slow save waits longer
    save(view, nowMs);
}

void Autosaver::saveNow(SculptView& view, double nowMs) {
    if (!enabled || view.revision() == saved_ || view.benchmarking() || restoring()) return;
    if (view.stroking()) view.endStroke(); // (the page is going away mid-stroke: keep what is there)
    save(view, nowMs);
}

void Autosaver::save(SculptView& view, double nowMs) {
    if (view.subtoolCount() == 0 || view.triangleCount() == 0) return; // (never write an empty scene over the work)
    if (!autosaveAvailable()) {
        saved_ = view.revision(); // (nowhere to keep it: don't try again every frame)
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::string path = autosaveWritePath();
    std::string error;
    if (!view.saveProject(path, error)) {
        SDL_Log("autosave: %s", error.c_str());
        savedAt_ = nowMs; // (try again later, not every frame)
        return;
    }
    commitAutosave(path);
    saveMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    savedAt_ = nowMs;
    saved_ = view.revision();
    SDL_Log("autosave: %.0f ms", saveMs_);
}

} // namespace files
