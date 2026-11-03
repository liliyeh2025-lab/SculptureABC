// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ComfyUI as an image-to-3D / text-to-3D generator: the artist's own ComfyUI
// (this computer, or a studio GPU box) runs any workflow they like - Hunyuan3D,
// TRELLIS, their own node graph - and SculptForge imports the mesh it writes.
//
// Workflows are ComfyUI's API format ("Export (API)"): {node_id: {class_type,
// inputs, _meta: {title}}}. The contract, by node title:
//   sf_image  - LoadImage node(s) that get the picture (fallback: the first LoadImage)
//   sf_prompt - node whose text / prompt / string input gets the description
//   sf_seed   - node whose seed gets a fresh value (as do inputs named seed / noise_seed)
// The result is the first mesh file (.glb .gltf .obj .ply .stl .fbx .usdz) in
// the prompt's /history outputs, downloaded through /view.
#pragma once

#include "ai/Generator3D.h"
#include "core/Json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sf::ai {

struct AiSettings;
struct GenerationProgress;

namespace comfy {

constexpr const char* kDefaultUrl = "http://127.0.0.1:8188";

// "localhost:8188/" -> "http://localhost:8188"
std::string normalizeUrl(const std::string& url);

// What /system_stats reports, for "Test connection".
struct ServerInfo {
    std::string comfyVersion; // empty on old servers
    std::string pythonVersion;
    std::string pytorchVersion;
    std::vector<std::string> devices; // "NVIDIA GeForce RTX 4090 (24.0 GB)", "cpu"
    std::string summary() const;      // "ComfyUI 0.3.40 - NVIDIA GeForce RTX 4090 (24.0 GB)"
};
bool parseSystemStats(const Json& stats, ServerInfo& info);
// Blocking. On failure `error` says what to do (start ComfyUI, --listen...).
bool checkServer(const std::string& url, ServerInfo& info, std::string& error, int timeoutSeconds = 5);

// --- Workflows -------------------------------------------------------------

// True for the editor's own save format ({nodes: [...], links: [...]}), which cannot be queued.
bool isUiFormat(const Json& workflow);
// Accepts a bare API workflow or one wrapped as {"prompt": {...}}; explains UI-format files.
bool parseWorkflow(const std::string& text, Json& workflow, std::string& error);
bool loadWorkflow(const std::string& path, Json& workflow, std::string& error);

struct ContractInputs {
    std::string image;              // uploaded image name ("sub/name.png"); empty = no image
    std::string prompt;             // description; empty = leave prompts alone
    std::optional<uint64_t> seed;   // set on sf_seed and seed / noise_seed inputs
};
// Which node ids got which input (for status lines and tests).
struct ContractReport {
    std::vector<std::string> imageNodes, promptNodes, seedNodes;
};
// Edits `workflow` in place. Fails when the image or prompt has nowhere to go.
bool applyContract(Json& workflow, const ContractInputs& in, std::string& error, ContractReport* report = nullptr);
// Whether a workflow takes a picture or a description ("image", "text" or "" if neither).
std::string workflowKind(const Json& workflow);

// --- Results ---------------------------------------------------------------

struct OutputFile {
    std::string nodeId, key; // where it was found in outputs ("9", "3d")
    std::string filename, subfolder, type = "output";
    bool isMesh() const;
};
struct HistoryEntry {
    bool found = false;     // the prompt is in /history (finished one way or another)
    bool completed = false;
    bool failed = false;
    std::string error;      // execution_error / interrupted, as the artist should read it
    std::vector<OutputFile> files;
};
// `history` is the /history/<id> reply.
HistoryEntry parseHistory(const Json& history, const std::string& promptId);
const OutputFile* firstMesh(const std::vector<OutputFile>& files);
// "node 9 'images': preview_00001_.png; node 12 'text'" - what a workflow produced, for errors.
std::string describeOutputs(const Json& history, const std::string& promptId);
std::string viewUrl(const std::string& baseUrl, const OutputFile& file);

// A rejected /prompt (HTTP 400) as a readable message: missing custom nodes,
// per-node input errors (node_errors), "no outputs"... `workflow` names the nodes.
std::string promptError(const std::string& body, const Json& workflow);
// Node types the workflow uses that /object_info does not list.
std::vector<std::string> missingNodeTypes(const Json& workflow, const Json& objectInfo);

// Queue position from /queue: 0 = running now, n = n prompts ahead, -1 = not queued.
int queuePosition(const Json& queue, const std::string& promptId);

// --- Running ----------------------------------------------------------------

// Blocking; run on a worker thread. Uses settings.comfyUrl and `workflowPath`
// (empty = settings.comfyImageWorkflow / comfyTextWorkflow). `imagePath` or
// `prompt` (text to 3D) drives the workflow; writes `<outputStem><ext>`.
// Cancelling removes the prompt from the queue, or interrupts it if running.
GeneratorResult run(const AiSettings& settings, const std::string& workflowPath, const std::string& imagePath,
                    const std::string& prompt, const std::string& outputStem, GenerationProgress* progress,
                    std::optional<uint64_t> seed = std::nullopt);

// Workflows shipped with SculptForge (integrations/comfyui/workflows), found
// next to the executable (../share/sculptforge/comfyui, ../Resources/comfyui,
// ./comfyui) or in the source tree it was built from.
struct BundledWorkflow {
    std::string name; // file name without .json
    std::string path;
    std::string kind; // "image" or "text"
};
std::vector<BundledWorkflow> bundledWorkflows();
// The bundled workflow of `kind` to use when none is chosen (names containing
// one of `preferred` win, in that order); empty if none is installed.
std::string defaultWorkflow(const std::string& kind, const std::vector<std::string>& preferred = {});

} // namespace comfy
} // namespace sf::ai
