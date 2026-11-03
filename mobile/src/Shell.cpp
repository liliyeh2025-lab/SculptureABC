// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "Shell.h"

#include "FileStore.h"
#include "I18n.h"
#include "InputProbe.h"
#include "SculptView.h"
#include "ViewportInput.h"

#include "core/Precision.h"
#include "core/Sculpt.h"
#include "render/Renderer.h"

#include <IconsLucide.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_misc.h> // SDL_OpenURL
#include <imgui.h>
#include <imgui_internal.h> // ImTextCharFromUtf8

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {

const char* const kSourceUrl = "https://github.com/liliyeh2025-lab/SculptureABC";

// Colours (Nomad-like dark bars, one blue accent).
const ImU32 kAccent = IM_COL32(64, 140, 230, 255);
const ImU32 kPressed = IM_COL32(255, 255, 255, 40);
const ImU32 kHover = IM_COL32(255, 255, 255, 18);
const ImU32 kText = IM_COL32(230, 232, 236, 255);
const ImU32 kDim = IM_COL32(140, 144, 152, 255);

constexpr float kBarH = 52.0f;    // top bar
constexpr float kStripW = 76.0f;  // left and right strips
constexpr float kTile = 60.0f;    // strip buttons
constexpr float kBrushW = 70.0f, kBrushH = 66.0f;
constexpr float kGap = 8.0f;
constexpr float kRadiusMin = 4.0f, kRadiusMax = 300.0f;

const ImGuiWindowFlags kBarFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
const ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoCollapse;

// 1234567 -> "1,234,567"
std::string grouped(long long n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// Every icon the shell draws (the font atlas carries only these: all of Lucide would need a 2048 x 4096 texture).
const char* const kUsedIcons = ICON_LC_BALLOON ICON_LC_BOX ICON_LC_CIRCLE ICON_LC_CIRCLE_DOT ICON_LC_CIRCLE_MINUS
    ICON_LC_EQUAL ICON_LC_FILE_BOX ICON_LC_FLIP_HORIZONTAL_2 ICON_LC_FOCUS ICON_LC_FOLDER_OPEN ICON_LC_GRID_3X3 ICON_LC_LAYERS
    ICON_LC_LAYOUT_GRID ICON_LC_MENU ICON_LC_MOVE ICON_LC_PRINTER ICON_LC_REDO_2 ICON_LC_ROTATE_3D ICON_LC_SETTINGS
    ICON_LC_SHRINK ICON_LC_SLASH ICON_LC_SPARKLES ICON_LC_TRIANGLE_ALERT ICON_LC_UNDO_2 ICON_LC_WAVES
    // the tools of Brushes.cpp, the brush settings
    ICON_LC_LAYERS_2 ICON_LC_PEN_TOOL ICON_LC_SCISSORS ICON_LC_HAND ICON_LC_SPLINE ICON_LC_WORM ICON_LC_ARROW_RIGHT_LEFT
    ICON_LC_ROTATE_CW ICON_LC_SCALE_3D ICON_LC_PAINTBRUSH ICON_LC_SLIDERS_HORIZONTAL ICON_LC_ROTATE_CCW
    // the Mask panel
    ICON_LC_CONTRAST ICON_LC_LASSO ICON_LC_SQUARE_DASHED ICON_LC_FLIP_VERTICAL ICON_LC_ERASER ICON_LC_DROPLET
    ICON_LC_MAXIMIZE_2 ICON_LC_MINIMIZE_2 ICON_LC_SQUARE
    // Settings
    ICON_LC_MAXIMIZE ICON_LC_MINIMIZE;

bool mouseHover() { return ImGui::GetIO().MouseSource == ImGuiMouseSource_Mouse && ImGui::IsItemHovered(); }

} // namespace

// --- Fonts and style --------------------------------------------------------------------------------------------

void Shell::loadFonts(float uiScale, float pixelDensity, const std::string& assetDir, const std::string& mobileDir) {
    ImGuiIO& io = ImGui::GetIO();
    density_ = pixelDensity;
    const std::string inter = assetDir + "/fonts/Inter-Regular.ttf";
    const std::string noto = mobileDir + "/assets/fonts/NotoSansTC-Subset.ttf";
    const std::string lucide = assetDir + "/fonts/lucide.ttf";

    // Only the characters the interface uses (the subset has no others anyway).
    static ImVector<ImWchar> cjk;
    ImFontGlyphRangesBuilder builder;
    builder.AddText(i18n::allText().c_str());
    builder.AddText("，。、：；（）！？…／％「」");
    builder.BuildRanges(&cjk);
    static ImVector<ImWchar> iconRanges;
    {
        ImFontGlyphRangesBuilder icons;
        icons.AddText(kUsedIcons);
        icons.BuildRanges(&iconRanges);
    }
    const ImWchar* iconRange = iconRanges.Data;

    auto make = [&](float size, bool withIcons) {
        const float px = size * uiScale * pixelDensity; // rasterise at device pixels, show at window units
        ImFontConfig base;
        base.OversampleH = 2;
        ImFont* font = io.Fonts->AddFontFromFileTTF(inter.c_str(), px, &base);
        if (!font) {
            ImFontConfig fallback;
            fallback.SizePixels = px;
            font = io.Fonts->AddFontDefault(&fallback);
        }
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.OversampleH = 1;
        if (!io.Fonts->AddFontFromFileTTF(noto.c_str(), px, &merge, cjk.Data)) SDL_Log("font: can't read %s", noto.c_str());
        if (withIcons) {
            ImFontConfig icons;
            icons.MergeMode = true;
            icons.GlyphMinAdvanceX = px;
            icons.GlyphOffset.y = px * 0.12f;
            if (!io.Fonts->AddFontFromFileTTF(lucide.c_str(), px, &icons, iconRange)) SDL_Log("font: can't read %s", lucide.c_str());
        }
        return font;
    };
    ui_ = make(17.0f, true);
    small_ = make(14.0f, false);
    {
        const float px = 26.0f * uiScale * pixelDensity;
        ImFontConfig icons;
        icons.GlyphMinAdvanceX = px;
        icon_ = io.Fonts->AddFontFromFileTTF(lucide.c_str(), px, &icons, iconRange);
        if (!icon_) icon_ = ui_;
    }
    io.FontDefault = ui_;
    io.FontGlobalScale = 1.0f / pixelDensity;

    // Characters of the table the font lacks (the subset is older than zh-TW.json: run mobile/tools/make-font-subset.py).
    const bool built = io.Fonts->Build();
    SDL_Log("fonts: built %d, atlas %dx%d, glyphs ui %d small %d icon %d, size %.0f/%.0f/%.0f px", int(built),
            io.Fonts->TexWidth, io.Fonts->TexHeight, ui_->Glyphs.Size, small_->Glyphs.Size, icon_->Glyphs.Size, ui_->FontSize,
            small_->FontSize, icon_->FontSize);
    int missing = 0;
    std::string sample;
    for (const char* p = i18n::allText().c_str(); *p;) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, nullptr);
        if (n <= 0) break;
        if (c >= 0x2E80 && !ui_->FindGlyphNoFallback(ImWchar(c))) {
            if (++missing <= 8) sample.append(p, size_t(n));
        }
        p += n;
    }
    if (missing) SDL_Log("font: %d characters missing from the Chinese subset (%s...)", missing, sample.c_str());
}

