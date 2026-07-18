#!/usr/bin/env python3
"""Byte-pair-encode the on-module help text into a compact C table.

Reads the plaintext help arrays (help1..help21) and the help_pages ordering
from module/help_mode.c, builds a pair-BPE dictionary, and emits
module/help_data_packed.h used by help_mode.c when built with -D HELP_PACKED=1.

Regenerate whenever the help strings change:  python3 utils/help_pack.py
The generated header is committed so the firmware build needs no Python.

Encoding: help text is 7-bit ASCII, so byte values 0x01-0x1F and 0x80-0xFF are
free to use as dictionary codes (0x00 terminates a line). Each code expands to a
pair (two symbols, each a literal byte or another code); the decoder in
help_mode.c expands recursively via a small stack.
"""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "module", "help_mode.c")
OUT = os.path.join(HERE, "..", "module", "help_data_packed.h")

# free code byte values (never appear in 7-bit ASCII help text; 0x00 reserved)
CODES = list(range(0x01, 0x20)) + list(range(0x80, 0x100))  # 31 + 128 = 159


def unescape(s):
    return s.encode("latin-1").decode("unicode_escape")


def parse():
    txt = open(SRC).read()
    a = txt.index("#if INCLUDE_HELP_TEXT")
    b = txt.index("#else  // !INCLUDE_HELP_TEXT")
    region = txt[a:b]
    # arrays: const char* const helpN[...] = { "..","..", };
    arrays = {}
    for m in re.finditer(
        r"const char\* const (help\d+)\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;", region, re.S
    ):
        name = m.group(1)
        strs = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
        arrays[name] = [unescape(s) for s in strs]
    # page order from help_pages table
    pm = re.search(r"help_pages\[HELP_PAGES\]\s*=\s*\{(.*?)\};", region, re.S)
    order = re.findall(r"help\d+", pm.group(1))
    pages = [arrays[n] for n in order]
    return pages, order


def build_bpe(lines):
    # corpus as list of symbol-lists (each symbol is an int: literal byte or code)
    corpus = [list(l.encode("latin-1")) for l in lines]
    dict_pairs = []  # code index -> (left, right)
    code_iter = iter(CODES)
    codes_used = []
    while True:
        # count adjacent pairs across all lines
        counts = {}
        for sym in corpus:
            for i in range(len(sym) - 1):
                p = (sym[i], sym[i + 1])
                counts[p] = counts.get(p, 0) + 1
        if not counts:
            break
        # best pair: maximize occurrences (each merge saves ~occ bytes,
        # costs 2 bytes of dict); require a real win
        pair, occ = max(counts.items(), key=lambda kv: kv[1])
        if occ < 3:
            break
        try:
            code = next(code_iter)
        except StopIteration:
            break
        codes_used.append(code)
        dict_pairs.append(pair)
        # replace every non-overlapping occurrence of pair with code
        for sym in corpus:
            i = 0
            out = []
            while i < len(sym):
                if i < len(sym) - 1 and (sym[i], sym[i + 1]) == pair:
                    out.append(code)
                    i += 2
                else:
                    out.append(sym[i])
                    i += 1
            sym[:] = out
    return corpus, dict_pairs, codes_used


def expand(sym, dict_map):
    # expand one encoded symbol list back to literal bytes (for verification +
    # max-length calc)
    out = bytearray()
    stack = list(reversed(sym))
    while stack:
        s = stack.pop()
        if s in dict_map:
            l, r = dict_map[s]
            stack.append(r)
            stack.append(l)
        else:
            out.append(s)
    return bytes(out)


