#!/usr/bin/env python3
"""Generate the project's external xfont format directly from a TTF/OTF font."""

from __future__ import annotations

import argparse
import struct
from PIL import Image, ImageDraw, ImageFont

XFONT_MAGIC = 0x31465A58  # XZF1
XFONT_VERSION = 1
XFONT_HEADER_FMT = "<IHBBHHIIIII"
XFONT_INDEX_FMT = "<IIHHBBbb"


def iter_default_codepoints():
    ranges = [
        (0x3000, 0x303F),  # CJK punctuation
        (0x4E00, 0x9FA5),  # common CJK unified ideographs
        (0xFF00, 0xFFEF),  # fullwidth punctuation/forms
    ]
    seen = set()
    for start, end in ranges:
        for cp in range(start, end + 1):
            if cp not in seen:
                seen.add(cp)
                yield cp


def iter_text_codepoints(path: str):
    seen = set()
    with open(path, "r", encoding="utf-8") as f:
        for ch in f.read():
            cp = ord(ch)
            if cp > 0x7F and cp not in seen:
                seen.add(cp)
                yield cp


def pack_1bpp(img: Image.Image) -> bytes:
    pixels = img.load()
    w, h = img.size
    out = bytearray((w * h + 7) // 8)
    bit = 0
    for y in range(h):
        for x in range(w):
            if pixels[x, y] >= 128:
                out[bit // 8] |= 0x80 >> (bit % 8)
            bit += 1
    return bytes(out)


def glyph_advance(font: ImageFont.FreeTypeFont, ch: str) -> int:
    try:
        return max(1, int(round(font.getlength(ch))))
    except Exception:
        left, top, right, bottom = font.getbbox(ch)
        return max(1, right - left)


def render_glyph(font: ImageFont.FreeTypeFont, cp: int, line_height: int) -> tuple[int, int, int, int, int, bytes] | None:
    ch = chr(cp)
    bbox = font.getbbox(ch)
    if bbox is None:
        return None

    adv_w = glyph_advance(font, ch)
    width = max(1, adv_w)
    image = Image.new("L", (width, line_height), 0)
    draw = ImageDraw.Draw(image)
    draw.text((0, 0), ch, fill=255, font=font)

    if image.getbbox() is None:
        return None

    bitmap = pack_1bpp(image)
    return adv_w, width, line_height, 0, 0, bitmap


def build(font_path: str, out_path: str, size: int, chars_file: str | None) -> None:
    font = ImageFont.truetype(font_path, size=size)
    ascent, descent = font.getmetrics()
    line_height = max(1, ascent + descent)
    codepoints = list(iter_text_codepoints(chars_file) if chars_file else iter_default_codepoints())

    records = []
    bitmaps = bytearray()
    max_bitmap = 0
    index_size = struct.calcsize(XFONT_INDEX_FMT)
    header_size = struct.calcsize(XFONT_HEADER_FMT)
    bitmap_offset_base = header_size + len(codepoints) * index_size

    for cp in codepoints:
        rendered = render_glyph(font, cp, line_height)
        if rendered is None:
            continue
        adv_w, box_w, box_h, ofs_x, ofs_y, bitmap = rendered
        if not bitmap:
            continue
        records.append((cp, adv_w, box_w, box_h, ofs_x, ofs_y, len(bitmap), bytes(bitmap)))
        max_bitmap = max(max_bitmap, len(bitmap))

    bitmap_offset_base = header_size + len(records) * index_size
    out = bytearray()
    out += struct.pack(
        XFONT_HEADER_FMT,
        XFONT_MAGIC,
        XFONT_VERSION,
        1,
        0,
        line_height,
        ascent,
        len(records),
        header_size,
        bitmap_offset_base,
        max_bitmap,
        0,
    )

    cur = bitmap_offset_base
    for cp, adv_w, box_w, box_h, ofs_x, ofs_y, bitmap_size, bitmap in records:
        out += struct.pack(XFONT_INDEX_FMT, cp, cur, bitmap_size, adv_w, box_w, box_h, ofs_x, ofs_y)
        bitmaps += bitmap
        cur += bitmap_size

    out += bitmaps
    with open(out_path, "wb") as f:
        f.write(out)

    print(f"wrote {out_path}: {len(records)} glyphs, {len(out)} bytes, max bitmap {max_bitmap} bytes, size {size}, bpp 1")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("font")
    parser.add_argument("out")
    parser.add_argument("--size", type=int, default=14)
    parser.add_argument("--chars-file", help="Optional UTF-8 text file limiting generated glyphs")
    args = parser.parse_args()
    build(args.font, args.out, args.size, args.chars_file)


if __name__ == "__main__":
    main()