// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "SculptView.h"

#include "core/Benchmark.h"
#include "core/Dyntopo.h"
#include "core/Multires.h"
#include "core/Polygroups.h"
#include "core/Primitives.h"
#include "core/Project.h"
#include "core/Kelvinlet.h"
#include "core/Masking.h"
#include "core/PaintLayer.h"
#include "core/Polypaint.h"
#include "core/StarterMeshes.h"
#include "core/MeshTransform.h"
#include "core/ScreenSelect.h"
#include "core/Sculpt.h"
#include "core/Stroke.h"
#include "core/SubTools.h"
#include "io/GltfIO.h"
#include "io/ObjIO.h"
#include "io/StlIO.h"

#include <SDL3/SDL_log.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Icosphere subdivisions: 6 = 81,920 triangles, enough to show a stroke's shape.
constexpr int kSphereLevel = 6;

#ifdef SCULPTURE_ASSET_DIR
const std::string kAssetDir = SCULPTURE_ASSET_DIR;
#else
const std::string kAssetDir = "/assets"; // the web build embeds what it needs here
#endif

// The speed test's sizes (upstream's benchmark head, core/Benchmark.h).
constexpr size_t kBenchSizes[] = {1'000'000, 3'000'000, 5'000'000};
constexpr int kBenchFrames = 30, kBenchDabs = 40;

// The extension in lower case, with the dot ("" when there is none).
std::string lowerExt(const std::string& path) {
    const size_t slash = path.find_last_of("/\\"), dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = path.substr(dot);
    for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return ext;
}

std::string heapText() {
#ifdef __EMSCRIPTEN__
    char buf[64];
    std::snprintf(buf, sizeof buf, "heap %.0f MB", double(emscripten_get_heap_size()) / (1024.0 * 1024.0));
    return buf;
#else
    return "";
#endif
}

} // namespace

float SculptView::worldRadius(const glm::vec3& point) const {
    if (fixedRadius_ > 0.0f) return fixedRadius_ * modelSize();
    return std::max(prefs().radiusPx, 1.0f) * camera_.worldPerPixel(point, float(std::max(viewHeight_, 1.0f)));
}

bool SculptView::init(std::string& error) {
    if (!renderer_.init(error)) return false;
    ready_ = true;
    newSphere();
    return true;
}

void SculptView::shutdown() {
    if (ready_) {
        groupOverlay_.release();
        renderer_.shutdown();
    }
    ready_ = false;
}

void SculptView::setModel(sf::Mesh mesh, const std::string& name) {
    if (stroking_) endStroke();
    doc_.setMesh(std::move(mesh), name); // also clears the history
    modelReplaced();
}

void SculptView::modelReplaced() {
    for (int i = 0; i < doc_.subtoolCount(); ++i) {
        sf::Mesh& m = doc_.subtoolMesh(i);
        if (m.normals.size() != m.vertexCount()) m.recomputeAllNormals();
    }
    // Frame everything that shows (a project or a file may hold several SubTools).
    updateBounds();
    groupOverlay_.invalidate();
    resetView();
    ++revision_;
    editsSinceLoad_ = 0;
    openedFile_ = false;
}

void SculptView::newSphere() { setModel(sf::makeIcosphere(kSphereLevel, 1.0f), "Sphere"); }

void SculptView::newBenchHead(size_t triangles) {
    setModel(sf::makeBenchHead(std::max<size_t>(triangles, 1000)), "Head");
}

bool SculptView::loadModel(const std::string& path, std::string& error) {
    sf::Mesh mesh;
    if (!sf::loadGltf(path, mesh, error)) return false;
    if (mesh.triangleCount() == 0) {
        error = "no triangles in " + path;
        return false;
    }
    setModel(std::move(mesh), path.find("lion-head") != std::string::npos ? "Lion head" : "Model");
    return true;
}

bool SculptView::loadLionHead(std::string& error) { return loadModel(kAssetDir + "/demo/lion-head.glb", error); }

bool SculptView::subdivide(std::string& error) {
    if (stroking_) endStroke();
    if (!sf::subdivDivide(doc_.mesh, true, error, &doc_.bvh, size_t(20'000'000))) return false;
    doc_.mesh.topologyDirty = true;
    doc_.bvh.build(doc_.mesh);
    doc_.undo.clear(); // (the steps before refer to the old topology)
    ++revision_;
    ++editsSinceLoad_;
    return true;
}

// --- Files ------------------------------------------------------------------------------------------------------

bool SculptView::openFile(const std::string& path, std::string& error) {
    if (stroking_) endStroke();
    const std::string ext = lowerExt(path);
    if (ext == ".sfproj") {
        if (!sf::loadProject(path, doc_, error)) return false; // (on failure the scene stays as it was)
        doc_.path.clear(); // (names come from the SubTools: the autosave's file name is no name for the work)
        modelReplaced();
        openedFile_ = true;
        return true;
    }
    // As upstream's `open` command (core/Commands.cpp): one SubTool per object in the file.
    std::vector<sf::MeshObject> objects;
    sf::Mesh single;
    bool ok;
    if (ext == ".glb" || ext == ".gltf") ok = sf::loadGltf(path, objects, error);
    else if (ext == ".obj") ok = sf::loadObj(path, objects, error);
    else if (ext == ".stl") ok = sf::loadStl(path, single, error);
    else {
        error = "unsupported file type " + (ext.empty() ? std::string("(none)") : ext);
        return false;
    }
    if (!ok) return false;
    if (single.triangleCount() > 0) objects.push_back({"", std::move(single)});
    std::erase_if(objects, [](const sf::MeshObject& o) { return o.mesh.triangleCount() == 0; });
    if (objects.empty()) {
        error = "no triangles in the file";
        return false;
    }
    // The file's name without folder and extension (by hand: the path is UTF-8 on every platform).
    const size_t slash = path.find_last_of("/\\");
    std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
    stem = stem.substr(0, stem.find_last_of('.'));
    sf::setSceneObjects(doc_, std::move(objects), stem.empty() ? "Model" : stem);
    doc_.path.clear();
    modelReplaced();
    openedFile_ = true;
    return true;
}

bool SculptView::saveProject(const std::string& path, std::string& error) const {
    return sf::saveProject(path, doc_, error);
}

bool SculptView::exportFile(const std::string& path, std::string& error) const {
    const std::string ext = lowerExt(path);
    if (ext == ".sfproj") return saveProject(path, error);
    if (ext == ".stl") {
        // As upstream's export_stl: the visible SubTools as one mesh, in millimetres with Z up (what slicers expect).
        if (doc_.subtoolCount() == 1) return sf::saveStl(path, doc_.mesh, error);
        return sf::saveStl(path, sf::mergedVisibleMesh(doc_), error);
    }
    const std::vector<sf::MeshObjectRef> objects = sf::visibleSubtoolObjects(doc_);
    if (objects.empty()) {
        error = "nothing visible to export";
        return false;
    }
    if (ext == ".glb") return sf::saveGlb(path, objects, fileStem(), error);
    if (ext == ".obj") return sf::saveObj(path, objects, error);
    error = "unsupported file type " + ext;
    return false;
}

std::string SculptView::fileStem() const {
    std::string out;
    for (char ch : doc_.activeSubtoolInfo().name)
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || (ch & 0x80)) out += ch; // (UTF-8 kept)
        else if (ch == ' ' && !out.empty() && out.back() != '_') out += '_';
    return out.empty() ? "Sculpture" : out;
}

float SculptView::modelSize() const { return bounds_.valid() ? std::max(glm::length(bounds_.extent()) * 0.5f, 1e-6f) : 1.0f; }

void SculptView::syncScene() {
    if (doc_.subtools.empty()) return;
    // Every SubTool (a project or a file may hold several); strokes go to the active one.
    std::vector<sf::SceneMesh> scene;
    for (int i = 0; i < doc_.subtoolCount(); ++i) {
        sf::SceneMesh m;
        m.id = doc_.subtools[size_t(i)].id;
        m.mesh = &doc_.subtoolMesh(i);
        m.active = i == doc_.activeSubtool;
        m.visible = doc_.subtools[size_t(i)].visible || m.active;
        m.wireframe = settings.wireframe && m.active;
        scene.push_back(m);
    }
    groupOverlay_.noteChanges(doc_.mesh); // (before the renderer clears the change marks)
    const auto t0 = Clock::now();
    renderer_.syncScene(scene);
    lastUploadMs_ = float(msSince(t0));
}

void SculptView::draw(int pixelWidth, int pixelHeight) {
    if (!ready_ || pixelWidth <= 0 || pixelHeight <= 0) return;
    width_ = pixelWidth;
    height_ = pixelHeight;
    syncScene();
    camera_.aspect = float(pixelWidth) / float(pixelHeight);
    const auto t0 = Clock::now();
    // Face group colours come from GroupOverlay, not the renderer (which can't tell triangles apart on WebGL2).
    sf::RenderSettings drawn = settings;
    drawn.showPolygroups = false;
    renderer_.draw(camera_, pixelWidth, pixelHeight, drawn);
    if (settings.showPolygroups) groupOverlay_.draw(doc_.mesh, camera_.projection() * camera_.view());
    lastDrawMs_ = float(msSince(t0));
}

double SculptView::timedFrame(int pixelWidth, int pixelHeight) {
    const auto t0 = Clock::now();
    draw(pixelWidth, pixelHeight);
    uint8_t pixel[4];
    gl::ReadPixels(pixelWidth / 2, pixelHeight / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel); // waits for the GPU
    return msSince(t0);
}

