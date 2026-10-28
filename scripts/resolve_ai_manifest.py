"""Release-time resolver. Writes only metadata; never downloads model weights.

Run with the maintainer's Python/pip. Users receive prebuilt archives and never run pip.
The resulting manifest is bundled with Fornaio, not a floating online dependency list.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import urllib.request

PINS = ["torch==2.9.1+cu128", "diffusers==0.42.0", "transformers==5.20.2",
        "accelerate==1.15.0", "Pillow==12.2.0"]
MODEL = "briaai/Fibo-2.0-edit-turbo"  # gated: accept its license, then set HF_TOKEN or log in with the HF CLI
ID_PREFIX = "fibo2-"
MODEL_PARTS = ("transformer/", "text_encoder/", "processor/", "vae/", "scheduler/", "fonts/")


def hf_token():
    import os
    token = os.environ.get("HF_TOKEN", "").strip()
    if not token:
        path = pathlib.Path(os.environ.get("HF_HOME", pathlib.Path.home() / ".cache" / "huggingface")) / "token"
        token = path.read_text(encoding="utf-8").strip() if path.exists() else ""
    return token


def supplement_licenses(manifest):
    # The tokenizers 0.23.3 Windows wheel omits its upstream license file.
    url = "https://raw.githubusercontent.com/huggingface/tokenizers/v0.23.3/LICENSE"
    with urllib.request.urlopen(url, timeout=60) as f:
        data = f.read()
    path = "runtime/Lib/site-packages/tokenizers/LICENSE"
    manifest["files"] = [f for f in manifest["files"] if f["path"] != path]
    manifest["files"].append({"path": path, "url": url, "bytes": len(data),
                              "sha256": hashlib.sha256(data).hexdigest(), "component": "runtime"})
    manifest.pop("id", None)
    canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    manifest["id"] = ID_PREFIX + hashlib.sha256(canonical).hexdigest()[:16]
    return manifest


def hf_request(url, method="GET"):
    headers = {"User-Agent": "Fornaio-release"}
    if "huggingface.co" in url and hf_token():
        headers["Authorization"] = "Bearer " + hf_token()
    return urllib.request.Request(url, headers=headers, method=method)


def get_json(url):
    with urllib.request.urlopen(hf_request(url), timeout=60) as f:
        return json.load(f)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--out", default="ai/manifest.json")
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fornaio-resolve-") as temp:
        report = pathlib.Path(temp) / "report.json"
        subprocess.run([sys.executable, "-m", "pip", "install", "--dry-run", "--ignore-installed",
                        "--only-binary=:all:", "--platform", "win_amd64", "--python-version", "3.12",
                        "--implementation", "cp", "--abi", "cp312", "--report", str(report),
                        "--extra-index-url", "https://download.pytorch.org/whl/cu128", *PINS], check=True)
        resolved = json.loads(report.read_text(encoding="utf-8"))["install"]
    files = []
    # Python's official embeddable distribution is a prebuilt ZIP, not an installer.
    python_url = "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip"
    with urllib.request.urlopen(python_url, timeout=60) as f:
        data = f.read()
    files.append({"path": "runtime/python.zip", "url": python_url, "bytes": len(data),
                  "sha256": hashlib.sha256(data).hexdigest(), "extract": "runtime", "component": "runtime"})
    versions = {}
    for item in resolved:
        info = item["download_info"]
        url = info["url"]
        request = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(request, timeout=60) as f:
            size = int(f.headers["Content-Length"])
        name = item["metadata"]["name"]
        versions[name] = item["metadata"]["version"]
        files.append({"path": "runtime/" + url.split("/")[-1].split("?")[0], "url": url,
                      "bytes": size, "sha256": info["archive_info"]["hashes"]["sha256"],
                      "extract": "runtime/Lib/site-packages", "component": "runtime"})
    model = get_json("https://huggingface.co/api/models/" + MODEL + "?blobs=true")
    revision = model["sha"]
    for item in model["siblings"]:
        name = item["rfilename"]
        if not (name.startswith(MODEL_PARTS) or name in ("model_index.json", "LICENSE.md", "README.md")):
            continue
        url = "https://huggingface.co/" + MODEL + "/resolve/" + revision + "/" + name
        size = item.get("size", 0)
        sha = item.get("lfs", {}).get("sha256")
        if not sha:
            with urllib.request.urlopen(hf_request(url), timeout=60) as f:
                data = f.read()
            size, sha = len(data), hashlib.sha256(data).hexdigest()
        files.append({"path": "model/" + name, "url": url, "bytes": size,
                      "sha256": sha, "component": "model"})
    manifest = {"format": "FornaioAI", "version": 1, "id": ID_PREFIX + "win64-1",
                "model": MODEL, "revision": revision, "python": "runtime/python.exe",
                "minDriver": 590, "minimumVramMiB": 12000,
                "versions": versions, "files": files}
    supplement_licenses(manifest)
    manifest.pop("id", None)
    manifest["id"] = ID_PREFIX + hashlib.sha256(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()).hexdigest()[:16]
    out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Manifest: {len(files)} files, {sum(f['bytes'] for f in files)/1e9:.2f} GB -> {out}")


if __name__ == "__main__":
    main()
