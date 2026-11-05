// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// Where files go on each platform. The model itself is read and written by upstream's code through
// SculptView (openFile / saveProject / exportFile); this is only about places and hand-overs:
//   autosave  web: the browser's private file area (OPFS, "autosave.sfproj"); Windows: the user's app-data folder
//   export    web: a download, or the share sheet on a touch device ("Save to Files" on an iPad); Windows: Downloads
//   import    web: the browser's file picker or a file dropped on the page; Windows: the open-file dialog or a drop
// Files coming in (the autosave read back, a picked or dropped file) wait in a queue that the main loop drains with
// takeIncoming(), so they always open between frames.
#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;
class SculptView;

namespace files {

enum class Kind { Restore, Import };
struct Incoming {
    Kind kind = Kind::Import;
    std::string path; // a copy the platform made (web) or the file itself (Windows); UTF-8
};

// Once at start-up. `restore`: read back the last autosave (it arrives through takeIncoming).
void init(SDL_Window* window, bool restore);
// Reads the autosave again (after the speed test replaced the model).
void restoreAgain();
// True until the autosave has been read back (or found missing): autosave waits, so it can't overwrite it first.
bool restoring();
bool takeIncoming(Incoming& out);
// Done with an incoming file: removes the platform's temporary copy.
void release(const Incoming& in);
// A message for the top bar from the platform side (a failed write...), or "".
std::string takeNotice();

// --- Autosave ---
// False where there is nowhere to keep it (a web page that isn't https or localhost has no OPFS).
bool autosaveAvailable();
// Where to write the project now; then commitAutosave() puts it in place.
std::string autosaveWritePath();
void commitAutosave(const std::string& path);
// For the File panel: "" (never saved this session), "saving", "error", or the local time of the last save.
std::string autosaveStatus();
// Where the autosave lives, for the File panel (Windows: the file; web: "").
std::string autosaveLocation();

// --- Export ---
// Where to write an export named `fileName` (e.g. "Sculpture.glb"); then deliverExport() hands it over.
std::string exportPath(const std::string& fileName);
// Web: download or share (`okLabel` / `cancelLabel` for the extra tap a tablet may need); Windows: already in
// Downloads. Returns a message for the top bar ("" when the browser takes it from here).
std::string deliverExport(const std::string& path, const std::string& fileName, const char* okLabel,
                          const char* cancelLabel);

// --- Import ---
// Opens the file picker; the chosen file arrives through takeIncoming(). `message` / labels for the extra tap.
void pickFile(const char* message, const char* okLabel, const char* cancelLabel);
// A file dropped on the window (Windows; the web page handles its own drops).
void dropFile(const std::string& path);

// --- Full screen ---
// Web: the browser's full screen (not offered when the page already runs as a home-screen app); Windows: the
// window. `message` / labels: the extra tap a tablet needs to enter it.
bool fullscreenAvailable();
// The page runs as a home-screen app (already without the browser's bars). Always false on Windows.
bool runningAsApp();
bool isFullscreen();
void toggleFullscreen(const char* message, const char* okLabel, const char* cancelLabel);

// --- A line of text (a layer's or a Part's name) ---
// Web: a box over the page with a text field, which brings up the tablet's keyboard; the answer comes later through
// takeText. Returns false where the shell has to ask itself (Windows: an ImGui text field).
bool askText(const char* title, const std::string& current, const char* okLabel, const char* cancelLabel);
// The answer once there is one: true, with `ok` false when it was cancelled.
bool takeText(std::string& text, bool& ok);

// --- Settings (small text, such as the brush settings) ---
// Web: the site's localStorage (each address keeps its own); Windows: a file in the app-data folder.
// "" when nothing was saved under `name`.
std::string loadSetting(const std::string& name);
void saveSetting(const std::string& name, const std::string& text);

// When to autosave: soon after the model stops changing, and at once when the page goes to the background.
class Autosaver {
public:
    // The model as it is now counts as saved (it came from the autosave, or from a start option).
    void markSaved(uint64_t revision) { saved_ = seen_ = revision; }
    // Every frame: saves once the model has been still for a moment (0.5-3 s). A slow save (a big model) waits
    // longer before the next one, so saving never takes more than about 5% of the time.
    void tick(SculptView& view, double nowMs);
    // Saves now if anything changed (the page is going to the background, the program is quitting).
    void saveNow(SculptView& view, double nowMs);
    bool enabled = true;

private:
    void save(SculptView& view, double nowMs);
    uint64_t saved_ = 0, seen_ = 0;
    double changedAt_ = 0.0, savedAt_ = -1e9, saveMs_ = 0.0;
};

} // namespace files
