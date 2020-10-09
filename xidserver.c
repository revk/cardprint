// Matica XID printer back end server
// Copyright © Adrian Kennard, Andrews & Arnold Ltd
// This is back end designed to run on a Raspberry pi and connect to an XID printer
// It operates on a TCP socket over which back to back JSON objects are sent each way
// This allows printing, moving cards, and access to USB connected in printer contact/contactless station
// The idea is that the interactive print job is performed in one connection, with others queued.

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <err.h>
#include <ajl.h>

// As this is single threader operation, on job at a time, we are using globals :-)

// Config
int debug = 0;                  // Top level debug
const char *printhost = NULL;   // Printer host/IP
const char *printport = "50730";        // Printer port (this is default for XID8600)

// Current connectionns
FILE *client = NULL;            // Connected client
int psock = -1;                 // Connected printer (ethernet)
unsigned char *buf = NULL;      // Printer message buffer
unsigned int buflen = 0;        // Buffer length
unsigned int bufmax = 0;        // Max buffer space malloc'd
unsigned int buftxcmd = 0;      // Last tx command
unsigned int bufrxcmd = 0;      // Last rx command
unsigned int seq = 0;           // Sequence
int posn = 0;                   // Current card position

const char *printer_connect(void)
{                               // Connect to printer, return error if fail
   // TODO USB?
   if (psock >= 0)
      return "Printer already connected";
   struct addrinfo base = { 0, PF_UNSPEC, SOCK_STREAM };
   struct addrinfo *res = NULL,
       *a;
   int r = getaddrinfo(printhost, printport, &base, &res);
   if (r)
      errx(1, "Cannot get addr info %s", printhost);
   for (a = res; a; a = a->ai_next)
   {
      int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s >= 0)
      {
         if (!connect(s, a->ai_addr, a->ai_addrlen))
         {
            psock = s;
            return NULL;        // Connected
         }
         close(s);
      }
   }
   freeaddrinfo(res);
   return "Could not connect to printer";
}

char *printer_disconnect(void)
{                               // Disconnect from printer
   if (psock < 0)
      return NULL;              // Not connected, that is OK
   close(psock);
   psock = -1;
   return NULL;
}

const char *printer_tx(void)
{                               // Raw printer send
   if (psock < 0)
      return "Printer not connected";
   if (buflen < 12)
      return "Bad tx";
   buftxcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   buflen = (buflen + 3) / 4 * 4;
   unsigned int n = buflen / 4 - 2;
   buf[4] = (n >> 24);
   buf[5] = (n >> 16);
   buf[6] = (n >> 8);
   buf[7] = (n);
   if (debug)
   {
      fprintf(stderr, "Tx:");
      int i = 0;
      for (i = 0; i < buflen && i < 200; i++)
         fprintf(stderr, "%s%02X", i && !(i & 31) ? "\n    " : (i & 3) ? "" : " ", buf[i]);
      if (i < buflen)
         fprintf(stderr, "... (%d)", buflen);
      fprintf(stderr, "\n");
   }
   n = 0;
   while (n < buflen)
   {
      ssize_t l = write(psock, buf + n, buflen - n);
      if (l <= 0)
      {
         warn("Tx %d", l);
         return "Tx fail";
      }
      n += l;
   }
   return NULL;
}

const char *printer_rx(void)
{                               // raw printer receive
   if (psock < 0)
      return "Printer not connected";
   bufrxcmd = 0;
   buflen = 0;
   unsigned int n = 8;
   while (buflen < n)
   {
      if (bufmax < n && !(buf = realloc(buf, bufmax = n)))
         errx(1, "malloc");
      ssize_t l = read(psock, buf + buflen, n - buflen);
      if (l <= 0)
      {
         warn("Rx %d", l);
         return "Rx fail";
      }
      buflen += l;
      if (buflen == 8)
         n = ((buf[4] << 24) + (buf[5] << 16) + (buf[6] << 8) + buf[7]) * 4 + 8;
   }
   if (debug)
   {
      fprintf(stderr, "Rx:");
      int i = 0;
      for (i = 0; i < buflen && i < 200; i++)
         fprintf(stderr, "%s%02X", i && !(i & 31) ? "\n    " : (i & 3) ? "" : " ", buf[i]);
      if (i < buflen)
         fprintf(stderr, "... (%d)", buflen);
      fprintf(stderr, "\n");
   }
   if (buflen < 12)
      return "Bad rx length";
   bufrxcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   return NULL;
}

void printer_msg(unsigned int cmd)
{                               // Start message
   if (bufmax < 12 && (buf = realloc(buf, bufmax = 12)))
      errx(1, "malloc");
   buf[0] = (cmd >> 24);
   buf[1] = (cmd >> 16);
   buf[2] = (cmd >> 8);
   buf[3] = (cmd);
   buf[8] = (seq >> 24);
   buf[9] = (seq >> 16);
   buf[10] = (seq >> 8);
   buf[11] = (seq);
   seq++;
   buflen = 12;
}

