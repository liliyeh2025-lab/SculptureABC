// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "Brushes.h"

#include "core/BrushAlpha.h"

#include <IconsLucide.h>

#include <algorithm>
#include <cstring>

namespace {

using sf::BrushType;

// The order inside each group is the Brushes panel's.
const std::vector<ToolDef> kTools = {
    // Sculpt: build up and carve
    {"Standard", BrushType::Standard, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_CIRCLE_DOT, "Standard",
     "Pushes the surface out. Invert digs in.", 0.5f},
    {"Clay", BrushType::Clay, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_LAYERS, "Clay",
     "Builds up flat layers of clay, filling the dents first.", 0.5f},
    {"ClayBuildup", BrushType::ClayBuildup, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_BOX, "Clay Block",
     "Lays on square-edged, flat-topped strips of clay.", 0.5f},
    {"Inflate", BrushType::Inflate, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_BALLOON, "Inflate",
     "Swells the surface outwards in every direction.", 0.4f},
    {"Layer", BrushType::Layer, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_LAYERS_2, "Raise",
     "Raises the surface to one even height a stroke; going over it again adds nothing.", 0.5f},
    {"Crease", BrushType::Crease, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_SLASH, "Crease",
     "Cuts a sharp groove. Invert raises a ridge.", 0.5f},
    {"DamStandard", BrushType::DamStandard, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_PEN_TOOL, "Sharp Crease",
     "A deep V-shaped cut with its sides pinched in: wrinkles, seams, eyelids.", 0.5f},
    {"Stamp", BrushType::Standard, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_STAMP, "Stamp",
     "Press and drag to pull one stamp of the alpha out: the drag sets its size and which way it faces.", 0.6f},
    {"EraseLayer", BrushType::Standard, false, Drive::Dabs, ToolGroup::Sculpt, ICON_LC_ERASER, "Erase Layer",
     "Rubs out what the active sculpt layer added under the brush (pick the layer in the Layers panel).", 0.5f,
     Shape::Mask, nullptr, true},
    // Smooth and flatten
    {"Smooth", BrushType::Smooth, false, Drive::Dabs, ToolGroup::Flatten, ICON_LC_WAVES, "Smooth",
     "Evens out bumps and rough spots.", 0.6f},
    {"Flatten", BrushType::Flatten, false, Drive::Dabs, ToolGroup::Flatten, ICON_LC_EQUAL, "Flatten",
     "Presses the surface onto a plane.", 0.5f},
    {"Trim", BrushType::Trim, false, Drive::Dabs, ToolGroup::Flatten, ICON_LC_SCISSORS, "Trim",
     "Shaves off whatever stands above a plane: flat cuts and hard edges.", 0.5f},
    {"Pinch", BrushType::Pinch, false, Drive::Dabs, ToolGroup::Flatten, ICON_LC_SHRINK, "Pinch",
     "Pulls the surface towards the stroke, sharpening edges.", 0.5f},
    // Move and pull
    {"Move", BrushType::Move, false, Drive::Grab, ToolGroup::Drag, ICON_LC_MOVE, "Move",
     "Takes hold of the area under the brush and drags it along.", 1.0f},
    {"KelvinGrab", BrushType::KelvinGrab, false, Drive::Elastic, ToolGroup::Drag, ICON_LC_HAND, "Elastic Grab",
     "Drags the area like soft rubber: the surroundings follow smoothly.", 0.5f},
    {"SnakeHook", BrushType::SnakeHook, false, Drive::Pull, ToolGroup::Drag, ICON_LC_SPLINE, "Tendril",
     "Pulls horns, spikes and tentacles out of the surface.", 0.5f},
    {"KelvinSnake", BrushType::KelvinSnake, false, Drive::Pull, ToolGroup::Drag, ICON_LC_WORM, "Elastic Tendril",
     "Pulls out smooth shapes that taper to a point.", 0.5f},
    {"Nudge", BrushType::Nudge, false, Drive::Dabs, ToolGroup::Drag, ICON_LC_ARROW_RIGHT_LEFT, "Nudge",
     "Slides the surface sideways along the stroke.", 0.5f},
    {"KelvinTwist", BrushType::KelvinTwist, false, Drive::Turn, ToolGroup::Drag, ICON_LC_ROTATE_CW, "Twist",
     "Press and drag sideways to twist the area round.", 0.5f},
    {"KelvinScale", BrushType::KelvinScale, false, Drive::Turn, ToolGroup::Drag, ICON_LC_SCALE_3D, "Elastic Scale",
     "Press and drag sideways: right swells the area, left shrinks it.", 0.5f},
    {"Gizmo", BrushType::Standard, false, Drive::Gizmo, ToolGroup::Drag, ICON_LC_MOVE_3D, "Gizmo",
     "Move, turn or resize the active Part with the handles (only the unmasked part, when there is a mask). The buttons under it switch between them.",
     1.0f},
    // Mask
    {"Mask", BrushType::Standard, true, Drive::Dabs, ToolGroup::Mask, ICON_LC_PAINTBRUSH, "Mask",
     "Paints a mask: masked areas turn dark and the other brushes leave them alone. Invert erases it.", 1.0f},
    {"MaskLasso", BrushType::Standard, true, Drive::Lasso, ToolGroup::Mask, ICON_LC_LASSO, "Mask Lasso",
     "Draw round an area to mask it, through to the back. Erase unmasks. Drawing round empty space clears the mask.", 1.0f},
    {"MaskRect", BrushType::Standard, true, Drive::Rect, ToolGroup::Mask, ICON_LC_SQUARE_DASHED, "Mask Rectangle",
     "Drag a rectangle to mask what is inside it. Erase unmasks.", 1.0f},
    // Face groups and hiding
    {"HideLasso", BrushType::Standard, false, Drive::Lasso, ToolGroup::Hide, ICON_LC_LASSO_SELECT, "Hide Lasso",
     "Draw round an area to hide it, through to the back. With Keep, only the inside stays. Drawing round empty space shows everything.",
     1.0f, Shape::Hide, "Keep"},
    {"HideRect", BrushType::Standard, false, Drive::Rect, ToolGroup::Hide, ICON_LC_BOX_SELECT, "Hide Rectangle",
     "Drag a rectangle to hide what is inside it. With Keep, only the inside stays.", 1.0f, Shape::Hide, "Keep"},
    {"GroupLasso", BrushType::Standard, false, Drive::Lasso, ToolGroup::Hide, ICON_LC_SHAPES, "Group Lasso",
     "Draw round an area to make it a face group of its own, through to the back.", 1.0f, Shape::Group},
    {"GroupPick", BrushType::Standard, false, Drive::Pick, ToolGroup::Hide, ICON_LC_MOUSE_POINTER_CLICK, "Pick Group",
     "Tap a face group to show it alone; tap it again to show everything. With Hide, the tap hides the group.", 1.0f,
     Shape::Mask, "Hide"},
    // Cut
    {"CutLasso", BrushType::Standard, false, Drive::Lasso, ToolGroup::Cut, ICON_LC_LASSO, "Cut Lasso",
     "Draw round an area: it is cut away through the whole Part and the hole is closed flat (or dug in, or added; see the brush settings).",
     1.0f, Shape::Cut},
    {"CutRect", BrushType::Standard, false, Drive::Rect, ToolGroup::Cut, ICON_LC_SQUARE_DASHED, "Cut Rectangle",
     "Drag a rectangle: what it covers is cut away through the whole Part (or dug in, or added).", 1.0f, Shape::Cut},
    {"CutLine", BrushType::Standard, false, Drive::Line, ToolGroup::Cut, ICON_LC_SCISSORS, "Cut Line",
     "Draw a straight line: everything on its left (seen along the line) is cut off. Other side flips it.", 1.0f, Shape::Cut,
     "Other side"},
    {"CutPolyline", BrushType::Standard, false, Drive::Poly, ToolGroup::Cut, ICON_LC_PENCIL_RULER, "Cut Polyline",
     "Tap corner after corner (drag to place each one); tap the first corner again, or Done, to cut. Straight edges and sharp corners.",
     1.0f, Shape::Cut},
    // Paint
    {"Paint", BrushType::Standard, false, Drive::Dabs, ToolGroup::Paint, ICON_LC_BRUSH, "Paint",
     "Paints the colour onto the surface. Invert paints back to white. Masked areas stay as they are.", 1.0f,
     Shape::Mask, nullptr, false, true},
};

float clampf(float v, float lo, float hi) { return std::clamp(v, lo, hi); }

} // namespace

