#!/usr/bin/env bash
# Runs a ComfyUI workflow end to end against an already running ComfyUI server, the way SculptForge does:
# upload an image, fill in sf_image / sf_prompt / seeds, queue it, wait for it, download the first mesh.
# Starts nothing. Needs curl and python3.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/comfyui-smoke.sh integrations/comfyui/workflows/sculptforge-inflate.json
#   tools/comfyui-smoke.sh --server http://127.0.0.1:8191 --out /tmp/meshes WORKFLOW.json
#   tools/comfyui-smoke.sh --prompt "a carved stone owl" integrations/comfyui/workflows/text-to-3d.json
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SERVER=${COMFYUI_URL:-http://127.0.0.1:8188}
IMAGE=$ROOT/assets/demo/teddy-sketch.png
PROMPT="a friendly cartoon teddy bear sitting"
OUT=.
TIMEOUT=600

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] WORKFLOW.json

Options:
  --server URL    ComfyUI server (default \$COMFYUI_URL or http://127.0.0.1:8188)
  --image PNG     image for the sf_image node (default assets/demo/teddy-sketch.png)
  --prompt TEXT   text for the sf_prompt node (default "$PROMPT")
  --out DIR       where to save the mesh (default: current directory)
  --timeout SEC   give up after this many seconds (default $TIMEOUT)
EOF
}

WORKFLOW=
while [ $# -gt 0 ]; do
  case $1 in
    --server) SERVER=$2; shift 2 ;;
    --image) IMAGE=$2; shift 2 ;;
    --prompt) PROMPT=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --timeout) TIMEOUT=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) WORKFLOW=$1; shift ;;
  esac
done
[ -n "$WORKFLOW" ] || { usage >&2; exit 2; }
[ -f "$WORKFLOW" ] || { echo "error: no such workflow: $WORKFLOW" >&2; exit 2; }
[ -f "$IMAGE" ] || { echo "error: no such image: $IMAGE" >&2; exit 2; }
SERVER=${SERVER%/}

step() { printf '\n== %s\n' "$*"; }
fail() { printf 'FAILED: %s\n' "$*" >&2; exit 1; }
# json EXPR: evaluates a Python expression over the JSON document on stdin (bound to "d").
json() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

step "Server $SERVER"
curl -sf --max-time 10 "$SERVER/system_stats" -o "$TMP/stats.json" || fail "no ComfyUI answering at $SERVER/system_stats"
json '"ComfyUI %s, Python %s, devices: %s" % (d["system"].get("comfyui_version", "?"), d["system"].get("python_version", "?").split()[0], ", ".join(x["name"] for x in d.get("devices", [])))' < "$TMP/stats.json"

step "Upload $(basename "$IMAGE")"
curl -sf --max-time 60 -F "image=@$IMAGE" -F overwrite=true "$SERVER/upload/image" -o "$TMP/upload.json" \
  || fail "upload to $SERVER/upload/image failed"
UPLOADED=$(json 'd["name"] if not d.get("subfolder") else d["subfolder"] + "/" + d["name"]' < "$TMP/upload.json")
echo "stored as $UPLOADED"

step "Queue $(basename "$WORKFLOW")"
CLIENT_ID=sculptforge-smoke-$$
# Apply the SculptForge contract: sf_image (or the first LoadImage) gets the upload, sf_prompt gets the
# prompt, every seed / noise_seed (and sf_seed) gets a fresh value.
python3 - "$WORKFLOW" "$UPLOADED" "$PROMPT" "$CLIENT_ID" > "$TMP/request.json" <<'EOF'
import json, random, sys
path, image, prompt, client_id = sys.argv[1:]
wf = json.load(open(path, encoding="utf-8"))
if "nodes" in wf and "links" in wf:
    sys.exit("error: this is the UI format; export the workflow with \"Export (API)\"")
title = lambda n: n.get("_meta", {}).get("title", "")
is_link = lambda v: isinstance(v, list) and len(v) == 2
loaders = [k for k, n in wf.items() if n.get("class_type") == "LoadImage"]
targets = [k for k in loaders if title(wf[k]) == "sf_image"] or loaders[:1]
for k in targets:
    wf[k]["inputs"]["image"] = image
    print(f"  image  -> node {k} ({title(wf[k]) or 'LoadImage'})", file=sys.stderr)
for k, n in wf.items():
    if title(n) == "sf_prompt":
        key = next((x for x in ("text", "prompt", "string") if x in n["inputs"]), None)
        if key:
            n["inputs"][key] = prompt
            print(f"  prompt -> node {k}.{key}", file=sys.stderr)
    for key in ("seed", "noise_seed") + (("value",) if title(n) == "sf_seed" else ()):
        if key in n["inputs"] and not is_link(n["inputs"][key]):
            n["inputs"][key] = random.randrange(2**32)
            print(f"  seed   -> node {k}.{key} = {n['inputs'][key]}", file=sys.stderr)
if not targets and not any(title(n) == "sf_prompt" for n in wf.values()):
    print("  (no LoadImage or sf_prompt node: running the workflow as is)", file=sys.stderr)