def main():
    pages, order = parse()
    flat = [l for pg in pages for l in pg]
    orig_str_bytes = sum(len(l) + 1 for l in flat)  # +NUL, pre-merge
    orig_ptr_bytes = len(flat) * 4
    n_lines = len(flat)

    corpus, dict_pairs, codes = build_bpe(flat)
    dict_map = {codes[i]: dict_pairs[i] for i in range(len(codes))}

    # re-encode per page (build blob with 0x00 line terminators) and verify
    # round-trip against the originals
    blob = bytearray()
    page_off = []
    idx = 0
    maxline = 0
    for pg in pages:
        page_off.append(len(blob))
        for _ in pg:
            enc = corpus[idx]
            dec = expand(enc, dict_map).decode("latin-1")
            assert dec == flat[idx], f"roundtrip fail:\n {flat[idx]!r}\n {dec!r}"
            maxline = max(maxline, len(dec))
            blob += bytes(enc)
            blob.append(0x00)
            idx += 1

    # dict stored as flat pairs: 2 bytes per code, indexed by (code - first)
    # we store two parallel arrays keyed by code value via a 256-entry map is
    # wasteful; instead store sorted codes + their pairs and a code->index LUT.
    # Simpler for decode: a full 256-entry table of {left,right}; code is an
    # index. Unused entries are 0. That's 512 bytes flat -- cheap and O(1).
    left = [0] * 256
    right = [0] * 256
    for c, (l, r) in dict_map.items():
        left[c] = l
        right[c] = r

    packed_total = len(blob) + 512 + len(page_off) * 2 + n_lines  # +help_length
    orig_total = orig_str_bytes + orig_ptr_bytes

    def carr(name, data, typ="uint8_t"):
        out = f"static const {typ} {name}[{len(data)}] = {{\n"
        for i in range(0, len(data), 16):
            out += "    " + ",".join(str(x) for x in data[i : i + 16]) + ",\n"
        return out + "};\n"

    hdr = []
    hdr.append("// GENERATED by utils/help_pack.py -- do not edit by hand.")
    hdr.append("// Byte-pair-encoded on-module help text; see help_pack.py.")
    hdr.append("#ifndef _HELP_DATA_PACKED_H_")
    hdr.append("#define _HELP_DATA_PACKED_H_")
    hdr.append("#include <stdint.h>")
    hdr.append("// clang-format off")
    hdr.append(f"#define HELP_PACKED_PAGES {len(pages)}")
    hdr.append(f"#define HELP_LINE_BUF {maxline + 1}")
    hdr.append(carr("help_bpe_l", left))
    hdr.append(carr("help_bpe_r", right))
    hdr.append(carr("help_blob", list(blob)))
    hdr.append(carr("help_page_off", page_off, "uint16_t"))
    hdr.append(
        carr("help_length_packed", [len(pg) for pg in pages])
    )
    hdr.append("// clang-format on")
    hdr.append("#endif")
    open(OUT, "w").write("\n".join(hdr) + "\n")

    print(f"pages={len(pages)} lines={n_lines} codes_used={len(codes)}/{len(CODES)}")
    print(f"longest decoded line = {maxline} chars (HELP_LINE_BUF={maxline+1})")
    print(f"blob (encoded text+NUL): {len(blob)} B")
    print(f"dict LUT (256x2):        512 B")
    print(f"page_off (2B each):      {len(page_off)*2} B")
    print(f"help_length:             {n_lines and len(pages)} B")
    print("-" * 48)
    print(f"ORIGINAL: strings {orig_str_bytes} + pointers {orig_ptr_bytes} = "
          f"{orig_total} B (~{orig_total/1024:.1f} KB)")
    print(f"          (note: linker str-merge trims ~1.9 KB of the strings)")
    print(f"PACKED:   {packed_total} B (~{packed_total/1024:.1f} KB) + decoder ~0.2 KB")
    print(f"SAVED:    ~{(orig_total-packed_total)/1024:.1f} KB (before decoder), "
          f"vs merged-original ~{(orig_total-1921-packed_total)/1024:.1f} KB")
    print(f"wrote {os.path.relpath(OUT)}")


if __name__ == "__main__":
    main()
