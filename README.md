# Brother PT-P710BT CUPS driver for macOS (24 mm tape)

Makes the PT-P710BT (P-touch Cube Plus) a normal macOS printer, so Snipe-IT asset
labels print straight from the browser's print dialog. USB only, 24 mm TZe tape only.

Brother ships no macOS driver for this model, just P-touch Editor. This driver
follows Brother's published protocol:
[Raster Command Reference PT-E550W/P750W/P710BT](https://download.brother.com/welcome/docp100064/cv_pte550wp750wp710bt_eng_raster_102.pdf).

It only installs software on the Mac. Nothing on the printer changes, and P-touch
Editor, the mobile app and Bluetooth keep working.

## Requirements

- macOS with Xcode Command Line Tools (`xcode-select --install`)
- Python 3 with Pillow, only for `make test`

## Install

Plug the printer in over USB and switch it on, then run:

```sh
make
make install      # sudo: installs the filter + PPD and creates queue "Brother_PT_P710BT"
```

## Printing from Snipe-IT

1. Generate labels in Snipe-IT. Either label engine works:
   - **Legacy engine** (HTML labels), e.g. 1.5" x 0.68" labels with 0 page margins
   - **New label engine** (PDF), with a `Tapes\Brother\TZe_24mm_*` template
2. Press Cmd+P and choose **Brother PT-P710BT (24mm)**.
3. Set **Paper size**:

   | Snipe-IT template | Paper size |
   |---|---|
   | TZe_24mm_C | 34 x 24 mm |
   | TZe_24mm_E | 45 x 24 mm |
   | TZe_24mm_A, TZe_24mm_D | 65 x 24 mm |
   | TZe_24mm_B | 73 x 24 mm |
   | Legacy engine (1.5" x 0.68" labels) | 78 x 24 mm (default, same as the Windows driver) |

4. Leave Chrome's **Scale** at **Default**. Margins don't matter: the driver centres
   whatever is printed on the part of the tape the print head reaches.

Each PDF page is one label and is cut after printing.

## Snipe-IT settings that work

Tested with the **legacy** label engine (Admin Settings → Labels):

| Setting | Value |
|---|---|
| New Label Engine | Off |
| Display 1D barcode | On, type `C128` |
| Display 2D barcode | On, type `QRCODE` |
| Labels per page | 1 |
| Label font size | 9 pt |
| Label dimensions | 1.5 w x 0.68 h (inches) |
| Label spacing | 0 horizontal, 0 vertical |
| Page margins | 0 top, bottom, left, right |
| Page dimensions | 1.5 w x 0.68 h (inches) |
| Visible fields | Asset Name, Serial, Asset Tag, Model, Company Name |

Print with paper size **78 x 24 mm**, or **45 x 24 mm** to use less tape. 0.68" (17.3 mm)
keeps the label inside the 18.1 mm the print head covers.

## Options (print dialog → printer features, or `lpoptions -p Brother_PT_P710BT -o ...`)

| Option | Values | Notes |
|---|---|---|
| `CutMode` | `AutoCut`, `NoCut` | |
| `ChainPrint` | `False`, `True` | `True` skips the feed and cut after the last label, which saves tape; that label comes out with the next job. |
| `Darkness` | `Light`, `Normal`, `Dark` | Grey cut-off. Use `Dark` if light logos drop out. |
| `CentreLabel` | `True`, `False` | Centre the content on the 18.1 mm the head prints. Off = print the page's middle 18.1 mm as laid out. |
| `FlipLabel` | `False`, `True` | Rotates output 180°. Use it if labels come out upside down. |

## How it works

- macOS's `cgpdftoraster` renders each page at 180 dpi in 8-bit grey (a 78 mm label is 553 x 170 dots).
- The 128-pin head only covers the middle 18.1 mm of the 24 mm tape. Chrome doesn't keep HTML
  labels inside that strip, so the filter finds the content across the tape and centres it on
  the head.
- Up to 2 mm of blank space at each end is trimmed, because the printer feeds 2 mm itself.
  A 78 mm page comes out as 78 mm of tape.
- Before printing, it asks the printer for its status. A missing tape, open cover or
  non-24 mm tape stops the job with an error in the print queue.

## Testing without a printer

```sh
make test     # renders a test PDF through the real CUPS chain, decodes it to test/preview-p*.png
```

`tools/decode.py` decodes any captured job (`.bin`) back to images and a command log.

## Known limits

- Because of where the cutter sits, any label shorter than 24.5 mm still uses 24.5 mm of tape
  (Brother spec).
- Bluetooth isn't supported. Share the queue from one Mac if others need it.
- Uninstall: `make uninstall`.

## Disclaimer

Not affiliated with or endorsed by Brother Industries. Brother and P-touch are
trademarks of Brother Industries, Ltd.

## Licence

MIT. Copyright (c) 2026 Dave Martinez <dave.martinez25@gmail.com>. See [LICENSE](LICENSE).
