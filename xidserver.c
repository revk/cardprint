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
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <err.h>
#include <ajl.h>

// As this is single threader operation, on job at a time, we are using globals :-)

const char *pos_name[] = { "print", "ic", "rfid", "mag", "reject", "eject" };

#define	POS_UNKNOWN	-2
#define	POS_OUT		-1
#define	POS_PRINT	0
#define	POS_IC		1
#define	POS_RFID	2
#define	POS_MAG		3
#define	POS_REJECT	4
#define	POS_EJECT	5

// Config
int debug = 0;                  // Top level debug
const char *printhost = NULL;   // Printer host/IP
const char *printport = "50730";        // Printer port (this is default for XID8600)
const char *keyfile = NULL;     // SSL
const char *certfile = NULL;

// Current connectionns
SSL *ss;                        // SSL client connection
int psock = -1;                 // Connected printer (ethernet)
unsigned char *buf = NULL;      // Printer message buffer
unsigned int buflen = 0;        // Buffer length
unsigned int bufmax = 0;        // Max buffer space malloc'd
unsigned int txcmd = 0;         // Last tx command
unsigned int rxcmd = 0;         // Last rx command
unsigned int rxerr = 0;         // Last rx error
unsigned int seq = 0;           // Sequence
const char *error = NULL;       // Error happened (stops more processing)
const char *status = NULL;      // Status
int queue = 0;                  // Command queue
int posn = 0;                   // Current card position
int dpi = 0,
    rows = 0,
    cols = 0;                   // Size

// Printer specific settings
unsigned char xid8600 = 0;      // Is an XID8600

const char *client_tx(j_t j);

j_t j_new(void)
{
   j_t j = j_create();
   if (status)
      j_store_string(j, "status", error ? "Error" : status);
   if (posn != POS_UNKNOWN)
      j_store_string(j, "position", posn < 0 || posn >= sizeof(pos_name) / sizeof(*pos_name) ? NULL : pos_name[posn]);
   if (error)
   {
      j_t e = j_store_object(j, "error");
      j_store_string(e, "description", error);
      if (rxerr)
         j_store_stringf(e, "code", "%08X", rxerr);
   }
   return j;
}

const char *printer_connect(void)
{                               // Connect to printer, return error if fail
   posn = POS_UNKNOWN;
   queue = 0;
   error = NULL;
   status = "Connecting";
   // TODO USB?
   seq = 0x99999999;
   txcmd = rxcmd = rxerr = 0;
   if (psock >= 0)
      return error = "Printer already connected";
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
            break;
         }
         close(s);
      }
   }
   freeaddrinfo(res);
   if (psock < 0)
      return error = "Could not connect to printer";
   return NULL;
}

const char *printer_disconnect(void)
{                               // Disconnect from printer
   if (psock < 0)
      return NULL;              // Not connected, that is OK
   close(psock);
   psock = -1;
   return NULL;
}

const char *printer_tx(void)
{                               // Raw printer send
   if (error)
      return error;
   if (psock < 0)
      return "Printer not connected";
   if (buflen < 16)
      return "Bad tx";
   queue++;
   txcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   buflen = (buflen + 3) / 4 * 4;
   unsigned int n = buflen / 4 - 2;
   buf[4] = (n >> 24);
   buf[5] = (n >> 16);
   buf[6] = (n >> 8);
   buf[7] = (n);
   if (debug)
   {
      fprintf(stderr, "Tx%d:", queue);
      int i = 0;
      for (i = 0; i < buflen && i < 200; i++)
         fprintf(stderr, "%s%02X", i && !(i & 31) ? "\n     " : (i & 3) ? "" : " ", buf[i]);
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
         warn("Tx %d", (int) l);
         return "Tx fail";
      }
      n += l;
   }
   return NULL;
}

