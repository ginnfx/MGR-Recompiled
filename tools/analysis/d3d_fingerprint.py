"""Fingerprint the statically linked XDK D3D library so its entry points can be named.

For every function in the D3D range it reports what the code builds and touches:
PM4 type-3 packet headers (opcode names), type-0 register-write headers, GPU
register indices loaded as immediates, store offsets above 0x400 (device struct
candidates), direct callees, and callers outside the library.

usage: d3d_fingerprint.py image.bin extents.txt callgraph.tsv > d3d_fingerprint.tsv
"""
import struct
import sys
from collections import defaultdict

IMAGE_BASE = 0x82000000
D3D_START, D3D_END = 0x826A0000, 0x826D0000
LIS_WINDOW = 8  # instructions after lis to look for the low half
DEVICE_OFFSET_MIN = 0x400  # smaller store offsets are mostly locals and objects

# Xenos PM4 type-3 opcodes (xenia/gpu/xenos.h).
PM4_OPCODES = {
    0x10: "NOP", 0x15: "INDIRECT_BUFFER_PFD", 0x1D: "WAIT_FOR_IDLE", 0x22: "DRAW_INDX",
    0x26: "VIZ_QUERY", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x2D: "SET_CONSTANT",
    0x2F: "LOAD_ALU_CONSTANT", 0x36: "DRAW_INDX_2", 0x37: "INDIRECT_BUFFER", 0x3B: "INVALIDATE_STATE",
    0x3C: "WAIT_REG_MEM", 0x3D: "MEM_WRITE", 0x3E: "REG_RMW", 0x3F: "COND_EXEC", 0x44: "REG_TO_MEM",
    0x46: "EVENT_WRITE", 0x48: "ME_INIT", 0x4A: "SET_SHADER_BASES", 0x54: "INTERRUPT",
    0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS", 0x57: "EVENT_WRITE_ZPD",
    0x58: "EVENT_WRITE_SHD", 0x59: "EVENT_WRITE_CFL", 0x5A: "EVENT_WRITE_EXT",
    0x5B: "CONTEXT_UPDATE", 0x5D: "SET_BIN_MASK_LO", 0x5E: "SET_BIN_MASK_HI",
    0x5F: "SET_BIN_SELECT_LO", 0x60: "SET_BIN_SELECT_HI", 0x61: "SET_BIN_MASK", 0x62: "SET_BIN_SELECT",
    0x64: "XE_SWAP",
}

# GPU registers worth naming when they appear as immediates (xenia register_table.inc).
REGISTERS = {
    0x0E00: "SQ_VS_PROGRAM?", 0x2000: "RB_SURFACE_INFO", 0x2001: "RB_COLOR_INFO",
    0x2002: "RB_DEPTH_INFO", 0x2003: "RB_COLOR1_INFO", 0x2004: "RB_COLOR2_INFO",
    0x2005: "RB_COLOR3_INFO", 0x2080: "PA_SC_WINDOW_OFFSET", 0x2081: "PA_SC_WINDOW_SCISSOR_TL",
    0x2082: "PA_SC_WINDOW_SCISSOR_BR", 0x2100: "VGT_MAX_VTX_INDX", 0x2104: "RB_COLOR_MASK",
    0x2180: "SQ_PROGRAM_CNTL", 0x2181: "SQ_CONTEXT_MISC", 0x2183: "SQ_INTERPOLATOR_CNTL",
    0x2200: "RB_DEPTHCONTROL", 0x2201: "RB_BLENDCONTROL0", 0x2202: "RB_COLORCONTROL",
    0x2205: "PA_SU_SC_MODE_CNTL", 0x2208: "RB_MODECONTROL", 0x2209: "RB_BLENDCONTROL1",
    0x2210: "RB_STENCILREFMASK", 0x2280: "PA_SU_POINT_SIZE", 0x2300: "PA_SC_VIZ_QUERY",
    0x2316: "RB_COPY_CONTROL", 0x2317: "RB_COPY_DEST_BASE", 0x2318: "RB_COPY_DEST_PITCH",
    0x2319: "RB_COPY_DEST_INFO", 0x231B: "RB_COPY_CLEAR", 0x2321: "RB_DEPTH_CLEAR",
    0x2323: "RB_COLOR_CLEAR", 0x2380: "PA_SU_POLY_OFFSET", 0x4000: "SHADER_CONSTANT_000_X",
    0x4800: "SHADER_CONSTANT_FETCH_00_0", 0x4900: "SHADER_CONSTANT_BOOL_000_031",
    0x4908: "SHADER_CONSTANT_LOOP_00",
}


