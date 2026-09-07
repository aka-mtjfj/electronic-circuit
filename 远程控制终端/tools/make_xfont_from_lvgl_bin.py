#!/usr/bin/env python3
"""Convert an LVGL v8 binary font into the project's external xfont format.

This avoids loading the full font into MCU RAM. The generated .xfont keeps
fixed-size glyph descriptors in an index table and glyph bitmaps in flash.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass

XFONT_MAGIC = 0x31465A58  # XZF1, little-endian bytes: XZF1
XFONT_VERSION = 1

CMAP_FORMAT0_FULL = 0
CMAP_SPARSE_FULL = 1
CMAP_FORMAT0_TINY = 2
CMAP_SPARSE_TINY = 3

HEADER_FMT = "<IHHHhHhHhhHHBBBBBBBBBBhH"
CMAP_TABLE_FMT = "<IIHHHBB"
XFONT_HEADER_FMT = "<IHBBHHIIIII"
XFONT_INDEX_FMT = "<IIHHBBbb"


@dataclass
class FontHeader:
    version: int
    tables_count: int
    font_size: int
    ascent: int
    descent: int
    typo_ascent: int
    typo_descent: int
    typo_line_gap: int
    min_y: int
    max_y: int
    default_advance_width: int
    kerning_scale: int
    index_to_loc_format: int
    glyph_id_format: int
    advance_width_format: int
    bits_per_pixel: int
    xy_bits: int
    wh_bits: int
    advance_width_bits: int
    compression_id: int
    subpixels_mode: int
    padding: int
    underline_position: int
    underline_thickness: int


@dataclass
class Glyph:
    adv_w: int
    box_w: int
    box_h: int
    ofs_x: int
    ofs_y: int
    bitmap: bytes


class BitReader:
    def __init__(self, data: bytes, offset: int):
        self.data = data
        self.offset = offset
        self.bit_pos = -1
        self.byte_value = 0

    def read_bits(self, n_bits: int) -> int:
        value = 0
        for bit_index in range(n_bits - 1, -1, -1):
            self.byte_value = (self.byte_value << 1) & 0xFF
            self.bit_pos -= 1
            if self.bit_pos < 0:
                self.bit_pos = 7
                self.byte_value = self.data[self.offset]
                self.offset += 1
            bit = 1 if (self.byte_value & 0x80) else 0
            value |= bit << bit_index
        return value

    def read_signed(self, n_bits: int) -> int:
        value = self.read_bits(n_bits)
        sign = 1 << (n_bits - 1)
        if value & sign:
            value -= 1 << n_bits
        return value


def read_label(data: bytes, start: int, label: bytes) -> tuple[int, int]:
    length = struct.unpack_from("<I", data, start)[0]
    got = data[start + 4:start + 8]
    if got != label:
        raise ValueError(f"expected label {label!r} at {start}, got {got!r}")
    return length, start + 8


def parse_header(data: bytes) -> tuple[FontHeader, int]:
    length, content = read_label(data, 0, b"head")
    values = struct.unpack_from(HEADER_FMT, data, content)
    return FontHeader(*values), length


def parse_cmaps(data: bytes, start: int) -> tuple[dict[int, int], int]:
    length, content = read_label(data, start, b"cmap")
    subtables = struct.unpack_from("<I", data, content)[0]
    pos = content + 4
    tables = []
    for _ in range(subtables):
        tables.append(struct.unpack_from(CMAP_TABLE_FMT, data, pos))
        pos += struct.calcsize(CMAP_TABLE_FMT)

    unicode_to_gid: dict[int, int] = {}
    for data_offset, range_start, range_length, glyph_id_start, entries_count, fmt_type, _pad in tables:
        p = start + data_offset
        if fmt_type == CMAP_FORMAT0_FULL:
            offsets = data[p:p + entries_count]
            for rcp, ofs in enumerate(offsets):
                gid = glyph_id_start + ofs
                if gid:
                    unicode_to_gid[range_start + rcp] = gid
        elif fmt_type == CMAP_FORMAT0_TINY:
            for rcp in range(range_length):
                unicode_to_gid[range_start + rcp] = glyph_id_start + rcp
        elif fmt_type in (CMAP_SPARSE_FULL, CMAP_SPARSE_TINY):
            unicode_list = list(struct.unpack_from(f"<{entries_count}H", data, p))
            p += entries_count * 2
            if fmt_type == CMAP_SPARSE_FULL:
                gid_offsets = list(struct.unpack_from(f"<{entries_count}H", data, p))
                for rel, ofs in zip(unicode_list, gid_offsets):
                    unicode_to_gid[range_start + rel] = glyph_id_start + ofs
            else:
                for idx, rel in enumerate(unicode_list):
                    unicode_to_gid[range_start + rel] = glyph_id_start + idx
        else:
            raise ValueError(f"unsupported cmap type {fmt_type}")

    return unicode_to_gid, length


def parse_loca(data: bytes, start: int, header: FontHeader) -> tuple[list[int], int]:
    length, content = read_label(data, start, b"loca")
    count = struct.unpack_from("<I", data, content)[0]
    p = content + 4
    if header.index_to_loc_format == 0:
        offsets = list(struct.unpack_from(f"<{count}H", data, p))
    elif header.index_to_loc_format == 1:
        offsets = list(struct.unpack_from(f"<{count}I", data, p))
    else:
        raise ValueError(f"unsupported loca format {header.index_to_loc_format}")
    return offsets, length


def parse_glyphs(data: bytes, start: int, offsets: list[int], header: FontHeader) -> tuple[list[Glyph], int]:
    length, _content = read_label(data, start, b"glyf")
    nbits = header.advance_width_bits + 2 * header.xy_bits + 2 * header.wh_bits
    glyphs: list[Glyph] = []

    for i, ofs in enumerate(offsets):
        reader = BitReader(data, start + ofs)
        if header.advance_width_bits == 0:
            adv_w = header.default_advance_width
        else:
            adv_w = reader.read_bits(header.advance_width_bits)
        if header.advance_width_format == 0:
            adv_w *= 16
        ofs_x = reader.read_signed(header.xy_bits)
        ofs_y = reader.read_signed(header.xy_bits)
        box_w = reader.read_bits(header.wh_bits)
        box_h = reader.read_bits(header.wh_bits)
        next_ofs = offsets[i + 1] if i < len(offsets) - 1 else length
        bmp_size = max(0, next_ofs - ofs - nbits // 8)

        if i == 0:
            glyphs.append(Glyph(0, 0, 0, 0, 0, b""))
            continue
        if box_w * box_h == 0 or bmp_size == 0:
            glyphs.append(Glyph((adv_w + 8) >> 4, box_w, box_h, ofs_x, ofs_y, b""))
            continue

        if nbits % 8 == 0:
            bmp_start = start + ofs + nbits // 8
            bitmap = data[bmp_start:bmp_start + bmp_size]
        else:
            # Mirror lv_font_loader.c: consume bitmap through the bit reader and align last byte to MSB.
            raw = bytearray()
            for _ in range(bmp_size - 1):
                raw.append(reader.read_bits(8))
            raw.append((reader.read_bits(8 - nbits % 8) << (nbits % 8)) & 0xFF)
            bitmap = bytes(raw)

        glyphs.append(Glyph((adv_w + 8) >> 4, box_w, box_h, ofs_x, ofs_y, bitmap))

    return glyphs, length


def convert(src: str, dst: str, min_codepoint: int) -> None:
    data = open(src, "rb").read()
    header, head_len = parse_header(data)
    if header.compression_id != 0:
        raise ValueError("compressed LVGL fonts are not supported for xfont conversion")

    cmaps_start = head_len
    unicode_to_gid, cmap_len = parse_cmaps(data, cmaps_start)
    loca_start = cmaps_start + cmap_len
    offsets, loca_len = parse_loca(data, loca_start, header)
    glyph_start = loca_start + loca_len
    glyphs, _glyph_len = parse_glyphs(data, glyph_start, offsets, header)

    records = []
    bitmap_blob = bytearray()
    max_bitmap = 0
    for unicode_value, gid in sorted(unicode_to_gid.items()):
        if unicode_value < min_codepoint or gid <= 0 or gid >= len(glyphs):
            continue
        glyph = glyphs[gid]
        if glyph.box_w == 0 or glyph.box_h == 0:
            continue
        bitmap_offset = struct.calcsize(XFONT_HEADER_FMT) + 0  # filled later
        records.append([unicode_value, bitmap_offset, len(glyph.bitmap), glyph])
        max_bitmap = max(max_bitmap, len(glyph.bitmap))

    index_offset = struct.calcsize(XFONT_HEADER_FMT)
    bitmap_offset = index_offset + len(records) * struct.calcsize(XFONT_INDEX_FMT)
    out = bytearray()
    out += struct.pack(
        XFONT_HEADER_FMT,
        XFONT_MAGIC,
        XFONT_VERSION,
        header.bits_per_pixel,
        0,
        max(1, header.ascent - header.descent),
        header.ascent,
        len(records),
        index_offset,
        bitmap_offset,
        max_bitmap,
        0,
    )

    cur_bitmap_offset = bitmap_offset
    for unicode_value, _old_ofs, bmp_size, glyph in records:
        out += struct.pack(
            XFONT_INDEX_FMT,
            unicode_value,
            cur_bitmap_offset,
            bmp_size,
            glyph.adv_w,
            glyph.box_w,
            glyph.box_h,
            glyph.ofs_x,
            glyph.ofs_y,
        )
        bitmap_blob += glyph.bitmap
        cur_bitmap_offset += bmp_size

    out += bitmap_blob
    with open(dst, "wb") as f:
        f.write(out)

    print(f"wrote {dst}: {len(records)} glyphs, {len(out)} bytes, max bitmap {max_bitmap} bytes, bpp {header.bits_per_pixel}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("src", help="LVGL binary font, e.g. wryh_16.bin")
    parser.add_argument("dst", help="Output xfont file")
    parser.add_argument("--min-codepoint", type=lambda s: int(s, 0), default=0x80,
                        help="Skip lower codepoints and use LVGL fallback for ASCII by default")
    args = parser.parse_args()
    convert(args.src, args.dst, args.min_codepoint)


if __name__ == "__main__":
    main()
