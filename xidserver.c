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

// TODO things we don't do as yet
// TODO Laminator
// TODO JIS mag encoding, but could easily be added if necessary
// TODO ISO mag Alternative track encoding options - again pretty easy if needed
// TODO Security lock (some sort of challenge / response, needs more debug of windows)

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
   unsigned char rpos;          // Pos in reading blocks
   unsigned char wpos;          // Pos in writing blocks
   unsigned char spos;          // Pos in soft settings write
   const char *name;
   const char *vals;
   int mul;
};

static const setting_t settings[] = {   // Ethernet and USB settings
   { 0x36, 0x17, 0x0A, "ymc-level", "//-1/0/1/2/3" },
   { 0x2D, 0x0D, 0x0B, "k-level", "//-1/0/1/2/3" },
   { 0x2E, 0x0E, 0x12, "k-mode", "standard/fine" },
   { 0x2F, 0x0F, 0x0C, "uv-level", "//-1/0/1/2/3" },
   { 0x30, 0x10, 0x0D, "po-level", "//-1/0/1/2/3}" },
   { 0x25, 0x04, 0x02, "transfer-temp", "-2/-1/0/1/2" },
   { 0x26, 0x05, 0x03, "transfer-speed-front", "/1/0/-1/-2/-3" },
   { 0x3C, 0x1C, 0x13, "transfer-speed-uv-front", "/1/0/-1/-2/-3" },
   { 0x27, 0x06, 0x04, "transfer-speed-back", "/1/0/-1/-2/-3" },
   { 0x3D, 0x1D, 0x14, "transfer-speed-uv-back", "/1/0/-1/-2/-3" },
   { 0x14, 0x0B, 0x08, "mg-peel-mode", "standard/stripe" },
   { 0x1B, 0x0C, 0x09, "standby-mode", "front/back" },
   { 0x3E, 0x1E, 0x15, "backside-cool", "false/true" },
   { 0x28, 0x07, 0x05, "bend-temp", "-5/-4/-3/-2///////false" },
   { 0x29, 0x08, 0x06, "bend-speed", "-2/-1/0/1/2" },
   { 0x46, 0x22, 0x0F, "iso-type", "loco/hico" },
   { 0x47, 0x23, 0x10, "jis-type", "loco/hico" },
   { 0x4B, 0x27, 0x11, "retry-count", "0/1/2/3" },
   { 0xFF, 0xFF, 0x0E, "laminate", "false/true" },
   { 0x1A, 0x13, 0xFF, "card-thickness", "standard//thin" },
   { 0x31, 0x15, 0xFF, "buzzer", "true/false" },
   { 0x32, 0x16, 0xFF, "hr-power-save", "//////45/60/false" },
   { 0x39, 0x1A, 0xFF, "display-mode", "counter/laminator" },
   { 0x3A, 0x1B, 0xFF, "display-counter", "total/head/free/clean/error" },
   { 0x37, 0x18, 0xFF, "display-contrast", "///0/1/2" },
   { 0x18, 0x11, 0xFF, "film-type", "1000/750" },
   { 0x0C, 0x12, 0xFF, "hr-control", "false/true" },
   { 0x3B, 0xFF, 0xFF, "locked", "false/true" },
};

#define SETTINGS (sizeof(settings)/sizeof(*settings))

static const char *inktype[] = { "YMCK", "", "", "", "YMCKK", "YMCKU" };

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
   if (e == 0x00052400)
      return "Wait"; // ??
   if (e == 0x00052600)
      return "Card position error";
   if (e == 0x0003AD00)
      return "Mag write fail";
   if (e == 0x00062800)
      return "Medium changed";
   return "Printer returned error (see code)";
}


static int debug = 0;           // Top level debug
static int dodump = 0;          // Force all dump
static libusb_device_handle *usb = NULL;        // Connected printer (USB)
static const char *readeric = NULL,
    *readerrfid = NULL;
