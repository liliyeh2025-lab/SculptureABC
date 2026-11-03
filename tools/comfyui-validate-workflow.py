#!/usr/bin/env python3
# Checks ComfyUI API-format workflows against a ComfyUI server's node definitions (/object_info)
# and against the SculptForge workflow contract (sf_image / sf_prompt / sf_seed, a mesh-saving output).
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/comfyui-validate-workflow.py integrations/comfyui/workflows/*.json
#   tools/comfyui-validate-workflow.py --server http://127.0.0.1:8188 my-workflow.json
#   tools/comfyui-validate-workflow.py --object-info object_info.json --strict my-workflow.json
#
# Errors (exit code 1): unknown node types, missing required inputs, links to nodes or outputs that
# don't exist, links whose types don't match, numbers out of range, invalid choices, cycles, no output node.
# Warnings: model files the server doesn't have, SculptForge nodes on a server without the pack
# (both errors with --strict), unused nodes, contract hints. Standard library only.
import argparse
import json
import sys
import urllib.error
import urllib.request

PACK_PREFIX = "SculptForge"  # class_type prefix of the ComfyUI-SculptForge node pack
PROMPT_KEYS = ("text", "prompt", "string")  # the input SculptForge fills on the "sf_prompt" node
SEED_KEYS = ("seed", "noise_seed")
MESH_SAVERS = ("SaveGLB", "Save3DAdvanced", "SculptForgeExport", "SculptForgeLODChain")


def load_object_info(server, path):
    if path:
        with open(path, encoding="utf-8") as f:
            return json.load(f), path
    url = server.rstrip("/") + "/object_info"
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return json.load(r), url
    except (urllib.error.URLError, OSError) as e:
        sys.exit(f"error: could not read {url}: {e}\n(start ComfyUI, pass --server URL, or use --object-info FILE)")


def input_spec(spec):
    """(type, options, config) from an /object_info input spec, for both the old and new styles:
    [["a", "b"], {...}] and ["COMBO", {"options": [...]}] are choices, ["INT", {...}] is a typed input."""
    kind = spec[0] if spec else "*"
    config = spec[1] if len(spec) > 1 and isinstance(spec[1], dict) else {}
    if isinstance(kind, list):
        return "COMBO", kind, config
    if kind == "COMBO":
        return "COMBO", config.get("options", []), config
    return kind, None, config


def types_match(produced, accepted):
    if produced == "*" or accepted == "*":
        return True
    have = {t.strip() for t in produced.split(",")}
    want = {t.strip() for t in accepted.split(",")}
    return bool(have & want) or "*" in want


def is_link(value):
    return (isinstance(value, list) and len(value) == 2 and isinstance(value[0], (str, int))
            and isinstance(value[1], int) and not isinstance(value[1], bool))


def is_file_choice(name, class_type):
    """Choices that list files on the server (models, input images): a mismatch only means
    the file isn't installed there, which is a setup issue, not a broken workflow."""
    return name.endswith("_name") or name in ("image", "model_file", "audio", "video") or "Loader" in class_type


class Report:
    def __init__(self, strict):
        self.strict, self.errors, self.warnings, self.notes = strict, [], [], []

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg, strict_error=False):
        (self.errors if self.strict and strict_error else self.warnings).append(msg)


def node_label(node_id, node):
    title = node.get("_meta", {}).get("title")
    return f'node {node_id} ({node.get("class_type", "?")}{f", “{title}”" if title else ""})'


def validate(workflow, info, rep):
    if not isinstance(workflow, dict) or not workflow:
        rep.error("not a JSON object of nodes")
        return
    if "nodes" in workflow and "links" in workflow:
        rep.error('this is ComfyUI\'s UI (editor) format; save it with "Export (API)" instead')
        return

    # The LoadImage node(s) SculptForge fills with its own upload: whatever image they name now doesn't matter.
    loaders = [nid for nid, n in workflow.items() if isinstance(n, dict) and n.get("class_type") == "LoadImage"]
    sf_image = [nid for nid in loaders if workflow[nid].get("_meta", {}).get("title") == "sf_image"]
    replaced = set(sf_image or loaders[:1])

    # Per node: its definition (or None), the inputs it was given.
    defs = {}
    for node_id, node in workflow.items():
        if not isinstance(node, dict) or "class_type" not in node or not isinstance(node.get("inputs"), dict):
            rep.error(f"node {node_id}: needs \"class_type\" and an \"inputs\" object")
            continue
        ct = node["class_type"]
        if ct in info:
            defs[node_id] = info[ct]
        elif ct.startswith(PACK_PREFIX):
            defs[node_id] = None
            rep.warn(f"{node_label(node_id, node)}: from the ComfyUI-SculptForge pack, which this server has not "
                     "loaded; its inputs were not checked", strict_error=True)
        else:
            rep.error(f"{node_label(node_id, node)}: unknown node type (missing custom node pack?)")

    for node_id, node in workflow.items():
        if node_id not in defs:
            continue
        d = defs[node_id]
        label = node_label(node_id, node)
        inputs = node["inputs"]
        required = d["input"].get("required", {}) if d else {}
        optional = d["input"].get("optional", {}) if d else {}
        hidden = d["input"].get("hidden", {}) if d else {}
        if d:
            for name in required:
                if name not in inputs:
                    rep.error(f"{label}: required input “{name}” is missing")
            for name in inputs:
                if name not in required and name not in optional and name not in hidden:
                    rep.warn(f"{label}: input “{name}” is not used by this node type")

        for name, value in inputs.items():
            spec = required.get(name) or optional.get(name)
            kind, options, config = input_spec(spec) if spec else ("*", None, {})
            if is_link(value):
                src_id, slot = str(value[0]), value[1]
                src = workflow.get(src_id)
                if src is None:
                    rep.error(f"{label}: input “{name}” links to node {src_id}, which does not exist")
                    continue
                src_def = defs.get(src_id)
                if src_def is None:
                    continue  # unknown or pack node: already reported
                outputs = src_def.get("output", [])
                if not 0 <= slot < len(outputs):
                    rep.error(f"{label}: input “{name}” links to output {slot} of {node_label(src_id, src)}, "
                              f"which has {len(outputs)} output(s)")
                    continue
                produced = outputs[slot]
                accepted = "COMBO" if kind == "COMBO" else kind
                if d and isinstance(produced, str) and not types_match(produced, accepted):
                    if not (kind == "COMBO" and produced in ("STRING", "COMBO")) and not isinstance(produced, list):
                        rep.error(f"{label}: input “{name}” expects {accepted} but {node_label(src_id, src)} "
                                  f"output {slot} is {produced}")
                continue
            if not d or not spec:
                continue
            if kind == "COMBO":
                if node_id in replaced and name == "image":
                    continue
                if options and value not in options:
                    if is_file_choice(name, node["class_type"]):
                        rep.warn(f"{label}: “{value}” is not on this server ({name}); install it before running",
                                 strict_error=True)
                    else:
                        rep.error(f"{label}: “{name}” = {value!r} is not one of {options}")
                elif not options and is_file_choice(name, node["class_type"]):
                    rep.warn(f"{label}: this server has no files for “{name}”; install {value!r} before running",
                             strict_error=True)
            elif kind in ("INT", "FLOAT"):
                if isinstance(value, bool) or not isinstance(value, (int, float)) or (kind == "INT" and value != int(value)):
                    rep.error(f"{label}: “{name}” must be {kind}, got {value!r}")
                    continue
                lo, hi = config.get("min"), config.get("max")
                if (lo is not None and value < lo) or (hi is not None and value > hi):
                    rep.error(f"{label}: “{name}” = {value} is outside [{lo}, {hi}]")
            elif kind == "BOOLEAN" and not isinstance(value, bool):
                rep.error(f"{label}: “{name}” must be true or false, got {value!r}")
            elif kind == "STRING" and not isinstance(value, str):
                rep.error(f"{label}: “{name}” must be a string, got {value!r}")

    # Cycles, output nodes, and nodes that never run (not upstream of any output).
    upstream = {nid: [str(v[0]) for v in n["inputs"].values() if is_link(v) and str(v[0]) in workflow]
                for nid, n in workflow.items() if isinstance(n, dict) and isinstance(n.get("inputs"), dict)}
    state = {}

    def visit(nid, stack):
        if state.get(nid) == 1:
            rep.error(f"cycle: {' -> '.join(stack + [nid])}")
            return
        if state.get(nid) == 2:
            return
        state[nid] = 1
        for src in upstream.get(nid, []):
            visit(src, stack + [nid])
        state[nid] = 2

    for nid in upstream:
        visit(nid, [])

    outputs = [nid for nid, d in defs.items()
               if (d and d.get("output_node")) or (d is None and workflow[nid]["class_type"] in MESH_SAVERS)]
    if not outputs:
        rep.error("no output node (e.g. Save 3D Model): ComfyUI would refuse to run it")
    reached, todo = set(), list(outputs)
    while todo:
        nid = todo.pop()
        if nid not in reached:
            reached.add(nid)
            todo.extend(upstream.get(nid, []))
    for nid in workflow:
        if nid not in reached and nid in defs:
            rep.warn(f"{node_label(nid, workflow[nid])}: does not feed any output node, so it never runs")

    # The SculptForge contract.
    titled = lambda t: [nid for nid, n in workflow.items() if isinstance(n, dict) and n.get("_meta", {}).get("title") == t]
    for nid in titled("sf_image"):
        if nid not in sf_image:
            rep.warn(f"{node_label(nid, workflow[nid])}: titled sf_image but is not a LoadImage node; SculptForge ignores it")
    if sf_image:
        rep.notes.append(f"image input: node {', '.join(sf_image)} (sf_image)")
    elif loaders:
        rep.notes.append(f"image input: node {loaders[0]} (first LoadImage; title it “sf_image” to be explicit)")
    for nid in titled("sf_prompt"):
        keys = [k for k in PROMPT_KEYS if k in workflow[nid]["inputs"]]
        if not keys:
            rep.error(f"{node_label(nid, workflow[nid])}: titled sf_prompt but has no {'/'.join(PROMPT_KEYS)} input")
        elif is_link(workflow[nid]["inputs"][keys[0]]):
            rep.error(f"{node_label(nid, workflow[nid])}: sf_prompt's “{keys[0]}” is a link; it must be a plain value "
                      "SculptForge can replace")
        else:
            rep.notes.append(f"prompt input: node {nid} (sf_prompt.{keys[0]})")
    seeds = [f"{nid}.{k}" for nid, n in workflow.items() if isinstance(n, dict) and isinstance(n.get("inputs"), dict)
             for k in SEED_KEYS if k in n["inputs"] and not is_link(n["inputs"][k])]
    if titled("sf_seed"):
        seeds.insert(0, f"{','.join(titled('sf_seed'))} (sf_seed)")
    if seeds:
        rep.notes.append(f"seeds: {', '.join(seeds)}")
    if not loaders and not titled("sf_prompt"):
        rep.warn("no LoadImage and no sf_prompt node: SculptForge has nothing to feed this workflow")
    savers = [nid for nid in outputs if workflow[nid]["class_type"] in MESH_SAVERS]
    if outputs and not savers:
        rep.warn("no known mesh-saving node (Save 3D Model, Save 3D Advanced, SculptForge Export); make sure an output "
                 "node writes a .glb/.obj/.ply/.stl/.fbx/.usdz file or SculptForge will find no mesh")
    elif savers:
        rep.notes.append(f"mesh output: node {', '.join(savers)} ({', '.join(sorted({workflow[n]['class_type'] for n in savers}))})")


def main():
    ap = argparse.ArgumentParser(description="Validate ComfyUI API-format workflows for SculptForge.")
    ap.add_argument("workflows", nargs="+", help="API-format workflow JSON files")
    ap.add_argument("--server", default="http://127.0.0.1:8188", help="ComfyUI URL (default %(default)s)")
    ap.add_argument("--object-info", metavar="FILE", help="use a saved /object_info JSON instead of a server")
    ap.add_argument("--strict", action="store_true",
                    help="treat missing model files and an unloaded SculptForge pack as errors")
    args = ap.parse_args()

    info, source = load_object_info(args.server, args.object_info)
    print(f"Checking against {source} ({len(info)} node types)")
    failed = 0
    for path in args.workflows:
        rep = Report(args.strict)
        workflow = None
        try:
            with open(path, encoding="utf-8") as f:
                workflow = json.load(f)
        except (OSError, ValueError) as e:
            rep.error(f"cannot read: {e}")
        else:
            validate(workflow, info, rep)
        status = "FAIL" if rep.errors else "ok"
        count = f"  ({len(workflow)} nodes)" if isinstance(workflow, dict) else ""
        print(f"\n{status:4}  {path}{count}")
        for msg in rep.errors:
            print(f"      error: {msg}")
        for msg in rep.warnings:
            print(f"      warning: {msg}")
        for msg in rep.notes:
            print(f"      {msg}")
        failed += bool(rep.errors)
    print(f"\n{len(args.workflows) - failed} of {len(args.workflows)} workflow(s) valid")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
