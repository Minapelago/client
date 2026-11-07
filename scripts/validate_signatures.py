#!/usr/bin/env python3
"""Local-only signature validator (no CMake, no compile step). Reads the
signature table from src/pal/windows/win_signatures.cpp and asserts each pattern
matches EXACTLY ONCE in a binary's .text -- that uniqueness is the pass/fail gate. A fingerprint resolving to the
wrong-but-lookalike function is caught by verifying the function MANUALLY in
Ghidra at carve time, not here.

--expect-map prints each Code entry's mapped vs resolved address as an
informational DRIFT note when they differ; it does NOT fail the run, because
absolute addresses shift every game build (the recorded RVAs and the binary on
disk are routinely different builds). It only lines up when validating against
the exact build the RVAs were carved from.

  validate_signatures.py --binary BIN --arch {pe,elf} --table src/pal/windows/win_signatures.cpp \
      [--mapping scripts/sig_mapping.json --expect-map {windows,linux}]
"""
import argparse, os, re, struct, sys

import sigtool  # sibling module; scripts/ is on sys.path when run directly


def load_table(path):
    """Parse kPatN/kMaskN arrays and kEntries rows from win_signatures.cpp.
    Exits with a message if any row cannot be parsed or is inconsistent."""
    with open(path) as f:
        src = f.read()
    arrays = {m.group(1): [int(b, 16) for b in re.findall(r"0x[0-9a-fA-F]{2}", m.group(2))]
              for m in re.finditer(r"\b(k(?:Pat|Mask)\d+)\[\]\s*=\s*\{([^}]*)\}", src)}
    row = re.compile(r'\{"([^"]+)",\s*mth::sig::Kind::(Code|DataRef),\s*(kPat\d+),\s*(kMask\d+),\s*sizeof\(\w+\),\s*(\d+),\s*(\d+)\}')
    rows = list(row.finditer(src))
    body = src.split("kEntries[] = {", 1)[-1]
    expected = len(re.findall(r'^\s*\{"', body, re.M))
    if not rows or len(rows) != expected:
        raise SystemExit(f"{path}: parsed {len(rows)} kEntries rows but found {expected} initializers")
    entries = []
    for m in rows:
        for arr in (m[3], m[4]):
            if arr not in arrays:
                raise SystemExit(f"{path}: entry {m[1]} references missing array {arr}")
        e = {"name": m[1], "kind": m[2], "pattern": arrays[m[3]], "mask": arrays[m[4]],
             "disp_off": int(m[5]), "next_insn": int(m[6])}
        if len(e["pattern"]) != len(e["mask"]):
            raise SystemExit(f"{path}: entry {m[1]} pattern has {len(e['pattern'])} bytes but mask has {len(e['mask'])}")
        entries.append(e)
    return entries


def resolve_va(text, text_rva, base, e, off):
    """Resolved virtual address, or None if a DataRef disp32 runs past .text
    (mirrors the C++ runtime's bounds check, which returns 0)."""
    if e["kind"] == "Code":
        return base + text_rva + off
    disp_at = off + e["disp_off"]
    if disp_at + 4 > len(text):
        return None
    disp = struct.unpack_from("<i", bytes(text), disp_at)[0]
    return base + text_rva + off + e["next_insn"] + disp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--arch", choices=["pe", "elf"], required=True)
    ap.add_argument("--table", default=os.path.join(os.path.dirname(__file__), "..", "src", "pal", "windows", "win_signatures.cpp"))
    ap.add_argument("--mapping")
    ap.add_argument("--expect-map", choices=["windows", "linux"])
    args = ap.parse_args()

    base, text_rva, text = sigtool.load_text(args.binary, args.arch)
    entries = load_table(args.table)
    expect = {}
    if args.mapping and args.expect_map:
        expect = sigtool.load_mapping(args.mapping)[args.expect_map]

    ok = 0
    failures = 0
    drift = 0
    for e in entries:
        name = e["name"]
        n = sigtool.count_matches(text, e["pattern"], e["mask"], cap=2)
        if n == 0:
            print(f"MISS   {name}"); failures += 1; continue
        if n > 1:
            print(f"AMBIG  {name}"); failures += 1; continue
        off = sigtool.first_match_offset(text, e["pattern"], e["mask"])
        va = resolve_va(text, text_rva, base, e, off)
        if va is None:
            print(f"OOB    {name} (disp32 runs past .text)"); failures += 1; continue
        exp = expect.get(name, {}).get("rva") if e["kind"] == "Code" else None
        if exp is not None and va != base + exp:
            # Informational only: absolute addresses move every build (see module docstring).
            print(f"DRIFT  {name} resolved {va:#x} (mapped {base + exp:#x}; different build)"); drift += 1; ok += 1; continue
        print(f"OK     {name} -> {va:#x}")
        ok += 1

    tail = f", {drift} address-drift note(s)" if drift else ""
    print(f"\n{ok} ok, {failures} failure(s){tail}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
