// Constructs composite card image from images or postscript for each layer
// - Colour layer
// - Black layer
// - UV layer
// - Inhibit layer (used to show holes in card, such as SIM cutout)
// Accepts artwork for each layer as matching dpi image (png, etc, anything gm understands), or ps, or svg
// Produces output in various formats
// - Image for display with card border / transparency
// - UV layer overlay image for display (provide only UV layer arguments)
// - Raw CMYKU/CMYKU for matica printer
// Cards are 2 1/8" by 3 3/8" so at 300dpi are 1012.5x637.x
// - Source bitmaps are centred on to output
// - Display images are centred to an output of 1024 x 640, and can be 512 x 320 (medium) or 256 x 160 (small)
// - Matica are 1036 x 664 so source images can make full use of that size

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <err.h>
#include <axl.h>
#include <errno.h>

unsigned int dpi = 300;
#define	cardw	(3370*dpi/1000) // Actual card width IEC-7810 ID-1 3.370"
#define	cardh	(2125*dpi/1000) // Actual card height IEC-7810 ID-1 2.125"
#define	imagew	(1024*dpi/300)  // Large image width
#define	imageh	(640*dpi/300)   // Large image height
#define	maticaw	(1036*dpi/300)  // Matica card width
#define	maticah	(664*dpi/300)   // Matica card heigh
#define	zebraw	(1024*dpi/300)  // Zebra card width
#define	zebrah	(648*dpi/300)   // Zebra card heigh
#define       maxw    (cardw+dpi)       // Max size we will do
#define       maxh    (cardh+dpi)       // Max size we will do

int debug = 0;
int fast = 0;
int unsafe = 0;
int keepfiles = 0;
xml_t svg = NULL;

int mkstempsuffix(char *fn)
{
   char *e = strrchr(fn, '.');
   if (!e)
   {
      int f = mkstemp(fn);
      if (f >= 0 && debug && keepfiles)
         fprintf(stderr, "Temp file %s\n", fn);
      return f;
   }
#if 0
   int f = mkstemps(fn, strlen(e));
   if (f >= 0 && debug && keepfiles)
      fprintf(stderr, "Temp file %s\n", fn);
   return f;
#else
   // Bodge - not bullet proof
   *e = 0;
   int f = mkstemp(fn);
   if (f < 0)
      return f;
   if (keepfiles)
      chmod(fn, 0666);
   char *was = strdupa(fn);
   *e = '.';
   if (!rename(was, fn))
      return f;
   close(f);
   unlink(was);
   if (f >= 0 && debug && keepfiles)
      fprintf(stderr, "Temp file %s\n", fn);
   return -1;
#endif
}

