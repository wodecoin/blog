#!/usr/bin/env python3
"""
@file gen_blog.py
@brief Generate BLOG IDs and the PC-side log dictionary.

Scans source files for:
    BLOG_D(TAG, "format", ...)
    BLOG_I(TAG, "format", ...)
    BLOG_W(TAG, "format", ...)
    BLOG_E(TAG, "format", ...)

Outputs:
    blog_ids.h
    log_dict.json

V1 checks:
    - duplicate TAG conflicts
    - 16-bit ID collisions
    - printf argument count mismatches
    - unsupported printf conversions
    - more than four runtime arguments
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

BLOG_RE = re.compile(r"\bBLOG_([DIWE])\s*\(")

LEVELS = {
    "D": "DEBUG",
    "I": "INFO",
    "W": "WARN",
    "E": "ERROR",
}

SOURCE_SUFFIXES = {
    ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp",
}

SKIP_DIRS = {
    ".git", ".svn", ".hg",
    "build", "out",
    ".venv", "venv",
    "__pycache__",
}

PRINTF_RE = re.compile(
    r"""
    %
    (?:
        % |
        [-+ #0]*
        (?:\d+)?
        (?:\.\d+)?
        (?:hh|h|l|ll|z|t|j)?
        ([diuxXc])
    )
    """,
    re.VERBOSE,
)


def crc16_ccitt_false(text: str) -> int:
    """Return CRC-16/CCITT-FALSE for UTF-8 text."""
    crc = 0xFFFF

    for byte in text.encode("utf-8"):
        crc ^= byte << 8

        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF

    return crc


def line_number(text: str, offset: int) -> int:
    """Return 1-based source line number."""
    return text.count("\n", 0, offset) + 1


def is_macro_definition(text: str, offset: int) -> bool:
    """Return whether an offset lies in a #define directive."""
    line_start = text.rfind("\n", 0, offset) + 1

    while line_start > 0:
        previous_end = line_start - 1
        previous_start = text.rfind("\n", 0, previous_end) + 1
        previous_line = text[previous_start:previous_end].rstrip("\r")

        if not previous_line.rstrip().endswith("\\"):
            break

        line_start = previous_start

    line_end = text.find("\n", line_start)

    if line_end < 0:
        line_end = len(text)

    directive = text[line_start:line_end].lstrip()
    return re.match(r"#\s*define\b", directive) is not None


def skip_ws_comments(text: str, pos: int) -> int:
    """Skip whitespace and C/C++ comments."""
    n = len(text)

    while pos < n:
        if text[pos].isspace():
            pos += 1
            continue

        if text.startswith("//", pos):
            end = text.find("\n", pos + 2)
            return n if end < 0 else skip_ws_comments(text, end + 1)

        if text.startswith("/*", pos):
            end = text.find("*/", pos + 2)

            if end < 0:
                raise ValueError("unterminated block comment")

            pos = end + 2
            continue

        break

    return pos


def parse_macro_body(
    text: str,
    open_paren: int,
) -> tuple[str, int]:
    """Extract a complete BLOG(...) body."""
    depth = 1
    start = open_paren + 1
    i = start
    n = len(text)

    while i < n:
        ch = text[i]

        if ch == '"':
            i += 1

            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue

                if text[i] == '"':
                    i += 1
                    break

                i += 1

            continue

        if ch == "'":
            i += 1

            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue

                if text[i] == "'":
                    i += 1
                    break

                i += 1

            continue

        if text.startswith("//", i):
            end = text.find("\n", i + 2)
            i = n if end < 0 else end + 1
            continue

        if text.startswith("/*", i):
            end = text.find("*/", i + 2)

            if end < 0:
                raise ValueError(
                    "unterminated block comment"
                )

            i = end + 2
            continue

        if ch == "(":
            depth += 1

        elif ch == ")":
            depth -= 1

            if depth == 0:
                return text[start:i], i + 1

        i += 1

    raise ValueError("unterminated BLOG macro call")


def split_top_level_args(body: str) -> list[str]:
    """Split macro arguments on top-level commas."""
    out = []
    start = 0

    paren = 0
    brack = 0
    brace = 0

    i = 0
    n = len(body)

    while i < n:
        ch = body[i]

        if ch == '"':
            i += 1

            while i < n:
                if body[i] == "\\":
                    i += 2
                    continue

                if body[i] == '"':
                    i += 1
                    break

                i += 1

            continue

        if ch == "'":
            i += 1

            while i < n:
                if body[i] == "\\":
                    i += 2
                    continue

                if body[i] == "'":
                    i += 1
                    break

                i += 1

            continue

        if body.startswith("//", i):
            end = body.find("\n", i + 2)
            i = n if end < 0 else end + 1
            continue

        if body.startswith("/*", i):
            end = body.find("*/", i + 2)

            if end < 0:
                raise ValueError(
                    "unterminated block comment"
                )

            i = end + 2
            continue

        if ch == "(":
            paren += 1

        elif ch == ")":
            paren -= 1

        elif ch == "[":
            brack += 1

        elif ch == "]":
            brack -= 1

        elif ch == "{":
            brace += 1

        elif ch == "}":
            brace -= 1

        elif (
            ch == ","
            and paren == 0
            and brack == 0
            and brace == 0
        ):
            out.append(body[start:i].strip())
            start = i + 1

        i += 1

    tail = body[start:].strip()

    if tail:
        out.append(tail)

    return out


C_STRING_RE = re.compile(
    r'(?:u8|u|U|L)?"((?:\\.|[^"\\])*)"',
    re.DOTALL,
)


def decode_c_string_expr(token: str) -> str:
    """Decode adjacent C string literals."""
    parts = []
    pos = 0

    while True:
        pos = skip_ws_comments(token, pos)

        if pos >= len(token):
            break

        match = C_STRING_RE.match(token, pos)

        if not match:
            raise ValueError(
                "format must use C string literals"
            )

        raw = match.group(1)

        try:
            decoded = bytes(
                raw,
                "utf-8",
            ).decode("unicode_escape")

        except UnicodeDecodeError as exc:
            raise ValueError(
                f"invalid string escape: {exc}"
            ) from exc

        parts.append(decoded)
        pos = match.end()

    if not parts:
        raise ValueError("missing format string")

    return "".join(parts)


def parse_printf_specs(fmt: str) -> list[str]:
    """Return printf conversions that consume arguments."""
    specs = []
    i = 0

    while i < len(fmt):
        if fmt[i] != "%":
            i += 1
            continue

        if (
            i + 1 < len(fmt)
            and fmt[i + 1] == "%"
        ):
            i += 2
            continue

        match = PRINTF_RE.match(fmt, i)

        if not match:
            raise ValueError(
                "unsupported printf format near "
                f"{fmt[i:i + 16]!r}"
            )

        conv = match.group(1)

        if conv:
            specs.append(conv)

        i = match.end()

    return specs


def scan_file(
    path: Path,
    root: Path,
) -> list[dict]:
    """Scan one source file."""
    text = path.read_text(
        encoding="utf-8",
        errors="replace",
    )

    records = []

    for match in BLOG_RE.finditer(text):
        if is_macro_definition(text, match.start()):
            continue

        level_code = match.group(1)
        open_paren = match.end() - 1
        line = line_number(text, match.start())

        try:
            body, _ = parse_macro_body(
                text,
                open_paren,
            )

            args = split_top_level_args(body)

            if len(args) < 2:
                raise ValueError(
                    "BLOG requires TAG and format"
                )

            tag = args[0].strip()

            if not re.fullmatch(
                r"[A-Za-z_]\w*",
                tag,
            ):
                raise ValueError(
                    f"invalid TAG: {tag!r}"
                )

            fmt = decode_c_string_expr(args[1])
            call_args = args[2:]
            specs = parse_printf_specs(fmt)

            if len(call_args) != len(specs):
                raise ValueError(
                    f"format expects {len(specs)} "
                    f"argument(s), "
                    f"call provides {len(call_args)}"
                )

            if len(call_args) > 4:
                raise ValueError(
                    "V1 supports at most "
                    f"4 arguments, got "
                    f"{len(call_args)}"
                )

            records.append({
                "tag": tag,
                "level": LEVELS[level_code],
                "fmt": fmt,
                "spec": specs,
                "argc": len(call_args),
                "file": path.relative_to(
                    root
                ).as_posix(),
                "line": line,
            })

        except ValueError as exc:
            raise RuntimeError(
                f"{path}:{line}: {exc}"
            ) from exc

    return records


def iter_sources(root: Path):
    """Yield source files under root."""
    for path in root.rglob("*"):
        if not path.is_file():
            continue

        if any(
            part in SKIP_DIRS
            for part in path.parts
        ):
            continue

        if path.suffix.lower() in SOURCE_SUFFIXES:
            yield path


def merge_records(
    records: list[dict],
) -> list[dict]:
    """Validate TAGs and assign stable IDs."""
    by_tag: dict[str, dict] = {}

    for record in records:
        old = by_tag.get(record["tag"])

        if old is None:
            by_tag[record["tag"]] = record
            continue

        same = (
            old["level"] == record["level"]
            and old["fmt"] == record["fmt"]
            and old["spec"] == record["spec"]
        )

        if not same:
            raise RuntimeError(
                "BLOG TAG conflict:\n"
                f"  TAG: {record['tag']}\n"
                f"  first: "
                f"{old['file']}:{old['line']}\n"
                f"  again: "
                f"{record['file']}:{record['line']}"
            )

    merged = sorted(
        by_tag.values(),
        key=lambda item: item["tag"],
    )

    by_id: dict[int, dict] = {}

    for record in merged:
        log_id = crc16_ccitt_false(
            record["tag"]
        )

        record["id"] = log_id

        old = by_id.get(log_id)

        if old is not None:
            raise RuntimeError(
                "BLOG 16-bit ID collision:\n"
                f"  0x{log_id:04X}: "
                f"{old['tag']}\n"
                f"  0x{log_id:04X}: "
                f"{record['tag']}"
            )

        by_id[log_id] = record

    return merged


def write_header(
    path: Path,
    records: list[dict],
) -> None:
    """Generate blog_ids.h."""
    lines = [
        "/**",
        " * @file blog_ids.h",
        " * @brief Auto-generated BLOG IDs.",
        " * @warning DO NOT EDIT MANUALLY.",
        " */",
        "",
        "#ifndef BLOG_IDS_H",
        "#define BLOG_IDS_H",
        "",
    ]

    for record in records:
        lines.append(
            "#define "
            f"BLOG_ID_{record['tag']:<32} "
            f"0x{record['id']:04X}u"
        )

    lines.extend([
        "",
        "#endif /* BLOG_IDS_H */",
        "",
    ])

    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    path.write_text(
        "\n".join(lines),
        encoding="utf-8",
    )


def write_json(
    path: Path,
    records: list[dict],
) -> None:
    """Generate the PC-side log dictionary."""
    logs = {}

    for record in sorted(
        records,
        key=lambda item: item["id"],
    ):
        logs[
            f"0x{record['id']:04X}"
        ] = {
            "tag": record["tag"],
            "level": record["level"],
            "fmt": record["fmt"],
            "spec": record["spec"],
            "argc": record["argc"],
            "file": record["file"],
            "line": record["line"],
        }

    document = {
        "version": 1,
        "id_algorithm":
            "CRC16-CCITT-FALSE(tag)",
        "logs": logs,
    }

    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    path.write_text(
        json.dumps(
            document,
            ensure_ascii=False,
            indent=2,
        ) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description=(
            "Generate BLOG IDs and "
            "PC-side log dictionary."
        )
    )

    parser.add_argument(
        "root",
        nargs="?",
        default=".",
        help=(
            "source tree to scan "
            "(default: current directory)"
        ),
    )

    parser.add_argument(
        "--header",
        default="blog_ids.h",
        help="output header path",
    )

    parser.add_argument(
        "--json",
        default="log_dict.json",
        help="output dictionary path",
    )

    args = parser.parse_args()

    root = Path(args.root).resolve()
    header = Path(args.header)
    dictionary = Path(args.json)

    if not root.is_dir():
        print(
            f"error: source root not found: "
            f"{root}",
            file=sys.stderr,
        )
        return 2

    try:
        records = []

        for path in iter_sources(root):
            records.extend(
                scan_file(path, root)
            )

        records = merge_records(records)

        if not records:
            print(
                "warning: no BLOG entries found",
                file=sys.stderr,
            )

        write_header(header, records)
        write_json(dictionary, records)

    except (OSError, RuntimeError) as exc:
        print(
            f"error: {exc}",
            file=sys.stderr,
        )
        return 1

    print(
        f"BLOG: {len(records)} unique log(s)"
    )
    print(f"  header: {header}")
    print(f"  json:   {dictionary}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
