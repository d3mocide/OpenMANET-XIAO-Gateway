#!/usr/bin/env python3
"""Shrink web_ui.html for embedding, without touching the source file.

web_ui.c serves the page straight out of flash: main/CMakeLists.txt embeds it
with target_add_binary_data(), so every byte of the file - comments and
indentation included - occupies a byte of the 3 MB app partition. The C sources
cost nothing to comment (the preprocessor discards those before codegen), but
this one asset is copied in verbatim, so it is the only place in the repo where
a comment has a size.

The fix is a build step rather than a leaner source file: web_ui.html keeps its
comments, and only the embedded copy is stripped. Run by CMake; the output goes
to the build directory and is never committed.

WHY THIS IS DELIBERATELY A WEAK MINIFIER
----------------------------------------
It only removes comments that *start their own line*, and only leading
indentation. It never joins lines, renames anything, or edits code.

That restraint is what makes it safe without a JS parser. Two constructs in
web_ui.html would break under a naive comment stripper:

  - regex literals - `text.replace(/<[^>]*>/g, '')` appears twice. A scanner
    that treats `/` as a possible comment start mangles these.
  - the bundled SHA-256/HMAC needed for auth (see design/ROADMAP.md), which
    must keep working byte-for-byte on a device where crypto.subtle is
    unavailable.

Every comment in web_ui.html currently begins a line, and every regex literal
has code before it, so "line-initial only" separates the two cases exactly. If
that ever stops being true, this script fails loudly rather than guessing - see
the mid-line checks below. Newlines are always preserved so JS automatic
semicolon insertion behaves identically to the unminified source.

GZIP
----
With --gzip the minified result is deflate-compressed before it is written, and
web_ui.c serves it with `Content-Encoding: gzip`. The two stages compound
rather than compete: measured on the current file, minify+gzip is 10,665 bytes
against 12,245 for gzip alone, so stripping comments first still earns its
place - and web_ui.html keeps its comments either way.

mtime is pinned to 0 in the gzip header. The default is "now", which would put
a fresh timestamp in the embedded asset on every build and churn the firmware
binary even when nothing changed.

Usage: minify_web_ui.py [--gzip] <input.html> <output>
"""

import gzip
import io
import re
import sys


class Unsafe(Exception):
    """Raised when the input contains something this script can't safely judge."""


def _strip_strings(line):
    """Blank out quoted spans so comment probes can't fire inside a literal."""
    return re.sub(r"""'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*\"""", "''", line)


def minify(text):
    out = []
    in_block = False        # inside a /* ... */ that began a line
    in_html_comment = False # inside a <!-- ... --> that began a line
    in_pre = False          # inside <pre>: whitespace is significant there

    for lineno, raw in enumerate(text.split("\n"), 1):
        line = raw

        if in_block:
            end = line.find("*/")
            if end == -1:
                continue                      # whole line is comment body
            rest = line[end + 2:]
            in_block = False
            if not rest.strip():
                continue
            line = rest

        if in_html_comment:
            end = line.find("-->")
            if end == -1:
                continue
            rest = line[end + 3:]
            in_html_comment = False
            if not rest.strip():
                continue
            line = rest

        stripped = line.strip()

        # <pre> content is served as-is; leave it byte-for-byte alone. The log
        # view is empty in the source today, but that is not guaranteed.
        if in_pre:
            out.append(raw)
            if "</pre>" in stripped:
                in_pre = False
            continue
        if "<pre" in stripped and "</pre>" not in stripped:
            in_pre = True
            out.append(line.lstrip())
            continue

        if not stripped:
            continue                          # blank line

        # Line-initial comments: safe to drop entirely.
        if stripped.startswith("//"):
            continue
        if stripped.startswith("<!--"):
            if "-->" in stripped:
                rest = stripped.split("-->", 1)[1]
                if not rest.strip():
                    continue
                line = rest
            else:
                in_html_comment = True
                continue
        elif stripped.startswith("/*"):
            end = stripped.find("*/", 2)
            if end == -1:
                in_block = True
                continue
            rest = stripped[end + 2:]
            if not rest.strip():
                continue
            line = rest

        # Anything comment-like that is NOT line-initial means the assumption
        # this script rests on no longer holds. Refuse rather than risk the
        # regex literals or the bundled crypto.
        probe = _strip_strings(line)
        # A regex literal or a URL can contain "//" legitimately, so only the
        # unambiguous block-comment opener is treated as fatal here.
        if "/*" in probe:
            raise Unsafe(
                f"line {lineno}: mid-line '/*' found. This minifier only handles "
                f"comments that start a line - see the module docstring.\n  {raw.strip()}"
            )

        out.append(line.lstrip())

    if in_block or in_html_comment:
        raise Unsafe("unterminated comment at end of file")

    return "\n".join(out) + "\n"


def _gzip_bytes(data):
    """Deterministic gzip: same input, byte-identical output, every build."""
    buf = io.BytesIO()
    # mtime=0 rather than the default "now" - see the module docstring.
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as gz:
        gz.write(data)
    return buf.getvalue()


def main():
    args = sys.argv[1:]
    use_gzip = False
    if "--gzip" in args:
        use_gzip = True
        args.remove("--gzip")
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = args

    with open(src, "r", encoding="utf-8") as f:
        text = f.read()

    try:
        result = minify(text)
    except Unsafe as exc:
        print(f"minify_web_ui.py: {exc}", file=sys.stderr)
        return 1

    payload = result.encode()
    minified = len(payload)
    if use_gzip:
        payload = _gzip_bytes(payload)

    with open(dst, "wb") as f:
        f.write(payload)

    before, after = len(text.encode()), len(payload)
    detail = f" (minified {minified}, then gzipped)" if use_gzip else ""
    print(f"web_ui.html: {before} -> {after} bytes embedded{detail} "
          f"({before - after} saved, {100.0 * (before - after) / before:.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
