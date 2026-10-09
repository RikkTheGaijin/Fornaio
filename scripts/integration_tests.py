"""Real-file and packaged CLI checks; Python standard library only."""
import argparse
import json
import os
import pathlib
import struct
import subprocess
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--exe', default=str(ROOT / 'build/Release/fornaio_cli.exe'))
parser.add_argument('--clean-env', action='store_true', help='Exclude development tool/runtime paths')
args = parser.parse_args()
OUT = ROOT / 'artifacts/integration'
OUT.mkdir(parents=True, exist_ok=True)
ENV = os.environ.copy()
if args.clean_env:
    ENV['PATH'] = str(pathlib.Path(ENV['SYSTEMROOT']) / 'System32')
    for key in list(ENV):
        if key.upper().startswith(('QT', 'CUDA', 'QML', 'PYTHON')):
            del ENV[key]

def run(*args, ok=True):
    p = subprocess.run([str(pathlib.Path(globals()['args'].exe).resolve()), *map(str, args)], cwd=ROOT, env=ENV, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if ok:
        assert p.returncode == 0, p.stderr
    else:
        assert p.returncode != 0, 'Expected validation failure'
    return p

def png(path, linear=True):
    data = pathlib.Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, compressed, header, gamma = 8, b'', None, None
    while pos < len(data):
        n = struct.unpack('>I', data[pos:pos+4])[0]
        kind, payload = data[pos+4:pos+8], data[pos+8:pos+8+n]
        assert zlib.crc32(kind + payload) == struct.unpack('>I', data[pos+8+n:pos+12+n])[0]
        if kind == b'IHDR': header = struct.unpack('>IIBBBBB', payload)
        if kind == b'IDAT': compressed += payload
        if kind == b'gAMA': gamma = struct.unpack('>I', payload)[0]
        pos += n + 12
    if linear:
        assert gamma == 100000
    raw = zlib.decompress(compressed)
    return header, raw

def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    chunk = lambda k, d: struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d))
    pathlib.Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                                   chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def pixels8(path):
    """Decode an 8-bit PNG into rows of channel bytes, undoing all scanline filters."""
    (w, h, depth, color, *_), raw = png(path, linear=False)
    assert depth == 8
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[color]
    rows, prev, pos = [], bytearray(w * bpp), 0
    for _ in range(h):
        f, line = raw[pos], bytearray(raw[pos+1:pos+1+w*bpp])
        pos += 1 + w * bpp
        for i in range(len(line)):
            a = line[i-bpp] if i >= bpp else 0
            b, c = prev[i], prev[i-bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows

fbx = json.loads(run('--inspect', 'samples/stacked_cards.fbx').stdout)
obj = json.loads(run('--inspect', 'samples/single_card.obj').stdout)
assert len(fbx['uv_sets']) == 2 and fbx['triangles'] == 6
assert len(obj['uv_sets']) == 1 and obj['triangles'] == 2
expected = json.loads((ROOT / 'samples/expected_import.json').read_text())
for name, truth in expected.items():
    imported = json.loads(run('--inspect', ROOT / 'samples' / name).stdout)
    assert imported['triangles'] == truth['triangles']
    assert abs(imported['bounds_diagonal'] - truth['bounds_diagonal']) < .001, f'{name}: transformed bounds differ'

outputs = []
for ext in ['png', 'tga', 'tiff']:
    path = OUT / f'mask_{ext}.png'
    run('--mesh', 'samples/stacked_cards.fbx', '--mask', f'samples/strands.{ext}', '--channel', 'red', '--size', 64, '--rays', 64, '--ss', 2, '--out', path)
    outputs.append(png(path))
assert outputs[0] == outputs[1] == outputs[2], 'Mask format results differ'

for mesh in ['stacked_cards.fbx', 'transformed_cards.fbx', 'single_card.obj']:
    base = ['--mesh', ROOT / 'samples' / mesh, '--mask', ROOT / 'samples/half.png', '--size', 32, '--rays', 64, '--ss', 2, '--padding', 0]
    gpu, cpu = OUT / (mesh + '_gpu.png'), OUT / (mesh + '_cpu.png')
    run(*base, '--out', gpu)
    run(*base, '--out', cpu, '--reference')
    (_, rawg), (_, rawc) = png(gpu), png(cpu)
    valuesg = [struct.unpack('>H', rawg[y*65+1+x*2:y*65+3+x*2])[0] for y in range(32) for x in range(32)]
    valuesc = [struct.unpack('>H', rawc[y*65+1+x*2:y*65+3+x*2])[0] for y in range(32) for x in range(32)]
    assert max(abs(a-b) for a,b in zip(valuesg,valuesc)) <= 2, f'{mesh}: imported-file GPU/reference mismatch'

failure = run('--mesh', 'samples/stacked_cards.fbx', '--mask', 'samples/strands.png', '--ao-uv', 1, '--strict-uv', '--size', 32, ok=False)
assert 'overlap' in failure.stderr
failure = run('--mesh', 'samples/single_card.obj', '--mask', 'samples/strands.png', '--ao-uv', 2, '--size', 32, ok=False)
assert 'UV' in failure.stderr

unicode_mesh = OUT / '髪カード.obj'
unicode_mesh.write_bytes((ROOT / 'samples/single_card.obj').read_bytes())
run('--mesh', unicode_mesh, '--mask', 'samples/strands.png', '--size', 32, '--rays', 4, '--out', OUT / '遮蔽.png')

path = OUT / 'transparent.png'
run('--mesh', 'samples/stacked_cards.fbx', '--mask', 'samples/transparent.png', '--channel', 'red', '--size', 32, '--rays', 4, '--out', path)
header, raw = png(path)
assert header == (32, 32, 16, 0, 0, 0, 0)
assert all(raw[y*65] == 0 and all(v == 255 for v in raw[y*65+1:y*65+65]) for y in range(32))
head_args = ['--mesh','samples/single_card.obj','--mask','samples/half.png',
             '--occluder','samples/occluder_no_uv.obj','--size',32,'--rays',64,'--padding',0]
run(*head_args,'--out',OUT/'head_gpu.png')
run(*head_args,'--reference','--out',OUT/'head_cpu.png')
_, rawg = png(OUT/'head_gpu.png')
_, rawc = png(OUT/'head_cpu.png')
gv=[struct.unpack('>H',rawg[y*65+1+x*2:y*65+3+x*2])[0] for y in range(32) for x in range(32)]
cv=[struct.unpack('>H',rawc[y*65+1+x*2:y*65+3+x*2])[0] for y in range(32) for x in range(32)]
assert max(abs(a-b) for a,b in zip(gv,cv))<=2,'Opaque occluder GPU/CPU mismatch'
assert min(gv)<6553,'Opaque occluder did not darken the hair'
print('PASS: UV-less occluder import, opaque casting, and GPU/CPU parity.')

# UV2 generation: the existing (overlapping) UV2 of the stacked cards is replaced by a packed, strictly
# non-overlapping layout that bakes, with and without island rotation.
for rotate in [[], ['--uv-rotate']]:
    uv2 = OUT / f'stacked_cards_UV2{"_rot" if rotate else ""}.fbx'
    report = json.loads(run('--generate-uv2', 'samples/stacked_cards.fbx', '--uv2-out', uv2, '--size', 256, *rotate).stdout)
    assert report['replaced_uv_sets'] == 1 and report['islands'] > 0 and 0 < report['utilization'] <= 1, report
    packed = json.loads(run('--inspect', uv2).stdout)
    assert len(packed['uv_sets']) == 2 and packed['triangles'] == fbx['triangles']
    run('--mesh', uv2, '--mask', 'samples/strands.png', '--ao-uv', 2, '--strict-uv', '--size', 32, '--rays', 4, '--out', OUT / (uv2.stem + '.png'))
failure = run('--generate-uv2', 'samples/single_card.obj', '--uv2-out', OUT / 'nope.fbx', ok=False)
assert 'FBX' in failure.stderr, failure.stderr
print('PASS: UV2 generation replaces overlapping UV2 with a strict, bakeable packed layout; non-FBX input rejected.')

# Texture transfer: identical 8x8 grids; the target UVs are mirrored horizontally, so every target texel
# centre maps exactly onto a source texel centre and the transfer must be lossless.
XFER = OUT / 'transfer'
XFER.mkdir(exist_ok=True)
for old in XFER.glob('out*/*.png'):
    old.unlink()
def grid_obj(path, mirror):
    n, lines = 8, []
    for y in range(n + 1):
        for x in range(n + 1):
            lines.append(f'v {x/n} {y/n} 0')
            lines.append(f'vt {1 - x/n if mirror else x/n} {y/n}')
    lines.append('vn 0 0 1')
    for y in range(n):
        for x in range(n):
            a = y * (n + 1) + x + 1
            quad = [a, a + 1, a + n + 2, a + n + 1]
            lines.append('f ' + ' '.join(f'{i}/{i}/1' for i in quad))
    pathlib.Path(path).write_text('\n'.join(lines) + '\n')
grid_obj(XFER / 'source.obj', False)
grid_obj(XFER / 'target.obj', True)
W = 64
write_png(XFER / 'cards_ALB.png', W, W, [[v for x in range(W) for v in (x * 4 + 2, y * 4 + 1, 128)] for y in range(W)])
write_png(XFER / 'cards_RGH.png', W, W, [[v for x in range(W) for v in (255 - x * 4,) * 3] for y in range(W)])
for backend, out_dir in [((), 'out_gpu'), (('--reference',), 'out_cpu')]:
    job = {'source': {'mesh': 'source.obj', 'uv': 0}, 'target': {'mesh': 'target.obj', 'uv': 0}, 'output_dir': out_dir,
           'sets': [{'name': 'cards', 'files': {'ALB': 'cards_ALB.png', 'RGH': 'cards_RGH.png'}}],
           'settings': {'padding': 4}}
    (XFER / 'job.json').write_text(json.dumps(job))
    report = json.loads(run('--transfer', XFER / 'job.json', *backend).stdout)
    assert len(report['maps']) == 2 and report['exact_triangles'] == report['target_triangles'] == 128, report
    w, h, bpp, alb = pixels8(XFER / out_dir / 'cards_ALB.png')
    _, _, _, rgh = pixels8(XFER / out_dir / 'cards_RGH.png')
    assert (w, h, bpp) == (W, W, 3)
    for y in range(W):
        for x in range(W):
            sx = W - 1 - x
            assert abs(alb[y][x*3] - (sx * 4 + 2)) <= 1 and abs(alb[y][x*3+1] - (y * 4 + 1)) <= 1, (out_dir, x, y)
            assert abs(rgh[y][x*3] - (255 - sx * 4)) <= 1, (out_dir, x, y)
failure = run('--transfer', XFER / 'job.json', '--reference', ok=False)
assert 'already exist' in failure.stderr
run('--transfer', XFER / 'job.json', '--reference', '--overwrite')
print('PASS: texture transfer job from JSON; exact face matching on mirrored UVs is lossless on GPU and CPU; '
      'existing outputs are protected unless --overwrite is given.')
print('PASS: FBX/OBJ import; independent UV sets; PNG/TGA/TIFF parity; transformed normals; CPU/GPU parity; overlap/missing-UV rejection; Unicode paths; transparent-mask white output; linear 16-bit PNG.')
