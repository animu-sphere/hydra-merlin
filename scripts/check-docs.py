#!/usr/bin/env python3
"""Check repository Markdown for cheap, mechanical documentation drift.

Checks:
  * repository-relative links in tracked Markdown resolve to Git-tracked files
    or directories, and `#anchor` fragments resolve to a heading in the target;
  * docs/roadmap/current.md contains no completed `[x]` task items.

The rules these checks enforce are described in docs/README.md.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
CURRENT_ROADMAP = Path("docs/roadmap/current.md")

FENCE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE = re.compile(r"`[^`]*`")
INLINE_LINK = re.compile(r"!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REFERENCE_DEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s+.*)?$")
HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
HTML_ANCHOR = re.compile(r"<a\s+(?:name|id)=\"([^\"]+)\"", re.IGNORECASE)
COMPLETED_TASK = re.compile(r"^\s*[-*+]\s+\[[xX]\]\s")
EXTERNAL = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")


def tracked_files() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [p for p in result.stdout.decode("utf-8").split("\0") if p]


def tracked_targets(files: list[str]) -> set[str]:
    """Tracked files plus every directory that contains one.

    Untracked and Git-ignored files (for example local-only reports) exist in a
    working copy but not in the published repository, so they are not valid
    link targets.
    """
    targets = set(files)
    for name in files:
        parent = Path(name).parent
        while parent != Path("."):
            targets.add(parent.as_posix())
            parent = parent.parent
    return targets


def iter_lines(text: str):
    """Yield (line number, line) outside fenced code blocks."""
    in_fence = False
    for number, line in enumerate(text.splitlines(), start=1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
            yield number, line


def github_slug(heading: str) -> str:
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "").lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


_anchor_cache: dict[Path, set[str]] = {}


def anchors(path: Path) -> set[str]:
    if path not in _anchor_cache:
        found: set[str] = set()
        counts: dict[str, int] = {}
        text = (ROOT / path).read_text(encoding="utf-8")
        for _, line in iter_lines(text):
            found.update(HTML_ANCHOR.findall(line))
            match = HEADING.match(line)
            if not match:
                continue
            slug = github_slug(match.group(2))
            seen = counts.get(slug, 0)
            counts[slug] = seen + 1
            found.add(slug if seen == 0 else f"{slug}-{seen}")
        _anchor_cache[path] = found
    return _anchor_cache[path]


def check_links(files: list[Path], tracked: set[str]) -> list[str]:
    errors: list[str] = []
    for source in files:
        text = (ROOT / source).read_text(encoding="utf-8")
        for number, line in iter_lines(text):
            stripped = INLINE_CODE.sub("", line)
            links = INLINE_LINK.findall(stripped)
            reference = REFERENCE_DEF.match(stripped)
            if reference:
                links.append(reference.group(1))
            for target in links:
                if EXTERNAL.match(target) or target.startswith("//"):
                    continue
                path_part, _, fragment = target.partition("#")
                if path_part:
                    resolved = (ROOT / source.parent / unquote(path_part)).resolve()
                    try:
                        relative = resolved.relative_to(ROOT)
                    except ValueError:
                        errors.append(f"{source}:{number}: link leaves the repository: {target}")
                        continue
                    if relative.as_posix() not in tracked and relative != Path("."):
                        errors.append(
                            f"{source}:{number}: link target is missing or not tracked by Git: {target}"
                        )
                        continue
                else:
                    relative = source
                if fragment and relative.suffix.lower() == ".md":
                    if unquote(fragment) not in anchors(relative):
                        errors.append(f"{source}:{number}: missing anchor: {target}")
    return errors


def check_current_roadmap() -> list[str]:
    path = ROOT / CURRENT_ROADMAP
    if not path.exists():
        return [f"{CURRENT_ROADMAP}: file is missing"]
    errors = []
    for number, line in iter_lines(path.read_text(encoding="utf-8")):
        if COMPLETED_TASK.match(line):
            errors.append(
                f"{CURRENT_ROADMAP}:{number}: completed task; remove it and record "
                "the result in the changelog/support matrix instead"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.parse_args()
    tracked = tracked_files()
    files = [Path(p) for p in tracked if p.lower().endswith(".md")]
    errors = check_links(files, tracked_targets(tracked)) + check_current_roadmap()
    for error in errors:
        print(error)
    if errors:
        print(f"check-docs: {len(errors)} problem(s) in {len(files)} Markdown files")
        return 1
    print(f"check-docs: {len(files)} Markdown files OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
