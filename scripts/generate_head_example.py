"""Create a UV-less ellipsoid aligned with the generated hair_400_cards.fbx example.
Only uses the Python standard library. Not required by the application.
"""
import math
from pathlib import Path

out = Path(__file__).resolve().parents[1] / 'samples/head_example_no_uv.obj'
segments, rings = 32, 16
vertices = [(0, 130, 0)]
for row in range(1, rings):
    theta = math.pi * row / rings
    for col in range(segments):
        phi = 2 * math.pi * col / segments
        vertices.append((90*math.sin(theta)*math.cos(phi), 25+105*math.cos(theta),
                         90*math.sin(theta)*math.sin(phi)))
vertices.append((0,-80,0))
faces = []
for col in range(segments):
    faces.append((1, 2+(col+1)%segments, 2+col))
for row in range(rings-2):
    for col in range(segments):
        a = 2+row*segments+col
        b = 2+row*segments+(col+1)%segments
        faces.append((a,b,b+segments,a+segments))
for col in range(segments):
    faces.append((len(vertices),2+(rings-2)*segments+col,2+(rings-2)*segments+(col+1)%segments))
out.write_text('# Generated opaque head example. No UVs. Units match hair_400_cards.fbx.\n'
               + ''.join('v %.9g %.9g %.9g\n' % p for p in vertices)
               + ''.join('f ' + ' '.join(map(str,f)) + '\n' for f in faces))
print(out)
