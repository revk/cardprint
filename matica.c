// Matica printing app
// This talks to Matica printer (tested on XID9300)
// Main use is to send print images to card, works on a single image file
// - the file contains a sequence of colour panels (YMCKUYMCKU)
// - each panel is 664 rows of 1036 columns of bytes 00=non printed FF=full print
// Can also mag encode at the same time
// Can also place on contact/contactless  station for separate action as needed to interact with contact reader
// - use --retain, run separate app for communications, then use --loaded to continue or --reject as needed

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <err.h>
#include <axl.h>
#include <execinfo.h>

int debug = 0;

int main(int argc, const char *argv[])
{
   int c;
   const char *printer = NULL;
   const char *portname = "9100";
   const char *imagefile = NULL;
   const char *jsstatus = NULL;
   const char *mag1 = NULL;
   const char *mag2 = NULL;
   const char *mag3 = NULL;
   const char *showstatus = NULL;
   int loaded = 0;
   int retain = 0;
   int reject = 0;
   int contact = 0;
   int contactless = 0;
   int copies = 1;
   int prints = 0;
   int magread = 0;
   int uvsingle = 0;

   poptContext optCon;          // context for parsing command-line options
   const struct poptOption optionsTable[] = {
      { "printer", 'P', POPT_ARG_STRING, &printer, 0, "Printer", "IP/hostname" },
      { "port", 'p', POPT_ARG_STRING, &portname, 0, "Port", "number/name" },
      { "image", 'i', POPT_ARG_STRING, &imagefile, 0, "Image file (YMCKUYMCKU panels of 664 rows of 1036 columns)", "filename" },
      { "mag-read", 0, POPT_ARG_NONE, &magread, 0, "Read tracks" },
      { "mag1", '1', POPT_ARG_STRING, &mag1, 0, "ISO Mag track 1", "data" },
      { "mag2", '2', POPT_ARG_STRING, &mag2, 0, "ISO Mag track 2", "data" },
      { "mag3", '3', POPT_ARG_STRING, &mag3, 0, "ISO Mag track 3", "data" },
      { "loaded", 'L', POPT_ARG_NONE, &loaded, 0, "Expect card to be loaded" },
      { "retain", 'K', POPT_ARG_NONE, &retain, 0, "Retain card" },
      { "reject", 'R', POPT_ARG_NONE, &reject, 0, "Reject card" },
      { "uv-single", 0, POPT_ARG_NONE, &uvsingle, 0, "UV on same retransfer" },
      { "contact", 'C', POPT_ARG_NONE, &contact, 0, "Place card on contact station" },
      { "contactless", 'L', POPT_ARG_NONE, &contactless, 0, "Place card on contactless station" },
      { "copies", 'N', POPT_ARGFLAG_SHOW_DEFAULT | POPT_ARG_INT, &copies, 0, "Copies", "N" },
      { "js-status", 'j', POPT_ARG_STRING, &jsstatus, 0, "Javascript output", "html-ID" },
      { "status", 0, POPT_ARGFLAG_DOC_HIDDEN | POPT_ARG_STRING, &showstatus, 0, "Show status", "Message" },
      { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
      POPT_AUTOHELP { }
   };

   optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
   //poptSetOtherOptionHelp (optCon, "");

   if ((c = poptGetNextOpt(optCon)) < -1)
      errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));

   if (poptPeekArg(optCon))
   {
      poptPrintUsage(optCon, stderr, 0);
      return -1;
   }

   int s = -1;                  // socket

   void status(const char *fmt, ...) {  // Report status
      char *e = NULL;
      va_list ap;
      va_start(ap, fmt);
      if (vasprintf(&e, fmt, ap) < 0)
         errx(1, "malloc");
      va_end(ap);
      if (debug)
      {
         if (prints && copies > 1)
            fprintf(stderr, "%d/%d: ", prints, copies);
         fprintf(stderr, "%s\n", e);
      }
      if (jsstatus)
      {
         printf("<script>document.getElementById('%s').innerHTML='", jsstatus);
         if (prints && copies > 1)
            printf("%d/%d: ", prints, copies);
         printf("%s';</script>\n", e);
         fflush(stdout);
      }
      free(e);
   }

   void fail(const char *fmt, ...) {    // Report status, and exit
      char *e = NULL;
      va_list ap;
      va_start(ap, fmt);
      if (vasprintf(&e, fmt, ap) < 0)
         errx(1, "malloc");
      va_end(ap);
      status("%s", e);
      fflush(stdout);
      if (s >= 0)
         close(s);
      exit(1);
   }
   unsigned char buf[700000];
   int seq = 0;
   int rxblock(void) {          // Read a block from socket (s) and return len. Puts in buf
      int l = recv(s, buf, 8, 0);       // First two words contain a code and number of words that follow.
      if (l < 8)
         errx(1, "Bad read from printer (len=%d)", l);
      // Find number of words
      int n = (buf[4] << 24) + (buf[5] << 16) + (buf[6] << 8) + buf[7];
      if (n + 8 > sizeof(buf) - 1)
         errx(1, "Read too long from printer (len=%d)", l);
      // Read rest of message
      l = recv(s, buf + 8, n * 4, 0);
      if (l < n * 4)
         errx(1, "Bad read from printer (len=%d)", l);
      l += 8;
      if (debug)
      {
         fprintf(stderr, "Rx");
         for (n = 0; n < l; n++)
            fprintf(stderr, " %02X", buf[n]);
         fprintf(stderr, "\n");
      }
      buf[l] = 0;               // Just in case
      // Sanity check
      if ((buf[0] & 0xF0) != 0xF0)
         errx(1, "Unexpected packet");
      return l;
   }
   int txblock(unsigned int code, unsigned int len, unsigned int param) {
      buf[0] = (code >> 24);
      buf[1] = (code >> 16);
      buf[2] = (code >> 8);
      buf[3] = code;
      len = (len + 3) / 4 - 2;
      buf[4] = (len >> 24);
      buf[5] = (len >> 16);
      buf[6] = (len >> 8);
      buf[7] = len;
      buf[8] = (param >> 24);
      buf[9] = (param >> 16);
      buf[10] = (param >> 8);
      buf[11] = param;
      buf[12] = (seq >> 24);
      buf[13] = (seq >> 16);
      buf[14] = (seq >> 8);
      buf[15] = seq;
      seq++;
      int l = send(s, buf, 8 + len * 4, 0);
      if (l < 8 + len * 4)
         errx(1, "Bad tx to printer");
      if (debug)
      {
         int n;
         fprintf(stderr, "Tx");
         for (n = 0; n < l && n < 40; n++)
            fprintf(stderr, " %02X", buf[n]);
         if (n < l)
            fprintf(stderr, "...");
         fprintf(stderr, "\n");
      }
      return rxblock();
   }
   void checkerror(void) {
      unsigned int e = (buf[8] << 24) + (buf[9] << 16) + (buf[10] << 8) + buf[11];
      if (!e)
         return;
      buf[16] = 0x05;           // Reject card
      buf[17] = 0x02;
      buf[18] = 0x00;
      buf[19] = 0x04;
      txblock(0xF0000100, 18 + buf[17], 0);
      fail("Error: %08X", e);
   }
   void cmd(unsigned c) {
      buf[16] = (c >> 24);
      buf[17] = (c >> 16);
      buf[18] = (c >> 8);
      buf[19] = c;
      txblock(0xF0000100, 18 + buf[17], 0);
      checkerror();
   }
   unsigned softcmd(unsigned c) {
      buf[16] = (c >> 24);
      buf[17] = (c >> 16);
      buf[18] = (c >> 8);
      buf[19] = c;
      txblock(0xF0000100, 18 + buf[17], 0);
      return (buf[8] << 24) + (buf[9] << 16) + (buf[10] << 8) + buf[11];
   }

