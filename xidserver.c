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
#include <png.h>

// Loads of globals - single job at a time

#define freez(n) do{if(n){free((void*)(n));n=NULL;}}while(0)    // Free in situ if needed and null

j_t j_new(void);
const char *client_tx(j_t j);
const char *pos_name[] = { "print", "ic", "rfid", "mag", "reject", "eject" };

#define	POS_UNKNOWN	-2
#define	POS_OUT		-1
#define	POS_PRINT	0
#define	POS_IC		1
#define	POS_RFID	2
#define	POS_MAG		3
#define	POS_REJECT	4
#define	POS_EJECT	5

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

// Config
int debug = 0;                  // Top level debug
const char *printhost = NULL;   // Printer host/IP
const char *printport = "50730";        // Printer port (this is default for XID8600)
const char *keyfile = NULL;     // SSL
const char *certfile = NULL;

// Current connectionns
SSL *ss;                        // SSL client connection
libusb_device_handle *usbhandle = NULL;
const char usbmanufacturer[] = "G-Printec, Inc.";
const char usbproduct[] = "Card Printer";
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
int count = 0;                  // Print count
int dpi = 0,
    rows = 0,
    cols = 0;                   // Size

// Cards
SCARDCONTEXT cardctx;
const char *readeric = NULL,
    *readerrfid = NULL;
SCARDHANDLE card;
BYTE atr[MAX_ATR_SIZE];
DWORD atrlen;

void card_check(void)
{                               // list the readers
   long res;
   DWORD temp;
   char *r,
   *e;
   freez(readeric);
   freez(readerrfid);
   if ((res = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &cardctx)) != SCARD_S_SUCCESS)
   {
      warnx("Cannot get PCSC context, is pcscd running?");
      return;
   }
   if ((res = SCardListReaders(cardctx, NULL, NULL, &temp)) != SCARD_S_SUCCESS)
   {
      warnx("Cannot get reader list (%s)", pcsc_stringify_error(res));
      return;
   }
   if (!(r = malloc(temp)))
   {
      warnx("Cannot allocated %d bytes for reader list", (int) temp);
      return;
   }
   if ((res = SCardListReaders(cardctx, NULL, r, &temp)) != SCARD_S_SUCCESS)
   {
      warnx("Cannot list readers (%s)", pcsc_stringify_error(res));
      return;
   }
   e = r + temp;
   while (*r && r < e)          // && !error)
   {
      if (!readeric && strstr(r, "HID Global OMNIKEY 3x21 Smart Card Reader"))
         readeric = strdup(r);
      else if (!readerrfid && strstr(r, "OMNIKEY AG CardMan 5121"))
         readerrfid = strdup(r);
      else
         warnx("Additional card reader %s ignored", r);
      r += strlen(r) + 1;
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
   j_t j = j_new();
   j_store_string(j, "atr", j_base16(atrlen, atr));
   client_tx(j);
   return NULL;
}

const char *card_disconnect(void)
{
   if (debug)
      warnx("Card disconnect");
   int res;
   if ((res = SCardDisconnect(card, SCARD_UNPOWER_CARD)) != SCARD_S_SUCCESS)
      return "Cannot end transaction";
   return NULL;
}