sf::Symmetry SculptView::symmetry() const {
    sf::Symmetry sym;
    sym.x = symmetryX;
    sym.y = sym.z = false;
    return sym;
}

sf::DabParams SculptView::dabParams(const glm::vec3& center, const glm::vec3& viewDir, float pressure) const {
    const BrushPrefs& b = prefs();
    const float pr = b.curve.apply(std::clamp(pressure, 0.0f, 1.0f));
    sf::DabParams p;
    p.center = center;
    p.viewDir = viewDir;
    p.radius = worldRadius(center) * (b.pressureSize && fixedRadius_ <= 0.0f ? std::max(pr, 0.05f) : 1.0f) * dabScale_;
    p.strength = b.strength * dabStrengthScale_;
    p.pressure = b.pressureStrength ? pr : 1.0f;
    p.invert = invert;
    p.focal = b.focal;
    p.keepVolume = toolDef().type == sf::BrushType::Smooth && !toolDef().mask && b.keepVolume;
    p.frontFacesOnly = b.frontFacesOnly;
    p.connectedOnly = b.connectedOnly;
    p.strokeDelta = center - lastCenter_;
    return p;
}

bool SculptView::alphaShown() const { return toolTakesAlpha(toolDef()) && prefs().alpha != "Round"; }

float SculptView::alphaAngle() const {
    return stroking_ && alphaOn_ ? alphaAngle_ : glm::radians(prefs().alphaOptions.angle);
}

void SculptView::orientAlpha(sf::DabParams& p, glm::vec2 screen) {
    // As upstream's dabAt: the fixed angle, the random turn and (Follow stroke direction) Blender's Rake; the
    // alpha's top is that turn of the screen's up.
    if (!alphaOn_) return;
    rake_.update(screen);
    alphaAngle_ = sf::alphaDabAngle(prefs().alphaOptions, rake_, alphaSeed_) + dabTurn_;
    p.alpha = &alpha_;
    p.alphaUp = camera_.orientation() * glm::vec3(-std::sin(alphaAngle_), std::cos(alphaAngle_), 0.0f);
}

void SculptView::dabAt(const glm::vec3& center, const glm::vec3& viewDir, float pressure, glm::vec2 screen) {
    sf::DabParams p = dabParams(center, viewDir, pressure);
    if (!toolDef().mask) orientAlpha(p, screen);
    const auto t0 = Clock::now();
    if (toolDef().layerEraser) eraseLayerAt(p);
    else if (toolDef().paint) sf::paintDab(doc_.sculptor, doc_.mesh, doc_.bvh, p, doc_.paintColor, symmetry());
    else if (toolDef().mask) doc_.sculptor.dabMask(doc_.mesh, doc_.bvh, p, symmetry());
    else doc_.sculptor.dab(doc_.mesh, doc_.bvh, toolDef().type, p, symmetry());
    lastDabMs_ = float(msSince(t0));
    ++dabsInStroke_;
    lastCenter_ = center;
    lastScreen_ = screen;
}

bool SculptView::viewPlanePoint(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& through, glm::vec3& out) const {
    const glm::vec3 n = camera_.forward();
    const float denom = glm::dot(dir, n);
    if (std::abs(denom) < 1e-6f) return false;
    out = origin + dir * (glm::dot(through - origin, n) / denom);
    return true;
}

bool SculptView::strokePoint(float& x, float& y, float& radiusPx) const {
    if (!stroking_) return false;
    x = strokeX_;
    y = strokeY_;
    radiusPx = strokeRadiusPx_;
    return true;
}

