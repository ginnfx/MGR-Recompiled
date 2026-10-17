"""MGR (Xbox 360) CRI CPK + PlatinumGames DAT reader.

Adapted from research scripts (scratchpad cpktoc.py/cpkpeek.py, written this session).
usage:
  mgr_cpk.py extract-shaders <data000.cpk> <out_dir>   # shader*.dat -> out_dir/<dat>/<name>.<vso|pso>
  mgr_cpk.py list <file.cpk>                           # TOC listing
"""
import json
import os
import struct
import sys

SHADER_DATS = ("shader.dat", "shader2.dat", "shadereff.dat")
SHADER_EXTS = ("vso", "pso")
def read_utf(buf):
    assert buf[:4] == b'@UTF', buf[:4]
    size, rows_off, str_off, data_off, name_off, ncols, rowlen, nrows = struct.unpack('>IIIIIHHI', buf[4:32])
    base = 8
    strings = buf[base + str_off: base + data_off]
    def s(o):
        e = strings.index(b'\0', o)
        return strings[o:e].decode('utf-8', 'replace')
    p = 32
    cols = []
    fmt = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i', 6: '>Q', 7: '>q', 8: '>f'}
    def readval(t, pos):
        if t in fmt:
            n = struct.calcsize(fmt[t]); return struct.unpack(fmt[t], buf[pos:pos+n])[0], pos + n
        if t == 0xA:
            o = struct.unpack('>I', buf[pos:pos+4])[0]; return s(o), pos + 4
        if t == 0xB:
            o, l = struct.unpack('>II', buf[pos:pos+8]); return (o, l), pos + 8
        raise ValueError(hex(t))
    for _ in range(ncols):
        flags = buf[p]; nm = s(struct.unpack('>I', buf[p+1:p+5])[0]); p += 5
        st, t = flags & 0xF0, flags & 0x0F
        const = None
        if st == 0x30:
            const, p = readval(t, p)
        cols.append((nm, st, t, const))
    rows = []
    for r in range(nrows):
        q = base + rows_off + r * rowlen
        row = {}
        for nm, st, t, const in cols:
            if st == 0x50:
                row[nm], q = readval(t, q)
            elif st == 0x30:
                row[nm] = const
            else:
                row[nm] = None
        rows.append(row)
    return s(name_off), rows


def read_toc(path):
    with open(path, "rb") as f:
        hdr = f.read(0x800)
        if hdr[:4] != b"CPK ":
            raise ValueError(f"{path}: not a CPK")
        _, rows = read_utf(hdr[16:])
        h = rows[0]
        toc_off, toc_size = h.get("TocOffset"), h.get("TocSize")
        f.seek(toc_off)
        t = f.read(toc_size)
        if t[:4] != b"TOC ":
            raise ValueError(f"{path}: bad TOC")
        _, files = read_utf(t[16:])
    return {"files": files, "toc_off": toc_off}


def crilayla(src):
    usize, hoff = struct.unpack('<II', src[8:16])
    out = bytearray(usize + 0x100)
    out[0:0x100] = src[hoff + 0x10: hoff + 0x110]
    state = {'pos': hoff + 0x10 - 1, 'pool': 0, 'left': 0}
    def bits(n):
        v = 0
        while n:
            if state['left'] == 0:
                state['pool'] = src[state['pos']]; state['pos'] -= 1; state['left'] = 8
            take = min(n, state['left'])
            v = (v << take) | ((state['pool'] >> (state['left'] - take)) & ((1 << take) - 1))
            state['left'] -= take; n -= take
        return v
    oend = 0x100 + usize - 1
    done = 0
    vle = (2, 3, 5, 8)
    while done < usize:
        if bits(1):
            boff = oend - done + bits(13) + 3
            blen = 3
            for lv in vle:
                t = bits(lv); blen += t
                if t != (1 << lv) - 1: break
            else:
                while True:
                    t = bits(8); blen += t
                    if t != 255: break
            for _ in range(blen):
                out[oend - done] = out[boff]; boff -= 1; done += 1
        else:
            out[oend - done] = bits(8); done += 1
    return bytes(out)


def entry(cpk, toc, name):
    for r in toc['files']:
        full = (r['DirName'] + '/' if r['DirName'] else '') + r['FileName']
        if full == name:
            with open(cpk, 'rb') as f:
                f.seek(r['FileOffset'] + toc['toc_off']); raw = f.read(r['FileSize'])
            return raw, r
    raise KeyError(name)


def dat_list(b):
    magic, n, posoff, extoff, namoff, sizoff = struct.unpack('>4sIIIII', b[:24])
    endian = 'BE'
    if n > 0xFFFF:
        magic, n, posoff, extoff, namoff, sizoff = struct.unpack('<4sIIIII', b[:24]); endian = 'LE'
    e = '>' if endian == 'BE' else '<'
    exts = [b[extoff + 4*i: extoff + 4*i + 4].rstrip(b'\0').decode('ascii', 'replace') for i in range(n)]
    namelen = struct.unpack(e + 'I', b[namoff:namoff+4])[0]
    names = [b[namoff+4+namelen*i: namoff+4+namelen*(i+1)].split(b'\0')[0].decode('ascii', 'replace') for i in range(n)]
    offs = [struct.unpack(e + 'I', b[posoff+4*i:posoff+4*i+4])[0] for i in range(n)]
    sizes = [struct.unpack(e + 'I', b[sizoff+4*i:sizoff+4*i+4])[0] for i in range(n)]
    return endian, list(zip(names, exts, offs, sizes))


def read_entry(cpk, toc, name):
    raw, _ = entry(cpk, toc, name)
    return crilayla(raw) if raw[:8] == b"CRILAYLA" else raw


def extract_shaders(cpk, out_dir):
    toc = read_toc(cpk)
    total = 0
    for dat in SHADER_DATS:
        data = read_entry(cpk, toc, dat)
        if data[:4] != b"DAT\0":
            raise ValueError(f"{dat}: not a DAT archive")
        _, items = dat_list(data)
        dst = os.path.join(out_dir, dat.rsplit(".", 1)[0])
        os.makedirs(dst, exist_ok=True)
        for name, ext, off, size in items:
            if ext not in SHADER_EXTS:
                continue
            blob = data[off:off + size]
            path = os.path.join(dst, name)
            index = 1
            while os.path.exists(path) and open(path, "rb").read() != blob:
                stem, dot_ext = os.path.splitext(name)  # same name, different shader
                path = os.path.join(dst, f"{stem}~{index}{dot_ext}")
                index += 1
            with open(path, "wb") as f:
                f.write(blob)
            total += 1
        print(f"{dat}: {sum(1 for i in items if i[1] in SHADER_EXTS)} shaders")
    print(f"total {total} shader containers -> {out_dir}")


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "extract-shaders":
        extract_shaders(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == "list":
        for r in read_toc(sys.argv[2])["files"]:
            print((r["DirName"] + "/" if r["DirName"] else "") + r["FileName"], r["FileSize"])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
