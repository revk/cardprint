// Card handling - convert SVG to matica for printing
// (c) 2018 Adrian Kennard Andrews & Arnold Ltd
// Expects inkscape tagged print layers (id) C1, K1, U1, C2, K2, U2
// Top level XML tags handled
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
#include <ajlparse.h>
#include "xidsvg.h"

#ifndef LIB
#define xquoted(x)      #x
#define quoted(x)       xquoted(x)
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
int preview = 0;
int loaded = 0;
int retain = 0;
int uvsingle = 0;
int copies = 1;
const char *input = NULL;
const char *output = NULL;
char *jsstatus = NULL;
int count = 0;
xml_t svg = NULL;
#endif

ssize_t xid_write_func(void *arg, void *buf, size_t len)
{
   return SSL_write(arg, buf, len);
}

ssize_t xid_read_func(void *arg, void *buf, size_t len)
{
   return SSL_read(arg, buf, len);
}

void xid_status_null(const char *s)
{
}

static xid_status_t *status = xid_status_null;

void xid_set_status(xid_status_t * s)
{
   status = s;
}

j_t xid_compose(xml_t svg, int dpi, int rows, int cols)
{                               // Convert SVG to png
   status("Compose");
   if (dpi < 0)
      dpi = 300;
   if (rows < 0)
      rows = 664 * dpi / 300;
   if (cols < 0)
      cols = 1036 * dpi / 300;
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
   for (int side = 0; side < 2; side++)
      for (int layer = 0; layer < 3; layer++)
      {
         xml_t find(xml_t x) {
            const char *v = xml_get(x, "@id");
            if (v && v[0] == layertag[layer] && v[1] == '1' + side && !v[2])
               return x;
            xml_t e = NULL,
                q;
            while ((e = xml_element_next(x, e)))
               if ((q = find(e)))
                  return q;
            return NULL;
         }
         if (!find(svg))
            continue;
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
            int a = 0;
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
            int n = open("/dev/null", 0);
            dup2(n, 1);
            dup2(n, 2);
            close(n);
            execv("/usr/bin/inkscape", (char *const *) args);
            err(1, "Failed to run inkscape");
         }
      }
   for (int side = 0; side < 2; side++)
      for (int layer = 0; layer < 3; layer++)
         if (tmp[side][layer])
         {
            int pstatus = 0;
            waitpid(pid[side][layer], &pstatus, 0);
            if (!WIFEXITED(pstatus) || WEXITSTATUS(pstatus))
               status("*SVG conversion fail");
         }
   j_t j = j_create();
   j_t p = j_store_array(j, "print");
   unsigned char *panel = malloc(cols * rows);
   for (int side = 0; side < 2; side++)
   {
      j_t s = j_append_object(p);
      for (int layer = 0; layer < 3; layer++)
         if (tmp[side][layer])
         {
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
            else
            {
               const char *tag[] = { "C", "K", "U" };
               size_t len = (length + 5) / 6 * 8 + 3;
               char *b64 = malloc(len);
               if (!b64)
                  errx(1, "malloc");
               j_store_stringf(s, tag[layer], "data:image/png;base64,%s", j_baseN(length, addr, len, b64, JBASE64, 6));
               free(b64);
               munmap(addr, length);
            }
            close(f);
         }
   }
   free(panel);
   // Cleanup
   unlink(tmpsvg);
   free(tmpsvg);
   for (int side = 0; side < 2; side++)
      for (int layer = 0; layer < 3; layer++)
         if (tmp[side][layer])
         {
            unlink(tmp[side][layer]);
            free(tmp[side][layer]);
         }
   return j;
}

const char *xid_connect(const char *xidserver, const char *keyfile, const char *certfile, ajl_t * i, ajl_t * o)
{                               // Send to xidserver
   if (i)
      *i = NULL;
   if (o)
      *o = NULL;
   if (!xidserver)
      return "No server";
   int psock = -1;
   struct addrinfo base = { 0, PF_UNSPEC, SOCK_STREAM };
   struct addrinfo *res = NULL,
       *a;
   int r = getaddrinfo(xidserver, "7810", &base, &res);
   if (r)
      return "Cannot locate to print server";
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
      return "Cannot connect to print server";
   SSL_library_init();
   SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());  // Negotiates TLS
   if (!ctx)
      return "Cannot make ctx";
   if (certfile && SSL_CTX_use_certificate_chain_file(ctx, certfile) != 1)
      return "Cannot load cert file";
   if (keyfile && SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1)
      return "Cannot load key file";
   SSL *ss = SSL_new(ctx);
   if (!ss)
   {
      close(psock);
      return "Cannot make TLS";
   }
   SSL_set_fd(ss, psock);
   if (SSL_connect(ss) != 1)
   {
      close(psock);
      return "Cannot connect to xid server";
   }
   if (i)
      *i = ajl_read_func(xid_read_func, ss);
   if (o)
      *o = ajl_write_func(xid_write_func, ss);
   return NULL;
}

