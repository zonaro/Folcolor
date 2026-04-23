
"""
Batch process Windows 7/8, 10 and 11 icon files colorings.
Generates icons for ALL colors defined in:
  https://github.com/zonaro/ColorNameAPI/blob/main/colors.json

Python 3.x — requires: pip install pillow

Base icon HSL statistics (from IconColorStats.py):
  Windows10.ico  — Hue Avg: 45.228, Sat Avg: 74.136, Lum Avg: 92.817
  Windows7_8.ico — Hue Avg: 48.765, Sat Avg: 71.403, Lum Avg: 76.613

The lum_scalar empirical correction factors (1.25 for Win10/11, 1.04 for Win7/8)
were derived by fitting the 14 original hand-tuned entries against the formula:
  lum_scalar ≈ (target_L / base_lum_avg) * lum_adj

References:
  https://opensource.com/article/19/3/python-image-manipulation-tools
  https://docs.python-guide.org/scenarios/imaging/
"""

import io
import os
import re
import sys
import json
import struct
import urllib.request
from colorsys import rgb_to_hls, hls_to_rgb
from PIL import Image   # pip3 install pillow

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

base_path = '../Controller/Resources/'

COLORS_JSON_URL   = "https://raw.githubusercontent.com/zonaro/ColorNameAPI/main/colors.json"
COLORS_JSON_CACHE = "colors.json"   # Local cache to avoid re-downloading

# Base icon statistics (from IconColorStats.py)
WIN10_HUE_AVG = 45.228   # degrees
WIN10_SAT_AVG = 74.136   # %
WIN10_LUM_AVG = 92.817   # %
WIN10_LUM_ADJ = 1.25     # Empirical luminance correction factor for Win10/11

WIN78_HUE_AVG = 48.765
WIN78_SAT_AVG = 71.403
WIN78_LUM_AVG = 76.613
WIN78_LUM_ADJ = 1.04     # Empirical luminance correction factor for Win7/8

# Skip colors whose luminance is >= this threshold (would look almost white)
LUM_SKIP_THRESHOLD = 95.0

# ---------------------------------------------------------------------------
# Color loading and table building
# ---------------------------------------------------------------------------