bool SculptView::beginStroke(float x, float y, float w, float h, float pressure) {
    doc_.syncBvh(); // (after a layer weight changed the whole surface)
    glm::vec3 origin, dir;
    sf::RayHit hit;
    if (w <= 0 || h <= 0) return false;
    viewHeight_ = h;
    if (pickingColor) {
        // The eyedropper: the colour under the press (on release; a second finger may still make it a gesture).
        camera_.screenRay(x, y, w, h, origin, dir);
        if (!doc_.bvh.raycast(doc_.mesh, origin, dir, hit)) return false;
        stroking_ = true;
        drive_ = Drive::Pick;
        pickColor_ = true;
        pickTriangle_ = hit.triangle;
        lastCenter_ = hit.position;
        strokeX_ = x, strokeY_ = y, strokeRadiusPx_ = 0.0f;
        return true;
    }
    if (toolDef().drive == Drive::Gizmo) {
        // A press on a handle starts a transform of the Part's moving vertices (upstream's TransformSession: one
        // undo step, the originals kept so a long drag never drifts); anywhere else turns the view.
        GizmoView g;
        if (!gizmo(w, h, g)) return false;
        const int handle = gizmoHit(g, x, y);
        if (handle < 0) return false;
        const glm::vec3 pivot = gizmoPivot();
        std::vector<float> weights = sf::transformWeights(doc_.mesh, true, 0.0f);
        sf::Symmetry mirror = symmetry(); // (a masked pose moves both sides together)
        if (!doc_.mesh.hasMask()) mirror.x = mirror.y = mirror.z = false;
        if (!transform_.begin(doc_.mesh, std::move(weights), mirror, pivot)) return false;
        stroking_ = true;
        drive_ = Drive::Gizmo;
        gizmoHandle_ = handle;
        gizmoStart_ = g;
        gizmoPress_ = {x, y};
        gizmoPivotStart_ = gizmoShown_ = pivot;
        gizmoLength_ = 90.0f * camera_.worldPerPixel(pivot, h);
        viewHeight_ = h;
        shapeW_ = w, shapeH_ = h;
        strokeX_ = x, strokeY_ = y, strokeRadiusPx_ = 0.0f;
        if (gizmoMode == GizmoMode::Rotate) {
            // Which way a turn about the axis goes on screen: turn a point on the ring a little and look.
            const glm::vec3 axis = handle < 3 ? glm::vec3(handle == 0, handle == 1, handle == 2) : camera_.forward();
            glm::vec3 u = glm::cross(axis, std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
            u = glm::normalize(u) * gizmoLength_;
            const glm::mat4 vp = camera_.projection() * camera_.view();
            const auto screen = [&](const glm::vec3& p) {
                const glm::vec4 c = vp * glm::vec4(p, 1.0f);
                return glm::vec2((c.x / c.w + 1.0f) * 0.5f * w, (1.0f - c.y / c.w) * 0.5f * h);
            };
            const glm::vec2 a = screen(pivot + u) - g.center;
            const glm::vec2 b = screen(pivot + glm::angleAxis(0.1f, axis) * u) - g.center;
            const float turn = std::remainder(std::atan2(b.y, b.x) - std::atan2(a.y, a.x), 6.2831853f);
            gizmoSign_ = turn >= 0.0f ? 1.0f : -1.0f;
            gizmoAngle_ = 0.0f;
            gizmoLastAngle_ = std::atan2(y - g.center.y, x - g.center.x);
        }
        return true;
    }
    if (toolDef().drive == Drive::Pick) {
        // Pick Group: remember the triangle under the press; the tap acts on release (a second finger may still turn
        // it into a gesture).
        camera_.screenRay(x, y, w, h, origin, dir);
        if (!doc_.bvh.raycast(doc_.mesh, origin, dir, hit)) return false;
        stroking_ = true;
        drive_ = Drive::Pick;
        pickTriangle_ = hit.triangle;
        strokeX_ = x, strokeY_ = y, strokeRadiusPx_ = 0.0f;
        return true;
    }
    if (toolDef().drive == Drive::Poly) {
        // Each press places a corner (dragged until the pen lifts); a press on the first corner closes the shape.
        if (polyTool_ != tool) polyClear();
        polyTool_ = tool;
        stroking_ = true;
        drive_ = Drive::Poly;
        shapeW_ = w, shapeH_ = h;
        strokeX_ = x, strokeY_ = y, strokeRadiusPx_ = 0.0f;
        polyClosing_ = poly_.size() >= 3 && glm::distance(poly_.front(), glm::vec2(x, y)) < kPolyClosePx;
        if (!polyClosing_) poly_.push_back({x, y});
        return true;
    }
    if (toolDef().drive == Drive::Lasso || toolDef().drive == Drive::Rect || toolDef().drive == Drive::Line) {
        // A mask shape starts anywhere (round empty space it clears the mask); it acts when the pen lifts.
        stroking_ = true;
        drive_ = toolDef().drive;
        shape_.assign(drive_ == Drive::Lasso ? 1 : 2, glm::vec2(x, y));
        shapeW_ = w, shapeH_ = h;
        strokeX_ = x, strokeY_ = y, strokeRadiusPx_ = 0.0f;
        return true;
    }
    camera_.screenRay(x, y, w, h, origin, dir);
    if (!doc_.bvh.raycast(doc_.mesh, origin, dir, hit)) return false;
    if (toolDef().layerEraser && (doc_.activeLayer < 0 || doc_.activeLayer >= int(doc_.mesh.layers.size()))) {
        notice = "nolayer"; // (nothing to erase on the base)
        return false;
    }
    if (!doc_.sculptor.beginStroke(doc_.mesh, toolDef().key, toolDef().paint ? -1 : doc_.activeLayer)) return false;
    paintBefore_.reset();
    if (toolDef().paint && !doc_.mesh.hasColors()) // (only the paint stack: the mesh itself stays out of the record)
        paintBefore_ = std::make_unique<sf::PaintStackUndo>(sf::PaintStackUndo::capture(doc_.mesh));
    const BrushPrefs& b = prefs();
    shapeW_ = w, shapeH_ = h;
    mode_ = toolDef().drive == Drive::Dabs && toolTakesStrokeMode(toolDef(), b.stroke) ? b.stroke : StrokeMode::Dots;
    anchor_ = lineEnd_ = {x, y};
    anchorHit_ = hit.position;
    stamped_ = false;
    strokePeak_ = pressure;
    spray_.reseed(0x2545f491u);
    dabScale_ = dabStrengthScale_ = 1.0f;
    dabTurn_ = 0.0f;
    if (mode_ == StrokeMode::DragDot || mode_ == StrokeMode::DragRect) doc_.sculptor.holdTopology(); // (they re-stamp)
    stroking_ = true;
    drive_ = toolDef().drive;
    dabsInStroke_ = 0;
    lastCenter_ = hit.position;
    lastScreen_ = {x, y};
    // The stroke's alpha (a copy, tiled as the brush says), and the rake and jitter started afresh.
    alphaOn_ = alphaShown() && sf::BrushAlpha::builtin(b.alpha, alpha_);
    if (alphaOn_) {
        alpha_.tileX = b.alphaOptions.tileX;
        alpha_.tileY = b.alphaOptions.tileY;
    }
    rake_.reset({x, y});
    alphaSeed_ = 0x9e3779b9u;
    alphaAngle_ = glm::radians(b.alphaOptions.angle);
    lastPressure_ = pressure;
    lazy_.radius = b.steadyPx;
    lazy_.reset({x, y});
    strokeX_ = x, strokeY_ = y;
    const float pr = b.curve.apply(std::clamp(pressure, 0.0f, 1.0f));
    strokeRadiusPx_ = b.radiusPx * (b.pressureSize ? std::max(pr, 0.05f) : 1.0f);
    grabLast_ = hit.position;
    const sf::DabParams p = dabParams(hit.position, camera_.forward(), pressure);
    switch (drive_) {
    case Drive::Dabs:
        switch (mode_) {
        case StrokeMode::Line:
        case StrokeMode::DragRect: break; // (on release / once the drag gives it a size)
        case StrokeMode::DragDot: slideTo(x, y, pressure); break;
        case StrokeMode::Spray: sprayAt({x, y}, pressure); break;
        default: dabAt(hit.position, dir, pressure, {x, y}); break;
        }
        break;
    case Drive::Grab: {
        // Move takes hold of the region under the brush once, then drags it on a plane facing the view.
        const auto t0 = Clock::now();
        doc_.sculptor.beginGrab(doc_.mesh, doc_.bvh, p, symmetry());
        lastDabMs_ = float(msSince(t0));
        dabsInStroke_ = 1;
        break;
    }
    case Drive::Pull: break; // (nothing moves until the pen does)
    case Drive::Elastic:
    case Drive::Turn:
        turnCenter_ = hit.position;
        kelvinRadius_ = p.radius;
        turnLastX_ = x;
        break;
    case Drive::Lasso:
    case Drive::Rect:
    case Drive::Line:
    case Drive::Poly:
    case Drive::Pick:
    case Drive::Gizmo: break; // (above)
    }
    return true;
}

bool SculptView::screenHit(float x, float y, sf::RayHit& hit) const {
    const auto cast = [&](float cx, float cy, sf::RayHit& out) {
        glm::vec3 o, d;
        camera_.screenRay(cx, cy, shapeW_, shapeH_, o, d);
        return doc_.bvh.raycast(doc_.mesh, o, d, out);
    };
    return sf::pickNearCursor(x, y, strokeRadiusPx_ * 0.5f, cast, hit);
}

void SculptView::sprayAt(glm::vec2 screen, float pressure) {
    // As upstream's Spray stroke: each dab lands somewhere within `scatter` radii of the path, at half to full size
    // and strength, turned at random.
    const BrushPrefs& b = prefs();
    const sf::SprayDab d = spray_.next(b.radiusPx, b.scatter);
    screen += d.offset;
    sf::RayHit hit;
    if (!screenHit(screen.x, screen.y, hit)) return;
    dabScale_ = d.scale, dabStrengthScale_ = d.strength, dabTurn_ = d.angle;
    dabAt(hit.position, camera_.forward(), pressure, screen);
    dabScale_ = dabStrengthScale_ = 1.0f;
    dabTurn_ = 0.0f;
}

void SculptView::slideTo(float x, float y, float pressure) {
    // As upstream's DragDot: the stamp is taken back and pressed again under the pen (positions only: no remeshing).
    sf::RayHit hit;
    if (!screenHit(x, y, hit)) return;
    if (dabsInStroke_ > 0) doc_.sculptor.restoreStroke(doc_.mesh, doc_.bvh);
    lastCenter_ = hit.position; // (no stroke direction: it is one stamp)
    dabAt(hit.position, camera_.forward(), pressure, {x, y});
}

void SculptView::pullStampTo(float x, float y) {
    // As upstream's DragRect: the stamp sits where the press was; the drag's length is its radius and the alpha's
    // top points along the drag.
    const glm::vec2 drag = glm::vec2(x, y) - anchor_;
    const float len = glm::length(drag);
    if (len < 2.0f) return;
    if (stamped_) doc_.sculptor.restoreStroke(doc_.mesh, doc_.bvh);
    sf::DabParams p = dabParams(anchorHit_, camera_.forward(), strokePeak_);
    p.radius = len * camera_.worldPerPixel(anchorHit_, shapeH_);
    p.strokeDelta = glm::vec3(0.0f);
    alphaAngle_ = std::atan2(-drag.x, -drag.y);
    if (alphaOn_) {
        p.alpha = &alpha_;
        p.alphaUp = camera_.orientation() * glm::vec3(-std::sin(alphaAngle_), std::cos(alphaAngle_), 0.0f);
    }
    const auto t0 = Clock::now();
    doc_.sculptor.dab(doc_.mesh, doc_.bvh, toolDef().type, p, symmetry());
    lastDabMs_ = float(msSince(t0));
    dabsInStroke_ = 1;
    stamped_ = true;
    strokeX_ = anchor_.x, strokeY_ = anchor_.y, strokeRadiusPx_ = len;
}

void SculptView::lineDabs() {
    // Dabs every `spacing` radii from the press to where the pen lifted, at the stroke's firmest pressure.
    const BrushPrefs& b = prefs();
    const float len = glm::distance(anchor_, lineEnd_);
    const float step = std::max(b.radiusPx * b.spacing, 1.0f);
    const int n = std::min(int(len / step), 4000);
    rake_.reset(anchor_);
    lastCenter_ = anchorHit_;
    for (int i = 0; i <= n; ++i) {
        const glm::vec2 s = len > 0.0f ? glm::mix(anchor_, lineEnd_, float(i) * step / len) : anchor_;
        sf::RayHit hit;
        if (!screenHit(s.x, s.y, hit)) continue;
        dabAt(hit.position, camera_.forward(), strokePeak_, s);
    }
}

void SculptView::eraseLayerAt(const sf::DabParams& p) {
    // Each vertex under the dab (or its mirror image) gives back part of what the layer moved it: most in the
    // middle, fading to nothing at the edge; masked and hidden vertices stay.
    const int layer = doc_.activeLayer;
    sf::Mesh& mesh = doc_.mesh;
    if (layer < 0 || layer >= int(mesh.layers.size())) return;
    const sf::SculptLayer& L = mesh.layers[size_t(layer)];
    if (L.delta.size() != mesh.vertexCount()) return;
    const float amount = std::clamp(p.strength * p.pressure * 2.0f, 0.0f, 1.0f);
    std::vector<uint32_t> found;
    std::vector<std::pair<uint32_t, float>> hits;
    for (const sf::SymCopy& c : sf::Sculptor::copies(symmetry())) {
        const glm::vec3 center = c.point(p.center);
        sf::brushVertices(mesh, doc_.bvh, center, p.radius, found);
        for (uint32_t v : found) {
            const float f = sf::brushFalloff(glm::distance(mesh.positions[v], center), p.radius, p.focal);
            if (f > 0.0f) hits.push_back({v, f});
        }
    }
    if (hits.empty()) return;
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.first < b.first || (a.first == b.first && a.second > b.second);
    });
    std::vector<uint32_t> verts;
    std::vector<glm::vec3> d;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i && hits[i].first == hits[i - 1].first) continue; // (the nearer copy's falloff, sorted first)
        const uint32_t v = hits[i].first;
        const float k = hits[i].second * amount * sf::vertexInfluence(mesh, v);
        if (k <= 0.0f || L.delta[v] == glm::vec3(0.0f)) continue;
        verts.push_back(v);
        d.push_back(-L.delta[v] * L.weight * k);
    }
    doc_.sculptor.moveVertices(mesh, doc_.bvh, verts, d);
}

