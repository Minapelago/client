"""Shared signature primitives: ELF/PE .text loading, iced-x86 based byte
wildcarding, masked scanning, and minimal-unique signature carving. Imported by
validate_signatures.py so carve and scan never drift. Run directly, it carves
every symbol in the mapping and prints the table in the format of
src/pal/windows/win_signatures.cpp:

  sigtool.py --binary BIN --mapping scripts/sig_mapping.json --arch {pe,elf} --rva-map {windows,linux}

This is a whole-table re-carve for a new build: it exits on the first symbol it
cannot carve uniquely, so refresh the RVAs in the mapping for the target build
first. Each mapping symbol needs name and kind; the per-build map needs rva for
Code, or ref_site_rva, disp_off and next_insn for DataRef (see carve_entry).
Paste the output over the arrays and kEntries in win_signatures.cpp. Not referenced by any CMakeLists; pure dev-time tooling."""
import argparse
import json
import sys

from iced_x86 import Code, Decoder, OpKind

# Operand kinds whose (relative) target immediate shifts between builds.
_NEAR_BRANCH = (OpKind.NEAR_BRANCH16, OpKind.NEAR_BRANCH32, OpKind.NEAR_BRANCH64)


def load_mapping(path):
    with open(path) as f:
        return json.load(f)


def load_text(path, arch):
    """Return (image_base, text_rva, text_bytes) for a PE or ELF file."""
    if arch == "pe":
        import pefile
        pe = pefile.PE(path, fast_load=True)
        for s in pe.sections:
            if s.Name.rstrip(b"\x00") == b".text":
                return pe.OPTIONAL_HEADER.ImageBase, s.VirtualAddress, s.get_data()
        raise SystemExit("no .text in PE")
    from elftools.elf.elffile import ELFFile
    with open(path, "rb") as f:
        sec = ELFFile(f).get_section_by_name(".text")
        # PIE ELF: image base 0, sh_addr is the virtual address of .text.
        return 0, sec["sh_addr"], sec.data()


def _wildcard_offsets(decoder, instr):
    """Byte offsets within an instruction whose value shifts between builds:
    the RIP-relative memory displacement and relative-branch (call/jmp/jcc)
    target immediates. Offsets come from iced's constant-offset table, so REX /
    prefixes are accounted for automatically."""
    offs = set()
    co = decoder.get_constant_offsets(instr)
    if instr.is_ip_rel_memory_operand and co.displacement_size:
        for i in range(co.displacement_size):
            offs.add(co.displacement_offset + i)
    if co.immediate_size and any(instr.op_kind(i) in _NEAR_BRANCH for i in range(instr.op_count)):
        for i in range(co.immediate_size):
            offs.add(co.immediate_offset + i)
    return offs


def _longest_fixed_run(mask):
    """(start, length) of the longest run of fixed (mask != 0) bytes, or (0, 0)."""
    best_start, best_len, i, n = 0, 0, 0, len(mask)
    while i < n:
        if mask[i]:
            j = i + 1
            while j < n and mask[j]:
                j += 1
            if j - i > best_len:
                best_start, best_len = i, j - i
            i = j
        else:
            i += 1
    return best_start, best_len


def _match_offsets(text, pattern, mask, cap):
    """Offsets (up to cap) where pattern matches text under mask. Anchors on the
    longest fixed-byte run and uses bytes.find (C-speed) to skip non-candidates,
    verifying the full mask only at the few anchor hits -- vs the old O(N*M)
    per-byte Python scan over multi-MB .text. Each full match has exactly one
    anchor occurrence (at start + a_start), so matches are neither missed nor
    double-counted."""
    plen = len(pattern)
    last = len(text) - plen
    if last < 0:
        return []
    pat = bytes(pattern)
    tb = text if isinstance(text, (bytes, bytearray)) else bytes(text)
    a_start, a_len = _longest_fixed_run(mask)
    if a_len == 0:  # fully wildcarded (degenerate): every position matches
        return list(range(min(last + 1, cap)))
    anchor = pat[a_start:a_start + a_len]
    hits, pos = [], 0
    while True:
        h = tb.find(anchor, pos)
        if h < 0:
            break
        pos = h + 1
        start = h - a_start
        if start < 0 or start > last:
            continue
        if all((not mask[j]) or tb[start + j] == pat[j] for j in range(plen)):
            hits.append(start)
            if len(hits) >= cap:
                break
    return hits


def count_matches(text, pattern, mask, cap=2):
    """Count masked matches of pattern in text, stopping once cap is reached."""
    return len(_match_offsets(text, pattern, mask, cap))


def first_match_offset(text, pattern, mask):
    offs = _match_offsets(text, pattern, mask, 1)
    return offs[0] if offs else -1