#define sides 2
#define panels 5
#define rows 664
#define cols 1036
   unsigned char colour[sides * panels * rows * cols];

   if (imagefile)
   {                            // Load image file
      // Image file expected to be panels in YMCKUYMCKU order, each 644 rows of 1036 columns
      memset(colour, 0, sizeof(colour));
      FILE *i = stdin;
      if (strcmp(imagefile, "-") && !(i = fopen(imagefile, "r")))
         fail("Cannot open file %s", imagefile);
      {
         size_t l = fread(colour, 1, sizeof(colour), i);
         if (l <= 0)
            fail("Bad file read");      // Fine if short as we have 0 padding which is non printed
      }
      fclose(i);
   }

   if (jsstatus)
   {                            // Browsers are funny, needs some data to start showing anything
      int n = 25;
      while (n--)
         status("Connecting to %s", printer);
   }

   if (printer)
   {                            // connect to printer
      struct addrinfo base = {
         0, PF_UNSPEC, SOCK_STREAM
      };
      struct addrinfo *a = 0,
          *t;
      if (getaddrinfo(printer, portname, &base, &a) || !a)
         fail("Cannot connect to %s", printer);
      for (t = a; t; t = t->ai_next)
      {
         s = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
         if (s >= 0 && connect(s, t->ai_addr, t->ai_addrlen))
         {                      // failed to connect
            perror(t->ai_canonname);
            close(s);
            s = -1;
         }
         if (s >= 0)
            break;
      }
      freeaddrinfo(a);
      if (s < 0)
         fail("Cannot connect to %s", printer);
   }
   status("Connected to %s", printer);
   int l = rxblock();           // The initial printer ID
   if (l < 30 || buf[0] != 0xF3)
      errx(1, "Bad rx initial message");
   if (debug)
      fprintf(stderr, "Printer: %s\nModel: %s\n", buf + 30, buf + 56);
   seq = (buf[12] << 24) + (buf[13] << 16) + (buf[14] << 8) + buf[15];
   // Initial job block, fixed...
   unsigned char job[] = {
      0x0f, 0x0a, 0x9c, 0x88, 0x73, 0x09, 0x09, 0x09, 0x0e, 0x27, 0x00, 0x00, 0x4f, 0x00, 0x57, 0x00, 0x4e, 0x00, 0x45, 0x00,
      0x52,
      0x00, 0x5f, 0x00, 0x54, 0x00, 0x4f, 0x00, 0x44, 0x00, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x78, 0x00, 0x69, 0x00, 0x64, 0x00, 0x2e, 0x00, 0x64, 0x00, 0x6f, 0x00, 0x63, 0x00, 0x75, 0x00, 0x6d,
      0x00, 0x65, 0x00, 0x6e, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
   };
   memmove(buf + 16, job, sizeof(job));
   txblock(0xF2000300, sizeof(job), 0);

   for (prints = 1; prints <= copies; prints++)
   {

      cmd(0x01020000);          // Status check
      cmd(0x02020000);          // Card posn check
      int posn = buf[19];
      if (buf[18])
         posn = 5;

      void move(int newposn) {  // Move to new position
         if (posn == newposn)
            return;             // Nothing to do
         if (posn == 1)
            cmd(0x0A024000);    // Disengage contact station
         if (posn == 4 || posn == 5)
         {
            status("Loading card");
            cmd(0x04028000 + newposn);  // Load
         } else if (newposn >= 0)
         {
            if (newposn == 1)
               status("Contact encoding");
            if (newposn == 2)
               status("Contactless encoding");
            if (newposn == 4)
               status("Rejecting card");
            cmd(0x05020000 + newposn);  // Move
         }
         posn = newposn;
         if (posn == 1)
            cmd(0x0A020000);    // Engage contacts
      }

      if (posn == 4 || posn == 5)
      {                         // Not in printer
         if (loaded)
            fail("Card is not in printer");
      } else
      {                         // In printer
         if (!loaded && !reject)
            fail("Card is in printer already");
      }

      if (mag1 || mag2 || mag3)
      {                         // Mag encode
         move(3);               // Posn for mag encoding
         status("Mag card encoding");
         unsigned char *p = buf + 20;
         int t = 0;
         if (mag1)
         {
            int len = strlen(mag1),
                q;
            if (len > 64)
               fail("Mag track 1 too long");
            *p++ = 0x16;        // Track 1, 6 bit coding
            *p++ = len;
            for (q = 0; q < len; q++)
               *p++ = ((mag1[q] & 0x3F) ^ 0x20);
            t++;
         }
         if (mag2)
         {
            int len = strlen(mag2),
                q;
            if (len > 64)
               fail("Mag track 2 too long");
            *p++ = 0x24;        // Track 2, 4 bit coding
            *p++ = len;
            for (q = 0; q < len; q++)
               *p++ = (mag2[q] & 0xF);
            t++;
         }
         if (mag3)
         {
            int len = strlen(mag3),
                q;
            if (len > 64)
               fail("Mag track 3 too long");
            *p++ = 0x34;        // Track 3, 4 bit coding
            *p++ = len;
            for (q = 0; q < len; q++)
               *p++ = (mag3[q] & 0xF);
            t++;
         }
         cmd(0x09000000 + t + ((p - buf - 18) << 16));
      }
      if (magread)
      {
         move(3);
         status("Reading mag tracks");
         int t;
         unsigned char track[] = { 0x16, 0x24, 0x34 };
         for (t = 0; t < sizeof(track); t++)
         {
            buf[20] = buf[21] = buf[22] = buf[23] = 0;
            buf[19 + t] = track[t];
            if (softcmd(0x08060000 + (t ? 0 : track[t])))
               printf("\n");
            else
            {
               unsigned char *p = buf + 19,
                   *e = buf + 18 + buf[17],
                   t = buf[18];
               while (p + 2 <= e && t--)
               {
                  unsigned char f = *p++;
                  unsigned char l = *p++;
                  if ((f & 0xF) == 6)
                     while (l--)
                        putchar(' ' + (*p++ & 0x3F));
                  else if ((f & 0xF) == 4)
                     while (l--)
                        putchar('0' + (*p++ & 0xF));
                  putchar('\n');
               }
            }
         }
      }

      if (contact)
      {
         move(1);
         status("Contact encoding");
      } else if (contactless)
         move(2);
      else if (reject)
         move(4);
      else if (imagefile || retain)
         move(0);               // Ready to print
      else
      {
         move(4);
         status("Nothing to print");
      }

      if (reject || retain || !imagefile)
         break;
      // Ready to print
      if (posn)
      {
         sleep(5);              // allow reader action
         move(0);               // for printing
      }
      status("Printing");
      unsigned char *col = colour;
      int side;
      int printed = 0;
      for (side = 0; side < sides; side++)
      {
         col = colour + side * panels * rows * cols;
         // Does this side need printing?
         int q;
         for (q = 0; q < panels * rows * cols && !col[q]; q++);
         if (q == panels * rows * cols)
         {
            if (!side)
               printed = 0;
            continue;           // Nothing to print this side
         }
         int ymc = 0;
         if (q < 3 * rows * cols)
            ymc = 1;            // Print as a set
         if (side == 1)
         {
            status("Second side");
            cmd(printed ? 0x07021000 : 0x05021000);     // Retransfer and flip if printed, else just flip
            status("Printing");
         }
         printed = 0;
         int p;
         for (p = 0; p < panels; p++)
         {                      // panels
            for (q = 0; q < rows * cols && !col[q]; q++);
            if ((p < 3 && ymc) || q < rows * cols)
            {                   // Print panel
               int data = rows * cols + 4;
               buf[16] = (p == 4 ? 0x40 : (1 << p));
               buf[17] = buf[18] = buf[19] = 0;
               buf[20] = (data >> 24);
               buf[21] = (data >> 16);
               buf[22] = (data >> 8);
               buf[23] = data;
               data -= 4;
               buf[24] = (data >> 24);
               buf[25] = (data >> 16);
               buf[26] = (data >> 8);
               buf[27] = data;
               memmove(buf + 28, col, data);
               txblock(0xF0000200, data + 12 + 16, 0);
               checkerror();
               printed |= (p == 4 ? 0x40 : (1 << p));
            }
            col += rows * cols;
         }
         if (printed)
         {
            if (uvsingle)
               cmd(0x06020000 + printed);       // UV printed with rest, no special handling
            else
            {                   // UV printed separately
               if (printed & 0x0F)
                  cmd(0x06020000 + (printed & 0x0F));   // Non UV, if any
               if (printed & 0x40)
               {                // UV
                  status("UV");
                  if (printed & 0x0F)
                     cmd(0x07020000);   // first transfer of non UV
                  cmd(0x06020000 + (printed & 0x40));   // UV print
                  printed &= 0x40;
               }
            }
         }
      }
      if (printed)
      {
         status("Transfer");
         cmd(0x07020005);       // Eject, print done
         status("Printed");
      } else
      {
         cmd(0x05020004);       // Reject - nothing was printed
         status("Rejected");
      }
      loaded = 0;
   }
   close(s);
   if (showstatus)
      status("%s", showstatus);
   return 0;
}