void card_txn(int txlen, const unsigned char *tx, LPDWORD rxlenp, unsigned char *rx)
{
   if (error)
      return;
   SCARD_IO_REQUEST recvpci;
   if (debug)
      fprintf(stderr, "Card Tx: %s\n", j_base16(txlen, tx));
   int res;
   if ((res = SCardTransmit(card, SCARD_PCI_T0, tx, txlen, &recvpci, rx, rxlenp)) != SCARD_S_SUCCESS)
   {
      warnx("Failed to send command (%s)", pcsc_stringify_error(res));
      *rxlenp = 0;
   }
   if (debug)
      fprintf(stderr, "Card Rx: %s\n", j_base16(*rxlenp, rx));
   if (*rxlenp < 2)
      warnx("Unexpected response");
   if (buf[0] == 0x93)
      warnx("Busy error %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x62 || buf[0] == 0x63)
      warnx("Warning %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x64 || buf[0] == 0x65)
      warnx("Execution error %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x67 || buf[0] == 0x6C)
      warnx("Wrong length %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x68)
      warnx("Function in CLA not supported %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x69)
      warnx("Command not allowed %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x6A || buf[0] == 0x6B)
      warnx("Wrong parameter %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x6D)
      warnx("Invalid INS %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x6E)
      warnx("Class not supported %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x6F)
      warnx("No diagnosis - error %02X %02X", buf[0], buf[1]);
   if (buf[0] == 0x68)
      warnx("CLA error %02X %02X", buf[0], buf[1]);
}

// Printer specific settings
unsigned char xid8600 = 0;      // Is an XID8600
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
   if (count)
      j_store_int(j, "count", count);
   return j;
}

const char *printer_connect(void)
{                               // Connect to printer, return error if fail
   posn = POS_UNKNOWN;
   queue = 0;
   error = NULL;
   status = "Connecting";
   seq = 0x99999999;
   txcmd = rxcmd = rxerr = 0;
   if (!printhost)
   {
      if (usbhandle)
         return error = "Printer already connected";
      if (libusb_init(NULL) < 0)
         return error = "USB init fail";
      libusb_device **devs;
      ssize_t cnt = libusb_get_device_list(NULL, &devs);
      if (cnt < 0)
         return error = "USB failed";
      for (int i = 0; i < cnt; i++)
      {
         struct libusb_device_descriptor desc;
         if (libusb_get_device_descriptor(devs[i], &desc) < 0)
            continue;
         unsigned char string[256];
         libusb_open(devs[i], &usbhandle);
         if (!usbhandle)
            continue;
         if (desc.iManufacturer && libusb_get_string_descriptor_ascii(usbhandle, desc.iManufacturer, string, sizeof(string)) > 0 && !strcasecmp((char *) string, usbmanufacturer) &&
             desc.iProduct && libusb_get_string_descriptor_ascii(usbhandle, desc.iProduct, string, sizeof(string)) > 0 && !strcasecmp((char *) string, usbproduct))
            break;
         libusb_close(usbhandle);
         usbhandle = NULL;
      }
      if (!usbhandle)
      {
         libusb_exit(NULL);
         return error = "USB Printer not found";
      }
      return NULL;
   }
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
   if (!printhost)
   {
      if (!usbhandle)
         return "Not connected";        // Not connected, that is OK
      libusb_exit(NULL);
      usbhandle = NULL;
      return NULL;
   }
   if (psock < 0)
      return "Not connected";   // Not connected, that is OK
   close(psock);
   psock = -1;
   return NULL;
}

const char *printer_tx(void)
{                               // Raw printer send
   if (error)
      return error;
   if (!usbhandle && psock < 0)
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
      for (i = 0; i < buflen && i < 32 * 4 - 4; i++)
         fprintf(stderr, "%s%02X", i && !(i & 31) ? "\n     " : (i & 3) ? "" : " ", buf[i]);
      if (i < buflen)
         fprintf(stderr, "... (%d)", buflen);
      fprintf(stderr, "\n");
   }
   n = 0;
   while (n < buflen)
   {
      int l = 0;
      if (psock < 0)
         libusb_bulk_transfer(usbhandle, 0x81, buf + buflen, n - buflen, &l, 60000);
      else
         l = write(psock, buf + n, buflen - n);
      if (usbhandle)
         warnx("tx=%d", (int) l);
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
   if (!usbhandle && psock < 0)
      rxcmd = 0;
   buflen = 0;
   unsigned int n = 8;
   if (usbhandle)
      n = 512;                  // TODO DEBUG
   while (buflen < n)
   {
      if (bufmax < n && !(buf = realloc(buf, bufmax = n)))
         errx(1, "malloc");
      int l = 0;
      if (psock < 0)
         libusb_bulk_transfer(usbhandle, 0x02, buf + buflen, n - buflen, &l, 60000);
      else
         l = read(psock, buf + buflen, n - buflen);
      if (usbhandle)
         warnx("rx=%d", (int) l);
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

const char *printer_start_cmd(unsigned int cmd);
const char *printer_rx_check(void)
{
   if (error)
      return error;
   printer_rx();
   if (!error && ((rxerr >> 16) == 2 || rxerr == 0x00062800))
   {                            // Wait
      while (queue)
         printer_rx();
      time_t giveup = time(0) + 300;
      const char *last = NULL;
      while (((rxerr >> 16) == 2 || rxerr == 0x00062800) && !error && time(0) < giveup)
      {
         const char *warn = msg(rxerr);
         rxerr = 0;
         if (warn != last)
         {
            last = warn;
            j_t j = j_new();
            j_store_string(j, "status", warn);
            j_store_boolean(j, "wait", 1);
            client_tx(j);
         }
         usleep(100000);
         printer_start_cmd(0x01020000);
         printer_tx();
         printer_rx();
      }
      if (!rxerr)
         client_tx(j_new());
   }
   if (!error && rxerr)
      error = msg(rxerr);
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
   // First byte if command
   // Second is number of bytes after this byte
   // Commands
   // 1: Check status
   // 2: Check position
   // 3: Initialise
   // 4: Load Move (third byte 80 for load maybe, 10 for flip, forth is position)
   // 5: Move (forth is position)
   // 6: Print to transfer film (forth byte is layers to transfer)
   // 7: Transfer to card (third/forth same as move card, done after transfer)
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
   }
   return error;
}

const char *moveto(int newposn)
{
   if (error || posn == newposn)
      return error;             // Nothing to do
   if (posn == POS_IC)
   {
      card_disconnect();
      printer_cmd(0x0A024000);  // Disengage contact station
   } else if (posn == POS_RFID)
   {
      card_disconnect();
      printer_cmd(0x0A025000);  // Disengage contact station
   }
   if (posn < 0)
   {                            // not in machine
      if (newposn == POS_EJECT)
         error = "Cannot eject card, not loaded";
      else if (newposn == POS_REJECT)
         error = "Cannot reject card, not loaded";
      else
      {
         status = "Load card";
         //printer_queue_cmd(0x04028000 + newposn);       // Load
         printer_queue_cmd(0x04020000 + newposn);       // Load
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
   {
      printer_cmd(0x0A020000);  // Engage contacts
      if ((error = card_connect(readeric)))
         return error;
   } else if (posn == POS_RFID)
   {
      printer_cmd(0x0A021000);  // Engage contacts
      if ((error = card_connect(readerrfid)))
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
   if (debug)
      j_err(j_write_pretty(j, stderr));
   j_err(j_write_func(j, ss_write_func, ss));   // flushes
   j_delete(&j);
   return NULL;
}

char *client_rx(j_t j, void *arg)
{                               // Process received message
   arg = arg;
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
   if ((cmd = j_find(j, "mag")))
   {
      // TODO mag read probably should say which tracks to try reading
      unsigned char temp[66 * 3];
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
      if (j_isstring(cmd))
         encode(0x24, cmd);
      else if (j_isarray(cmd))
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
      if (j_isnull(cmd) || j_istrue(cmd))
      {                         // Read
         status = "Reading";
         client_tx(j_new());
         moveto(POS_MAG);
         check_position();
         // Load tacks separately as loading all at once causes error if any do not read
         void mread(j_t j, unsigned char tag) {
            char t = (tag >> 4) - 1;
            unsigned char temp[4] = { };
            if (t)
               temp[t - 1] = tag;
            printer_start_cmd(0x08060000 + (t ? 0 : 0x16));
            printer_data(4, temp);
            printer_tx();
            printer_rx();
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
         j_t j = j_new();
         j_t m = j_store_array(j, "mag");
         mread(m, 0x16);
         mread(m, 0x24);
         mread(m, 0x34);
         client_tx(j);
      }
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
         j_t j = j_new();
         j_store_string(j, "ic", j_base16(rxlen, rx));
         client_tx(j);
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
         j_t j = j_new();
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
      unsigned char printed = 0;
      unsigned char side = 0;
      const char *print_side(j_t panel) {
         if (error)
            return error;
         if (!panel)
            return NULL;
         unsigned char found = 0;
         unsigned char *data[8] = { };
         const char *add(const char *tag, int layer) {
            if (error)
               return error;
            const char *d = j_get(panel, tag);
            if (!d)
               return NULL;
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
                  warnx("PNG %s%d:%ux%u (%+d/%+d) card %d/%d", tag, side, width, height, dx, dy, cols, rows);
               png_set_expand(png_ptr); // Expand palette, etc
               png_set_strip_16(png_ptr);       // Reduce to 8 bit
               png_set_packing(png_ptr);        // Unpack
               if (layer)
                  png_set_rgb_to_gray(png_ptr, 1, 54.0 / 256, 183.0 / 256);
               else
                  png_set_gray_to_rgb(png_ptr); // RGB
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
               {                // CMY
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
                     //if (r < 2 || r >= height - 2) warnx("Row %d y=%d Data %02X %02X ... %02X %02X", r, y, image[0], image[1], image[width - 2], image[width - 1]);
                     if (y >= 0 && y < rows)
                        for (int c = 0; c < width; c++)
                        {
                           int x = c + dx;
                           if (x >= 0 && x < cols)
                           {
                              int o = (rows - 1 - y) * cols + (cols - 1 - x);
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
                     {          // Blank
                        free(data[layer]);
                        data[layer] = NULL;
                     } else
                        found |= (1 << layer);
                  }
                  free(image);
               } else
               {                // K or U
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
                              int o = (rows - 1 - y) * cols + (cols - 1 - x);
                              if (layer == 3)
                                 data[layer][o] = ((image[c] & 0x80) ? 0 : 0xFF);       // Black
                              else
                                 data[layer][o] = image[c] ^ 0xFF;
                           }
                        }
                  }
                  int z;
                  for (z = 0; z < rows * cols && !data[layer][z]; z++);
                  if (z == rows * cols)
                  {             // Blank
                     free(data[layer]);
                     data[layer] = NULL;
                  } else
                     found |= (1 << layer);
                  free(image);
               }
               png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
               return NULL;
            }
            process();
            fclose(f);
            free(png);
            return NULL;
         }
         add("C", 0);
         add("K", 3);
         add("U", 6);
         if (found)
         {
            moveto(POS_PRINT);  // ready to print
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
                  temp[0] = (1 << p);
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
                  printed |= (1 << p);
                  while (queue > 3)
                     printer_rx_check();
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
                     if (printed & 0x0F)
                     {
                        status = "Printing";
                        client_tx(j_new());
                        printer_queue_cmd(0x07020000);  // first transfer of non UV
                     }
                     status = "UV";
                     client_tx(j_new());
                     printer_queue_cmd(0x06020000 + (printed & 0x40));  // UV print
                  }
               }
            }
            check_status();
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
      while (queue)
         printer_rx_check();
      if (printed)
      {
         status = "Transfer";
         client_tx(j_new());
         printer_cmd(0x07020000 + (posn = POS_EJECT));
         status = "Printed";
         count++;
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
   card_check();
   count = 0;
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
   j_t j = j_new();
   j_store_string(j, "id", id);
   j_store_string(j, "type", type);
   j_store_int(j, "rows", rows);
   j_store_int(j, "cols", cols);
   j_store_int(j, "dpi", dpi);
   j_store_boolean(j, "ic", readeric);
   j_store_boolean(j, "rfid", readerrfid);
   client_tx(j);
   check_status();
   check_position();
   if (posn >= 0)
      moveto(POS_REJECT);
   check_position();
   client_tx(j_new());
   // Handle messages both ways
   char *ers = NULL;
   if (!error)
      ers = j_stream_func(ss_read_func, ss, client_rx, NULL);
   if (!ers && error)
      ers = strdup(error);
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
   int lqueue = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "host", 'h', POPT_ARG_STRING, &bindhost, 0, "Host to bind", "Host/IP" },
         { "port", 'p', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &port, 0, "Port to bind", "port" },
         { "printer", 'H', POPT_ARG_STRING, &printhost, 0, "Printer", "Host/IP" },
         { "print-port", 'P', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &printport, 0, "Printer port", "port" },
         { "key-file", 'k', POPT_ARG_STRING, &keyfile, 0, "SSL key file", "filename" },
         { "cert-file", 'k', POPT_ARG_STRING, &certfile, 0, "SSL cert file", "filename" },
         { "listen", 'q', POPT_ARG_INT, &lqueue, 0, "Listen queue", "N" },
         { "daemon", 'd', POPT_ARG_NONE, &background, 0, "Background" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
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
      {                         // Child (fork to ensure memory leaks never and issue - yeh, cheating)
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
         return 0;
      }
      int pstatus = 0;
      waitpid(pid, &pstatus, 0);
      if (!WIFEXITED(pstatus) || WEXITSTATUS(pstatus))
         warnx("Job failed");
      close(s);
   }
   {
      int res;
      if ((res = SCardReleaseContext(cardctx)) != SCARD_S_SUCCESS)
         errx(1, "Cant release context (%s)", pcsc_stringify_error(res));
   }

   return 0;
}
