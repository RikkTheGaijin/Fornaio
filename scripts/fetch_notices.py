"""Retrieve the exact Qt corresponding-source archives and collect license notices."""
import concurrent.futures
import json
import pathlib
import shutil
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
LICENSES = ROOT / 'licenses'
LICENSES.mkdir(exist_ok=True)
DEST = ROOT / 'dist/qt-sources'
DEST.mkdir(parents=True, exist_ok=True)

def qt(module):
    name = f'{module}-everywhere-src-6.8.3.tar.xz'
    path = DEST / name
    if not path.exists():
        urllib.request.urlretrieve(f'https://download.qt.io/archive/qt/6.8/6.8.3/submodules/{name}', path)
    notices = []
    with tarfile.open(path) as archive:
        for entry in archive.getmembers():
            base = pathlib.PurePosixPath(entry.name).name.lower()
            if entry.isfile() and ('/LICENSES/' in entry.name or base.startswith(('license', 'copying')) or base == 'qt_attribution.json'):
                data = archive.extractfile(entry).read().decode('utf-8', errors='replace')
                notices.append(f'\n\n--- {entry.name} ---\n\n{data}')
    (LICENSES / f'{module}-licenses-and-attributions.txt').write_text(''.join(notices), encoding='utf-8')
    shutil.copy2(ROOT / f'.deps/Qt/6.8.3/msvc2022_64/sbom/{module}-6.8.3.spdx.json', LICENSES)
    return name

with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    for name in pool.map(qt, ['qtbase', 'qtimageformats']):
        print(name)
for source, name in [('third_party/ufbx/LICENSE', 'ufbx-MIT.txt'), ('third_party/stb/LICENSE', 'stb-MIT-or-public-domain.txt'), ('third_party/optix/LICENSE.txt', 'NVIDIA-OptiX.txt')]:
    shutil.copy2(ROOT / source, LICENSES / name)
shutil.copy2(pathlib.Path(r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\EULA.txt'), LICENSES / 'NVIDIA-CUDA-EULA.txt')
print('Notices and Qt corresponding source archives ready.')
