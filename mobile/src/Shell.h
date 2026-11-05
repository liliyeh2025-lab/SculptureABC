// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// The tablet shell (after Nomad Sculpt / ZBrush for iPad): the model fills the screen, thin bars round
// it, and the longer lists open as floating panels that close again when the view is touched.
//   top      File panel, Topology panel, model size, Full screen, Settings panel
//   left     Size and Strength (tall thumb sliders), Undo, Redo, Invert, the Brush settings panel
//   right    Mirror, Wireframe, Mask panel (mask tools and operations), Hide panel (face groups and hiding), Parts
//            panel (the separate meshes, the Gizmo), Layers panel (sculpt layers), View panel (front / side / top...),
//            Reset view
//   bottom   the favourite tools (icon + name; the current one too when it isn't a favourite), and the Brushes
//            panel (every tool by group, with what each one does). Tapping the current tool opens its settings.
#pragma once

#include <string>
#include <vector>

struct ImFont;
struct ImDrawList;
struct ImVec2;
struct BrushPrefs;
struct ToolDef;
namespace sf {
struct PressurePoints;
}
class SculptView;
namespace files {
class Autosaver;
}
class ViewportInput;
class InputProbe;
struct ProbeInfo;

struct ShellContext {
    SculptView& view;
    ViewportInput& input;
    InputProbe& probe;
    ProbeInfo& info;
    std::string& status; // the last error of an action (shown in the top bar)
    int& diagIn;         // diagnostics: frames until they run (-1 none)
    double nowMs;
    std::string& notice;  // a passing message in the top bar ("Exported ..."), shown until noticeUntil
    double& noticeUntil;
    // Shows `text` in the top bar for a few seconds.
    void notify(const std::string& text) {
        notice = text;
        noticeUntil = nowMs + 5000.0;
    }
};

class Shell {
public:
    // Inter for Latin, the Noto Sans TC subset for Chinese, Lucide for icons. `assetDir` holds upstream's fonts,
    // `mobileDir` ours (mobile/). Sizes in window units; fonts are rasterised at device pixels.
    void loadFonts(float uiScale, float pixelDensity, const std::string& assetDir, const std::string& mobileDir);
    void applyStyle(float uiScale);

    void draw(ShellContext& c);

    // On a bar or an open panel (as drawn last frame): the pointer belongs to the interface.
    bool hits(float x, float y) const;
    bool panelOpen() const { return panel_ != Panel::None; }
    void closePanel() { panel_ = Panel::None; }
    // Opens a panel by name ("file", "settings", "view", "brushes", "brush", "mask", "groups", "parts", "layers", "topology"): the ?panel= start
    // option,
    // for tests.
    void openPanel(const std::string& name);

    bool showProbe = false;
    bool showStats = false;

private:
    enum class Panel { None, File, Settings, View, Brushes, BrushSettings, Mask, Groups, Parts, Layers, Topology, Color, Material };
    struct Rect {
        float x, y, w, h;
    };
    void remember(); // records the current ImGui window's rectangle for hits()
    void togglePanel(Panel p) { panel_ = panel_ == p ? Panel::None : p; }