unsigned char *loadfile(const char *tag, const char *filename, int width, int height, int bpp, int flip)
{                               // Load a file - makes in to a BMP with alpha 0xFF for visible
   if (!filename)
      return NULL;
   if (debug)
      fprintf(stderr, "Load %s (bpp=%d flip=%d)\n", filename, bpp, flip);
   int temp;
   ssize_t l;
   int f,
    t;
   const char *zaplater = NULL;
   unsigned char *m = 0;
   struct stat s;
   char *id = strrchr(filename, '#');
   if (id)
      *id++ = 0;
   int w = 0,
       h = 0;                   // RGBA layour
   f = open(filename, O_RDONLY);
   if (f < 0)
   {
      warnx("Cannot open: %s", filename);
      return NULL;
   }
   {
      if (fstat(f, &s))
         errx(1, "Cannot stat: %s", filename);
      if (!s.st_size)
      {
         if (debug)
            warnx("%s empty file", filename);
         close(f);
         return NULL;
      }
      char magic[2];
      if (read(f, magic, 2) != 2)
         errx(1, "%s cannot read magic number", filename);
      lseek(f, 0, SEEK_SET);
      if (magic[0] == '<' && magic[1] == '?')
      {                         // assuming SVG - convert to PNG
         if (debug)
            fprintf(stderr, "Convert from SVG\n");
         char tmp[] = "/tmp/cardXXXXXX.png";
         if ((t = mkstempsuffix(tmp)) < 0)
            errx(1, "Bad tmp %s", tmp);
         close(t);
         close(f);
         char *args[20];
         int a = 0;
         args[a++] = "inkscape";
         args[a++] = "--without-gui";
         args[a++] = "--export-area-page";
         if (asprintf(&args[a++], "--export-dpi=%d", dpi) < 0)
            errx(1, "malloc");
         if (id)
         {
            args[a++] = "--export-id-only";
            args[a++] = "--export-id";
            args[a++] = id;
         }
         args[a++] = "--export-png";
         args[a++] = tmp;
         args[a++] = (char *) filename;
         args[a++] = NULL;
         if (debug)
         {
            for (a = 0; args[a]; a++)
               fprintf(stderr, "%s ", args[a]);
            fprintf(stderr, "\n");
         }
         pid_t p = fork();
         if (p < 0)
            err(1, "inkscape");
         if (p)
         {                      // Parent
            if (waitpid(p, NULL, 0) < 0)
               err(1, "inkscape");
         } else
         {                      // Child
            setenv("HOME", "/tmp", 1);
            close(1);
            execvp("inkscape", args);
            err(1, "inkspace");
         }
         f = open(tmp, O_RDONLY);
         if (f < 0)
            errx(1, "Cannot open: %s [%s]", filename, tmp);
         if (fstat(f, &s))
            errx(1, "Cannot stat: %s", filename);
         if (!s.st_size)
         {
            warnx("%s produced no output", filename);
            close(f);
            return NULL;
         }
         //if (!keepfiles) unlink (tmp);
         filename = strdup(tmp);        //  Used to convert to bmp
         zaplater = filename;
      }
      if (magic[0] == '%' && magic[1] == '!')
      {                         // Postscript
         if (debug)
            fprintf(stderr, "Convert from PS\n");
         char cmd[10000];
         char tmp[] = "/tmp/cardXXXXXX.png";
         if ((t = mkstempsuffix(tmp)) < 0)
            errx(1, "Bad tmp %s", tmp);
         close(t);
         sprintf(cmd, "gs %s%s-dNOPAUSE -sDEVICE=%s -sOutputFile=%s -r%dx%d -g%dx%d%s - > /dev/null", debug ? "" : "-q -dBATCH ", unsafe ? "" : "-dSAFER ", (bpp == 1) ? "pngmono" : (bpp == 8) ? "pnggray" : "png16m", tmp, dpi, dpi, width, height, fast ? "" : " -dTextAlphaBits=4 -dGraphicsAlphaBits=4");

         if (debug)
            fprintf(stderr, "%s\n", cmd);
         FILE *ps = popen(cmd, "w");
         if (!ps)
         {
            warnx("Could not run %s on %s", cmd, filename);
            close(f);
            return NULL;
         }
         fprintf(ps, "%%!\n%d 72 mul %d div %d 72 mul %d div translate\n", width - cardw, dpi * 2, height - cardh, dpi * 2);    // 0,0 is edge of card
         //if (flip) fprintf (ps, "%d 72 mul %d div %d 72 mul %d div translate 180 rotate\n", cardw, dpi, cardh, dpi);
         {                      // feed in the postscript
            unsigned char temp[1024];
            int l;
            while ((l = read(f, temp, sizeof(temp))) > 0 && fwrite(temp, 1, l, ps) == l);
         }
         if (pclose(ps) < 0)
         {
            warnx("%s failed to convert", filename);
            close(f);
            return NULL;
         }
         close(f);
         f = open(tmp, O_RDONLY);
         if (f < 0)
            errx(1, "Cannot open: %s [%s]", filename, tmp);
         if (fstat(f, &s))
            errx(1, "Cannot stat: %s", filename);
         if (!s.st_size)
         {
            warnx("%s produced no output", filename);
            close(f);
            return NULL;
         }
         filename = strdup(tmp);        //  Used to convert to bmp
         zaplater = filename;
      }
      if (svg)
      {                         // Make an SVG from bitmaps
         xml_t g = xml_element_add(svg, "g");
         xml_add(g, "@id", tag);
         xml_t i = xml_element_add(g, "image");
         xml_addf(i, "@width", "%d", width);
         xml_addf(i, "@height", "%d", height);
         xml_add(i, "@x", "0");
         xml_add(i, "@y", "0");
         int f = open(filename, O_RDONLY);
         if (f < 0)
            err(1, "Cannot open png");
         struct stat st;
         if (fstat(f, &st) < 0)
            err(1, "Cannot stat png");
         size_t length = st.st_size;
         void *addr = mmap(NULL, length, PROT_READ, MAP_PRIVATE, f, 0);
         if (addr == MAP_FAILED)
            err(1, "Cannot map png");
         size_t len = (length + 5) / 6 * 8 + 3;
         char *b64 = malloc(len);
         if (!b64)
            errx(1, "malloc");
         xml_addf(i, "@xlink:href", "data:image/png;base64,%s", xml_baseN(length, addr, len, b64, BASE64, 6));
         free(b64);
         munmap(addr, length);
         close(f);
      } else
      {                         // image - convert to rgba in BMP style
         if (debug)
            fprintf(stderr, "Convert to RGBA %s\n", filename);
         char *e = strrchr(filename, '.');
         if (1)                 // Convert anyway
         {                      // Convert to RGBA
            char cmd[10000];
            char tmp[] = "/tmp/cardXXXXXX.bmp";
            if (e && strlen(e) == 4 && isalnum(e[1]) && isalnum(e[2]) && isalnum(e[3]))
               strcpy(tmp + sizeof(tmp) - 5, e);        // Extn
            if ((t = mkstempsuffix(tmp)) < 0)
               errx(1, "Bad tmp %s", tmp);
            close(t);
            unlink(tmp);
            if (link(filename, tmp))
            {                   // Link or copy source
               if (errno != EXDEV)
                  err(1, "Cannot link %s %s", filename, tmp);
               // copy
               int i = open(filename, O_RDONLY);
               if (i < 0)
                  err(1, "Cannot copy %s", filename);
               int o = open(tmp, O_WRONLY | O_CREAT, 05666);
               if (o < 0)
                  err(1, "Cannot copy to %s", tmp);
               unsigned char tmp[1024];
               size_t l;
               while ((l = read(i, tmp, sizeof(tmp))) > 0)
                  if (write(o, tmp, l) != l)
                     err(1, "Bad copy to %s", tmp);
               close(i);
               close(o);
            }
            // We cannot trust gm to make uncompressed BMP so we fudge it
            sprintf(cmd, "gm identify -format \"%%W,%%H\" %s", tmp);
            if (debug)
               fprintf(stderr, "%s\n", cmd);
            FILE *gm = popen(cmd, "r");
            if (!gm)
               err(1, "Cannot run %s", cmd);
            if (fscanf(gm, "%d,%d", &w, &h) != 2)
               errx(1, "Cannot identify %s", filename);
            fclose(gm);
            if (!w || !h)
               errx(1, "Cannot identify %s", filename);
            char tmp2[] = "/tmp/cardXXXXXX.rgba";
            if ((t = mkstempsuffix(tmp2)) < 0)
               errx(1, "Bad tmp %s", tmp2);
            close(t);
            sprintf(cmd, "gm convert %s%s +compress -format RGBA %s", tmp, flip ? " -flop" : " -flip", tmp2);
            if (debug)
               fprintf(stderr, "%s\n", cmd);
            if (system(cmd))
               errx(1, "Convert failed");
            close(f);
            f = open(tmp2, O_RDONLY);
            if (f < 0)
               err(1, "Failed %s", tmp2);
            if (!keepfiles)
            {
               unlink(tmp);
               unlink(tmp2);
            }
         } else
            lseek(f, 0, SEEK_SET);      // is a BMP already
         if (fstat(f, &s))
            errx(1, "Cannot stat: %s", filename);
      }
   }
   if (!svg)
   {
      if (w)
      {                         // rgba
         if (s.st_size != w * h * 4)
            errx(1, "%s bad file size", filename);
         m = malloc(s.st_size + 54);    // Include BMP header
         if (!m)
            errx(1, "malloc");
         memset(m, 0, 54);
         if (read(f, m + 54, s.st_size) != s.st_size)
            errx(1, "Bad read");
         close(f);
         // Some data in pseudo BMP header
         m[10] = 54;            // offset to actual data
         m[28] = 32;            // RGBA
         m[18] = w;
         m[19] = (w >> 8);
         m[22] = h;
         m[23] = (h >> 8);
      } else
      {
         if (s.st_size < 54)
            errx(1, "%s file too small", filename);
         if (s.st_size > 5000000)
            errx(1, "%s file too large", filename);
         m = malloc(s.st_size);
         if (!m)
            errx(1, "No memory for %llu bytes", (unsigned long long) s.st_size);
         l = read(f, m, s.st_size);
         if (l < 0)
            errx(1, "Read failed: %s", filename);
         if (l < s.st_size)
            errx(1, "%s did not read whole file", filename);
         close(f);
         if (m[0] != 0x42 || m[1] != 0x4D || m[6] || m[7] || m[8] || m[9] || m[15] || m[26] != 1 || m[27])
            errx(1, "Image file convert failed");
         temp = (m[10] + (m[11] << 8) + (m[12] << 16) + (m[13] << 24));
         if (temp > s.st_size)
            errx(1, "%s Bad offset", filename);
         if (m[30] || m[31] || m[32] || m[33])
            errx(1, "%s Use uncompressed BMPs %02X%02X%02X%02X", filename, m[30], m[31], m[32], m[33]);
      }
   }
   if (debug)
      fprintf(stderr, "%s size %lld\n", filename, (unsigned long long) s.st_size);
   if (zaplater && !keepfiles)
      unlink(zaplater);
   return m;
}