def _is_anchor(instr):
    """True if the instruction cross-references code or data: a RIP-relative
    memory operand, or a near call/branch. The opcode+modrm of such a site is
    function-identifying body content (the relative/displacement bytes are still
    masked), so including one makes the signature fail-to-match (loud) on a real
    code change instead of silently matching a lookalike prologue elsewhere."""
    if instr.is_ip_rel_memory_operand:
        return True
    return any(instr.op_kind(i) in _NEAR_BRANCH for i in range(instr.op_count))


def carve(text, text_rva, start_rva, max_bytes=80, want_anchor=True):
    """Grow a masked pattern from start_rva until it is unique in text AND (when
    want_anchor) it has captured at least one cross-reference instruction. Returns
    (pattern_bytes, mask_bytes). Falls back to the minimal-unique pattern if no
    cross-ref appears within max_bytes."""
    off = start_rva - text_rva
    if not 0 <= off < len(text):
        raise SystemExit(f"start rva {start_rva:#x} is outside .text [{text_rva:#x}, {text_rva + len(text):#x})")
    code = bytes(text[off:off + max_bytes + 16])
    decoder = Decoder(64, code, ip=start_rva)
    pattern, mask, pos = bytearray(), bytearray(), 0
    unique = anchored = False
    for instr in decoder:
        if instr.code == Code.INVALID or instr.len == 0:
            break
        wc = _wildcard_offsets(decoder, instr)
        anchored = anchored or _is_anchor(instr)
        for k in range(instr.len):
            pattern.append(code[pos + k])
            mask.append(0 if k in wc else 1)
        pos += instr.len
        unique = unique or count_matches(text, pattern, mask) == 1
        if unique and (anchored or not want_anchor):
            return bytes(pattern), bytes(mask)
        if len(pattern) >= max_bytes:
            break
    if unique:
        return bytes(pattern), bytes(mask)  # unique but no cross-ref within cap
    raise SystemExit(f"could not carve a unique pattern from rva {start_rva:#x}")


def carve_entry(text, text_rva, sym, mapinfo):
    """Carve one symbol into a JSON-serialisable entry dict.

    DataRef entries MUST supply disp_off and next_insn explicitly: they pin the
    disp32 location and the next-instruction offset the C++ runtime adds for the
    RIP-relative resolve. Defaulting them would silently emit a wrong-but-valid
    signature the runtime cannot detect, so a missing field is a hard error."""
    kind = sym["kind"]
    if kind == "DataRef":
        if "disp_off" not in mapinfo or "next_insn" not in mapinfo:
            raise SystemExit(f"{sym['name']}: DataRef mapping must specify disp_off and next_insn")
        start, disp_off, next_insn = mapinfo["ref_site_rva"], mapinfo["disp_off"], mapinfo["next_insn"]
    else:
        start, disp_off, next_insn = mapinfo["rva"], 0, 0
    pattern, mask = carve(text, text_rva, start)
    return {"name": sym["name"], "kind": kind, "pattern": list(pattern), "mask": list(mask),
            "disp_off": disp_off, "next_insn": next_insn}


def format_cpp(entries):
    """C++ text for the kPat/kMask arrays and the kEntries table, as in win_signatures.cpp."""
    def arr(name, vals):
        return f"static constexpr std::uint8_t {name}[] = {{{', '.join(f'0x{v:02x}' for v in vals)}}};"
    L = []
    for i, e in enumerate(entries):
        L.append(arr(f"kPat{i}", e["pattern"]))
        L.append(arr(f"kMask{i}", e["mask"]))
    L += ["", "static constexpr mth::sig::Entry kEntries[] = {"]
    for i, e in enumerate(entries):
        kind = "mth::sig::Kind::Code" if e["kind"] == "Code" else "mth::sig::Kind::DataRef"
        L.append(f'    {{"{e["name"]}", {kind}, kPat{i}, kMask{i}, sizeof(kPat{i}), {e["disp_off"]}, {e["next_insn"]}}},')
    L.append("};")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description="Carve every mapped symbol and print C++ table text.")
    ap.add_argument("--binary", required=True)
    ap.add_argument("--mapping", required=True)
    ap.add_argument("--arch", choices=["pe", "elf"], required=True)
    ap.add_argument("--rva-map", choices=["windows", "linux"], required=True)
    args = ap.parse_args()

    _, text_rva, text = load_text(args.binary, args.arch)
    mapping = load_mapping(args.mapping)
    rva_map = mapping[args.rva_map]
    entries = []
    for sym in mapping["symbols"]:
        name = sym["name"]
        if name not in rva_map:
            print(f"skip {name}: no {args.rva_map} mapping", file=sys.stderr)
            continue
        e = carve_entry(text, text_rva, sym, rva_map[name])
        entries.append(e)
        print(f"ok {name}: {len(e['pattern'])} bytes", file=sys.stderr)
    sys.stdout.write(format_cpp(entries))


if __name__ == "__main__":
    main()
