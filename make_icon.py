#!/usr/bin/env python3
"""
make_icon.py — Generate icon.png and icon.ico for DmgConverter.

Apple-style rounded-square icon: dark navy background, silver optical disc
with iridescent rainbow band, centre hole, "DMG → ISO" label at bottom.

Run:  python make_icon.py
Output: icon.png (1024×1024) and icon.ico (multi-size) in the current directory.
"""

from PIL import Image, ImageDraw, ImageFont
import math, struct, io, os

# ── Canvas ────────────────────────────────────────────────────────────────────
SIZE   = 1024
RADIUS = 200          # rounded-square corner radius

BG_COLOR   = (42, 48, 65, 255)    # dark navy
MASK_COLOR = (0, 0, 0, 0)        # transparent (outside rounded square)

# ── Disc geometry ─────────────────────────────────────────────────────────────
# Keep the disc fully inside the rounded square.
# At the corners the mask cuts in RADIUS pixels from each edge.
# The usable circle that fits inside the rounded square has:
#   max radius = SIZE/2 - RADIUS  = 512 - 200 = 312
# Use 290 to leave a comfortable margin.
DISC_R     = 290
CX         = SIZE // 2           # 512
CY         = SIZE // 2 - 20     # shift slightly upward to leave room for label

HOLE_R     = 38                  # centre spindle hole
BAND_W     = 80                  # width of iridescent band ring


def make_rounded_mask(size, radius):
    """Return an 'L' mask that is 255 inside the rounded square, 0 outside."""
    mask = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return mask


def draw_disc(draw, cx, cy, disc_r, hole_r, band_w):
    """Draw a silver optical disc with an iridescent band and centre hole."""

    # ── Silver base ───────────────────────────────────────────────────────────
    draw.ellipse(
        [cx - disc_r, cy - disc_r, cx + disc_r, cy + disc_r],
        fill=(200, 200, 210)
    )

    # ── Subtle radial gradient rings (simulate brushed metal) ─────────────────
    steps = 18
    for i in range(steps):
        t = i / steps
        r = int(disc_r * (1 - t * 0.55))
        shade = int(160 + 60 * t)
        color = (shade, shade, shade + 10)
        draw.ellipse(
            [cx - r, cy - r, cx + r, cy + r],
            outline=color, width=2
        )

    # ── Iridescent band ───────────────────────────────────────────────────────
    band_inner = disc_r - band_w
    band_outer = disc_r - 8
    rainbow = [
        (255, 80,  80),   # red
        (255, 180,  0),   # orange-yellow
        (80,  220,  80),  # green
        (60,  180, 255),  # blue
        (200,  80, 255),  # violet
        (255,  80, 160),  # pink
    ]
    slices = len(rainbow)
    for i, col in enumerate(rainbow):
        start_angle = i * (360 / slices)
        end_angle   = (i + 1) * (360 / slices)
        draw.arc(
            [cx - band_outer, cy - band_outer, cx + band_outer, cy + band_outer],
            start=start_angle, end=end_angle,
            fill=col, width=band_w
        )

    # Overlay a semi-transparent white sheen over the band
    # (achieved by drawing lighter arcs on top)
    for i in range(slices):
        start_angle = i * (360 / slices)
        end_angle   = start_angle + (360 / slices) * 0.5
        r, g, b = rainbow[i]
        light = (min(r + 80, 255), min(g + 80, 255), min(b + 80, 255))
        draw.arc(
            [cx - band_outer + 6, cy - band_outer + 6,
             cx + band_outer - 6, cy + band_outer - 6],
            start=start_angle, end=end_angle,
            fill=light, width=band_w // 3
        )

    # ── Centre data area (dark hub ring) ──────────────────────────────────────
    data_r = band_inner - 4
    draw.ellipse(
        [cx - data_r, cy - data_r, cx + data_r, cy + data_r],
        fill=(170, 172, 180)
    )

    # Inner silver hub
    hub_r = hole_r + 22
    draw.ellipse(
        [cx - hub_r, cy - hub_r, cx + hub_r, cy + hub_r],
        fill=(210, 212, 218)
    )

    # ── Centre hole ───────────────────────────────────────────────────────────
    draw.ellipse(
        [cx - hole_r, cy - hole_r, cx + hole_r, cy + hole_r],
        fill=(42, 48, 65)    # same as background = punched through
    )

    # Hole edge highlight
    draw.ellipse(
        [cx - hole_r, cy - hole_r, cx + hole_r, cy + hole_r],
        outline=(130, 130, 140), width=3
    )


def draw_label(draw, cx, bottom_y):
    """Draw 'DMG → ISO' label near the bottom of the icon."""
    font_size = 72
    try:
        font = ImageFont.truetype("arialbd.ttf", font_size)
    except Exception:
        try:
            font = ImageFont.truetype("arial.ttf", font_size)
        except Exception:
            font = ImageFont.load_default()

    text = "DMG \u2192 ISO"

    # Measure text
    bbox = draw.textbbox((0, 0), text, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]

    tx = cx - tw // 2
    ty = bottom_y - th - 30

    # Drop shadow
    draw.text((tx + 3, ty + 3), text, font=font, fill=(0, 0, 0, 180))
    # Main text (bright white)
    draw.text((tx, ty), text, font=font, fill=(230, 235, 255, 255))


def build_ico(png_path, ico_path):
    """Build a proper multi-size .ico from the 1024×1024 PNG."""
    sizes = [256, 128, 64, 48, 32, 16]
    images = []
    src = Image.open(png_path).convert("RGBA")
    for s in sizes:
        img = src.resize((s, s), Image.LANCZOS)
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        images.append(buf.getvalue())

    # ICO file format
    count = len(images)
    header = struct.pack("<HHH", 0, 1, count)   # reserved, type=1 (ICO), count
    offset = 6 + count * 16                     # header + directory entries
    directory = b""
    for i, data in enumerate(images):
        s = sizes[i]
        w = s if s < 256 else 0
        h = s if s < 256 else 0
        directory += struct.pack("<BBBBHHII",
            w, h,          # width, height (0 = 256)
            0,             # color count (0 = no palette)
            0,             # reserved
            1,             # planes
            32,            # bit count
            len(data),     # size of image data
            offset         # offset of image data
        )
        offset += len(data)

    with open(ico_path, "wb") as f:
        f.write(header)
        f.write(directory)
        for data in images:
            f.write(data)


def main():
    # ── Create RGBA canvas ────────────────────────────────────────────────────
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    bg     = Image.new("RGBA", (SIZE, SIZE), BG_COLOR)

    # Draw on a temporary layer (so we can apply the rounded mask cleanly)
    layer = Image.new("RGBA", (SIZE, SIZE), BG_COLOR)
    draw  = ImageDraw.Draw(layer)

    # Disc
    draw_disc(draw, CX, CY, DISC_R, HOLE_R, BAND_W)

    # Label — positioned below the disc
    label_bottom = CY + DISC_R + 10   # just below disc edge
    # Clamp so label stays inside icon
    label_bottom = min(label_bottom, SIZE - 20)
    draw_label(draw, CX, label_bottom)

    # Apply rounded-square mask
    mask = make_rounded_mask(SIZE, RADIUS)
    canvas.paste(layer, mask=mask)

    # Save PNG
    png_path = "icon.png"
    canvas.save(png_path)
    print(f"Saved {png_path}")

    # Save ICO
    ico_path = "icon.ico"
    build_ico(png_path, ico_path)
    print(f"Saved {ico_path}")


if __name__ == "__main__":
    main()
