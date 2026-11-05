// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// The tools the shell offers (upstream's brushes plus the Mask brush) and the settings each one keeps for itself, as
// in Nomad: size, strength, the edge's softness (upstream's Focal Shift), what pen pressure changes and its curve
// (core/Precision.h PressurePoints), Steady Stroke (core/Stroke.h LazyMouse), dab spacing, the alpha (the stamp's
// shape, core/BrushAlpha.h) with how it turns and repeats (core/AlphaOptions.h), and a few brush options.
// The settings are kept between visits (BrushBook::toJson, saved by main.cpp through files::savePrefs).
#pragma once

#include "core/AlphaOptions.h"
#include "core/Json.h"
#include "core/Precision.h"
#include "core/Sculpt.h"

#include <array>
#include <string>
#include <vector>

// How a tool's stroke turns pointer motion into edits (SculptView).
enum class Drive {
    Dabs,    // dabs along the path (most brushes, Mask)
    Grab,    // Move: holds the region under the brush and drags it on a plane facing the view
    Pull,    // Tendril, Elastic Tendril: dabs that pull the surface along from where the stroke started
    Elastic, // Elastic Grab: a Kelvinlet grab that follows the pointer on a plane facing the view
    Turn,    // Twist, Elastic Scale: act where the stroke began; dragging sideways turns / swells
    Lasso,   // Mask Lasso: draw a shape (on or off the model); on release the vertices inside get masked
    Rect,    // Mask Rectangle: the same with a rectangle from the press to the pen
    Pick,    // Pick Group: a tap on the model acts on the face group under it (on release)
    Gizmo,   // Gizmo: drag a handle to move, turn or resize the active Part (SculptView::gizmo)
    Line,    // Cut Line: a straight line from the press to the pen; on release the side to its left is cut
    Poly,    // Cut Polyline: taps set corners (the pen drags the newest); a tap on the first corner closes and cuts
};

// How a dab tool's stroke places its dabs (upstream's StrokeType, plus a straight line).
enum class StrokeMode {
    Dots,     // evenly spaced dabs along the pen's path
    Line,     // a straight line from the press to the pen, dabbed on release
    DragDot,  // one dab that slides over the surface with the pen and stays where the pen lifts (Slide Stamp)
    DragRect, // one dab pulled out from the press: the drag's length is its size, its direction the alpha's top
    Spray,    // dabs scattered round the path, of random size, strength and turn
    Count
};
const char* strokeModeKey(StrokeMode m);  // stored name
const char* strokeModeName(StrokeMode m); // English; T() translates
const char* strokeModeIcon(StrokeMode m);

enum class ToolGroup { Sculpt, Flatten, Drag, Mask, Hide, Cut, Paint, Count };

// What a Lasso / Rect tool does with its shape.
enum class Shape {
    Mask,  // masks (or with Erase unmasks) what is inside
    Hide,  // hides what is inside (or with Keep, what is outside)
    Group, // makes what is inside a face group of its own
    Cut,   // cuts the Part with the shape (SculptView::cutMode: away, a pocket in, or added out)
};

struct ToolDef {
    const char* key;   // stored name (brush settings, favourites): never changes
    sf::BrushType type;
    bool mask;         // the Mask brush (Sculptor::dabMask) rather than a sculpting brush
    Drive drive;
    ToolGroup group;
    const char* icon;  // Lucide
    const char* name;  // English; T() translates
    const char* what;  // one line for the Brushes panel
    float strength;    // default strength
    Shape shape = Shape::Mask;        // Lasso / Rect tools
    const char* invertName = nullptr; // what the Invert button is called with this tool (null: Invert, or Erase for Mask)
    bool layerEraser = false;         // Erase Layer: dabs take the active sculpt layer's offsets back out
    bool paint = false;               // Paint: dabs blend the paint colour into the vertices (core/Polypaint.h)
};

extern const std::vector<ToolDef>& toolDefs();
int toolCount();
int toolIndex(const char* key); // -1 when unknown
int toolIndex(sf::BrushType type);
const char* toolGroupName(ToolGroup g);
// Whether a tool's dabs take an alpha (the sculpting brushes that stamp dabs; not Mask, Move or the Kelvinlets).
bool toolTakesAlpha(const ToolDef& d);
// Whether a tool offers the stroke modes, and which (the Mask brush and Erase Layer have no stamps).
bool toolTakesStroke(const ToolDef& d);
bool toolTakesStrokeMode(const ToolDef& d, StrokeMode m);

struct BrushPrefs {
    float radiusPx = 50.0f;       // screen size, window units
    float strength = 0.5f;        // 0..1
    float focal = 0.0f;           // edge: -1 hard and broad .. +1 soft and pointed (upstream's Focal Shift)
    bool pressureStrength = true; // pen pressure scales the strength
    bool pressureSize = false;    // ... and / or the size
    sf::PressurePoints curve;     // pen pressure -> brush pressure
    bool steady = false;          // Steady Stroke: the brush trails the pen on a string
    float steadyPx = 30.0f;       // the string's length, window units
    float spacing = 0.25f;        // dab spacing, a fraction of the radius
    bool keepVolume = false;      // Smooth: keep the volume (a Taubin step)
    bool frontFacesOnly = false;  // only the side of the model facing the view
    bool connectedOnly = false;   // only the surface joined to the point under the brush
    std::string alpha = "Round";  // the stamp: one of upstream's built-in alphas by name (Round = no alpha)
    sf::AlphaOptions alphaOptions; // follow the stroke, a fixed angle, a random angle, tiling
    StrokeMode stroke = StrokeMode::Dots;
    float scatter = 1.0f;         // Spray: how far dabs scatter from the path, in brush radii

    bool operator==(const BrushPrefs&) const = default;
    sf::Json toJson() const;
    void applyJson(const sf::Json& j); // lenient: missing keys keep their values, values are clamped
    void clamp();
};

constexpr float kBrushMinPx = 4.0f, kBrushMaxPx = 300.0f;

// Every tool's settings and the brush bar's favourites.
class BrushBook {
public:
    BrushBook();
    BrushPrefs& at(int tool) { return prefs_[size_t(tool)]; }
    const BrushPrefs& at(int tool) const { return prefs_[size_t(tool)]; }
    static BrushPrefs shipped(int tool); // the defaults a tool starts with
    void reset(int tool) { prefs_[size_t(tool)] = shipped(tool); }

    // The brush bar: the tools shown there, in order.
    std::vector<int> favourites;
    static std::vector<int> shippedFavourites();

    sf::Json toJson() const;
    void applyJson(const sf::Json& j);

private:
    std::vector<BrushPrefs> prefs_;
};