const char *printer_rx(void)
{                               // raw printer receive
   if (queue)
      queue--;
   if (error)
      return error;
   if (psock < 0)
      return "Printer not connected";
   rxcmd = 0;
   buflen = 0;
   unsigned int n = 8;
   while (buflen < n)
   {
      if (bufmax < n && !(buf = realloc(buf, bufmax = n)))
         errx(1, "malloc");
      ssize_t l = read(psock, buf + buflen, n - buflen);
      if (!l && !buflen)
         return "Printer disconnected link";
      if (l <= 0)
      {
         warn("Rx %d", (int) l);
         return "Rx fail";
      }
      buflen += l;
      if (buflen == 8)
         n = ((buf[4] << 24) + (buf[5] << 16) + (buf[6] << 8) + buf[7]) * 4 + 8;
   }
   if (debug)
   {
      fprintf(stderr, "Rx%d:", queue);
      int i = 0;
      for (i = 0; i < buflen && i < 200; i++)
         fprintf(stderr, "%s%02X", i && !(i & 31) ? "\n     " : (i & 3) ? "" : " ", buf[i]);
      if (i < buflen)
         fprintf(stderr, "... (%d)", buflen);
      fprintf(stderr, "\n");
   }
   if (buflen < 16)
      return "Bad rx length";
   rxcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   rxerr = (buf[8] << 24) + (buf[9] << 16) + (buf[10] << 8) + buf[11];
   return NULL;
}

const char *printer_rx_check(void)
{
   if (error)
      return error;
   printer_rx();
   if (!error && rxerr)
   {
      if (rxerr == 0x0002DB00)
         error = "Initialising, not ready";
      else if (rxerr == 0x0002DA00)
         error = "Warming up, not ready";
      else if (rxerr == 0x0002D100)
         error = "Door open";
      else if (rxerr == 0x0003A100)
         error = "Transfer film missing";
      else if (rxerr == 0x0003B000)
         error = "Colour film missing";
      else if (rxerr == 0x0002D000)
         error = "No cards";
      else if (rxerr == 0x00039000)
         error = "Hopper jam";
      else if (rxerr == 0x00052600)
         error = "Mag read fail";
      else if (rxerr == 0x0003AD00)
         error = "Mag write fail";
      else
         error = "Printer returned error (see code)";
   }
   return error;
}

void printer_start(unsigned int cmd, unsigned int param)
{                               // Start message
   if (bufmax < 16 && !(buf = realloc(buf, bufmax = 16)))
      errx(1, "malloc");
   buf[0] = (cmd >> 24);
   buf[1] = (cmd >> 16);
   buf[2] = (cmd >> 8);
   buf[3] = (cmd);
   buf[8] = (param >> 24);
   buf[9] = (param >> 16);
   buf[10] = (param >> 8);
   buf[11] = (param);
   buf[12] = (seq >> 24);
   buf[13] = (seq >> 16);
   buf[14] = (seq >> 8);
   buf[15] = (seq);
   if (seq == 0x99999999)
      seq = 0;
   else
      seq++;
   buflen = 16;
}

void printer_data(unsigned int len, const unsigned char *data)
{
   if (bufmax < buflen + len && !(buf = realloc(buf, bufmax = buflen + len)))
      errx(1, "malloc");
   if (data)
      memcpy(buf + buflen, data, len);
   else
      memset(buf + buflen, 0, len);
   buflen += len;
}

const char *printer_tx_check(void)
{                               // Send and check reply
   if (error)
      return error;
   printer_tx();
   while (queue)
      printer_rx_check();       // Catch up
   return error;
}

const char *printer_start_cmd(unsigned int cmd)
{
   // TODO work out move and flip logic?
   // First byte if command
   // Second is number of bytes after this byte
   // Commands
   // 1: Check status
   // 2: Check position
   // 4: Load Move (third byte 80 for load, forth is position)
   // 5: Move (forth is position)
   // 6: Transfer first
   // 7: Transfer and flip?
   // 8: Mag read
   // 9: Mag write
   // A: Contacts (0x40 disengage, 0x00 engage)
   if (error)
      return error;
   printer_start(0xF0000100, 0);
   unsigned char c[4] = { cmd >> 24, cmd >> 16, cmd >> 8, cmd };
   printer_data(4, c);
   return NULL;
}

