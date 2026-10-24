#!/usr/bin/env python3
"""Exports the phone's own icons for the web page's interactive tour.

- docs/assets/glyphs.svg: Material Symbols Rounded glyphs (the same font files
  the firmware's icon fonts are built from) as an SVG sprite, outline and fill.
- docs/assets/icon-*.png: the launcher's bitmap icons, decoded from the LVGL
  ARGB8888 arrays in main/ui/resources/images.

Run with a Python that has fontTools (SurvivalP4/tools/pyenv):
    tools/pyenv/bin/python firmware/tools/export_site_assets.py
"""
from __future__ import annotations

import re
import struct
import zlib
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets/icon/material_symbols"
IMAGE_DIR = ROOT / "main/ui/resources/images"
OUT = ROOT / "docs/assets"

GLYPHS = {
    "menu_book": 0xEA19, "map": 0xE55B, "photo_library": 0xE413, "mic": 0xE31D,
    "videogame_asset": 0xE338, "arrow_back": 0xE5C4, "search": 0xE8B6,
    "my_location": 0xE55C, "near_me": 0xE569, "explore": 0xE87A,
    "add": 0xE145, "remove": 0xE15B, "send": 0xE163,
    "signal_wifi_4_bar": 0xF065, "energy_savings_leaf": 0xEC1A,
    "volume_off": 0xE04F, "volume_up": 0xE050, "lock": 0xE899, "sunny": 0xE81A,
    "photo_camera": 0xE412, "videocam": 0xE04B, "qr_code_2": 0xE00A,
    "sd_card": 0xE623, "check_circle": 0xF0BE, "undo": 0xE166,
    "refresh": 0xE5D5, "layers": 0xE53B, "bolt": 0xEA0B,
    "settings_input_antenna": 0xE8BF, "eco": 0xEA35, "forest": 0xEA99,
    "local_florist": 0xE545, "close": 0xE5CD, "chevron_right": 0xE5CC,
    "schedule": 0xEFD6, "nutrition": 0xE110, "flip_camera_android": 0xEA37,
}
IMAGES = ["camera_inner_icon_68x68", "music_inner_icon_68x68",
          "settings_inner_icon_68x68", "radio_inner_icon_68x68",
          "files_inner_icon_68x68", "cit_inner_icon_56x68"]


def sprite() -> str:
    symbols = []
    for style, name in (("o", "MaterialSymbolsRounded.ttf"),
                        ("f", "MaterialSymbolsRoundedFill.ttf")):
        font = TTFont(FONT_DIR / name)
        glyphs = font.getGlyphSet()
        cmap = font.getBestCmap()
        upm = font["head"].unitsPerEm
        ascent = font["hhea"].ascent
        for key, codepoint in GLYPHS.items():
            glyph_name = cmap.get(codepoint)
            if glyph_name is None:
                raise SystemExit(f"{name} has no glyph for {key}")
            pen = SVGPathPen(glyphs)
            glyphs[glyph_name].draw(pen)
            # Font units have y up; flip into a square viewBox of one em.
            symbols.append(
                f'<symbol id="{style}-{key}" viewBox="0 0 {upm} {upm}">'
                f'<path transform="matrix(1 0 0 -1 0 {ascent})" '
                f'd="{pen.getCommands()}"/></symbol>')
    return ('<svg xmlns="http://www.w3.org/2000/svg">' + "".join(symbols) +
            "</svg>\n")


def png(width: int, height: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    rows = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4]
                    for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def export_image(stem: str) -> None:
    source = (IMAGE_DIR / f"{stem}.c").read_text()
    width = int(re.search(r"\.header\.w\s*=\s*(\d+)", source).group(1))
    height = int(re.search(r"\.header\.h\s*=\s*(\d+)", source).group(1))
    body = source[source.index("_map[] = {"):source.index("};")]
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", body))
    if len(data) != width * height * 4:
        raise SystemExit(f"{stem}: {len(data)} bytes, expected {width * height * 4}")
    # LVGL ARGB8888 is stored B, G, R, A.
    rgba = bytearray()
    for index in range(0, len(data), 4):
        b, g, r, a = data[index:index + 4]
        rgba += bytes((r, g, b, a))
    name = stem.split("_inner_icon")[0]
    (OUT / f"icon-{name}.png").write_bytes(png(width, height, bytes(rgba)))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "glyphs.svg").write_text(sprite())
    for stem in IMAGES:
        export_image(stem)
    for path in sorted(OUT.iterdir()):
        print(f"{path.name:28} {path.stat().st_size:>8} B")


if __name__ == "__main__":
    main()
