#!/usr/bin/env python3
"""Cache registers in call-free generated functions; keep all guest memory accesses.

This is a build-tree transformation, never an edit to the code generator's output.
Unknown helpers/macros/context fields make a function ineligible. Registers are
initialized from ctx (not zeroed) and every normal return publishes their state.
"""
import argparse
import collections
import json
from pathlib import Path
import re

FUNCTION = re.compile(r"^DEFINE_REX_FUNC\((\w+)\) \{\n(.*?)^\}\n", re.M | re.S)
REGISTER = re.compile(r"\bctx\.(r(?:[0-9]|[12][0-9]|3[01])|cr[0-7]|ctr|xer)\b")
CALL = re.compile(r"\b([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)(?:<[^<>\n]*>)?\s*\(")
ALLOWED_CALLS = {
    "REX_FUNC_PROLOGUE", "REX_RAW_ADDR",
    "REX_LOAD_U8", "REX_LOAD_U16", "REX_LOAD_U32", "REX_LOAD_U64",
    "REX_STORE_U8", "REX_STORE_U16", "REX_STORE_U32", "REX_STORE_U64",
    "if", "switch", "while", "for", "static_cast", "reinterpret_cast",
    "compare",  # CRRegister::compare(integer, integer, XER), inline noexcept
    "__builtin_bswap16", "__builtin_bswap32", "__builtin_bswap64",
    "__builtin_clz", "__builtin_clzll", "__builtin_rotateleft32",
    "__builtin_rotateleft64", "__builtin_rotateright32", "__builtin_rotateright64",
}
ALLOWED_MACROS = {name for name in ALLOWED_CALLS if name.startswith("REX_")}


def localize(body):
    # Strip instruction comments before analysis; retain them in emitted code.
    code = re.sub(r"//[^\n]*", "", body)
    if code.count("{") != code.count("}") or "/*" in code:
        return None, "unknown_structure"
    if re.search(r"\b(?:asm|throw|try|catch|co_await|co_yield)\b|[\"']", code):
        return None, "observable_or_unknown_code"
    if set(re.findall(r"\b[A-Z][A-Z0-9_]{2,}\b", code)) - ALLOWED_MACROS:
        return None, "guest_call_mmio_or_runtime_macro"
    if set(CALL.findall(code)) - ALLOWED_CALLS:
        return None, "unknown_helper"
    registers = sorted(set(REGISTER.findall(code)))
    if not registers:
        return None, "no_registers"
    # Reject LR/MSR/FPSCR/reservation access, raw ctx use, hooks, and methods
    # beyond the explicitly audited integer CR helper. This also excludes normal
    # non-leaf prologues and every generated direct/indirect guest function call.
    remaining = REGISTER.sub("mgr_local", code)
    if re.search(r"\bctx\b", remaining):
        return None, "other_context_access"
    methods = re.findall(r"\.([A-Za-z_]\w*)\s*(?:<[^<>\n]*>)?\s*\(", remaining)
    if set(methods) - {"compare"}:
        return None, "unknown_method"
    # No escaped register references/pointers or hidden mutable aliases.
    if re.search(r"&\s*(?:mgr_local|\(\s*mgr_local)|\b(?:auto|PPCContext)\b", remaining):
        return None, "register_escape"
    # Published full union/CR state preserves partial-width stores and incoming
    # carry/condition bits. Keep all loads/stores, fences, labels and branches.
    prefix = "\t// Host locals for audited call-free guest register accesses.\n"
    prefix += "".join(f"\tauto mgr_leaf_{r} = ctx.{r};\n" for r in registers)
    flush = "".join(f"\tctx.{r} = mgr_leaf_{r};\n" for r in registers)
    translated = REGISTER.sub(lambda m: "mgr_leaf_" + m[1], body)
    translated = re.sub(r"\breturn\s*;", "{\n" + flush + "\treturn;\n\t}", translated)
    return prefix + translated + flush, None


def write_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(text)
        temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    counts = collections.Counter()
    functions = []

    def transform(match):
        body, reason = localize(match[2])
        if body is None:
            counts[reason] += 1
            return match[0]
        counts["localized"] += 1
        functions.append(match[1])
        return f"DEFINE_REX_FUNC({match[1]}) {{\n{body}}}\n"

    output = FUNCTION.sub(transform, args.input.read_text())
    write_changed(args.output, output)
    write_changed(args.report, json.dumps({"source": str(args.input),
        "counts": dict(counts), "functions": functions}, indent=2) + "\n")


if __name__ == "__main__":
    main()