const char *printer_queue_cmd(unsigned int cmd)
{
   printer_start_cmd(cmd);
   return printer_tx();
}

const char *printer_cmd(unsigned int cmd)
{                               // Simple command and response
   printer_start_cmd(cmd);
   return printer_tx_check();
}

const char *check_status(void)
{
   while (queue)
      printer_rx_check();
   return printer_cmd(0x01020000);
}

const char *check_position(void)
{
   while (queue)
      printer_rx_check();
   if (error)
      return error;
   if (!printer_cmd(0x02020000))
   {
      posn = buf[19];
      if (buf[18])
         posn = POS_OUT;
      j_t j = j_new();
      client_tx(j);
   }
   return error;
}

const char *moveto(int newposn)
{
   if (error || posn == newposn)
      return error;             // Nothing to do
   if (posn == POS_IC)
      printer_queue_cmd(0x0A024000);    // Disengage contact station
   if (posn < 0)
   {                            // not in machine
      if (newposn == POS_EJECT)
         error = "Cannot eject card, not loaded";
      else if (newposn == POS_REJECT)
         error = "Cannot reject card, not loaded";
      else
      {
         status = "Load card";
         printer_queue_cmd(0x04028000 + newposn);       // Load
      }
   } else if (newposn >= 0)
   {
      if (newposn == POS_PRINT)
         status = "Printing";
      if (newposn == POS_IC)
         status = "IC encoding";
      if (newposn == POS_RFID)
         status = "RFID encoding";
      if (newposn == POS_REJECT)
         status = "Reject card";
      printer_queue_cmd(0x05020000 + newposn);  // Move
   }
   posn = newposn;
   if (posn == POS_IC)
      printer_queue_cmd(0x0A020000);    // Engage contacts
   if (posn == POS_EJECT || posn == POS_REJECT)
      posn = POS_OUT;           // Out of machine
   return error;
}

ssize_t ss_write_func(void *arg, void *buf, size_t len)
{
   return SSL_write(arg, buf, len);
}

ssize_t ss_read_func(void *arg, void *buf, size_t len)
{
   return SSL_read(arg, buf, len);
}

const char *client_tx(j_t j)
{                               // Send data to client (deletes)
   if (!ss)
      return "No client";
   if (debug)
      j_err(j_write_pretty(j, stderr));
   j_err(j_write_func(j, ss_write_func, ss));   // flushes
   j_delete(&j);
   return NULL;
}

