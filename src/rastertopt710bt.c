/*
 * rastertopt710bt - CUPS raster filter for the Brother PT-P710BT (P-touch Cube Plus).
 *
 * Copyright (c) 2026 Dave Martinez <dave.martinez25@gmail.com>. MIT licence, see LICENSE.
 *
 * Converts CUPS raster pages into Brother's PT raster protocol, as documented in
 * "Software Developer's Manual - Raster Command Reference PT-E550W/P750W/P710BT" v1.02.
 *
 * Only 24 mm TZe tape is supported: the 128-pin head covers the middle 18.1 mm of
 * the 24 mm (170 dot) tape, so the page's ink is centred on those 128 dots.
 *
 * Usage (as called by CUPS): rastertopt710bt job user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <cups/sidechannel.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEAD_PINS        128
#define HEAD_BYTES       (HEAD_PINS / 8)
#define TAPE_WIDTH_MM    24
#define MIN_MARGIN_DOTS  14     /* 2 mm at 180 dpi, the printer minimum */
#define MIN_LENGTH_DOTS  31     /* 4.4 mm minimum print length */
#define MAX_LENGTH_DOTS  7086   /* 1000 mm maximum print length */

typedef struct
{
  int auto_cut;
  int chain;
  int flip;
  int centre;
  int threshold;   /* 0-255 grey level; darker than this prints */
} options_t;

typedef struct
{
  uint8_t *data;
  size_t   len;
  size_t   cap;
} buf_t;

static volatile sig_atomic_t canceled = 0;

static void
cancel_job(int sig)
{
  (void)sig;
  canceled = 1;
}

static void
buf_put(buf_t *b, const void *src, size_t n)
{
  if (b->len + n > b->cap)
  {
    size_t cap = b->cap ? b->cap * 2 : 65536;

    while (cap < b->len + n)
      cap *= 2;

    if ((b->data = realloc(b->data, cap)) == NULL)
    {
      fputs("ERROR: Out of memory\n", stderr);
      exit(1);
    }
    b->cap = cap;
  }

  memcpy(b->data + b->len, src, n);
  b->len += n;
}

static void
buf_byte(buf_t *b, uint8_t c)
{
  buf_put(b, &c, 1);
}

/*
 * TIFF PackBits, limited to the printer's rule: if the result is longer than
 * 17 bytes, send the line as one literal run instead.
 */
static size_t
packbits(const uint8_t *in, size_t n, uint8_t *out)
{
  size_t i = 0, o = 0;

  while (i < n)
  {
    size_t run = 1;

    while (i + run < n && in[i + run] == in[i] && run < 128)
      run ++;

    if (run >= 2)
    {
      out[o++] = (uint8_t)(257 - run);
      out[o++] = in[i];
      i += run;
    }
    else
    {
      size_t start = i, lit = 0;

      while (i < n && lit < 128)
      {
        if (i + 1 < n && in[i] == in[i + 1])
          break;
        i ++;
        lit ++;
      }

      out[o++] = (uint8_t)(lit - 1);
      memcpy(out + o, in + start, lit);
      o += lit;
    }
  }

  if (o > n + 1)
  {
    out[0] = (uint8_t)(n - 1);
    memcpy(out + 1, in, n);
    o = n + 1;
  }

  return o;
}

static int
choice_is(ppd_file_t *ppd, int num_options, cups_option_t *options,
          const char *name, const char *value)
{
  const char *v = cupsGetOption(name, num_options, options);

  if (!v && ppd)
  {
    ppd_choice_t *c = ppdFindMarkedChoice(ppd, name);

    if (c)
      v = c->choice;
  }

  return v && !strcasecmp(v, value);
}

static void
load_options(const char *optstr, options_t *opts)
{
  int            num_options;
  cups_option_t *options = NULL;
  ppd_file_t    *ppd = NULL;
  const char    *ppdfile = getenv("PPD");
  const char    *v;

  num_options = cupsParseOptions(optstr, 0, &options);

  if (ppdfile && (ppd = ppdOpenFile(ppdfile)) != NULL)
  {
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
  }

  opts->auto_cut  = !choice_is(ppd, num_options, options, "CutMode", "NoCut");
  opts->chain     = choice_is(ppd, num_options, options, "ChainPrint", "True");
  opts->flip      = choice_is(ppd, num_options, options, "FlipLabel", "True");
  opts->centre    = !choice_is(ppd, num_options, options, "CentreLabel", "False");
  opts->threshold = 128;

  v = cupsGetOption("Darkness", num_options, options);
  if (!v && ppd)
  {
    ppd_choice_t *c = ppdFindMarkedChoice(ppd, "Darkness");

    if (c)
      v = c->choice;
  }
  if (v)
  {
    if (!strcasecmp(v, "Light"))
      opts->threshold = 80;
    else if (!strcasecmp(v, "Dark"))
      opts->threshold = 180;
  }

  if (ppd)
    ppdClose(ppd);
  cupsFreeOptions(num_options, options);
}