void Shell::applyStyle(float uiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(uiScale);
    style.WindowRounding = 14.0f * uiScale;
    style.FrameRounding = 8.0f * uiScale;
    style.GrabRounding = 8.0f * uiScale;
    style.PopupRounding = 10.0f * uiScale;
    style.WindowBorderSize = 0.0f;
    style.FramePadding = ImVec2(10.0f * uiScale, 8.0f * uiScale); // finger-sized
    style.ItemSpacing = ImVec2(8.0f * uiScale, 8.0f * uiScale);
    style.WindowPadding = ImVec2(8.0f * uiScale, 8.0f * uiScale);
    style.GrabMinSize = 30.0f * uiScale;
    style.ScrollbarSize = 16.0f * uiScale;
    ImVec4* col = style.Colors;
    col[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.90f);
    col[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.15f, 0.97f);
    col[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
    col[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.29f, 1.0f);
    col[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.29f, 0.33f, 1.0f);
    col[ImGuiCol_SliderGrab] = ImVec4(0.25f, 0.55f, 0.90f, 1.0f);
    col[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.66f, 0.98f, 1.0f);
    col[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.66f, 0.98f, 1.0f);
    col[ImGuiCol_Button] = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
    col[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.28f, 0.32f, 1.0f);
    col[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.55f, 0.90f, 1.0f);
    col[ImGuiCol_Header] = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
    col[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.28f, 0.32f, 1.0f);
    col[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.55f, 0.90f, 1.0f);
}

// --- Buttons ----------------------------------------------------------------------------------------------------

bool Shell::tile(const char* id, const char* icon, const char* label, float w, float h, bool active, bool enabled) {
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##tile", ImVec2(w, h)) && enabled;
    const bool held = ImGui::IsItemActive() && enabled;
    const bool hover = mouseHover() && enabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (active) dl->AddRectFilled(a, b, kAccent, rounding);
    if (held) dl->AddRectFilled(a, b, kPressed, rounding);
    else if (hover) dl->AddRectFilled(a, b, kHover, rounding);
    const ImU32 fg = !enabled ? IM_COL32(110, 112, 118, 255) : active ? IM_COL32_WHITE : kText;
    const float iconSize = icon_->FontSize / density_, labelSize = small_->FontSize / density_;
    const bool hasLabel = label && *label;
    const ImVec2 is = icon_->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon);
    const float top = hasLabel ? a.y + (h - iconSize - labelSize - 2.0f) * 0.5f : a.y + (h - is.y) * 0.5f;
    dl->AddText(icon_, iconSize, ImVec2(a.x + (w - is.x) * 0.5f, top), fg, icon);
    if (hasLabel) {
        const ImVec2 ls = small_->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, label);
        dl->AddText(small_, labelSize, ImVec2(a.x + std::max((w - ls.x) * 0.5f, 2.0f), top + iconSize + 2.0f), fg, label);
    }
    ImGui::PopID();
    return pressed;
}

bool Shell::wideButton(const char* id, const char* icon, const char* label, float w, float h, bool active, bool enabled) {
    ImGui::PushID(id);
    const float uiSize = ui_->FontSize / density_;
    const ImVec2 ls = ui_->CalcTextSizeA(uiSize, FLT_MAX, 0.0f, label);
    const float pad = 12.0f, iconW = icon ? uiSize + 8.0f : 0.0f;
    if (w <= 0.0f) w = pad * 2.0f + iconW + ls.x;
    const bool pressed = ImGui::InvisibleButton("##wide", ImVec2(w, h)) && enabled;
    const bool held = ImGui::IsItemActive() && enabled;
    const bool hover = mouseHover() && enabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (active) dl->AddRectFilled(a, b, kAccent, rounding);
    if (held) dl->AddRectFilled(a, b, kPressed, rounding);
    else if (hover) dl->AddRectFilled(a, b, kHover, rounding);
    const ImU32 fg = !enabled ? IM_COL32(110, 112, 118, 255) : active ? IM_COL32_WHITE : kText;
    const float y = a.y + (h - uiSize) * 0.5f;
    if (icon) dl->AddText(ui_, uiSize, ImVec2(a.x + pad, y), fg, icon);
    dl->AddText(ui_, uiSize, ImVec2(a.x + pad + iconW, y), fg, label);
    ImGui::PopID();
    return pressed;
}

void Shell::remember() {
    const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    building_.push_back({p.x, p.y, s.x, s.y});
}

void Shell::openPanel(const std::string& name) {
    if (name == "file") panel_ = Panel::File;
    if (name == "settings") panel_ = Panel::Settings;
    if (name == "view") panel_ = Panel::View;
    if (name == "brushes") panel_ = Panel::Brushes;
    if (name == "brush") panel_ = Panel::BrushSettings;
    if (name == "mask") panel_ = Panel::Mask;
}