const std::vector<ToolDef>& toolDefs() { return kTools; }
int toolCount() { return int(kTools.size()); }

int toolIndex(const char* key) {
    for (size_t i = 0; i < kTools.size(); ++i)
        if (std::strcmp(kTools[i].key, key) == 0) return int(i);
    return -1;
}

int toolIndex(sf::BrushType type) {
    for (size_t i = 0; i < kTools.size(); ++i)
        if (!kTools[i].mask && kTools[i].type == type) return int(i);
    return 0;
}

bool toolTakesAlpha(const ToolDef& d) {
    return !d.mask && !d.layerEraser && (d.drive == Drive::Dabs || d.drive == Drive::Pull);
}

bool toolTakesStroke(const ToolDef& d) { return d.drive == Drive::Dabs; }

bool toolTakesStrokeMode(const ToolDef& d, StrokeMode m) {
    if (!toolTakesStroke(d)) return m == StrokeMode::Dots;
    if (d.mask || d.layerEraser) return m == StrokeMode::Dots || m == StrokeMode::Line || m == StrokeMode::Spray;
    return true;
}

const char* strokeModeKey(StrokeMode m) {
    switch (m) {
    case StrokeMode::Line: return "line";
    case StrokeMode::DragDot: return "drag_dot";
    case StrokeMode::DragRect: return "drag_rect";
    case StrokeMode::Spray: return "spray";
    default: return "dots";
    }
}