/*
 * Ask the printer for its status over the USB back channel and check the tape.
 * Returns 0 to continue, -1 to abort the job. A printer that doesn't answer
 * (or a non-USB queue) is not treated as an error.
 */
static int
check_status(void)
{
  static const uint8_t req[] = { 0x1b, 0x69, 0x53 };
  uint8_t              st[32];
  ssize_t              got = 0, n;

  fwrite(req, 1, sizeof(req), stdout);
  fflush(stdout);

  while (got < (ssize_t)sizeof(st) &&
         (n = cupsBackChannelRead((char *)st + got, sizeof(st) - (size_t)got, 3.0)) > 0)
    got += n;

  if (got < (ssize_t)sizeof(st) || st[0] != 0x80 || st[1] != 0x20)
  {
    fputs("DEBUG: No status reply from printer, continuing\n", stderr);
    return 0;
  }

  fprintf(stderr, "DEBUG: Status err1=%02x err2=%02x width=%dmm type=%02x\n",
          st[8], st[9], st[10], st[11]);

  if (st[8] & 0x01)
  {
    fputs("STATE: +media-empty-error\n", stderr);
    fputs("ERROR: No tape cassette in the printer\n", stderr);
    return -1;
  }
  if (st[9] & 0x10)
  {
    fputs("STATE: +cover-open-error\n", stderr);
    fputs("ERROR: Printer cover is open\n", stderr);
    return -1;
  }
  if (st[8] & 0x04)
  {
    fputs("ERROR: Cutter jam\n", stderr);
    return -1;
  }
  if (st[9] & 0x20)
  {
    fputs("ERROR: Printer is overheating, wait and retry\n", stderr);
    return -1;
  }
  if (st[10] && st[10] != TAPE_WIDTH_MM)
  {
    fputs("STATE: +media-needed-error\n", stderr);
    fprintf(stderr, "ERROR: Loaded tape is %d mm; this driver needs 24 mm tape\n", st[10]);
    return -1;
  }
  if (st[8] & 0x08)
    fputs("WARNING: Printer batteries are weak\n", stderr);

  fputs("STATE: -media-empty-error,cover-open-error,media-needed-error\n", stderr);
  return 0;
}

static int
is_black(const cups_page_header2_t *h, const uint8_t *line, unsigned x, int threshold)
{
  int additive = h->cupsColorSpace == CUPS_CSPACE_W ||
                 h->cupsColorSpace == CUPS_CSPACE_SW ||
                 h->cupsColorSpace == CUPS_CSPACE_RGB ||
                 h->cupsColorSpace == CUPS_CSPACE_SRGB;

  if (h->cupsBitsPerPixel == 1)
  {
    int bit = (line[x / 8] >> (7 - (x % 8))) & 1;

    return additive ? !bit : bit;
  }
  else
  {
    /* 8-bit grey; for RGB, look at the green channel. */
    unsigned bpp = h->cupsBitsPerPixel / 8;
    int      grey = line[x * bpp + (bpp >= 3 ? 1 : 0)];

    if (!additive)
      grey = 255 - grey;

    return grey < threshold;
  }
}

/*
 * Render one page into `out`. Returns 0 on success.
 *
 * The page is laid out like a Snipe-IT TZe_24mm label: landscape, label length
 * along x, tape width (170 dots) along y. A portrait page is treated as that
 * label rotated 90 degrees clockwise.
 */
