#!/usr/bin/env python3
"""Fail-closed changed-file routing for the example CI workflow.

This is a deliberately narrow CI selector.  It is not a replacement for the
repository Markdown audit, which also checks ownership, language links, and
public-text policy.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import subprocess
import sys
from pathlib import Path, PurePosixPath


IDF_ROOT = "examples/esp-idf/"
ARDUINO_ROOT = "examples/arduino/examples/"
ARDUINO_LIBRARIES = "examples/arduino/libraries/"
FIRMWARE_ROOT = "Firmware/"
GLOBAL_PREFIXES = (".github/", "scripts/", "tests/", "releases/")
CONFIG_KEYS = {
    "build_override_patterns",
    "documentation_patterns",
    "documentation_asset_patterns",
    "ignore_build_patterns",
    "firmware_patterns",
    "esp_idf_shared_patterns",
    "arduino_shared_patterns",
    "esp_idf_global_patterns",
    "arduino_global_patterns",
    "global_build_patterns",
}


class InputError(RuntimeError):
    """The complete changed-file input is unavailable or empty."""


def _normalize_path(value: str) -> str:
    path = value.replace("\\", "/").removeprefix("./")
    pure = PurePosixPath(path)
    if not path or pure.is_absolute() or ".." in pure.parts:
        raise InputError(f"unsafe changed path: {value!r}")
    return pure.as_posix()


def _git_paths(repo: Path, base: str, head: str) -> list[str]:
    try:
        result = subprocess.run(
            ["git", "diff", "--name-status", "-z", "--find-renames", "--find-copies", base, head],
            cwd=repo,
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise InputError(f"cannot read complete diff {base}..{head}") from exc

    fields = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")
    paths: list[str] = []
    index = 0
    while index < len(fields) - 1:
        status = fields[index]
        index += 1
        if not status:
            continue
        if status[0] in {"R", "C"}:
            if index + 1 >= len(fields):
                raise InputError("incomplete rename/copy record")
            paths.extend((fields[index], fields[index + 1]))
            index += 2
        else:
            if index >= len(fields):
                raise InputError("incomplete changed-file record")
            paths.append(fields[index])
            index += 1
    if not paths:
        raise InputError("changed-file set is empty")
    return [_normalize_path(path) for path in paths]


def _paths_from_file(path: Path) -> list[str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise InputError(f"cannot read changed-file list {path}") from exc

    paths: list[str] = []
    for raw_line in lines:
        line = raw_line.rstrip("\r")
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) == 1:
            paths.append(fields[0])
            continue
        status = fields[0]
        if status.startswith(("R", "C")):
            if len(fields) != 3:
                raise InputError(f"incomplete rename/copy record: {line!r}")
            paths.extend(fields[1:])
        elif status[:1] in {"A", "D", "M", "T", "U", "X", "B"}:
            if len(fields) != 2:
                raise InputError(f"invalid changed-file record: {line!r}")
            paths.append(fields[1])
        else:
            raise InputError(f"unknown changed-file status: {status!r}")

    if not paths:
        raise InputError("changed-file set is empty")
    return [_normalize_path(item) for item in paths]


def _project(path: str, root: str) -> str | None:
    if not path.startswith(root):
        return None
    remainder = path[len(root) :]
    parts = remainder.split("/")
    return f"{root}{parts[0]}" if parts and parts[0] else None


def _result(mode: str, selected: set[str]) -> dict[str, object]:
    return {"mode": mode, "selectors": sorted(selected) if mode == "selected" else []}


def _matches(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def _load_config(path: Path) -> dict[str, list[str]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise InputError(f"cannot read routing config {path}") from exc
    if not isinstance(data, dict) or set(data) - CONFIG_KEYS or any(
        not isinstance(value, list) or not all(isinstance(item, str) for item in value)
        for value in data.values()
    ):
        raise InputError(f"invalid routing config {path}")
    return data


def _is_document(path: str, config: dict[str, list[str]]) -> bool:
    lower = path.lower()
    return (
        lower.endswith((".md", ".mdx", ".rst"))
        or _matches(path, config.get("documentation_patterns", []))
        or _matches(path, config.get("documentation_asset_patterns", []))
        or _matches(path, config.get("ignore_build_patterns", []))
    )


def classify(
    paths: list[str], config: dict[str, list[str]], force_all: bool = False
) -> dict[str, object]:
    idf: set[str] = set()
    arduino: set[str] = set()
    idf_all = force_all
    arduino_all = force_all
    firmware: list[str] = []
    release_artifacts: list[str] = []
    unknown: list[str] = []
    docs_only = bool(paths) and not force_all

    for path in paths:
        lower = path.lower()
        if path.startswith(FIRMWARE_ROOT) or _matches(
            path, config.get("firmware_patterns", [])
        ):
            firmware.append(path)
            if lower.endswith((".bin", ".zip", ".tar", ".tar.gz", ".tgz")):
                release_artifacts.append(path)
            if not _is_document(path, config):
                docs_only = False
            continue
        if _matches(path, config.get("build_override_patterns", [])):
            docs_only = False
            idf_all = True
            arduino_all = True
            continue
        if _is_document(path, config):
            continue
        if _matches(path, config.get("global_build_patterns", [])) or path.startswith(
            GLOBAL_PREFIXES
        ):
            docs_only = False
            idf_all = True
            arduino_all = True
            continue
        if _matches(path, config.get("esp_idf_global_patterns", [])):
            docs_only = False
            idf_all = True
            continue
        if _matches(path, config.get("arduino_global_patterns", [])):
            docs_only = False
            arduino_all = True
            continue
        if _matches(path, config.get("esp_idf_shared_patterns", [])):
            docs_only = False
            idf_all = True
            continue
        if _matches(path, config.get("arduino_shared_patterns", [])) or path.startswith(
            ARDUINO_LIBRARIES
        ):
            docs_only = False
            arduino_all = True
            continue
        project = _project(path, IDF_ROOT)
        if project:
            docs_only = False
            idf.add(project)
            continue
        sketch = _project(path, ARDUINO_ROOT)
        if sketch:
            docs_only = False
            arduino.add(sketch)
            continue
        docs_only = False
        unknown.append(path)
        idf_all = True
        arduino_all = True

    return {
        "esp_idf": _result("all" if idf_all else "selected" if idf else "none", idf),
        "arduino": _result("all" if arduino_all else "selected" if arduino else "none", arduino),
        "firmware_paths": sorted(set(firmware)),
        "release_artifact_paths": sorted(set(release_artifacts)),
        "unknown_paths": sorted(set(unknown)),
        "docs_only": docs_only,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Classify a complete CI changed-file set.")
    parser.add_argument("--repo", default=".")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--base")
    mode.add_argument("--changed-files-from")
    mode.add_argument("--all", action="store_true", help="Route all examples for manual dispatch.")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--config", default="ci-routing-config.json")
    parser.add_argument("--github-output")
    args = parser.parse_args()
    repo = Path(args.repo).resolve()
    try:
        if args.all:
            paths: list[str] = []
        elif args.changed_files_from:
            paths = _paths_from_file(Path(args.changed_files_from).resolve())
        else:
            paths = _git_paths(repo, args.base, args.head)
        result = classify(paths, _load_config(Path(args.config).resolve()), force_all=args.all)
    except InputError as exc:
        print(f"classifier input error: {exc}", file=sys.stderr)
        return 2

    output = json.dumps(result, separators=(",", ":"))
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as handle:
            handle.write(f"routing={output}\n")
            handle.write(f"idf_mode={result['esp_idf']['mode']}\n")
            handle.write(f"idf_selector={','.join(result['esp_idf']['selectors']) or 'all'}\n")
            handle.write(f"arduino_mode={result['arduino']['mode']}\n")
            handle.write(f"arduino_selector={','.join(result['arduino']['selectors']) or 'all'}\n")
            handle.write(f"docs_only={str(result['docs_only']).lower()}\n")
            handle.write(f"firmware_changed={str(bool(result['firmware_paths'])).lower()}\n")
            handle.write(f"release_review={str(bool(result['release_artifact_paths'])).lower()}\n")
    else:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
