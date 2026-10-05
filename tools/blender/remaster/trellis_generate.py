#!/usr/bin/env python3
"""Generated meshes from a model's renders with TRELLIS (github.com/microsoft/TRELLIS).

Runs in TRELLIS's own Python environment (CUDA), not Blender's:

    python trellis_generate.py VIEWS_DIR OUT_DIR [--seeds 1,2,3]

VIEWS_DIR holds the RGBA renders remaster-prepare made (view_00.png is the front). Every
seed gives one candidate, OUT_DIR/trellis_s<seed>.obj: the raw mesh (TRELLIS's Z-up axes,
in a unit cube), without TRELLIS's own texture bake: remaster-build maps the game's
textures onto it. A candidate already there is not generated again.
"""

import argparse
import glob
import os
import sys

os.environ.setdefault("SPCONV_ALGO", "native")  # no benchmarking for a few runs


def write_obj(path, vertices, faces):
    tmp = path + ".part"
    with open(tmp, "w") as f:
        f.write("# TRELLIS mesh, Z up\n")
        for v in vertices:
            f.write("v %.6f %.6f %.6f\n" % (v[0], v[1], v[2]))
        for t in faces:
            f.write("f %d %d %d\n" % (t[0] + 1, t[1] + 1, t[2] + 1))
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("views")
    ap.add_argument("out")
    ap.add_argument("--seeds", default="1,2,3")
    ap.add_argument("--model", default=os.environ.get("TRELLIS_MODEL", "microsoft/TRELLIS-image-large"))
    ap.add_argument("--max-views", type=int, default=4, help="the first N renders (front first)")
    ap.add_argument("--steps", type=int, default=25, help="sampler steps for both stages")
    ap.add_argument("--simplify", type=float, default=0.5, help="share of faces TRELLIS's clean-up removes")
    args = ap.parse_args()

    views = sorted(glob.glob(os.path.join(args.views, "view_*.png")))[:args.max_views]
    if not views:
        sys.exit("no view_*.png in %s" % args.views)
    os.makedirs(args.out, exist_ok=True)
    seeds = [int(s) for s in args.seeds.split(",") if s.strip()]
    todo = [s for s in seeds if not os.path.exists(os.path.join(args.out, "trellis_s%d.obj" % s))]
    if not todo:
        print("all candidates present")
        return

    from PIL import Image
    from trellis.pipelines import TrellisImageTo3DPipeline
    from trellis.utils import postprocessing_utils

    pipeline = TrellisImageTo3DPipeline.from_pretrained(args.model)
    pipeline.cuda()
    images = [Image.open(p) for p in views]  # RGBA: TRELLIS crops to the alpha, no background removal
    params = {"sparse_structure_sampler_params": {"steps": args.steps, "cfg_strength": 7.5},
              "slat_sampler_params": {"steps": args.steps, "cfg_strength": 3.0},
              "formats": ["mesh"]}
    for seed in todo:
        if len(images) == 1:
            outputs = pipeline.run(images[0], seed=seed, **params)
        else:
            outputs = pipeline.run_multi_image(images, seed=seed, **params)
        mesh = outputs["mesh"][0]
        vertices = mesh.vertices.detach().cpu().numpy()
        faces = mesh.faces.detach().cpu().numpy()
        # TRELLIS's own clean-up: decimation and the removal of faces no view can see
        # (the inside of the extracted surface)
        vertices, faces = postprocessing_utils.postprocess_mesh(
            vertices, faces, simplify=args.simplify > 0, simplify_ratio=args.simplify, fill_holes=True)
        out = os.path.join(args.out, "trellis_s%d.obj" % seed)
        write_obj(out, vertices, faces)
        print("wrote %s (%d faces)" % (out, len(faces)))


if __name__ == "__main__":
    main()
