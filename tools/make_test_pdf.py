"""Make a test PDF shaped like a Snipe-IT TZe_24mm_A label (65 x 24 mm, one label per page)."""
import sys
from PIL import Image, ImageDraw, ImageFont

DPI = 600
MM = DPI / 25.4


def label(tag: str, length_mm: float = 65.0) -> Image.Image:
    w, h = round(length_mm * MM), round(24 * MM)
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    m = 3.2 * MM  # Snipe-IT TZe_24mm margins
    # Fake 2D barcode block at the left, like Snipe-IT's QR
    q = h - 2 * m
    cell = q / 9
    for r in range(9):
        for c in range(9):
            if (r * 7 + c * 3) % 5 < 2 or r in (0, 8) or c in (0, 8):
                d.rectangle([m + c * cell, m + r * cell, m + (c + 1) * cell, m + (r + 1) * cell], fill=0)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", round(3.2 * MM))
        small = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", round(2.2 * MM))
    except OSError:
        font = small = ImageFont.load_default()
    x = m + q + 1.4 * MM
    d.text((x, m), "Example Co", font=font, fill=0)
    d.text((x, m + 4.5 * MM), f"Tag: {tag}", font=font, fill=0)
    d.text((x, m + 9 * MM), "TOP-LEFT reads first", font=small, fill=0)
    # Arrow pointing to the label's end (right)
    d.polygon([(w - m, h / 2 + 4 * MM), (w - m - 4 * MM, h / 2 + 1.5 * MM), (w - m - 4 * MM, h / 2 + 6.5 * MM)], fill=0)
    # Thin frame at the printable band edges (18.1 mm centred) to show the crop
    band = (24 - 18.1) / 2 * MM
    d.line([(0, band), (w, band)], fill=128)
    return img


out = sys.argv[1] if len(sys.argv) > 1 else "test.pdf"
pages = [label("ASSET-00042"), label("ASSET-00043")]
pages[0].save(out, "PDF", resolution=DPI, save_all=True, append_images=pages[1:])
print(out)
