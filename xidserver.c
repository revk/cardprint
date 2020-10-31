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
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <netdb.h>
#include <winscard.h>
#include <libusb-1.0/libusb.h>
#include <err.h>
#include <ajl.h>
#include <ajlparse.h>
#include <png.h>

#define	POS_UNKNOWN	-2
#define	POS_OUT		-1
#define	POS_PRINT	0
#define	POS_IC		1
#define	POS_RFID	2
#define	POS_MAG		3
#define	POS_REJECT	4
#define	POS_EJECT	5

typedef struct setting_s setting_t;
struct setting_s {
   unsigned char rpos;
   unsigned char wpos;
   unsigned char etag;
   const char *name;
   const char *vals;
   int mul;
};

const setting_t settings[] = {  // Ethernet and USB settings
   { 0x1A, 0x13, 0x14, "card-thickness", "standard//thin" },
   { 0x31, 0x15, 0x16, "buzzer", "true/false" },
   { 0x32, 0x16, 0x18, "hr-power-save", "//////45/60/false" },
   { 0x39, 0x1A, 0x1d, "display-mode", "counter/laminator" },
   { 0x3A, 0x1B, 0x1e, "display-counter", "total/head/free/clean/error" },
   { 0x37, 0x18, 0x1b, "display-contrast", "///0/1/2" },
   { 0xFF, 0xFF, 0x1f, "security-lock", "false/true" }, // not in main USB settings block
   { 0xFF, 0xFF, 0x28, "retry-count", "0/1/2/3" },      // Not in main USB settings block
   { 0xFF, 0xFF, 0x29, "jis-type", "loco/hico" },       // Not in main USB settings block
   { 0xFF, 0xFF, 0x2a, "iso-type", "loco/hico" },       // not in main USB settings block
   { 0x18, 0x11, 0x33, "film-type", "1000/750" },
   { 0x2D, 0x0D, 0x46, "k-level", "//-1/0/1/2/3" },
   { 0x2E, 0x0E, 0x47, "k-mode", "standard/fine" },
   { 0x36, 0x17, 0x48, "ymc-level", "//-1/0/1/2/3" },
   { 0x2F, 0x0F, 0x4a, "uv-level", "//-1/0/1/2/3" },
   { 0x30, 0x10, 0x4b, "po-level", "//-1/0/1/2/3}" },
   { 0x25, 0x04, 0x4c, "transfer-temp", "-2/-1/0/1/2" },
   { 0x26, 0x05, 0x4d, "transfer-speed-front", "/1/0/-1/-2/-3" },
   { 0x27, 0x06, 0x4e, "transfer-speed-back", "/1/0/-1/-2/-3" },
   { 0x28, 0x07, 0x4f, "bend-temp", "-5/-4/-3/-2///////false" },
   { 0x29, 0x08, 0x50, "bend-speed", "-2/-1/0/1/2" },
   { 0x14, 0x0B, 0x55, "mg-peel-mode", "standard/stripe" },
   { 0x1B, 0x0C, 0x56, "standby-mode", "front/back" },
   { 0x0C, 0x12, 0x5c, "hr-control", "false/true" },
   { 0x3C, 0x1C, 0x5d, "transfer-speed-uv-front", "/1/0/-1/-2/-3" },
   { 0x3D, 0x1D, 0x5e, "transfer-speed-uv-back", "/1/0/-1/-2/-3" },
   { 0x3E, 0x1E, 0x5f, "backside-cool", "false/true" },
};

#define SETTINGS (sizeof(settings)/sizeof(*settings))

const char *inktype[] = { "YMCK", "", "", "", "YMCKK", "YMCKU" };

const setting_t info[] = {      // Ethernet info
   { 0xFF, 0xFF, 0x0d, "ink", "YMCK////YMCKK/YMCKU" },
   { 0xFF, 0xFF, 0x0f, "ink-lot-number", NULL },
   { 0xFF, 0xFF, 0x0b, "ink-available", NULL, 2 },
   { 0xFF, 0xFF, 0x0e, "ink-total", NULL },
   { 0xFF, 0xFF, 0x0c, "transfer-available", NULL, 10 },
   { 0xFF, 0xFF, 0x0a, "cards-available", "true//false" },
};

#define INFO (sizeof(info)/sizeof(*info))

static const char *msg(unsigned int e)
{
   if (e == 0x0002DA00)
      return "Warming up, not ready";
   if (e == 0x0002DB00)
      return "Initialising, not ready";
   if (e == 0x0002D000)
      return "No cards";
   if (e == 0x0002C100)
      return "Cam Error";
   if (e == 0x0002C200)
      return "HR Overheat";
   if (e == 0x0002C300)
      return "Power Intrpt";
   if (e == 0x0002D100)
      return "Door open";
   if (e == 0x0002D300)
      return "Busy";
   if (e == 0x0002D400)
      return "Busy printing";
   if (e == 0x0002D800)
      return "Hardware";
   if (e == 0x0002F000)
      return "TR Overheat";
   if (e == 0x0002F100)
      return "TR Heater";
   if (e == 0x0002F200)
      return "TR Thermister";
   if (e == 0x0002F300)
      return "RR Overheat";
   if (e == 0x0002F400)
      return "RR Heater";
   if (e == 0x0002F500)
      return "RR Thermister";
   if (e == 0x0002F600)
      return "Overcool";
   if (e == 0x0002F800)
      return "Head Overheat";
   if (e == 0x00034400)
      return "Hardware";
   if (e == 0x00039000)
      return "Jam(Hopper)";
   if (e == 0x00039100)
      return "Jam(TurnOver)";
   if (e == 0x00039200)
      return "Jam(MG)";
   if (e == 0x00039300)
      return "Jam(Transfer)";
   if (e == 0x00039400)
      return "Jam(Discharge)";
   if (e == 0x00039500)
      return "Jam(Retran.)";
   if (e == 0x0003A100)
      return "Film Search";
   if (e == 0x0003A800)
      return "MG Test Err";
   if (e == 0x0003AB00)
      return "MG Mechanical";
   if (e == 0x0003AC00)
      return "MG Hardware";
   if (e == 0x0003B000)
      return "Ink Error";
   if (e == 0x0003B100)
      return "Ink Search";
   if (e == 0x0003B200)
      return "Ink Run Out";
   if (e == 0x00052000)
      return "Bad cmd";
   if (e == 0x00052600)
      return "Card position error";
   if (e == 0x0003AD00)
      return "Mag write fail";
   if (e == 0x00062800)
      return "Reset";
   return "Printer returned error (see code)";
}


int debug = 0;                  // Top level debug
int dodump = 0;                 // Force all dump
int tcp = -1;                   // Connected printer (TCP)
int udp = -1;                   // Connected printer (UDP)
libusb_device_handle *usb = NULL;       // Connected printer (USB)
const char *readeric = NULL,
    *readerrfid = NULL;
const char *printhost = NULL;   // Printer host/IP
const char *printtcp = "50730"; // Printer port (this is default for XID8600)
const char *printudp = "50731"; // Printer UDP port
const char *printusb = "2166:701d";     // Printer USB device
const char *error = NULL;
const char *status = NULL;      // Status
unsigned int rxerr = 0;         // Last rx error
int posn = 0;                   // Current card position
int dpi = 0,
    rows = 0,
    cols = 0;                   // Size
unsigned char xid8600 = 0;      // Is an XID8600
unsigned char flip = 0;         // Image needs flipping
ajl_t i = NULL,
    o = NULL;                   // Output to client

// General
const char *client_tx(j_t j);
const char *moveto(int newposn);

static void dump(const void *buf, size_t len, const char *tag)
{
   if (!debug)
      return;
   for (int i = 0; i < 16; i++)
      fprintf(stderr, " %X ", i);
   fprintf(stderr, "%s\n", tag);
   int rows = 0,
       i;
   for (int b = 0; b < len; b += 16)
   {
      if (b)
      {
         for (i = b; i < b + 16 && !((unsigned char *) buf)[i - 16] && !((unsigned char *) buf)[i]; i++);
         if (i == b + 16)
            continue;
      }
      for (i = b; i < b + 16; i++)
         if (i >= len)
            fprintf(stderr, "   ");
         else
            fprintf(stderr, "%02X ", ((unsigned char *) buf)[i]);
      for (i = b; i < b + 16; i++)
         if (i >= len)
            fputc(' ', stderr);
         else if (((unsigned char *) buf)[i] >= ' ' && ((unsigned char *) buf)[i] <= 0x7E)
            fputc(((unsigned char *) buf)[i], stderr);
         else
            fputc('.', stderr);
      rows++;
      fprintf(stderr, " %04X", b);
      if (rows > 10 && b + 16 < len)
      {
         fprintf(stderr, " (%d)\n", len);
         break;
      }
      fputc('\n', stderr);
   }
}

// Low level USB functions