void Shell::selectTool(ShellContext& c, int tool) {
    if (c.view.stroking() || tool < 0 || tool >= toolCount()) return;
    c.view.tool = tool;
    curveDrag_ = curveSelected_ = -1;
}

bool Shell::hits(float x, float y) const {
    for (const Rect& r : rects_)
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) return true;
    return false;
}

// --- The frame --------------------------------------------------------------------------------------------------

void Shell::draw(ShellContext& c) {
    building_.clear();
    sizePreview_ = false;
    topBar(c);
    leftBar(c);
    rightBar(c);
    brushBar(c);
    switch (panel_) {
    case Panel::File: filePanel(c); break;
    case Panel::Settings: settingsPanel(c); break;
    case Panel::View: viewPanel(c); break;
    case Panel::Brushes: brushesPanel(c); break;
    case Panel::BrushSettings: brushSettingsPanel(c); break;
    case Panel::Mask: maskPanel(c); break;
    case Panel::None: break;
    }
    if (showProbe) {
        c.probe.draw(c.info);
    }
    brushCircle(c);
    rects_.swap(building_);
}

void Shell::topBar(ShellContext& c) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kBarH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::Begin("##top", nullptr, kBarFlags);
    const float h = kBarH - 12.0f;
    fileButtonX_ = ImGui::GetCursorScreenPos().x;
    if (wideButton("file", ICON_LC_MENU, T("File"), 0.0f, h, panel_ == Panel::File)) togglePanel(Panel::File);

    // The middle: the model's size (and the timings when asked for), or the last error.
    {
        char text[160];
        SculptView& v = c.view;
        const bool notice = c.status.empty() && !c.notice.empty() && c.nowMs < c.noticeUntil;
        if (!c.status.empty()) {
            std::snprintf(text, sizeof text, "%s", c.status.c_str());
        } else if (notice) {
            std::snprintf(text, sizeof text, "%s", c.notice.c_str());
        } else if (showStats) {
            std::snprintf(text, sizeof text, T("%s triangles | %.0f fps | dab %.2f ms"), grouped(v.triangleCount()).c_str(),
                          c.info.fps, v.lastDabMs());
        } else {
            std::snprintf(text, sizeof text, T("%s triangles"), grouped(v.triangleCount()).c_str());
        }
        const float size = small_->FontSize / density_;
        const ImVec2 ts = small_->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const ImVec2 at(vp->WorkPos.x + (vp->WorkSize.x - ts.x) * 0.5f, vp->WorkPos.y + (kBarH - ts.y) * 0.5f);
        const ImU32 colour = !c.status.empty() ? IM_COL32(240, 140, 120, 255) : notice ? IM_COL32(150, 215, 160, 255) : kDim;
        ImGui::GetWindowDrawList()->AddText(small_, size, at, colour, text);
    }

    const float uiSize = ui_->FontSize / density_;
    const float settingsW = 24.0f + uiSize + 8.0f + ui_->CalcTextSizeA(uiSize, FLT_MAX, 0.0f, T("Settings")).x;
    ImGui::SameLine(vp->WorkSize.x - settingsW - 6.0f);
    settingsButtonX_ = ImGui::GetCursorScreenPos().x;
    if (wideButton("settings", ICON_LC_SETTINGS, T("Settings"), settingsW, h, panel_ == Panel::Settings))
        togglePanel(Panel::Settings);
    remember();
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Shell::leftBar(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Two tall sliders share what the buttons leave of the height.
    const float free = vp->WorkSize.y - kBarH - kBrushH - 4.0f * (kTile + kGap) - 120.0f;
    const float sliderH = std::clamp(free * 0.5f, 90.0f, 260.0f);
    const float sliderW = kStripW - 28.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + kGap, vp->WorkPos.y + kBarH + kGap));
    ImGui::SetNextWindowSize(ImVec2(kStripW, 0.0f));
    ImGui::Begin("##left", nullptr, kBarFlags | ImGuiWindowFlags_AlwaysAutoResize);
    const float inner = kStripW - ImGui::GetStyle().WindowPadding.x * 2.0f;

    auto caption = [&](const char* text) {
        ImGui::PushFont(small_);
        const float tw = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((inner - tw) * 0.5f, 0.0f));
        ImGui::TextColored(ImColor(kDim), "%s", text);
        ImGui::PopFont();
    };
    // A tall slider with its filled part shown (the thumb's travel from the bottom).
    auto tall = [&](const char* id, float* value, float lo, float hi, bool logScale, float fill) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - sliderW) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
        const bool changed = ImGui::VSliderFloat(id, ImVec2(sliderW, sliderH), value, lo, hi, "",
                                                 logScale ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None);
        ImGui::PopStyleColor(2);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float y = b.y - (b.y - a.y) * std::clamp(fill, 0.0f, 1.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float r = ImGui::GetStyle().FrameRounding;
        dl->AddRectFilled(ImVec2(a.x, y), b, ImGui::IsItemActive() ? IM_COL32(90, 160, 240, 255) : kAccent, r,
                          y - a.y < r ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(a.x + 6.0f, y - 2.0f), ImVec2(b.x - 6.0f, y + 2.0f), IM_COL32_WHITE, 2.0f);
        return changed;
    };

    // (Each tool keeps its own size and strength.)
    BrushPrefs& b = v.prefs();
    char value[32];
    caption(T("Size"));
    std::snprintf(value, sizeof value, "%.0f", b.radiusPx);
    caption(value);
    const float sizeFill = std::log(b.radiusPx / kRadiusMin) / std::log(kRadiusMax / kRadiusMin);
    tall("##size", &b.radiusPx, kRadiusMin, kRadiusMax, true, sizeFill);
    sizePreview_ = ImGui::IsItemActive();

    caption(T("Strength"));
    std::snprintf(value, sizeof value, "%.0f%%", b.strength * 100.0f);
    caption(value);
    tall("##strength", &b.strength, 0.0f, 1.0f, false, b.strength);
    b.clamp();

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (tile("undo", ICON_LC_UNDO_2, T("Undo"), inner, kTile, false, v.canUndo())) v.undo();
    if (tile("redo", ICON_LC_REDO_2, T("Redo"), inner, kTile, false, v.canRedo())) v.redo();
    if (tile("invert", ICON_LC_CIRCLE_MINUS, v.toolDef().mask ? T("Erase") : T("Invert"), inner, kTile, v.invert))
        v.invert = !v.invert;
    if (tile("brushsettings", ICON_LC_SLIDERS_HORIZONTAL, T("Brush"), inner, kTile, panel_ == Panel::BrushSettings))
        togglePanel(Panel::BrushSettings);
    remember();
    ImGui::End();
}