    void topBar(ShellContext& c);
    void leftBar(ShellContext& c);
    void rightBar(ShellContext& c);
    void brushBar(ShellContext& c);
    void filePanel(ShellContext& c);
    void settingsPanel(ShellContext& c);
    void viewPanel(ShellContext& c);
    void brushesPanel(ShellContext& c);
    void brushSettingsPanel(ShellContext& c);
    void maskPanel(ShellContext& c);
    void groupsPanel(ShellContext& c);
    void partsPanel(ShellContext& c);
    void layersPanel(ShellContext& c);
    // Subdivision levels, dynamic topology, Voxel Remesh and Decimate (from the top bar).
    void topologyPanel(ShellContext& c);
    // The paint colour (a wheel and swatches), the eyedropper, fills and the paint layers (from the left strip).
    void colorPanel(ShellContext& c);
    // MatCaps, or realistic (PBR) shading with an environment (HDRI), from the top bar.
    void materialPanel(ShellContext& c);
    // The Gizmo tool: its handles (drawn behind the interface) and the Move / Rotate / Scale buttons under it.
    void gizmoDraw(ShellContext& c);
    void gizmoBar(ShellContext& c);
    // Cut Polyline: Done and Cancel while its corners are being placed.
    void polyBar(ShellContext& c);
    // The brush settings' stroke modes (Dots, Line, Slide / Drag Stamp, Spray).
    void strokeSection(const ToolDef& d, BrushPrefs& b, float inner);
    // Renaming a sculpt layer (kind 1) or a Part (kind 2): the web page's text box, or an ImGui field on Windows.
    void startRename(int kind, int index, const std::string& current);
    void renameBox(ShellContext& c);
    // The pressure curve: drag a point, tap the graph to add one; the selected inner point can be removed.
    void curveEditor(sf::PressurePoints& curve, float w, float h);
    void selectTool(ShellContext& c, int tool);
    // Inside a panel: a finger dragging its empty space scrolls it.
    void touchScroll();
    void brushCircle(ShellContext& c);
    // The alpha picker (upstream's built-in alphas as a grid of pictures) and the alpha options.
    void alphaSection(BrushPrefs& b, float inner);
    // The alpha inside the brush circle, turned the way it stamps, with a mark at its top.
    void alphaMark(ShellContext& c, ImDrawList* dl, const ImVec2& center, float radius);
    // A built-in alpha's picture (a GL texture, white with the alpha as opacity), made on first use.
    unsigned alphaTexture(int builtin);
    // The File panel's actions. Replacing the model asks first when that would lose work.
    enum class Replace { None, Sphere, Lion, Import, Starter };
    void replaceModel(ShellContext& c, Replace what);
    void exportAs(ShellContext& c, const char* extension);

    // A square-ish button: the icon above, a short label below. Returns true when tapped.
    bool tile(const char* id, const char* icon, const char* label, float w, float h, bool active = false, bool enabled = true);
    // A wide button with the icon left of the label (bars and panel lists).
    bool wideButton(const char* id, const char* icon, const char* label, float w, float h, bool active = false,
                    bool enabled = true);

    ImFont* ui_ = nullptr;
    ImFont* small_ = nullptr;
    ImFont* icon_ = nullptr;
    float density_ = 1.0f;
    Panel panel_ = Panel::None;
    std::vector<Rect> rects_, building_;
    float fileButtonX_ = 8, settingsButtonX_ = 0, topologyButtonX_ = 0, materialButtonX_ = 0;
    std::vector<float> paintOpacityBefore_; // a paint layer's opacity before its eye turned it off (by layer)
    int remeshResolution_ = 200; // Voxel Remesh: voxels across the longest side
    float decimateKeep_ = 0.5f;  // Decimate: the share of triangles kept
    bool sizePreview_ = false; // the Size slider is being dragged: show the brush circle in the middle
    Replace confirm_ = Replace::None; // the File panel asks "replace the current work?" for this
    int starter_ = -1;                // the starter model Replace::Starter opens
    int curveDrag_ = -1, curveSelected_ = -1; // the pressure curve's point being dragged / last touched
    std::vector<unsigned> alphaTex_;           // alphaTexture's, by built-in index (0 = not made yet)
    std::vector<float> layerWeightBefore_; // a layer's weight before its eye turned it off (by layer)
    std::string confirmCommand_;               // the Parts panel asks before a command that can't be undone
    bool showGroups_ = false;                  // face group colours (also shown while a group / hide tool is in use)
    int renameKind_ = 0, renameIndex_ = -1;    // what is being renamed (0: nothing)
    bool renameByPage_ = false;                // the web page asks (files::askText)
    char renameText_[128] = {};                // the ImGui field's text
};