const char *client_tx(j_t j)
{                               // Send data to client (deletes)
   if (!client)
      return "No client";
   if (debug)
      j_err(j_write_pretty(j, stderr));
   j_err(j_write(j, client));
   fflush(client);
   j_delete(&j);
}

char *client_rx(j_t j)
{
   if (debug)
      j_err(j_write_pretty(j, stderr));
}


// Main connection handling
char *job(const char *from)
{                               // This handles a connection from client, and connects to printer to perform operations for a job
   const char *er = NULL;
   // Connect to printer, get answer back, report to client
   er = printer_connect();
   if (!er)
      er = printer_rx();
   if (er)
      return strdup(er);
   if (buflen < 120 || bufrxcmd != 0xF3000200)
      er = "Unexpected init message";
   if (!er)
   {                            // Send printer info
      j_t j = j_create();
      j_store_string(j, "id", buf + 30);
      j_store_stringf(j, "type", "%.16s", buf + 56);
      client_tx(j);
   }
   // TODO threads for regular status updates?

   // Handle messages both ways
   char *ers = NULL;
   if (!er)
      ers = j_stream(client, client_rx);
   if (posn)
   {                            // Reject card if not done
      // TODO
   }
   if ((er = printer_disconnect()) && !ers)
      ers = strdup(er);
   return ers;
}

// Main server code
int main(int argc, const char *argv[])
{
   const char *bindhost = NULL;
   const char *port = "7810";
   int background = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "host", 'h', POPT_ARG_STRING, &bindhost, 0, "Host to bind", "Host/IP" },
         { "port", 'p', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &port, 0, "Port to bind", "port" },
         { "printer", 'H', POPT_ARG_STRING, &printhost, 0, "Printer", "Host/IP" },
         { "print-port", 'P', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &printport, 0, "Printer port", "port" },
         { "daemon", 'd', POPT_ARG_NONE, &background, 0, "Background" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
         POPT_AUTOHELP { }
      };

      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");

      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));

      if (poptPeekArg(optCon) || !printhost)
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      poptFreeContext(optCon);
   }
   if (background)
      daemon(0, debug);

   // Bind for connection
   int l = -1;
 const struct addrinfo hints = { ai_flags: AI_PASSIVE, ai_socktype: SOCK_STREAM, ai_family:AF_UNSPEC };
   struct addrinfo *res = NULL,
       *r;
   if (getaddrinfo(bindhost, port, &hints, &res))
      err(1, "Failed to get address info");
   if (!res)
      err(1, "Cannot find port");
   const char *err = NULL;
   for (r = res; r && r->ai_family != AF_INET6; r = r->ai_next);
   if (!r)
      r = res;
   for (; r; r = r->ai_next)
   {
      l = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
      if (l < 0)
      {
         err = "Cannot create socket";
         continue;
      }
      int on = 1;
      if (setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
      {
         close(l);
         err = "Failed to set socket option (REUSE)";
         continue;
      }
      if (bind(l, r->ai_addr, r->ai_addrlen))
      {
         close(l);
         err = "Failed to bind to address";
         continue;
      }
      if (listen(l, 10))
      {
         close(l);
         err = "Could not listen on port";
         continue;
      }
      // Worked
      err = NULL;
      break;
   }
   freeaddrinfo(res);
   if (err)
      errx(1, "Failed: %s", err);

   // Handle connections
   while (1)
   {
      struct sockaddr_in6 addr = { 0 };
      socklen_t len = sizeof(addr);
      int s = accept(l, (void *) &addr, &len);
      if (s < 0)
      {
         warn("Bad accept");
         continue;
      }
      char from[INET6_ADDRSTRLEN + 1] = "";
      if (addr.sin6_family == AF_INET)
         inet_ntop(addr.sin6_family, &((struct sockaddr_in *) &addr)->sin_addr, from, sizeof(from));
      else
         inet_ntop(addr.sin6_family, &addr.sin6_addr, from, sizeof(from));
      if (!strncmp(from, "::ffff:", 7) && strchr(from, '.'))
         memmove(from, from + 7, strlen(from + 7) + 1);
      if (debug)
         warnx("Connect from %s", from);
      char *er = NULL;
      client = fdopen(s, "r+"); // Stream to client
      if (!client)
         er = strdup("Open failed");
      if (!er)
         er = job(from);
      if (debug)
         warnx("Finished %s: %s", from, er ? : "OK");
      if (er)
      {
         j_t j = j_create();
         j_store_string(j, "error", er);
         client_tx(j);
         free(er);
      }
      close(s);
      fclose(client);
   }

   return 0;
}