void SculptView::pullTo(const glm::vec3& origin, const glm::vec3& dir, float pressure, glm::vec2 screen) {
    // As upstream's snakeHookTo: small dabs from the last point towards the pen's point on the drag plane, each one
    // pulling the surface along by its own step.
    glm::vec3 target;
    if (!viewPlanePoint(origin, dir, grabLast_, target)) return;
    sf::DabParams p = dabParams(grabLast_, camera_.forward(), pressure);
    const glm::vec3 delta = target - grabLast_;
    const int steps = std::clamp(int(std::ceil(glm::length(delta) / std::max(p.radius * prefs().spacing, 1e-6f))), 1, 64);
    p.strokeDelta = delta / float(steps);
    orientAlpha(p, screen);
    const auto t0 = Clock::now();
    for (int i = 0; i < steps; ++i) {
        p.center = grabLast_;
        doc_.sculptor.dab(doc_.mesh, doc_.bvh, toolDef().type, p, symmetry());
        grabLast_ += p.strokeDelta;
        ++dabsInStroke_;
    }
    lastDabMs_ = float(msSince(t0) / steps);
}

void SculptView::elasticTo(const glm::vec3& origin, const glm::vec3& dir, float pressure) {
    // As upstream's kelvinNextStep: the grabbed point follows the pen on the plane through the press point, in
    // pieces of at most a brush radius (a fast drag then still integrates smoothly).
    glm::vec3 target;
    if (!viewPlanePoint(origin, dir, turnCenter_, target)) return;
    const sf::DabParams p = dabParams(grabLast_, camera_.forward(), pressure);
    const float amount = std::min(1.0f, 2.0f * p.strength) * p.pressure;
    const auto t0 = Clock::now();
    for (int i = 0; i < 16; ++i) {
        glm::vec3 delta = target - grabLast_;
        const float len = glm::length(delta);
        if (len < 1e-7f * kelvinRadius_) break;
        if (len > kelvinRadius_) delta *= kelvinRadius_ / len;
        sf::KelvinletStep step;
        step.radius = kelvinRadius_;
        step.viewDir = camera_.forward();
        step.axis = -camera_.forward();
        step.frontFacesOnly = p.frontFacesOnly;
        step.center = grabLast_;
        step.delta = delta * amount;
        step.travel = delta;
        step.substeps = std::clamp(int(std::ceil(glm::length(delta) / (0.4f * kelvinRadius_))), 1, 3);
        doc_.sculptor.kelvinletStep(doc_.mesh, doc_.bvh, toolDef().type, step, symmetry());
        grabLast_ += delta;
        ++dabsInStroke_;
    }
    if (doc_.sculptor.kelvinletFarPending()) doc_.sculptor.flushKelvinletFar(doc_.mesh, doc_.bvh, false);
    lastDabMs_ = float(msSince(t0));
}

void SculptView::turnTo(float x, float pressure) {
    // As upstream's kelvinTo for Twist / Scale: the drag's sideways distance in brush radii turns or swells the
    // area round the press point (to the left: back / shrinks; Invert reverses), in bounded steps.
    const sf::DabParams p = dabParams(turnCenter_, camera_.forward(), pressure);
    const float amount = std::min(1.0f, 2.0f * p.strength) * p.pressure;
    const bool twist = toolDef().type == sf::BrushType::KelvinTwist;
    const float dx = (x - turnLastX_) / std::max(prefs().radiusPx, 1.0f) * amount * (invert ? -1.0f : 1.0f);
    turnLastX_ = x;
    float pending = twist ? -dx * 0.5f * 3.14159265f : dx * 0.6f;
    const float most = twist ? 0.3f : 0.2f;
    const auto t0 = Clock::now();
    for (int i = 0; i < 16 && std::abs(pending) > 1e-6f; ++i) {
        const float now = std::clamp(pending, -most, most);
        pending -= now;
        sf::KelvinletStep step;
        step.radius = kelvinRadius_;
        step.viewDir = camera_.forward();
        step.axis = -camera_.forward();
        step.frontFacesOnly = p.frontFacesOnly;
        step.center = turnCenter_;
        if (twist) step.angle = now;
        else step.logScale = now;
        doc_.sculptor.kelvinletStep(doc_.mesh, doc_.bvh, toolDef().type, step, symmetry());
        ++dabsInStroke_;
    }
    if (doc_.sculptor.kelvinletFarPending()) doc_.sculptor.flushKelvinletFar(doc_.mesh, doc_.bvh, false);
    lastDabMs_ = float(msSince(t0));
}

void SculptView::strokeTo(float x, float y, float w, float h, float pressure) {
    if (!stroking_ || w <= 0 || h <= 0) return;
    if (drive_ == Drive::Rect || drive_ == Drive::Line) {
        shape_.back() = {x, y};
        strokeX_ = x, strokeY_ = y;
        return;
    }
    if (drive_ == Drive::Poly) {
        if (!polyClosing_ && !poly_.empty()) poly_.back() = {x, y};
        strokeX_ = x, strokeY_ = y;
        return;
    }
    if (drive_ == Drive::Lasso) {
        if (glm::distance(shape_.back(), glm::vec2(x, y)) >= 3.0f) shape_.push_back({x, y});
        strokeX_ = x, strokeY_ = y;
        return;
    }
    if (drive_ == Drive::Pick) return;
    if (drive_ == Drive::Gizmo) {
        strokeX_ = x, strokeY_ = y;
        gizmoTo(x, y);
        return;
    }
    const BrushPrefs& b = prefs();
    strokePeak_ = std::max(strokePeak_, pressure);
    if (drive_ == Drive::Dabs && mode_ == StrokeMode::Line) {
        lineEnd_ = {x, y};
        strokeX_ = x, strokeY_ = y;
        return;
    }
    if (drive_ == Drive::Dabs && mode_ == StrokeMode::DragRect) {
        pullStampTo(x, y);
        return;
    }
    if (b.steady && drive_ != Drive::Turn) {
        // Steady Stroke: the brush trails the pen on a string and moves only once it is taut.
        const glm::vec2 before = lazy_.point, now = lazy_.follow({x, y});
        if (glm::distance(before, now) < 0.01f) return;
        x = now.x, y = now.y;
    }
    strokeX_ = x, strokeY_ = y;
    const float pr = b.curve.apply(std::clamp(pressure, 0.0f, 1.0f));
    strokeRadiusPx_ = b.radiusPx * (b.pressureSize ? std::max(pr, 0.05f) : 1.0f);
    glm::vec3 origin, dir;
    camera_.screenRay(x, y, w, h, origin, dir);
    switch (drive_) {
    case Drive::Grab: {
        glm::vec3 p;
        if (!viewPlanePoint(origin, dir, grabLast_, p)) return;
        const auto t0 = Clock::now();
        doc_.sculptor.grab(doc_.mesh, doc_.bvh, p - grabLast_);
        lastDabMs_ = float(msSince(t0));
        ++dabsInStroke_;
        grabLast_ = p;
        return;
    }
    case Drive::Pull: pullTo(origin, dir, pressure, {x, y}); return;
    case Drive::Elastic: elasticTo(origin, dir, pressure); return;
    case Drive::Turn: turnTo(x, pressure); return;
    case Drive::Lasso:
    case Drive::Rect:
    case Drive::Line:
    case Drive::Poly:
    case Drive::Pick:
    case Drive::Gizmo: return; // (above)
    case Drive::Dabs: break;
    }
    if (mode_ == StrokeMode::DragDot) {
        slideTo(x, y, pressure);
        return;
    }
    // The surface under the brush; near a silhouette, the edge the brush circle still covers (upstream #84).
    sf::RayHit hit;
    const auto cast = [&](float cx, float cy, sf::RayHit& out) {
        glm::vec3 o, d;
        camera_.screenRay(cx, cy, w, h, o, d);
        return doc_.bvh.raycast(doc_.mesh, o, d, out);
    };
    if (!sf::pickNearCursor(x, y, strokeRadiusPx_ * 0.5f, cast, hit)) return; // off the model
    // Dabs every `spacing` radii along the way (pointer samples can be far apart on a fast stroke).
    const glm::vec3 from = lastCenter_;
    const glm::vec2 fromScreen = lastScreen_;
    const float spacing = std::max(worldRadius(hit.position) * b.spacing, 1e-6f);
    const float length = glm::distance(from, hit.position);
    const int steps = int(length / spacing);
    for (int i = 1; i <= steps; ++i) {
        const float t = float(i) * spacing / length;
        const glm::vec3 c = glm::mix(from, hit.position, t);
        const glm::vec2 s = glm::mix(fromScreen, glm::vec2(x, y), t);
        const float pr = glm::mix(lastPressure_, pressure, t);
        if (mode_ != StrokeMode::Spray) {
            dabAt(c, dir, pr, s);
            continue;
        }
        sprayAt(s, pr);
        lastCenter_ = c, lastScreen_ = s; // (the path goes on from the path, not from the scattered dab)
    }
    if (steps > 0) lastPressure_ = pressure;
}

