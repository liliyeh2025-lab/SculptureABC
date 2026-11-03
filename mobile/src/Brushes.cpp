// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "Brushes.h"

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
    // Mask
    {"Mask", BrushType::Standard, true, Drive::Dabs, ToolGroup::Mask, ICON_LC_PAINTBRUSH, "Mask",
     "Paints a mask: masked areas turn dark and the other brushes leave them alone. Invert erases it.", 1.0f},
    {"MaskLasso", BrushType::Standard, true, Drive::Lasso, ToolGroup::Mask, ICON_LC_LASSO, "Mask Lasso",
     "Draw round an area to mask it, through to the back. Erase unmasks. Drawing round empty space clears the mask.", 1.0f},
    {"MaskRect", BrushType::Standard, true, Drive::Rect, ToolGroup::Mask, ICON_LC_SQUARE_DASHED, "Mask Rectangle",
     "Drag a rectangle to mask what is inside it. Erase unmasks.", 1.0f},
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

const char* toolGroupName(ToolGroup g) {
    switch (g) {
    case ToolGroup::Sculpt: return "Sculpt";
    case ToolGroup::Flatten: return "Smooth and flatten";
    case ToolGroup::Drag: return "Move and pull";
    case ToolGroup::Mask: return "Mask";
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
    curve.sort();
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
    return p;
}

std::vector<int> BrushBook::shippedFavourites() {
    std::vector<int> out;
    for (const char* key : {"Standard", "Clay", "Smooth", "Move", "Inflate", "Crease", "Flatten", "Mask"}) out.push_back(toolIndex(key));
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
