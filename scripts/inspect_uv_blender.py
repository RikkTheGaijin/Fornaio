"""Independent, read-only FBX UV check. Writes a diagnostic report, never the input mesh."""
import bpy
import json
import sys
from pathlib import Path

args = sys.argv[sys.argv.index('--') + 1:]
source, destination = map(Path, args)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.fbx(filepath=str(source))
objects = []
for obj in bpy.context.scene.objects:
    if obj.type != 'MESH' or len(obj.data.uv_layers) < 2:
        continue
    mesh = obj.data
    mesh.calc_loop_triangles()
    uv = mesh.uv_layers[1].data
    faces = []
    for p in mesh.polygons:
        points = [list(uv[i].uv) for i in p.loop_indices]
        if any(.034 < a[0] < .049 and .057 < a[1] < .071 for a in points):
            faces.append({'face': p.index, 'uv': points})
    objects.append({'name': obj.name, 'polygons':len(mesh.polygons), 'uv_names': [u.name for u in mesh.uv_layers], 'nearby_faces':faces})
destination.write_text(json.dumps(objects, indent=2), encoding='utf-8')