static const char *printusb = NULL;
static const char *error = NULL;
static const char *status = NULL;       // Status
static unsigned int rxerr = 0;  // Last rx error
static int posn = 0;            // Current card position
static int dpi = 0,
    rows = 0,
    cols = 0;                   // Size
static unsigned char xid8600 = 0;       // Is an XID8600
static unsigned char flip = 0;  // Image needs flipping
static SSL *ss;                 // SSL client connection
static ajl_t i = NULL,
    o = NULL;                   // Output to client

// General
static const char *client_tx(j_t * jp);
static const char *client_status(const char *s);
static const char *moveto(int newposn);

static void dump(const void *buf, size_t len, const char *tag)
{
   if (!debug)
      return;
   static const char *head[] = { "𝟶", "𝟷", "𝟸", "𝟹", "𝟺", "𝟻", "𝟼", "𝟽", "𝟾", "𝟿", "𝙰", "𝙱", "𝙲", "𝙳", "𝙴", "𝙵" };
   for (int i = 0; i < 16; i++)
      fprintf(stderr, " %s ", head[i]);
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
         fprintf(stderr, " (%d)\n", (int) len);
         break;
      }
      fputc('\n', stderr);
   }
}

// Low level USB functions

typedef struct {
   const char *tag;             // For debug
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
static const char *usb_txn_opts(usb_txn_t o)
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
      if (!o.nodump || r)
         dump(cmd, txsize, o.tag);
      if (r)
         return error = libusb_strerror(r);
      if (txsize != sizeof(cmd))
         return error = "USB cmd tx size error";
   }
   if (o.len)
   {                            // Data transfer
      int len = 0;
      if (o.rxlen)
      {
         r = libusb_bulk_transfer(usb, 0x81, o.buf, o.len, &len, to);
         if (!o.nodump)
            dump(o.buf, len, "UDP Rx");
         if (r && r != LIBUSB_ERROR_PIPE)
            return error = libusb_strerror(r);
         *o.rxlen = len;
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
         dump(status, rxsize, status[12] ? "Check condition" : "Good");
      if (error)
         return error;
      //if (status[8] || status[9] || status[10] || status[11]) return error = "Bad USB status residue";
      if (status[12])
         return "";             // Check condition, not an error
   }
   return error;
}

static const char *get_status(void)
{                               // Does not wait
   unsigned char rx[20];
   int rxlen = 0;
 usb_txn("Test status", nodump:1);
 if (!usb_txn("Get status", 0x03, p4: 20, len: 20, buf: rx, rxlen: &rxlen, nodump:1))
      rxerr = (rx[2] << 16) + (rx[12] << 8);
   return error;
}

static const char *usb_ready(int needcards)
{                               // Wait ready
   int last = 0;
   if (error)
      return error;
   while (!error)
   {
      get_status();
      if (!rxerr || (!needcards && rxerr == 0x0002D000))
         break;
      if (!error && ((rxerr >> 16) == 3 || (rxerr >> 16) == 5))
         error = msg(rxerr);
      if (rxerr && rxerr != last)
      {
         if (debug)
            warnx("Status %X: %s", rxerr, msg(rxerr));
         last = rxerr;
         client_tx(NULL);
      }
      usleep(100000);
   }
   if (last)
      client_tx(NULL);
   return error;
}

