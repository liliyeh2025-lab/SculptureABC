# ComfyUI workflows for SculptForge

Ready-made [ComfyUI](https://github.com/comfyanonymous/ComfyUI) workflows that SculptForge's **Generate 3D** window can
run on your own ComfyUI server (image or text in, mesh out). They are saved in ComfyUI's **API format**, so you can also
queue them yourself with `POST /prompt`, or drag one onto the ComfyUI canvas to look at and change it.

| Workflow | Input | What it does | Needs | Runs on |
|---|---|---|---|---|
| [`sculptforge-inflate.json`](sculptforge-inflate.json) | image | Inflates a sketch or silhouette into a rounded, closed base mesh with SculptForge, voxel-remeshes it into an even, watertight sculpting surface (removing the crease where the front and back halves meet), then exports a GLB. | The [ComfyUI-SculptForge](../ComfyUI-SculptForge/) node pack and SculptForge installed. No models. | Any CPU, about a second |
| [`hunyuan3d-image-to-3d.json`](hunyuan3d-image-to-3d.json) | image | Hunyuan3D 2.1 image-to-3D with ComfyUI's built-in nodes: a detailed, untextured mesh saved as GLB. | `hunyuan_3d_v2.1.safetensors` (7.4 GB). | A GPU (Tencent quotes about 10 GB of VRAM for shape generation) |
| [`text-to-3d.json`](text-to-3d.json) | text | FLUX.1 [schnell] draws the object on a plain backdrop, BiRefNet cuts it out onto white, Hunyuan3D 2.1 turns it into a mesh. Also saves the concept image. | `flux1-schnell-fp8.safetensors` (17.2 GB), `birefnet.safetensors` (444 MB), `hunyuan_3d_v2.1.safetensors` (7.4 GB). | A GPU; ComfyUI moves each model in and out of VRAM as it is needed |

Apart from the SculptForge nodes in the first one, all three use only nodes that ship with ComfyUI. They were
checked against ComfyUI 0.42.0; older versions may lack *Remove Background* or *Trim Text*, so update ComfyUI if a
node is reported missing.

## Model files

Download each file into the folder shown, inside your ComfyUI install, then restart ComfyUI or press **R** in the
editor to refresh the lists.

| File | Folder | Used by | Download |
|---|---|---|---|
| `hunyuan_3d_v2.1.safetensors` | `models/checkpoints/` | hunyuan3d-image-to-3d, text-to-3d | [Comfy-Org/hunyuan3D_2.1_repackaged](https://huggingface.co/Comfy-Org/hunyuan3D_2.1_repackaged/resolve/main/hunyuan_3d_v2.1.safetensors) |
| `flux1-schnell-fp8.safetensors` | `models/checkpoints/` | text-to-3d | [Comfy-Org/flux1-schnell](https://huggingface.co/Comfy-Org/flux1-schnell/resolve/main/flux1-schnell-fp8.safetensors) |
| `birefnet.safetensors` | `models/background_removal/` | text-to-3d | [Comfy-Org/BiRefNet](https://huggingface.co/Comfy-Org/BiRefNet/resolve/main/background_removal/birefnet.safetensors) |

```
ComfyUI/
└── models/
    ├── checkpoints/
    │   ├── hunyuan_3d_v2.1.safetensors
    │   └── flux1-schnell-fp8.safetensors
    └── background_removal/
        └── birefnet.safetensors
```

The Hunyuan3D 2.1 checkpoint holds the shape model, its image encoder and its VAE, so it is the only file the
image-to-3D workflow needs. To use **Hunyuan3D 2.0** instead (smaller, 4.9 GB), download
[`hunyuan3d-dit-v2_fp16.safetensors`](https://huggingface.co/Comfy-Org/hunyuan3D_2.0_repackaged/resolve/main/split_files/hunyuan3d-dit-v2_fp16.safetensors)
into `models/checkpoints/`, pick it in the loader, and set the latent `resolution` to 3072, `steps` to 20 and `cfg`
to 8. FLUX.1 [schnell] and BiRefNet are released under licences that allow commercial use; check the
[Hunyuan3D 2.1 licence](https://github.com/Tencent-Hunyuan/Hunyuan3D-2.1/blob/main/LICENSE) for where it applies.

The image-to-3D workflow works best with one object, whole and centred, on a plain light background (a white sketch
page is ideal). For photos with busy backgrounds, copy the BiRefNet nodes (9 to 12) from `text-to-3d.json` in front
of the Hunyuan3D part.

## How SculptForge fills in a workflow

Any API-format workflow works with SculptForge if it follows these rules. The workflows here already do.

- **Image:** every `LoadImage` node titled `sf_image` gets the picture SculptForge uploads. If no node has that title,
  the first `LoadImage` node does.
- **Text:** the node titled `sf_prompt` gets the prompt in its `text`, `prompt` or `string` input, whichever it has.
  In `text-to-3d.json` that is a *Trim Text* node, so the prompt then gets the 3D-friendly framing ("a single
  object, centered, plain white background, ...") appended before it reaches FLUX.
- **Seed:** a node titled `sf_seed`, and every input named `seed` or `noise_seed`, may get a fresh random seed.
- **Result:** SculptForge downloads the first mesh file (`.glb`, `.gltf`, `.obj`, `.ply`, `.stl`, `.fbx`, `.usdz`) in
  the finished job's outputs. Other outputs, such as the concept image, are ignored.

To make your own: build the graph in ComfyUI, set node titles with *right click > Title*, end it with a mesh saver
(*Save 3D Model*, or *SculptForge Export*), then save it with *Workflow > Export (API)*.

## Checking a workflow

Two scripts in [`tools/`](../../../tools/) check workflows against a running ComfyUI. Both use only Python's standard
library (and curl).

```sh
# Every node exists on the server, every required input is set, every link points at a real output of the
# right type, values are in range, an output node exists, plus which nodes SculptForge will fill in.
tools/comfyui-validate-workflow.py integrations/comfyui/workflows/*.json
tools/comfyui-validate-workflow.py --server http://192.168.1.20:8188 --strict my-workflow.json

# Run one for real like SculptForge does: upload assets/demo/teddy-sketch.png, fill in the inputs, queue it,
# wait, download the mesh and check it.
tools/comfyui-smoke.sh integrations/comfyui/workflows/sculptforge-inflate.json
tools/comfyui-smoke.sh --prompt "a carved stone owl" --out /tmp/meshes integrations/comfyui/workflows/text-to-3d.json
```

Missing model files and an unloaded SculptForge pack are warnings in the validator (errors with `--strict`), since
they are about the server rather than the workflow.
