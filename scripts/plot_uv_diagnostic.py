"""Plot the independent Blender UV report without modifying the geometry."""
import json
import sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon

report = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
obj = next(o for o in report if o['name'] == 'doll_hair:Layer_4')
fig, ax = plt.subplots(figsize=(9, 7))
for face in obj['nearby_faces']:
    if face['face'] > 368:
        continue
    uv = face['uv']
    problem = face['face'] == 365
    ax.add_patch(Polygon(uv, closed=True, facecolor='#e65f65' if problem else '#77b9cd',
                         edgecolor='#ba1823' if problem else '#305979', alpha=.45,
                         linewidth=2 if problem else 1, zorder=4 if problem else 1))
    if not problem:
        ax.text(sum(p[0] for p in uv)/4, sum(p[1] for p in uv)/4, str(face['face']),
                ha='center', va='center', fontsize=10)
bad = next(f for f in obj['nearby_faces'] if f['face'] == 365)
ax.plot(*zip(*(bad['uv'] + [bad['uv'][0]])), color='#ba1823', linewidth=2, zorder=5)
corner = bad['uv'][3]
ax.plot(*corner, 'o', color='#ba1823', markersize=8, zorder=6)
ax.annotate('Polygon 365: this corner crosses\nthe two neighbouring columns\nUV (0.035179, 0.069999)',
            xy=corner, xytext=(.0335,.087), fontsize=11, color='#9d1120',
            arrowprops={'arrowstyle':'->','color':'#ba1823','lw':1.8}, zorder=7)
ax.set(xlim=(.032,.051), ylim=(.044,.094), xlabel='U', ylabel='V',
       title='hair.fbx · Map2 (UV2)\ndoll_hair:Layer_4 — folded polygon, not a mirrored island')
ax.grid(alpha=.2)
ax.ticklabel_format(useOffset=False)
fig.text(.5,.015,'Polygon numbers are zero-based. Coordinates independently read with Blender; input file unchanged.',
         ha='center',fontsize=9)
fig.tight_layout(rect=(0,.025,1,1))
fig.savefig(sys.argv[2],dpi=160)