static const char *usb_connect(j_t j)
{
   if (usb)
      return error;             // connected
   posn = POS_UNKNOWN;
   error = NULL;
   status = "Connecting";
   enum libusb_error r;
   if ((r = libusb_init(NULL)))
      return error = libusb_strerror(r);
   if (printusb && *printusb)
   {
      int vendor,
       product;
      if (sscanf(printusb, "%X:%X", &vendor, &product) != 2)
         return "USB setting is vendor:product";
      usb = libusb_open_device_with_vid_pid(NULL, vendor, product);
   } else
   {                            // default
      usb = libusb_open_device_with_vid_pid(NULL, 0x2166, 0x701d);      // XID8600
      if (!usb)
         usb = libusb_open_device_with_vid_pid(NULL, 0x04f1, 0x403c);   // XID9300
   }
   if (!usb)
      return error;             // Could not connect, let's try ethernet shall we
   if ((r = libusb_set_auto_detach_kernel_driver(usb, 1)))
      return error = libusb_strerror(r);
   if ((r = libusb_claim_interface(usb, 0)))
      return error = libusb_strerror(r);
   // Connected
   status = "Connected";
   j_store_true(j, "usb");
   {                            // Basic info
      unsigned char rx[96];
      int rxlen = 0;
    if (usb_txn("Get info", 0x12, 0, 0, 0, 96, 0, buf: rx, len: 96, rxlen:&rxlen))
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
      strncpy(temp, (char *) rx + 32, 8);
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
    if (usb_txn("Get info", 0x1A, 0, 0x68, 0, 64, 0, buf: rx, len: 64, rxlen: &rxlen, to:5))
         return error;
      dpi = (rx[8] << 8) + rx[9];
      if (!dpi || (rx[10] << 8) + rx[11] != dpi)
         return error = "DPI mismatch";
      rows = (rx[34] << 8) + rx[35];
      cols = (rx[32] << 8) + rx[33];
      j_store_int(j, "card-rows", (rx[18] << 8) + rx[19]);
      j_store_int(j, "card-cols", (rx[16] << 8) + rx[17]);
   }
   {                            // MAC
      unsigned char rx[6];
      int rxlen = 0;
    if (usb_txn("Get MAC", 0x3C, 2, 0x70, 0, 0, 0, 0, 0, 6, buf: rx, len: 6, rxlen:&rxlen))
         return error;
      j_store_string(j, "MAC", j_base16(6, rx));
   }
   return error;
}

static void usb_disconnect(void)
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

static const char *get_settings(j_t j, unsigned char nvr)
{
   unsigned char soft[24];
   memset(soft, 0xFF, 24);
   soft[0] = 0x2B;              // Tag
   soft[1] = 0x16;              // Len
   unsigned char rx[64 + 14];
   int rxlen = 0;
 if (usb_txn(nvr ? "Get NVR settings" : "Get job settings", 0x1A, 0, nvr ? 0x68 : 0x28, 0, 64, buf: rx, len: 64, rxlen:&rxlen))
      return error;
 if (usb_txn(nvr ? "Get NVR settings" : "Get job settings", 0x1A, 0, nvr ? 0x6A : 0x2A, 0, 14, buf: rx + 64, len: 14, rxlen:&rxlen))
      return error;
   j = j_store_object(j, "settings");
   int n;
   for (int i = 0; i < SETTINGS; i++)
      if ((n = settings[i].rpos) < sizeof(rx))
      {
         n = rx[n];
         int s = settings[i].spos;
         if (s < sizeof(soft))
            soft[s] = n;
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
            if (isdigit(*v) || *v == '-')
               j_store_int(j, settings[i].name, atoi(v));
            else if (e - v == 4 && !strncmp(v, "true", e - v))
               j_store_true(j, settings[i].name);
            else if (e - v == 5 && !strncmp(v, "false", e - v))
               j_store_false(j, settings[i].name);
            else
               j_store_stringn(j, settings[i].name, v, e - v);
         }
      }
   if (nvr)
   {                            // Update settings to NVR
    if (usb_txn("Set job settings", 0x15, 0x10, 0x2B, 0, 0x18, buf: soft, len:24))
         return error;
   }
   return error;
}

