#!/usr/bin/env python3
"""Discover RenderDoc release tags that still need Python module assets."""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request


TAG_RE = re.compile(r"refs/tags/(v1\.(\d+))(?:\^\{\})?$")


def run_git_ls_remote(repo_url):
    result = subprocess.run(
        ["git", "ls-remote", "--tags", repo_url, "v1.*"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(
            "git ls-remote failed with exit code "
            f"{result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout.splitlines()


def discover_tags(repo_url, min_minor):
    tags = {}
    for line in run_git_ls_remote(repo_url):
        parts = line.split()
        if len(parts) != 2:
            continue
        match = TAG_RE.search(parts[1])
        if not match:
            continue
        tag = match.group(1)
        minor = int(match.group(2))
        if minor >= min_minor:
            tags[tag] = minor
    return [tag for tag, _ in sorted(tags.items(), key=lambda item: item[1])]


def github_api_json(path, token):
    request = urllib.request.Request(
        f"https://api.github.com/{path}",
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "renderdoc-pythonmodules-ci",
            **({"Authorization": f"Bearer {token}"} if token else {}),
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            return None
        raise


def release_has_asset(owner_repo, release_tag, asset_name, token):
    release = github_api_json(f"repos/{owner_repo}/releases/tags/{release_tag}", token)
    if not release:
        return False
    return any(asset.get("name") == asset_name for asset in release.get("assets", []))


def write_github_output(values):
    output_path = os.environ.get("GITHUB_OUTPUT")
    if not output_path:
        for key, value in values.items():
            print(f"{key}={value}")
        return
    with open(output_path, "a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def main():
    parser = argparse.ArgumentParser(
        description="Find RenderDoc tags that do not have Python module release assets."
    )
    parser.add_argument("--renderdoc-repo", default="https://github.com/baldurk/renderdoc.git")
    parser.add_argument("--owner-repo", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--min-minor", type=int, default=42)
    parser.add_argument("--python-minor", default="3.13")
    parser.add_argument("--platform", default="x64")
    args = parser.parse_args()

    if not args.owner_repo:
        print("owner repo is required via --owner-repo or GITHUB_REPOSITORY", file=sys.stderr)
        return 2

    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    include = []
    for tag in discover_tags(args.renderdoc_repo, args.min_minor):
        release_tag = f"renderdoc-python-{tag}"
        asset_name = f"renderdoc-python-{tag}-py{args.python_minor}-{args.platform}.zip"
        if release_has_asset(args.owner_repo, release_tag, asset_name, token):
            print(f"[skip] {tag}: {asset_name} already exists on {release_tag}")
            continue
        print(f"[build] {tag}: missing {asset_name}")
        include.append(
            {
                "renderdoc_tag": tag,
                "release_tag": release_tag,
                "asset_name": asset_name,
            }
        )

    matrix = json.dumps({"include": include}, separators=(",", ":"))
    write_github_output({"matrix": matrix, "count": str(len(include))})
    print(f"Discovered {len(include)} RenderDoc tag(s) requiring builds.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
