"""Extract MGR's menu bitmap font from the user's Xbox 360 data000.cpk.

Writes a local runtime asset; no game font data is checked into the repository.
Uses the existing CPK/DAT reader and the SDK's Xbox 360 tiled DXT5 layout.
"""
import argparse
import struct
from pathlib import Path

from mgr_cpk import read_toc, read_entry, dat_list


def tiled_offset(x, y, pitch):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 11
    micro = ((x & 7) + ((y & 14) << 2)) << 4
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 1) << 4)
    return ((off & ~511) << 3) + ((y & 16) << 7) + ((off & 448) << 2) + \
        (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 63)


def alpha_atlas(wtb):
    offsets, ids, headers = (struct.unpack_from('>I', wtb, p)[0] for p in (12, 24, 28))
    base = struct.unpack_from('>I', wtb, offsets)[0]
    texture_id = struct.unpack_from('>I', wtb, ids)[0]
    size = struct.unpack_from('>I', wtb, headers + 36)[0]
    width, height = (size & 8191) + 1, ((size >> 13) & 8191) + 1
    if wtb[headers + 35] & 63 != 20:
        raise ValueError('Expected Xbox 360 DXT5 font texture')
    alpha = bytearray(width * height)
    for by in range((height + 3) // 4):
        for bx in range((width + 3) // 4):
            off = base + tiled_offset(bx, by, (width + 3) // 4)
            block = bytearray(wtb[off:off + 16])
            for i in range(0, 16, 2):
                block[i], block[i + 1] = block[i + 1], block[i]
            a, b = block[:2]
            palette = [a, b]
            if a > b:
                palette += [((7 - i) * a + i * b) // 7 for i in range(1, 7)]
            else:
                palette += [((5 - i) * a + i * b) // 5 for i in range(1, 5)] + [0, 255]
            bits = int.from_bytes(block[2:8], 'little')
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < width and y < height:
                    alpha[y * width + x] = palette[(bits >> (3 * i)) & 7]
    return texture_id, width, height, alpha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cpk', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    toc = read_toc(args.cpk)
    glyphs = {}
    archives = ['ui/ui_core_us.dat', 'ui/ui_option_us.dat']
    archives += ['ui/' + r['FileName'] for r in toc['files']
                 if r['DirName'] == 'ui' and r['FileName'].endswith('_us.dat')
                 and r['FileName'] not in ('ui_core_us.dat', 'ui_option_us.dat')]
    for archive in archives:
        dat = read_entry(args.cpk, toc, archive)
        def item(data, wanted):
            for name, _, off, size in dat_list(data)[1]:
                if name == wanted:
                    return data[off:off + size]
            raise ValueError(wanted)
        dtt = None
        for message, ext, off, size in dat_list(dat)[1]:
            if ext != 'mcd':
                continue
            mcd = dat[off:off + size]
            _, _, sym_off, sym_count, glyph_off, _, _, _, _, _ = struct.unpack_from('>10I', mcd)
            atlas = None
            for i in range(sym_count):
                font, char, index = struct.unpack_from('>HHI', mcd, sym_off + i * 8)
                if font not in (3, 6) or char < 33 or char > 126 or (font, char) in glyphs:
                    continue
                if atlas is None:
                    if dtt is None:
                        dtt = read_entry(args.cpk, toc, archive[:-4] + '.dtt')
                    atlas = alpha_atlas(item(dtt, message[:-4] + '.wtb'))
                texture_id, width, height, alpha = atlas
                tex, u1, v1, u2, v2, advance, line, above, spacing, horizontal = \
                    struct.unpack_from('>I9f', mcd, glyph_off + index * 40)
                if tex != texture_id:
                    raise ValueError('Glyph texture mismatch')
                x, y = round(u1 * width), round(v1 * height)
                w, h = round((u2 - u1) * width), round((v2 - v1) * height)
                if not (w > 0 and h > 0 and x >= 0 and y >= 0 and x + w <= width and y + h <= height):
                    raise ValueError('Glyph outside atlas')
                pixels = b''.join(alpha[(y + row) * width + x:(y + row) * width + x + w]
                                  for row in range(h))
                glyphs[(font, char)] = struct.pack('<IIHHff', font, char, w, h, advance + spacing, line) + pixels
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(b'MGRFONT2' + struct.pack('<I', len(glyphs)) +
                            b''.join(glyphs[k] for k in sorted(glyphs)))
    print(f'Prepared {len(glyphs)} original menu glyphs: {args.output}')


if __name__ == '__main__':
    main()