typedef struct {
   unsigned char cmd;           // The command, can be followed by up to 8 bytes and 00, total is 6 or 10
   unsigned char p1;            // The command parameters
   unsigned char p2;
   unsigned char p3;
   unsigned char p4;
   unsigned char p5;
   unsigned char p6;
   unsigned char p7;
   unsigned char p8;
   unsigned char cmdlen;        // Total cmd len, seems to be 6 or 10
   void *buf;                   // The buffer for tx or rx of bulk data
   int len;                     // The size of buffer (how much to request if rx)
   int *rxlen;                  // Where to store the rx size
   unsigned char to;            // timeout
   unsigned int nodump:1;       // Don't dump
} usb_txn_t;
#define	usb_txn(...) usb_txn_opts((usb_txn_t){__VA_ARGS__})
const char *usb_txn_opts(usb_txn_t o)
{
   int to = o.to * 1000;
   if (!to)
      to = 1000;
   if (dodump)
      o.nodump = 0;
   if (!usb)
      errx(1, "usb_txn: USB not connected");
   enum libusb_error r;
   if (!o.cmdlen)
      o.cmdlen = ((o.p6 || o.p7 || o.p8 || o.cmd == 0x31 || o.cmd == 0x32) ? 10 : 6);   // default len
   static unsigned int tag = 0;
   ++tag;
   {                            // Send command
      unsigned char cmd[31] = { 'U', 'S', 'B', 'C', tag, tag >> 8, tag >> 16, tag >> 24, o.len, o.len >> 8, o.len >> 16, o.len >> 24, o.rxlen ? 0x80 : 0, 0, o.cmdlen, o.cmd, o.p1, o.p2, o.p3, o.p4, o.p5, o.p6, o.p7, o.p8 };
      int try = 10,
          txsize = 0;
      while (try--)
      {
         if ((r = libusb_bulk_transfer(usb, 2, cmd, 31, &txsize, to)) != LIBUSB_ERROR_PIPE)
            break;
         libusb_clear_halt(usb, 2);
      }
      if (r)
         return error = libusb_strerror(r);
      if (!o.nodump)
         dump(cmd, txsize, "USB CMD");
      if (txsize != sizeof(cmd))
         return error = "USB cmd tx size error";
   }
   if (o.len)
   {                            // Data transfer
      int len = 0;
      if (o.rxlen)
      {
         int try = 10;
         while (try--)
         {
            if ((r = libusb_bulk_transfer(usb, 0x81, o.buf, o.len, &len, to)) != LIBUSB_ERROR_PIPE)
               break;
            libusb_clear_halt(usb, 0x81);
         }
         if (!r)
         {
            if (!o.nodump)
               dump(o.buf, len, "UDP Rx");
            *o.rxlen = len;
         }
      } else
      {
         int try = 10;
         while (try--)
         {
            if ((r = libusb_bulk_transfer(usb, 2, o.buf, o.len, &len, to)) != LIBUSB_ERROR_PIPE)
               break;
            libusb_clear_halt(usb, 2);
         }
         if (!r)
            if (!o.nodump)
               dump(o.buf, len, "UDP Tx");
      }
      if (r && r != LIBUSB_ERROR_PIPE)
         return error = libusb_strerror(r);
   }
   {                            // Get status
      unsigned char status[13] = { };
      int try = 10,
          rxsize = 0;
      while (try--)
      {
         if ((r = libusb_bulk_transfer(usb, 0x81, status, 13, &rxsize, to)) != LIBUSB_ERROR_PIPE)
            break;
         libusb_clear_halt(usb, 0x81);
      }
      if (r)
         return error = libusb_strerror(r);
      if (rxsize != sizeof(status))
         error = "USB status rx size error";
      else if (status[0] != 'U' || status[1] != 'S' || status[2] != 'B' || status[3] != 'S')
         error = "Bad USB status response";
      else if (status[4] + (status[5] << 8) + (status[6] << 16) + (status[7] << 24) != tag)
         error = "Bad USB status tag";
      if (error || !o.nodump)
         dump(status, rxsize, "USB status");
      if (error)
         return error;
      //if (status[8] || status[9] || status[10] || status[11]) return error = "Bad USB status residue";
      if (status[12])
         return "";
   }
   return error;
}

const char *usb_get_status(void)
{                               // Does not wait
   unsigned char rx[20];
   int rxlen = 0;
 usb_txn(nodump:1);
 if (!usb_txn(0x03, p4: 20, len: 20, buf: rx, rxlen: &rxlen, nodump:1))
      rxerr = (rx[2] << 16) + (rx[12] << 8);
   return error;
}

const char *usb_ready(int needcards)
{                               // Wait ready
   int last = 0;
   if (error)
      return error;
   while (!error)
   {
      usb_get_status();
      if (!rxerr || (!needcards && rxerr == 0x0002D000))
         break;
      if (rxerr && rxerr != last)
      {
         if (debug)
            warnx("Status %X: %s", rxerr, msg(rxerr));
         last = rxerr;
         j_t j = j_create();
         j_store_true(j, "wait"); // Add any as not added for No cards otherwise
         client_tx(j);
      }
      usleep(100000);
   }
   if (last)
      client_tx(j_create());
   return error;
}

const char *usb_connect(j_t j)
{
   if (usb)
      return error;             // connected
   posn = POS_UNKNOWN;
   error = NULL;
   status = "Connecting";
   enum libusb_error r;
   if ((r = libusb_init(NULL)))
      return error = libusb_strerror(r);
   int vendor,
    product;
   if (sscanf(printusb, "%X:%X", &vendor, &product) != 2)
      return "USB setting is vendor:product";
   usb = libusb_open_device_with_vid_pid(NULL, vendor, product);
   if (!usb)
      return error;             // Could not connect, let's try ethernet shall we
   if ((r = libusb_set_auto_detach_kernel_driver(usb, 1)))
      return error = libusb_strerror(r);
   if ((r = libusb_claim_interface(usb, 0)))
      return error = libusb_strerror(r);
   // Connected
   j_store_true(j, "usb");
   {                            // Basic info
      unsigned char rx[96];
      int rxlen = 0;
    if (usb_txn(0x12, 0, 0, 0, 96, 0, buf: rx, len: 96, rxlen:&rxlen))
         return error;
      char temp[33];
      strncpy(temp, (char *) rx + 16, 16);
      for (int i = 15; i >= 0 && temp[i] == ' '; i--)
         temp[i] = 0;
      j_store_string(j, "type", temp);
      if (!strcmp(temp, "XID8600"))
         flip = xid8600 = 1;
      else if (!strncmp(temp, "XID580", 6))
         flip = xid8600 = 0;
      else
         return error = "Unknown printer type";
      strncpy(temp, (char *) rx + 8, 8);
      for (int i = 7; i >= 0 && temp[i] == ' '; i--)
         temp[i] = 0;
      j_store_string(j, "manufacturer", temp);
      strncpy(temp, (char *) rx + 24, 8);
      for (int i = 7; i >= 0 && temp[i] == ' '; i--)
         temp[i] = 0;
      j_store_string(j, "serial", temp);
      j_t a = j_store_array(j, "version");
      strncpy(temp, (char *) rx + 58, 13);
      for (int i = 12; i >= 0 && temp[i] == ' '; i--)
         temp[i] = 0;
      j_append_string(a, temp);
      strncpy(temp, (char *) rx + 71, 8);
      for (int i = 7; i >= 0 && temp[i] == ' '; i--)
         temp[i] = 0;
      j_append_string(a, temp);
      strncpy(temp, (char *) rx + 79, 8);
      for (int i = 7; i >= 0 && temp[i] == ' '; i--)
         temp[i] = 0;
      j_append_string(a, temp);
   }
   {                            // Printer info
      unsigned char rx[64];
      int rxlen = 0;
    if (usb_txn(0x1A, 0, 0x68, 0, 64, 0, buf: rx, len: 64, rxlen:&rxlen))
         return error;
      dpi = (rx[8] << 8) + rx[9];
      if (!dpi || (rx[10] << 8) + rx[11] != dpi)
         return error = "DPI mismatch";
      rows = (rx[34] << 8) + rx[35];
      cols = (rx[32] << 8) + rx[33];
      j_store_int(j, "card-rows", (rx[18] << 8) + rx[19]);
      j_store_int(j, "card-cols", (rx[16] << 8) + rx[17]);
   }
   return error;
}

void usb_disconnect(void)
{
   if (!usb)
      return;
   if (debug)
      warnx("Disconnect USB");
   libusb_release_interface(usb, 0);
   libusb_close(usb);
   libusb_exit(NULL);
   usb = NULL;
}

const char *usb_get_settings(j_t j)
{
   unsigned char rx[64];
   int rxlen = 0;
 if (usb_txn(0x1A, 0, 0x68, 0, 64, buf: rx, len: 64, rxlen:&rxlen))
      return error;
   j = j_store_object(j, "settings");
   int n;
   for (int i = 0; i < SETTINGS; i++)
      if ((n = settings[i].rpos) != 0xFF)
      {
         n = rx[n];
         const char *v = settings[i].vals;
         while (n-- && v)
         {
            v = strchr(v, '/');
            if (v)
               v++;
         }
         if (v)
         {
            const char *e = strchr(v, '/');
            if (!e)
               e = v + strlen(v);
            if (e > v)
               j_store_stringn(j, settings[i].name, v, (int) (e - v));
         }
      }
   return error;
}

