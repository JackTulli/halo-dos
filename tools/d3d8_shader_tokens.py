#!/usr/bin/env python3
"""Direct3D 8 shader tokens from assembly (the few instructions of vs.1.1,
ps.1.1 and ps.1.4 the Windows 95 build's Direct3D 8 renderer uses):
d3d8.dll takes a shader as its tokens, and there is no D3DX here to
assemble it. Prints a C array of the tokens, each instruction's source as
its comment, for port/dos/win95/w95_d3d.c.

    python tools/d3d8_shader_tokens.py NAME < shader.asm

The token layout is the DirectX 8 SDK's (d3d8types.h): the version, then
each instruction's opcode (bit 30 a co-issued one), its destination
(0x80000000 | type << 28 | write mask << 16 | modifier << 20 | shift << 24
| number) and sources (0x80000000 | type << 28 | swizzle << 16 | modifier
<< 24 | number), and the end.
"""

import re
import sys

OPCODES = {
    "nop": (0, 0), "mov": (1, 2), "add": (2, 3), "sub": (3, 3), "mad": (4, 4), "mul": (5, 3), "rcp": (6, 2),
    "rsq": (7, 2), "dp3": (8, 3), "dp4": (9, 3), "min": (10, 3), "max": (11, 3), "slt": (12, 3), "sge": (13, 3),
    "lrp": (18, 4), "frc": (19, 2), "texcrd": (64, 2), "texkill": (65, 1), "tex": (66, 1), "texld": (66, 2),
    "texbem": (67, 2), "texm3x3pad": (73, 2), "texm3x3tex": (74, 2), "texm3x3vspec": (77, 2), "cnd": (80, 4),
    "cmp": (88, 4),
}

REGISTER_TYPES = {"r": 0, "v": 1, "c": 2, "t": 3, "a": 3}
OUTPUT_TYPES = {"opos": (4, 0), "ofog": (4, 1), "opts": (4, 2)}

SOURCE_MODIFIERS = {"_bias": 2, "_bx2": 4, "_x2": 7, "_dz": 9, "_dw": 10}
SHIFTS = {"_x2": 1, "_x4": 2, "_x8": 3, "_d2": 0xF, "_d4": 0xE, "_d8": 0xD}
COMPONENTS = {"x": 0, "y": 1, "z": 2, "w": 3, "r": 0, "g": 1, "b": 2, "a": 3}


def register(text, vertex):
    """a register's type and number"""
    name = text.lower()
    if name in OUTPUT_TYPES:
        return OUTPUT_TYPES[name]
    match = re.fullmatch(r"o([dt])(\d+)", name)
    if match:
        return (5 if match.group(1) == "d" else 6), int(match.group(2))
    match = re.fullmatch(r"([rvcta])(\d+)", name)
    if not match:
        raise ValueError(f"not a register: {text}")
    kind = match.group(1)
    if kind == "t" and vertex:
        raise ValueError("t registers are the pixel shaders'")
    return REGISTER_TYPES[kind], int(match.group(2))


def destination(text, vertex):
    name, _, mask_text = text.partition(".")
    kind, number = register(name, vertex)
    mask = 0xF
    if mask_text:
        mask = 0
        for letter in mask_text.lower():
            mask |= 1 << COMPONENTS[letter]
    return 0x80000000 | (kind << 28) | (mask << 16) | number


def source(text, vertex):
    text = text.strip()
    modifier = 0
    negate = False
    if text.startswith("-"):
        negate = True
        text = text[1:]
    if text.startswith("1-"):
        modifier = 6
        text = text[2:]
    name, _, swizzle_text = text.partition(".")
    for suffix, value in SOURCE_MODIFIERS.items():
        if name.lower().endswith(suffix):
            modifier = value
            name = name[: -len(suffix)]
            break
    if negate:
        # (negated: none, bias, signed, x2 each have a negated form)
        modifier = {0: 1, 2: 3, 4: 5, 7: 8}[modifier]
    kind, number = register(name, vertex)
    swizzle = 0xE4
    if swizzle_text:
        letters = [COMPONENTS[c] for c in swizzle_text.lower()]
        letters += [letters[-1]] * (4 - len(letters))
        swizzle = letters[0] | (letters[1] << 2) | (letters[2] << 4) | (letters[3] << 6)
    return 0x80000000 | (kind << 28) | (swizzle << 16) | (modifier << 24) | number


def assemble(lines):
    tokens = []
    vertex = None
    for raw in lines:
        line = raw.split("//")[0].split(";")[0].strip()
        if not line:
            continue
        lower = line.lower()
        if lower.startswith(("vs.", "ps.")):
            major, minor = (int(x) for x in lower[3:].split("."))
            vertex = lower.startswith("vs.")
            tokens.append(((0xFFFE0000 if vertex else 0xFFFF0000) | (major << 8) | minor, line))
            continue
        if lower == "phase":
            tokens.append((0x0000FFFD, line))
            continue
        coissue = line.startswith("+")
        text = line[1:].strip() if coissue else line
        mnemonic, _, operands = text.partition(" ")
        parts = mnemonic.lower().split("_")
        name, modifiers = parts[0], ["_" + p for p in parts[1:]]
        opcode, count = OPCODES[name]
        operand_list = [o.strip() for o in operands.split(",") if o.strip()]
        if len(operand_list) != count:
            raise ValueError(f"{line}: {count} operands, not {len(operand_list)}")
        instruction = opcode | (0x40000000 if coissue else 0)
        words = [instruction]
        if operand_list:
            first = destination(operand_list[0], vertex)
            for modifier in modifiers:
                if modifier == "_sat":
                    first |= 1 << 20
                else:
                    first |= SHIFTS[modifier] << 24
            words.append(first)
            words += [source(o, vertex) for o in operand_list[1:]]
        tokens.append((words, line))
    tokens.append((0x0000FFFF, "end"))
    return tokens


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "shader"
    print(f"static const DWORD {name}[] =\n{{")
    for words, text in assemble(sys.stdin.read().splitlines()):
        if not isinstance(words, list):
            words = [words]
        print("\t" + ", ".join(f"0x{w:08x}" for w in words) + f", /* {text} */")
    print("};")


if __name__ == "__main__":
    main()