void Shell::rightBar(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - kStripW - kGap, vp->WorkPos.y + kBarH + kGap));
    ImGui::SetNextWindowSize(ImVec2(kStripW, 0.0f));
    ImGui::Begin("##right", nullptr, kBarFlags | ImGuiWindowFlags_AlwaysAutoResize);
    const float inner = kStripW - ImGui::GetStyle().WindowPadding.x * 2.0f;
    if (tile("mirror", ICON_LC_FLIP_HORIZONTAL_2, T("Mirror"), inner, kTile, v.symmetryX)) v.symmetryX = !v.symmetryX;
    if (tile("wire", ICON_LC_GRID_3X3, T("Wireframe"), inner, kTile, v.settings.wireframe))
        v.settings.wireframe = !v.settings.wireframe;
    if (tile("mask", ICON_LC_CONTRAST, T("Mask"), inner, kTile, panel_ == Panel::Mask || v.toolDef().mask)) togglePanel(Panel::Mask);
    if (tile("view", ICON_LC_ROTATE_3D, T("View"), inner, kTile, panel_ == Panel::View)) togglePanel(Panel::View);
    if (tile("frame", ICON_LC_FOCUS, T("Reset view"), inner, kTile)) v.resetView();
    remember();
    ImGui::End();
}

void Shell::brushBar(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = 4.0f;
    // The favourites, then the current tool when it isn't one of them, then the Brushes panel.
    std::vector<int> shown = v.brushes.favourites;
    const bool extra = std::find(shown.begin(), shown.end(), v.tool) == shown.end();
    if (extra) shown.push_back(v.tool);
    const int count = int(shown.size()) + 1;
    const float width = count * kBrushW + (count - 1) * spacing + (extra ? 20.0f : 10.0f) + style.WindowPadding.x * 2.0f;
    const float height = kBrushH + style.WindowPadding.y * 2.0f;
    const float maxW = vp->WorkSize.x - 2.0f * kGap;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + std::max((vp->WorkSize.x - width) * 0.5f, kGap),
                                   vp->WorkPos.y + vp->WorkSize.y - height - kGap));
    ImGui::SetNextWindowSize(ImVec2(std::min(width, maxW), height + (width > maxW ? style.ScrollbarSize : 0.0f)));
    ImGui::Begin("##brushes", nullptr, kBarFlags | ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    for (size_t i = 0; i < shown.size(); ++i) {
        const ToolDef& d = toolDefs()[size_t(shown[i])];
        if (i) ImGui::SameLine(0.0f, extra && i + 1 == shown.size() ? spacing + 10.0f : spacing);
        if (tile(d.key, d.icon, T(d.name), kBrushW, kBrushH, v.tool == shown[i])) {
            // Tapping the current tool again opens its settings (as in Nomad).
            if (v.tool == shown[i]) togglePanel(Panel::BrushSettings);
            else selectTool(c, shown[i]);
        }
    }
    ImGui::SameLine(0.0f, spacing + 10.0f);
    if (tile("all", ICON_LC_LAYOUT_GRID, T("Brushes"), kBrushW, kBrushH, panel_ == Panel::Brushes)) togglePanel(Panel::Brushes);
    ImGui::PopStyleVar();
    remember();
    ImGui::End();
}

// --- Panels -----------------------------------------------------------------------------------------------------

void Shell::filePanel(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(fileButtonX_, vp->WorkPos.y + kBarH + 4.0f));
    ImGui::Begin("##filepanel", nullptr, kPanelFlags);
    const float w = 300.0f, h = 48.0f, half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const auto dimText = [&](const char* text, ImU32 colour = kDim) {
        ImGui::PushFont(small_);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextColored(ImColor(colour), "%s", text);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    };

    if (confirm_ != Replace::None) {
        // Replacing the model loses the work in it (the autosave keeps only the newest model).
        ImGui::PushFont(icon_);
        ImGui::TextColored(ImColor(IM_COL32(240, 190, 90, 255)), "%s", ICON_LC_TRIANGLE_ALERT);
        ImGui::PopFont();
        dimText(T("This replaces the current work. It can't be brought back unless you exported it."), kText);
        ImGui::Spacing();
        if (wideButton("cancel", nullptr, T("Cancel"), half, h)) confirm_ = Replace::None;
        ImGui::SameLine();
        if (wideButton("replace", nullptr, T("Replace"), half, h, true)) {
            const Replace what = confirm_;
            confirm_ = Replace::None;
            replaceModel(c, what);
        }
        remember();
        ImGui::End();
        return;
    }

    const auto ask = [&](Replace what) {
        if (v.replacingLosesWork()) confirm_ = what;
        else replaceModel(c, what);
    };
    if (wideButton("sphere", ICON_LC_CIRCLE, T("New sphere"), w, h)) ask(Replace::Sphere);
    if (wideButton("lion", ICON_LC_SPARKLES, T("Lion head (sample)"), w, h)) ask(Replace::Lion);
    if (wideButton("import", ICON_LC_FOLDER_OPEN, T("Open a file..."), w, h)) ask(Replace::Import);
    if (wideButton("subdiv", ICON_LC_GRID_3X3, T("Subdivide (4x the triangles)"), w, h)) {
        c.status.clear();
        if (!v.subdivide(c.status)) SDL_Log("subdivide: %s", c.status.c_str());
        closePanel();
    }

    ImGui::Separator();
    ImGui::TextColored(ImColor(kDim), "%s", T("Export"));
    if (wideButton("sfproj", ICON_LC_FILE_BOX, T("Project"), half, h)) exportAs(c, ".sfproj");
    ImGui::SameLine();
    if (wideButton("glb", ICON_LC_BOX, "GLB", half, h)) exportAs(c, ".glb");
    if (wideButton("obj", ICON_LC_BOX, "OBJ", half, h)) exportAs(c, ".obj");
    ImGui::SameLine();
    if (wideButton("stl", ICON_LC_PRINTER, T("STL (print)"), half, h)) exportAs(c, ".stl");

    ImGui::Separator();
    ImGui::PushFont(small_);
    ImGui::TextColored(ImColor(kDim), T("%s vertices, %s triangles"), grouped(v.vertexCount()).c_str(),
                       grouped(v.triangleCount()).c_str());
    ImGui::PopFont();
    {
        const std::string state = files::autosaveStatus();
        std::string line;
        if (!files::autosaveAvailable()) line = T("Autosave is off: this page needs https (or localhost).");
        else if (state == "saving") line = T("Autosaving...");
        else if (state == "error") line = T("Autosave failed (see the top bar).");
        else if (!state.empty()) line = i18n::format(T("Autosaved at %s"), state.c_str());
        else line = T("Your work is saved automatically.");
        dimText(line.c_str());
    }
