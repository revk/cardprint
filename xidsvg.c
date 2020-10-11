// Card handling - convert SVG to matica for printing
// (c) 2018 Adrian Kennard Andrews & Arnold Ltd
// Expects print layers (id) C1, K1, U1, C2, K2, U2, and @layers and @sides defined, etc
// Expects to be convertible at 300dpi to an image covering card (ideally at least 1024x648 to 1036x664), but centres if smaller/larger
// At present this prints to matica, but the idea is that if we make other drivers this will have options to use them instead

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <err.h>
#include <signal.h>
#include <execinfo.h>
#include <axl.h>
#include <ajl.h>

#define xquoted(x)      #x
#define quoted(x)       xquoted(x)
const char xidport[] = "7810";
#ifdef	XIDSERVER
char *xidserver = quoted(XIDSERVER);
#else
char *xidserver = NULL;
#endif
#ifdef	PRINTCOLS
const int cols = PRINTCOLS;
#else
int cols = -1;
#endif
#ifdef	PRINTROWS
const int rows = PRINTROWS;
#else
int rows = -1;
#endif
#ifdef	DPI
const int dpi = DPI;
#else
int dpi = -1;
#endif

int debug = 0;
int png = 0;
int loaded = 0;
int retain = 0;
int uvsingle = 0;
int copies = 1;
const char *input = NULL;
const char *output = NULL;
char *jsstatus = NULL;

ssize_t ss_write_func(void *arg, void *buf, size_t len)
{
   return SSL_write(arg, buf, len);
}

ssize_t ss_read_func(void *arg, void *buf, size_t len)
{
   return SSL_read(arg, buf, len);
}