static const char *set_settings(j_t j)
{
   unsigned char soft[24];
   memset(soft, 0xFF, 24);
   soft[0] = 0x2B;              // Tag
   soft[1] = 0x16;              // Len
   unsigned char rx[64 + 14];
   int rxlen = 0;
 if (usb_txn("Get NVR settings", 0x1A, 0, 0x68, 0, 64, buf: rx, len: 64, rxlen:&rxlen))
      return error;
 if (usb_txn("Get NVR settings", 0x1A, 0, 0x6A, 0, 14, buf: rx + 64, len: 14, rxlen:&rxlen))
      return error;
   unsigned char tx[32 + 10];
   memset(tx, 0xff, 32 + 10);
   tx[0] = 0x28;                // Type
   tx[1] = 0x1E;                // Length
   tx[32] = 0x2A;
   tx[33] = 0x08;               // Length
   int change = 0;
   for (int i = 0; i < SETTINGS; i++)
      if (settings[i].wpos < sizeof(tx))
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
         else
         {                      // Set
            if (settings[i].rpos >= sizeof(rx) || rx[settings[i].rpos] != n)
            {
               tx[settings[i].wpos] = n;
               change++;
            }
            if (settings[i].spos < sizeof(soft))
               soft[settings[i].spos] = n;
         }
      }
   if (change)
   {
      if (j_test(j, "save", 0))
      {                         // Update NVR
       usb_txn("Set NVR settings", 0x15, 0x10, 0x28, 0, 32, len: 32, buf:tx);
       usb_txn("Set NVR settings", 0x15, 0x10, 0x2A, 0, 10, len: 10, buf:tx + 32);
      }
    usb_txn("Set job settings", 0x15, 0x10, 0x2B, 0, 0x18, buf: soft, len:24);
   }
   return error;
}

static const char *get_info(j_t j)
{
   j = j_store_object(j, "info");
   {
      unsigned char rx[44];
      int rxlen = 0;
    if (usb_txn("Get info", 0x1A, 0, 0x63, 0, 44, buf: rx, len: 44, rxlen:&rxlen))
         return error;
      if (rx[6] < sizeof(inktype) / sizeof(*inktype))
         j_store_string(j, "ink", inktype[rx[6]]);
      if (rx[12] < 0xFF)
         j_store_string(j, "ink-lot-number", strndupa((char *) rx + 12, 6));
      j_store_int(j, "ink-total", (rx[8] << 8) + rx[9]);
   }
   {
      unsigned char rx[64];
      int rxlen = 0;
    if (usb_txn("Get info", 0x1A, 0, 0x68, 0, 64, buf: rx, len: 64, rxlen:&rxlen))
         return error;
      j_store_int(j, "ink-available", 2 * rx[0x34]);
      j_store_int(j, "transfer-available", 10 * rx[0x33]);
      j_store_boolean(j, "cards-available", !rx[0x35]);
   }
   return error;
}