#ifdef __EMSCRIPTEN__
    dimText(T("Your work is kept in this browser. Clearing the site's data deletes it: export important work."),
            IM_COL32(240, 190, 90, 255));
#else
    dimText(i18n::format(T("Kept in %s"), files::autosaveLocation().c_str()).c_str());
#endif
    remember();
    ImGui::End();
}

void Shell::replaceModel(ShellContext& c, Replace what) {
    SculptView& v = c.view;
    c.status.clear();
    switch (what) {
    case Replace::Sphere: v.newSphere(); break;
    case Replace::Lion:
        if (!v.loadLionHead(c.status)) SDL_Log("lion head: %s", c.status.c_str());
        break;
    case Replace::Import:
        files::pickFile(T("Open a model or project file"), T("Choose a file"), T("Cancel"));
        break;
    case Replace::None: break;
    }
    closePanel();
}

void Shell::exportAs(ShellContext& c, const char* extension) {
    const std::string name = c.view.fileStem() + extension;
    const std::string path = files::exportPath(name);
    std::string error;
    c.status.clear();
    if (!c.view.exportFile(path, error)) {
        c.status = i18n::format(T("Could not export: %s"), error.c_str());
        SDL_Log("export %s: %s", path.c_str(), error.c_str());
        return;
    }
    const std::string where = files::deliverExport(path, name, T("Save"), T("Cancel"));
    c.notify(where.empty() ? i18n::format(T("Exported %s"), name.c_str()) : i18n::format(T("Saved to %s"), where.c_str()));
}

void Shell::settingsPanel(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = 360.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - kGap, vp->WorkPos.y + kBarH + 4.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, vp->WorkSize.y - kBarH - 2.0f * kGap));
    ImGui::Begin("##settingspanel", nullptr, kPanelFlags);

    if (files::fullscreenAvailable()) {
        const bool full = files::isFullscreen();
        if (wideButton("fullscreen", full ? ICON_LC_MINIMIZE : ICON_LC_MAXIMIZE, full ? T("Leave full screen") : T("Full screen"),
                       w - 16.0f, 46.0f))
        {
            files::toggleFullscreen(T("Full screen"), T("Full screen"), T("Cancel"));
            closePanel();
        }
        ImGui::Separator();
    } else if (!files::runningAsApp()) {
        // (iPad Chrome, for one, doesn't let a page go full screen.)
        ImGui::PushFont(small_);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 20.0f);
        ImGui::TextColored(ImColor(kDim), "%s",
                           T("This browser can't show the page full screen: open it in Safari, or add it to the Home Screen."));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::Separator();
    }
    ImGui::TextColored(ImColor(kDim), "%s", T("Language"));
    const bool zh = i18n::chinese();
    // (Each language's name in itself.)
    if (wideButton("zh", nullptr, "中文", (w - 24.0f) * 0.5f, 44.0f, zh)) i18n::setChinese(true);
    ImGui::SameLine();
    if (wideButton("en", nullptr, "English", (w - 24.0f) * 0.5f, 44.0f, !zh)) i18n::setChinese(false);

    ImGui::Separator();
    ImGui::Checkbox(T("Fingers sculpt too"), &c.input.fingersSculpt);
    ImGui::PushFont(small_);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 20.0f);
    ImGui::TextColored(ImColor(kDim), "%s",
                       T("Turns off by itself once a pen is used: the pen sculpts, fingers turn, slide and zoom the view."));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    bool matcap = v.settings.matcap >= 0;
    if (ImGui::Checkbox(T("Clay shading (MatCap)"), &matcap)) v.settings.matcap = matcap ? sf::defaultMatcap() : -1;
    ImGui::Checkbox(T("Show performance"), &showStats);

    ImGui::Separator();
    ImGui::PushFont(small_);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 20.0f);
    ImGui::TextColored(ImColor(kDim), "%s",
                       T("Gestures: one finger off the model turns the view; two fingers slide and pinch to zoom; "
                         "tap with two fingers to undo, three to redo."));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();

    // About: the GPL asks a public web app to point at its source.
    ImGui::Separator();
    ImGui::TextColored(ImColor(kDim), "%s", T("About"));
    ImGui::Text("SculptureABC %s", SCULPTURE_VERSION);
    ImGui::PushFont(small_);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 20.0f);
    ImGui::TextColored(ImColor(kDim), "%s",
                       T("Free software (GPL-3.0-or-later), built on SculptForge. You may use, share and change it; "
                         "the source code is public."));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    if (ImGui::Button(T("Source code (GitHub)"))) SDL_OpenURL(kSourceUrl);

    if (ImGui::CollapsingHeader(T("Developer"))) {
        ImGui::Checkbox(T("Input probe panel"), &showProbe);
        ImGui::BeginDisabled(v.benchmarking());
        if (ImGui::Button(v.benchmarking() ? T("Running...") : T("Speed test (1M / 3M / 5M triangles)"))) v.startBenchmark();
        ImGui::EndDisabled();
        ImGui::PushFont(small_);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 20.0f);
        for (const std::string& line : v.benchmarkLines()) ImGui::TextUnformatted(line.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (ImGui::Button(c.diagIn >= 0 ? T("Running...") : T("Diagnostics (current model)")) && c.diagIn < 0) c.diagIn = 2;
        ImGui::PushFont(small_);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 20.0f);
        for (const std::string& line : v.diagnosticLines()) ImGui::TextUnformatted(line.c_str());
        ImGui::Text("%s | %s", c.info.glVersion.c_str(), c.info.glRenderer.c_str());
        ImGui::Text("%dx%d (x%.2f) | %.0f fps | draw %.1f ms | upload %.1f ms", c.info.windowW, c.info.windowH,
                    c.info.pixelDensity, c.info.fps, v.lastDrawMs(), v.lastUploadMs());
        ImGui::Text("threads %u", std::thread::hardware_concurrency());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    remember();
    ImGui::End();
}

