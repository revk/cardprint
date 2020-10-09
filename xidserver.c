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

// TODO SSL maybe
// TODO USB printing maybe

int debug = 0;                  // Top level debug

// Main connection handling
char *job(int s, const char *from)
{                               // This handles a connection from client, and connects to printer to perform operations for a job
   FILE *f = fdopen(s, "rw"); // Stream to client
   int p=-1;	// Socket connection to printer
   int posn=0;	// Last card position
   int bufmax=0; // Max malloc for buffer
   unsigned char *buf=NULL;

   int prx(void)
   { // (raw) Receive from printer

   }
   int ptx(unsigned int cmd,unsigned int param)
   { // (raw) Send to printer

   }
   int cmd(unsigned char cmd)
   { // Send command to printer, get response, check error

   }

   void jout(j_t j) {           // Send data to client (deletes)
      if (debug)
         j_err(j_write_pretty(j, stderr));
      j_err(j_write(j, f));
      fflush(f);
      j_delete(&j);
   }
   char *jin(j_t j) {           // Received command from client
      if (debug)
         j_err(j_write_pretty(j, stderr));
      // Process command
      // TODO
   }
   // Connect to printer, get answer back, report to client
   // TODO

   // TODO threads for regular status updates?

   // Handle messages both ways
   char *er = j_stream(f, jin);
    if(posn)
    { // Reject card if not done
	    // TODO
    }
    close(p); // Done
    if(buf)free(buf);
   return er;
}

// Main server code
int main(int argc, const char *argv[])
{
   const char *host = NULL;
   const char *port = "7810";
   int background = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "host", 'h', POPT_ARG_STRING, &host, 0, "Host to bind" },
         { "port", 'p', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &port, 0, "Port to bind" },
         { "daemon", 'd', POPT_ARG_NONE, &background, 0, "Background" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
         POPT_AUTOHELP { }
      };

      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");

      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));

      if (poptPeekArg(optCon))
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
   if (getaddrinfo(host, port, &hints, &res))
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
      char *er = job(s, from);
      if (debug)
         warnx("Finished %s: %s", from, er ? : "OK");
      if (er)
         free(er);
      close(s);
   }

   return 0;
}