void xid_disconnect(ajl_t * i, ajl_t * o)
{
   SSL *ss = ajl_arg(*i);
   if (ss)
   {
      int fd = SSL_get_fd(ss);
      SSL_shutdown(ss);
      SSL_free(ss);
      close(fd);
   }
   ajl_delete(i);
   ajl_delete(o);
}

#ifndef LIB
void mystatus(const char *status)
{                               // Report status (if start * then error)
   if (jsstatus)
   {
      printf("<script>document.getElementById('%s').innerHTML='%s';</script>", jsstatus, status);
      fflush(stdout);
   }
   if (*status == '*')
      errx(1, "%s", status + 1);
}

int main(int argc, const char *argv[])
{
   char *xidserver = getenv("CARDPRINTER");;
   const char *certfile = NULL;
   const char *keyfile = NULL;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "xidserver", 'S', POPT_ARG_STRING, &xidserver, 0, "Send to xidserver", "hostname" },
         { "key-file", 'k', POPT_ARG_STRING, &keyfile, 0, "SSL client key file", "filename" },
         { "cert-file", 'k', POPT_ARG_STRING, &certfile, 0, "SSL client cert file", "filename" },
         { "loaded", 'L', POPT_ARG_NONE, &loaded, 0, "Expect card to be loaded" },
         { "retain", 'K', POPT_ARG_NONE, &retain, 0, "Retain card" },
         { "uv-single", 0, POPT_ARG_NONE, &uvsingle, 0, "UV on same retransfer" },
         { "copies", 'N', POPT_ARGFLAG_SHOW_DEFAULT | POPT_ARG_INT, &copies, 0, "Copies", "N" },
         { "js-status", 'j', POPT_ARG_STRING, &jsstatus, 0, "Javascript output", "html-ID" },
         { "preview", 'p', POPT_ARG_NONE, &preview, 0, "Make image preview" },
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
      if (poptPeekArg(optCon) || (!xidserver && !preview))
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      poptFreeContext(optCon);
   }
   xid_set_status(mystatus);
   if (output && !freopen(output, "w", stdout))
      mystatus("*Cannot open output");
   if (input && !freopen(input, "r", stdin))
      mystatus("*Cannot open input");
   // Read SVG
   svg = xml_tree_read(stdin);
   if (!svg)
      mystatus("*Cannot load svg");
   int v;
   if ((v = atoi(xml_get(svg, "@dpi") ? : "")))
   {
      if (dpi < 0)
         dpi = v;
      else if (dpi != v)
         mystatus("*DPI mismatch");
   }
   if ((v = atoi(xml_get(svg, "@rows") ? : "")))
   {
      if (rows < 0)
         rows = v;
      else if (rows != v)
         mystatus("*Rows mismatch");
   }
   if ((v = atoi(xml_get(svg, "@cols") ? : "")))
   {
      if (cols < 0)
         cols = v;
      else if (cols != v)
         mystatus("*Cols mismatch");
   }

   if (preview)
   {
      j_t j = xid_compose(svg, dpi, rows, cols);
      j_t p = j_find(j, "print");
      for (j_t s = j_first(p); s; s = j_next(s))
         for (j_t l = j_first(s); l; l = j_next(l))
            printf("<img border=1 src=\"%s\">\n", j_val(l));
      j_delete(&j);
   }

   if (!preview && xidserver)
   {
      ajl_t i,
       o;
      {
         const char *er = xid_connect(xidserver, keyfile, certfile, &i, &o);
         if (er)
            errx(1, "%s", er);
      }
      j_t j = j_create();
      char *er = j_recv(j, i);
      int n;
      if (!er && (n = atoi(j_get(j, "dpi") ? : "")))
      {
         if (dpi < 0)
            dpi = n;
         else if (dpi != n)
            er = strdup("DPI mismatch (printer)");
      }
      if (!er && (n = atoi(j_get(j, "rows") ? : "")))
      {
         if (rows < 0)
            rows = n;
         else if (rows != n)
            er = strdup("Rows mismatch (printer)");
      }
      if (!er && (n = atoi(j_get(j, "cols") ? : "")))
      {
         if (cols < 0)
            cols = n;
         else if (cols != n)
            er = ("Cols mismatch (printer)");
      }
      const char *v;
      if ((v = j_get(j, "status")))
         status(v);
      while (j_test(j, "wait", 0) && !(er = j_recv(j, i)))
         if ((v = j_get(j, "status")))
            status(v);
      if (!er)
      {
         j_t j = xid_compose(svg, dpi, rows, cols);
         status("Printing");
         j_err(j_send(j, o));
         j_delete(&j);
      }
      if (!er)
         while (!(er = j_recv(j, i)))
         {
            if ((v = j_get(j, "status")))
               status(v);
            if ((v = j_get(j, "count")))
               count = atoi(v);
            if (j_find(j, "error"))
               er = strdup(j_get(j, "error.description"));
         }
      j_delete(&j);
      xid_disconnect(&i, &o);
      if (er && *er)
         status(er);
      else if (!count)
         status("Nothing printed");
      if (er)
         free(er);
   }

   xml_tree_delete(svg);
   return count ? 0 : 1;
}
#endif