void Shell::viewPanel(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - kStripW - 2.0f * kGap, vp->WorkPos.y + kBarH + kGap),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::Begin("##viewpanel", nullptr, kPanelFlags);
    struct Side {
        const char* label;
        int which;
    };
    static const Side sides[] = {{"Front", 0}, {"Back", 1}, {"Left", 2}, {"Right", 3}, {"Top", 4}, {"Bottom", 5}};
    for (int i = 0; i < 6; ++i) {
        if (i % 2) ImGui::SameLine();
        if (wideButton(sides[i].label, nullptr, T(sides[i].label), 110.0f, 46.0f)) v.setView(sides[i].which);
    }
    if (wideButton("reset", ICON_LC_FOCUS, T("Reset view"), 228.0f, 46.0f)) v.resetView();
    remember();
    ImGui::End();
}

void Shell::brushesPanel(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(640.0f, vp->WorkSize.x - 2.0f * kStripW - 4.0f * kGap);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - kBrushH - 4.0f * kGap),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, vp->WorkSize.y - kBarH - kBrushH - 6.0f * kGap));
    ImGui::Begin("##brushespanel", nullptr, kPanelFlags);
    // Every tool by group, as a grid of tiles (Nomad's tool list); what the current one does underneath.
    const float inner = w - ImGui::GetStyle().WindowPadding.x * 2.0f, spacing = 4.0f;
    const int perRow = std::max(1, int((inner + spacing) / (kBrushW + spacing)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    for (int g = 0; g < int(ToolGroup::Count); ++g) {
        ImGui::PushFont(small_);
        ImGui::TextColored(ImColor(kDim), "%s", T(toolGroupName(ToolGroup(g))));
        ImGui::PopFont();
        int n = 0;
        for (int t = 0; t < toolCount(); ++t) {
            const ToolDef& d = toolDefs()[size_t(t)];
            if (int(d.group) != g) continue;
            if (n % perRow) ImGui::SameLine();
            ++n;
            if (tile(d.key, d.icon, T(d.name), kBrushW, kBrushH, v.tool == t)) {
                selectTool(c, t);
                closePanel();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
    }
    ImGui::PopStyleVar();
    ImGui::Separator();
    ImGui::TextUnformatted(T(v.toolDef().name));
    ImGui::PushFont(small_);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    ImGui::TextColored(ImColor(kDim), "%s", T(v.toolDef().what));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    touchScroll();
    remember();
    ImGui::End();
}

void Shell::touchScroll() {
    // Fingers drag a tall panel's empty space to scroll it (ImGui only scrolls by wheel or scrollbar).
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID active = ImGui::GetActiveID();
    if ((active == 0 || active == window->MoveId) && ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) &&
        ImGui::GetIO().MouseSource != ImGuiMouseSource_Mouse)
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
}

void Shell::curveEditor(sf::PressurePoints& curve, float w, float h) {
    ImGui::InvisibleButton("##curve", ImVec2(w, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const auto screen = [&](glm::vec2 p) { return ImVec2(a.x + p.x * (b.x - a.x), b.y - p.y * (b.y - a.y)); };
    const auto graph = [&](ImVec2 s) {
        return glm::vec2(std::clamp((s.x - a.x) / (b.x - a.x), 0.0f, 1.0f), std::clamp((b.y - s.y) / (b.y - a.y), 0.0f, 1.0f));
    };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated()) {
        // Take the nearest point within reach of a fingertip, or add one on the curve where the graph was touched.
        curveDrag_ = -1;
        float best = 26.0f;
        for (int i = 0; i < curve.count; ++i) {
            const ImVec2 s = screen(curve.points[size_t(i)]);
            const float d = std::hypot(s.x - mouse.x, s.y - mouse.y);
            if (d < best) best = d, curveDrag_ = i;
        }
        if (curveDrag_ < 0) {
            const float x = graph(mouse).x;
            if (curve.addPoint(x)) {
                for (int i = 0; i < curve.count; ++i)
                    if (std::abs(curve.points[size_t(i)].x - x) < 1e-4f) curveDrag_ = i;
            }
        }
        curveSelected_ = curveDrag_;
    }
    if (ImGui::IsItemActive() && curveDrag_ >= 0 && curveDrag_ < curve.count) {
        glm::vec2 p = graph(mouse);
        const int i = curveDrag_;
        if (i == 0) p.x = 0.0f;
        else if (i == curve.count - 1) p.x = 1.0f;
        else p.x = std::clamp(p.x, curve.points[size_t(i - 1)].x + 0.02f, curve.points[size_t(i + 1)].x - 0.02f);
        curve.points[size_t(i)] = p;
    }
    if (!ImGui::IsItemActive()) curveDrag_ = -1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(32, 34, 40, 255), 6.0f);
    for (int k = 1; k < 4; ++k) {
        const float t = float(k) / 4.0f;
        dl->AddLine(ImVec2(a.x + t * (b.x - a.x), a.y), ImVec2(a.x + t * (b.x - a.x), b.y), IM_COL32(255, 255, 255, 18));
        dl->AddLine(ImVec2(a.x, a.y + t * (b.y - a.y)), ImVec2(b.x, a.y + t * (b.y - a.y)), IM_COL32(255, 255, 255, 18));
    }
    dl->AddLine(screen({0, 0}), screen({1, 1}), IM_COL32(255, 255, 255, 40)); // (linear, for reference)
    ImVec2 line[49];
    for (int k = 0; k <= 48; ++k) {
        const float x = float(k) / 48.0f;
        line[k] = screen({x, curve.apply(x)});
    }
    dl->AddPolyline(line, 49, kAccent, ImDrawFlags_None, 3.0f);
    for (int i = 0; i < curve.count; ++i) {
        const ImVec2 s = screen(curve.points[size_t(i)]);
        dl->AddCircleFilled(s, i == curveSelected_ ? 9.0f : 7.0f, i == curveSelected_ ? IM_COL32_WHITE : IM_COL32(200, 210, 225, 255));
    }
    ImGui::PushFont(small_);
    const float size = small_->FontSize / density_;
    dl->AddText(small_, size, ImVec2(a.x + 6.0f, b.y - size - 4.0f), kDim, T("light"));
    const char* hard = T("heavy");
    dl->AddText(small_, size, ImVec2(b.x - 6.0f - small_->CalcTextSizeA(size, FLT_MAX, 0.0f, hard).x, b.y - size - 4.0f), kDim, hard);
    ImGui::PopFont();
}

void Shell::brushSettingsPanel(ShellContext& c) {
    SculptView& v = c.view;
    const ToolDef& d = v.toolDef();
    BrushPrefs& b = v.prefs();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = 360.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + kStripW + 2.0f * kGap, vp->WorkPos.y + kBarH + kGap));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, vp->WorkSize.y - kBarH - kBrushH - 5.0f * kGap));
    ImGui::Begin("##brushsettings", nullptr, kPanelFlags);
    const float inner = w - ImGui::GetStyle().WindowPadding.x * 2.0f - ImGui::GetStyle().ScrollbarSize;
    const float labelW = 92.0f;
    const auto dimText = [&](const char* text) {
        ImGui::PushFont(small_);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        ImGui::TextColored(ImColor(kDim), "%s", text);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    };
    const auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inner - labelW);
    };

    // The tool, what it does, and a reset.
    ImGui::PushFont(icon_);
    ImGui::TextUnformatted(d.icon);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T(d.name));
    const float resetW = 24.0f + ui_->CalcTextSizeA(ui_->FontSize / density_, FLT_MAX, 0.0f, T("Reset")).x + ui_->FontSize / density_ + 8.0f;
    ImGui::SameLine(inner - resetW + ImGui::GetStyle().WindowPadding.x);
    if (wideButton("reset", ICON_LC_ROTATE_CCW, T("Reset"), resetW, 40.0f)) {
        v.brushes.reset(v.tool);
        curveSelected_ = -1;
    }
    dimText(T(d.what));
    ImGui::Separator();

    const bool shapeTool = d.drive == Drive::Lasso || d.drive == Drive::Rect;
    if (shapeTool) {
        // Mask Lasso / Rectangle: how much mask, and which side.
        float strength = b.strength * 100.0f;
        label(T("Strength"));
        if (ImGui::SliderFloat("##maskstrength", &strength, 5.0f, 100.0f, "%.0f%%")) b.strength = strength / 100.0f;
        ImGui::Checkbox(T("Front side only"), &b.frontFacesOnly);
        dimText(T("Off: the shape masks through to the back of the model too."));
    } else {
    // The tip: its edge and how close the dabs are.
    float edge = b.focal;
    label(T("Edge"));
    if (ImGui::SliderFloat("##edge", &edge, -1.0f, 1.0f, edge < -0.05f ? T("hard") : edge > 0.05f ? T("soft") : T("normal")))
        b.focal = std::abs(edge) < 0.04f ? 0.0f : edge;
    if (d.drive == Drive::Dabs || d.drive == Drive::Pull) {
        float spacing = b.spacing * 100.0f;
        label(T("Spacing"));
        if (ImGui::SliderFloat("##spacing", &spacing, 5.0f, 100.0f, "%.0f%%")) b.spacing = spacing / 100.0f;
    }

    // Pen pressure and its curve.
    ImGui::Separator();
    ImGui::TextColored(ImColor(kDim), "%s", T("Pen pressure"));
    ImGui::Checkbox(T("Changes strength"), &b.pressureStrength);
    ImGui::SameLine(inner * 0.5f + 8.0f);
    ImGui::Checkbox(T("Changes size"), &b.pressureSize);
    {
        const auto& presets = sf::PressurePoints::presets();
        const char* current = b.curve.presetName();
        const float bw = (inner - 3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
        for (size_t i = 0; i < presets.size(); ++i) {
            if (i) ImGui::SameLine();
            const bool on = current && std::string(current) == presets[i].name;
            if (wideButton(presets[i].name, nullptr, T(presets[i].name), bw, 40.0f, on)) {
                b.curve = presets[i].make();
                curveSelected_ = -1;
            }
        }
    }
    curveEditor(b.curve, inner, 150.0f);
    {
        const bool inside = curveSelected_ > 0 && curveSelected_ < b.curve.count - 1;
        dimText(T("Drag a point; tap the graph to add one."));
        if (wideButton("removepoint", nullptr, T("Remove the selected point"), inner, 40.0f, false, inside)) {
            b.curve.removePoint(curveSelected_);
            curveSelected_ = -1;
        }
    }

    // The stroke.
    ImGui::Separator();
    if (d.drive != Drive::Turn) {
        ImGui::Checkbox(T("Steady stroke"), &b.steady);
        dimText(T("The brush trails the pen on a string: smooth, even lines."));
        if (b.steady) {
            label(T("String"));
            ImGui::SliderFloat("##steady", &b.steadyPx, 5.0f, 150.0f, "%.0f");
        }
    }
    if (d.type == sf::BrushType::Smooth && !d.mask) ImGui::Checkbox(T("Keep the volume"), &b.keepVolume);
    ImGui::Checkbox(T("Front side only"), &b.frontFacesOnly);
    ImGui::Checkbox(T("Connected surface only"), &b.connectedOnly);
    } // (not a shape tool)

    // On the brush bar or not.
    ImGui::Separator();
    auto& fav = v.brushes.favourites;
    const auto it = std::find(fav.begin(), fav.end(), v.tool);
    bool onBar = it != fav.end();
    if (ImGui::Checkbox(T("Show on the brush bar"), &onBar)) {
        if (onBar) fav.push_back(v.tool);
        else if (fav.size() > 1) fav.erase(it);
    }
    b.clamp();
    touchScroll();
    remember();
    ImGui::End();
}

