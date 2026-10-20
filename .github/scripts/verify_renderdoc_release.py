#!/usr/bin/env python3
"""Verify a built RenderDoc Python module release directory."""

import argparse
import os
from pathlib import Path
import re
import sys


def github_output(key, value):
    output_path = os.environ.get("GITHUB_OUTPUT")
    if output_path:
        with open(output_path, "a", encoding="utf-8") as output:
            output.write(f"{key}={value}\n")
    else:
        print(f"{key}={value}")


def find_release_dir(release_root, renderdoc_tag, python_minor, platform):
    escaped_tag = re.escape(renderdoc_tag)
    escaped_python = re.escape(python_minor)
    escaped_platform = re.escape(platform.lower())
    pattern = re.compile(
        rf"^{escaped_tag}_py{escaped_python}\.\d+_{escaped_platform}$"
    )
    candidates = [
        path for path in Path(release_root).iterdir()
        if path.is_dir() and pattern.match(path.name)
    ]
    if not candidates:
        raise FileNotFoundError(
            f"No release directory under {release_root} for "
            f"{renderdoc_tag} Python {python_minor} {platform}"
        )
    return sorted(candidates)[-1]


def require_file(path):
    if not path.is_file():
        raise FileNotFoundError(f"Missing required file: {path}")


def require_stubs(release_dir):
    renderdoc_stubs = list((release_dir / "stubs" / "renderdoc").glob("*.py"))
    qrenderdoc_stubs = list((release_dir / "stubs" / "qrenderdoc").glob("*.py"))
    if not renderdoc_stubs:
        raise FileNotFoundError("Missing renderdoc stub files")
    if not qrenderdoc_stubs:
        raise FileNotFoundError("Missing qrenderdoc stub files")
    print(f"renderdoc stubs: {len(renderdoc_stubs)}")
    print(f"qrenderdoc stubs: {len(qrenderdoc_stubs)}")


def verify_imports(release_dir, expected_version):
    release_dir_str = str(release_dir.resolve())
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(release_dir_str)
    os.environ["PATH"] = os.pathsep.join([release_dir_str, os.environ.get("PATH", "")])
    sys.path.insert(0, release_dir_str)

    import renderdoc
    import qrenderdoc  # noqa: F401

    if not hasattr(renderdoc, "GetVersionString"):
        raise AttributeError("renderdoc.GetVersionString is missing")
    version = str(renderdoc.GetVersionString())
    print(f"RenderDoc version: {version}")
    if expected_version not in version:
        raise RuntimeError(
            f"RenderDoc version mismatch: expected {expected_version}, got {version}"
        )
    return version


def main():
    parser = argparse.ArgumentParser(description="Verify RenderDoc Python release output.")
    parser.add_argument("--release-root", required=True)
    parser.add_argument("--renderdoc-tag", required=True)
    parser.add_argument("--python-minor", default="3.13")
    parser.add_argument("--platform", default="x64")
    args = parser.parse_args()

    release_dir = find_release_dir(
        args.release_root,
        args.renderdoc_tag,
        args.python_minor,
        args.platform,
    )
    print(f"Release directory: {release_dir}")

    for filename in ["renderdoc.pyd", "qrenderdoc.pyd", "renderdoc.dll", "d3dcompiler_47.dll"]:
        require_file(release_dir / filename)
    require_stubs(release_dir)

    expected_version = args.renderdoc_tag.removeprefix("v")
    version = verify_imports(release_dir, expected_version)

    github_output("release_dir", str(release_dir.resolve()))
    github_output("version_string", version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
