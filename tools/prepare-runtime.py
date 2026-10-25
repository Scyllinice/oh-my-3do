#!/usr/bin/env python3
"""Verify and inventory the complete pinned SDK bootstrap runtime."""
import hashlib
import json
import os
from pathlib import Path
import sys


def template_files(sdk):
    return sorted(
        [sdk / "AppStartup", sdk / "BannerScreen"]
        + [path for path in (sdk / "System").rglob("*") if path.is_file()]
    )


def prepare(stage, audit):
    stage = stage.resolve()
    sdk = Path(os.environ["TDO_DEVKIT_PATH"]) / "takeme"
    sources = template_files(sdk)
    expected = {path.relative_to(sdk).as_posix() for path in sources}
    actual = {path.relative_to(stage).as_posix() for path in stage.rglob("*") if path.is_file()}
    if actual != expected:
        raise ValueError(
            f"SDK runtime differs: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}"
        )
    inventory = []
    for source in sources:
        name = source.relative_to(sdk).as_posix()
        target = stage / name
        if target.is_symlink() or not target.resolve().is_relative_to(stage):
            raise ValueError(f"Unsafe SDK runtime component: {name}")
        data = target.read_bytes()
        if data != source.read_bytes():
            raise ValueError(f"Modified SDK runtime component: {name}")
        inventory.append(
            {
                "path": name,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "origin": "3do-devkit/takeme/" + name,
                "redistribution_permission": "unresolved",
            }
        )
    audit.write_text(
        json.dumps(
            {
                "runtime_policy": "complete SDK bootstrap template",
                "sdk_components": inventory,
                "removed_files": [],
                "project_startup_scripts": [],
            },
            indent=2,
        )
        + "\n"
    )
    print(
        f"Runtime: verified all {len(inventory)} SDK bootstrap files; no pruning or startup replacements"
    )


if __name__ == "__main__":
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))
