"""Build guest call graph (direct calls) from generated code: caller -> callees.

usage: call_graph.py generated/default > callgraph.tsv   (caller<TAB>callee)
"""
import glob
import re
import sys

DEF = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
CALL = re.compile(r"\bsub_([0-9A-F]{8})\(ctx, base\)")

def main():
    seen = set()
    for path in sorted(glob.glob(f"{sys.argv[1]}/mgr_recomp.*.cpp")):
        cur = None
        for line in open(path):
            m = DEF.match(line)
            if m:
                cur = m.group(1)
                continue
            if cur:
                for callee in CALL.findall(line):
                    if callee != cur and (cur, callee) not in seen:
                        seen.add((cur, callee))
                        print(f"{cur}\t{callee}")

if __name__ == "__main__":
    main()