char *client_rx(j_t j)
{                               // Process received message
   if (debug)
   {
      if (j_find(j, "print"))
         warnx("Print rx not dumped");
      else
         j_err(j_write_pretty(j, stderr));
   }
   j_t cmd = NULL;
   if ((cmd = j_find(j, "mag")))
   {
      unsigned char temp[66 * 3];
      int p = 0;
      int c = 0;
      void encode(unsigned char tag, j_t j) {
         const char *v = "";
         int len = 1;
         if (j_isstring(j))
         {
            v = j_val(j);
            len = j_len(j);
         }
         if (len > 64)
            error = "Mag track too long";
         else
         {
            temp[p++] = tag;
            temp[p++] = len;
            if ((tag & 0xF) == 6)
               for (int q = 0; q < len; q++)
                  temp[p++] = ((v[q] & 0x3F) ^ 0x20);
            else
               for (int q = 0; q < len; q++)
                  temp[p++] = (v[q] & 0xF);
            c++;
         }
      }
      if (j_isstring(cmd))
      {
         encode(0x16, NULL);
         encode(0x24, cmd);
         encode(0x34, NULL);
      } else if (j_isarray(cmd))
      {
         encode(0x16, j_index(cmd, 0));
         encode(0x24, j_index(cmd, 1));
         encode(0x34, j_index(cmd, 2));
      }
      if (c)
      {
         status = "Encoding";
         client_tx(j_new());
         moveto(POS_MAG);
         check_position();
         printer_start_cmd(0x09000000 + ((p + 2) << 16) + c);
         printer_data(p, temp);
         printer_tx_check();
         status = "Encoded";
         client_tx(j_new());
      }
      // Read
      status = "Reading";
      client_tx(j_new());
      moveto(POS_MAG);
      check_position();
      status = "Read done";
      {
         unsigned char temp[4] = { 0x24, 0x34 };
         printer_start_cmd(0x08060016);
         printer_data(4, temp);
      }
      printer_tx();
      printer_rx();
      if (!rxerr)
      {
         j_t j = j_new();
         j_t m = j_store_array(j, "mag");
         int p = 20;
         int c = buf[p++];
         while (c--)
         {
            unsigned char tag = buf[p++];
            unsigned char len = buf[p++];
            if ((tag & 0xF) == 6)
               for (int q = 0; q < len; q++)
                  buf[p + q] = (buf[p + q] & 0x3F) + 0x20;
            else
               for (int q = 0; q < len; q++)
                  buf[p + q] = (buf[p + q] & 0xF) + '0';
            j_append_stringn(m, (char *) buf + p, len);
            p += len;
         }
         client_tx(j);
      }
   }
   if ((cmd = j_find(j, "ic")))
   {
      // TODO
   }
   if ((cmd = j_find(j, "rfid")))
   {
      // TODO
   }
   if ((cmd = j_find(j, "print")))
   {
      unsigned char printed = 0;
      unsigned char side = 0;
      moveto(POS_PRINT);        // ready to print
      const char *print_side(j_t panel) {
         if (error)
            return error;
         if (!panel)
            return NULL;
         static const char *panelname[8] = { "Y", "M", "C", "K", "P", "Q", "U", "Z" };  // No idea what the extra panels are, but why not have them...
         unsigned char found = 0;
         unsigned char *data[8] = { };
         for (int p = 0; p < 8; p++)
         {                      // Load panel data
            const char *d = j_get(panel, panelname[p]);
            if (!d)
               continue;
            int l = j_base64d(d, &data[p]);
            if (l != rows * cols)
            {
               error = "Wrong print data size";
               break;
            }
            for (l = 0; l < rows * cols && !data[p][l]; l++);
            if (l == rows * cols)
            {                   // Blank
               free(data[p]);
               data[p] = NULL;
            } else
               found |= (1 << p);
         }
         if (found)
         {
            if (side)
            {
               status = "Second side";
               client_tx(j_new());
               printer_queue_cmd(printed ? 0x07021000 : 0x05021000);    // Retransfer and flip if printed, else just flip
            } else
            {
               status = "First side";
               client_tx(j_new());
            }
            printed = 0;
            for (int p = 0; p < 8; p++)
               if ((p < 3 && (found & 7)) || (found & (1 << p)))
               {                // Send panel
                  printer_start(0xF0000200, 0);
                  unsigned char temp[12] = { };
                  int len = rows * cols + 4;
                  temp[0] = (p == 4 ? 0x40 : (1 << p));
                  temp[4] = (len >> 24);
                  temp[5] = (len >> 16);
                  temp[6] = (len >> 8);
                  temp[7] = (len);
                  len -= 4;
                  temp[8] = (len >> 24);
                  temp[9] = (len >> 16);
                  temp[10] = (len >> 8);
                  temp[11] = (len);
                  printer_data(12, temp);
                  printer_data(rows * cols, data[p]);
                  printer_tx();
                  printed |= (p == 4 ? 0x40 : (1 << p));
               }
            if (printed)
            {
               if (j_test(panel, "uvsingle", 0))
                  printer_cmd(0x06020000 + printed);    // UV printed with rest, no special handling
               else
               {                // UV printed separately
                  if (printed & 0x0F)
                     printer_queue_cmd(0x06020000 + (printed & 0x0F));  // Non UV, if any
                  if (printed & 0x40)
                  {             // UV
                     status = "UV";
                     client_tx(j_new());
                     if (printed & 0x0F)
                        printer_queue_cmd(0x07020000);  // first transfer of non UV
                     printer_queue_cmd(0x06020000 + (printed & 0x40));  // UV print
                     printed &= 0x40;
                  }
               }
            }
         }
         for (int i = 0; i < 8; i++)
            if (data[i])
               free(data[i]);
         side++;
         return error;
      }
      if (j_isobject(cmd))
         print_side(cmd);
      else if (j_isarray(cmd))
      {
         print_side(j_index(cmd, 0));
         print_side(j_index(cmd, 1));
      }
      if (printed)
      {
         status = "Transfer";
         client_tx(j_new());
         printer_cmd(0x07020000 + (posn = POS_EJECT));
         status = "Printed";
      } else
      {
         moveto(POS_EJECT);     // Done anyway
         status = "Unprinted";
      }
   } else if ((cmd = j_find(j, "reject")))
      moveto(POS_REJECT);
   else if ((cmd = j_find(j, "eject")))
      moveto(POS_EJECT);
   check_position();
   client_tx(j_new());
   if (error)
      return strdup(error);
   if (posn < 0)
      return strdup("");        // Done
   return NULL;
}

