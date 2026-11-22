"""Tier bakes of the showcase's Tripo props (art-src tripo-raw/showcase/<slug>/<slug>.glb, raw H3.1 at about 1M
triangles, never edited) into art-src props/showcase/<slug>/<tier>/<slug>.glb, for fetch_art.sh.

    blender -b --factory-startup --python tools/tripo_tiers.py -- RAW_DIR OUT_DIR [slug,slug] [--tiers high,medium,low]

high: decimated to HIGH tris, a fresh unwrap, and colour, roughness/metal and normals ray-cast across from the full
raw at 4K (appearance-preserving simplification: Cohen, Olano and Manocha, SIGGRAPH 1998). Tripo's H3.1 raws pack
hundreds of gutterless charts, so a raw decimated with its own UVs cracks white along them; the rebake avoids that.
medium and low: the high mesh decimated again (its unwrap has big charts with margins, so it survives the collapse)
with its maps scaled to 2K and 1K. Every tier keeps the raw's scale (about a metre across) and orientation; the
origin goes to the centre of its footprint on the ground. The bench sizes and turns each prop where it places it.
One-sided materials (a new Blender material exports doubleSided, which casts shadows from inside faces too).
"""
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
RAW, OUT = argv[0], argv[1]
ONLY = argv[2].split(",") if len(argv) > 2 and not argv[2].startswith("--") else []
TIERS = argv[argv.index("--tiers") + 1].split(",") if "--tiers" in argv else ["high", "medium", "low"]
# triangles per tier; the buildings keep more
BIG = {"sea_fort", "sea_fort_causeway", "lighthouse", "lighthouse_keepers_house"}
TRIS = {"high": 250000, "medium": 60000, "low": 12000}
TRIS_BIG = {"high": 400000, "medium": 120000, "low": 25000}
# long pieces laid diagonally in the raw, turned so their length runs along x
ALIGN = {"timber_rail"}
TEX = {"high": 4096, "medium": 2048, "low": 1024}


def tris(ob):
    return sum(len(p.vertices) - 2 for p in ob.data.polygons)


def decimate(ob, target):
    n = tris(ob)
    if n <= target:
        return
    m = ob.modifiers.new("dec", "DECIMATE")
    m.decimate_type = "COLLAPSE"
    m.ratio = target / n
    m.use_collapse_triangulate = True
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier=m.name)


def load(slug):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=os.path.join(RAW, slug, slug + ".glb"))
    obs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in obs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = obs[0]
    if len(obs) > 1:
        bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    for o in list(bpy.context.scene.objects):
        if o != ob and o.type != "MESH":
            bpy.data.objects.remove(o)
    ob.parent = None
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    vs = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", vs)
    vs = vs.reshape(-1, 3)
    if slug in ALIGN:
        # a long piece Tripo laid diagonally: its main axis in plan (PCA) turned onto x
        xy = vs[:, :2] - vs[:, :2].mean(0)
        w, v = np.linalg.eigh(xy.T @ xy)
        ax = v[:, np.argmax(w)]
        ob.data.transform(__import__("mathutils").Matrix.Rotation(-math.atan2(ax[1], ax[0]), 4, "Z"))
        ob.data.vertices.foreach_get("co", vs)
    # the origin on the ground at the footprint's centre
    lo, hi = vs.min(0), vs.max(0)
    off = Vector((-(lo[0] + hi[0]) * 0.5, -(lo[1] + hi[1]) * 0.5, -lo[2]))
    ob.data.transform(__import__("mathutils").Matrix.Translation(off))
    ob.data.update()
    ob.name = slug
    return ob


def tex_of(inp):
    n = inp.links[0].from_node if inp.links else None
    while n is not None and n.type != "TEX_IMAGE":
        n = next((i.links[0].from_node for i in n.inputs if i.links), None)
    return n