int main(int argc, const char *argv[])
{
   warnx("Deprecated code in use");
   int c;
   const char *c1file = NULL;
   const char *k1file = NULL;
   const char *u1file = NULL;
   const char *i1file = NULL;
   const char *c2file = NULL;
   const char *k2file = NULL;
   const char *u2file = NULL;
   const char *i2file = NULL;
   const char *outfile = NULL;
   const char *format = NULL;
   int trans = 0;               // Make output partly transparent to overlay image of blank card
   int longedge = 0;            // Flip on long edge
   int black = 1;               // 1 BPP black (maybe have options for this some time)

   poptContext optCon;          // context for parsing command-line options
   const struct poptOption optionsTable[] = {
      { "c1", 'c', POPT_ARG_STRING, &c1file, 0, "Colour first side", "filename" },
      { "k1", 'k', POPT_ARG_STRING, &k1file, 0, "Black first side", "filename" },
      { "u1", 'u', POPT_ARG_STRING, &u1file, 0, "UV first side", "filename" },
      { "i1", 'i', POPT_ARG_STRING, &i1file, 0, "Inhibit first side", "filename" },
      { "c2", 'C', POPT_ARG_STRING, &c2file, 0, "Colour second side", "filename" },
      { "k2", 'K', POPT_ARG_STRING, &k2file, 0, "Black second side", "filename" },
      { "u2", 'U', POPT_ARG_STRING, &u2file, 0, "UV second side", "filename" },
      { "i2", 'I', POPT_ARG_STRING, &i2file, 0, "Inhibit second side", "filename" },
      { "output", 'o', POPT_ARG_STRING, &outfile, 0, "Output file", "filename" },
      { "format", 'f', POPT_ARG_STRING, &format, 0, "Format", "small/medium/large/Matica/Zebra/WxH/svg" },
      { "trans", 0, POPT_ARG_NONE, &trans, 0, "Make partly transparent for image overlay" },
      { "long-edge", 0, POPT_ARG_NONE, &longedge, 0, "Second side is flipped on long edge not short" },
      { "unsafe", 0, POPT_ARG_NONE, &unsafe, 0, "Allow unsafe ghostscript" },
      { "fast", 0, POPT_ARG_NONE, &fast, 0, "Faster preview" },
      { "dpi", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &dpi, 0, "DPI", "N" },
      { "keep-files", 0, POPT_ARGFLAG_DOC_HIDDEN | POPT_ARG_NONE, &keepfiles, 0, "Keep temp files" },
      { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
      POPT_AUTOHELP { }
   };

   optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
   //poptSetOtherOptionHelp (optCon, "");

   if ((c = poptGetNextOpt(optCon)) < -1)
      errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));

   if (poptPeekArg(optCon) || !format || !*format)
   {
      poptPrintUsage(optCon, stderr, 0);
      return -1;
   }


   unsigned char *c1 = NULL,
       *k1 = NULL,
       *u1 = NULL,
       *i1 = NULL;
   unsigned char *c2 = NULL,
       *k2 = NULL,
       *u2 = NULL,
       *i2 = NULL;

   if (isdigit(*format) || !strcasecmp(format, "small") || !strcasecmp(format, "medium") || !strcasecmp(format, "large"))
   {                            // Generate display image
      // Note, using RGBA conversion, alpha is 0xFF for visible, 0x00 for invisible
      int height = imageh,
          width = imagew;
      int scale = 1;
      if (*format == 'm')
         scale = 2;
      if (*format == 's')
         scale = 4;
      if (isdigit(*format))
      {                         // format is W or WxH
         const char *f = format;
         int w = 0,
             h = 0;
         while (isdigit(*f))
            w = w * 10 + *f++ - '0';
         if (*f == 'x')
         {
            f++;
            while (isdigit(*f))
               h = h * 10 + *f++ - '0';
         }
         if (!w)
            errx(1, "WTF?");
         if (w > maxw)
            errx(1, "Too wide");
         if (h > maxh)
            errx(1, "Too high");
         if (w < cardw)
            scale = (cardw + w - 1) / w;
         width = w * scale;
         if (h)
            height = h * scale;
         else
            height = cardh * width / cardw;
      }
      c1 = loadfile("C1", c1file, width, height, 24, 0);
      k1 = loadfile("K1", k1file, width, height, black, 0);
      u1 = loadfile("U1", u1file, width, height, 8, 0);
      i1 = loadfile("I1", i1file, width, height, 1, 0);
      c2 = loadfile("C2", c2file, width, height, 24, 0);
      k2 = loadfile("K2", k2file, width, height, black, 0);
      u2 = loadfile("U2", u2file, width, height, 8, 0);
      i2 = loadfile("I2", i2file, width, height, 1, 0);
      int h = height;
      if (c2 || k2 || i2 || u2)
         h *= 2;                // Two sides on one image
      // Generate a BMP of the right output size with alpha layer
      // Make the card outline as the base image
      int t;
      char cmd[10000];
      char tmp[] = "/tmp/cardXXXXXX.png";
      if ((t = mkstempsuffix(tmp)) < 0)
         errx(1, "Bad tmp %s", tmp);
      close(t);
      sprintf(cmd, "gs -q %s-dBATCH -dNOPAUSE -sDEVICE=pngalpha -sOutputFile=%s -r%dx%d -g%dx%d%s - > /dev/null", unsafe ? "" : "-dSAFER ", tmp, dpi, dpi, width, h, fast ? "" : " -dTextAlphaBits=4 -dGraphicsAlphaBits=4");
      if (debug)
         fprintf(stderr, "%s\n", cmd);
      FILE *ps = popen(cmd, "w");
      if (!ps)
         errx(1, "Cannot make image");
      fprintf(ps, "%%!\n%d 72 mul %d div %d 72 mul %d div translate\n", width - cardw, dpi * 2, height - cardh, dpi * 2);
      fprintf(ps, "1 1 1 setrgbcolor\n");
      if (c2 || k2 || i2 || u2)
      {                         // Second side
         fprintf(ps, "newpath 9 9 9 180 270 arc 234 9 9 270 0 arc 234 144 9 0 90 arc 9 144 9 90 180 arc fill\n");
         fprintf(ps, "0 %d 72 mul %d div translate\n", dpi, height);
      }
      fprintf(ps, "newpath 9 9 9 180 270 arc 234 9 9 270 0 arc 234 144 9 0 90 arc 9 144 9 90 180 arc fill\n");
      fprintf(ps, "showpage\n");
      pclose(ps);
      struct stat s;
      unsigned char *prev = NULL;
      int f;
      {                         // Make template in memory as BMP
         char tmp2[] = "/tmp/cardXXXXXX.rgba";
         if ((t = mkstempsuffix(tmp2)) < 0)
            errx(1, "Bad tmp %s", tmp2);
         close(t);
         snprintf(cmd, sizeof(cmd), "gm convert %s -format RGBA %s", tmp, tmp2);        // flip does not matter, symmetric
         if (debug)
            fprintf(stderr, "%s\n", cmd);
         if (system(cmd))
            errx(1, "Convert failed");
         f = open(tmp2, O_RDONLY);
         if (!keepfiles)
         {
            unlink(tmp);
            unlink(tmp2);
         }
         if (f < 0)
            errx(1, "Cannot write %s", tmp2);
         if (fstat(f, &s))
            errx(1, "%s", tmp2);
         if (s.st_size != width * h * 4)
            errx(1, "Bad template %s", tmp2);
         prev = malloc(s.st_size + 54);
         if (!prev)
            errx(1, "No memory for %llu bytes %s", (unsigned long long) s.st_size + 54, tmp2);
         memset(prev, 0, 54);
         unsigned long v;
         prev[0] = 'B';         // BMP header for final saved BMP file
         prev[1] = 'M';
         prev[2] = (v = (s.st_size + 54));
         prev[3] = (v >> 8);
         prev[4] = (v >> 16);
         prev[5] = (v >> 24);
         prev[10] = 54;         // offset to image
         prev[14] = 40;         // header size
         prev[18] = width;
         prev[19] = (width >> 8);
         prev[22] = h;
         prev[23] = (h >> 8);
         prev[26] = 1;
         prev[28] = 32;         // bpp
         prev[34] = (v = s.st_size);
         prev[35] = (v >> 8);
         prev[36] = (v >> 16);
         prev[37] = (v >> 24);
         size_t l = read(f, prev + 54, s.st_size);
         if (l != s.st_size)
            errx(1, "Bad read %s", tmp2);
         close(f);
         if (!keepfiles)
            unlink(tmp2);
      }
      char tmp2[] = "/tmp/cardXXXXXX.bmp";
      if ((t = mkstempsuffix(tmp2)) < 0)
         errx(1, "Bad tmp %s", tmp2);
      close(t);
      unsigned char *p = prev + 54,
          *q;
      void load(unsigned char *c, unsigned char *k, unsigned char *i, unsigned char *u) {
         unsigned char *e = p + width * height * 4;
         typedef void loader(unsigned char *, unsigned char, unsigned char, unsigned char, unsigned char);
         void loadc(unsigned char *out, unsigned char R, unsigned char G, unsigned char B, unsigned char A) {   // Colour
            out[0] = R;
            out[1] = G;
            out[2] = B;
            if (trans)
            {
               int t = 255 - (out[0] + out[1] + out[2]) * (out[0] + out[1] + out[2]) / 2295;
               if (t < out[3])
                  out[3] = t;
            }
         }
         void loadk(unsigned char *out, unsigned char R, unsigned char G, unsigned char B, unsigned char A) {   // Black
            if (R + G + B < 3 * 128 && A > 128)
            {                   // Apply black
               out[0] = 0;
               out[1] = 0;
               out[2] = 0;
               out[3] = 0xFF;
            }
         }
         void loadu(unsigned char *out, unsigned char R, unsigned char G, unsigned char B, unsigned char A) {   // UV
            if (!c && !k)
            {
               out[0] = (255 - R) * 0xFF / 255;
               out[1] = (255 - G) * 0x99 / 255;
               out[2] = (255 - B) * 0x99 / 255;
               if (out[3] > 0x80)
                  out[3] = 0x80;
            } else if (R + G + B < 3 * 128)
            {
               out[0] = 0xFF;
               out[1] = 0x99;
               out[2] = 0x99;
            }
         }
         void loadi(unsigned char *out, unsigned char R, unsigned char G, unsigned char B, unsigned char A) {   // Inhibit
            if (R + G + B < 3 * 128)
               out[3] = 0;
         }
         void scan(unsigned char *bmp, loader * load) {
            if (!bmp)
               return;
            unsigned char *r = bmp + bmp[10] + (bmp[11] << 8) + (bmp[12] << 16) + (bmp[13] << 24);
            int bpp = (bmp[28] + (bmp[29] << 8) + (bmp[30] << 16) + (bmp[31] << 24));
            int row = (bmp[22] + (bmp[23] << 8) + (bmp[24] << 16) + (bmp[25] << 24));
            int col = (bmp[18] + (bmp[19] << 8) + (bmp[20] << 16) + (bmp[21] << 24));
            int line = ((col * bpp + 7) / 8 + 3) / 4 * 4;
            unsigned char *ebmp = r + row * line;
            q = p;
            int y = 0;
            if (row > height)
               r += (row - height) / 2 * line;
            else
            {                   // too few rows, skip some in image
               y = (height - row) / 2;
               q += y * width * 4;
            }
            for (; y < height && q < e && r < ebmp; y++)
            {                   // Rows...
               unsigned char *i = r;
               r += line;
               int x,
                l = (col - width) / 2,
                   v = 0;
               for (x = 0; x < col; x++)
               {
                  int R = 0,
                      G = 0,
                      B = 0,
                      A = 0XFF;
                  if (bpp == 1)
                  {
                     if (!(x & 7))
                        v = *i++;
                     if (!(v & 0x80))
                        R = G = B = 0xFF;
                     v <<= 1;
                  } else if (bpp == 8)
                     R = G = B = *i++;
                  else if (!*bmp)
                  {             // RGBA from GM directly not real BMP
                     B = *i++;
                     G = *i++;
                     R = *i++;
                     if (bpp > 24)
                        A = *i++;
                  } else
                  {
                     R = *i++;
                     G = *i++;
                     B = *i++;
                     if (bpp > 24)
                        A = *i++;
                  }
                  if (x >= l && x < l + width)
                     load(q + (x - l) * 4, R, G, B, A);
               }
               q += width * 4;  // next row
            }
         }
         scan(c, loadc);
         scan(k, loadk);
         scan(i, loadi);
         scan(u, loadu);
      }
      if (c2 || k2 || i2 || u2)
      {
         load(c2, k2, i2, u2);
         p += width * height * 4;
      }
      load(c1, k1, i1, u1);
      f = 1;
      if (outfile)
         f = open(tmp2, O_WRONLY | O_CREAT, 0666);
      if (f < 0)
         errx(1, "Cannot write %s", outfile ? : "stdout");
      if (write(f, prev, s.st_size + 54) < 0)
         err(1, "write");
      close(f);
      // Convert to png or requested format using gm
      char tmp3[] = "/tmp/cardXXXXXX.png";
      if (outfile)
      {
         char *e = strrchr(outfile, '.');
         if (e && strlen(e) == 4 && isalnum(e[1]) && isalnum(e[2]) && isalnum(e[3]))
            strcpy(tmp + sizeof(tmp3) - 5, e);  // Extn
      }
      if ((t = mkstempsuffix(tmp3)) < 0)
         errx(1, "Bad tmp %s", tmp3);
      close(t);
      snprintf(cmd, sizeof(cmd), "gm convert %s -resize %dx%d %s", tmp2, width / scale, h / scale, tmp3);
      if (debug)
         fprintf(stderr, "%s\n", cmd);
      if (system(cmd))
         errx(1, "Convert failed");
      if (!keepfiles)
         unlink(tmp2);
      if (rename(tmp3, outfile))
      {
         if (errno != EXDEV)
            err(1, "Rename failed");
         // copy
         int i = open(tmp3, O_RDONLY);
         if (i < 0)
            err(1, "Cannot copy %s", tmp3);
         int o = open(outfile, O_WRONLY | O_CREAT, 05666);
         if (o < 0)
            err(1, "Cannot copy to %s", outfile);
         unsigned char tmp[1024];
         size_t l;
         while ((l = read(i, tmp, sizeof(tmp))) > 0)
            if (write(o, tmp, l) != l)
               err(1, "Bad copy to %s", outfile);
         close(i);
         close(o);
         unlink(tmp3);
      }
      chmod(outfile, 0666);
      return 0;
   }

   if (!strcasecmp(format, "zebra") || !strcasecmp(format, "matica") || !strcasecmp(format, "svg"))
   {                            // Generate matica file.
      int width = 0,
          height = 0;
      if (*format == 'Z')
      {                         // Zebra raw format (TODO inhibit on zebra)
         longedge = 1 - longedge;
         width = zebraw;
         height = zebrah;
      } else
      {                         // Matica raw format
         width = maticaw;
         height = maticah;
      }
      if (!strcasecmp(format, "svg"))
      {
         svg = xml_tree_new("svg");
         xml_element_set_namespace(svg, xml_namespace(svg, NULL, "http://www.w3.org/2000/svg"));
         xml_namespace(svg, "xlink", "http://www.w3.org/1999/xlink");
         xml_add(svg, "@version", "1.1");
         xml_addf(svg, "@width", "%fmm", 25.4 * width / dpi);
         xml_addf(svg, "@height", "%fmm", 25.4 * height / dpi);
         xml_addf(svg, "@viewBox", "0 0 %d %d", width, height);
         xml_addf(svg, "@dpi", "%d", dpi);
         xml_addf(svg, "@cols", "%d", width);
         xml_addf(svg, "@rows", "%d", height);
      }
      c1 = loadfile("C1", c1file, width, height, 24, 0);
      k1 = loadfile("K1", k1file, width, height, black, 0);
      u1 = loadfile("U1", u1file, width, height, 8, 0);
      c2 = loadfile("C1", c2file, width, height, 24, !longedge);
      k2 = loadfile("K2", k2file, width, height, black, !longedge);
      u2 = loadfile("U2", u2file, width, height, 8, !longedge);
      int o = fileno(stdout);
      if (svg)
      {
         FILE *f = fdopen(o, "w");
         xml_write(f, svg);
         fflush(f);
         fclose(f);
         xml_tree_delete(svg);
      } else
      {
         if (outfile && (o = open(outfile, O_WRONLY | O_CREAT, 0666)) < 0)
            err(1, "Cannot write %s", outfile);
         void panel(unsigned char *bmp, int colour) {   // Output a panel
            unsigned char p[height * width];
            memset(p, 0, sizeof(p));
            if (bmp)
            {
               unsigned char *r = bmp + bmp[10] + (bmp[11] << 8) + (bmp[12] << 16) + (bmp[13] << 24);
               int bpp = (bmp[28] + (bmp[29] << 8) + (bmp[30] << 16) + (bmp[31] << 24));
               int row = (bmp[22] + (bmp[23] << 8) + (bmp[24] << 16) + (bmp[25] << 24));
               int col = (bmp[18] + (bmp[19] << 8) + (bmp[20] << 16) + (bmp[21] << 24));
               int line = ((col * bpp + 7) / 8 + 3) / 4 * 4;
               if (debug)
                  fprintf(stderr, "%dx%dx%d\n", col, row, bpp);
               int y = 0;
               if (row > height)
                  r += (row - height) / 2 * line;
               else
                  y = (height - row) / 2;
               for (; y < height && row--; y++)
               {
                  unsigned char *i = r;
                  r += line;
                  int x = 0,
                      c = col;
                  if (col > width)
                  {             // Skip half extra cols
                     if (bpp == 1)
                        i += (col - width) / 16;        // not ideal
                     else
                        i += (col - width) / 2 * (bpp / 8);
                  } else
                     x = (width - col) / 2;
                  unsigned char *q = p + width * (height - y - 1),
                      v = 0;
                  for (; x < width && c--; x++)
                  {
                     if (bpp == 1)
                     {
                        if (!(x & 7))
                           v = *i++;
                        if ((v & 0x80))
                           q[x] = 0xFF;
                        v <<= 1;
                     } else
                     {
                        if (colour < 0)
                           q[x] = ((i[0] < 0x80 && i[3] > 0x80) ? 0xFF : 0);
                        else
                        {
                           if (colour > 0 && bpp >= 24)
                              q[x] = (i[colour - 1] ^ 0xFF);
                           else
                              q[x] = (*i ^ 0xFF);
                           if (bpp == 32)
                              q[x] = q[x] * i[3] / 255;
                        }
                        i += bpp / 8;
                     }
                  }
               }
            }
            if (write(o, p, sizeof(p)) < 0)
               err(1, "write");
         }
         panel(c1, 3);
         panel(c1, 2);
         panel(c1, 1);
         panel(k1, -1);
         panel(u1, 0);
         panel(c2, 3);
         panel(c2, 2);
         panel(c2, 1);
         panel(k2, -1);
         panel(u2, 0);
      }
      close(o);
      if (outfile)
         chmod(outfile, 0666);
      return 0;
   }
   return 0;
}