// Main connection handling
char *job(const char *from)
{                               // This handles a connection from client, and connects to printer to perform operations for a job
   // Connect to printer, get answer back, report to client
   printer_connect();
   printer_rx_check();
   if (!error && (buflen < 72 || rxcmd != 0xF3000200))
      error = "Unexpected init message";
   status = "Connected";
   char id[13] = { };
   char type[17] = { };
   if (!error)
   {                            // Send printer info
      // Note IPv4 at 40
      // Note IPv6 at 72
      strncpy(id, (char *) buf + 30, sizeof(id) - 1);
      strncpy(type, (char *) buf + 56, sizeof(type) - 1);
      int e = strlen(type);
      while (e && type[e - 1] == ' ')
         e--;
      type[e] = 0;
      if (!strcmp(type, "XID8600"))
         xid8600 = 1;
      else if (!strncmp(type, "XID580", 6))
         xid8600 = 0;
      else
         error = "Unknown printer type";
      dpi = (xid8600 ? 600 : 300);
      rows = (xid8600 ? 1328 : 664);
      cols = (xid8600 ? 2072 : 1036);
   }
   // Send response
   if (!error)
   {
      printer_start(0xF2000300, xid8600 ? 2 : 0);
      if (xid8600)
      {
         static const unsigned char reply[] = {
            0x00, 0x00, 0x00, 0x00, 0x78, 0x09, 0x09, 0x0a, 0x38, 0x21, 0x00, 0x00, 0x4f, 0x00, 0x57, 0x00,     //
            0x4e, 0x00, 0x45, 0x00, 0x52, 0x00, 0x5f, 0x00, 0x54, 0x00, 0x4f, 0x00, 0x44, 0x00, 0x4f, 0x00,     //
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00, 0x69, 0x00,     //
            0x64, 0x00, 0x2e, 0x00, 0x64, 0x00, 0x6f, 0x00, 0x63, 0x00, 0x75, 0x00, 0x6d, 0x00, 0x65, 0x00,     //
            0x6e, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     //
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     //
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
         };
         printer_data(sizeof(reply), reply);
      } else
      {
         static const unsigned char reply[] = {
            0x0f, 0x0a, 0x9c, 0x88, 0x73, 0x09, 0x09, 0x09, 0x0e, 0x27, 0x00, 0x00, 0x4f, 0x00, 0x57, 0x00,     //
            0x4e, 0x00, 0x45, 0x00, 0x52, 0x00, 0x5f, 0x00, 0x54, 0x00, 0x4f, 0x00, 0x44, 0x00, 0x4f, 0x00,     //
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00, 0x69, 0x00,     //
            0x64, 0x00, 0x2e, 0x00, 0x64, 0x00, 0x6f, 0x00, 0x63, 0x00, 0x75, 0x00, 0x6d, 0x00, 0x65, 0x00,     //
            0x6e, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     //
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     //
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
         };
         printer_data(sizeof(reply), reply);
      }
      printer_tx_check();
   }
   check_status();
   check_position();
   j_t j = j_new();
   j_store_string(j, "id", id);
   j_store_string(j, "type", type);
   j_store_int(j, "rows", rows);
   j_store_int(j, "cols", cols);
   j_store_int(j, "dpi", dpi);
   client_tx(j);
   // Handle messages both ways
   char *ers = NULL;
   if (!error)
      ers = j_stream_func(ss_read_func, ss, client_rx);
   if (!error && posn >= 0)
   {
      moveto(POS_EJECT);
      printer_rx_check();
   }
   if (!ers && error)
      ers = strdup(error);
   if (error)
   {
      error = NULL;
      check_position();
      moveto(POS_REJECT);
      printer_rx();
   }
   printer_disconnect();
   return ers;
}

