// Card handling - convert SVG to matica for printing
// (c) 2018 Adrian Kennard Andrews & Arnold Ltd
// Expects inkscape tagged print layers (id) C1, K1, U1, C2, K2, U2
// Top level XML tags handled
// sides        Number of sides, 1 or 2
// layers       Number of layers 1 to 3 (colour, black, UK)
// rows         Number of print pixel rows expected
// cols         Number of print pixel cols expected
// dpi          DPI expected

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
int img = 0;
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
   int count = 0;
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
         { "img", 'p', POPT_ARG_NONE, &img, 0, "Make image preview" },
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
      if (poptPeekArg(optCon) || (!xidserver && !img))
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      poptFreeContext(optCon);
   }
   void status(const char *status) {    // Report status (if start * then error)
      if (jsstatus)
      {
         printf("<script>document.getElementById('%s').innerHTML='%s';</script>", jsstatus, status);
         fflush(stdout);
      }
      if (*status)
         errx(1, "%s", status + 1);
   }

   if (output && !freopen(output, "w", stdout))
      status("*Cannot open output");
   if (input && !freopen(input, "r", stdin))
      status("*Cannot open input");
   // Read SVG
   xml_t svg = xml_tree_read(stdin);
   if (!svg)
      status("*Cannot load svg");
   int sides = atoi(xml_get(svg, "@sides") ? : "");
   if (!sides || sides > 2)
      status("Not created for print, expects \"sides\"= at top level");
   int layers = atoi(xml_get(svg, "@layers") ? : "");
   if (!layers || layers > 3)
      status("Not created for print, expects \"layerss\"= at top level");
   int v;
   if ((v = atoi(xml_get(svg, "@dpi") ? : "")))
   {
      if (dpi < 0)
         dpi = v;
      else if (dpi != v)
         status("*DPI mismatch");
   }
   if ((v = atoi(xml_get(svg, "@rows") ? : "")))
   {
      if (rows < 0)
         rows = v;
      else if (rows != v)
         status("*Rows mismatch");
   }
   if ((v = atoi(xml_get(svg, "@cols") ? : "")))
   {
      if (cols < 0)
         cols = v;
      else if (cols != v)
         status("*Cols mismatch");
   }
   if (dpi < 0)
      dpi = 300;
   if (rows < 0)
      rows = 664 * dpi / 300;
   if (cols < 0)
      cols = 1036 * dpi / 300;
   char *mag1 = xml_get(svg, "@track1");
   char *mag2 = xml_get(svg, "@track2");
   char *mag3 = xml_get(svg, "@track3");

   status("Compose");
   char *tmpsvg = strdup("/tmp/cardXXXXXX.svg");
   {
      int f = mkstemps(tmpsvg, 4);
      if (f < 0)
         status("*Cannot make svg tmp file");
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
            status("*Cannot make PNG tmp file");
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
               status("*malloc");
            if (asprintf(&args[a++], "--export-dpi=%d", dpi) < 0)
               status("*malloc");
            if (asprintf(&args[a++], "--export-id=%s", id) < 0)
               status("*malloc");
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
         int pstatus = 0;
         waitpid(pid[side][layer], &pstatus, 0);
         if (!WIFEXITED(pstatus) || WEXITSTATUS(pstatus))
            status("*SVG conversion fail");
      }

   if (xidserver || img)
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
               status("*Cannot open png tmp file");
            struct stat st;
            if (fstat(f, &st) < 0)
               status("*Cannot stat png tmp file");
            size_t length = st.st_size;
            void *addr = mmap(NULL, length, PROT_READ, MAP_PRIVATE, f, 0);
            if (addr == MAP_FAILED)
               status("*Cannot map png tmp file");
            const char *tag[] = { "C", "K", "U" };
            j_store_stringf(s, tag[layer], "data:image/png;base64,%s", j_base64(length, addr));
            munmap(addr, length);
            close(f);
            if (img)
               printf("<img border=1 src='%s'>\n", j_get(s, tag[layer]));
         }
         for (int layer = 0; layer < layers; layer++)
            add(layer);
      }
      free(panel);
      if (xidserver)
      {                         // Send
         status("Connecting");
         int psock = -1;
         struct addrinfo base = { 0, PF_UNSPEC, SOCK_STREAM };
         struct addrinfo *res = NULL,
             *a;
         int r = getaddrinfo(xidserver, xidport, &base, &res);
         if (r)
            status("*Cannot locate to print server");
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
            {
               printf("<script>document.getElementById('%s').innerHTML='%s';</script>", jsstatus, "Not connected");
               fflush(stdout);
            }
            status("*Cannot connect to print server");
         }
         if (jsstatus)
         {
            printf("<script>document.getElementById('%s').innerHTML='%s';</script>", jsstatus, "Queued");
            fflush(stdout);
         }
         SSL_library_init();
         SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());    // Negotiates TLS
         if (!ctx)
            status("*Cannot make ctx");
         if (certfile && SSL_CTX_use_certificate_chain_file(ctx, certfile) != 1)
            status("*Cannot load cert file");
         if (keyfile && SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1)
            status("*Cannot load key file");
         SSL *ss = SSL_new(ctx);
         if (!ss)
            status("*Cannot make TLS");
         if (!SSL_set_fd(ss, psock))
            status("*Cannot connect socket");
         if (SSL_connect(ss) != 1)
            status("*Cannot connect to xid server");
         status("Connected");
         char *jin(j_t i) {
            if (debug)
               j_err(j_write_pretty(i, stderr));
            const char *v;
            if ((v = j_get(i, "count")))
               count = atoi(v);
            if ((v = j_get(i, "status")))
               status(v);
            if (j_find(i, "error"))
            {
               v = strdup(j_get(i, "error.description"));
               status(v);
               return (char *) v;
            }
            if ((v = j_get(i, "dpi")) && atoi(v) != dpi)
               return strdup("DPI mismatch");
            if ((v = j_get(i, "rows")) && atoi(v) != rows)
               return strdup("Rows mismatch");
            if ((v = j_get(i, "cols")) && atoi(v) != cols)
               return strdup("Cols mismatch");
            if (j_find(i, "id") && j)
            {                   // Send print
               j_err(j_write_func(j, ss_write_func, ss));
               j_delete(&j);
            }
            return NULL;
         }
         char *er = j_stream_func(ss_read_func, ss, jin);
         SSL_shutdown(ss);
         SSL_free(ss);
         close(psock);
         if (er && *er)
            errx(1, "Failed %s", er);
      }
      j_delete(&j);
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
   return count ? 0 : 1;
}