const char *usb_set_settings(j_t j)
{
   unsigned char rx[64];
   int rxlen = 0;
 if (usb_txn(0x1A, 0, 0x68, 0, 64, buf: rx, len: 64, rxlen:&rxlen))
      return error;
   unsigned char tx[32];
   memset(tx, 0xff, 32);
   tx[0] = 0x28;
   tx[1] = 0x1E;                // Length I expect
   int change = 0;
   for (int i = 0; i < SETTINGS; i++)
      if (settings[i].wpos < 32)
      {
         const char *v = j_get(j, settings[i].name);
         if (!v)
            continue;
         int l = strlen(v);
         if (!l)
            continue;
         const char *s = settings[i].vals;
         int n = 0;
         while (*s)
         {
            if (!strncmp(s, v, l))
               break;
            n++;
            while (*s && *s != '/')
               s++;
            if (*s)
               s++;
         }
         if (!*s)
            error = "Bad setting";
         else if (rx[settings[i].rpos] != n)
         {
            tx[settings[i].wpos] = n;
            change++;
         }
      }
   if (change)
    usb_txn(0x15, 0x10, 0x28, 0, 32, len: 32, buf:tx);
   return error;
}

const char *usb_get_info(j_t j)
{
   unsigned char rx[44];
   int rxlen = 0;
 if (usb_txn(0x1A, 0, 0x63, 0, 44, buf: rx, len: 44, rxlen:&rxlen))
      return error;
   j = j_store_object(j, "info");
   if (rx[6] < sizeof(inktype) / sizeof(*inktype))
      j_store_string(j, "ink", inktype[rx[6]]);
   if (rx[12] < 0xFF)
      j_store_string(j, "ink-lot-number", strndupa((char *) rx + 12, 6));
   j_store_int(j, "ink-total", (rx[8] << 8) + rx[9]);
   return error;
}

const char *usb_get_counters(j_t j)
{
   unsigned char rx[52];
   int rxlen = 0;
 if (usb_txn(0x4D, 0, 0x78, 0, 0, 0, 0, 0, 52, buf: rx, len: 52, rxlen:&rxlen))
      return error;
   j = j_store_object(j, "counters");
   int p = 4;
   if ((rx[p + 0] << 8) + rx[p + 1] == 0 && (rx[p + 2] << 8) + rx[p + 3] == 4)
      j_store_int(j, "total", (rx[p + 4] << 24) + (rx[p + 5] << 16) + (rx[p + 6] << 8) + rx[p + 7]);
   p += 8;
   if ((rx[p + 0] << 8) + rx[p + 1] == 1 && (rx[p + 2] << 8) + rx[p + 3] == 4)
      j_store_int(j, "free", (rx[p + 4] << 24) + (rx[p + 5] << 16) + (rx[p + 6] << 8) + rx[p + 7]);
   p += 8;
   if ((rx[p + 0] << 8) + rx[p + 1] == 2 && (rx[p + 2] << 8) + rx[p + 3] == 4)
      j_store_int(j, "head", (rx[p + 4] << 24) + (rx[p + 5] << 16) + (rx[p + 6] << 8) + rx[p + 7]);
   p += 8;
   if ((rx[p + 0] << 8) + rx[p + 1] == 3 && (rx[p + 2] << 8) + rx[p + 3] == 4)
      j_store_int(j, "clean", (rx[p + 4] << 24) + (rx[p + 5] << 16) + (rx[p + 6] << 8) + rx[p + 7]);
   p += 8;
   if ((rx[p + 0] << 8) + rx[p + 1] == 4 && (rx[p + 2] << 8) + rx[p + 3] == 4)
      j_store_int(j, "error", (rx[p + 4] << 24) + (rx[p + 5] << 16) + (rx[p + 6] << 8) + rx[p + 7]);
   return error;
}

const char *usb_get_position(void)
{
   if (usb_ready(0))
      return error;
   unsigned char rx[8];
   int rxlen = 0;
 if (usb_txn(0x34, buf: rx, len: 8, rxlen: &rxlen, cmdlen:10))
      return error;
   if (rx[0])
      posn = POS_OUT;
   else
      posn = rx[7];
   return error;
}

const char *usb_card_load(unsigned char newposn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   if (usb_ready(1))
      return error;
 if (usb_txn(0x31, 0x01, p2: immediate ? 1 : 0, p4: (flip ? 2 : 0) + (filminit ? 4 : 0), p7: newposn, to:60))
      return error;
   posn = newposn;
   return error;
}

const char *usb_card_move(unsigned char newposn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   if (usb_ready(0))
      return error;
 if (usb_txn(0x31, 0x0B, p2: immediate ? 1 : 0, p4: (flip ? 2 : 0) + (filminit ? 4 : 0), p7: newposn, to:30))
      return error;
   posn = newposn;
   return error;
}

const char *usb_transfer_flip(unsigned char immediate)
{
   if (!usb_ready(0))
    usb_txn(0x31, 0x0A, to:30);
   return error;
}

const char *usb_transfer_eject(unsigned char immediate)
{
   if (!usb_ready(0))
    usb_txn(0x31, 0x09, to:30);
   return error;
}

const char *usb_transfer_return(unsigned char immediate)
{
   if (!usb_ready(0))
    usb_txn(0x31, 0x0D, to:30);
   return error;
}

const char *usb_ic_engage(void)
{
   if (!usb_ready(0))
    usb_txn(0x32, 0x00, to:10);
   return error;
}

const char *usb_ic_disengage(void)
{
   if (!usb_ready(0))
    usb_txn(0x32, 0x01, to:10);
   return error;
}

const char *usb_rfid_engage(void)
{
   if (!usb_ready(0))
    usb_txn(0x32, 0x04, to:10);
   return error;
}

const char *usb_rfid_disengage(void)
{
   if (!usb_ready(0))
    usb_txn(0x32, 0x05, to:10);
   return error;
}

