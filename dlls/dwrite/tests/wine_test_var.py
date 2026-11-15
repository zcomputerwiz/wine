#!/usr/bin/env python3
# Generates wine_test_var.ttf, a minimal variable TrueType font for the DirectWrite tests: axes wght (100-900)
# and opsz (8-36), four fvar named instances and a STAT table. It has no outlines (two empty glyphs) and no gvar:
# the tests are about how DirectWrite names the named instances (font sets and the WWS and typographic family
# models), not about rendering. Only the Python standard library is used.
#
# Copyright 2026 WineExplorer contributors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA

import struct
import sys

FAMILY = "Wine Var Test"

# name IDs: the standard ones, then fvar and STAT names from 256
NAMES = {
    1: FAMILY, 2: "Regular", 3: "Wine Var Test Regular", 4: "Wine Var Test Regular", 5: "Version 1.000",
    6: "WineVarTest-Regular", 16: FAMILY, 17: "Text",
    256: "Weight", 257: "Optical size",
    258: "Small", 259: "Text", 260: "Display", 261: "Semibold Text",
    262: "Regular", 263: "Semibold",
}

# fvar axes: tag, min, default, max, name ID
AXES = [(b"wght", 100.0, 400.0, 900.0, 256), (b"opsz", 8.0, 12.0, 36.0, 257)]

# named instances: subfamily name ID, coordinates in axis order
INSTANCES = [(258, (400.0, 8.0)), (259, (400.0, 12.0)), (260, (400.0, 36.0)), (261, (600.0, 12.0))]

# STAT axis values (format 1): axis index, flags, value name ID, value; flag 2 = elidable
STAT_VALUES = [(0, 2, 262, 400.0), (0, 0, 263, 600.0),
               (1, 0, 258, 8.0), (1, 0, 259, 12.0), (1, 0, 260, 36.0)]


def fixed(value):
    return struct.pack(">i", int(round(value * 65536)))


def table_head():
    return struct.pack(">IIIIHHqqhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x000B, 1000, 0, 0,
                       0, -200, 1000, 800, 0, 8, 2, 0, 0)


def table_hhea():
    return struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, 800, -200, 0, 500, 0, 0, 500, 1, 0, 0,
                       0, 0, 0, 0, 0, 2)


def table_maxp():
    return struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)


def table_os2():
    return struct.pack(">HhHHHhhhhhhhhhhh10sIIII4sHHHhhhHHIIhhHHH",
                       4, 500, 400, 5, 0, 650, 700, 0, 140, 650, 700, 0, 480, 50, 250, 0,
                       bytes(10), 1, 0, 0, 0, b"WINE", 0x0040, 0x20, 0x20, 800, -200, 0, 800, 200,
                       1, 0, 500, 700, 0, 0x20, 0)


def table_hmtx():
    return struct.pack(">HhHh", 500, 0, 500, 0)


def table_cmap():
    # format 4: U+0020 -> glyph 1, then the end segment
    seg = struct.pack(">HHHHHHHHHHHHHHHH", 4, 32, 0, 4, 4, 1, 0,
                      0x20, 0xFFFF, 0, 0x20, 0xFFFF, (1 - 0x20) & 0xFFFF, 1, 0, 0)
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + seg


def table_post():
    return struct.pack(">IiHhIIIII", 0x00030000, 0, 0, 0, 0, 0, 0, 0, 0)


def table_loca():
    return struct.pack(">HHH", 0, 0, 0)


def table_name():
    records, strings = [], b""
    for nameid in sorted(NAMES):
        data = NAMES[nameid].encode("utf-16-be")
        records.append(struct.pack(">HHHHHH", 3, 1, 0x409, nameid, len(data), len(strings)))
        strings += data
    header = struct.pack(">HHH", 0, len(records), 6 + 12 * len(records))
    return header + b"".join(records) + strings


def table_fvar():
    axis_size, instance_size = 20, 4 + 4 * len(AXES)
    data = struct.pack(">HHHHHHHH", 1, 0, 16, 2, len(AXES), axis_size, len(INSTANCES), instance_size)
    for tag, lo, default, hi, nameid in AXES:
        data += tag + fixed(lo) + fixed(default) + fixed(hi) + struct.pack(">HH", 0, nameid)
    for nameid, coords in INSTANCES:
        data += struct.pack(">HH", nameid, 0) + b"".join(fixed(c) for c in coords)
    return data


def table_stat():
    axis_size = 8
    design_offset = 20
    values_offset = design_offset + axis_size * len(AXES)
    data = struct.pack(">HHHH", 1, 1, axis_size, len(AXES))
    data += struct.pack(">I", design_offset)
    data += struct.pack(">H", len(STAT_VALUES))
    data += struct.pack(">I", values_offset)
    data += struct.pack(">H", 2)          # elidedFallbackNameID: "Regular"
    for ordering, (tag, _lo, _default, _hi, nameid) in enumerate(AXES):
        data += tag + struct.pack(">HH", nameid, ordering)
    offsets, values = b"", b""
    first = 2 * len(STAT_VALUES)
    for axis, flags, nameid, value in STAT_VALUES:
        offsets += struct.pack(">H", first + len(values))
        values += struct.pack(">HHHH", 1, axis, flags, nameid) + fixed(value)
    return data + offsets + values


def checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def build():
    tables = {
        b"OS/2": table_os2(), b"STAT": table_stat(), b"cmap": table_cmap(), b"fvar": table_fvar(),
        b"glyf": b"", b"head": table_head(), b"hhea": table_hhea(), b"hmtx": table_hmtx(),
        b"loca": table_loca(), b"maxp": table_maxp(), b"name": table_name(), b"post": table_post(),
    }
    tags = sorted(tables)
    count = len(tags)
    power = 1
    while power * 2 <= count:
        power *= 2
    header = struct.pack(">IHHHH", 0x00010000, count, power * 16, (power.bit_length() - 1), count * 16 - power * 16)
    offset = 12 + 16 * count
    directory, body = b"", b""
    for tag in tags:
        data = tables[tag]
        directory += tag + struct.pack(">III", checksum(data), offset + len(body), len(data))
        body += data + b"\0" * (-len(data) % 4)
    font = bytearray(header + directory + body)
    # head.checkSumAdjustment
    head_offset = 12 + 16 * count + sum(len(tables[t]) + (-len(tables[t]) % 4) for t in tags[:tags.index(b"head")])
    adjustment = (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF
    font[head_offset + 8:head_offset + 12] = struct.pack(">I", adjustment)
    return bytes(font)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "wine_test_var.ttf"
    with open(out, "wb") as f:
        f.write(build())
