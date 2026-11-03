// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One-click export presets for game engines: sizes, units, naming and LOD
// packaging set the way each engine's importer expects.
#pragma once

#include "core/Document.h"

#include <string>

namespace sf {

struct UnrealExportOptions {
    std::string folder;         // where to write; the file name is chosen by the preset
    float heightCm = 0.0f;      // model height in Unreal (cm); 0 = 1 SculptForge unit = 1 m
    int lods = 4;               // LOD0..N-1 in one file (1 = no LOD group)
    float ratio = 0.5f;         // triangle ratio between LODs
    std::string assetName;      // empty = SM_/SK_ + document name
};

struct UnrealExportResult {
    std::string path;           // the written .fbx
    std::string assetName;      // SM_Name, or SK_Name when the sculpt has layers (morph targets)
    bool skeletal = false;      // Unreal needs a skeletal mesh for morph targets
    int lodCount = 1;
    float scale = 1.0f;         // SculptForge units -> cm
};

// Unreal: FBX in centimetres, SM_/SK_ prefix, LODs as an FBX LOD group (static
// meshes), sculpt layers as blend shapes (skeletal meshes; Unreal adds a root bone).
bool exportForUnreal(const Document& doc, const UnrealExportOptions& options, UnrealExportResult& result,
                     std::string& error);

} // namespace sf