static int
render_page(cups_raster_t *ras, const cups_page_header2_t *h,
            const options_t *opts, int page, buf_t *out)
{
  unsigned  w = h->cupsWidth, ht = h->cupsHeight, bpl = h->cupsBytesPerLine;
  unsigned  across, length, y, x, f, i;
  unsigned  first, last, start, stop, lines;
  int       top, amin, amax;
  uint8_t  *raster, *bits, *line;
  int       landscape = w >= ht;

  if (h->HWResolution[0] != 180 || h->HWResolution[1] != 180)
  {
    fprintf(stderr, "ERROR: Unsupported resolution %ux%u\n",
            h->HWResolution[0], h->HWResolution[1]);
    return -1;
  }

  across = landscape ? ht : w;
  length = landscape ? w : ht;

  if ((raster = malloc((size_t)bpl * ht)) == NULL ||
      (bits = calloc((size_t)length, HEAD_BYTES)) == NULL)
  {
    fputs("ERROR: Out of memory\n", stderr);
    return -1;
  }

  for (y = 0; y < ht; y ++)
    if (cupsRasterReadPixels(ras, raster + (size_t)y * bpl, bpl) != bpl)
    {
      fputs("ERROR: Short raster read\n", stderr);
      free(raster);
      free(bits);
      return -1;
    }

#define SRC_XY(a, f) \
  do { if (landscape) { y = (a); x = (f); } else { y = (f); x = w - 1 - (a); } } while (0)

  /*
   * Apps don't reliably place labels inside the printable strip (Chrome ignores
   * the PPD's margins for HTML pages), so find the ink across the tape and
   * centre it on the 128-pin head. Content taller than the head is centred and
   * clipped equally top and bottom.
   */
  amin = (int)across;
  amax = -1;
  for (f = 0; f < length; f ++)
    for (i = 0; i < across; i ++)
    {
      SRC_XY(i, f);
      if (is_black(h, raster + (size_t)y * bpl, x, opts->threshold))
      {
        if ((int)i < amin)
          amin = (int)i;
        if ((int)i > amax)
          amax = (int)i;
      }
    }

  if (!opts->centre || amax < 0)
    top = ((int)across - HEAD_PINS) / 2;
  else
  {
    top = (amin + amax + 1) / 2 - HEAD_PINS / 2;
    if (amax - amin + 1 > HEAD_PINS)
      fprintf(stderr, "WARNING: Label content is %.1f mm across the tape; only 18.1 mm prints\n",
              (amax - amin + 1) * 25.4 / 180.0);
  }
  fprintf(stderr, "DEBUG: Ink across tape %d-%d of %u dots, head starts at %d\n",
          amin, amax, across, top);

  /*
   * bits[f] holds one head line for feed position f. Pin index i counts from
   * the top edge of the label (as it reads), stored MSB-first from byte 0.
   */
  for (f = 0; f < length; f ++)
  {
    uint8_t *dst = bits + (size_t)f * HEAD_BYTES;

    for (i = 0; i < HEAD_PINS; i ++)
    {
      int a = top + (int)i;

      if (a < 0 || a >= (int)across)
        continue;

      SRC_XY((unsigned)a, f);
      if (is_black(h, raster + (size_t)y * bpl, x, opts->threshold))
        dst[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    }
  }
#undef SRC_XY
  free(raster);

  if (opts->flip)
  {
    for (f = 0; f < length / 2; f ++)
    {
      uint8_t tmp[HEAD_BYTES];

      memcpy(tmp, bits + (size_t)f * HEAD_BYTES, HEAD_BYTES);
      memcpy(bits + (size_t)f * HEAD_BYTES, bits + (size_t)(length - 1 - f) * HEAD_BYTES, HEAD_BYTES);
      memcpy(bits + (size_t)(length - 1 - f) * HEAD_BYTES, tmp, HEAD_BYTES);
    }
    for (f = 0; f < length; f ++)
    {
      uint8_t *l = bits + (size_t)f * HEAD_BYTES, rev[HEAD_BYTES];

      for (i = 0; i < HEAD_BYTES; i ++)
      {
        uint8_t b = l[HEAD_BYTES - 1 - i], r = 0;
        int     k;

        for (k = 0; k < 8; k ++)
          if (b & (1 << k))
            r |= (uint8_t)(0x80 >> k);
        rev[i] = r;
      }
      memcpy(l, rev, HEAD_BYTES);
    }
  }

  /*
   * The printer adds MIN_MARGIN_DOTS of feed before and after the print data.
   * Absorb that into the page's own blank ends so the tape comes out at the
   * page length.
   */
  for (first = 0; first < length; first ++)
  {
    static const uint8_t zero[HEAD_BYTES];

    if (memcmp(bits + (size_t)first * HEAD_BYTES, zero, HEAD_BYTES))
      break;
  }
  for (last = length; last > first; last --)
  {
    static const uint8_t zero[HEAD_BYTES];

    if (memcmp(bits + (size_t)(last - 1) * HEAD_BYTES, zero, HEAD_BYTES))
      break;
  }

  start = first < MIN_MARGIN_DOTS ? first : MIN_MARGIN_DOTS;
  stop  = length - ((length - last) < MIN_MARGIN_DOTS ? (length - last) : MIN_MARGIN_DOTS);
  if (stop <= start)
  {
    start = 0;
    stop  = length;
  }

  lines = stop - start;
  if (lines > MAX_LENGTH_DOTS)
  {
    fprintf(stderr, "WARNING: Label longer than 1000 mm, truncating\n");
    stop  = start + MAX_LENGTH_DOTS;
    lines = MAX_LENGTH_DOTS;
  }
  if (lines < MIN_LENGTH_DOTS)
    lines = MIN_LENGTH_DOTS;

  /* Control codes for this page. */
  {
    uint8_t cmd[] =
    {
      0x1b, 0x69, 0x61, 0x01,                                    /* raster mode */
      0x1b, 0x69, 0x7a, 0x84, 0x00, TAPE_WIDTH_MM, 0x00,        /* print info: width valid, recover */
      (uint8_t)lines, (uint8_t)(lines >> 8), (uint8_t)(lines >> 16), (uint8_t)(lines >> 24),
      (uint8_t)(page == 0 ? 0 : 1), 0x00,
      0x1b, 0x69, 0x4d, (uint8_t)(opts->auto_cut ? 0x40 : 0x00), /* various mode */
      0x1b, 0x69, 0x4b, (uint8_t)(opts->chain ? 0x00 : 0x08),    /* advanced mode */
      0x1b, 0x69, 0x64, MIN_MARGIN_DOTS, 0x00,                   /* margin */
      0x4d, 0x02                                                 /* TIFF compression */
    };

    buf_put(out, cmd, sizeof(cmd));
  }

  for (f = start; f < stop; f ++)
  {
    static const uint8_t zero[HEAD_BYTES];
    uint8_t              packed[HEAD_BYTES + 2];
    size_t               n;

    line = bits + (size_t)f * HEAD_BYTES;

    if (!memcmp(line, zero, HEAD_BYTES))
    {
      buf_byte(out, 'Z');
      continue;
    }

    n = packbits(line, HEAD_BYTES, packed);
    buf_byte(out, 'G');
    buf_byte(out, (uint8_t)n);
    buf_byte(out, 0x00);
    buf_put(out, packed, n);
  }
  for (f = stop - start; f < lines; f ++)
    buf_byte(out, 'Z');

  free(bits);
  return 0;
}

int
main(int argc, char *argv[])
{
  int                 fd = 0, page = 0, status = 0;
  cups_raster_t      *ras;
  cups_page_header2_t header;
  options_t           opts;
  buf_t               pending = { 0 };
  uint8_t             invalidate[100] = { 0 };
  static const uint8_t init[] = { 0x1b, 0x40 };

  setbuf(stderr, NULL);

  if (argc < 6 || argc > 7)
  {
    fputs("Usage: rastertopt710bt job user title copies options [file]\n", stderr);
    return 1;
  }

  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0)
  {
    perror("ERROR: Unable to open raster file");
    return 1;
  }

  signal(SIGTERM, cancel_job);
  signal(SIGPIPE, SIG_IGN);

  load_options(argv[5], &opts);
  fprintf(stderr, "DEBUG: auto_cut=%d chain=%d flip=%d centre=%d threshold=%d\n",
          opts.auto_cut, opts.chain, opts.flip, opts.centre, opts.threshold);

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

  fwrite(invalidate, 1, sizeof(invalidate), stdout);
  fwrite(init, 1, sizeof(init), stdout);

  /* fd 3 is the CUPS back channel; when testing by hand the raster file may land there. */
  if (fd != 3 && check_status() < 0)
  {
    cupsRasterClose(ras);
    return 1;
  }

  /*
   * A page ends with FF, except the last which ends with Control-Z (print and
   * feed). We only know a page was the last once the next header read fails,
   * so each page is held back until then.
   */
  while (!canceled && cupsRasterReadHeader2(ras, &header))
  {
    if (pending.len)
    {
      buf_byte(&pending, 0x0c);
      fwrite(pending.data, 1, pending.len, stdout);
      fflush(stdout);
      pending.len = 0;
    }

    fprintf(stderr, "PAGE: %d %d\n", page + 1, header.NumCopies ? (int)header.NumCopies : 1);
    fprintf(stderr, "DEBUG: Page %d: %ux%u, %u bpp, colorspace %d\n", page + 1,
            header.cupsWidth, header.cupsHeight, header.cupsBitsPerPixel, header.cupsColorSpace);

    if (render_page(ras, &header, &opts, page, &pending))
    {
      status = 1;
      break;
    }
    page ++;
  }

  if (canceled)
  {
    fwrite(invalidate, 1, sizeof(invalidate), stdout);
    fwrite(init, 1, sizeof(init), stdout);
  }
  else if (pending.len)
  {
    buf_byte(&pending, 0x1a);
    fwrite(pending.data, 1, pending.len, stdout);
  }
  fflush(stdout);

  free(pending.data);
  cupsRasterClose(ras);
  if (fd)
    close(fd);

  if (page == 0 && !status)
    fputs("ERROR: No pages found\n", stderr);

  return status || page == 0;
}
