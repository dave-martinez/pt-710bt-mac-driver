CFLAGS ?= -Wall -Wextra -O2
CUPS_LIBS := $(shell cups-config --libs) -lcupsimage

build/rastertopt710bt: src/rastertopt710bt.c
	mkdir -p build
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ $< $(CUPS_LIBS)

# Render a Snipe-IT-shaped test PDF through the real macOS filter chain, then
# decode the printer bytes back to PNGs in test/ for checking.
test: build/rastertopt710bt
	mkdir -p test
	python3 tools/make_test_pdf.py test/snipeit-24mm-A.pdf
	sed 's|/Library/Printers/PT-P710BT/Filter/rastertopt710bt|$(CURDIR)/build/rastertopt710bt|' \
	  ppd/Brother-PT-P710BT-24mm.ppd > test/local.ppd
	cupsfilter -e -p test/local.ppd -m printer/foo -o PageSize=Tape24x65mm \
	  test/snipeit-24mm-A.pdf > test/job.bin 2> test/cupsfilter.log 3</dev/null
	python3 tools/decode.py test/job.bin test/preview

install: build/rastertopt710bt
	sudo ./install.sh

uninstall:
	-sudo lpadmin -x Brother_PT_P710BT
	sudo rm -rf /Library/Printers/PT-P710BT
	sudo rm -f /Library/Printers/PPDs/Contents/Resources/Brother-PT-P710BT-24mm.ppd

clean:
	rm -rf build test

.PHONY: test install uninstall clean