void Shell::brushCircle(ShellContext& c) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const BrushPrefs& b = c.view.prefs();
    float x, y, r = b.radiusPx;
    if (sizePreview_) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 mid(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
        dl->AddCircle(mid, r, IM_COL32(255, 255, 255, 200), 64, 2.0f);
        if (b.focal != 0.0f) dl->AddCircle(mid, r * sf::focalRingFraction(b.focal), IM_COL32(255, 255, 255, 90), 64, 1.0f);
        return;
    }
    const bool pointer = c.input.cursor(x, y, c.nowMs);
    const Drive drive = c.view.toolDef().drive;
    if (drive == Drive::Lasso || drive == Drive::Rect) {
        // The mask shape being drawn (closed), or a small cross where the pen is.
        const std::vector<glm::vec2> shape = c.view.maskShape();
        const ImU32 col = c.view.invert ? IM_COL32(150, 200, 255, 230) : IM_COL32(255, 210, 120, 230);
        if (shape.size() >= 2) {
            std::vector<ImVec2> pts;
            for (const glm::vec2& p : shape) pts.push_back(ImVec2(p.x, p.y));
            if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), int(pts.size()), IM_COL32(255, 210, 120, 25));
            dl->AddPolyline(pts.data(), int(pts.size()), col, ImDrawFlags_Closed, 2.0f);
        } else if (pointer && !hits(x, y)) {
            dl->AddLine(ImVec2(x - 8.0f, y), ImVec2(x + 8.0f, y), col, 1.5f);
            dl->AddLine(ImVec2(x, y - 8.0f), ImVec2(x, y + 8.0f), col, 1.5f);
        }
        return;
    }
    float bx, by, br;
    if (c.view.strokePoint(bx, by, br)) {
        // The brush where it is (behind the pen with Steady Stroke), as big as the pressure makes it.
        const ImU32 col = IM_COL32(120, 180, 255, 200);
        if (pointer && b.steady && std::hypot(x - bx, y - by) > 1.0f) {
            dl->AddLine(ImVec2(x, y), ImVec2(bx, by), IM_COL32(255, 255, 255, 120), 1.5f);
            dl->AddCircleFilled(ImVec2(x, y), 3.0f, IM_COL32(255, 255, 255, 160));
        }
        dl->AddCircle(ImVec2(bx, by), br, col, 64, 1.5f);
        if (b.focal != 0.0f) dl->AddCircle(ImVec2(bx, by), br * sf::focalRingFraction(b.focal), IM_COL32(120, 180, 255, 90), 64, 1.0f);
        return;
    }
    if (!pointer || hits(x, y)) return;
    const ImU32 col = c.view.toolDef().mask ? IM_COL32(255, 210, 120, 170) : IM_COL32(255, 255, 255, 150);
    dl->AddCircle(ImVec2(x, y), r, col, 64, 1.5f);
    if (b.focal != 0.0f) dl->AddCircle(ImVec2(x, y), r * sf::focalRingFraction(b.focal), IM_COL32(255, 255, 255, 60), 64, 1.0f);
    if (c.view.invert) dl->AddLine(ImVec2(x - 5.0f, y), ImVec2(x + 5.0f, y), col, 1.5f);
}

