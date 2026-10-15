#!/usr/bin/env python3
"""Reduce an SDK template to the reviewed runtime files before adding our assets."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent.parent


def prepare(stage, audit):
    stage = stage.resolve()
    paths = [line for line in (ROOT / "runtime/sdk-files.txt").read_text().splitlines()
             if line and not line.startswith("#")]
    # Validate the complete selection before modifying the temporary build stage.
    for name in paths:
        path = stage / name
        if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(stage):
            raise ValueError(f"Missing or unsafe SDK runtime component: {name}")
    inventory = []
    removed = []
    for path in sorted(stage.rglob("*")):
        if not path.is_file():
            continue
        name = path.relative_to(stage).as_posix()
        if name not in paths:
            removed.append(name)
            path.unlink()
        else:
            inventory.append({"path": name, "bytes": path.stat().st_size,
                              "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                              "origin": "3do-devkit/takeme/" + name,
                              "redistribution_permission": "unresolved"})
    for name in ("AppStartup", "System/Scripts/startopera"):
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "runtime" / name, target)
    # Remove empty directories left by omitted examples and optional services.
    for path in sorted(stage.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if path.is_dir() and not any(path.iterdir()):
            path.rmdir()
    audit.write_text(json.dumps({"sdk_components": inventory, "removed_files": removed,
                                "project_startup_scripts": ["AppStartup", "System/Scripts/startopera"]},
                               indent=2) + "\n")
    print(f"Runtime: retained {len(paths)} SDK runtime files; replaced startup scripts; removed {len(removed)} template files")


if __name__ == "__main__":
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))
