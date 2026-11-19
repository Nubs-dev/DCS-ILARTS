"""Builds the ILARTS.exe icons (assets/ILARTS-dark.ico, assets/ILARTS-light.ico) from the artwork in assets/.

Each .ico holds 16, 24, 32, 48, 64, 128 and 256 px images, stored as PNG (Windows Vista and later read these).
The 24-256 px PNGs come from assets/ as they are; 16 and 48 px are rendered from the SVG with Inkscape.
Run it again after changing the artwork, then rebuild ILARTS.exe (viewer/ILARTS.rc embeds the .ico files) and the
installer (deploy/ILARTS.iss uses ILARTS-light.ico).

    python tools/make_icon.py [--inkscape "C:/Program Files/Inkscape/bin/inkscape.exe"]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
SIZES = [16, 24, 32, 48, 64, 128, 256]
RENDERED = {16, 48}  # no PNG in assets/: rendered from the SVG


def png_size(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("not a PNG")
    return struct.unpack(">II", data[16:24])


def write_ico(path, images):
    """images: list of (size, png bytes), smallest first."""
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        dim = 0 if size >= 256 else size  # 0 means 256 in an ICONDIRENTRY
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    with open(path, "wb") as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inkscape", default=r"C:\Program Files\Inkscape\bin\inkscape.exe")
    args = ap.parse_args()
    if not os.path.isfile(args.inkscape):
        sys.exit(f"Inkscape not found at {args.inkscape} (pass --inkscape)")
    with tempfile.TemporaryDirectory() as tmp:
        for variant in ("dark", "light"):
            images = []
            for size in SIZES:
                if size in RENDERED:
                    src = os.path.join(tmp, f"{variant}-{size}.png")
                    svg = os.path.join(ASSETS, f"ILARTS-icon-{variant}-256.svg")
                    subprocess.run([args.inkscape, svg, "--export-type=png", f"--export-filename={src}",
                                    f"--export-width={size}", f"--export-height={size}"],
                                   check=True, capture_output=True)
                else:
                    src = os.path.join(ASSETS, f"ILARTS-icon-{variant}-{size}.png")
                with open(src, "rb") as f:
                    data = f.read()
                w, h = png_size(data)
                if (w, h) != (size, size):
                    sys.exit(f"{src}: {w}x{h}, expected {size}x{size}")
                images.append((size, data))
            dst = os.path.join(ASSETS, f"ILARTS-{variant}.ico")
            write_ico(dst, images)
            print(f"{os.path.relpath(dst, ROOT)}: {', '.join(str(s) for s, _ in images)} px")


if __name__ == "__main__":
    main()
