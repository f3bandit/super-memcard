#!/usr/bin/env python3
"""Super MemCard: the default save icon of each system, drawn here from scratch (no logos, no characters).

Like a PS1 save's icon: 16x16, 16 colours (PS1 BGR555), up to 3 frames. Writes
  icons/<SYSTEM>.png         the icon (1x), for Console Mode's menu or anything else
  icons/sheet.png            every icon at 8x with its system, to look at
  host/smc_icons.c           the icons as icon.bin bytes for the host library (smc_system_icon())

icon.bin, the file the card keeps beside a game's saves (docs/05-design.md):
  'S' 'M' 'I' 'C'   magic
  version (1), frames (1-3), width (16), height (16)
  palette: 16 x u16 little-endian, PS1 colour (r | g << 5 | b << 10, 5 bits each; colour 0 is transparent)
  frames x 128 bytes: 4 bits a pixel, left pixel in the low nibble, rows top to bottom (as a PS1 save icon)

Run: python3 tools/icons/make_icons.py      (needs Pillow)
Copyright (C) 2026 f3bandit.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

# 5-pixel-high font for the labels (most letters 3 wide)
FONT = {
    "0": ["111", "101", "101", "101", "111"], "1": ["11", "01", "01", "01", "01"],
    "2": ["111", "001", "111", "100", "111"], "3": ["111", "001", "011", "001", "111"],
    "4": ["101", "101", "111", "001", "001"], "6": ["111", "100", "111", "101", "111"],
    "A": ["010", "101", "111", "101", "101"], "B": ["110", "101", "110", "101", "110"],
    "C": ["011", "100", "100", "100", "011"], "D": ["110", "101", "101", "101", "110"],
    "E": ["111", "100", "110", "100", "111"], "G": ["011", "100", "101", "101", "011"],
    "J": ["001", "001", "001", "101", "010"], "M": ["10001", "11011", "10101", "10001", "10001"],
    "N": ["110", "101", "101", "101", "101"], "O": ["010", "101", "101", "101", "010"],
    "P": ["110", "101", "110", "100", "100"], "S": ["011", "100", "010", "001", "110"],
    "T": ["111", "010", "010", "010", "010"], "X": ["101", "101", "010", "101", "101"],
}

# system (Main's save folder) -> shape, body colour, label colour, label
SYSTEMS = [
    ("NES",       "cart",   (150, 150, 158), (200, 40, 40),   "NES"),
    ("SNES",      "cart",   (176, 176, 190), (110, 80, 170),  "SNES"),
    ("N64",       "cart",   (70, 70, 80),    (240, 200, 40),  "N64"),
    ("GAMEBOY",   "handheld", (190, 190, 180), (60, 110, 60), "GB"),
    ("GBC",       "handheld", (120, 60, 170), (60, 110, 60),  "GBC"),   # Console Mode keeps Color saves apart
    ("GBA",       "wide",   (90, 70, 170),   (60, 110, 60),   "GBA"),
    ("SMS",       "cart",   (40, 40, 44),    (220, 50, 50),   "SMS"),
    ("Genesis",   "cart",   (36, 36, 40),    (240, 200, 60),  "GEN"),
    ("MegaDrive", "cart",   (36, 36, 40),    (70, 140, 230),  "MD"),
    ("MegaCD",    "disc",   (180, 190, 210), (70, 140, 230),  "MCD"),
    ("S32X",      "cart",   (50, 50, 56),    (230, 70, 60),   "32X"),
    ("Saturn",    "disc",   (170, 175, 195), (90, 90, 110),   "SAT"),
    ("TGFX16",    "card",   (230, 230, 225), (220, 120, 30),  "TG16"),
    ("Jaguar",    "cart",   (40, 40, 44),    (220, 40, 40),   "JAG"),
    ("NEOGEO",    "cart",   (40, 40, 48),    (230, 200, 60),  "NEO"),
]
# No PSX: a PS1 save carries its own icon (its memory card directory) and Console Mode already shows it.
# (N64 saves have none of their own: a Controller Pak note is a game code and a name, EEPROM/SRAM bare data.)

OUTLINE = (16, 16, 20)
WHITE = (240, 240, 240)
GOLD = (210, 180, 90)


def text_width(s):
    return sum(len(FONT[c][0]) + 1 for c in s) - 1


def draw_text(px, s, x, y, col):
    for ch in s:
        for r, row in enumerate(FONT[ch]):
            for c, bit in enumerate(row):
                if bit == "1":
                    px[x + c, y + r] = col
        x += len(FONT[ch][0]) + 1


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


def draw(shape, body, label_col, label):
    img = Image.new("RGB", (16, 16), (0, 0, 0))  # black = transparent (colour 0)
    d = ImageDraw.Draw(img)
    px = img.load()
    if shape == "cart":
        # a cartridge: body with a grip notch at the top, contacts at the bottom, a label
        d.rectangle([1, 1, 14, 14], fill=body, outline=OUTLINE)
        d.rectangle([4, 1, 11, 2], fill=shade(body, 0.75), outline=OUTLINE)
        d.rectangle([0, 4, 15, 10], fill=label_col)   # the label band runs edge to edge: 4 letters fit
        d.line([0, 4, 15, 4], fill=OUTLINE)
        d.line([0, 10, 15, 10], fill=OUTLINE)
        for x in range(3, 13, 2):
            px[x, 13] = GOLD
        tw = text_width(label)
        draw_text(px, label, (16 - tw + 1) // 2, 5, WHITE)
    elif shape == "card":
        # a credit-card-size game card (HuCARD-like shape, no markings): wide and flat
        d.rectangle([0, 3, 15, 13], fill=body, outline=OUTLINE)
        d.rectangle([1, 4, 14, 10], fill=label_col)
        for x in range(2, 14, 2):
            px[x, 12] = GOLD
        tw = text_width(label)
        draw_text(px, label, (16 - tw + 1) // 2, 5, WHITE)
    elif shape == "disc":
        # a disc with its label band
        d.ellipse([0, 0, 15, 15], fill=body, outline=OUTLINE)
        d.ellipse([3, 3, 12, 12], fill=shade(body, 1.15), outline=None)
        d.rectangle([1, 5, 14, 10], fill=label_col)
        d.line([1, 5, 14, 5], fill=OUTLINE)
        d.line([1, 10, 14, 10], fill=OUTLINE)
        tw = text_width(label)
        draw_text(px, label, (16 - tw) // 2, 5 + 1, WHITE)
        px[7, 2] = px[8, 2] = WHITE  # a glint
    elif shape == "handheld":
        # a tall handheld: screen with the label, a d-pad and two buttons
        d.rectangle([2, 0, 13, 15], fill=body, outline=OUTLINE)
        d.rectangle([3, 1, 12, 7], fill=(70, 70, 80), outline=None)
        d.rectangle([4, 2, 11, 6], fill=label_col)
        draw_text(px, label, 4 + (8 - text_width(label)) // 2, 2, (200, 230, 160))
        for x, y in ((4, 11), (5, 10), (5, 11), (5, 12), (6, 11)):
            px[x, y] = OUTLINE
        px[10, 10] = px[11, 11] = (170, 40, 90)
    elif shape == "wide":
        # a wide handheld: screen in the middle with the label, d-pad left, buttons right
        d.rounded_rectangle([0, 3, 15, 13], radius=2, fill=body, outline=OUTLINE)
        d.rectangle([2, 4, 13, 10], fill=label_col)
        draw_text(px, label, 2 + (12 - text_width(label) + 1) // 2, 5, (200, 230, 160))
        for x, y in ((1, 12), (2, 11), (2, 12), (3, 12)):
            px[x, y] = OUTLINE
        px[12, 12] = px[14, 11] = (220, 220, 230)
    return img


def to_ps1(img):
    """16 colours (0 transparent) -> (palette as u16 list, 128 bytes of 4bpp pixels)."""
    cols = []
    idx = []
    for y in range(16):
        for x in range(16):
            c = img.getpixel((x, y))
            if c == (0, 0, 0):
                idx.append(0)
                continue
            q = tuple(v >> 3 for v in c)
            if q not in cols:
                cols.append(q)
            idx.append(cols.index(q) + 1)
    assert len(cols) <= 15, f"{len(cols)} colours"
    pal = [0] + [r | g << 5 | b << 10 for (r, g, b) in cols]
    pal += [0] * (16 - len(pal))
    data = bytes(idx[i] | idx[i + 1] << 4 for i in range(0, 256, 2))
    return pal, data


def icon_bin(img):
    pal, data = to_ps1(img)
    out = bytearray(b"SMIC") + bytes([1, 1, 16, 16])
    for p in pal:
        out += bytes([p & 0xFF, p >> 8])
    return bytes(out + data)


def main():
    outdir = os.path.join(ROOT, "icons")
    os.makedirs(outdir, exist_ok=True)
    sheet = Image.new("RGB", (5 * 160, 3 * 170), (36, 36, 44))
    sd = ImageDraw.Draw(sheet)
    c = ["/* Generated by tools/icons/make_icons.py: the default save icon of each system (icon.bin, docs/05-design.md).",
         " * Do not edit; change the script and run it again. */",
         '#include "smc_sync.h"', "", "#include <string.h>", ""]
    table = []
    for i, (sysname, shape, body, lab, label) in enumerate(SYSTEMS):
        img = draw(shape, body, lab, label)
        img.save(os.path.join(outdir, f"{sysname}.png"))
        b = icon_bin(img)
        name = "icon_" + sysname.lower()
        c.append(f"static const uint8_t {name}[{len(b)}] = {{")
        for k in range(0, len(b), 16):
            c.append("    " + ", ".join(f"0x{v:02x}" for v in b[k:k + 16]) + ",")
        c.append("};")
        table.append(f'    {{"{sysname}", {name}, sizeof({name})}},')
        big = img.resize((128, 128), Image.NEAREST)
        x, y = (i % 5) * 160 + 16, (i // 5) * 170 + 10
        # checkerboard behind transparent pixels
        bg = Image.new("RGB", (128, 128))
        bd = ImageDraw.Draw(bg)
        for yy in range(16):
            for xx in range(16):
                bd.rectangle([xx * 8, yy * 8, xx * 8 + 7, yy * 8 + 7], fill=(60, 60, 70) if (xx + yy) % 2 else (75, 75, 88))
        mask = Image.eval(img.convert("L"), lambda v: 255 if v else 0).resize((128, 128), Image.NEAREST)
        bg.paste(big, (0, 0), mask)
        sheet.paste(bg, (x, y))
        sd.text((x, y + 134), sysname, fill=(230, 230, 230))
    sheet.save(os.path.join(outdir, "sheet.png"))
    c += ["", "static const struct { const char *sys; const uint8_t *data; size_t len; } icons[] = {"] + table + ["};", ""]
    c += ["const uint8_t *smc_system_icon(const char *sys, size_t *len) {",
          "    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)",
          "        if (!strcmp(icons[i].sys, sys)) {",
          "            *len = icons[i].len;",
          "            return icons[i].data;",
          "        }",
          "    return NULL;",
          "}", ""]
    with open(os.path.join(ROOT, "host", "smc_icons.c"), "w") as f:
        f.write("\n".join(c))
    print(f"{len(SYSTEMS)} icons, {len(b)} bytes each (icon.bin)")


if __name__ == "__main__":
    main()