const char *usb_mag_iso_encode(j_t j)
{
   status = "Encoding";
   client_tx(j_create());
   moveto(POS_MAG);
   if (usb_ready(0))
      return error;
   unsigned char temp[76 + 37 + 104 + 6];
   unsigned char tags[3] = { };
   int p = 0;
   int c = 0;
   void encode(unsigned char tag, unsigned char max, j_t j) {
      if (!j_isstring(j) || tag < 0xA0 || tag > 0xCF)
         return;
      const char *v = j_val(j);
      int len = j_len(j);
      if (len > 64)
         error = "Mag track too long";
      else
      {
         tags[(tag >> 4) - 0x0A] = tag;
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
   if (j_isstring(j))
      encode(0xB4, 37, j);
   else if (j_isarray(j))
   {
      encode(0xA6, 76, j_index(j, 0));
      encode(0xB4, 37, j_index(j, 1));
      encode(0xC4, 104, j_index(j, 2));
   }
 usb_txn(0x2D, 0, 0, tags[0], tags[1], tags[2], 0, 0, p, buf: temp, len: p, to:60);
   return error;
}

const char *usb_mag_iso_read(j_t j)
{
   j_array(j);
   j_extend(j, 3);
   status = "Reading";
   client_tx(j_create());
   moveto(POS_MAG);
   if (usb_ready(0))
      return error;
   char rx[100];
   int rxlen = 0;
   void decode(void) {
      int p = 0;
      while (p < rxlen)
      {
         if (p + 2 > rxlen)
            break;
         if (rx[p] < 0xA0 || rx[p] > 0xCF)
            break;
         int track = (rx[p] >> 4) - 0xA;
         int bits = (rx[p] & 0xF);
         int q = p + 2;
         p = q + rx[p + 1];
         if (p > rxlen)
            p = rxlen;
         char temp[256];
         int z = 0;
         if (bits == 7)
            while (q < p)
               temp[z++] = rx[q++];
         else if (bits == 6)
            while (q < p)
               temp[z++] = (rx[q++] & 0x3F) + 0x20;
         else if (bits == 4)
            while (q < p)
               temp[z++] = (rx[q++] & 0xF) + '0';
         j_stringn(j_index(j, track), temp, z);
      }
   }
 if (!usb_txn(0x2C, 0, 0, 0xA6, 0, 0, 76, 0, 0, buf: rx, len: 76, rxlen: &rxlen, to:60))
      decode();
 if (!usb_txn(0x2C, 0, 0, 0, 0xB4, 0, 0, 37, 0, buf: rx, len: 37, rxlen: &rxlen, to:60))
      decode();
 if (!usb_txn(0x2C, 0, 0, 0, 0, 0xC4, 0, 0, 104, buf: rx, len: 104, rxlen: &rxlen, to:60))
      decode();
   // TODO Not coping if one of these txns fails...
   return error;
}

const char *usb_mag_jis_encode(j_t j)
{
   // TODO
   return error;
}

const char *usb_mag_jis_read(j_t j)
{
   // TODO
   return error;
}

const char *usb_send_panel(unsigned char panel, unsigned int len, void *data)
{
   const unsigned char map[] = { 3, 2, 1, 0, 0, 5, 4 };
   if (!usb_ready(0))
    usb_txn(0x2A, 0, map[panel], 0, 0, len >> 24, len >> 16, len >> 8, len, len: len, buf:data);
   return error;
}

const char *usb_print_panels(unsigned char panels, unsigned char immediate, unsigned char buffer)
{
   unsigned char set = (immediate ? 0x01 : 0);
   if (panels & 0x07)
      set |= 0x02;              // YMC
   if (panels & 0x08)
      set |= 0x04;              // K
   if (panels & 0x40)
      set |= 0x08;              // UV
   if (panels & 0x20)
      set |= 0x10;              // PO
   if (!usb_ready(0))
    usb_txn(0x31, 0x08, set, 0, buffer, to:30);
   return error;
}

// Low level ETH functions
void eth_start(unsigned int cmd, unsigned int param);
void eth_data(unsigned int len, const unsigned char *data);
const char *eth_connect_udp(j_t j)
{                               // Set up UDP connect
   if (usb)
      return error;             // USB is connected
   if (udp >= 0)
      return error;
 const struct addrinfo hints = { ai_family: AF_INET, ai_socktype:SOCK_DGRAM };
   int e;
   struct addrinfo *res = NULL,
       *a;
   if ((e = getaddrinfo(printhost, printudp, &hints, &res)))
      errx(1, "getaddrinfo: %s", gai_strerror(e));
   if (!res)
      return "Cannot find printer";
   for (a = res; a; a = a->ai_next)
   {
      int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s >= 0)
      {
         if (!connect(s, a->ai_addr, a->ai_addrlen))
         {
            udp = s;
            break;
         }
         close(s);
      }
   }
   freeaddrinfo(res);
   if (udp < 0)
      return error = "Could not connect to printer (UDP)";
   return error;
}

int queue = 0;                  // Command queue
unsigned int seq = 0;           // Sequence
unsigned int txcmd = 0;         // Last tx command
unsigned int rxcmd = 0;         // Last rx command
SSL *ss;                        // SSL client connection
unsigned char *buf = NULL;      // Printer message buffer
unsigned int buflen = 0;        // Buffer length
unsigned int bufmax = 0;        // Max buffer space malloc'd
const char *eth_rx(void);
const char *eth_tx_check(void);
const char *eth_connect_tcp(j_t j)
{                               // Connect to printer, return error if fail
   if (usb)
      return error;             // USB is connected
   posn = POS_UNKNOWN;
   queue = 0;
   error = NULL;
   status = "Connecting";
   seq = 0x99999999;
   txcmd = rxcmd = rxerr = 0;
   if (tcp >= 0)
      return error = "Printer already connected";
   struct addrinfo base = { 0, PF_UNSPEC, SOCK_STREAM };
   struct addrinfo *res = NULL,
       *a;
   int r = getaddrinfo(printhost, printtcp, &base, &res);
   if (r)
      errx(1, "Cannot get addr info %s", printhost);
   for (a = res; a; a = a->ai_next)
   {
      int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s >= 0)
      {
         if (!connect(s, a->ai_addr, a->ai_addrlen))
         {
            tcp = s;
            break;
         }
         close(s);
      }
   }
   freeaddrinfo(res);
   if (tcp < 0)
      return error = "Could not connect to printer";
   // Connected
   eth_rx();
   if (!error && (buflen < 72 || rxcmd != 0xF3000200))
      error = "Unexpected init message";
   char type[17] = { };
   if (error)
      return error;
   j_store_true(j, "eth");
   // Note IPv4 at 40
   // Note IPv6 at 72
   strncpy(type, (char *) buf + 56, sizeof(type) - 1);
   int e = strlen(type);
   while (e && type[e - 1] == ' ')
      e--;
   type[e] = 0;
   if (!strcmp(type, "XID8600"))
      flip = xid8600 = 1;
   else if (!strncmp(type, "XID580", 6))
      flip = xid8600 = 0;
   else
      error = "Unknown printer type";
   dpi = (xid8600 ? 600 : 300);
   rows = (xid8600 ? 1328 : 664);
   cols = (xid8600 ? 2072 : 1036);
   j_store_string(j, "type", type);
   // Send response
   if (!error)
   {
      eth_start(0xF2000300, xid8600 ? 2 : 0);
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
         eth_data(sizeof(reply), reply);
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
         eth_data(sizeof(reply), reply);
      }
      eth_tx_check();
   }
   return error;
}

const char *eth_disconnect(void)
{                               // Disconnect from printer
   if (tcp < 0)
      return "Not connected";   // Not connected, that is OK
   close(tcp);
   tcp = -1;
   if (udp >= 0)
      close(udp);
   udp = -1;
   return error;
}

const char *eth_tx_udp(void)
{
   if (error)
      return error;
   if (udp < 0)
      return error = "Printer not connected (UDP)";
   if (buflen < 16)
      return "Bad tx";
   txcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   buflen = (buflen + 3) / 4 * 4;
   unsigned int n = buflen / 4 - 2;
   buf[4] = (n >> 24);
   buf[5] = (n >> 16);
   buf[6] = (n >> 8);
   buf[7] = (n);
   dump(buf, buflen, "Tx UDP");
   int l = send(udp, buf, buflen, 0);
   if (l < 0)
      err(1, "Tx fail");
   return error;
}

const char *eth_rx_udp(void)
{
   if (error)
      return error;
   if (udp < 0)
      return error = "Printer not connected (UDP)";
   struct timeval timeout = { 1, 0 };
   fd_set readfs;
   FD_ZERO(&readfs);
   FD_SET(udp, &readfs);
   if (select(udp + 1, &readfs, NULL, NULL, &timeout) <= 0)
      return error;
   if (bufmax < 1024 && !(buf = realloc(buf, bufmax = 1024)))
      errx(1, "malloc");
   buflen = recv(udp, buf, bufmax, 0);
   if (!buflen)
      return error = "No reply from printer";
   if (buflen < 0)
      err(1, "Rx UDP fail");
   int n = ((buf[4] << 24) + (buf[5] << 16) + (buf[6] << 8) + buf[7]) * 4 + 8;
   dump(buf, buflen, "Rx UDP");
   if (buflen < 16 || buflen < n || n < 16)
      return "Bad rx length";
   buflen = n;
   rxcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   rxerr = (buf[8] << 24) + (buf[9] << 16) + (buf[10] << 8) + buf[11];
   return error;
}

const char *eth_tx(void)
{                               // Raw printer send
   if (error)
      return error;
   if (tcp < 0)
      return error = "Printer not connected (tx)";
   if (buflen < 16)
      return error = "Bad tx";
   queue++;
   txcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   buflen = (buflen + 3) / 4 * 4;
   unsigned int n = buflen / 4 - 2;
   buf[4] = (n >> 24);
   buf[5] = (n >> 16);
   buf[6] = (n >> 8);
   buf[7] = (n);
   dump(buf, buflen, "Tx TCP");
   n = 0;
   while (n < buflen)
   {
      int l = 0;
      l = write(tcp, buf + n, buflen - n);
      if (l <= 0)
      {
         warn("Tx %d", (int) l);
         return "Tx fail";
      }
      n += l;
   }
   return error;
}

