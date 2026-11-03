// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Community brush gallery: SHA-256, index parsing and validation, gallery
// paths, search, installing from a local gallery folder, and the `stroke`
// command used to render brush previews.
#include "TestHarness.h"

#include "ai/BrushGalleryClient.h"
#include "core/BrushGallery.h"
#include "core/Commands.h"
#include "core/PathStroke.h"
#include "core/Primitives.h"
#include "core/Sha256.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace sf;

namespace {

const char* kIndex = R"({
  "format": "sculptforge-brush-gallery",
  "version": 1,
  "brushes": [
    {"name": "Snake Scales", "description": "Small scales.", "author": "Ann", "tags": ["Creature", "scales"],
     "file": "brushes/snake-scales.sfbrush", "preview": "previews/snake-scales.png",
     "sha256": "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855", "added": "2026-10-21"},
    {"name": "Cracked Earth", "description": "Dry mud.", "author": "Bo", "tags": ["environment", "cracks"],
     "file": "brushes/cracked-earth.sfbrush", "preview": "previews/cracked-earth.png",
     "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"name": "Escape", "file": "../../etc/passwd.sfbrush",
     "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"name": "No Hash", "file": "brushes/no-hash.sfbrush"},
    {"name": "snake scales", "file": "brushes/dupe.sfbrush",
     "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"name": "Odd Preview", "file": "brushes/odd.sfbrush", "preview": "https://evil.example/x.png",
     "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    42
  ]
})";

// Points the per-user data folder (where the gallery cache lives) at `dir` while alive.
struct ScopedDataDir {
#if defined(_WIN32)
    static constexpr const char* kVar = "APPDATA";
#else
    static constexpr const char* kVar = "XDG_DATA_HOME";
#endif
    std::string old;
    bool had = false;
    explicit ScopedDataDir(const std::string& dir) {
        if (const char* v = std::getenv(kVar)) old = v, had = true;
        set(dir.c_str());
    }
    ~ScopedDataDir() { had ? set(old.c_str()) : unset(); }
    static void set(const char* v) {
#if defined(_WIN32)
        _putenv_s(kVar, v);
#else
        setenv(kVar, v, 1);
#endif
    }
    static void unset() {
#if defined(_WIN32)
        _putenv_s(kVar, "");
#else
        unsetenv(kVar);
#endif
    }
};

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

} // namespace