int main(int argc, const char *argv[])
{
   int debugimg = 0;
   const char *certfile = NULL;
   const char *keyfile = NULL;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
#ifndef	XIDSERVER
         { "xidserver", 'S', POPT_ARG_STRING, &xidserver, 0, "Send to xidserver", "hostname" },
#endif
         { "key-file", 'k', POPT_ARG_STRING, &keyfile, 0, "SSL client key file", "filename" },
         { "cert-file", 'k', POPT_ARG_STRING, &certfile, 0, "SSL client cert file", "filename" },
         { "loaded", 'L', POPT_ARG_NONE, &loaded, 0, "Expect card to be loaded" },
         { "retain", 'K', POPT_ARG_NONE, &retain, 0, "Retain card" },
         { "uv-single", 0, POPT_ARG_NONE, &uvsingle, 0, "UV on same retransfer" },
         { "copies", 'N', POPT_ARGFLAG_SHOW_DEFAULT | POPT_ARG_INT, &copies, 0, "Copies", "N" },
         { "js-status", 'j', POPT_ARG_STRING, &jsstatus, 0, "Javascript output", "html-ID" },
         { "png", 'p', POPT_ARG_NONE, &png, 0, "Make PNG instead of printing" },
#ifndef	DPI
         { "dpi", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &dpi, 0, "DPI", "dpi" },
#endif
#ifndef	COLS
         { "cols", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &cols, 0, "Columns", "pixels" },
#endif
#ifndef	ROWS
         { "rows", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &rows, 0, "Rows", "pixels" },
#endif
         { "input", 'i', POPT_ARG_STRING, &input, 0, "Input file (else stdin)", "filename" },
         { "output", 'o', POPT_ARG_STRING, &output, 0, "Output file (else stdout)", "filename" },
         { "debug-img", 0, POPT_ARG_NONE, &debugimg, 0, "Debug img" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
         POPT_AUTOHELP { }
      };

      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      poptSetOtherOptionHelp(optCon, "[in] [out]");

      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));

      if (!input && poptPeekArg(optCon))
         input = poptGetArg(optCon);
      if (!output && poptPeekArg(optCon))
         output = poptGetArg(optCon);
      if (poptPeekArg(optCon) || (!xidserver && !png))
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      poptFreeContext(optCon);
   }
   if (output && !freopen(output, "w", stdout))
      err(1, "Cannot open %s", output);
   if (input && !freopen(input, "r", stdin))
      err(1, "Cannot open %s", input);
   if (dpi < 0)
      dpi = 300;
   if (rows < 0)
      rows = 664 * dpi / 300;
   if (cols < 0)
      cols = 1036 * dpi / 300;
   // Read SVG
   xml_t svg = xml_tree_read(stdin);
   if (!svg)
      errx(1, "Cannot load SVG");
   int sides = atoi(xml_get(svg, "@sides") ? : "");
   if (!sides || sides > 2)
      errx(1, "Needs to be created for print, with @sides at top level");
   int layers = atoi(xml_get(svg, "@layers") ? : "");
   if (!layers || layers > 3)
      errx(1, "Needs to be created for print, with @layers at top level");
   char *mag1 = xml_get(svg, "@track1");
   char *mag2 = xml_get(svg, "@track2");
   char *mag3 = xml_get(svg, "@track3");
   if (debug)
      fprintf(stderr, "%d side%s, %d layer%s\n", sides, sides == 1 ? "" : "s", layers, layers == 1 ? "" : "s");

   char *tmpsvg = strdup("/tmp/cardXXXXXX.svg");
   {
      int f = mkstemps(tmpsvg, 4);
      if (f < 0)
         errx(1, "Cannot make temp %s", tmpsvg);
      FILE *o = fdopen(f, "w");
      xml_write(o, svg);
      fclose(o);
   }
   const char layertag[] = "CKU";
   char *tmp[2][3] = { };
   pid_t pid[2][3] = { };
   for (int side = 0; side < sides; side++)
      for (int layer = 0; layer < layers; layer++)
      {
         tmp[side][layer] = strdup("/tmp/cardXX-XXXXXX.png");
         tmp[side][layer][9] = layertag[layer];
         tmp[side][layer][10] = '1' + side;
         int f = mkstemps(tmp[side][layer], 4);
         if (f < 0)
            errx(1, "Cannot make temp %s", tmp[side][layer]);
         close(f);
         char id[3] = { layertag[layer], '1' + side };
         if (!(pid[side][layer] = fork()))
         {
            char *args[100];
            int a = 0,
                i;
            args[a++] = "inkscape";
            args[a++] = "--without-gui";
            args[a++] = "--export-area-page";
            args[a++] = "--export-id-only";
            if (asprintf(&args[a++], "--export-png=%s", tmp[side][layer]) < 0)
               errx(1, "malloc");
            if (asprintf(&args[a++], "--export-dpi=%d", dpi) < 0)
               errx(1, "malloc");
            if (asprintf(&args[a++], "--export-id=%s", id) < 0)
               errx(1, "malloc");
            args[a++] = tmpsvg;
            args[a++] = NULL;
            if (debug)
               for (i = 0; i < a; i++)
                  fprintf(stderr, "%s%s", i ? " " : "", args[i] ? : "\n");
            int n = open("/dev/null", 0);
            dup2(n, 1);
            dup2(n, 2);
            close(n);
            execv("/usr/bin/inkscape", (char *const *) args);
            return 1;
         }
      }
   for (int side = 0; side < sides; side++)
      for (int layer = 0; layer < layers; layer++)
      {
         int status = 0;
         waitpid(pid[side][layer], &status, 0);
         if (!WIFEXITED(status) || WEXITSTATUS(status))
            errx(1, "inkscape failed");
      }

   if (png)
   {                            // Make png montage
      char *tmppng = strdup("/tmp/cardXXXXXX.png");
      {
         int f = mkstemps(tmppng, 4);
         if (f < 0)
            errx(1, "Cannot make temp %s", tmppng);
         close(f);
      }
      int status = 0;
      pid_t child = fork();
      if (child)
         waitpid(child, &status, 0);
      else
      {
         char *args[100];
         int a = 0,
             i;
         args[a++] = "gm";
         args[a++] = "montage";
         args[a++] = "-gravity";
         args[a++] = "center";
         args[a++] = "-geometry";
         if (asprintf(&args[a++], "%dx%d", cols, rows) < 0)
            errx(1, "malloc");
         args[a++] = "-tile";
         if (asprintf(&args[a++], "%dx%d", sides, layers) < 0)
            errx(1, "malloc");
         for (int layer = 0; layer < layers; layer++)
            for (int side = 0; side < sides; side++)
               args[a++] = tmp[side][layer];
         args[a++] = tmppng;
         args[a++] = NULL;
         if (debug)
            for (i = 0; i < a; i++)
               fprintf(stderr, "%s%s", i ? " " : "", args[i] ? : "\n");
         int n = open("/dev/null", 0);
         dup2(n, 1);
         dup2(n, 2);
         close(n);
         execv("/usr/bin/gm", (char *const *) args);
         return 1;
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status))
         errx(1, "gm failed");
      FILE *f = fopen(tmppng, "r");
      int c;
      while ((c = fgetc(f)) >= 0)
         putchar(c);
      fclose(f);
      if (!debug)
         unlink(tmppng);
      free(tmppng);
   }

   if (xidserver)
   {                            // Send to xidserver
      // Make JSON
      j_t j = j_create();
      if (mag1 || mag2 || mag3)
      {
         j_t m = j_store_array(j, "mag");
         j_append_string(m, mag1);
         j_append_string(m, mag2);
         j_append_string(m, mag3);
      }
      j_t p = j_store_array(j, "print");
      unsigned char *panel = malloc(cols * rows);
      for (int side = 0; side < sides; side++)
      {
         j_t s = j_append_object(p);
         void add(int layer) {  // base64 uses alloca so make a function, why not
            int f = open(tmp[side][layer], O_RDONLY);
            if (f < 0)
               err(1, "Cannot open %s", tmp[side][layer]);
            struct stat st;
            if (fstat(f, &st) < 0)
               err(1, "Cannot stat file %s", tmp[side][layer]);
            size_t length = st.st_size;
            void *addr = mmap(NULL, length, PROT_READ, MAP_PRIVATE, f, 0);
            if (addr == MAP_FAILED)
               err(1, "Cannot map file %s", tmp[side][layer]);
            const char *tag[] = { "C", "K", "U" };
            j_store_stringf(s, tag[layer], "data:image/png;base64,%s", j_base64(length, addr));
            munmap(addr, length);
            close(f);
            if (debugimg)
               printf("<img src='%s'>\n", j_get(s, tag[layer]));
         }
         for (int layer = 0; layer < layers; layer++)
            add(layer);
      }
      free(panel);
      // Send
      int psock = -1;
      struct addrinfo base = { 0, PF_UNSPEC, SOCK_STREAM };
      struct addrinfo *res = NULL,
          *a;
      int r = getaddrinfo(xidserver, xidport, &base, &res);
      if (r)
         errx(1, "Cannot get addr info %s", xidserver);
      for (a = res; a; a = a->ai_next)
      {
         int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
         if (s >= 0)
         {
            if (!connect(s, a->ai_addr, a->ai_addrlen))
            {
               psock = s;
               break;
            }
            close(s);
         }
      }
      freeaddrinfo(res);
      if (psock < 0)
      {
         if (jsstatus)
            printf("<script>document.getElementById('%s').innerHTML='%s';</script>", jsstatus, "Not connected");
         fflush(stdout);
         errx(1, "Not connected to xidserver");
      }
      SSL_library_init();
      SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());       // Negotiates TLS
      if (!ctx)
         errx(1, "Cannot make ctx");
      if (certfile && SSL_CTX_use_certificate_chain_file(ctx, certfile) != 1)
         errx(1, "Cannot load cert file");
      if (keyfile && SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1)
         errx(1, "Cannot load key file");
      SSL *ss = SSL_new(ctx);
      if (!ss)
         errx(1, "Cannot make TLS");
      if (!SSL_set_fd(ss, psock))
         errx(1, "Cannot connect socket");
      if (SSL_connect(ss) != 1)
         errx(1, "Cannot connect to xid server");
      char *jin(j_t i) {
         if (debug)
            j_err(j_write_pretty(i, stderr));
         const char *v;
         if (jsstatus && (v = j_get(i, "status")))
         {
            printf("<script>document.getElementById('%s').innerHTML='%s';</script>", jsstatus, v);
            fflush(stdout);
         }
         if (j_find(i, "error"))
         {
            v = strdup(j_get(i, "error.description"));
            if (jsstatus)
            {
               printf("<script>document.getElementById('%s').innerHTML='%s';</script>", jsstatus, v);
               fflush(stdout);
            }
            return (char *) v;
         }
         if ((v = j_get(i, "dpi")) && atoi(v) != dpi)
            return strdup("DPI mismatch");
         if ((v = j_get(i, "rows")) && atoi(v) != rows)
            return strdup("Rows mismatch");
         if ((v = j_get(i, "cols")) && atoi(v) != cols)
            return strdup("Cols mismatch");
         if (j_find(i, "id") && j)
         {                      // Send print
            j_err(j_write_func(j, ss_write_func, ss));
            j_delete(&j);
         }
         return NULL;
      }
      char *er = j_stream_func(ss_read_func, ss, jin);
      SSL_shutdown(ss);
      SSL_free(ss);
      close(psock);
      j_delete(&j);
      if (er && *er)
         errx(1, "Failed %s", er);
   }
   // Cleanup
   if (!debug)
      unlink(tmpsvg);
   free(tmpsvg);
   for (int side = 0; side < sides; side++)
      for (int layer = 0; layer < layers; layer++)
      {
         if (!debug)
            unlink(tmp[side][layer]);
         free(tmp[side][layer]);
      }
   xml_tree_delete(svg);

   return 0;
}