const char *eth_rx(void)
{                               // raw printer receive
   if (queue)
      queue--;
   if (error)
      return error;
   if (tcp < 0)
      rxcmd = 0;
   buflen = 0;
   unsigned int n = 8;
   while (buflen < n)
   {
      if (bufmax < n && !(buf = realloc(buf, bufmax = n)))
         errx(1, "malloc");
      int l = 0;
      l = read(tcp, buf + buflen, n - buflen);
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
   dump(buf, buflen, "Rx TCP");
   if (buflen < 16)
      return "Bad rx length";
   rxcmd = (buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3];
   rxerr = (buf[8] << 24) + (buf[9] << 16) + (buf[10] << 8) + buf[11];
   return error;
}

const char *eth_get_counters(j_t j)
{                               // Not done for ethernet
   return error;
}

const char *eth_get_settings(j_t j, int req, const char *label, int N, const setting_t * settings)
{
   if (error)
      return error;
   eth_start(0xF0000000 + (req << 8), 0);
   unsigned char data[N * 4];
   for (int i = 0; i < N; i++)
   {
      data[i * 4] = settings[i].etag;
      data[i * 4 + 1] = 0;
      data[i * 4 + 2] = 0;
      data[i * 4 + 3] = 0;
   } eth_data(N * 4, data);
   eth_tx_udp();
   eth_rx_udp();
   if (error)
      return error;
   j_t s = j_store_object(j, label);
   unsigned char *p = buf + 16,
       *e = buf + 8 + 4 * ((buf[4] << 24) + (buf[5] << 16) + (buf[6] << 8) + buf[7]);
   while (p < e && *p)
   {
      if (p[1] > 1)
         for (int i = 0; i < N; i++)
            if (*p == settings[i].etag)
            {                   // Simple number
               if (p[2] <= 4)
               {                // Number
                  long long n = 0;
                  for (int q = 0; q < p[2]; q++)
                     n = (n << 8) + p[3 + q];
                  if (settings[i].mul)
                     n *= settings[i].mul;
                  const char *v = settings[i].vals;
                  if (v)
                  {
                     while (n--)
                     {
                        while (*v && *v != '/')
                           v++;
                        if (*v)
                           v++;
                     }
                     const char *e = v;
                     while (*e && *e != '/')
                        e++;
                     if (e > v)
                     {
                        if (isdigit(*v) || *v == '-')
                           j_store_int(s, settings[i].name, atoi(v));
                        else if (e - v == 4 && !strncmp(v, "true", e - v))
                           j_store_true(s, settings[i].name);
                        else if (e - v == 5 && !strncmp(v, "false", e - v))
                           j_store_false(s, settings[i].name);
                        else
                           j_store_stringn(s, settings[i].name, v, e - v);
                     }
                  } else
                     j_store_int(s, settings[i].name, n);
               } else
                  j_store_string(s, settings[i].name, strndupa((char *) p + 3, p[2]));  // String
               break;           // found
            }
      p += 2 + p[1];
   }
   return error;
}

const char *eth_set_settings(j_t s)
{
   if (error)
      return error;
   unsigned char data[SETTINGS * 4] = { };
   int p = 0;
   for (int i = 0; i < SETTINGS; i++)
   {
      const char *v = j_get(s, settings[i].name);
      if (!v)
         continue;
      int l = strlen(v);
      if (!l)
         continue;
      const char *s = settings[i].vals;
      int n = 0;
      while (*s)
      {
         if (!strncmp(s, v, l))
            break;
         n++;
         while (*s && *s != '/')
            s++;
         if (*s)
            s++;
      }
      if (!*s)
         error = "Bad setting";
      else
      {
         data[p++] = settings[i].etag;
         data[p++] = 2;
         data[p++] = 1;
         data[p++] = n;
      }
   }
   eth_start(0xF0000800, 0);
   eth_data(p, data);
   eth_tx_udp();
   eth_rx_udp();
   return error;
}

const char *eth_start_cmd(unsigned int cmd)
{
   if (error)
      return error;
   eth_start(0xF0000100, 0);
   unsigned char c[4] = { cmd >> 24, cmd >> 16, cmd >> 8, cmd };
   eth_data(4, c);
   return error;
}

const char *eth_rx_check(void)
{
   if (error)
      return error;
   if (!error && ((rxerr >> 16) == 2 || rxerr == 0x00062800) && rxerr != 0x0002D000)
   {                            // Wait
      while (queue && !error)
         eth_rx();
      if (error)
         return error;
      time_t update = 0;
      time_t now = 0;
      int last = 0;
      while (((rxerr >> 16) == 2 || rxerr == 0x00062800) && !error && rxerr != 0x0002D000)
      {
         if (rxerr != last || now > update)
         {
            last = rxerr;
            j_t j = j_create();
            j_store_true(j, "wait");
            client_tx(j);
            update = now;
         }
         usleep(100000);
         eth_start_cmd(0x01020000);
         eth_tx();
         eth_rx();
      }
      if (!rxerr && o)
         client_tx(j_create());
      if (!error && rxerr && rxerr != 0x0002D000)
         error = msg(rxerr);
      return error;
   }
   if (!error && rxerr && rxerr != 0x0002D000)
      error = msg(rxerr);
   else
      eth_rx();
   return error;
}

const char *eth_printer_cmd(unsigned int cmd)
{                               // Simple command and response
   eth_start_cmd(cmd);
   return eth_tx_check();
}

const char *eth_get_position(void)
{
   while (queue && !error)
      eth_rx_check();
   if (error)
      return error;
   if (!eth_printer_cmd(0x02020000))
   {
      if (buf[7] >= 3)
      {
         posn = buf[19];
         if (buf[18])
            posn = POS_OUT;
      }
   }
   return error;
}

const char *eth_printer_queue_cmd(unsigned int cmd)
{
   eth_start_cmd(cmd);
   return eth_tx();
}

const char *eth_card_load(unsigned char newposn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   eth_printer_queue_cmd(0x04020000 + newposn + (flip ? 0x1000 : 0) + (filminit ? 0x8000 : 0) + (immediate ? 1 : 0));   // Load
   posn = newposn;
   return error;
}

const char *eth_card_move(unsigned char newposn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   eth_printer_queue_cmd(0x05020000 + newposn + (flip ? 0x1000 : 0) + (filminit ? 0x8000 : 0) + (immediate ? 1 : 0));   // Load
   posn = newposn;
   return error;
}

const char *eth_transfer_flip(unsigned char immediate)
{
   eth_printer_queue_cmd(0x07021000 + (immediate ? 1 : 0));     // Retransfer and flip
   return error;
}

const char *eth_transfer_eject(unsigned char immediate)
{
   eth_printer_queue_cmd(0x07020005 + (immediate ? 1 : 0));     // Retransfer and eject
   return error;
}

const char *eth_transfer_return(unsigned char immediate)
{
   eth_printer_queue_cmd(0x07020000 + (immediate ? 1 : 0));     // Retransfer and return
   return error;
}

const char *eth_ic_engage(void)
{
   eth_printer_queue_cmd(0x0A020000);
   return error;
}

const char *eth_ic_disengage(void)
{
   eth_printer_queue_cmd(0x0A024000);
   return error;
}

const char *eth_rfid_engage(void)
{
   eth_printer_queue_cmd(0x0A021000);
   return error;
}

const char *eth_rfid_disengage(void)
{
   eth_printer_queue_cmd(0x0A025000);
   return error;
}

const char *eth_mag_iso_encode(j_t j)
{
   unsigned char temp[76 + 37 + 104 + 6];
   int p = 0;
   int c = 0;
   void encode(unsigned char tag, j_t j) {
      if (!j_isstring(j))
         return;
      const char *v = j_val(j);
      int len = j_len(j);
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
   if (j_isstring(j))
      encode(0x24, j);
   else if (j_isarray(j))
   {
      encode(0x16, j_index(j, 0));
      encode(0x24, j_index(j, 1));
      encode(0x34, j_index(j, 2));
   }
   if (c)
   {
      status = "Encoding";
      client_tx(j_create());
      moveto(POS_MAG);
      eth_start_cmd(0x09000000 + ((p + 2) << 16) + c);
      eth_data(p, temp);
      eth_tx_check();
      status = "Encoded";
      j_t j = j_create();
      j_store_boolean(j, "mag", rxerr ? 0 : 1);
      client_tx(j);
   }
   return error;
}

const char *eth_mag_iso_read(j_t j)
{
   status = "Reading";
   client_tx(j_create());
   moveto(POS_MAG);
   // Load tacks separately as loading all at once causes error if any do not read
   void mread(j_t j, unsigned char tag) {
      char t = (tag >> 4) - 1;
      unsigned char temp[4] = { };
      if (t)
         temp[t - 1] = tag;
      eth_start_cmd(0x08060000 + (t ? 0 : 0x16));
      eth_data(4, temp);
      eth_tx();
      eth_rx();
      if (rxerr)
         j_append_null(j);
      else
      {
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
            j_append_stringn(j, (char *) buf + p, len);
            p += len;
         }
      }
   }
   mread(j, 0x16);
   mread(j, 0x24);
   mread(j, 0x34);
   return error;
}

const char *eth_mag_jis_encode(j_t j)
{
   // TODO
   return error;
}

const char *eth_mag_jis_read(j_t j)
{
   // TODO
   return error;
}


const char *eth_get_status(void)
{
   if (error)
      return error;
   while (queue && !error)
      eth_rx_check();
   return eth_printer_cmd(0x01020000);
}

const char *eth_send_panel(unsigned char panel, unsigned int len, void *data)
{
   eth_start(0xF0000200, 0);
   unsigned char temp[12] = { };
   len += 4;
   temp[0] = (1 << panel);
   temp[4] = (len >> 24);
   temp[5] = (len >> 16);
   temp[6] = (len >> 8);
   temp[7] = (len);
   len -= 4;
   temp[8] = (len >> 24);
   temp[9] = (len >> 16);
   temp[10] = (len >> 8);
   temp[11] = (len);
   eth_data(12, temp);
   eth_data(len, data);
   eth_tx();
   while (queue > 3 && !error)
      eth_rx_check();
   return error;
}

const char *eth_print_panels(unsigned char panels, unsigned char immediate, unsigned char buffer)
{
   eth_printer_cmd(0x06020000 + panels);
   return error;
}

// Common commands

const char *get_status(void)
{
   if (usb)
      return usb_get_status();
   return eth_get_status();
}

const char *card_load(unsigned char posn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   if (usb)
      return usb_card_load(posn, immediate, flip, filminit);
   return eth_card_load(posn, immediate, flip, filminit);
}

const char *card_move(unsigned char posn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   if (usb)
      return usb_card_move(posn, immediate, flip, filminit);
   return eth_card_move(posn, immediate, flip, filminit);
}

const char *transfer_flip(unsigned char immediate)
{
   if (usb)
      return usb_transfer_flip(immediate);
   return eth_transfer_flip(immediate);
}

const char *transfer_eject(unsigned char immediate)
{
   if (usb)
      return usb_transfer_eject(immediate);
   return eth_transfer_eject(immediate);
}

const char *transfer_return(unsigned char immediate)
{
   if (usb)
      return usb_transfer_return(immediate);
   return eth_transfer_return(immediate);
}

const char *get_counters(j_t j)
{
   if (usb)
      return usb_get_counters(j);
   return eth_get_counters(j);
}

const char *get_settings(j_t j)
{
   if (usb)
      return usb_get_settings(j);
   return eth_get_settings(j, 10, "settings", SETTINGS, settings);
}

const char *get_info(j_t j)
{
   if (usb)
      return usb_get_info(j);
   return eth_get_settings(j, 6, "info", INFO, info);
}

const char *set_settings(j_t j)
{
   if (usb)
      return usb_set_settings(j);
   return eth_set_settings(j);
}

const char *get_position()
{
   if (usb)
      return usb_get_position();
   return eth_get_position();
}

const char *ic_engage(void)
{
   if (usb)
      return usb_ic_engage();
   return eth_ic_engage();
}

const char *ic_disengage(void)
{
   if (usb)
      return usb_ic_disengage();
   return eth_ic_disengage();
}

const char *rfid_engage(void)
{
   if (usb)
      return usb_rfid_engage();
   return eth_rfid_engage();
}

const char *rfid_disengage(void)
{
   if (usb)
      return usb_rfid_disengage();
   return eth_rfid_disengage();
}

const char *mag_iso_encode(j_t j)
{
   if (usb)
      return usb_mag_iso_encode(j);
   return eth_mag_iso_encode(j);
}

const char *mag_iso_read(j_t j)
{
   if (usb)
      return usb_mag_iso_read(j);
   return eth_mag_iso_read(j);
}

const char *mag_jis_encode(j_t j)
{
   if (usb)
      return usb_mag_jis_encode(j);
   return eth_mag_jis_encode(j);
}

const char *mag_jis_read(j_t j)
{
   if (usb)
      return usb_mag_jis_read(j);
   return eth_mag_jis_read(j);
}

const char *send_panel(unsigned char panel, unsigned int len, void *data)
{
   if (usb)
      return usb_send_panel(panel, len, data);
   return eth_send_panel(panel, len, data);
}

const char *print_panels(unsigned char panels, unsigned char immediate, unsigned char buffer)
{
   if (!panels)
      return error;
   if (usb)
      return usb_print_panels(panels, immediate, buffer);
   return eth_print_panels(panels, immediate, buffer);
}

// ------------------------------------------------------------------------------------------


// Printer commands


// Loads of globals - single job at a time

#define freez(n) do{if(n){free((void*)(n));n=NULL;}}while(0)    // Free in situ if needed and null

const char *pos_name[] = { "print", "ic", "rfid", "mag", "reject", "eject" };

                                        // Config
const char *keyfile = NULL;
const char *certfile = NULL;

// Current connections
int count = 0;                  // Print count

// Cards
SCARDCONTEXT cardctx;
SCARDHANDLE card;
BYTE atr[MAX_ATR_SIZE];
DWORD atrlen;

void card_check(void)
{                               // list the readers
   long res;
   DWORD len;
   freez(readeric);
   freez(readerrfid);
   if ((res = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &cardctx)) != SCARD_S_SUCCESS)
   {
      warnx("Cannot get PCSC context, is pcscd running?");
      return;
   }
   if ((res = SCardListReaders(cardctx, NULL, NULL, &len)) != SCARD_S_SUCCESS)
   {
      warnx("Cannot get reader list (%s)", pcsc_stringify_error(res));
      return;
   }
   char *r = NULL;
   if (!(r = malloc(len)))
      errx(1, "malloc");
   if ((res = SCardListReaders(cardctx, NULL, r, &len)) != SCARD_S_SUCCESS)
   {
      warnx("Cannot list readers (%s)", pcsc_stringify_error(res));
      return;
   }
   char *p = r,
       *e = r + len;
   while (*p && p < e)          // && !error)
   {
      if (!readeric && strstr(p, "HID Global OMNIKEY 3x21 Smart Card Reader"))
         readeric = strdup(p);
      else if (!readeric && strstr(p, "Precise Biometrics Sense MC"))
         readeric = strdup(p);
      else if (!readerrfid && strstr(p, "OMNIKEY AG CardMan 5121"))
         readerrfid = strdup(p);
      else if (debug)
         warnx("Additional card reader %s ignored", p);
      p += strlen(p) + 1;
   }
   free(r);
}