void SculptView::endStroke() {
    if (!stroking_) return;
    if (drive_ == Drive::Lasso || drive_ == Drive::Rect || drive_ == Drive::Line) {
        // (These read the shape while the stroke still counts as running.)
        switch (toolDef().shape) {
        case Shape::Mask: applyMaskShape(); break;
        case Shape::Hide: applyHideShape(); break;
        case Shape::Group: applyGroupShape(); break;
        case Shape::Cut: applyCut(); break;
        }
        stroking_ = false;
        shape_.clear();
        return;
    }
    if (drive_ == Drive::Pick) {
        if (pickColor_) {
            // As upstream's pickColorUnderCursor: the paint there (white where there is none).
            doc_.paintColor = doc_.mesh.hasColors() ? sf::sampleColor(doc_.mesh, pickTriangle_, lastCenter_) : glm::vec3(1.0f);
            pickingColor = pickColor_ = false;
            notice = "picked";
        } else {
            applyPick();
        }
        stroking_ = false;
        return;
    }
    if (drive_ == Drive::Poly) {
        stroking_ = false;
        if (polyClosing_) polyFinish();
        polyClosing_ = false;
        return;
    }
    if (drive_ == Drive::Dabs && mode_ == StrokeMode::Line) lineDabs();
    if (drive_ == Drive::Dabs && mode_ == StrokeMode::DragRect && !stamped_) {
        // A tap without a drag: one stamp of the brush's own size.
        sf::RayHit hit;
        if (screenHit(anchor_.x, anchor_.y, hit)) dabAt(hit.position, camera_.forward(), strokePeak_, anchor_);
    }
    if (drive_ == Drive::Gizmo) {
        static const char* const labels[] = {"Move", "Rotate", "Scale"};
        sf::UndoRecord record = transform_.finish(doc_.mesh, doc_.bvh, labels[int(gizmoMode)]);
        stroking_ = false;
        gizmoHandle_ = -1;
        if (record.empty()) return;
        doc_.undo.push(std::move(record));
        updateBounds();
        ++revision_;
        ++editsSinceLoad_;
        return;
    }
    stroking_ = false;
    doc_.sculptor.flushDetailClone(doc_.mesh, doc_.bvh, symmetry()); // (a Kelvinlet stroke's deferred far field)
    sf::UndoRecord record = doc_.sculptor.endStroke();
    if (paintBefore_) {
        // The colours the stroke recorded are the white it started from; restoring "no paint" replaces them.
        if (doc_.mesh.hasColors()) {
            record.colors = {};
            record.paintAlpha = {};
            record.paint = std::move(paintBefore_);
        }
        paintBefore_.reset();
    }
    if (record.empty()) return; // (a line too short to land, an erase with nothing to erase...)
    doc_.undo.push(std::move(record));
    if (doc_.sculptor.dyntopo.enabled) groupOverlay_.invalidate(); // (the triangles changed under the brush)
    ++revision_;
    ++editsSinceLoad_;
}

void SculptView::cancelStroke() {
    if (!stroking_) return;
    if (drive_ == Drive::Gizmo) {
        transform_.cancel(doc_.mesh, doc_.bvh);
        stroking_ = false;
        gizmoHandle_ = -1;
        return;
    }
    if (drive_ == Drive::Poly) { // (the corner being placed goes; the others stay)
        stroking_ = false;
        if (!polyClosing_ && !poly_.empty()) poly_.pop_back();
        polyClosing_ = false;
        return;
    }
    if (drive_ == Drive::Dabs && mode_ == StrokeMode::Line) mode_ = StrokeMode::Dots; // (no dabs for a line that goes)
    pickColor_ = false;
    if (drive_ == Drive::Lasso || drive_ == Drive::Rect || drive_ == Drive::Line || drive_ == Drive::Pick) { // (nothing applied yet)
        stroking_ = false;
        shape_.clear();
        return;
    }
    endStroke();
    doc_.undo.undo(doc_.mesh, doc_.bvh);
}

// --- Mask ---------------------------------------------------------------------------------------------------------

std::vector<glm::vec2> SculptView::maskShape() const {
    if (toolDef().drive == Drive::Poly && polyTool_ == tool && !(stroking_ && drive_ == Drive::Lasso)) return poly_;
    if (!stroking_ || (drive_ != Drive::Lasso && drive_ != Drive::Rect && drive_ != Drive::Line) || shape_.empty()) return {};
    if (drive_ == Drive::Lasso || drive_ == Drive::Line) return shape_;
    const glm::vec2 a = shape_.front(), b = shape_.back();
    return {a, {b.x, a.y}, b, {a.x, b.y}};
}

bool SculptView::strokeLine(glm::vec2& a, glm::vec2& b) const {
    if (!stroking_ || drive_ != Drive::Dabs || mode_ != StrokeMode::Line) return false;
    a = anchor_, b = lineEnd_;
    return true;
}

void SculptView::polyClear() {
    poly_.clear();
    polyClosing_ = false;
}

void SculptView::polyFinish() {
    // Cuts as a lasso through the corners.
    if (poly_.size() < 3 || toolDef().drive != Drive::Poly) {
        polyClear();
        return;
    }
    shape_ = poly_;
    drive_ = Drive::Lasso;
    stroking_ = true; // (maskShape reads the running stroke's shape)
    applyCut();
    stroking_ = false;
    shape_.clear();
    polyClear();
}

void SculptView::applyMaskShape() {
    // As upstream's mask_lasso / mask_rect: the visible vertices projecting inside the shape (and their mirror
    // images) gain mask, or lose it with Erase; a shape round empty space clears the mask (ZBrush).
    std::vector<glm::vec2> pts = maskShape();
    if (pts.size() < 3) return;
    for (glm::vec2& p : pts) p = {2.0f * p.x / shapeW_ - 1.0f, 1.0f - 2.0f * p.y / shapeH_};
    const sf::ScreenPolygon polygon(std::move(pts));
    if (!polygon.valid()) return;
    camera_.aspect = shapeW_ / std::max(shapeH_, 1.0f);
    const glm::mat4 vp = camera_.projection() * camera_.view();
    const std::vector<sf::SymCopy> mirrors = sf::Sculptor::copies(symmetry());
    sf::UndoRecord record = sf::UndoRecord::captureMask(doc_.mesh, invert ? "Unmask" : "Mask");
    bool changed;
    if (!sf::shapeCoversMesh(doc_.mesh, vp, polygon, mirrors)) changed = invert ? false : sf::maskClear(doc_.mesh);
    else changed = sf::maskInShape(doc_.mesh, vp, polygon, prefs().strength, invert, mirrors, prefs().frontFacesOnly);
    if (!changed) return;
    doc_.undo.push(std::move(record));
    ++revision_;
    ++editsSinceLoad_;
}

// --- Gizmo ----------------------------------------------------------------------------------------------------------

glm::vec3 SculptView::gizmoPivot() const {
    // The centre of what moves (the unmasked part, as upstream's Gizmo), worked out again only after a change.
    if (pivotRevision_ != revision_ || pivotPart_ != doc_.activeSubtool) {
        pivot_ = sf::transformCenter(doc_.mesh, sf::transformWeights(doc_.mesh, true, 0.0f));
        pivotRevision_ = revision_;
        pivotPart_ = doc_.activeSubtool;
    }
    return pivot_;
}

bool SculptView::gizmo(float w, float h, GizmoView& out) const {
    if (doc_.mesh.triangleCount() == 0 || w <= 0.0f || h <= 0.0f) return false;
    const bool dragging = stroking_ && drive_ == Drive::Gizmo;
    const glm::vec3 c = dragging ? gizmoShown_ : gizmoPivot();
    const glm::mat4 vp = camera_.projection() * camera_.view();
    bool behind = false;
    const auto screen = [&](const glm::vec3& p) {
        const glm::vec4 q = vp * glm::vec4(p, 1.0f);
        if (q.w <= 1e-6f) behind = true;
        return glm::vec2((q.x / q.w + 1.0f) * 0.5f * w, (1.0f - q.y / q.w) * 0.5f * h);
    };
    const float length = 90.0f * camera_.worldPerPixel(c, h); // (arrows about 90 window units long)
    out.center = screen(c);
    for (int k = 0; k < 3; ++k) {
        const glm::vec3 axis(k == 0, k == 1, k == 2), u(k == 1, k == 2, k == 0), v(k == 2, k == 0, k == 1);
        out.tip[k] = screen(c + axis * length);
        out.ring[k].resize(48);
        for (int i = 0; i < 48; ++i) {
            const float a = 6.2831853f * float(i) / 48.0f;
            out.ring[k][size_t(i)] = screen(c + (u * std::cos(a) + v * std::sin(a)) * (0.85f * length));
        }
    }
    out.viewRingPx = 108.0f;
    out.hot = dragging ? gizmoHandle_ : -1;
    return !behind;
}

int SculptView::gizmoHit(const GizmoView& g, float x, float y) const {
    // The nearest handle within reach of a fingertip; the centre wins a tie (an axis pointing at the viewer
    // ends on it).
    const glm::vec2 p(x, y);
    const auto segment = [&](glm::vec2 a, glm::vec2 b) {
        const glm::vec2 d = b - a;
        const float t = glm::dot(d, d) > 1e-6f ? std::clamp(glm::dot(p - a, d) / glm::dot(d, d), 0.0f, 1.0f) : 0.0f;
        return glm::distance(p, a + d * t);
    };
    int best = -1;
    float bestD = 1e9f;
    const auto take = [&](int handle, float d, float reach) {
        if (d < reach && d < bestD) best = handle, bestD = d;
    };
    const float toCenter = glm::distance(p, g.center);
    if (gizmoMode == GizmoMode::Rotate) {
        for (int k = 0; k < 3; ++k)
            for (size_t i = 0; i < g.ring[k].size(); ++i) take(k, segment(g.ring[k][i], g.ring[k][(i + 1) % g.ring[k].size()]), 16.0f);
        take(3, std::abs(toCenter - g.viewRingPx), 16.0f);
        return best;
    }
    for (int k = 0; k < 3; ++k) {
        const glm::vec2 d = g.tip[k] - g.center;
        const float len = glm::length(d);
        if (len < 12.0f) continue; // (pointing at the viewer: use the centre)
        take(k, glm::distance(p, g.tip[k]) * 0.8f, 24.0f);
        take(k, segment(g.center + d * (18.0f / len), g.tip[k]), 14.0f);
    }
    take(3, toCenter * 0.7f, 20.0f);
    return best;
}

