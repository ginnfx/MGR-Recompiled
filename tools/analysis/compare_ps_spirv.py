"""Compare SPIR-V size of Xenia-translated pixel shaders vs XenosRecomp output for the same microcode.

usage: compare_ps_spirv.py <repo root>
Needs: private/shaders (containers), private/shader-hlsl (XenosRecomp dumps),
private/xenia-dump (run with --dump_shaders).
"""
import os
import statistics
import struct
import subprocess
import sys

def main():
    root = sys.argv[1]
    dxc_dir = f"{root}/vendor/reblue/thirdparty/XenosRecomp/thirdparty/dxc-bin"
    env = dict(os.environ, DYLD_LIBRARY_PATH=f"{dxc_dir}/lib/arm64")
    cont = {}
    for d, _, fs in os.walk(f"{root}/private/shaders"):
        for f in fs:
            b = open(os.path.join(d, f), "rb").read()
            _, vs, _, _, _, _, so = struct.unpack_from(">7I", b, 0)
            po, sz = struct.unpack_from(">II", b, so)
            cont.setdefault(b[vs + po:vs + po + sz], f)
    dump = f"{root}/private/xenia-dump/"
    files = os.listdir(dump)
    rows = []
    for f in files:
        if not f.endswith(".ucode.bin.frag"):
            continue
        h = f.split(".")[0]
        raw = open(dump + f, "rb").read()
        swapped = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
        name = cont.get(raw) or cont.get(swapped)  # Xenia dumps host-endian words
        spv = [x for x in files if x.startswith(h + "_") and x.endswith("spirv.bin.frag")]
        hlsl = f"{root}/private/shader-hlsl/{name.rsplit('.', 1)[0]}.hlsl" if name else ""
        if not spv or not os.path.exists(hlsl) or not open(hlsl, encoding="latin-1").read(20).startswith("// pixel"):
            continue
        r = subprocess.run([f"{dxc_dir}/bin/arm64/dxc-macos", "-T", "ps_6_0", "-HV", "2021",
                            "-all-resources-bound", "-spirv", "-fvk-use-dx-layout", "-Wno-ignored-attributes",
                            "-Qstrip_debug", "-DREBLUE_RECOMP", "-Fo", "/tmp/xr.spv", hlsl],
                           capture_output=True, env=env)
        if r.returncode:
            print("dxc failed:", name, r.stderr[:200])
            continue
        rows.append((max(os.path.getsize(dump + x) for x in spv), os.path.getsize("/tmp/xr.spv"), len(raw) // 4, name))
    rows.sort(reverse=True)
    print(f"pairs {len(rows)}\nxenia_spv  xenosrecomp_spv  ucode_words  shader")
    for a, b, w, n in rows[:15]:
        print(f"{a:9d} {b:12d} {w:10d}  {n}")
    if rows:
        print(f"total xenia {sum(r[0] for r in rows)}  total xenosrecomp {sum(r[1] for r in rows)}  "
              f"median ratio {statistics.median(r[0] / r[1] for r in rows):.2f}")

if __name__ == "__main__":
    main()
