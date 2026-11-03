# Generate 3D with your own ComfyUI

If you already run [ComfyUI](https://github.com/comfyanonymous/ComfyUI), SculptForge can use it as its
image-to-3D and text-to-3D generator. ComfyUI can run on your own computer or on a GPU server at your
studio. It runs **any workflow you like**: Hunyuan3D, TRELLIS, a custom node pack, or your own graph.
SculptForge uploads the picture (or sends the description), queues the workflow, waits for it to finish,
downloads the mesh it saved and opens it ready to sculpt, remeshed exactly like the other services.

Nothing goes anywhere except your ComfyUI server. There's no account, no key and no cost per model.

## Setup

1. **Start ComfyUI.** By default it listens on `http://127.0.0.1:8188`. If SculptForge runs on a
   different computer, start ComfyUI with `--listen` (for example `python main.py --listen 0.0.0.0`)
   so other machines can reach it. Only do that on a network you trust: ComfyUI has no login.
2. **Install the models your workflow needs.** For the bundled Hunyuan3D 2.1 workflow, that's
   `hunyuan_3d_v2.1.safetensors` in `ComfyUI/models/checkpoints`. The workflows README
   (`integrations/comfyui/workflows/README.md`) lists every file, its size and its download link.
3. In SculptForge, open **Generate 3D** (File > Generate 3D..., or drop a picture onto the window) and
   pick **ComfyUI - your server**.
4. Enter the **Server** address and press **Test connection**. You should see the ComfyUI version and
   your GPU, for example `Connected: ComfyUI 0.42.0 - NVIDIA GeForce RTX 4090 (24.0 GB)`.
5. Pick a **Workflow**: one of the bundled ones, or **Other file (API format)...** for your own.
6. Press **Generate 3D model**. The progress bar shows your place in ComfyUI's queue and how long the
   workflow has been running. **Cancel** removes the job from the queue, or stops it if it's already
   running.

**Save settings** stores the server address and your chosen workflows in `ai.json`, so they're still
set next time you open SculptForge.

### Bundled workflows

SculptForge ships these in `integrations/comfyui/workflows/` (installed under
`share/sculptforge/comfyui/workflows`):

| Workflow | Tab | Needs |
|---|---|---|
| `hunyuan3d-image-to-3d` | From image | The Hunyuan3D 2.1 checkpoint (7.4 GB) and a GPU (Tencent quotes about 10 GB of VRAM) |
| `text-to-3d` | From text | FLUX.1 schnell, BiRefNet and Hunyuan3D 2.1 (about 25 GB of downloads) and a GPU |
| `sculptforge-inflate` | From image | The [ComfyUI-SculptForge](https://github.com/islandmed/ComfyUI-SculptForge) node pack (ComfyUI-Manager: *Install via Git URL*). No GPU: a quick inflated blockout, handy for checking your setup |

When you haven't chosen a workflow, SculptForge uses the first bundled one that fits the tab.

### From the command line, Python or an MCP agent

```sh
sculptforge-cli -c 'comfyui_check'                                     # is it up? version and GPU
sculptforge-cli -c 'image_to_3d path=hero.png service=comfyui workflow=my-hunyuan-api.json; export_glb path=hero.glb'
sculptforge-cli -c 'text_to_3d prompt="a carved stone owl" service=comfyui url=http://gpu-box:8188 seed=42'
```

`workflow=` and `url=` override your saved settings for that one run. `seed=` makes a run
repeatable; without it, every run gets a fresh seed.

## Workflows: API format and the sf_* contract

SculptForge runs workflows saved in ComfyUI's **API format**. That's not the file ComfyUI's ordinary
Save writes. To export one:

- **Current ComfyUI:** open the workflow, then use **Workflow > Export (API)**.
- **Older ComfyUI:** in the settings, turn on **Enable Dev mode Options**, then use **Save (API Format)**.

If you choose an ordinary editor save by mistake, SculptForge says so and tells you how to export it.

To tell SculptForge where things go, give nodes these **titles** (right-click a node > Title):

| Title | Node | What SculptForge does |
|---|---|---|
| `sf_image` | Load Image | Sets `image` to the uploaded picture. Every node with this title gets it. **Without one, the first Load Image node is used.** |
| `sf_prompt` | Any node with a `text`, `prompt` or `string` input (CLIP Text Encode, String, Primitive String...) | Sets that input to your description. **Without one, the first CLIP Text Encode that isn't titled "negative" is used.** |
| `sf_seed` | A Primitive Int or similar | Sets its value to the seed. Any input named `seed` or `noise_seed` gets it too. |

Titles are not case-sensitive. Inputs that are wired to another node are left alone, apart from those
on the titled nodes above.

**The result:** SculptForge imports the **first mesh file** (`.glb`, `.gltf`, `.obj`, `.ply`, `.stl`,
`.fbx`, `.usdz`) that the workflow saves. Core **Save GLB**, **Preview 3D** and the SculptForge
**Export** node all work, and so do most 3D node packs. End your workflow with one of them. If the
workflow finishes without saving a mesh, SculptForge lists what it did produce.

The picture is prepared the same way as for the hosted services: the subject is cropped out of a
transparent background, centred on a square and scaled to at most 1024 px. It's uploaded to ComfyUI's
`input` folder as `sculptforge_<random>.png`.

### Make your own workflow

1. Build and test the graph in ComfyUI, starting from a **Load Image** node (or a text node for text to
   3D) and ending with a node that saves the mesh.
2. Title the Load Image node `sf_image` (or the text node `sf_prompt`).
3. Export it with **Workflow > Export (API)** and choose it in SculptForge.

`tools/comfyui-validate-workflow.py --server http://127.0.0.1:8188 my-workflow.json` checks the workflow
against your server before you run it: missing nodes and models, bad links, and which nodes the contract
will use.

## Troubleshooting

| Message | What to do |
|---|---|
| *Nothing is answering at http://...* | ComfyUI isn't running at that address, or a firewall blocks it. Check the address and port (8188 by default). For another computer, start ComfyUI with `--listen`. |
| *...answered, but not like ComfyUI* | Something else is on that port, or a proxy is in the way. Use the address ComfyUI prints when it starts. |
| *This workflow was saved in ComfyUI's editor format* | Export it again with **Workflow > Export (API)**. |
| *This ComfyUI is missing the nodes ...* | The workflow uses custom nodes this server doesn't have. Install them (ComfyUI Manager > **Install Missing Custom Nodes**) and restart ComfyUI. |
| *...names a model file this server doesn't have* | Download the model into the folder named in the message (for example `models/checkpoints`). |
| *ComfyUI rejected the workflow: node N ...* | An input is out of range or not allowed. Open the workflow in ComfyUI: it shows the same error on that node. |
| *The workflow finished but saved no mesh file* | End the workflow with Save GLB (or another node that saves a mesh). The message lists what it did produce. |
| *... failed: CUDA out of memory* | The GPU ran out of memory. Try a lower resolution or octree size, or a smaller model. |
| *The job disappeared from ComfyUI's queue* | Someone cleared the queue, or ComfyUI restarted. Generate again. |

On a shared studio server, your job waits in the same queue as everyone else's, and the progress bar
shows your place. **Cancel** only removes or stops your own job.
