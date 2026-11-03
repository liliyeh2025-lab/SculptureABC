// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "InputProbe.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace {

const char* kKindNames[3] = {"Mouse", "Touch", "Pen"};
const char* kPhaseNames[5] = {"down", "move", "up", "cancel", "hover"};
const ImU32 kKindColors[3] = {IM_COL32(120, 200, 255, 220), IM_COL32(255, 200, 90, 220),
                              IM_COL32(255, 90, 140, 230)};

} // namespace

void InputProbe::onSample(const PointerSample& s) {
    int k = std::clamp(int(s.kind), 0, 2);
    KindStats& st = stats_[k];
    ++st.samples;
    if (s.coalesced) ++st.coalesced;
    if (s.phase == PointerPhase::Down) ++st.downs;
    if (s.phase == PointerPhase::Hover) ++st.hovers;
    st.recent.push_back(s.timeMs);
    while (!st.recent.empty() && st.recent.front() < s.timeMs - 1000.0) st.recent.pop_front();
    st.lastTime = s.timeMs;
    st.tiltX = s.tiltX;
    st.tiltY = s.tiltY;
    if (s.eraser) st.sawEraser = true;

    bool touching = s.phase == PointerPhase::Down || s.phase == PointerPhase::Move;
    if (touching) {
        st.lastPressure = s.pressure;
        st.minPressure = std::min(st.minPressure, s.pressure);
        st.maxPressure = std::max(st.maxPressure, s.pressure);
        if (s.kind == PointerKind::Pen) {
            pressureHistory_.push_back(s.pressure);
            if (pressureHistory_.size() > 400) pressureHistory_.erase(pressureHistory_.begin());
        }
        if (showDots_ && s.kind == PointerKind::Pen && !hitsPanel(s.x, s.y)) {
            dots_.push_back({s.x, s.y, s.pressure, k});
            if (dots_.size() > 6000) dots_.erase(dots_.begin(), dots_.begin() + 1000);
        }
    }

    auto it = std::find_if(active_.begin(), active_.end(), [&](auto& a) { return a.first == s.id * 4 + k; });
    if (s.phase == PointerPhase::Up || s.phase == PointerPhase::Cancel) {
        if (it != active_.end()) active_.erase(it);
    } else if (touching) {
        if (it == active_.end()) active_.push_back({s.id * 4 + k, s});
        else it->second = s;
    }

    if (s.phase != PointerPhase::Move && s.phase != PointerPhase::Hover) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s %s id=%lld (%.0f, %.0f) p=%.3f tilt=(%.0f, %.0f)%s", kKindNames[k],
                      kPhaseNames[int(s.phase)], (long long)s.id, s.x, s.y, s.pressure, s.tiltX, s.tiltY,
                      s.eraser ? " eraser" : "");
        lastEvent_ = buf;
    }
}

bool InputProbe::hitsPanel(float x, float y) const {
    return x >= panelX_ && y >= panelY_ && x <= panelX_ + panelW_ && y <= panelY_ + panelH_;
}

InputProbe::Actions InputProbe::draw(const ProbeInfo& info) {
    Actions actions;

    // Pen / mouse strokes: dot radius follows pressure.
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    for (const Dot& d : dots_) bg->AddCircleFilled(ImVec2(d.x, d.y), 1.0f + d.pressure * 10.0f, kKindColors[d.kind]);
    for (auto& [key, s] : active_) {
        int k = std::clamp(int(s.kind), 0, 2);
        bg->AddCircle(ImVec2(s.x, s.y), 28.0f + s.pressure * 30.0f, kKindColors[k], 0, 3.0f);
    }

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    ImGui::Begin("Input probe");
    ImGui::Text("SculptureABC %s   %s", SCULPTURE_VERSION, info.platform.c_str());
    if (info.cjkFont) ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "中文測試：雕刻、筆刷、遮罩、圖層");
    ImGui::Separator();
    ImGui::TextWrapped("GL: %s", info.glVersion.c_str());
    ImGui::TextWrapped("GPU: %s", info.glRenderer.c_str());
    ImGui::TextWrapped("GLSL: %s", info.glslVersion.c_str());
    ImGui::Text("Window %dx%d  pixels %dx%d  density %.2f  scale %.2f", info.windowW, info.windowH, info.pixelW,
                info.pixelH, info.pixelDensity, info.contentScale);
    ImGui::Text("%.1f fps   %d triangles   RAM %d MB", info.fps, info.triangles, info.systemRamMB);
    if (!info.extra.empty()) ImGui::TextWrapped("%s", info.extra.c_str());
    ImGui::Separator();
    ImGui::Text("Input source: %s", pointer::sourceName());
    if (ImGui::BeginTable("kinds", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("samples");
        ImGui::TableSetupColumn("Hz");
        ImGui::TableSetupColumn("coalesced");
        ImGui::TableSetupColumn("pressure");
        ImGui::TableHeadersRow();
        for (int k = 0; k < 3; ++k) {
            const KindStats& st = stats_[k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kKindColors[k]), "%s", kKindNames[k]);
            ImGui::TableNextColumn();
            ImGui::Text("%lld", st.samples);
            ImGui::TableNextColumn();
            ImGui::Text("%d", int(st.recent.size()));
            ImGui::TableNextColumn();
            ImGui::Text("%lld", st.coalesced);
            ImGui::TableNextColumn();
            if (st.maxPressure > 0) ImGui::Text("%.3f (%.2f-%.2f)", st.lastPressure, st.minPressure, st.maxPressure);
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    const KindStats& pen = stats_[int(PointerKind::Pen)];
    ImGui::Text("Pen tilt: x %.1f  y %.1f   hover samples %lld%s", pen.tiltX, pen.tiltY, pen.hovers,
                pen.sawEraser ? "   eraser seen" : "");
    if (!pressureHistory_.empty())
        ImGui::PlotLines("##pressure", pressureHistory_.data(), int(pressureHistory_.size()), 0, "pen pressure", 0.0f,
                         1.0f, ImVec2(-1, 70));
    ImGui::Text("Active pointers: %d", int(active_.size()));
    ImGui::TextWrapped("Last: %s", lastEvent_.empty() ? "-" : lastEvent_.c_str());
    ImGui::Separator();
    ImGui::Checkbox("Pen dots (size = pressure)", &showDots_);
    ImGui::SameLine();
    if (ImGui::Button("Clear dots")) {
        dots_.clear();
        pressureHistory_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset stats")) {
        for (auto& st : stats_) st = KindStats{};
        lastEvent_.clear();
    }
    ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    panelX_ = pos.x;
    panelY_ = pos.y;
    panelW_ = size.x;
    panelH_ = ImGui::IsWindowCollapsed() ? ImGui::GetFrameHeight() : size.y;
    ImGui::End();
    return actions;
}