TEST(sha256_known_vectors) {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // 55 / 56 / 64 bytes: the padding boundary cases.
    CHECK(sha256Hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha256Hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(sha256Hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(gallery_index_parses_and_skips_bad_entries) {
    GalleryIndex index;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(parseGalleryIndex(kIndex, index, error, &warnings));
    CHECK(index.brushes.size() == 3); // Snake Scales, Cracked Earth, Odd Preview
    CHECK(index.brushes[0].name == "Cracked Earth"); // sorted by name
    CHECK(index.brushes[2].name == "Snake Scales");
    const GalleryBrush& snake = index.brushes[2];
    CHECK(snake.author == "Ann");
    CHECK(snake.tags.size() == 2 && snake.tags[0] == "creature"); // lowercased
    CHECK(snake.sha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(snake.added == "2026-10-21");
    CHECK(index.brushes[1].name == "Odd Preview" && index.brushes[1].preview.empty()); // remote preview dropped
    CHECK(warnings.size() == 5); // escape, no hash, duplicate, odd preview, not an object
}

TEST(gallery_index_rejects_wrong_documents) {
    GalleryIndex index;
    std::string error;
    CHECK(!parseGalleryIndex("not json", index, error));
    CHECK(!parseGalleryIndex(R"({"brushes": []})", index, error));
    CHECK(!parseGalleryIndex(R"({"format": "sculptforge-brush-gallery", "version": 2, "brushes": []})", index, error));
    CHECK(error.find("newer") != std::string::npos);
    CHECK(parseGalleryIndex(R"({"format": "sculptforge-brush-gallery", "version": 1, "brushes": []})", index, error));
    CHECK(index.brushes.empty());
}

TEST(gallery_paths_are_confined) {
    CHECK(isSafeGalleryPath("brushes/snake-scales.sfbrush"));
    CHECK(isSafeGalleryPath("previews/a_b.1.png"));
    CHECK(!isSafeGalleryPath(""));
    CHECK(!isSafeGalleryPath("/etc/passwd"));
    CHECK(!isSafeGalleryPath("brushes/../../x.sfbrush"));
    CHECK(!isSafeGalleryPath("brushes\\x.sfbrush"));
    CHECK(!isSafeGalleryPath("https://evil.example/x.sfbrush"));
    CHECK(!isSafeGalleryPath("brushes//x.sfbrush"));
    CHECK(!isSafeGalleryPath("C:/x.sfbrush"));
    CHECK(isSha256Hex(std::string(64, 'a')) && !isSha256Hex(std::string(63, 'a')) && !isSha256Hex(std::string(64, 'g')));
}

TEST(gallery_locations_resolve) {
    CHECK(galleryIndexLocation("") == kDefaultBrushGalleryUrl);
    CHECK(galleryIndexLocation("  https://example.com/g/index.json ") == "https://example.com/g/index.json");
    CHECK(galleryIndexLocation("https://example.com/g/") == "https://example.com/g/index.json");
    CHECK(galleryIndexLocation("/srv/brushes") == "/srv/brushes/index.json");
    CHECK(galleryIndexLocation("C:\\Studio\\Brushes") == "C:\\Studio\\Brushes\\index.json");
    CHECK(resolveGalleryPath("https://example.com/g/index.json", "previews/a.png") == "https://example.com/g/previews/a.png");
    CHECK(resolveGalleryPath("/srv/brushes/index.json", "brushes/a.sfbrush") == "/srv/brushes/brushes/a.sfbrush");
    CHECK(resolveGalleryPath("file:///srv/g/index.json", "brushes/a.sfbrush") == "file:///srv/g/brushes/a.sfbrush");
    CHECK(galleryLocalPath("https://example.com/index.json").empty());
    CHECK(galleryLocalPath("HTTP://example.com/index.json").empty());
    CHECK(galleryLocalPath("/srv/brushes/index.json") == "/srv/brushes/index.json");
#if !defined(_WIN32)
    CHECK(galleryLocalPath("file:///srv/my%20brushes/index.json") == "/srv/my brushes/index.json");
#endif
}

TEST(gallery_hash_check) {
    const std::string data = "{\"name\": \"X\"}";
    CHECK(galleryHashMatches(data, sha256Hex(data)));
    std::string upper = sha256Hex(data);
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    CHECK(galleryHashMatches(data, upper));
    CHECK(!galleryHashMatches(data + " ", sha256Hex(data)));
    CHECK(!galleryHashMatches(data, "nothex"));
}

TEST(gallery_search_and_tags) {
    GalleryIndex index;
    std::string error;
    CHECK(parseGalleryIndex(kIndex, index, error));
    const GalleryBrush& snake = index.brushes[2];
    CHECK(galleryBrushMatches(snake, "", ""));
    CHECK(galleryBrushMatches(snake, "SCALES", ""));
    CHECK(galleryBrushMatches(snake, "small creature", "")); // every word, anywhere
    CHECK(!galleryBrushMatches(snake, "small mud", ""));
    CHECK(galleryBrushMatches(snake, "", "Creature"));
    CHECK(!galleryBrushMatches(snake, "", "cracks"));
    CHECK(galleryBrushMatches(snake, "ann", "")); // author
    const auto tags = galleryTags(index);
    CHECK(tags.size() == 4);
}

TEST(gallery_install_from_local_folder) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(test::tempPath("sf_gallery_test"));
    fs::remove_all(dir);
    fs::create_directories(dir / "brushes");
    ScopedDataDir dataDir((dir / "appdata").string());
    BrushRecipe r;
    r.name = "Test Gallery Brush";
    r.depth = -0.3f;
    std::string err;
    CHECK(r.save((dir / "brushes" / "test-gallery-brush.sfbrush").string(), err));
    std::string brushText;
    {
        std::ifstream f(dir / "brushes" / "test-gallery-brush.sfbrush", std::ios::binary);
        brushText.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    auto writeIndex = [&](const std::string& sha) {
        writeText(dir / "index.json", std::string(R"({"format": "sculptforge-brush-gallery", "version": 1, "brushes": [
            {"name": "Test Gallery Brush", "tags": ["test"], "file": "brushes/test-gallery-brush.sfbrush",
             "sha256": ")") + sha + "\"}]}");
    };
    writeIndex(sha256Hex(brushText));

    CommandRegistry reg;
    ai::registerBrushGalleryCommands(reg);
    Document doc;
    const std::string gallery = dir.string();
    CommandResult res = reg.execute(doc, "brush_gallery_search", {{"tag", std::string("test")}, {"gallery", gallery}});
    CHECK(res.ok && res.message.find("Test Gallery Brush") != std::string::npos);
    res = reg.execute(doc, "brush_gallery_install", {{"name", std::string("test gallery brush")}, {"gallery", gallery}});
    CHECK(res.ok);
    CHECK(doc.activeBrush >= 0 && doc.brushes[doc.activeBrush].name == "Test Gallery Brush");
    CHECK(std::abs(doc.brushes[doc.activeBrush].depth + 0.3f) < 1e-4f);

    // A tampered file (hash no longer matches) is refused.
    writeIndex(std::string(64, '0'));
    const size_t before = doc.brushes.size();
    res = reg.execute(doc, "brush_gallery_install", {{"name", std::string("Test Gallery Brush")}, {"gallery", gallery}});
    CHECK(!res.ok && res.message.find("checksum") != std::string::npos);
    CHECK(doc.brushes.size() == before);
    res = reg.execute(doc, "brush_gallery_install", {{"name", std::string("Missing")}, {"gallery", gallery}});
    CHECK(!res.ok);
    fs::remove_all(dir);
}

TEST(gallery_index_cached_for_offline_use) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(test::tempPath("sf_gallery_cache_test"));
    fs::remove_all(dir);
    ScopedDataDir dataDir((dir / "appdata").string());
    const std::string location = galleryIndexLocation((dir / "gallery").string());
    writeText(dir / "gallery" / "index.json", kIndex);
    ai::GalleryFetch r = ai::fetchGalleryIndex(location);
    CHECK(r.ok && !r.fromCache);
    fs::remove_all(dir / "gallery"); // the gallery goes away: the last good copy is used
    r = ai::fetchGalleryIndex(location);
    CHECK(r.ok && r.fromCache && !r.error.empty());
    GalleryIndex index;
    std::string error;
    CHECK(parseGalleryIndex(r.data, index, error) && index.brushes.size() == 3);
    // A gallery that answers with something broken is an error, not a silent fallback.
    writeText(dir / "gallery" / "index.json", "{\"oops\": 1}");
    r = ai::fetchGalleryIndex(location);
    CHECK(!r.ok);
    // Never-seen galleries have no cache.
    CHECK(!ai::fetchGalleryIndex(galleryIndexLocation((dir / "nowhere").string())).ok);
    fs::remove_all(dir);
}

TEST(stroke_path_sampling) {
    // Straight segments at even spacing, ending within one step of the last point.
    auto s = samplePath({{0, 0, 0}, {1, 0, 0}}, false, 0.25f);
    CHECK(s.size() == 5);
    CHECK(glm::distance(s.back(), glm::vec3(1, 0, 0)) < 1e-4f);
    // Curves pass through every control point's neighbourhood and stay evenly spaced.
    s = samplePath({{0, 0, 0}, {1, 1, 0}, {2, 0, 0}}, true, 0.05f);
    CHECK(s.size() > 40);
    bool even = true;
    for (size_t i = 1; i < s.size(); ++i) even = even && std::abs(glm::distance(s[i], s[i - 1]) - 0.05f) < 0.01f;
    CHECK(even);
    float closest = 1e9f;
    for (const auto& p : s) closest = std::min(closest, glm::distance(p, glm::vec3(1, 1, 0)));
    CHECK(closest < 0.05f);
    CHECK(samplePath({{1, 2, 3}}, true, 0.1f).size() == 1);

    std::vector<glm::vec3> pts;
    std::string err;
    CHECK(parsePathPoints("0,0,0 1,2.5,-3|4,5,6", pts, err) && pts.size() == 3 && pts[1].z == -3.0f);
    CHECK(!parsePathPoints("0,0", pts, err));
    CHECK(!parsePathPoints("0,0,0,", pts, err));
    CHECK(!parsePathPoints("  ", pts, err));
}

TEST(stroke_command_sculpts_along_path) {
    CommandRegistry reg;
    Document doc = test::makeDoc(makePlane(80, 2.0f)); // XZ plane, normal +Y
    const Args args = {{"points", std::string("-0.6,0,0 0,0,0.2 0.6,0,0")},
                       {"view", std::string("0,-1,0")},
                       {"brush", std::string("Dragon Scales")},
                       {"radius", 0.1}};
    CommandResult r = reg.execute(doc, "stroke", args);
    CHECK(r.ok);
    CHECK(doc.undo.canUndo());
    float maxY = 0.0f, farY = 0.0f;
    for (const auto& p : doc.mesh.positions) {
        if (std::abs(p.z - 0.1f) < 0.15f && std::abs(p.x) < 0.5f) maxY = std::max(maxY, std::abs(p.y));
        if (p.z < -0.5f) farY = std::max(farY, std::abs(p.y));
    }
    CHECK(maxY > 1e-3f); // the path moved the surface
    CHECK(farY == 0.0f); // and nothing far from it
    r = reg.execute(doc, "undo");
    CHECK(r.ok);
    bool flat = true;
    for (const auto& p : doc.mesh.positions) flat = flat && p.y == 0.0f;
    CHECK(flat);

    // Built-in brushes by name (any case), inverted; errors are reported.
    CHECK(reg.execute(doc, "stroke", {{"points", std::string("0,0,0 0.3,0,0")}, {"view", std::string("0,-1,0")},
                                      {"brush", std::string("clay")}, {"invert", true}})
              .ok);
    CHECK(!reg.execute(doc, "stroke", {{"points", std::string("0,0,0")}, {"brush", std::string("Nope")}}).ok);
    CHECK(!reg.execute(doc, "stroke", {{"points", std::string("5,0,5 6,0,6")}, {"view", std::string("0,-1,0")}}).ok);
    CHECK(!reg.execute(doc, "stroke", {{"points", std::string("0,0,0")}, {"view", std::string("0,0,0")}}).ok);
}