const char* strokeModeName(StrokeMode m) {
    switch (m) {
    case StrokeMode::Line: return "Line";
    case StrokeMode::DragDot: return "Slide Stamp";
    case StrokeMode::DragRect: return "Drag Stamp";
    case StrokeMode::Spray: return "Spray";
    default: return "Dots";
    }
}

const char* strokeModeIcon(StrokeMode m) {
    switch (m) {
    case StrokeMode::Line: return ICON_LC_MINUS;
    case StrokeMode::DragDot: return ICON_LC_MOUSE_POINTER_2;
    case StrokeMode::DragRect: return ICON_LC_STAMP;
    case StrokeMode::Spray: return ICON_LC_SPRAY_CAN;
    default: return ICON_LC_ELLIPSIS;
    }
}

const char* toolGroupName(ToolGroup g) {
    switch (g) {
    case ToolGroup::Sculpt: return "Sculpt";
    case ToolGroup::Flatten: return "Smooth and flatten";
    case ToolGroup::Drag: return "Move and pull";
    case ToolGroup::Mask: return "Mask";
    case ToolGroup::Hide: return "Face groups and hiding";
    case ToolGroup::Cut: return "Cut";
    case ToolGroup::Paint: return "Paint";
    default: return "";
    }
}

// --- BrushPrefs ---------------------------------------------------------------------------------------------------

void BrushPrefs::clamp() {
    radiusPx = clampf(radiusPx, kBrushMinPx, kBrushMaxPx);
    strength = clampf(strength, 0.0f, 1.0f);
    focal = clampf(focal, -1.0f, 1.0f);
    steadyPx = clampf(steadyPx, 5.0f, 150.0f);
    spacing = clampf(spacing, 0.05f, 1.0f);
    scatter = clampf(scatter, 0.0f, 3.0f);
    if (int(stroke) < 0 || stroke >= StrokeMode::Count) stroke = StrokeMode::Dots;
    curve.sort();
    alphaOptions.clamp();
    const auto& all = sf::BrushAlpha::builtins();
    if (std::none_of(all.begin(), all.end(), [&](const sf::BrushAlpha& a) { return a.name == alpha; })) alpha = "Round";
}

sf::Json BrushPrefs::toJson() const {
    sf::Json j = sf::Json::object();
    j.set("size", double(radiusPx));
    j.set("strength", double(strength));
    j.set("focal", double(focal));
    j.set("pressure_strength", pressureStrength);
    j.set("pressure_size", pressureSize);
    j.set("curve", curve.toJson());
    j.set("steady", steady);
    j.set("steady_px", double(steadyPx));
    j.set("spacing", double(spacing));
    j.set("keep_volume", keepVolume);
    j.set("front_faces_only", frontFacesOnly);
    j.set("connected_only", connectedOnly);
    j.set("alpha", alpha);
    j.set("alpha_follow", alphaOptions.followStroke);
    j.set("alpha_angle", double(alphaOptions.angle));
    j.set("alpha_jitter", double(alphaOptions.jitter));
    j.set("alpha_tile_x", alphaOptions.tileX);
    j.set("alpha_tile_y", alphaOptions.tileY);
    j.set("stroke", std::string(strokeModeKey(stroke)));
    j.set("spray_scatter", double(scatter));
    return j;
}

