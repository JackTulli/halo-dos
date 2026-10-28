"""Checks of the launcher (port/win9x/platform/w9x_launcher.c) against
port_config.c's settings table, on the PC that builds the game.

    python3 port/win9x/tests/launcher/launcher_check.py build/win9x/tests/launcher/t_launcher.exe \
        [--config-test build/win9x/tests/launcher/t_config.exe]

The settings table is read from port_config.c as each build compiles it
(the preprocessor conditions on HALO_WIN9X and HALO_XBOX, the platform
bits), once for the 9x build and once for the native Windows build. For
each, t_launcher builds the dialog template for the settings that build
reads and prints it, and this script checks that:

 - the template's items are the layout's controls, with ids each used once;
 - every control lies inside the dialog, a page's controls inside the tab
   control, the rest outside it, and no two controls that show at the same
   time overlap;
 - the dialog fits a 600 by 440 pixel desktop at 96 dpi with the frame
   (dialog units of the 8-point dialog font: 1.5 by 1.625 pixels);
 - every setting the build reads is bound to exactly one control, or to
   the Advanced list, and nothing else is;
 - the settings with controls of their own (renderer, mode, field of
   view...) have the controls meant for them.

With --config-test, t_config (the table interface and the writer, built
with HALO_WIN9X) runs its scenes in a folder of its own, and the table it
prints is compared with the one read from the source.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
CONFIG_SOURCE = ROOT / "port/linux/src/port_config.c"

# the platform bits each build reads (port_config.c's CONFIG_PLATFORM)
BUILDS = {
    "win9x": {"defines": {"HALO_WIN9X"},
              "platforms": {"_platform_desktop", "_platform_all", "_platform_computers", "_platform_win9x"},
              "discord": False},
    "windows": {"defines": set(),
                "platforms": {"_platform_desktop", "_platform_all", "_platform_computers"},
                "discord": True},
}

# dialog units to pixels at 96 dpi with the 8-point dialog font (6 by 13
# pixel base units), and the window frame round the client area
DLU_X = 6 / 4
DLU_Y = 13 / 8
FRAME_WIDTH = 8
FRAME_HEIGHT = 27
DESKTOP = (600, 440)

TYPE_NAMES = {"_config_boolean": 0, "_config_integer": 1, "_config_real": 2, "_config_string": 3}

# the hand-made controls: setting -> the kind its control must have
EXPECTED_KINDS = {
    "display.renderer": "combo_list",
    "display.mode": "combo_edit",
    "display.field_of_view": "trackbar",
    "display.gl1_texture_bits": "combo_list",
    "display.fullscreen": "checkbox",
    "display.vsync": "checkbox",
    "display.window_scale": "edit",
    "display.interpolation": "checkbox",
    "display.fps_counter": "checkbox",
    "graphics.light_pipeline": "combo_list",
    "audio.output": "combo_list",
    "audio.rate": "combo_edit",
    "launcher.show": "checkbox",
}
# the pages the sections' settings must be on (-1: the bottom row)
EXPECTED_PAGES = {"graphics": 1, "audio": 2, "input": 3}


def tokens(text):
    """C tokens of the table's body: strings (joined when adjacent),
    words, numbers and punctuation; comments dropped"""
    index = 0
    while index < len(text):
        char = text[index]
        if char.isspace():
            index += 1
        elif text.startswith("/*", index):
            index = text.index("*/", index) + 2
        elif char == '"':
            end = index + 1
            value = ""
            while text[end] != '"':
                if text[end] == "\\":
                    escape = text[end + 1]
                    value += {"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(escape, escape)
                    end += 2
                else:
                    value += text[end]
                    end += 1
            yield ("string", value)
            index = end + 1
        elif char.isalnum() or char == "_" or (char in "-." and index + 1 < len(text) and text[index + 1].isdigit()):
            match = re.match(r"[-.]?[A-Za-z0-9_.]+", text[index:])
            yield ("word", match.group(0))
            index += len(match.group(0))
        else:
            yield ("punct", char)
            index += 1


def read_table(defines):
    """port_config.c's settings as the build with these macros defined
    compiles them: (name, type number, default text, platform words,
    comment)"""
    source = CONFIG_SOURCE.read_text(encoding="utf-8")
    start = source.index("config_settings[] =")
    body = source[source.index("{", start) + 1:source.index("\n};", start)]
    # the preprocessor: keep the lines of the active branches
    kept, stack = [], []
    for line in body.split("\n"):
        stripped = line.strip()
        if stripped.startswith("#ifdef ") or stripped.startswith("#ifndef "):
            macro = stripped.split()[1]
            active = (macro in defines) == stripped.startswith("#ifdef ")
            stack.append(active)
        elif stripped.startswith("#else"):
            stack[-1] = not stack[-1]
        elif stripped.startswith("#endif"):
            stack.pop()
        elif stripped.startswith("#"):
            raise SystemExit(f"unexpected preprocessor line in the table: {stripped}")
        elif all(stack):
            kept.append(line)
    macros = {"CONFIG_INTERPOLATION_DEFAULT": '"false"' if "HALO_WIN9X" in defines else '"true"',
              "CONFIG_DISCORD_PLATFORMS": "0" if "HALO_WIN9X" in defines else "_platform_desktop"}
    settings = []
    fields, field, depth = [], [], 0
    for kind, value in tokens("\n".join(kept)):
        if kind == "punct" and value == "{":
            depth += 1
            fields, field = [], []
        elif kind == "punct" and value == "}":
            depth -= 1
            fields.append(field)
            name, type_name, default, _, _, platforms, comment = fields
            default = default[0]
            if default[0] == "word" and default[1] in macros:
                default = ("string", macros[default[1]].strip('"'))
            platforms = {token for kind, token in platforms if kind == "word"}
            if "CONFIG_DISCORD_PLATFORMS" in platforms:
                platforms = {macros["CONFIG_DISCORD_PLATFORMS"]}
            type_number = TYPE_NAMES[type_name[0][1]]
            default_text = default[1]
            # a string's default is written as a TOML basic string
            if type_number == 3 and len(default_text) >= 2 and default_text[0] == default_text[-1] == '"':
                default_text = default_text[1:-1]
            settings.append((name[0][1], type_number, default_text, platforms, comment[0][1]))
        elif depth and kind == "punct" and value == ",":
            fields.append(field)
            field = []
        elif depth:
            field.append((kind, value))
    return settings


def build_settings(build):
    """the settings the build reads, in the table's order"""
    rules = BUILDS[build]
    return [s for s in read_table(rules["defines"]) if s[3] & rules["platforms"]]


