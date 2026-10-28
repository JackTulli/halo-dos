"""hook.py: helpers to carry the DOS port's hooks (HALO_DOS) into the game's
sources (the chupathingyce submodule), whose code around them differs from
the tree they were first written in. Every helper does nothing when its hook
is there already."""

import re
from pathlib import Path

GAME = Path(__file__).resolve().parent.parent / "chupathingyce"


def read(path):
    return (GAME / path).read_text(encoding="utf-8", errors="surrogateescape")


def write(path, text):
    (GAME / path).write_text(text, encoding="utf-8", errors="surrogateescape", newline="\n")


def at_function_start(path, name, code):
    """code right after the opening brace of the function named name (a
    definition at the start of a line); nothing when the body already
    starts with a HALO_DOS block"""
    text = read(path)
    pattern = r"^[A-Za-z_][^\n;]*\b" + re.escape(name) + r"\([^;{]*?\)[ \t]*\n\{\n"
    match = re.search(pattern, text, re.M)
    if not match:
        raise SystemExit(f"{path}: no definition of {name}")
    if text[match.end():].startswith("#ifdef HALO_DOS"):
        return False
    write(path, text[:match.end()] + code.rstrip("\n") + "\n" + text[match.end():])
    return True


def replace(path, old, new, count=1):
    """old (count times) by new; nothing when new is there and old is not"""
    text = read(path)
    if new in text:
        return False
    if text.count(old) != count:
        raise SystemExit(f"{path}: {text.count(old)} of {old[:70]!r}")
    write(path, text.replace(old, new))
    return True


def insert_before(path, anchor, code):
    """code before anchor (once); nothing when code is there"""
    text = read(path)
    if code in text:
        return False
    if text.count(anchor) != 1:
        raise SystemExit(f"{path}: {text.count(anchor)} of {anchor[:70]!r}")
    write(path, text.replace(anchor, code + anchor))
    return True


def insert_after(path, anchor, code):
    """code after anchor (once); nothing when code is there"""
    text = read(path)
    if code in text:
        return False
    if text.count(anchor) != 1:
        raise SystemExit(f"{path}: {text.count(anchor)} of {anchor[:70]!r}")
    write(path, text.replace(anchor, anchor + code))
    return True


def append(path, code):
    """code at the end of the file; nothing when code is there"""
    text = read(path)
    if code in text:
        return False
    write(path, text.rstrip("\n") + "\n" + code)
    return True
