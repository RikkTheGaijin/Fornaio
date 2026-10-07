"""Developer-only fixture generation. Run with Blender --background --factory-startup --python.
The delivered application never uses Blender or Python.
"""
import bpy
import math
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'samples'
OUT.mkdir(exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
expected = {}

def export_cards(name, count, segments=1, hair=False):
    vertices, faces, mask_uvs, ao_uvs = [], [], [], []
    cols = math.ceil(math.sqrt(count))
    for i in range(count):
        angle = (i % 40) * math.tau / 40 + (i // 40) * .17
        row = i // 40
        start = len(vertices)
        for j in range(segments + 1):
            t = j / segments
            if hair:
                r = 1 + row * .015 + .12 * math.sin(t * math.pi)
                z = 1.2 - row * .12 - t * 1.15
                width = .09 * (1 - .7 * t)
                for side in [-1, 1]:
                    vertices.append((r * math.cos(angle) - side * width * math.sin(angle),
                                     r * math.sin(angle) + side * width * math.cos(angle), z))
            else:
                vertices.extend([(-1, -1 + 2*t, i * .12), (1, -1 + 2*t, i * .12)])
        for j in range(segments):
            a, b = start + j*2, start + (j+1)*2
            # Orient hair card normals outwards, flat cards toward +Z.
            corners = [a, b, b+1, a+1] if hair else [a, a+1, b+1, b]
            faces.append(corners)
            for index in corners:
                k = index - start
                u, v = k % 2, (k // 2) / segments
                mask_uvs.append((u, v))
                ao_uvs.append(((i % cols + .08 + .84*u) / cols,
                               (i // cols + .08 + .84*v) / cols))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    for uvname, values in [('HairMask', mask_uvs), ('UniqueAO', ao_uvs)]:
        uv = mesh.uv_layers.new(name=uvname)
        for loop, value in zip(uv.data, values):
            loop.uv = value
    if name == 'transformed_cards':
        obj.location = (1.3, -.7, .4)
        obj.rotation_euler = (.15, .3, -.2)
        obj.scale = (-1.2, .7, 1.5)
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.fbx(filepath=str(OUT / f'{name}.fbx'), use_selection=True,
        add_leaf_bones=False, bake_anim=False, axis_forward='-Z', axis_up='Y',
        apply_unit_scale=False, global_scale=1.0)
    points = [obj.matrix_world @ vertex.co for vertex in mesh.vertices]
    lo = [min(p[a] for p in points) for a in range(3)]
    hi = [max(p[a] for p in points) for a in range(3)]
    expected[name + '.fbx'] = {'bounds_diagonal': 100 * math.sqrt(sum((hi[a]-lo[a])**2 for a in range(3))),
                             'triangles': len(faces)*2}
    obj.hide_set(True)
    return obj

export_cards('stacked_cards', 3)
export_cards('transformed_cards', 3)
export_cards('hair_400_cards', 400, segments=6, hair=True)
(OUT / 'expected_import.json').write_text(json.dumps(expected, indent=2))

# OBJ has only one UV set; this simple unique-UV fixture intentionally uses one map.
(OUT / 'single_card.obj').write_text('''# Front-facing card, one unique UV set
v -1 -1 0
v 1 -1 0
v 1 1 0
v -1 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
f 1/1/1 2/2/1 3/3/1 4/4/1
''')

for kind in ['strands', 'opaque', 'transparent', 'half']:
    w, h = 256, 256
    im = bpy.data.images.new(kind, width=w, height=h, alpha=False)
    pixels = []
    for y in range(h):
        for x in range(w):
            if kind == 'strands':
                distance = abs(((x + 2*math.sin(y*.045)) % 12) - 6)
                a = max(0.0, min(1.0, 2.5-distance))
                a *= min(1.0, y/15, (h-1-y)/30)
            else:
                a = {'opaque': 1, 'transparent': 0, 'half': .5}[kind]
            pixels.extend((a, a, a, 1))
    im.colorspace_settings.name = 'Non-Color'
    im.pixels = pixels
    im.filepath_raw = str(OUT / f'{kind}.png')
    im.file_format = 'PNG'
    im.save()
    if kind == 'strands':
        for ext, fmt in [('tga', 'TARGA'), ('tiff', 'TIFF')]:
            im.filepath_raw = str(OUT / f'{kind}.{ext}')
            im.file_format = fmt
            im.save()
print('Fixture files written to', OUT)