void BrushPrefs::applyJson(const sf::Json& j) {
    if (!j.isObject()) return;
    const auto num = [&](const char* key, float& v) {
        if (j[key].isNumber()) v = float(j[key].asNumber());
    };
    const auto flag = [&](const char* key, bool& v) {
        if (j[key].isBool()) v = j[key].asBool();
    };
    num("size", radiusPx);
    num("strength", strength);
    num("focal", focal);
    flag("pressure_strength", pressureStrength);
    flag("pressure_size", pressureSize);
    if (j.has("curve")) curve = sf::PressurePoints::fromJson(j["curve"]);
    flag("steady", steady);
    num("steady_px", steadyPx);
    num("spacing", spacing);
    flag("keep_volume", keepVolume);
    flag("front_faces_only", frontFacesOnly);
    flag("connected_only", connectedOnly);
    sf::BrushAlpha known;
    if (j["alpha"].isString() && sf::BrushAlpha::builtin(j["alpha"].asString(), known)) alpha = known.name;
    flag("alpha_follow", alphaOptions.followStroke);
    num("alpha_angle", alphaOptions.angle);
    num("alpha_jitter", alphaOptions.jitter);
    if (j["alpha_tile_x"].isNumber()) alphaOptions.tileX = int(j["alpha_tile_x"].asNumber());
    if (j["alpha_tile_y"].isNumber()) alphaOptions.tileY = int(j["alpha_tile_y"].asNumber());
    if (j["stroke"].isString())
        for (int m = 0; m < int(StrokeMode::Count); ++m)
            if (j["stroke"].asString() == strokeModeKey(StrokeMode(m))) stroke = StrokeMode(m);
    num("spray_scatter", scatter);
    clamp();
}

// --- BrushBook ----------------------------------------------------------------------------------------------------

BrushBook::BrushBook() {
    for (int i = 0; i < toolCount(); ++i) prefs_.push_back(shipped(i));
    favourites = shippedFavourites();
}

BrushPrefs BrushBook::shipped(int tool) {
    BrushPrefs p;
    const ToolDef& d = kTools[size_t(tool)];
    p.strength = d.strength;
    if (d.drive == Drive::Pull) p.spacing = 0.15f;
    if (std::strcmp(d.key, "Stamp") == 0) {
        p.stroke = StrokeMode::DragRect;
        p.alpha = "Rivets";
    }
    return p;
}

std::vector<int> BrushBook::shippedFavourites() {
    std::vector<int> out;
    for (const char* key : {"Standard", "Clay", "Smooth", "Move", "Inflate", "Crease", "Flatten", "Mask", "Paint"}) out.push_back(toolIndex(key));
    return out;
}

sf::Json BrushBook::toJson() const {
    sf::Json j = sf::Json::object();
    j.set("version", 1);
    sf::Json brushes = sf::Json::object();
    for (int i = 0; i < toolCount(); ++i)
        if (!(prefs_[size_t(i)] == shipped(i))) brushes.set(kTools[size_t(i)].key, prefs_[size_t(i)].toJson());
    j.set("brushes", std::move(brushes));
    sf::Json fav = sf::Json::array();
    for (int t : favourites) fav.push(kTools[size_t(t)].key);
    j.set("favourites", std::move(fav));
    return j;
}

void BrushBook::applyJson(const sf::Json& j) {
    if (!j.isObject()) return;
    for (const auto& [key, value] : j["brushes"].members()) {
        const int t = toolIndex(key.c_str());
        if (t >= 0) prefs_[size_t(t)].applyJson(value);
    }
    if (j["favourites"].isArray()) {
        std::vector<int> fav;
        for (const sf::Json& k : j["favourites"].items()) {
            const int t = k.isString() ? toolIndex(k.asString().c_str()) : -1;
            if (t >= 0 && std::find(fav.begin(), fav.end(), t) == fav.end()) fav.push_back(t);
        }
        if (!fav.empty()) favourites = std::move(fav);
    }
}