const char *card_connect(const char *reader)
{
   int res;
   if (debug)
      warnx("Connecting to %s", reader);
   if (!reader)
      return "No IC reader found";
   // Yes, we have context, but seems to need it again!
   if ((res = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &cardctx)) != SCARD_S_SUCCESS)
      return "Cannot get PCSC context, is pcscd running?";
   SCARD_READERSTATE status = { };
   status.szReader = reader;
   time_t giveup = time(0) + 10;
   while ((res = SCardGetStatusChange(cardctx, 1000, &status, 1)) == SCARD_S_SUCCESS && (status.dwEventState & SCARD_STATE_EMPTY) && time(0) < giveup);
   if (status.dwEventState & SCARD_STATE_EMPTY)
      return "IC card not responding";
   DWORD proto;
   if ((res = SCardConnect(cardctx, reader, SCARD_SHARE_EXCLUSIVE, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &card, &proto)) != SCARD_S_SUCCESS)
   {
      warnx("Cannot connect to %s (%s)", reader, pcsc_stringify_error(res));
      return "IC card failed to connect";
   }
   atrlen = sizeof(atr);
   DWORD state;
   DWORD temp;
   if ((res = SCardStatus(card, 0, &temp, &state, &proto, atr, &atrlen)) != SCARD_S_SUCCESS)
      return "Cannot get card status";
   j_t j = j_create();
   j_store_string(j, "atr", j_base16(atrlen, atr));
   client_tx(j);
   return error;
}

const char *card_disconnect(void)
{
   if (debug)
      warnx("Card disconnect");
   int res;
   if ((res = SCardDisconnect(card, SCARD_UNPOWER_CARD)) != SCARD_S_SUCCESS)
      return "Cannot end transaction";
   return error;
}