void SculptView::gizmoTo(float x, float y) {
    const GizmoView& g = gizmoStart_;
    const int k = gizmoHandle_;
    const glm::vec2 drag = glm::vec2(x, y) - gizmoPress_;
    const glm::vec3 axis = k < 3 ? glm::vec3(k == 0, k == 1, k == 2) : camera_.forward();
    glm::vec2 along(0.0f);
    float alongPx = 1.0f;
    if (k < 3) {
        const glm::vec2 d = g.tip[k] - g.center;
        alongPx = std::max(glm::length(d), 1.0f);
        along = d / alongPx;
    }
    sf::MeshTransform t;
    t.pivot = gizmoPivotStart_;
    switch (gizmoMode) {
    case GizmoMode::Move:
        if (k < 3) {
            t.translate = axis * (glm::dot(drag, along) / alongPx * gizmoLength_);
        } else { // on the plane facing the view through the pivot
            glm::vec3 o0, d0, o1, d1, p0, p1;
            camera_.screenRay(gizmoPress_.x, gizmoPress_.y, shapeW_, shapeH_, o0, d0);
            camera_.screenRay(x, y, shapeW_, shapeH_, o1, d1);
            if (viewPlanePoint(o0, d0, t.pivot, p0) && viewPlanePoint(o1, d1, t.pivot, p1)) t.translate = p1 - p0;
        }
        break;
    case GizmoMode::Rotate: {
        // The pen's turn round the centre on screen, unwrapped so a drag can go round more than once.
        const float now = std::atan2(y - g.center.y, x - g.center.x);
        gizmoAngle_ += std::remainder(now - gizmoLastAngle_, 6.2831853f);
        gizmoLastAngle_ = now;
        t.rotate = glm::angleAxis(gizmoSign_ * gizmoAngle_, axis);
        break;
    }
    case GizmoMode::Scale:
        if (k < 3) t.scale[k] = std::clamp(1.0f + glm::dot(drag, along) / alongPx, 0.05f, 20.0f);
        else t.scale = glm::vec3(std::clamp(std::exp((drag.x - drag.y) / 150.0f), 0.05f, 20.0f));
        break;
    }
    const auto t0 = Clock::now();
    transform_.apply(doc_.mesh, t);
    lastDabMs_ = float(msSince(t0));
    gizmoShown_ = t.pivot + t.translate;
}

// --- Face groups and hiding ---------------------------------------------------------------------------------------

std::vector<glm::vec2> SculptView::shapeNdc(glm::mat4& viewProj) {
    std::vector<glm::vec2> pts = maskShape();
    for (glm::vec2& p : pts) p = {2.0f * p.x / shapeW_ - 1.0f, 1.0f - 2.0f * p.y / shapeH_};
    camera_.aspect = shapeW_ / std::max(shapeH_, 1.0f);
    viewProj = camera_.projection() * camera_.view();
    return pts;
}

bool SculptView::commitHidden(const std::vector<uint8_t>& proposed, const char* label) {
    // As upstream's commitHidden (core/Polygroups.cpp): only the triangles that change go into the undo record, and
    // the array is dropped again once nothing is hidden.
    sf::Mesh& m = doc_.mesh;
    if (proposed.size() != m.triangleCount()) return false;
    sf::UndoRecord r;
    r.label = label;
    for (uint32_t t = 0; t < m.triangleCount(); ++t) {
        const uint8_t old = uint8_t(m.triangleHidden(t));
        if (old != (proposed[t] ? 1 : 0)) r.hidden.add(t, old);
    }
    if (r.hidden.empty()) return false;
    m.ensureHidden();
    for (uint32_t t : r.hidden.indices) m.hidden[t] = uint8_t(proposed[t] ? 1 : 0);
    if (std::all_of(m.hidden.begin(), m.hidden.end(), [](uint8_t h) { return h == 0; })) m.clearHidden();
    m.facesDirty = true;
    doc_.undo.push(std::move(r));
    ++revision_;
    ++editsSinceLoad_;
    return true;
}

bool SculptView::commitGroups(const std::vector<uint32_t>& proposed, const char* label) {
    sf::Mesh& m = doc_.mesh;
    if (proposed.size() != m.triangleCount()) return false;
    sf::UndoRecord r;
    r.label = label;
    for (uint32_t t = 0; t < m.triangleCount(); ++t) {
        const uint32_t old = m.polygroupAt(t);
        if (old != proposed[t]) r.polygroups.add(t, old);
    }
    if (r.polygroups.empty()) return false;
    m.ensurePolygroups();
    for (uint32_t t : r.polygroups.indices) m.polygroups[t] = proposed[t];
    if (std::all_of(m.polygroups.begin(), m.polygroups.end(), [](uint32_t g) { return g == 0; })) m.clearPolygroups();
    m.facesDirty = true;
    doc_.undo.push(std::move(r));
    ++revision_;
    ++editsSinceLoad_;
    return true;
}

void SculptView::applyHideShape() {
    // As upstream's hide_lasso / hide_rect: the visible triangles whose centre (or its mirror image) projects inside
    // are hidden, or with Keep the ones outside. A shape round empty space shows everything (ZBrush). Nothing
    // happens when everything would be hidden.
    glm::mat4 vp;
    std::vector<glm::vec2> pts = shapeNdc(vp);
    if (pts.size() < 3) return;
    const std::vector<sf::SymCopy> mirrors = sf::Sculptor::copies(symmetry());
    const sf::ScreenPolygon polygon(pts);
    if (!polygon.valid()) return;
    if (!sf::shapeCoversMesh(doc_.mesh, vp, polygon, mirrors)) {
        if (doc_.mesh.hasHidden()) commitHidden(std::vector<uint8_t>(doc_.mesh.triangleCount(), 0), "Show all"); // (groupOp refuses mid-stroke)
        return;
    }
    const std::vector<uint8_t> hidden = sf::hiddenByLasso(doc_.mesh, vp, pts, !invert, mirrors);
    if (!hidden.empty()) commitHidden(hidden, "Hide");
}

namespace {

// An unused group id whose colour (sf::polygroupColor, a hash of the id) stands apart from the groups already on the
// model: neighbouring ids can hash to nearly the same colour, and a new group the same red as the rest is no use.
uint32_t freshGroupId(const sf::Mesh& m) {
    std::vector<uint32_t> used(m.polygroups.begin(), m.polygroups.end());
    if (used.empty()) used.push_back(0);
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    const uint32_t first = sf::nextPolygroupId(m);
    uint32_t best = first;
    float bestGap = -1.0f;
    for (uint32_t id = first; id < first + 48; ++id) {
        float gap = 1e9f;
        for (uint32_t u : used) gap = std::min(gap, glm::distance(sf::polygroupColor(id), sf::polygroupColor(u)));
        if (gap > bestGap) bestGap = gap, best = id;
        if (used.size() > 64) break; // (many groups: any new id will do)
    }
    return best;
}

} // namespace

void SculptView::applyGroupShape() {
    // The visible triangles whose centre (or its mirror image) projects inside become one new face group.
    glm::mat4 vp;
    const std::vector<glm::vec2> pts = shapeNdc(vp);
    if (pts.size() < 3) return;
    const sf::ScreenPolygon polygon(pts);
    if (!polygon.valid()) return;
    const std::vector<sf::SymCopy> mirrors = sf::Sculptor::copies(symmetry());
    const sf::Mesh& m = doc_.mesh;
    const uint32_t id = freshGroupId(m);
    std::vector<uint32_t> groups(m.triangleCount());
    bool any = false;
    for (uint32_t t = 0; t < m.triangleCount(); ++t) {
        groups[t] = m.polygroupAt(t);
        if (m.triangleHidden(t)) continue;
        const glm::uvec3& tri = m.triangles[t];
        const glm::vec3 c = (m.positions[tri.x] + m.positions[tri.y] + m.positions[tri.z]) / 3.0f;
        glm::vec2 ndc;
        bool in = sf::projectToNdc(vp, c, ndc) && polygon.contains(ndc);
        for (size_t k = 0; k < mirrors.size() && !in; ++k)
            in = !mirrors[k].identity() && sf::projectToNdc(vp, mirrors[k].point(c), ndc) && polygon.contains(ndc);
        if (in) groups[t] = id, any = true;
    }
    if (any) commitGroups(groups, "Face group");
}

void SculptView::applyPick() {
    // ZBrush's Ctrl+Shift click: show the tapped group alone, or everything again when it already is; with Hide, hide it.
    const sf::Mesh& m = doc_.mesh;
    if (pickTriangle_ >= m.triangleCount()) return;
    const uint32_t group = m.polygroupAt(pickTriangle_);
    if (invert) {
        const std::vector<uint8_t> hidden = sf::hiddenHideGroup(m, group);
        if (!hidden.empty()) commitHidden(hidden, "Hide group");
    } else if (sf::polygroupIsolated(m, group)) {
        if (doc_.mesh.hasHidden()) commitHidden(std::vector<uint8_t>(doc_.mesh.triangleCount(), 0), "Show all"); // (groupOp refuses mid-stroke)
    } else {
        const std::vector<uint8_t> hidden = sf::hiddenIsolateGroup(m, group);
        if (!hidden.empty()) commitHidden(hidden, "Isolate group");
    }
}