def bake_high(slug, size, target):
    hi = load(slug)
    bpy.ops.object.select_all(action="DESELECT")
    hi.select_set(True)
    bpy.context.view_layer.objects.active = hi
    bpy.ops.object.duplicate()
    ob = bpy.context.view_layer.objects.active
    ob.name = slug + "_t"
    decimate(ob, target)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 1
    scene.render.threads_mode = "FIXED"
    scene.render.threads = int(os.environ.get("BAKE_THREADS", "2"))
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(60.0), island_margin=0.003)
    bpy.ops.object.mode_set(mode="OBJECT")
    hmat = hi.data.materials[0]
    hbsdf = next(n for n in hmat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    base_tex, rm_tex = tex_of(hbsdf.inputs["Base Color"]), tex_of(hbsdf.inputs["Roughness"])
    mat = bpy.data.materials.new(slug)
    mat.use_nodes = True
    mat.use_backface_culling = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = nodes["Principled BSDF"]
    ob.data.materials.clear()
    ob.data.materials.append(mat)

    def target_img(name, cs):
        img = bpy.data.images.new(slug + "_" + name, size, size, alpha=False)
        img.colorspace_settings.name = cs
        n = nodes.new("ShaderNodeTexImage")
        n.image = img
        return n

    base_n, rm_n, nrm_n = target_img("basecolor", "sRGB"), target_img("rm", "Non-Color"), target_img("normal", "Non-Color")
    links.new(base_n.outputs["Color"], bsdf.inputs["Base Color"])
    sep = nodes.new("ShaderNodeSeparateColor")
    links.new(rm_n.outputs["Color"], sep.inputs[0])
    links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    nm = nodes.new("ShaderNodeNormalMap")
    links.new(nrm_n.outputs["Color"], nm.inputs["Color"])
    links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    bpy.ops.object.select_all(action="DESELECT")
    hi.select_set(True)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    ray = 0.01

    def bake(kind, node, **kw):
        nodes.active = node
        bpy.ops.object.bake(type=kind, use_selected_to_active=True, cage_extrusion=ray * 0.5, max_ray_distance=ray,
                            margin=8, margin_type="EXTEND", **kw)

    em = hbsdf.inputs["Emission Color"]
    hbsdf.inputs["Emission Strength"].default_value = 1.0
    hmat.node_tree.links.new(base_tex.outputs["Color"], em)
    bake("EMIT", base_n)
    if rm_tex is not None:
        hmat.node_tree.links.new(rm_tex.outputs["Color"], em)
        bake("EMIT", rm_n)
    else:
        rm_n.image.generate_type = "BLANK"
        rm_n.image.pixels.foreach_set(np.tile(np.array([0, 0.8, 0, 1], np.float32), size * size))
    hmat.node_tree.links.remove(em.links[0])
    hbsdf.inputs["Emission Strength"].default_value = 0.0
    bake("NORMAL", nrm_n, normal_space="TANGENT")
    # texels no ray found: flat normals, colours from their neighbours
    a = np.empty(size * size * 4, dtype=np.float32)
    nrm_n.image.pixels.foreach_get(a)
    a = a.reshape(size, size, 4)
    missed = a[..., 2] < 0.3
    a[missed] = (0.5, 0.5, 1.0, 1.0)
    nrm_n.image.pixels.foreach_set(a.ravel())
    for node in (base_n, rm_n):
        c = np.empty(size * size * 4, dtype=np.float32)
        node.image.pixels.foreach_get(c)
        c = c.reshape(size, size, 4)
        hole = missed.copy()
        for _ in range(10):
            total, count = np.zeros_like(c), np.zeros((size, size), dtype=np.float32)
            for ax, d in ((0, 1), (0, -1), (1, 1), (1, -1)):
                known = np.roll(~hole, d, ax)
                total += np.roll(c, d, ax) * known[..., None]
                count += known
            fill = hole & (count > 0)
            c[fill] = total[fill] / count[fill][..., None]
            hole &= ~fill
        node.image.pixels.foreach_set(c.ravel())
    bpy.data.objects.remove(hi)
    print("BAKED", slug, size, tris(ob), "missed %.3f" % missed.mean())
    return ob, (base_n.image, rm_n.image, nrm_n.image)


def export(ob, imgs, tier, size):
    d = os.path.join(OUT, ob.name.removesuffix("_t"), tier)
    os.makedirs(d, exist_ok=True)
    slug = os.path.basename(os.path.dirname(d))
    for img, ext in zip(imgs, ("jpg", "png", "png")):
        if img.size[0] != size:
            img.scale(size, size)
        p = os.path.join(d, "%s_%s.%s" % (slug, img.name.split("_")[-1], ext))
        img.filepath_raw = p
        img.file_format = "JPEG" if ext == "jpg" else "PNG"
        if ext == "jpg":
            bpy.context.scene.render.image_settings.quality = 92
        img.save()
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    bpy.ops.export_scene.gltf(filepath=os.path.join(d, slug + ".glb"), use_selection=True, export_format="GLB",
                              export_image_format="AUTO", export_materials="EXPORT",
                              export_apply=True, export_yup=True)
    for img in imgs:
        os.remove(bpy.path.abspath(img.filepath_raw))
    print("EXPORTED", slug, tier, tris(ob), size)


slugs = sorted(s for s in os.listdir(RAW) if not s.startswith("_") and os.path.exists(os.path.join(RAW, s, s + ".glb")))
for slug in slugs:
    if ONLY and slug not in ONLY:
        continue
    budget = TRIS_BIG if slug in BIG else TRIS
    ob, imgs = bake_high(slug, TEX["high"], budget["high"])
    for tier in ("high", "medium", "low"):
        if tier != "high":
            decimate(ob, budget[tier])
        if tier in TIERS:
            export(ob, imgs, tier, TEX[tier])