def run_launcher(program, settings):
    """t_launcher's output for these settings, parsed"""
    text = "".join(f"{name}\t{type_number}\t{default}\n" for name, type_number, default, _, _ in settings)
    result = subprocess.run([str(program)], input=text, capture_output=True, text=True, check=False)
    if result.returncode:
        raise SystemExit(f"{program} failed ({result.returncode}): {result.stderr}")
    dialog, items, controls, advanced, header = None, [], {}, [], {}
    for line in result.stdout.splitlines():
        fields = line.split("\t")
        if fields[0] == "dialog":
            dialog = {"x": int(fields[1]), "y": int(fields[2]), "width": int(fields[3]), "height": int(fields[4]),
                      "items": int(fields[5]), "size": int(fields[6])}
        elif fields[0] == "item":
            items.append({"id": int(fields[1]), "x": int(fields[2]), "y": int(fields[3]), "width": int(fields[4]),
                          "height": int(fields[5]), "class": fields[6], "style": int(fields[7], 16),
                          "text": fields[8] if len(fields) > 8 else ""})
        elif fields[0] == "control":
            controls[int(fields[1])] = {"page": int(fields[2]), "kind": fields[3], "setting": fields[4]}
        elif fields[0] == "advanced":
            advanced.append(fields[1])
        else:
            header[fields[0]] = fields[1:]
    return dialog, items, controls, advanced, header