void card_txn(int txlen, const unsigned char *tx, LPDWORD rxlenp, unsigned char *rx)
{
   if (error)
      return;
   SCARD_IO_REQUEST recvpci;
   dump(tx, txlen, "Card Tx");
   int res;
   if ((res = SCardTransmit(card, SCARD_PCI_T0, tx, txlen, &recvpci, rx, rxlenp)) != SCARD_S_SUCCESS)
   {
      warnx("Failed to send command (%s)", pcsc_stringify_error(res));
      *rxlenp = 0;
   }
   dump(rx, *rxlenp, "Card Rx");
   if (*rxlenp < 2)
      warnx("Unexpected response");
   if (rx[0] == 0x93)
      warnx("Busy error %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x62 || rx[0] == 0x63)
      warnx("Warning %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x64 || rx[0] == 0x65)
      warnx("Execution error %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x67 || rx[0] == 0x6C)
      warnx("Wrong length %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x68)
      warnx("Function in CLA not supported %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x69)
      warnx("Command not allowed %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x6A || rx[0] == 0x6B)
      warnx("Wrong parameter %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x6D)
      warnx("Invalid INS %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x6E)
      warnx("Class not supported %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x6F)
      warnx("No diagnosis - error %02X %02X", rx[0], rx[1]);
   if (rx[0] == 0x68)
      warnx("CLA error %02X %02X", rx[0], rx[1]);
}

// Printer specific settings

void eth_start(unsigned int cmd, unsigned int param)
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

void eth_data(unsigned int len, const unsigned char *data)
{
   if (bufmax < buflen + len && !(buf = realloc(buf, bufmax = buflen + len)))
      errx(1, "malloc");
   if (data)
      memcpy(buf + buflen, data, len);
   else
      memset(buf + buflen, 0, len);
   buflen += len;
}

const char *eth_tx_check(void)
{                               // Send and check reply
   if (error)
      return error;
   eth_tx();
   while (queue && !error)
      eth_rx_check();           // Catch up
   return error;
}

const char *moveto(int newposn)
{
   if (error || posn == newposn)
      return error;             // Nothing to do
   if (posn == POS_IC)
   {
      card_disconnect();
      ic_disengage();
   } else if (posn == POS_RFID)
   {
      card_disconnect();
      rfid_disengage();
   }
   if (posn < 0)
   {                            // not in machine
      get_status();
      if (!usb && rxerr == 0x0002D000)
      {                         // Need cards!
         j_t j = j_create();
         j_store_string(j, "status", "No cards");
         j_store_true(j, "wait");
         client_tx(j);
         while (rxerr == 0x0002D000)
         {
            usleep(100000);
            get_status();
         }
      }
      if (newposn == POS_EJECT)
         error = "Cannot eject card, not loaded";
      else if (newposn == POS_REJECT)
         error = "Cannot reject card, not loaded";
      else
         card_load(newposn, 0, 0, 0);
      client_tx(j_create());
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
      card_move(newposn, 0, 0, 0);
   }
   posn = newposn;
   if (posn == POS_IC)
   {
      ic_engage();
      get_status();
      if (!error && (error = card_connect(readeric)))
      {
         error = NULL;
         ic_disengage();
         sleep(1);
         ic_engage();
         sleep(1);
         if (!error && (error = card_connect(readeric)))
            return error;
      }
   } else if (posn == POS_RFID)
   {
      rfid_engage();
      get_status();
      if (!error && (error = card_connect(readerrfid)))
         return error;
   }
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
   if (posn != POS_UNKNOWN)
      j_store_string(j, "position", posn < 0 || posn >= sizeof(pos_name) / sizeof(*pos_name) ? NULL : pos_name[posn]);
   if (error)
   {
      j_store_string(j, "status", "Error");
      j_t e = j_store_object(j, "error");
      j_store_string(e, "description", error);
      if (rxerr)
         j_store_stringf(e, "code", "%08X", rxerr);
   } else if (rxerr)
      j_store_string(j, "status", msg(rxerr));
   else if (status)
      j_store_string(j, "status", status);
   if (rxerr && rxerr != 0x0002D000)
      j_store_true(j, "wait");
   if (count)
      j_store_int(j, "count", count);
   if (debug)
      j_err(j_write_pretty(j, stderr));
   if (o)
      j_err(j_send(j, o));
   j_delete(&j);
   return error;
}

// Main connection handling
char *job(const char *from)
{                               // This handles a connection from client, and connects to printer to perform operations for a job
   count = 0;
   j_t j = j_create();
   // Connect to printer, get answer back, report to client
   card_check();
   j_store_boolean(j, "ic", readeric);
   j_store_boolean(j, "rfid", readerrfid);
   usb_connect(j);
   eth_connect_udp(j);
   eth_connect_tcp(j);
   if (!error && (!rows || !cols || !dpi))
      error = "Bad printer info";
   j_store_int(j, "rows", rows);
   j_store_int(j, "cols", cols);
   j_store_int(j, "dpi", dpi);
   if (!error && tcp < 0 && !usb)
      error = "No printer available";
   if (error)
   {
      if (usb)
         libusb_reset_device(usb);
      return strdup(error);
   }
   j_store_string(j, "status", status = "Connected");
   get_counters(j);
   get_settings(j);
   get_info(j);
   get_status();
   client_tx(j);

   get_position();
   if (posn >= 0)
   {
      if (debug)
         warnx("Unexpected card position %d", posn);
      moveto(POS_REJECT);
      if (debug)
         warnx("Rejected");
   }

   // Handle messages both ways
   char *ers;
   j = j_create();
   while (!(ers = j_recv(j, i)))
   {
      j_t print = j_find(j, "print");
      if (print)
      {
         j_detach(print);
         if (debug)
            warnx("Print command not dumped");
      }
      if (debug)
         j_err(j_write_pretty(j, stderr));
      j_t cmd = NULL;
      if ((cmd = j_find(j, "settings")))
         set_settings(cmd);
      if ((cmd = j_find(j, "jis")))
      {
         j_t j = j_create();
         if (j_isstring(cmd))
         {
            mag_jis_encode(cmd);
            if (!error)
               j_store_true(j, "mag");
         } else if (j_isnull(cmd) || j_istrue(cmd))
            mag_jis_read(j_store_array(j, "mag"));
         client_tx(j);
      }
      if ((cmd = j_find(j, "mag")))
      {
         j_t j = j_create();
         if (j_isarray(cmd) || j_isstring(cmd))
         {
            mag_iso_encode(cmd);
            if (!error)
               j_store_true(j, "mag");
         } else if (j_isnull(cmd) || j_istrue(cmd))
            mag_iso_read(j_store_array(j, "mag"));
         client_tx(j);
      }
      if ((cmd = j_find(j, "ic")))
      {
         moveto(POS_IC);
         if (j_isstring(cmd))
         {
            unsigned char *tx = NULL;
            int txlen = j_base16d(j_val(cmd), &tx);;
            unsigned char rx[256];
            DWORD rxlen = sizeof(rx);
            card_txn(txlen, tx, &rxlen, rx);
            warnx("Txn done len %ld", rxlen);
            j_t j = j_create();
            j_store_string(j, "ic", j_base16(rxlen, rx));
            client_tx(j);
            warnx("ic done");
         }
      }
      if ((cmd = j_find(j, "rfid")))
      {
         moveto(POS_RFID);
         if (j_isstring(cmd))
         {
            unsigned char *tx = NULL;
            int txlen = j_base16d(j_val(cmd), &tx);;
            unsigned char rx[256];
            DWORD rxlen = sizeof(rx);
            card_txn(txlen, tx, &rxlen, rx);
            j_t j = j_create();
            j_store_string(j, "rfid", j_base16(rxlen, rx));
            client_tx(j);
         }
      }
      if ((cmd = j_find(j, "mifare")))
      {
         moveto(POS_RFID);
         // TODO
      }
      if (print)
      {
         if (j_istrue(print) || j_isnull(print))
            moveto(POS_PRINT);  // ready to print
         else
         {
            unsigned char printed = 0;
            unsigned char side = 0;
            const char *print_side(j_t panel) {
               if (error)
                  return error;
               if (!panel)
                  return error;
               unsigned char found = 0;
               unsigned char *data[8] = { };
               const char *add(const char *tag1, const char *tag2, int layer) {
                  if (error)
                     return error;
                  const char *d = j_get(panel, tag1);
                  if (!d)
                     d = j_get(panel, tag2);
                  if (!d)
                     return error;
                  if (strncasecmp(d, "data:image/png;base64,", 22))
                     return error = "Image data must be png base64";
                  unsigned char *png = NULL;
                  int l = j_base64d(d + 22, &png);
                  FILE *f = fmemopen(png, l, "rb");
                  const char *process(void) {
                     if (png_sig_cmp(png, 0, l))
                        return error = "Not PNG";
                     png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
                     if (!png_ptr)
                        return error = "Bad PNG init";
                     png_infop info_ptr = png_create_info_struct(png_ptr);
                     png_infop end_info = png_create_info_struct(png_ptr);
                     if (!info_ptr || !end_info)
                     {
                        png_destroy_read_struct(&png_ptr, NULL, NULL);
                        return error = "Bad PNG init";
                     }
                     png_init_io(png_ptr, f);
                     png_read_info(png_ptr, info_ptr);
                     unsigned int width,
                      height;
                     int bit_depth,
                      color_type,
                      interlace_type,
                      compression_type,
                      filter_type;
                     png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, &compression_type, &filter_type);
                     int dx = (cols - (int) width) / 2,
                         dy = (rows - (int) height) / 2;
                     if (debug)
                        warnx("PNG %s%d:%ux%u (%+d/%+d) card %d/%d", tag1, side, width, height, dx, dy, cols, rows);
                     png_set_expand(png_ptr);   // Expand palette, etc
                     static const png_color_16 bg = { 255, 65535, 65535, 65535, 65535 };
                     png_set_background(png_ptr, &bg, PNG_BACKGROUND_GAMMA_FILE, 0, 1.0);
                     png_set_strip_16(png_ptr); // Reduce to 8 bit
                     png_set_packing(png_ptr);  // Unpack
                     if (layer)
                        png_set_rgb_to_gray(png_ptr, 1, 54.0 / 256, 183.0 / 256);
                     else
                        png_set_gray_to_rgb(png_ptr);   // RGB
                     png_set_strip_alpha(png_ptr);
                     png_set_interlace_handling(png_ptr);
                     // Gamma adjust
                     const char *v = j_get(panel, "@gamma");
                     if (v)
                     {
                        double screen_gamma = strtod(v, NULL);
                        if (screen_gamma)
                        {
                           double gamma = 0;
                           if (png_get_gAMA(png_ptr, info_ptr, &gamma))
                              png_set_gamma(png_ptr, screen_gamma, gamma);
                           else
                              png_set_gamma(png_ptr, screen_gamma, 1);
                        }
                     }
                     png_read_update_info(png_ptr, info_ptr);
                     if (!layer)
                     {          // CMY
                        png_bytep image = malloc(4 * width);
                        for (int layer = 0; layer < 3; layer++)
                        {
                           data[layer] = malloc(rows * cols);
                           memset(data[layer], 0, rows * cols);
                        }
                        for (int r = 0; r < height; r++)
                        {
                           int y = r + dy;
                           png_read_row(png_ptr, image, NULL);
                           if (y >= 0 && y < rows)
                              for (int c = 0; c < width; c++)
                              {
                                 int x = c + dx;
                                 if (x >= 0 && x < cols)
                                 {
                                    int o = (flip ? ((rows - 1 - y) * cols + (cols - 1 - x)) : (y * cols + x));
                                    png_bytep p = image + 3 * c;
                                    data[2][o] = *p++ ^ 0xFF;
                                    data[1][o] = *p++ ^ 0xFF;
                                    data[0][o] = *p++ ^ 0xFF;
                                 }
                              }
                        }
                        for (int layer = 0; layer < 3; layer++)
                        {
                           int z;
                           for (z = 0; z < rows * cols && !data[layer][z]; z++);
                           if (z == rows * cols)
                           {    // Blank
                              free(data[layer]);
                              data[layer] = NULL;
                           } else
                              found |= (1 << layer);
                        }
                        free(image);
                     } else
                     {          // K or U
                        png_bytep image = malloc(width);
                        data[layer] = malloc(rows * cols);
                        memset(data[layer], 0, rows * cols);
                        for (int r = 0; r < height; r++)
                        {
                           int y = r + dy;
                           png_read_row(png_ptr, image, NULL);
                           if (y >= 0 && y < rows)
                              for (int c = 0; c < width; c++)
                              {
                                 int x = c + dx;
                                 if (x >= 0 && x < cols)
                                 {
                                    int o = (flip ? ((rows - 1 - y) * cols + (cols - 1 - x)) : (y * cols + x));
                                    if (layer == 3)
                                       data[layer][o] = ((image[c] & 0x80) ? 0 : 0xFF); // Black
                                    else
                                       data[layer][o] = image[c] ^ 0xFF;
                                 }
                              }
                        }
                        int z;
                        for (z = 0; z < rows * cols && !data[layer][z]; z++);
                        if (z == rows * cols)
                        {       // Blank
                           free(data[layer]);
                           data[layer] = NULL;
                        } else
                           found |= (1 << layer);
                        free(image);
                     }
                     png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
                     return error;
                  }
                  process();
                  fclose(f);
                  free(png);
                  return error;
               }
               add("C", "CMY", 0);
               add("K", "K", 3);
               add("P", "PO", 5);
               add("U", "UV", 6);
               if (found)
               {
                  moveto(POS_PRINT);    // ready to print
                  if (side)
                  {
                     status = "Second side";
                     client_tx(j_create());
                     if (printed)
                        transfer_flip(0);
                     else
                        card_move(POS_PRINT, 0, 1, 0);  // Flip no transfer
                  } else
                  {
                     status = "First side";
                     client_tx(j_create());
                  }
                  printed = 0;
                  for (int p = 0; p < 8; p++)
                     if ((p < 3 && (found & 7)) || (found & (1 << p)))
                     {          // Send panel
                        send_panel(p, rows * cols, data[p]);
                        printed |= (1 << p);
                     }
                  if (printed)
                  {
                     if (j_test(panel, "uvsingle", 0))
                        print_panels(printed, 0, 0);    // All in one
                     else
                     {          // UV printed separately
                        if (printed & 0x0F)
                           print_panels(printed & 0x0F, 0, 0);  // Non UV
                        if (printed & 0x40)
                        {       // UV
                           if (printed & 0x0F)
                           {
                              status = "Printing";
                              client_tx(j_create());
                              transfer_return(0);
                           }
                           status = "UV";
                           client_tx(j_create());
                           print_panels(printed & 0x40, 0, 0);  // UV print
                        }
                     }
                  }
                  get_status();
               }
               for (int i = 0; i < 8; i++)
                  if (data[i])
                     free(data[i]);
               side++;
               return error;
            }
            if (j_isobject(print))
               print_side(print);
            else if (j_isarray(print))
            {
               print_side(j_index(print, 0));
               print_side(j_index(print, 1));
            }
            if (printed)
               count++;
            while (queue && !error)
               eth_rx_check();
            if (printed)
            {
               status = "Transfer";
               client_tx(j_create());
               transfer_eject(0);
               status = "Printed";
            } else
            {
               moveto(POS_EJECT);       // Done anyway
               status = "Unprinted";
            }
            get_status();
            get_position();
            client_tx(j_create());
            break;
         }
      } else if ((cmd = j_find(j, "reject")))
      {
         if (posn >= 0)
            moveto(POS_REJECT);
         get_status();
         get_position();
         client_tx(j_create());
         break;
      } else if ((cmd = j_find(j, "eject")))
      {
         moveto(POS_EJECT);
         get_status();
         get_position();
         client_tx(j_create());
         break;
      }
      get_position();
      client_tx(j_create());
   }

   j_delete(&j);
   if (!ers && error)
      ers = strdup(error);
   if (ers)
   {
      error = NULL;
      if (posn >= 0)
      {
         moveto(POS_REJECT);
         sleep(5);
      }
      if (usb)
      {
         if (debug)
            warnx("Resetting USB");
         libusb_reset_device(usb);
      }
   }
   eth_disconnect();
   usb_disconnect();
   return ers;
}

// Main server code
int main(int argc, const char *argv[])
{
   signal(SIGPIPE, SIG_IGN);
   const char *bindhost = NULL;
   const char *port = "7810";
   int background = 0;
   int lqueue = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "host", 'h', POPT_ARG_STRING, &bindhost, 0, "Host to bind", "Host/IP" },
         { "port", 'p', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &port, 0, "Port to bind", "port" },
         { "printer", 'H', POPT_ARG_STRING, &printhost, 0, "Printer", "Host/IP" },
         { "print-port", 'P', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &printtcp, 0, "Printer port (TC)", "port" },
         { "print-udp", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &printudp, 0, "Printer port (UDP)", "port" },
         { "print-usb", 'U', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &printusb, 0, "Printer port (USB)", "XXXX:XXXX" },
         { "key-file", 'k', POPT_ARG_STRING, &keyfile, 0, "SSL key file", "filename" },
         { "cert-file", 'k', POPT_ARG_STRING, &certfile, 0, "SSL cert file", "filename" },
         { "listen", 'q', POPT_ARG_INT, &lqueue, 0, "Listen queue", "N" },
         { "daemon", 'd', POPT_ARG_NONE, &background, 0, "Background" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
         { "dump", 0, POPT_ARG_NONE, &dodump, 0, "Extra dumping" },
         POPT_AUTOHELP { }
      };
      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");
      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
      if (poptPeekArg(optCon) || !keyfile || !certfile)
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
   const char *er = NULL;
   for (r = res; r && r->ai_family != AF_INET6; r = r->ai_next);
   if (!r)
      r = res;
   for (; r; r = r->ai_next)
   {
      l = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
      if (l < 0)
      {
         er = "Cannot create socket";
         continue;
      }
      int on = 1;
      if (setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
      {
         close(l);
         er = "Failed to set socket option (REUSE)";
         continue;
      }
      if (bind(l, r->ai_addr, r->ai_addrlen))
      {
         close(l);
         er = "Failed to bind to address";
         continue;
      }
      if (listen(l, lqueue))
      {
         close(l);
         er = "Could not listen on port";
         continue;
      }
      // Worked
      er = NULL;
      break;
   }
   freeaddrinfo(res);
   if (er)
      errx(1, "Failed: %s", er);
   SSL_library_init();
   SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());  // Negotiates TLS
   if (!ctx)
      errx(1, "Cannot create SSL CTX");
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
      pid_t pid = fork();
      if (pid < 0)
         err(1, "Forking hell");
      if (!pid)
      {                         // Child (fork to ensure memory leaks never and issue - yeh, cheating) - also handles err and alarm not exiting
         alarm(300);            // Just in case
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
         if (SSL_CTX_use_certificate_chain_file(ctx, certfile) != 1)
            errx(1, "Cannot load cert file");
         if (SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1)
            errx(1, "Cannot load key file");
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
         i = ajl_read_func(ss_read_func, ss);
         o = ajl_write_func(ss_write_func, ss);
         if (!er)
            er = job(from);
         if (debug)
            warnx("Finished %s: %s", from, er ? : "OK");
         if (er)
         {
            j_t j = j_create();
            j_store_string(j, "status", "Error");
            j_t e = j_store_object(j, "error");
            j_store_string(e, "description", er);
            j_err(j_send(j, o));
            free(er);
         }
         ajl_delete(&i);
         ajl_delete(&o);
         SSL_shutdown(ss);
         SSL_free(ss);
         ss = NULL;
         return 0;
      }
      // Parent
      close(s);
      int pstatus = 0;
      waitpid(pid, &pstatus, 0);
      if (!WIFEXITED(pstatus))
         warnx("Job crashed");
      else if (WEXITSTATUS(pstatus))
         warnx("Job failed %d", WEXITSTATUS(pstatus));
   }
   {
      int res;
      if ((res = SCardReleaseContext(cardctx)) != SCARD_S_SUCCESS)
         errx(1, "Cant release context (%s)", pcsc_stringify_error(res));
   }

   return 0;
}
