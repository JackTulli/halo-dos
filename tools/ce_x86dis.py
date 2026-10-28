"""Small disassembly helper for a 32-bit PE (capstone).

    python dis.py <exe> refs <hex value>          code that uses a value (push/mov/cmp imm, or a memory operand)
    python dis.py <exe> at <hex address> [count]  instructions from an address
    python dis.py <exe> func <hex address>        from the function start (searched backwards) to its ret
    python dis.py <exe> calls <hex address>       call sites of a function
"""
import struct
import sys

import capstone
import pefile


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.text = None
        for s in self.pe.sections:
            if s.Name.rstrip(b"\0") == b".text":
                self.text = (self.base + s.VirtualAddress, s.get_data())
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False

    def code(self, va, size):
        start, data = self.text
        o = va - start
        return data[o:o + size]

    def disassemble(self, va, count=40, stop_at_ret=False):
        out = []
        for ins in self.md.disasm(self.code(va, count * 16), va):
            out.append(ins)
            if len(out) >= count or (stop_at_ret and ins.mnemonic == "ret"):
                break
        return out

    def refs(self, value):
        start, data = self.text
        needle = struct.pack("<I", value)
        hits = []
        i = 0
        while True:
            i = data.find(needle, i)
            if i < 0:
                break
            # find the instruction containing these bytes: try starts up to 7 bytes before
            for back in range(1, 8):
                va = start + i - back
                ins = next(self.md.disasm(self.code(va, 16), va), None)
                if ins and ins.address + ins.size >= start + i + 4 and ins.address <= start + i:
                    hits.append(ins)
                    break
            i += 1
        return hits

    def function_start(self, va):
        # walk back to a 'push ebp; mov ebp, esp' or an int3/nop pad boundary
        start, data = self.text
        o = va - start
        while o > 0:
            if data[o - 1] in (0xCC, 0x90, 0xC3) and data[o] not in (0xCC, 0x90):
                # padding before: likely a function start (functions are 16-aligned)
                if (start + o) % 16 == 0:
                    return start + o
            o -= 1
        return start

    def calls(self, target):
        start, data = self.text
        hits = []
        for i in range(len(data) - 5):
            if data[i] == 0xE8:
                rel = struct.unpack_from("<i", data, i + 1)[0]
                if start + i + 5 + rel == target:
                    hits.append(start + i)
        return hits


def show(instructions):
    for ins in instructions:
        print("  %08X  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))


def main():
    image = Image(sys.argv[1])
    what = sys.argv[2]
    if what == "refs":
        for ins in image.refs(int(sys.argv[3], 16)):
            print("%08X (function %08X)  %s %s" % (ins.address, image.function_start(ins.address), ins.mnemonic,
                ins.op_str))
    elif what == "at":
        show(image.disassemble(int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 40))
    elif what == "func":
        va = image.function_start(int(sys.argv[3], 16))
        print("function %08X" % va)
        show(image.disassemble(va, int(sys.argv[4]) if len(sys.argv) > 4 else 400, stop_at_ret=False))
    elif what == "calls":
        for site in image.calls(int(sys.argv[3], 16)):
            print("%08X (function %08X)" % (site, image.function_start(site)))


if __name__ == "__main__":
    main()