static const char *get_counters(j_t j)
{
   unsigned char rx[52];
   int rxlen = 0;
 if (usb_txn("Get counter", 0x4D, 0, 0x78, 0, 0, 0, 0, 0, 52, buf: rx, len: 52, rxlen:&rxlen))
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

static const char *get_position(void)
{
   if (usb_ready(1))
      return error;
   unsigned char rx[8];
   int rxlen = 0;
 if (usb_txn("Get position", 0x34, buf: rx, len: 8, rxlen: &rxlen, cmdlen:10))
      return error;
   if (rx[0])
      posn = POS_OUT;
   else
      posn = rx[7];
   return error;
}

static const char *card_load(unsigned char newposn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   client_status("Loading card");
   if (usb_ready(1))
      return error;
 if (usb_txn("Card load", 0x31, 0x01, p2: immediate ? 1 : 0, p4: (flip ? 2 : 0) + (filminit ? 4 : 0), p7: newposn, to:60))
      return error;
   posn = newposn;
   return error;
}

static const char *card_move(unsigned char newposn, unsigned char immediate, unsigned char flip, unsigned char filminit)
{
   client_status("Moving card");
   if (usb_ready(0))
      return error;
 if (usb_txn("Card move", 0x31, 0x0B, p2: immediate ? 1 : 0, p4: (flip ? 2 : 0) + (filminit ? 4 : 0), p7: newposn, to:30))
      return error;
   posn = newposn;
   return error;
}

static const char *transfer_flip(unsigned char immediate)
{
   client_status("Transfer flip");
   if (!usb_ready(0))
    usb_txn("Transfer flip", 0x31, 0x0A, to:60);
   // May have to wait for temp change
   return error;
}

static const char *transfer_eject(unsigned char immediate)
{
   client_status("Transfer and done");
   if (!usb_ready(0))
    usb_txn("Transfer eject", 0x31, 0x09, to:60);
   // May have to wait for temp change
   return error;
}

static const char *transfer_return(unsigned char immediate)
{
   client_status("Transfer");
   if (!usb_ready(0))
    usb_txn("Transfer return", 0x31, 0x0D, to:60);
   // May have to wait for temp change
   return error;
}

static const char *ic_engage(void)
{
   if (!usb_ready(0))
    usb_txn("IC engage", 0x32, 0x00, to:10);
   return error;
}

static const char *ic_disengage(void)
{
   if (!usb_ready(0))
    usb_txn("IC disengage", 0x32, 0x01, to:10);
   return error;
}

static const char *rfid_engage(void)
{
   if (!usb_ready(0))
    usb_txn("RFID engage", 0x32, 0x04, to:10);
   return error;
}

static const char *rfid_disengage(void)
{
   if (!usb_ready(0))
    usb_txn("RFID disengage", 0x32, 0x05, to:10);
   return error;
}

static const char *mag_iso_encode(j_t j)
{
   moveto(POS_MAG);
   client_status("Mag encode");
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
      else if (p + 2 < sizeof(temp))
      {
         tags[(tag >> 4) - 0x0A] = tag;
         temp[p++] = tag;
         temp[p++] = len;
         if ((tag & 0xF) == 6)
            for (int q = 0; q < len && q < max && p < sizeof(temp); q++)
               temp[p++] = ((v[q] & 0x3F) ^ 0x20);
         else
            for (int q = 0; q < len && q < max && p < sizeof(temp); q++)
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
 usb_txn("ISO encode", 0x2D, 0, 0, tags[0], tags[1], tags[2], 0, 0, p, buf: temp, len: p, to:60);
   client_status("Mag encode done");
   return error;
}

static const char *mag_iso_read(j_t j)
{
   j_array(j);
   j_extend(j, 3);
   moveto(POS_MAG);
   client_status("Mag read");
   if (usb_ready(0))
      return error;
   char rx[100];
   int rxlen = 0;
   void decode(void) {
      warnx("Decode %d", (int) rxlen);
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
   client_status("Track 1");
 if (!usb_txn("ISO read", 0x2C, 0, 0, 0xA6, 0, 0, 76, 0, 0, buf: rx, len: 76, rxlen: &rxlen, to:60))
      decode();
   client_status("Track 2");
 if (!usb_txn("ISO read", 0x2C, 0, 0, 0, 0xB4, 0, 0, 37, 0, buf: rx, len: 37, rxlen: &rxlen, to:60))
      decode();
   client_status("Track 3");
 if (!usb_txn("ISO read", 0x2C, 0, 0, 0, 0, 0xC4, 0, 0, 104, buf: rx, len: 104, rxlen: &rxlen, to:60))
      decode();
   client_status("Mag read done");
   return error;
}

static const char *mag_jis_encode(j_t j)
{
   // TODO
   return error;
}

static const char *mag_jis_read(j_t j)
{
   // TODO
   return error;
}

static const char *send_panel(unsigned char panel, unsigned int len, void *data, unsigned char buffer)
{
   const unsigned char map[] = { 3, 2, 1, 0, 0, 5, 4 };
   if (!usb_ready(0))
    usb_txn("Send panel", 0x2A, 0, map[panel] + (buffer ? 8 : 0), 0, 0, len >> 24, len >> 16, len >> 8, len, len: len, buf:data);
   return error;
}

static const char *print_panels(unsigned char panels, unsigned char immediate, unsigned char buffer)
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
    usb_txn("Print panels", 0x31, 0x08, set, 0, buffer, to:30);
   return error;
}

// Loads of globals - single job at a time

#define freez(n) do{if(n){free((void*)(n));n=NULL;}}while(0)    // Free in situ if needed and null

static const char *pos_name[] = { "print", "ic", "rfid", "mag", "reject", "eject" };

   // Config
static const char *keyfile = NULL;
static const char *certfile = NULL;

// Current connections
static int count = 0;           // Print count

// Cards
static SCARDCONTEXT cardctx;
static SCARDHANDLE card;
static BYTE atr[MAX_ATR_SIZE];
static DWORD atrlen;

static void card_check(void)
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

static const char *card_connect(const char *reader)
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
   client_tx(&j);
   return error;
}

static const char *card_disconnect(void)
{
   if (debug)
      warnx("Card disconnect");
   int res;
   if ((res = SCardDisconnect(card, SCARD_UNPOWER_CARD)) != SCARD_S_SUCCESS)
      return "Cannot end transaction";
   return error;
}

static void card_txn(int txlen, const unsigned char *tx, LPDWORD rxlenp, unsigned char *rx)
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

static const char *moveto(int newposn)
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
         client_tx(&j);
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
         card_load(newposn, 0, 0, 1);
   } else if (newposn >= 0)
      card_move(newposn, 0, 0, 0);
   if (newposn == POS_MAG)
      client_status("Mag stripe");
   else if (newposn == POS_IC)
      client_status("IC station");
   else if (newposn == POS_RFID)
      client_status("RFID station");
   else if (newposn == POS_PRINT)
      client_status("Printing");
   else if (newposn == POS_REJECT)
      client_status("Reject");
   else if (newposn == POS_EJECT)
      client_status("Done");
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

