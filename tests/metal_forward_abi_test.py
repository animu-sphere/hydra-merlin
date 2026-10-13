"""Check Slang reflection and emitted MSL against the native Metal upload ABI."""
import json
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])


def load(stem, entry, stage):
    data = json.loads((root / f"{stem}.metal.reflection.json").read_text())
    ep, = data["entryPoints"]
    assert (ep["name"], ep["stage"]) == (entry, stage), stem
    return data, (root / f"{stem}.metal").read_text()


def parameter(data, source, name, kind, slot, attribute="buffer", used=True):
    param, = (p for p in data["parameters"] if p["name"] == name)
    binding = param.get("binding", param.get("bindings", [None])[0])
    assert (binding["kind"], binding["index"]) == (kind, slot), name
    if used:
        assert re.search(rf"\b{name}_\d+ \[\[{attribute}\({slot}\)\]\]", source), name
    return param["type"]


def constants(typ, size, fields):
    assert typ["elementVarLayout"]["binding"]["size"] == size, typ
    actual = typ["elementType"]["fields"]
    assert [(f["name"], f["binding"]["offset"], f["binding"]["size"]) for f in actual] == fields


material_fields = [("base_color", 0, 16), ("light_direction_intensity", 16, 16),
                   ("light_color_alpha_cutoff", 32, 16), ("diffuse_environment", 48, 144), ("output_transform", 192, 16)]
draw_fields = [("model_view_projection", 0, 64), ("normal_matrix_column0", 64, 16),
               ("normal_matrix_column1", 80, 16), ("normal_matrix_column2", 96, 16),
               ("feature_mask", 112, 4), ("prim_id", 116, 4), ("instance_id", 120, 4),
               ("texture_index", 124, 4), ("sampler_index", 128, 4), ("padding", 132, 12)]
scene_fields = [("view_projection", 0, 64), ("draw_slot", 64, 4), ("padding", 68, 12)]

for stem, entry, stage in [
    ("forward.vertex", "merlin_vertex", "vertex"),
    ("forward.conventional", "merlin_fragment_conventional", "fragment"),
    ("forward.argument-buffer", "merlin_fragment_argument_buffer", "fragment"),
    ("forward.gpu-scene-vertex", "merlin_vertex_gpu_scene", "vertex"),
    ("forward.gpu-scene-fragment", "merlin_fragment_gpu_scene", "fragment"),
]:
    data, source = load(stem, entry, stage)
    scene = "gpu-scene" in stem
    constants(parameter(data, source, "gpu_scene_constants" if scene else "draw_constants",
                        "constantBuffer", 1), 80 if scene else 144,
              scene_fields if scene else draw_fields)
    if stage == "vertex":
        fields = data["entryPoints"][0]["parameters"][0]["type"]["fields"]
        assert [f["binding"]["index"] for f in fields] == [0, 1, 2, 3]
        assert [f["type"]["elementCount"] for f in fields] == [3, 3, 4, 2]
    else:
        constants(parameter(data, source, "material_constants", "constantBuffer", 2), 208, material_fields)
        outputs = data["entryPoints"][0]["result"]["type"]["fields"]
        assert [f["binding"]["index"] for f in outputs] == [0, 1, 2]
        assert outputs[0]["type"]["elementType"]["scalarType"] == "float32"
        assert [f["type"]["scalarType"] for f in outputs[1:]] == ["uint32", "uint32"]
    if scene:
        for name, slot, size in [("gpu_geometries", 4, 64), ("gpu_instances", 5, 128),
                                 ("gpu_materials", 6, 48), ("gpu_draws", 7, 32)]:
            typ = parameter(data, source, name, "constantBuffer", slot)
            assert typ["baseShape"] == "structuredBuffer"
            fields = typ["resultType"]["fields"]
            assert max(f["binding"]["offset"] + f["binding"]["size"] for f in fields) == size
            assert re.search(rf"device const\* {name}_\d+", source), name
    if stem in ("forward.argument-buffer", "forward.gpu-scene-fragment"):
        typ = parameter(data, source, "resources", "constantBuffer", 3)
        fields = typ["elementType"]["fields"]
        assert [(f["name"], f["type"]["elementCount"]) for f in fields] == [("textures", 128), ("samplers", 32)]
        assert [(f["binding"]["offset"], f["binding"]["size"]) for f in fields] == [(0, 1024), (1024, 256)]
        assert typ["elementVarLayout"]["binding"]["count"] == 160
    if stem == "forward.conventional":
        parameter(data, source, "base_color_texture", "shaderResource", 0, "texture")
        parameter(data, source, "base_color_sampler", "samplerState", 0, "sampler")

load("presentation.vertex", "merlin_presentation_vertex", "vertex")
data, source = load("presentation.fragment", "merlin_presentation_fragment", "fragment")
parameter(data, source, "presentation_color", "shaderResource", 0, "texture")
parameter(data, source, "presentation_sampler", "samplerState", 0, "sampler")
constants(parameter(data, source, "presentation_constants", "constantBuffer", 0), 4, [("color_space", 0, 4)])
data, source = load("forward.color-convert", "merlin_convert_color", "compute")
assert data["entryPoints"][0]["threadGroupSize"] == [8, 8, 1]
parameter(data, source, "source_color", "shaderResource", 0, "texture")
parameter(data, source, "destination_color", "shaderResource", 1, "texture")
print("Metal Forward, GPU Scene, presentation and color-copy ABI verified")