bool SculptView::groupOp(GroupOp op) {
    if (stroking_ || doc_.mesh.triangleCount() == 0) return false;
    const sf::Mesh& m = doc_.mesh;
    switch (op) {
    case GroupOp::ShowAll: return m.hasHidden() && commitHidden(std::vector<uint8_t>(m.triangleCount(), 0), "Show all");
    case GroupOp::InvertVisibility: {
        const std::vector<uint8_t> hidden = sf::hiddenInverted(m);
        return !hidden.empty() && commitHidden(hidden, "Invert visibility");
    }
    case GroupOp::ByNormals: return commitGroups(sf::groupsByNormals(m, 30.0f), "Group by shape");
    case GroupOp::ByParts: return commitGroups(sf::groupsByConnectivity(m), "Group by parts");
    case GroupOp::FromMask: return m.hasMask() && commitGroups(sf::groupsFromMask(m), "Group from mask");
    case GroupOp::MergeVisible: return commitGroups(sf::groupsMergeVisible(m), "Merge groups");
    case GroupOp::Clear: return commitGroups(std::vector<uint32_t>(m.triangleCount(), 0), "Clear groups");
    }
    return false;
}

bool SculptView::maskOp(MaskOp op) {
    if (stroking_ || doc_.mesh.vertexCount() == 0) return false;
    sf::UndoRecord record = sf::UndoRecord::captureMask(doc_.mesh, "Mask");
    bool changed = false;
    switch (op) {
    case MaskOp::Clear: changed = sf::maskClear(doc_.mesh); break;
    case MaskOp::All: changed = sf::maskAll(doc_.mesh); break;
    case MaskOp::Invert: changed = sf::maskInvert(doc_.mesh); break;
    case MaskOp::Blur: changed = sf::maskBlur(doc_.mesh, 2); break;
    case MaskOp::Sharpen: changed = sf::maskSharpen(doc_.mesh, 2); break;
    case MaskOp::Grow: changed = sf::maskGrow(doc_.mesh, 1); break;
    case MaskOp::Shrink: changed = sf::maskShrink(doc_.mesh, 1); break;
    }
    if (!changed) return false;
    doc_.undo.push(std::move(record));
    ++revision_;
    ++editsSinceLoad_;
    return true;
}

bool SculptView::hitsModel(float x, float y, float w, float h) const {
    if (w <= 0 || h <= 0) return false;
    glm::vec3 origin, dir;
    sf::RayHit hit;
    camera_.screenRay(x, y, w, h, origin, dir);
    return doc_.bvh.raycast(doc_.mesh, origin, dir, hit);
}

bool SculptView::undo() {
    if (stroking_ || !doc_.undo.undo(doc_.mesh, doc_.bvh)) return false;
    sceneChanged();
    return true;
}

bool SculptView::redo() {
    if (stroking_ || !doc_.undo.redo(doc_.mesh, doc_.bvh)) return false;
    sceneChanged();
    return true;
}

void SculptView::updateBounds() {
    bounds_ = sf::AABB{};
    for (int i = 0; i < doc_.subtoolCount(); ++i)
        if (doc_.subtools[size_t(i)].visible || i == doc_.activeSubtool) {
            const sf::AABB b = doc_.subtoolMesh(i).bounds();
            if (b.valid()) {
                bounds_.expand(b.min);
                bounds_.expand(b.max);
            }
        }
}

void SculptView::sceneChanged() {
    for (int i = 0; i < doc_.subtoolCount(); ++i) {
        sf::Mesh& m = doc_.subtoolMesh(i);
        if (m.normals.size() != m.vertexCount()) m.recomputeAllNormals();
    }
    updateBounds();
    groupOverlay_.invalidate();
    ++revision_;
}

// --- Cut and starter models ----------------------------------------------------------------------------------------

void SculptView::applyCut() {
    // Upstream's `cut` with the shape in normalised device coordinates and this view (its mirror image cuts too
    // with symmetry on). A rectangle goes as its two corners, a line as its ends.
    if (shape_.size() < 2) return;
    if (drive_ == Drive::Line && glm::distance(shape_.front(), shape_.back()) < 8.0f) return; // (a tap)
    glm::mat4 vp;
    shapeNdc(vp); // (sets the camera's aspect to the stroke's window)
    std::string points;
    char buf[64];
    for (const glm::vec2& p : shape_) {
        std::snprintf(buf, sizeof buf, "%.6f %.6f ", 2.0 * p.x / shapeW_ - 1.0, 1.0 - 2.0 * p.y / shapeH_);
        points += buf;
    }
    std::string matrix;
    for (int i = 0; i < 16; ++i) {
        std::snprintf(buf, sizeof buf, "%.9g ", double(vp[i / 4][i % 4]));
        matrix += buf;
    }
    static const char* const modes[] = {"away", "in", "add"};
    sf::Args args{{"shape", std::string(drive_ == Drive::Lasso ? "lasso" : drive_ == Drive::Rect ? "rect" : "line")},
                  {"points", points},
                  {"mode", std::string(modes[int(cutMode)])},
                  {"depth", double(cutDepth)},
                  {"flip", invert},
                  {"view_proj", matrix}};
    if (symmetryX) args["symmetry"] = std::string("x");
    const sf::CommandResult r = commands_.execute(doc_, "cut", args);
    SDL_Log("cut: %s", r.message.c_str());
    if (!r.ok || r.message.find("misses") != std::string::npos) {
        notice = "missed";
        return;
    }
    sceneChanged();
    ++editsSinceLoad_;
}

std::vector<SculptView::StarterInfo> SculptView::starters() const {
    std::vector<StarterInfo> out;
    for (const sf::StarterMesh& m : sf::starterMeshes()) out.push_back({m.name, m.description});
    return out;
}

bool SculptView::newStarter(int index, std::string& error) {
    const auto& all = sf::starterMeshes();
    if (stroking_ || index < 0 || index >= int(all.size())) return false;
    if (!sf::loadStarterMesh(all[size_t(index)], doc_, -1, error)) return false;
    modelReplaced();
    return true;
}

// --- Topology -----------------------------------------------------------------------------------------------------

int SculptView::subdivLevels() const { return sf::subdivLevelCount(doc_.mesh); }
int SculptView::subdivLevel() const { return sf::subdivCurrentLevel(doc_.mesh); }
size_t SculptView::subdivLevelTriangles(int level) const { return sf::subdivLevelTriangles(doc_.mesh, level); }
std::string SculptView::dyntopoBlocker() const { return sf::dyntopoBlocker(doc_.mesh); }

// --- Sculpt layers ------------------------------------------------------------------------------------------------

std::vector<SculptView::LayerInfo> SculptView::layers() const {
    std::vector<LayerInfo> out;
    for (const sf::SculptLayer& l : doc_.mesh.layers) out.push_back({l.name, l.weight});
    return out;
}

void SculptView::setLayerWeight(int layer, float weight) {
    if (stroking_ || layer < 0 || layer >= int(doc_.mesh.layers.size())) return;
    doc_.mesh.setLayerWeight(layer, weight);
    doc_.mesh.markAllDirty();
    doc_.bvhDirty = true;
    doc_.modified = true;
    ++revision_;
    ++editsSinceLoad_;
}

// --- Parts --------------------------------------------------------------------------------------------------------

// --- Painting -----------------------------------------------------------------------------------------------------

glm::vec3 SculptView::paintColor() const { return glm::clamp(sf::linearToSrgb(doc_.paintColor), 0.0f, 1.0f); }
void SculptView::setPaintColor(const glm::vec3& srgb) { doc_.paintColor = sf::srgbToLinear(glm::clamp(srgb, 0.0f, 1.0f)); }

std::vector<SculptView::PaintLayerInfo> SculptView::paintLayers() const {
    std::vector<PaintLayerInfo> out;
    for (const sf::PaintLayer& l : doc_.mesh.paintLayers) out.push_back({l.name, l.opacity, l.visible});
    return out;
}

void SculptView::setPaintLayerLook(int layer, float opacity, bool visible) {
    sf::Mesh& m = doc_.mesh;
    if (stroking_ || layer < 0 || layer >= int(m.paintLayers.size())) return;
    sf::PaintLayer& l = m.paintLayers[size_t(layer)];
    l.opacity = std::clamp(opacity, 0.0f, 1.0f);
    l.visible = visible;
    sf::compositePaintAll(m);
    m.markAllDirty();
    doc_.modified = true;
    ++revision_;
}

bool SculptView::paintCommand(const std::string& name, const sf::Args& args, std::string* message) {
    const bool ok = runCommand(name, args, message);
    doc_.mesh.markAllDirty(); // (the colours of the whole Part may have changed)
    ++editsSinceLoad_;
    return ok;
}

void SculptView::renameLayer(int layer, const std::string& name) {
    if (layer < 0 || layer >= int(doc_.mesh.layers.size()) || name.empty()) return;
    doc_.mesh.layers[size_t(layer)].name = name;
    doc_.modified = true;
    ++revision_;
}

std::vector<SculptView::PartInfo> SculptView::parts() const {
    std::vector<PartInfo> out;
    for (int i = 0; i < doc_.subtoolCount(); ++i) {
        PartInfo p;
        p.name = doc_.subtools[size_t(i)].name;
        p.triangles = doc_.subtoolMesh(i).triangleCount();
        p.visible = doc_.subtools[size_t(i)].visible;
        p.active = i == doc_.activeSubtool;
        out.push_back(std::move(p));
    }
    return out;
}