void Shell::maskPanel(ShellContext& c) {
    SculptView& v = c.view;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - kStripW - 2.0f * kGap, vp->WorkPos.y + kBarH + kGap),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::Begin("##maskpanel", nullptr, kPanelFlags);
    const float w = 300.0f, spacing = ImGui::GetStyle().ItemSpacing.x;
    const float third = (w - 2.0f * spacing) / 3.0f, half = (w - spacing) * 0.5f, h = 46.0f;

    // The mask tools.
    const char* keys[] = {"Mask", "MaskLasso", "MaskRect"};
    for (int i = 0; i < 3; ++i) {
        const int t = toolIndex(keys[i]);
        const ToolDef& d = toolDefs()[size_t(t)];
        if (i) ImGui::SameLine();
        if (tile(d.key, d.icon, T(d.name), third, kBrushH, v.tool == t)) {
            selectTool(c, t);
            closePanel();
        }
    }
    if (wideButton("erase", ICON_LC_ERASER, T("Erase (unmask)"), w, h, v.invert && v.toolDef().mask)) {
        if (!v.toolDef().mask) selectTool(c, toolIndex("Mask"));
        v.invert = !v.invert;
    }

    // Whole-model operations (each one undo step).
    ImGui::Separator();
    struct Op {
        const char* icon;
        const char* label;
        SculptView::MaskOp op;
    };
    static const Op ops[] = {
        {ICON_LC_FLIP_VERTICAL, "Invert", SculptView::MaskOp::Invert}, {ICON_LC_CIRCLE, "Clear", SculptView::MaskOp::Clear},
        {ICON_LC_SQUARE, "Mask all", SculptView::MaskOp::All},          {ICON_LC_DROPLET, "Blur", SculptView::MaskOp::Blur},
        {ICON_LC_SPARKLES, "Sharpen", SculptView::MaskOp::Sharpen},     {ICON_LC_MAXIMIZE_2, "Grow", SculptView::MaskOp::Grow},
        {ICON_LC_MINIMIZE_2, "Shrink", SculptView::MaskOp::Shrink},
    };
    for (size_t i = 0; i < std::size(ops); ++i) {
        if (i % 2) ImGui::SameLine();
        const bool needsMask = ops[i].op != SculptView::MaskOp::All;
        if (wideButton(ops[i].label, ops[i].icon, T(ops[i].label), half, h, false, !needsMask || v.hasMask())) v.maskOp(ops[i].op);
    }
    ImGui::PushFont(small_);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextColored(ImColor(kDim), "%s", T("Masked areas show dark; brushes leave them alone."));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    remember();
    ImGui::End();
}