def load_colors():
    """
    Load the color list from a local cache file, or download it from the
    ColorNameAPI GitHub repository and cache it locally.

    Returns:
        list[dict]: list of {"ID", "Color", "Hexadecimal"} entries.
    """
    if os.path.exists(COLORS_JSON_CACHE):
        print(f"Using local cache: {COLORS_JSON_CACHE}")
        with open(COLORS_JSON_CACHE, encoding='utf-8') as f:
            return json.load(f)

    print(f"Downloading colors from: {COLORS_JSON_URL}")
    with urllib.request.urlopen(COLORS_JSON_URL) as resp:
        data = json.loads(resp.read().decode('utf-8'))

    with open(COLORS_JSON_CACHE, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

    print(f"Cache saved to: {COLORS_JSON_CACHE}")
    return data


def sanitize_filename(name: str) -> str:
    """
    Sanitize a color name string for use as a Windows filename.
    Removes parenthetical variant labels like (Crayola) or (web),
    strips invalid path characters and collapses whitespace.

    Args:
        name: raw color name from the JSON.

    Returns:
        Clean string safe for use as a filename (without extension).
    """
    # Remove parenthetical qualifiers, e.g. "(Crayola)", "(web)"
    name = re.sub(r'\s*\([^)]*\)', '', name)
    # Strip characters forbidden in Windows filenames
    name = re.sub(r'[<>:"/\\|?*\'&]', '', name)
    # Collapse runs of whitespace to a single space
    name = re.sub(r'\s+', ' ', name).strip()
    return name[:80]


def hex_to_hsl(hex_color: str):
    """
    Convert a hex color string to HSL.

    Args:
        hex_color: color in "#RRGGBB" format (case-insensitive).

    Returns:
        Tuple (H, S, L) where H is 0-360 degrees, S and L are 0-100 percent.
    """
    h_str = hex_color.lstrip('#')
    r = int(h_str[0:2], 16) / 255.0
    g = int(h_str[2:4], 16) / 255.0
    b = int(h_str[4:6], 16) / 255.0
    h, l, s = rgb_to_hls(r, g, b)
    return h * 360.0, s * 100.0, l * 100.0


def build_table(colors, icon_set_folder: str,
                base_hue: float, base_sat: float, base_lum: float,
                lum_adj: float) -> list:
    """
    Build the color tweak table for a specific icon set from the color list.

    For each color the following values are computed:
      hue_offset  = target_hue - base_hue_avg
                    (shifts average pixel hue toward the target color)
      sat_scalar  = target_S / base_sat_avg   (clamped to [0, 3])
      lum_scalar  = (target_L / base_lum_avg) * lum_adj   (clamped to [0.05, 2.5])

    Colors with luminance >= LUM_SKIP_THRESHOLD are skipped as they would
    produce near-white icons indistinguishable from the default folder.

    Args:
        colors:           list of color dicts from colors.json.
        icon_set_folder:  output sub-folder name, e.g. "Win10set".
        base_hue:         average hue of the source icon (degrees).
        base_sat:         average saturation of the source icon (%).
        base_lum:         average luminance of the source icon (%).
        lum_adj:          empirical correction factor for luminance scalar.

    Returns:
        List of [out_path, hue_offset, sat_scalar, lum_scalar] entries.
    """
    seen_names: set = set()
    table: list = []
    skipped = 0

    for color in colors:
        raw_name = color.get('Color', '').strip()
        hex_val  = color.get('Hexadecimal', '').strip()

        filename = sanitize_filename(raw_name)
        if not filename:
            skipped += 1
            continue

        # Resolve duplicate sanitized names by appending the original ID
        if filename in seen_names:
            filename = f"{filename} {color.get('ID', '')}"
        if filename in seen_names:
            skipped += 1
            continue
        seen_names.add(filename)

        try:
            h, s, l = hex_to_hsl(hex_val)
        except (ValueError, IndexError):
            skipped += 1
            continue

        # Skip near-white colors — they are barely visible as folder tints
        if l >= LUM_SKIP_THRESHOLD:
            skipped += 1
            continue

        # Hue offset so the average pixel hue shifts from base to target
        hue_off = -base_hue + h

        # Saturation scalar derived from base average
        sat_scalar = (s / base_sat) if base_sat > 0 else 0.0
        sat_scalar = max(0.0, min(3.0, sat_scalar))

        # Luminance scalar with empirical correction factor
        lum_scalar = ((l / base_lum) * lum_adj) if base_lum > 0 else 0.5
        lum_scalar = max(0.05, min(2.5, lum_scalar))

        out_path = f"{icon_set_folder}/{filename}.ico"
        table.append([out_path, hue_off, sat_scalar, lum_scalar])

    print(f"  -> {len(table)} icons to generate, {skipped} entries skipped")
    return table


# Apply hue to a copy of the base image and save it
def apply_color(im, out_path, hue_o, sat_s, lum_s, isWin10):
    """
    Apply a hue/saturation/luminance transformation to the base icon and write
    the result to disk as a multi-size ICO file.

    Args:
        im:        PIL Image object for the source ICO file.
        out_path:  relative output path inside base_path (e.g. "Win10set/Blue.ico").
        hue_o:     hue offset in degrees (will be divided by 360 internally).
        sat_s:     saturation scalar multiplier.
        lum_s:     luminance scalar multiplier.
        isWin10:   True for Win10/11 icons (no label masking); False for Win7/8.
    """
    print("%s:" % out_path)

    # Ensure output directory exists
    out_full = base_path + out_path
    os.makedirs(os.path.dirname(out_full), exist_ok=True)

    # PIL uses scalar values (0-1 range)
    hue_o /= 360.0

    label_x_limit = 1000
    # Apply color tweak to each sub-icon from the base input set
    out_frames = []
    for i in range(im.ico.nb_items):
        png_im = im.ico.frame(i)
        pxi = png_im.load()
        print(" [%u] size: %u" % (i, png_im.width))

        if not isWin10:   # Approximate Win 7/8 label x start position
            label_x_limit = (png_im.width * 0.6)

        new_im = Image.new("RGBA", (png_im.width, png_im.width), "purple")
        pxo = new_im.load()

        for y in range(png_im.width):
            for x in range(png_im.width):
                # Pixel from RGB to HLS color space
                r, g, b, a = pxi[x, y]

                # No need to colorize transparent pixels
                if a > 0:
                    h, l, s = rgb_to_hls(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0)

                    # Leave the Windows 7/8 folder label region untouched
                    if not ((x >= label_x_limit) and (h < (40.0 / 360.0)) or (h > (100.0 / 360.0))):
                        # Tweak pixel
                        h = (h + hue_o)
                        # Hue is circular — wrap around both ends
                        if h > 1.0:
                            h -= 1.0
                        elif h < 0.0:
                            h += 1.0
                        s = min(1.0, (s * sat_s))
                        l = min(1.0, (l * lum_s))

                        # Back to RGB from HLS
                        r, g, b = hls_to_rgb(h, l, s)
                        r = min(int(r * 255.0), 255)
                        g = min(int(g * 255.0), 255)
                        b = min(int(b * 255.0), 255)

                pxo[x, y] = (r, g, b, a)

        #new_im.show()
        out_frames.append(new_im)

    # Write the multi-size ICO file once, after all frames are collected.
    # PIL supports loading embedded ICO files but has limited write support
    # (only saves scaled thumbnails of the first frame). Instead we write the
    # ICO binary format directly, storing each frame as a compressed PNG
    # section (supported since Windows Vista), which keeps file size much
    # smaller than the default BMP format used by most icon editors.
    fp = open(out_full, 'wb')
    fp.write(b"\0\0\1\0")  # Magic
    fp.write(struct.pack("<H", len(out_frames)))  # idCount(2)
    offset = fp.tell() + len(out_frames) * 16

    for ni in out_frames:
        width, height = ni.width, ni.height
        # 0 means 256
        fp.write(struct.pack("B", width if width < 256 else 0))   # bWidth(1)
        fp.write(struct.pack("B", height if height < 256 else 0)) # bHeight(1)
        fp.write(b"\0")  # bColorCount(1)
        fp.write(b"\0")  # bReserved(1)
        fp.write(b"\0\0")  # wPlanes(2)
        fp.write(struct.pack("<H", 32))  # wBitCount(2)

        image_io = io.BytesIO()
        ni.save(image_io, "png")
        image_io.seek(0)
        image_bytes = image_io.read()
        bytes_len = len(image_bytes)
        fp.write(struct.pack("<I", bytes_len))  # dwBytesInRes(4)
        fp.write(struct.pack("<I", offset))     # dwImageOffset(4)
        current = fp.tell()
        fp.seek(offset)
        fp.write(image_bytes)
        offset = offset + bytes_len
        fp.seek(current)

    fp.close()
    print(" ")


# From Windows base icon image to our color sets
def process_icon_base(in_file, nfo_table, isWin10):
    """
    Load the base icon file and generate all color variant ICO files defined
    in nfo_table.

    Args:
        in_file:   path to the source ICO template (e.g. "Windows10.ico").
        nfo_table: list of [out_path, hue_offset, sat_scalar, lum_scalar].
        isWin10:   True for Win10/11 icons; False for Win7/8.
    """
    # Load base icon image
    im = Image.open(in_file)

    # Build color sets
    for e in nfo_table:
        apply_color(im, e[0], e[1], e[2], e[3], isWin10)

# Windows 11
# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    colors = load_colors()
    print(f"\nTotal colors loaded: {len(colors)}")

    # Build color tweak tables for each platform
    print("\nBuilding table for Windows 11..")
    win11_table = build_table(colors, 'Win11set',
                              WIN10_HUE_AVG, WIN10_SAT_AVG, WIN10_LUM_AVG, WIN10_LUM_ADJ)

    print("\nBuilding table for Windows 10..")
    win10_table = build_table(colors, 'Win10set',
                              WIN10_HUE_AVG, WIN10_SAT_AVG, WIN10_LUM_AVG, WIN10_LUM_ADJ)

    print("\nBuilding table for Windows 7 & 8..")
    win7_8_table = build_table(colors, 'Win7_8set',
                               WIN78_HUE_AVG, WIN78_SAT_AVG, WIN78_LUM_AVG, WIN78_LUM_ADJ)

    # Generate icon sets
    print("\n-- Generating Windows 11 set..")
    process_icon_base("Windows11.ico", win11_table, True)

    print("-- Generating Windows 10 set..")
    process_icon_base("Windows10.ico", win10_table, True)

    print("-- Generating Windows 7 & 8 set..")
    process_icon_base("Windows7_8_rgba.ico", win7_8_table, False)

    print("\nDone!")