def load_functions(path):
    funcs = []
    for line in open(path):
        start, end = (int(x, 16) for x in line.split())
        if D3D_START <= start < D3D_END:
            funcs.append((start, end))
    return funcs


def load_external_callers(path, funcs):
    names = {f"{s:08X}" for s, _ in funcs}
    callers = defaultdict(set)
    for line in open(path):
        caller, callee = line.split()
        if callee in names and caller not in names:
            callers[int(callee, 16)].add(int(caller, 16))
    return callers


def _simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def built_constants(words):
    """Yield every 32-bit value a register holds after li/lis/addi/addis/ori/oris.

    Straight-line tracking that ignores branches: values die after LIS_WINDOW
    instructions without an update, so unrelated code rarely combines.
    """
    known = {}  # reg -> (value, index of last update)
    for i, w in enumerate(words):
        op, rd, ra = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F
        value = None
        if op in (14, 15):  # addi / addis (li / lis when rA == 0)
            imm = _simm(w) << (16 if op == 15 else 0)
            if ra == 0:
                value = imm
            elif ra in known and i - known[ra][1] <= LIS_WINDOW:
                value = known[ra][0] + imm
            target = rd
        elif op in (24, 25):  # ori / oris rA, rS, uimm
            target, source = ra, rd
            if source in known and i - known[source][1] <= LIS_WINDOW:
                value = known[source][0] | ((w & 0xFFFF) << (16 if op == 25 else 0))
        else:
            continue
        if value is None:
            known.pop(target, None)
            continue
        value &= 0xFFFFFFFF
        known[target] = (value, i)
        yield value


def classify(constants):
    pm4, type0, regs = set(), set(), set()
    for c in constants:
        c &= 0xFFFFFFFF
        if c >> 30 == 3 and c & 0xFE == 0:
            name = PM4_OPCODES.get((c >> 8) & 0xFF)
            if name:
                pm4.add(name)
        elif c >> 30 == 0 and c >> 16 and (c & 0x7FFF) in REGISTERS:
            type0.add(REGISTERS[c & 0x7FFF])
        elif c in REGISTERS:
            regs.add(REGISTERS[c])
    return pm4, type0, regs


def store_offsets(words):
    offsets = set()
    for w in words:
        if w >> 26 in (36, 37, 38, 44, 52, 54):  # stw stwu stb sth stfs stfd
            d = w & 0xFFFF
            if not d & 0x8000 and d >= DEVICE_OFFSET_MIN:
                offsets.add(d)
    return offsets


def callees(words, start):
    out = set()
    for i, w in enumerate(words):
        if w >> 26 == 18 and w & 3 == 1:  # bl, relative
            disp = w & 0x03FFFFFC
            if disp & 0x02000000:
                disp -= 0x04000000
            out.add((start + 4 * i + disp) & 0xFFFFFFFF)
    return out


def main():
    img = open(sys.argv[1], "rb").read()
    funcs = load_functions(sys.argv[2])
    external = load_external_callers(sys.argv[3], funcs)
    print("addr\tsize\text_callers\tcallees\tpm4\ttype0_regs\treg_imms\tstore_offsets\tsample_callers")
    for start, end in funcs:
        n = (end - start) // 4
        words = struct.unpack_from(f">{n}I", img, start - IMAGE_BASE)
        pm4, type0, regs = classify(built_constants(words))
        offs = sorted(store_offsets(words))
        ext = sorted(external.get(start, ()))
        print("\t".join([
            f"{start:08X}", str(end - start), str(len(ext)), str(len(callees(words, start))),
            ",".join(sorted(pm4)), ",".join(sorted(type0)), ",".join(sorted(regs)),
            ",".join(f"{o:X}" for o in offs[:24]) + ("…" if len(offs) > 24 else ""),
            ",".join(f"{c:08X}" for c in ext[:4]),
        ]))


if __name__ == "__main__":
    main()