class Checker:
    def __init__(self, build):
        self.build = build
        self.failures = 0
        self.checks = 0

    def check(self, condition, message):
        self.checks += 1
        if not condition:
            self.failures += 1
            print(f"FAIL [{self.build}] {message}")
        return condition


def rect(item):
    """a control's rectangle, as it shows: a combo's height is its closed
    height (its template height is the dropped list's)"""
    height = 12 if item["class"] == "atom0085" else item["height"]
    return item["x"], item["y"], item["x"] + item["width"], item["y"] + height


def overlap(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def check_build(build, program):
    settings = build_settings(build)
    names = [s[0] for s in settings]
    dialog, items, controls, advanced, header = run_launcher(program, settings)
    c = Checker(build)
    print(f"[{build}] {len(settings)} settings, {len(items)} controls, dialog {dialog['width']}x{dialog['height']} "
          f"dialog units, template {dialog['size']} bytes")
    # the template and the layout agree
    c.check(dialog["items"] == len(items), f"header says {dialog['items']} items, {len(items)} follow")
    c.check(len(items) == len(controls), f"{len(items)} items in the template, {len(controls)} controls in the layout")
    ids = [item["id"] for item in items]
    c.check(len(set(ids)) == len(ids), f"ids used twice: {sorted(i for i in set(ids) if ids.count(i) > 1)}")
    c.check(set(ids) == set(controls), "the template's ids are not the layout's")
    c.check(header.get("font") == ["8", "MS Shell Dlg"], f"font {header.get('font')}")
    c.check(header.get("title") == ["Halo"], f"title {header.get('title')}")
    # the size
    width = dialog["width"] * DLU_X + FRAME_WIDTH
    height = dialog["height"] * DLU_Y + FRAME_HEIGHT
    c.check(width <= DESKTOP[0] and height <= DESKTOP[1],
            f"the window is {width:.0f}x{height:.0f} pixels at 96 dpi, more than {DESKTOP[0]}x{DESKTOP[1]}")
    print(f"[{build}] window about {width:.0f}x{height:.0f} pixels with the frame at 96 dpi")
    # places
    tab = next((item for item in items if item["class"] == "SysTabControl32"), None)
    if c.check(tab is not None, "no tab control"):
        tab_rect = rect(tab)
        # the pages' area: below the tab's row of labels (about 14 units)
        page_area = (tab_rect[0] + 2, tab_rect[1] + 14, tab_rect[2] - 2, tab_rect[3] - 2)
    for item in items:
        r = rect(item)
        control = controls[item["id"]]
        c.check(0 <= r[0] and 0 <= r[1] and r[2] <= dialog["width"] and r[3] <= dialog["height"],
                f"control {item['id']} ({control['kind']} {item['text']!r}) at {r} is outside the dialog")
        c.check(item["width"] > 0 and item["height"] > 0, f"control {item['id']} has no size")
        if tab is None or item is tab:
            continue
        if control["page"] >= 0:
            c.check(page_area[0] <= r[0] and page_area[1] <= r[1] and r[2] <= page_area[2] and r[3] <= page_area[3],
                    f"page {control['page']} control {item['id']} ({control['kind']} {item['text']!r}) at {r} "
                    f"is outside the pages' area {page_area}")
        else:
            c.check(not overlap(r, tab_rect),
                    f"control {item['id']} ({control['kind']} {item['text']!r}) at {r} lies over the tab control")
    # overlaps between controls that show at once
    for first in range(len(items)):
        for second in range(first + 1, len(items)):
            a, b = items[first], items[second]
            if a is tab or b is tab:
                continue
            page_a, page_b = controls[a["id"]]["page"], controls[b["id"]]["page"]
            if page_a != page_b and page_a >= 0 and page_b >= 0:
                continue
            c.check(not overlap(rect(a), rect(b)),
                    f"controls {a['id']} ({a['text']!r} at {rect(a)}) and {b['id']} ({b['text']!r} at {rect(b)}) "
                    f"overlap on page {page_a}")
    # bindings
    bound = {}
    for control_id, control in controls.items():
        if control["setting"] != "-":
            bound.setdefault(control["setting"], []).append(f"control {control_id} ({control['kind']})")
    for name in advanced:
        bound.setdefault(name, []).append("the Advanced list")
    for name in names:
        c.check(len(bound.get(name, [])) == 1, f"{name} is bound to {bound.get(name, [])}")
    for name in bound:
        c.check(name in names, f"{name} is shown but the build does not read it")
    for name, kind in EXPECTED_KINDS.items():
        if name in names:
            kinds = [c_["kind"] for c_ in controls.values() if c_["setting"] == name]
            c.check(kinds == [kind], f"{name} should be a {kind}, is {kinds or 'in the Advanced list'}")
    for name in names:
        section = name.split(".")[0]
        if section in EXPECTED_PAGES:
            pages = [c_["page"] for c_ in controls.values() if c_["setting"] == name]
            c.check(pages == [EXPECTED_PAGES[section]], f"{name} should be on page {EXPECTED_PAGES[section]}, "
                    f"is {pages or 'in the Advanced list'}")
    if "launcher.show" in names:
        pages = [c_["page"] for c_ in controls.values() if c_["setting"] == "launcher.show"]
        c.check(pages == [-1], "launcher.show should be on the bottom row")
    # what the pages hold, for the report
    per_page = {}
    for control in controls.values():
        if control["setting"] != "-":
            per_page.setdefault(control["page"], []).append(control["setting"])
    for page in sorted(per_page):
        print(f"[{build}] page {page}: {', '.join(per_page[page])}")
    print(f"[{build}] Advanced list: {', '.join(advanced)}")
    print(f"[{build}] {c.checks} checks, {c.failures} failed")
    return c.failures


def check_config_test(program):
    """t_config's scenes, and its table against the source's"""
    failures = 0
    settings = read_table({"HALO_WIN9X"})
    with tempfile.TemporaryDirectory() as folder:
        for scene in ("table", "first-run", "player-file", "broken-file", "nolauncher", "launcher", "link",
                      "link-launcher", "quit"):
            result = subprocess.run([str(program), folder, scene], capture_output=True, text=True, check=False)
            lines = result.stdout.splitlines()
            failed = result.returncode != 0 or any("FAIL" in line for line in lines)
            summary = next((line for line in lines if line.startswith("# ") and "checks" in line), "")
            print(f"[t_config {scene}] exit {result.returncode} {summary}")
            for line in lines:
                if "FAIL" in line:
                    print(f"  {line}")
            if failed:
                failures += 1
            if scene == "table":
                printed = [line.split("\t") for line in lines if line.startswith("setting\t")]
                names = [fields[1] for fields in printed]
                expected = [s[0] for s in settings]
                if names != expected:
                    print(f"FAIL [t_config] the table printed differs from the source's: "
                          f"{sorted(set(names) ^ set(expected))}")
                    failures += 1
                for fields in printed:
                    source = next((s for s in settings if s[0] == fields[1]), None)
                    if source and (int(fields[2]) != source[1] or fields[3] != source[2]):
                        print(f"FAIL [t_config] {fields[1]}: type {fields[2]} default {fields[3]!r}, "
                              f"the source says {source[1]} {source[2]!r}")
                        failures += 1
                    reads = source is not None and bool(source[3] & BUILDS["win9x"]["platforms"])
                    if source and bool(int(fields[4])) != reads:
                        print(f"FAIL [t_config] {fields[1]}: read_here {fields[4]}, the source says {reads}")
                        failures += 1
                print(f"[t_config table] {len(printed)} settings, as the source has them")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("program", help="t_launcher.exe")
    parser.add_argument("--config-test", help="t_config.exe")
    args = parser.parse_args()
    os.chdir(ROOT)
    failures = 0
    for build in BUILDS:
        failures += check_build(build, Path(args.program))
    if args.config_test:
        failures += check_config_test(Path(args.config_test))
    print("all checks passed" if not failures else f"{failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
