// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// The 3D view: a model made and sculpted by upstream's core (sf::Document: Mesh, BVH, Sculptor, undo), drawn by
// upstream's renderer (sf::Renderer: clusters, culling, MatCap with cavity shading) through the GLES 3.0 / WebGL2
// stand-ins of render/GLCompat.h. The tools of Brushes.h (upstream's brushes and the Mask brush), each with its own
// settings; the caller (main.cpp) decides which pointer sculpts and which turns the view.
#pragma once

#include "Brushes.h"

#include "core/Document.h"
#include "core/Stroke.h"
#include "render/Camera.h"
#include "render/Renderer.h"

#include <array>
#include <cstdint>
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
    // The shape a Mask Lasso / Mask Rectangle stroke has drawn so far (window units; a rectangle as its 4 corners).
    std::vector<glm::vec2> maskShape() const;

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
    void dabAt(const glm::vec3& center, const glm::vec3& viewDir, float pressure);
    // The stroke drives (Brushes.h Drive) after the first sample.
    void pullTo(const glm::vec3& origin, const glm::vec3& dir, float pressure);
    void elasticTo(const glm::vec3& origin, const glm::vec3& dir, float pressure);
    void turnTo(float x, float pressure);
    // Where the ray meets the plane through `through` facing the view; false when they run parallel.
    bool viewPlanePoint(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& through, glm::vec3& out) const;
    void syncScene();
    // Draws and waits for the GPU (a 1-pixel read back); returns the milliseconds.
    double timedFrame(int pixelWidth, int pixelHeight);

    sf::Document doc_;
    sf::Renderer renderer_;
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
    float shapeW_ = 1.0f, shapeH_ = 1.0f;
    void applyMaskShape();
    glm::vec3 lastCenter_{0.0f};
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
