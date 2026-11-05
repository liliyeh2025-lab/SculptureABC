// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// The 3D view: a model made and sculpted by upstream's core (sf::Document: Mesh, BVH, Sculptor, undo), drawn by
// upstream's renderer (sf::Renderer: clusters, culling, MatCap with cavity shading) through the GLES 3.0 / WebGL2
// stand-ins of render/GLCompat.h. The tools of Brushes.h (upstream's brushes and the Mask brush), each with its own
// settings; the caller (main.cpp) decides which pointer sculpts and which turns the view.
#pragma once

#include "Brushes.h"

#include "core/AlphaOptions.h"
#include "core/BrushAlpha.h"
#include "core/Commands.h"
#include "core/Document.h"
#include "core/MeshTransform.h"
#include "core/Stroke.h"
#include "render/Camera.h"
#include "render/GroupOverlay.h"
#include "render/Renderer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class SculptView {
public:
    bool init(std::string& error);
    void shutdown();
    void draw(int pixelWidth, int pixelHeight);

    // Drag in window units; `viewportHeight` scales the turn so a full-height drag is about 180 degrees.
    void rotate(float dx, float dy, float viewportHeight);
    // factor > 1 moves closer.
    void zoom(float factor);
    // Slides the view by a drag in window units (the model follows the fingers).
    void pan(float dx, float dy, float viewportHeight);
    void resetView();
    // Looks along a world axis: 0 front, 1 back, 2 left, 3 right, 4 top, 5 bottom (keeps the distance and target).
    void setView(int which);

    void newSphere();
    void newBenchHead(size_t triangles); // upstream's benchmark head (core/Benchmark.h)
    // The demo model (assets/demo/lion-head.glb). Returns false and sets `error` when it can't be read.
    bool loadLionHead(std::string& error);
    bool loadModel(const std::string& path, std::string& error);
    // Divides the model once (Loop / Catmull-Clark, about 4x the triangles).
    bool subdivide(std::string& error);

    // --- Files (upstream's readers and writers; paths are UTF-8) ---
    // Replaces the scene with a file: a project (.sfproj) restores every SubTool; .glb / .gltf / .obj / .stl open one
    // SubTool per object in the file, as upstream's `open` command does. On failure the scene is untouched.
    bool openFile(const std::string& path, std::string& error);
    // Writes the whole scene as a project (.sfproj, upstream's saveProject).
    bool saveProject(const std::string& path, std::string& error) const;
    // Exports the visible SubTools by the path's extension: .glb, .obj, .stl (millimetres, Z up, for printing) or
    // .sfproj.
    bool exportFile(const std::string& path, std::string& error) const;
    // A file name for exports: the active SubTool's name (letters, digits, - and _, and any non-ASCII text), or "Sculpture".
    std::string fileStem() const;
    // Counts every change to the model (strokes, undo, redo, a new or opened model): autosave compares it.
    uint64_t revision() const { return revision_; }
    // True when replacing the model would lose work (it was sculpted, or came from a file or the autosave): the
    // shell asks first.
    bool replacingLosesWork() const { return editsSinceLoad_ > 0 || openedFile_; }
    int subtoolCount() const { return doc_.subtoolCount(); }

    // Strokes, in window units (x, y from the top left of a w x h window). beginStroke returns false when the
    // point misses the model (the caller then turns the view instead).
    bool beginStroke(float x, float y, float w, float h, float pressure);
    void strokeTo(float x, float y, float w, float h, float pressure);
    void endStroke();
    // Ends the stroke and takes it back (a second finger landed: the touch was a gesture after all).
    void cancelStroke();
    bool stroking() const { return stroking_; }
    // True when the point (window units) lands on the model.
    bool hitsModel(float x, float y, float w, float h) const;

    // --- Mask (each one undo step) ---
    enum class MaskOp { Clear, All, Invert, Blur, Sharpen, Grow, Shrink };
    bool maskOp(MaskOp op);
    bool hasMask() const { return doc_.mesh.hasMask(); }
    // The shape a lasso / rectangle tool's stroke has drawn so far (window units; a rectangle as its 4 corners), or
    // the corners of a polyline being placed.
    std::vector<glm::vec2> maskShape() const;
    // A Line stroke being drawn: from `a` (the press) to `b` (the pen), window units.
    bool strokeLine(glm::vec2& a, glm::vec2& b) const;

    // --- Polyline (Cut Polyline): corners placed tap by tap, between strokes ---
    const std::vector<glm::vec2>& polyline() const { return poly_; }
    static constexpr float kPolyClosePx = 22.0f; // a tap this near the first corner closes the shape
    bool polyCanFinish() const { return poly_.size() >= 3; }
    void polyFinish(); // closes the shape and cuts with it
    void polyClear();

    // --- Face groups and hiding (each one undo step; false when nothing changed) ---
    enum class GroupOp { ShowAll, InvertVisibility, ByNormals, ByParts, FromMask, MergeVisible, Clear };
    bool groupOp(GroupOp op);
    bool hasHidden() const { return doc_.mesh.hasHidden(); }
    bool hasGroups() const { return doc_.mesh.hasPolygroups(); }

    // --- Parts (upstream's SubTools, core/SubTools.h): separate meshes, one sculpted at a time ---
    struct PartInfo {
        std::string name;
        size_t triangles = 0;
        bool visible = true, active = false;
    };
    std::vector<PartInfo> parts() const;
    int activePart() const { return doc_.activeSubtool; }
    // Runs one of upstream's commands (subtool_append, subtool_merge_down...) on the scene. False, with `message`
    // set to upstream's (English) reason, when it didn't.
    bool runCommand(const std::string& name, const sf::Args& args = {}, std::string* message = nullptr);

    // --- Sculpt layers of the active Part (core/Mesh.h SculptLayer): strokes go into the active one ---
    struct LayerInfo {
        std::string name;
        float weight = 1.0f;
    };
    std::vector<LayerInfo> layers() const;
    int activeLayer() const { return doc_.activeLayer; } // -1 = the base
    // How strongly a layer shows (not undoable, as upstream's layer_set_weight). Picking waits for the next stroke
    // to bring the model's search tree up to date.
    void setLayerWeight(int layer, float weight);
    void renameLayer(int layer, const std::string& name); // (not undoable, as upstream's Part rename)

    // --- Painting (upstream's vertex paint, core/Polypaint.h, and paint layers, core/PaintLayer.h) ---
    glm::vec3 paintColor() const; // display (sRGB) 0..1
    void setPaintColor(const glm::vec3& srgb);
    // The eyedropper: the next press on the model takes the colour there (and turns this off).
    bool pickingColor = false;
    bool hasPaint() const { return doc_.mesh.hasColors(); }
    struct PaintLayerInfo {
        std::string name;
        float opacity = 1.0f;
        bool visible = true;
    };
    std::vector<PaintLayerInfo> paintLayers() const; // above the base, bottom first
    int activePaintLayer() const { return doc_.mesh.paintLayers.empty() ? -1 : doc_.mesh.activePaintLayer; } // -1 = base
    // How strongly a paint layer shows, and whether it shows (not undoable, as the sculpt layers' weights).
    void setPaintLayerLook(int layer, float opacity, bool visible);
    // Runs a paint command (paint_fill, paint_layer_add...) and has the colours uploaded again.
    bool paintCommand(const std::string& name, const sf::Args& args = {}, std::string* message = nullptr);

    // --- Look (materials) ---
    unsigned matcapTexture(int index) const { return renderer_.matcapTexture(index); } // a GL texture for previews

    // --- Cut (the Cut tools, upstream's `cut`) ---
    enum class CutMode { Away, In, Add };
    CutMode cutMode = CutMode::Away;
    float cutDepth = 0.1f; // In, Add: a share of the Part's size
    // A message for the top bar ("missed": a cut that missed; "nolayer": Erase Layer with no layer; "picked": the
    // eyedropper took a colour), taken by the shell.
    std::string notice;

    // --- Starter models (upstream's core/StarterMeshes.h, built into the program) ---
    struct StarterInfo {
        std::string name, what;
    };
    std::vector<StarterInfo> starters() const;
    // Replaces the scene with starter model `index` (divided as it comes).
    bool newStarter(int index, std::string& error);

    // --- Topology of the active Part ---
    // Subdivision levels (core/Multires.h): how many (0 = none) and the one shown (0-based).
    int subdivLevels() const;
    int subdivLevel() const;
    size_t subdivLevelTriangles(int level) const;
    // Dynamic topology (core/Dyntopo.h): while on, every dab remeshes the surface under the brush first.
    sf::DyntopoSettings& dyntopo() { return doc_.sculptor.dyntopo; }
    // Why dynamic topology can't remesh the active Part ("" when it can).
    std::string dyntopoBlocker() const;

    // --- Gizmo (the Gizmo tool): handles on the active Part's centre, or on its unmasked part's ---
    enum class GizmoMode { Move, Rotate, Scale };
    GizmoMode gizmoMode = GizmoMode::Move;
    // Where the handles are on a w x h window (window units). Handles 0..2 are the X, Y, Z axes: arrows (Move),
    // rings (Rotate) or end squares (Scale); handle 3 is the centre (Move on the view plane, Scale evenly) or, for
    // Rotate, the outer circle (a turn about the line of sight).
    struct GizmoView {
        glm::vec2 center{0.0f};
        glm::vec2 tip[3] = {};
        std::vector<glm::vec2> ring[3];
        float viewRingPx = 0.0f;
        int hot = -1; // the handle being dragged
    };
    bool gizmo(float w, float h, GizmoView& out) const;

    bool undo();
    bool redo();
    bool canUndo() const { return doc_.undo.canUndo(); }
    bool canRedo() const { return doc_.undo.canRedo(); }

    // The tool (an index into toolDefs()) and every tool's own settings (the shell edits them). The size is on
    // screen, as in Nomad: zooming in makes the brush finer.
    int tool = 0;
    BrushBook brushes;
    const ToolDef& toolDef() const { return toolDefs()[size_t(tool)]; }
    BrushPrefs& prefs() { return brushes.at(tool); }
    const BrushPrefs& prefs() const { return brushes.at(tool); }
    // While a stroke runs: where the brush is on screen (behind the pen with Steady Stroke) and its radius there
    // (pen pressure may shrink it).
    bool strokePoint(float& x, float& y, float& radiusPx) const;
    // Whether the current tool stamps an alpha (not Round), and the way its top points on screen: radians, 0 = up,
    // counter-clockwise (while a stroke runs, the last dab's; otherwise the brush's fixed angle).
    bool alphaShown() const;
    float alphaAngle() const;
    bool invert = false;
    bool symmetryX = true;
    sf::RenderSettings settings;

    int triangleCount() const { return int(doc_.mesh.triangleCount()); }
    int vertexCount() const { return int(doc_.mesh.vertexCount()); }
    int strokeDabs() const { return dabsInStroke_; }
    float lastDabMs() const { return lastDabMs_; }
    float lastDrawMs() const { return lastDrawMs_; }     // CPU time of the last Renderer::draw
    float lastUploadMs() const { return lastUploadMs_; } // CPU time of the last Renderer::syncScene
    int drawnTriangles() const { return int(renderer_.lastDrawnTriangles()); }

    // Speed test (1M / 3M / 5M triangle heads): one step per call, so the page keeps drawing between steps.
    // Returns true while there is more to do; the results build up in benchmarkLines().
    void startBenchmark();
    bool benchmarking() const { return benchStep_ >= 0; }
    bool benchmarkStep(int pixelWidth, int pixelHeight);
    const std::vector<std::string>& benchmarkLines() const { return benchLines_; }

    // Diagnostics (?diag=1): a plain compute loop, raycasts and 100 dabs on the current model, timed one by one, to
    // tell a generally slow WebAssembly engine from a slow brush. Results go to diagnosticLines() and the log.
    void runDiagnostics(int pixelWidth, int pixelHeight);
    const std::vector<std::string>& diagnosticLines() const { return diagLines_; }

