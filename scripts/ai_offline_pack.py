"""Create an optional, verified offline AI pack from a completed download cache.

This output is deliberately separate from Fornaio's application/source packages.
The user imports its manifest.json; no system Python is needed to use it.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import zipfile


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--cache", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", default="ai/manifest.json", type=pathlib.Path)
    parser.add_argument("--licenses-only", action="store_true", help="Extract an audit folder without copying large archives")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if (args.out.resolve() == args.cache.resolve() or
            args.cache.resolve() in args.out.resolve().parents):
        parser.error("The offline pack must be outside the download cache")
    objects = args.out / "objects"
    objects.mkdir(parents=True, exist_ok=True)
    for item in manifest["files"]:
        source = args.cache / "objects" / item["sha256"]
        digest = hashlib.sha256()
        with source.open("rb") as f:
            while block := f.read(8 * 1024 * 1024):
                digest.update(block)
        if source.stat().st_size != item["bytes"] or digest.hexdigest() != item["sha256"]:
            raise ValueError("Invalid cached component: " + item["path"])
        target = objects / item["sha256"]
        if not args.licenses_only and not target.exists():
            shutil.copyfile(source, target)
        if item.get("extract"):
            with zipfile.ZipFile(source) as archive:
                for name in archive.namelist():
                    if name.endswith("/") or not any(word in name.lower() for word in ("license", "notice", "copying")):
                        continue
                    relative = pathlib.PurePosixPath(name)
                    if relative.is_absolute() or ".." in relative.parts:
                        raise ValueError("Unsafe archive license path")
                    notice = args.out / "licenses" / pathlib.Path(item["path"]).name / pathlib.Path(*relative.parts)
                    notice.parent.mkdir(parents=True, exist_ok=True)
                    notice.write_bytes(archive.read(name))
        elif any(word in item["path"].lower() for word in ("license", "notice", "copying")):
            notice = args.out / "licenses" / pathlib.Path(item["path"]).name
            notice.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, notice)
        print(item["path"])
    shutil.copyfile(args.manifest, args.out / "manifest.json")
    shutil.copyfile(pathlib.Path(__file__).resolve().parents[1] / "ai/AI_NOTICES.md", args.out / "AI_NOTICES.md")
    print("License audit ready:" if args.licenses_only else "Offline pack ready:", args.out)


if __name__ == "__main__":
    main()