// Main server code
int main(int argc, const char *argv[])
{
   signal(SIGPIPE, SIG_IGN);
   const char *bindhost = NULL;
   const char *port = "7810";
   int background = 0;
   int single = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "host", 'h', POPT_ARG_STRING, &bindhost, 0, "Host to bind", "Host/IP" },
         { "port", 'p', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &port, 0, "Port to bind", "port" },
         { "printer", 'H', POPT_ARG_STRING, &printhost, 0, "Printer", "Host/IP" },
         { "print-port", 'P', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &printport, 0, "Printer port", "port" },
         { "key-file", 'k', POPT_ARG_STRING, &keyfile, 0, "SSL key file", "filename" },
         { "cert-file", 'k', POPT_ARG_STRING, &certfile, 0, "SSL cert file", "filename" },
         { "daemon", 'd', POPT_ARG_NONE, &background, 0, "Background" },
         { "single", 0, POPT_ARG_NONE | POPT_ARGFLAG_DOC_HIDDEN, &single, 0, "Single run (for memory debug)" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
         POPT_AUTOHELP { }
      };

      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");

      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));

      if (poptPeekArg(optCon) || !printhost || !keyfile || !certfile)
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

   SSL_library_init();
   SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());  // Negotiates TLS
   if (!ctx)
      errx(1, "Cannot create SSL CTX");
   if (SSL_CTX_use_certificate_chain_file(ctx, certfile) != 1)
      errx(1, "Cannot load cert file");
   if (SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1)
      errx(1, "Cannot load key file");

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
      ss = SSL_new(ctx);
      if (!ss)
      {
         warnx("Cannot create SSL server structure");
         close(s);
         continue;
      }
      if (!SSL_set_fd(ss, s))
      {
         close(s);
         SSL_free(ss);
         ss = NULL;
         warnx("Could not set client SSL fd");
         continue;
      }
      if (SSL_accept(ss) != 1)
      {
         close(s);
         SSL_free(ss);
         ss = NULL;
         warnx("Could not establish SSL client connection");
         continue;
      }

      if (!er)
         er = job(from);
      if (debug)
         warnx("Finished %s: %s", from, er ? : "OK");
      if (er && *er)
      {
         j_t j = j_new();
         client_tx(j);
      }
      if (er)
         free(er);
      SSL_shutdown(ss);
      SSL_free(ss);
      ss = NULL;
      close(s);
      if (single)
         break;                 // debug one run
   }

   return 0;
}