private:
    // The brush radius in world units at `point`: radiusPx through the camera, or (speed test, diagnostics) a fixed
    // share of the model so their numbers compare across screens.
    float worldRadius(const glm::vec3& point) const;
    void setModel(sf::Mesh mesh, const std::string& name);
    void modelReplaced(); // after the document got a new scene: normals, bounds, view, counters
    float modelSize() const;
    // A dab's parameters for the current tool at `center` with raw pen pressure `pressure`.
    sf::DabParams dabParams(const glm::vec3& center, const glm::vec3& viewDir, float pressure) const;
    sf::Symmetry symmetry() const;
    // `screen`: where the dab is on screen (window units), for the alpha following the stroke.
    void dabAt(const glm::vec3& center, const glm::vec3& viewDir, float pressure, glm::vec2 screen);
    // Gives a dab the stroke's alpha, turned by the brush's alpha options.
    void orientAlpha(sf::DabParams& p, glm::vec2 screen);
    // The stroke drives (Brushes.h Drive) after the first sample.
    void pullTo(const glm::vec3& origin, const glm::vec3& dir, float pressure, glm::vec2 screen);
    void elasticTo(const glm::vec3& origin, const glm::vec3& dir, float pressure);
    void turnTo(float x, float pressure);
    // Stroke modes (BrushPrefs::stroke) of the Dabs drive.
    void sprayAt(glm::vec2 screen, float pressure); // one scattered dab round a point of the path
    void slideTo(float x, float y, float pressure);   // Slide Stamp: the one stamp, put back and pressed again here
    void pullStampTo(float x, float y);               // Drag Stamp: the one stamp, sized and turned by the drag
    void lineDabs();                                  // Line: dabs along the line, on release
    // Erase Layer: the active layer's offsets under the dab (and its mirror) shrink towards none.
    void eraseLayerAt(const sf::DabParams& p);
    bool screenHit(float x, float y, sf::RayHit& hit) const; // the stroke's window (shapeW_ x shapeH_), near silhouettes too
    // Where the ray meets the plane through `through` facing the view; false when they run parallel.
    bool viewPlanePoint(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& through, glm::vec3& out) const;
    void syncScene();
    // Draws and waits for the GPU (a 1-pixel read back); returns the milliseconds.
    double timedFrame(int pixelWidth, int pixelHeight);

    sf::Document doc_;
    sf::Renderer renderer_;
    GroupOverlay groupOverlay_; // face group colours (settings.showPolygroups), see GroupOverlay.h
    sf::Camera camera_;
    bool ready_ = false;
    sf::AABB bounds_;
    int width_ = 1, height_ = 1;
    float viewHeight_ = 1.0f; // the window's height in window units (worldRadius' pixels), from the last stroke

    float fixedRadius_ = 0.0f; // > 0: worldRadius() is this share of the model's size
    bool stroking_ = false;
    Drive drive_ = Drive::Dabs; // the running stroke's
    sf::LazyMouse lazy_;        // Steady Stroke
    float strokeX_ = 0.0f, strokeY_ = 0.0f, strokeRadiusPx_ = 0.0f;
    glm::vec3 grabLast_{0.0f};  // Move, Pull, Elastic: the last point on the drag plane
    glm::vec3 turnCenter_{0.0f};
    float kelvinRadius_ = 0.0f; // Elastic, Turn: the brush radius at the press (world units)
    float turnLastX_ = 0.0f;
    std::vector<glm::vec2> shape_; // Lasso / Rect: the points drawn (window units)
    StrokeMode mode_ = StrokeMode::Dots; // the running Dabs stroke's
    glm::vec2 anchor_{0.0f}, lineEnd_{0.0f}; // the press on screen (Line, Drag Stamp); the line's other end
    glm::vec3 anchorHit_{0.0f};              // the press on the model
    bool stamped_ = false;                   // Drag Stamp: the drag has pressed a stamp
    float strokePeak_ = 0.0f;                // the strongest pen pressure of the stroke (Line, Drag Stamp)
    sf::Spray spray_;
    float dabScale_ = 1.0f, dabStrengthScale_ = 1.0f, dabTurn_ = 0.0f; // Spray: this dab's size, strength, turn
    std::vector<glm::vec2> poly_;  // Cut Polyline: the corners so far (window units)
    int polyTool_ = -1;            // the tool they belong to
    bool pickColor_ = false;       // the running press is the eyedropper's
    // A Paint stroke on a mesh without paint yet: the stack as it was, so undo takes the paint away again.
    std::unique_ptr<sf::PaintStackUndo> paintBefore_;
    bool polyClosing_ = false;     // the running press is on the first corner
    float shapeW_ = 1.0f, shapeH_ = 1.0f;
    void applyMaskShape();
    void applyHideShape();
    void applyGroupShape();
    void applyPick(); // Pick Group, on release
    void applyCut();  // the Cut tools, on release
    void updateBounds(); // bounds_ from the Parts that show (without moving the view)
    void sceneChanged(); // after a command, undo or redo: bounds, counters, the group overlay

    sf::CommandRegistry commands_;
    // Gizmo drag
    int gizmoHit(const GizmoView& g, float x, float y) const;
    void gizmoTo(float x, float y);
    glm::vec3 gizmoPivot() const;
    sf::TransformSession transform_;
    int gizmoHandle_ = -1;
    GizmoView gizmoStart_;
    glm::vec2 gizmoPress_{0.0f};
    glm::vec3 gizmoPivotStart_{0.0f}, gizmoShown_{0.0f};
    float gizmoLength_ = 1.0f; // the arrows' length in world units at the press
    float gizmoAngle_ = 0.0f, gizmoLastAngle_ = 0.0f, gizmoSign_ = 1.0f; // Rotate: turned so far (screen), direction
    mutable glm::vec3 pivot_{0.0f};
    mutable uint64_t pivotRevision_ = ~uint64_t(0);
    mutable int pivotPart_ = -1;
    uint32_t pickTriangle_ = 0;
    // Writes per-triangle visibility / groups with one undo record of the triangles that change.
    bool commitHidden(const std::vector<uint8_t>& proposed, const char* label);
    bool commitGroups(const std::vector<uint32_t>& proposed, const char* label);
    // The shape drawn so far in normalised device coordinates, with the matching view-projection.
    std::vector<glm::vec2> shapeNdc(glm::mat4& viewProj);
    glm::vec3 lastCenter_{0.0f};
    glm::vec2 lastScreen_{0.0f};   // the last dab's point on screen
    bool alphaOn_ = false;         // the running stroke stamps alpha_
    sf::BrushAlpha alpha_;         // its alpha, with the brush's tiling
    sf::AlphaRake rake_;           // Follow stroke direction
    uint32_t alphaSeed_ = 0;       // Angle jitter
    float alphaAngle_ = 0.0f;      // the last dab's (radians)
    float lastPressure_ = 1.0f;
    int dabsInStroke_ = 0;
    float lastDabMs_ = 0.0f, lastDrawMs_ = 0.0f, lastUploadMs_ = 0.0f;

    uint64_t revision_ = 0;
    int editsSinceLoad_ = 0;
    bool openedFile_ = false;

    int benchStep_ = -1;
    int benchSavedTool_ = 0;
    BrushPrefs benchSavedPrefs_;
    std::vector<std::string> benchLines_;
    std::vector<std::string> diagLines_;
};