static ssize_t ss_write_func(void *arg, void *buf, size_t len)
{
   return SSL_write(arg, buf, len);
}

static ssize_t ss_read_func(void *arg, void *buf, size_t len)
{
   return SSL_read(arg, buf, len);
}

static const char *client_tx(j_t * jp)
{                               // Send data to client (deletes)
   j_t j;
   if (!jp)
   {
      j = j_create();
      jp = &j;
   } else
      j = *jp;
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
   if (count)
      j_store_int(j, "count", count);
   if (debug)
      j_err(j_write_pretty(j, stderr));
   if (o)
      j_err(j_send(j, o));
   j_delete(jp);
   return error;
}

static const char *client_status(const char *s)
{
   if (error || !strcmp(status ? : "", s ? : ""))
      return error;
   status = s;
   client_tx(NULL);
   return error;
}

// Main connection handling
static char *job(const char *from)
{                               // This handles a connection from client, and connects to printer to perform operations for a job
   count = 0;
   j_t j = j_create();
   j_store_string(j, "status", status = "Connected");
   // Connect to printer, get answer back, report to client
   card_check();
   j_store_boolean(j, "ic", readeric);
   j_store_boolean(j, "rfid", readerrfid);
   usb_connect(j);
   if (!error && !usb)
      error = "No printer available";
   if (!error && (!rows || !cols || !dpi))
      error = "Bad printer info";
   j_store_int(j, "rows", rows);
   j_store_int(j, "cols", cols);
   j_store_int(j, "dpi", dpi);
   if (error)
   {
      if (usb)
         libusb_reset_device(usb);
      return strdup(error);
   }
   get_counters(j);
   get_settings(j, 1);
   get_info(j);
   get_status();
   if (!rxerr || rxerr == 0x0002D000)
      j_store_true(j, "ready"); // Ready for first command
   client_tx(&j);

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
   j_t rx = j_create();
   while (!(ers = j_recv(rx, i)))
   {
      get_status();
      j_t print = j_find(rx, "print");
      if (print && (j_isobject(print) || j_isarray(print)))
      {
         j_detach(print);
         if (debug)
            warnx("Print command not dumped");
      }
      if (debug)
         j_err(j_write_pretty(rx, stderr));
      if (!j_isobject(rx))
      {
         if (debug)
            warnx("Job complete");
         break;                 // Sending null exists
      }
      j_t cmd = NULL;
      if ((cmd = j_find(rx, "settings")))
         set_settings(cmd);
      if ((cmd = j_find(rx, "jis")))
      {
         j_t j = j_create();
         if (j_isstring(cmd))
         {
            mag_jis_encode(cmd);
            if (!error)
               j_store_true(j, "mag");
         } else if (j_isnull(cmd) || j_istrue(cmd))
            mag_jis_read(j_store_array(j, "mag"));
         j_store_true(j, "ready");
         client_tx(&j);
         continue;
      }
      if ((cmd = j_find(rx, "mag")))
      {
         j_t j = j_create();
         if (j_isarray(cmd) || j_isstring(cmd))
         {
            mag_iso_encode(cmd);
            if (!error)
               j_store_true(j, "mag");
         } else if (j_isnull(cmd) || j_istrue(cmd))
            mag_iso_read(j_store_array(j, "mag"));
         j_store_true(j, "ready");
         client_tx(&j);
         continue;
      }
      if ((cmd = j_find(rx, "ic")))
      {
         moveto(POS_IC);
         if (j_isstring(cmd))
         {
            unsigned char *tx = NULL;
            int txlen = j_base16d(j_val(cmd), &tx);;
            unsigned char rx[256];
            DWORD rxlen = sizeof(rx);
            card_txn(txlen, tx, &rxlen, rx);
            j_t j = j_create();
            j_store_string(j, "ic", j_base16(rxlen, rx));
            j_store_true(j, "ready");
            client_tx(&j);
            continue;
         }
      }
      if ((cmd = j_find(rx, "rfid")))
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
            j_store_true(j, "ready");
            client_tx(&j);
            continue;
         }
      }
      if (print)
      {
         if (j_istrue(print) || j_isnull(print))
         {
            moveto(POS_PRINT);  // ready to print
            client_status("Ready to print");
         } else
         {
            client_status("Making artwork");
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
                     if (printed)
                        transfer_flip(0);
                     else
                        card_move(POS_PRINT, 0, 1, 0);  // Flip no transfer
                     client_status("Second side");
                  } else
                     client_status("First side");
                  printed = 0;
                  for (int p = 0; p < 8; p++)
                     if ((p < 3 && (found & 7)) || (found & (1 << p)))
                     {          // Send panel
                        send_panel(p, rows * cols, data[p], 0);
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
                              transfer_return(0);
                           client_status("UV");
                           print_panels(printed & 0x40, 0, 0);  // UV print
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
            if (j_isobject(print))
               print_side(print);
            else if (j_isarray(print))
            {
               print_side(j_index(print, 0));
               print_side(j_index(print, 1));
            }
            if (printed)
               count++;
            if (printed)
            {
               client_tx(NULL);
               transfer_eject(0);
               status = "Printed";
            } else
            {
               moveto(POS_EJECT);       // Done anyway
               status = "Unprinted";
            }
            break;
         }
      } else if ((cmd = j_find(rx, "reject")))
      {
         if (posn >= 0)
            moveto(POS_REJECT);
         break;
      } else if ((cmd = j_find(rx, "eject")))
      {
         moveto(POS_EJECT);
         break;
      }
      get_status();
      j_t j = j_create();
      j_store_true(j, "ready"); // Ready for next command
      client_tx(&j);
   }
   j_delete(&rx);
   if (!ers && error)
      ers = strdup(error);
   if (ers)
   {
      if (debug)
         warnx("Error: %s", ers);
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
   {
      j_t j = j_create();
      j_store_true(j, "complete");      // Done.
      client_tx(&j);
   }
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
         { "usb", 'U', POPT_ARG_STRING, &printusb, 0, "Printer port (USB)", "XXXX:XXXX" },
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