bool SculptView::runCommand(const std::string& name, const sf::Args& args, std::string* message) {
    if (stroking_) return false;
    const sf::CommandResult r = commands_.execute(doc_, name, args);
    if (message) *message = r.message;
    if (!r.ok) {
        SDL_Log("%s: %s", name.c_str(), r.message.c_str());
        return false;
    }
    sceneChanged();
    ++editsSinceLoad_;
    return true;
}

void SculptView::rotate(float dx, float dy, float viewportHeight) {
    // Camera::orbit turns 0.008 rad a pixel: scale so a full-height drag is about half a turn.
    const float scale = 3.14159265f / std::max(viewportHeight, 1.0f) / 0.008f;
    camera_.orbit(dx * scale, dy * scale);
}

void SculptView::zoom(float factor) {
    if (factor <= 0.0f) return;
    camera_.distance = std::clamp(camera_.distance / factor, modelSize() * 0.2f, modelSize() * 50.0f);
}

void SculptView::pan(float dx, float dy, float viewportHeight) { camera_.pan(dx, dy, std::max(viewportHeight, 1.0f)); }

void SculptView::setView(int which) {
    constexpr float kHalf = 1.5707963f, kTop = 1.5690509f; // (the turntable stops just short of straight down)
    camera_.roll = 0.0f;
    camera_.pitch = 0.0f;
    switch (which) {
    case 0: camera_.yaw = 0.0f; break;
    case 1: camera_.yaw = 2.0f * kHalf; break;
    case 2: camera_.yaw = -kHalf; break;
    case 3: camera_.yaw = kHalf; break;
    case 4: camera_.yaw = 0.0f, camera_.pitch = -kTop; break;
    case 5: camera_.yaw = 0.0f, camera_.pitch = kTop; break;
    default: break;
    }
}

void SculptView::resetView() {
    camera_ = sf::Camera{};
    camera_.yaw = 0.0f;
    camera_.pitch = 0.0f;
    camera_.frame(bounds_);
}

// --- Speed test ------------------------------------------------------------------------------------------------
// Steps per size: 0 build the head and draw it once (the full upload), 1 turn the view (frames waited for), 2 a stroke.

void SculptView::startBenchmark() {
    benchLines_.clear();
    fixedRadius_ = 0.08f;
    benchSavedTool_ = tool;
    benchSavedPrefs_ = brushes.at(0);
    tool = 0; // Standard, as shipped
    brushes.reset(0);
    benchLines_.push_back("size | build | 1st frame (upload) | frame (fps) | draw CPU | dab | dab+upload | memory");
    benchStep_ = 0;
}

bool SculptView::benchmarkStep(int w, int h) {
    if (benchStep_ < 0) return false;
    const int size = benchStep_ / 3, phase = benchStep_ % 3;
    if (size >= int(std::size(kBenchSizes))) {
        benchStep_ = -1;
        fixedRadius_ = 0.0f;
        tool = benchSavedTool_;
        brushes.at(0) = benchSavedPrefs_;
        for (const std::string& line : benchLines_) SDL_Log("bench: %s", line.c_str());
        newSphere();
        return false;
    }
    static double buildMs, uploadMs;
    char buf[256];
    switch (phase) {
    case 0: {
        const auto t0 = Clock::now();
        newBenchHead(kBenchSizes[size]);
        buildMs = msSince(t0);
        uploadMs = timedFrame(w, h); // (the first frame uploads everything)
        break;
    }
    case 1: {
        double total = 0.0, cpu = 0.0;
        for (int i = 0; i < kBenchFrames; ++i) {
            camera_.yaw += 0.05f;
            total += timedFrame(w, h);
            cpu += lastDrawMs_;
        }
        std::snprintf(buf, sizeof buf, "%.1fM | %.0f ms | %.0f ms | %.1f ms (%.0f) | %.1f ms", double(triangleCount()) / 1e6, buildMs,
                      uploadMs, total / kBenchFrames, 1000.0 / (total / kBenchFrames), cpu / kBenchFrames);
        benchLines_.push_back(buf);
        break;
    }
    case 2: {
        // A stroke across the middle of the view: dabs a fixed distance apart, each followed by its upload.
        resetView();
        const float x0 = float(w) * 0.35f, x1 = float(w) * 0.65f, y = float(h) * 0.5f;
        double dabMs = 0.0, withUpload = 0.0;
        int dabs = 0;
        if (beginStroke(x0, y, float(w), float(h), 1.0f)) {
            syncScene();
            for (int i = 1; i <= kBenchDabs; ++i) {
                glm::vec3 origin, dir;
                sf::RayHit hit;
                camera_.screenRay(x0 + (x1 - x0) * float(i) / kBenchDabs, y, float(w), float(h), origin, dir);
                if (!doc_.bvh.raycast(doc_.mesh, origin, dir, hit)) continue;
                const auto t0 = Clock::now();
                dabAt(hit.position, dir, 1.0f, glm::vec2(0.0f));
                dabMs += lastDabMs_;
                syncScene();
                withUpload += msSince(t0);
                ++dabs;
            }
            endStroke();
        }
        std::snprintf(buf, sizeof buf, " | %.2f ms | %.2f ms | %s", dabs ? dabMs / dabs : 0.0, dabs ? withUpload / dabs : 0.0,
                      heapText().c_str());
        benchLines_.back() += buf;
        break;
    }
    }
    ++benchStep_;
    return true;
}

// --- Diagnostics -----------------------------------------------------------------------------------------------

void SculptView::runDiagnostics(int w, int h) {
    diagLines_.clear();
    const int savedTool = tool;
    const BrushPrefs savedPrefs = brushes.at(0);
    tool = 0; // Standard, as shipped
    brushes.reset(0);
    fixedRadius_ = 0.08f;
    char buf[256];
    auto add = [&] {
        diagLines_.push_back(buf);
        SDL_Log("diag: %s", buf);
    };

    // 1. A plain loop over 16 MB (memory loads and float math, no sculpting code).
    {
        std::vector<float> a(4'000'000);
        for (size_t i = 0; i < a.size(); ++i) a[i] = float(i & 1023) * 0.001f;
        const auto t0 = Clock::now();
        float sum = 0.0f;
        for (int rep = 0; rep < 10; ++rep)
            for (size_t i = 0; i < a.size(); ++i) {
                a[i] = a[i] * 0.9999f + 0.5f;
                sum += a[i];
            }
        std::snprintf(buf, sizeof buf, "loop 40M: %.1f ms (sum %.0f)", msSince(t0), double(sum));
        add();
    }

    // 2. Raycasts across the view.
    {
        int hits = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < 1000; ++i) {
            glm::vec3 origin, dir;
            sf::RayHit hit;
            camera_.screenRay(float(w) * (0.3f + 0.4f * float(i % 40) / 40.0f), float(h) * (0.3f + 0.4f * float(i / 40) / 25.0f),
                              float(w), float(h), origin, dir);
            if (doc_.bvh.raycast(doc_.mesh, origin, dir, hit)) ++hits;
        }
        std::snprintf(buf, sizeof buf, "raycast x1000: %.1f ms (%d hits)", msSince(t0), hits);
        add();
    }

    // 3. 100 dabs back and forth across the middle of the view, each timed.
    {
        std::vector<float> ms;
        const float y = float(h) * 0.5f;
        const auto pointAt = [&](int i, glm::vec3& pos, glm::vec3& dir) {
            const float t = float(i % 50) / 50.0f, u = (i / 50) % 2 ? 1.0f - t : t;
            glm::vec3 origin;
            sf::RayHit hit;
            camera_.screenRay(float(w) * (0.35f + 0.3f * u), y, float(w), float(h), origin, dir);
            if (!doc_.bvh.raycast(doc_.mesh, origin, dir, hit)) return false;
            pos = hit.position;
            return true;
        };
        if (beginStroke(float(w) * 0.35f, y, float(w), float(h), 1.0f)) {
            ms.push_back(lastDabMs_);
            for (int i = 1; i < 100; ++i) {
                glm::vec3 pos, dir;
                if (!pointAt(i, pos, dir)) continue;
                dabAt(pos, dir, 1.0f, glm::vec2(0.0f));
                ms.push_back(lastDabMs_);
            }
            const auto t0 = Clock::now();
            endStroke();
            const double endMs = msSince(t0);
            std::snprintf(buf, sizeof buf, "dabs: first %.1f, 2nd %.1f, 3rd %.1f, last %.1f ms; end stroke %.1f ms", ms[0],
                          ms.size() > 1 ? ms[1] : 0.0f, ms.size() > 2 ? ms[2] : 0.0f, ms.back(), endMs);
            add();
            std::vector<float> sorted = ms;
            std::sort(sorted.begin(), sorted.end());
            std::snprintf(buf, sizeof buf, "dabs x%d: min %.2f, median %.2f, max %.2f ms (radius %.3f, %d tris)", int(ms.size()),
                          sorted.front(), sorted[sorted.size() / 2], sorted.back(), fixedRadius_, triangleCount());
            add();
            undo(); // leave the model as it was
        } else {
            std::snprintf(buf, sizeof buf, "dabs: the middle of the view misses the model");
            add();
        }
    }
    std::snprintf(buf, sizeof buf, "%s", heapText().c_str());
    add();
    fixedRadius_ = 0.0f;
    tool = savedTool;
    brushes.at(0) = savedPrefs;
}
