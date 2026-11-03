// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "SculptView.h"

#include "core/Benchmark.h"
#include "core/Multires.h"
#include "core/Primitives.h"
#include "core/Project.h"
#include "core/Kelvinlet.h"
#include "core/Masking.h"
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
    if (ready_) renderer_.shutdown();
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
    bounds_ = sf::AABB{};
    for (int i = 0; i < doc_.subtoolCount(); ++i)
        if (doc_.subtools[size_t(i)].visible || i == doc_.activeSubtool) {
            const sf::AABB b = doc_.subtoolMesh(i).bounds();
            if (b.valid()) {
                bounds_.expand(b.min);
                bounds_.expand(b.max);
            }
        }
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
    renderer_.draw(camera_, pixelWidth, pixelHeight, settings);
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
    p.radius = worldRadius(center) * (b.pressureSize && fixedRadius_ <= 0.0f ? std::max(pr, 0.05f) : 1.0f);
    p.strength = b.strength;
    p.pressure = b.pressureStrength ? pr : 1.0f;
    p.invert = invert;
    p.focal = b.focal;
    p.keepVolume = toolDef().type == sf::BrushType::Smooth && !toolDef().mask && b.keepVolume;
    p.frontFacesOnly = b.frontFacesOnly;
    p.connectedOnly = b.connectedOnly;
    p.strokeDelta = center - lastCenter_;
    return p;
}

void SculptView::dabAt(const glm::vec3& center, const glm::vec3& viewDir, float pressure) {
    const sf::DabParams p = dabParams(center, viewDir, pressure);
    const auto t0 = Clock::now();
    if (toolDef().mask) doc_.sculptor.dabMask(doc_.mesh, doc_.bvh, p, symmetry());
    else doc_.sculptor.dab(doc_.mesh, doc_.bvh, toolDef().type, p, symmetry());
    lastDabMs_ = float(msSince(t0));
    ++dabsInStroke_;
    lastCenter_ = center;
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
    glm::vec3 origin, dir;
    sf::RayHit hit;
    if (w <= 0 || h <= 0) return false;
    viewHeight_ = h;
    if (toolDef().drive == Drive::Lasso || toolDef().drive == Drive::Rect) {
        // A mask shape starts anywhere (round empty space it clears the mask); it acts when the pen lifts.
        stroking_ = true;
        drive_ = toolDef().drive;
        shape_.assign(drive_ == Drive::Rect ? 2 : 1, glm::vec2(x, y));
        shapeW_ = w, shapeH_ = h;
        strokeX_ = x, strokeY_ = y, strokeRadiusPx_ = 0.0f;
        return true;
    }
    camera_.screenRay(x, y, w, h, origin, dir);
    if (!doc_.bvh.raycast(doc_.mesh, origin, dir, hit)) return false;
    if (!doc_.sculptor.beginStroke(doc_.mesh, toolDef().key)) return false;
    const BrushPrefs& b = prefs();
    stroking_ = true;
    drive_ = toolDef().drive;
    dabsInStroke_ = 0;
    lastCenter_ = hit.position;
    lastPressure_ = pressure;
    lazy_.radius = b.steadyPx;
    lazy_.reset({x, y});
    strokeX_ = x, strokeY_ = y;
    const float pr = b.curve.apply(std::clamp(pressure, 0.0f, 1.0f));
    strokeRadiusPx_ = b.radiusPx * (b.pressureSize ? std::max(pr, 0.05f) : 1.0f);
    grabLast_ = hit.position;
    const sf::DabParams p = dabParams(hit.position, camera_.forward(), pressure);
    switch (drive_) {
    case Drive::Dabs: dabAt(hit.position, dir, pressure); break;
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
    case Drive::Rect: break; // (above)
    }
    return true;
}

void SculptView::pullTo(const glm::vec3& origin, const glm::vec3& dir, float pressure) {
    // As upstream's snakeHookTo: small dabs from the last point towards the pen's point on the drag plane, each one
    // pulling the surface along by its own step.
    glm::vec3 target;
    if (!viewPlanePoint(origin, dir, grabLast_, target)) return;
    sf::DabParams p = dabParams(grabLast_, camera_.forward(), pressure);
    const glm::vec3 delta = target - grabLast_;
    const int steps = std::clamp(int(std::ceil(glm::length(delta) / std::max(p.radius * prefs().spacing, 1e-6f))), 1, 64);
    p.strokeDelta = delta / float(steps);
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
    if (drive_ == Drive::Rect) {
        shape_.back() = {x, y};
        strokeX_ = x, strokeY_ = y;
        return;
    }
    if (drive_ == Drive::Lasso) {
        if (glm::distance(shape_.back(), glm::vec2(x, y)) >= 3.0f) shape_.push_back({x, y});
        strokeX_ = x, strokeY_ = y;
        return;
    }
    const BrushPrefs& b = prefs();
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
    case Drive::Pull: pullTo(origin, dir, pressure); return;
    case Drive::Elastic: elasticTo(origin, dir, pressure); return;
    case Drive::Turn: turnTo(x, pressure); return;
    case Drive::Lasso:
    case Drive::Rect: return; // (above)
    case Drive::Dabs: break;
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
    const float spacing = std::max(worldRadius(hit.position) * b.spacing, 1e-6f);
    const float length = glm::distance(from, hit.position);
    const int steps = int(length / spacing);
    for (int i = 1; i <= steps; ++i) {
        const float t = float(i) * spacing / length;
        dabAt(glm::mix(from, hit.position, t), dir, glm::mix(lastPressure_, pressure, t));
    }
    if (steps > 0) lastPressure_ = pressure;
}

void SculptView::endStroke() {
    if (!stroking_) return;
    if (drive_ == Drive::Lasso || drive_ == Drive::Rect) {
        applyMaskShape(); // (reads the shape while the stroke still counts as running)
        stroking_ = false;
        shape_.clear();
        return;
    }
    stroking_ = false;
    doc_.sculptor.flushDetailClone(doc_.mesh, doc_.bvh, symmetry()); // (a Kelvinlet stroke's deferred far field)
    doc_.undo.push(doc_.sculptor.endStroke());
    ++revision_;
    ++editsSinceLoad_;
}

void SculptView::cancelStroke() {
    if (!stroking_) return;
    if (drive_ == Drive::Lasso || drive_ == Drive::Rect) { // (nothing was applied yet)
        stroking_ = false;
        shape_.clear();
        return;
    }
    endStroke();
    doc_.undo.undo(doc_.mesh, doc_.bvh);
}

// --- Mask ---------------------------------------------------------------------------------------------------------

std::vector<glm::vec2> SculptView::maskShape() const {
    if (!stroking_ || (drive_ != Drive::Lasso && drive_ != Drive::Rect) || shape_.empty()) return {};
    if (drive_ == Drive::Lasso) return shape_;
    const glm::vec2 a = shape_.front(), b = shape_.back();
    return {a, {b.x, a.y}, b, {a.x, b.y}};
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
    ++revision_;
    return true;
}

bool SculptView::redo() {
    if (stroking_ || !doc_.undo.redo(doc_.mesh, doc_.bvh)) return false;
    ++revision_;
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
                dabAt(hit.position, dir, 1.0f);
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
                dabAt(pos, dir, 1.0f);
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
