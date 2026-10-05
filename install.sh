#!/bin/sh
# Install the PT-P710BT 24 mm CUPS driver and create a print queue.
# Run with sudo, with the printer plugged in over USB and switched on.
set -eu

QUEUE=${QUEUE:-Brother_PT_P710BT}
FILTER_DIR=/Library/Printers/PT-P710BT/Filter
PPD_DIR=/Library/Printers/PPDs/Contents/Resources
PPD_NAME=Brother-PT-P710BT-24mm.ppd
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo: sudo $0" >&2
  exit 1
fi

if [ ! -x "$HERE/build/rastertopt710bt" ]; then
  echo "Build first: make" >&2
  exit 1
fi

# CUPS only runs filters owned by root and not writable by others, all the way up the path.
install -d -o root -g wheel -m 755 /Library/Printers/PT-P710BT "$FILTER_DIR"
install -o root -g wheel -m 755 "$HERE/build/rastertopt710bt" "$FILTER_DIR/rastertopt710bt"
install -o root -g wheel -m 644 "$HERE/ppd/$PPD_NAME" "$PPD_DIR/$PPD_NAME"
echo "Installed filter and PPD."

URI=${URI:-$(lpinfo -v 2>/dev/null | awk '/usb:\/\/Brother\/PT-P710BT/ {print $2; exit}')}
if [ -z "$URI" ]; then
  echo "Printer not found on USB. Plug it in, switch it on, then rerun (or set URI=usb://...)." >&2
  lpinfo -v | grep -i usb >&2 || true
  exit 1
fi

lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD_DIR/$PPD_NAME" \
  -D "Brother PT-P710BT (24mm)" -L "USB" \
  -o PageSize=Tape24x78mm -o printer-is-shared=false
cupsenable "$QUEUE"
cupsaccept "$QUEUE"
echo "Created queue $QUEUE on $URI"