json.dump({"prompt": wf, "client_id": client_id}, sys.stdout)
EOF
HTTP=$(curl -s --max-time 60 -o "$TMP/queued.json" -w '%{http_code}' -H 'Content-Type: application/json' \
  --data-binary "@$TMP/request.json" "$SERVER/prompt") || fail "POST $SERVER/prompt failed"
if [ "$HTTP" != 200 ]; then
  echo "ComfyUI rejected the workflow (HTTP $HTTP):" >&2
  python3 - "$TMP/queued.json" >&2 <<'EOF' || cat "$TMP/queued.json" >&2
import json, sys
d = json.load(open(sys.argv[1]))
e = d.get("error", {})
print(f"  {e.get('message', '')} {e.get('details', '')}".rstrip())
for node, info in d.get("node_errors", {}).items():
    for err in info.get("errors", []):
        print(f"  node {node} ({info.get('class_type', '?')}): {err.get('message')}: {err.get('details')}")
EOF
  exit 1
fi
PROMPT_ID=$(json 'd["prompt_id"]' < "$TMP/queued.json")
echo "prompt_id $PROMPT_ID"

step "Run"
START=$(date +%s)
while :; do
  curl -sf --max-time 30 "$SERVER/history/$PROMPT_ID" -o "$TMP/history.json" || fail "GET /history/$PROMPT_ID failed"
  STATE=$(json '(lambda h: "pending" if not h else h.get("status", {}).get("status_str") or ("success" if h.get("outputs") else "running"))(next(iter(d.values()), None))' < "$TMP/history.json")
  case $STATE in
    success) break ;;
    error)
      python3 - "$TMP/history.json" >&2 <<'EOF'
import json, sys
h = next(iter(json.load(open(sys.argv[1])).values()))
for kind, msg in h.get("status", {}).get("messages", []):
    if kind == "execution_error":
        print(f"  node {msg.get('node_id')} ({msg.get('node_type')}): {msg.get('exception_type')}: {msg.get('exception_message', '').strip()}")
EOF
      fail "the workflow raised an error" ;;
  esac
  [ $(( $(date +%s) - START )) -lt "$TIMEOUT" ] || {
    curl -s -X POST "$SERVER/interrupt" >/dev/null || true
    fail "timed out after ${TIMEOUT}s (sent /interrupt)"
  }
  sleep 1
done
echo "finished in $(( $(date +%s) - START ))s"

step "Outputs"
# Every output file, then the first mesh among them (same extensions and order as SculptForge).
python3 - "$TMP/history.json" > "$TMP/mesh.txt" <<'EOF'
import json, sys
h = next(iter(json.load(open(sys.argv[1])).values()))
mesh_ext = (".glb", ".gltf", ".obj", ".ply", ".stl", ".fbx", ".usdz")
first = None
for node, outputs in h.get("outputs", {}).items():
    for key, items in outputs.items():
        for item in items if isinstance(items, list) else []:
            if isinstance(item, dict) and item.get("filename"):
                print(f"  node {node} {key}: {item.get('subfolder') or '.'}/{item['filename']} ({item.get('type', 'output')})",
                      file=sys.stderr)
                if first is None and item["filename"].lower().endswith(mesh_ext):
                    first = item
if first:
    print("\t".join([first["filename"], first.get("subfolder", ""), first.get("type", "output")]))
EOF
[ -s "$TMP/mesh.txt" ] || fail "the workflow finished but produced no mesh file (.glb .gltf .obj .ply .stl .fbx .usdz)"
IFS=$'\t' read -r NAME SUBFOLDER TYPE < "$TMP/mesh.txt"

step "Download $NAME"
mkdir -p "$OUT"
DEST=$OUT/$NAME
curl -sf --max-time 120 -G "$SERVER/view" --data-urlencode "filename=$NAME" --data-urlencode "subfolder=$SUBFOLDER" \
  --data-urlencode "type=$TYPE" -o "$DEST" || fail "GET /view for $NAME failed"
python3 - "$DEST" <<'EOF'
import json, os, struct, sys
path = sys.argv[1]
data = open(path, "rb").read()
print(f"{path}: {len(data):,} bytes")
if not data:
    sys.exit("FAILED: the mesh file is empty")
if path.lower().endswith(".glb"):
    magic, version, length = struct.unpack_from("<4sII", data)
    if magic != b"glTF" or length != len(data):
        sys.exit(f"FAILED: not a valid GLB (magic {magic!r}, header length {length})")
    chunk_len, chunk_type = struct.unpack_from("<I4s", data, 12)
    gltf = json.loads(data[20:20 + chunk_len])
    accessors = gltf.get("accessors", [])
    verts = tris = 0
    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            verts += accessors[prim["attributes"]["POSITION"]]["count"]
            if "indices" in prim:
                tris += accessors[prim["indices"]]["count"] // 3
    print(f"GLB v{version}: {len(gltf.get('meshes', []))} mesh(es), {verts:,} vertices, {tris:,} triangles")
EOF
printf '\nOK: %s -> %s\n' "$(basename "$WORKFLOW")" "$DEST"
