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


// Protocol notes
// TCP 50730
// Ethernet send/rx blocks consisting of sequence of 4 byte words
// Tx (to printer)
// 0:   is some sort of function - low bit of 1st byte is 0 for command
//      F2 03   Document info
//      F0 00 01   Command
//      F0 00 02   Load image
//      F0 00 04   Get Settings (send set of setting words with 000000)
//      F0 00 06   Check status
//      FO 00 08   Change settings (send set of new settings works) - UDP...
// 1:   is count of words from this point (i.e. total-1)
// 2:   is a parameter of some sort, usually 0
// 3:   is sequence
// 4:   onwards is data
// Rx (from printer)
// 0:   Ack/function low bit of first byte is 1 for response
//      F3 02   Connect response
//      F3 04   Response to document info
//      F1 00 01   Command response, OK?
//      F1 00 02   Command response, Not OK?
//      F1 00 03   Command response, error?
//      F1 00 04   Settings
// 1:   is count of words from this point (i.e. total-1)
// 2:   is status/error (0=OK)
// 3:   is sequence (normally echos request to which this is response)
// The command syntax is then bytes starting on word 4
// 0:   Command
// 1:   Number of bytes following this
// Commands:
// 01:  Status check, send 2 bytes 00 00
// 02:  Read position, response is position and mode
// 03:  Initialise, 2 bytes 00 00
// 04:  Load card, 2 bytes: flags and position
//      00      Normal
//      10      Flip
//      80      Film initialise
// 05:  Move card, 2 bytes as per load card
// 06:  Print, two bytes, flags
//      0000    No MAC on UV - yay!
//      0100    Upper right MAC on UV
//      0200    Lower left MAC on UV
//      2000    Buffer 1
// 07:  Transfer, 6 bytes, flags and position and 00
//      10      Flip
// 08:  Mag read
// 09:  Mag write
// 0A:  Contacts, 2 bytes, flags and 00
//      10      Not contact engage
//      40      Not contactless engage
// 0B:
// Dump examples
/*
// Connect response
// f300 0200 0000 001c
   0000 0000 0003 3118 0000 0000 0000 0000
   0000 0000 0000 5052 494e 5445 5230 3100
   5bf0 b0c7 0000 0000 0000 0000 0000 0000
   5849 4438 3630 3020 2020 2020 2020 2020
   fe80 0000 0000 0000 923d 68ff fe02 58c3
   2001 067c 2a40 0000 923d 68ff fe02 58c3
   0000 0000 0000 0000 0000 0000 0000 0000
// Document start
// f200 0300 0000 001d
   0000 0002 9999 9999 0000 0000 7809 1107
   1529 0000 4f00 7700 6e00 6500 7200 0000
   0000 0000 0000 0000 0000 0000 0000 0000
   0000 0000 4400 6f00 6300 7500 6d00 6500
   6e00 7400 0000 0000 0000 0000 0000 0000
   0000 0000 0000 0000 0000 0000 0000 0000
   0000 0000 0000 0000 0000 0000 0000 0000
   0000 0000
// Response
// f300 0400 0000 0002
   0000 0000 9999 9999
// Check status
// f000 0600 0000 0003
   0000 0000 0000 0000 0100 0000
// f100 0600 0000 0004
   0000 0000 0000 0000 0106 04ff 02d0 0000	No card
// f100 0600 0000 0004
   0000 0000 0000 0000 0106 0400 0000 0000	OK
// Check laminator status
// f000 0600 0000 0003
   0000 0000 0000 0000 3e00 0000
// f100 0600 0000 0004
   0000 0000 0000 0000 3e06 02fe 0000 0000	No laminator?
// Test ready
// f000 0100 0000 0003
   0000 0000 0000 0000 0102 0000
// f100 0200 0000 0002
   0002 d000 0000 0000				No card
// f100 0100 0000 0002
   0000 0000 0000 0000				Ready
// Read position
// f000 0100 0000 0003
   0000 0000 0000 0000 0202 0000
// f100 0300 0000 0003
   0000 0000 0000 0000 0102 0400		Card not in machine...
// Card load to position 2 (Non contact)
// f000 0100 0000 0003
   0000 0000 0000 0000 0402 0002
// f100 0100 0000 0002
   0000 0000 0000 0000				
// Card load position 4 (NG Card Exit)
// f000 0100 0000 0003
   0000 0000 0000 0000 0402 0004
// f100 0200 0000 0002
   0005 2a00 0000 0000				Won't!
// f000 0100 0000 0003
   0000 0000 0000 0000 0402 1000		Load flip
// f000 0100 0000 0003
   0000 0000 0000 0000 0402 8000		Load film init
// Read position
// f000 0100 0000 0003
   0000 0000 0000 0000 0202 0000
// f100 0300 0000 0003
   0000 0000 0000 0000 0102 0002		Position 2 mode 0 (supply with card try)
// Print
// f000 0100 0000 0004
   0000 0000 0000 0000 0706 0005 0000 0000	Transfer Eject
// f000 0100 0000 0004
   0000 0000 0000 0000 0706 0000 0000 0000	Transfer Return
// f000 0100 0000 0004
   0000 0000 0000 0000 0706 1000 0000 0000	Transfer turn
// f000 0100 0000 0003
   0000 0000 0000 0000 0602 016f		Print (CMY/K/UV/PO) buffer 0 upper right
// f000 0100 0000 0003
   0000 0000 0000 0000 0602 216f		^ buffer 1
// f000 0100 0000 0003
   0000 0000 0000 0000 0602 026f		^ Lower left
// f000 0100 0000 0003
   0000 0002 0000 0000 0602 016f		^ Immediate (no change)
// Card move
// f000 0100 0000 0003
   0000 0000 0000 0000 0502 0001		// Position 1
// f000 0100 0000 0003
   0000 0002 0000 0000 0502 0001		^ Immediate (no change)
// f000 0100 0000 0003
   0000 0000 0000 0000 0502 1001		^ Flip
// f000 0100 0000 0003
   0000 0000 0000 0000 0502 8001		^ Film initialise
// f000 0100 0000 0003
   0000 0000 0000 0000 0502 0004		^ Exit
// Initialise
// f000 0100 0000 0003
   0000 0000 0000 0000 0302 0000
// IC control
// f000 0100 0000 0003
   0000 0000 0000 0000 0a02 0000		// Contact / contact
// f000 0100 0000 0003
   0000 0000 0000 0000 0a02 1000		// No contact / contact
// f000 0100 0000 0003
   0000 0000 0000 0000 0a02 4000		// Contact / release
// f000 0100 0000 0003
   0000 0000 0000 0000 0a02 5000		// No contact / release
// Load image
   F0000200 000A7F25 00000000 00000005 01000000 0029FC84 0029FC80 00000000 ...
// Settings UDP
	f000 0400
	0x0020:  0000 001d 0000 0000 0000 0000 4600 0000
	0x0030:  4700 0000 4a00 0000 4b00 0000 5c00 0000
	0x0040:  4c00 0000 4d00 0000 4e00 0000 5500 0000
	0x0050:  5600 0000 4f00 0000 5000 0000 3300 0000
	0x0060:  1400 0000 1600 0000 1800 0000 2a00 0000
	0x0070:  2800 0000 4800 0000 1d00 0000 1e00 0000
	0x0080:  1b00 0000 2900 0000 1f00 0000 5d00 0000
	0x0090:  5e00 0000 5f00 0000

	// Looks like 4 bytes, starting with code and 00 00 00
	// Response looks same with data in those three bytes
	// len, type, data, so 0201NN is value NN

	1402 0100	Card thickness 0=Standard, 2=Thin
	1602 0100	Buzzer, 0=On,1=Off
	1802 0108	Heat Roller power save, 6=45min,7=60min,8=Off
	1d02 0101	Display mode, 0=counter,1=laminator state
	1e02 0100	Display counter, 0=Total,1=Head,2=Free,3=Clean,4=Error
	1b02 0103	Display contrast, 3=0,4=+1,5=+2
	1f02 0100	Presumed security lock
	2802 0101	Retry count,0-3
	2902 0100	Mag JIS type, 0=LoCo,1=HiCo
	2a02 0101	Mag ISO type, 0=LoCo,1=HiCo
	3206 0400 0000 0000 Counter reset
	3302 0100	Film type 0=1000, 2=750
	4602 0103	K level, 2=-1,3=0,4=+1,5=+2,6=+3
	4702 0101	K mode, 0=Standard,1=Fine
	4802 0103	YMC level, 2=-1,3=0,4=+1,5=+2,6=+3
	4a02 0106	UV Level, 2=-1,3=0,4=+1,5=+2,6=+3
	4b02 0103	PO level, 2=-1,3=0,4=+1,5=+2,6=+3
	4c02 0103	Transfer temp, 0=-2,1=-1,2=0,3=+1,4=+2
	4d02 0101	Transfer speed front, 1=+1,2=0,3=-1,4=-2,5=-3
	4e02 0101	Transfer speed back, 1=+1,2=0,3=-1,4=-2,5=-3
	4f02 010a	Bend temp level, 10=Off,0=-5,1=-4,2=-3,3=-2
	5002 0104	Bend speed, 0=-2,1=-1,2=0,3=+1,4=+2
	5502 0100	MG Peel mode, 0=standard, 1=MG Stripe
	5602 0100	Standby mode, 0=front wait, 1=back wait
	5c02 0100	Heat roller Control, 0=Off,1=On
	5d02 0100	Transfer speed front UV, 1=+1,2=0,3=-1,4=-2,5=-3
	5e02 0100	Transfer speed back UV, 1=+1,2=0,3=-1,4=-2,5=-3
	5f02 0100	Backside cool, 0=0ff, 1=On

*/

typedef struct setting_s setting_t;
const struct setting_s {
   unsigned char tag;
   const char *name;
   const char *vals;
} settings[] = {
   { 0x14, "card-thickness", "standard//thin" },
   { 0x16, "buzzer", "true/false" },
   { 0x18, "hr-power-save", "//////45/60/false" },
   { 0x1d, "display-mode", "counter/laminator" },
   { 0x1e, "display-counter", "total/head/free/clean/error" },
   { 0x1b, "display-contrast", "///0/1/2" },
   { 0x1f, "security-lock", "false/true" },
   { 0x28, "retry-count", "0/1/2/3" },
   { 0x29, "jis-type", "loco/hico" },
   { 0x2a, "iso-type", "loco/hico" },
   { 0x33, "film-type", "1000/750" },
   { 0x46, "k-level", "//-1/0/1/2/3" },
   { 0x47, "k-mode", "standard/fine" },
   { 0x48, "ymc-level", "//-1/0/1/2/3" },
   { 0x4a, "uv-level", "//-1/0/1/2/3" },
   { 0x4b, "po-level", "//-1/0/1/2/3}" },
   { 0x4c, "transfer-temp", "-2/-1/0/1/2" },
   { 0x4d, "transfer-speed-front", "/1/0/-1/-2/-3" },
   { 0x4e, "transfer-speed-back", "/1/0/-1/-2/-3" },
   { 0x4f, "bend-temp", "-5/-4/-3/-2///////false" },
   { 0x50, "bend-speed", "-2/-1/0/1/2" },
   { 0x55, "mg-peel-mode", "standard/stripe" },
   { 0x56, "standby-mode", "front/back" },
   { 0x5c, "hr-control", "false/true" },
   { 0x5d, "transfer-speed-uv-front", "/1/0/-1/-2/-3" },
   { 0x5e, "transfer-speed-uv-back", "/1/0/-1/-2/-3" },
   { 0x5f, "backside-cool", "false/true" },
};

#define SETTINGS (sizeof(settings)/sizeof(*settings))

// Loads of globals - single job at a time

#define freez(n) do{if(n){free((void*)(n));n=NULL;}}while(0)    // Free in situ if needed and null

j_t j_new(void);
const char *client_tx(j_t j, ajl_t o);
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
      else if (!readeric && strstr(p, "XIRING Teo"))
         readeric = strdup(p);
      else if (!readerrfid && strstr(p, "OMNIKEY AG CardMan 5121"))
         readerrfid = strdup(p);
      else
         warnx("Additional card reader %s ignored", p);
      p += strlen(p) + 1;
   }
   free(r);
}

const char *card_connect(const char *reader, ajl_t o)
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
   client_tx(j, o);
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
      for (i = 0; i < buflen && i < 32 * 5 - 4; i++)
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
const char *printer_rx_check(ajl_t o)
{
   if (error)
      return error;
   if (!error && ((rxerr >> 16) == 2 || rxerr == 0x00062800))
   {                            // Wait
      while (queue && !error)
         printer_rx();
      if (error)
         return error;
      time_t giveup = time(0) + 300;
      time_t update = 0;
      time_t now = 0;
      int last = 0;
      while (((rxerr >> 16) == 2 || rxerr == 0x00062800) && !error && (now = time(0)) < giveup)
      {
         if (rxerr != last || now > update)
         {
            last = rxerr;
            j_t j = j_new();
            j_store_true(j, "wait");
            client_tx(j, o);
            update = now;
         }
         usleep(100000);
         printer_start_cmd(0x01020000);
         printer_tx();
         printer_rx();
      }
      if (!rxerr)
         client_tx(j_new(), o);
      if (!error && rxerr)
         error = msg(rxerr);
      return error;
   }
   if (!error && rxerr)
      error = msg(rxerr);
   else
      printer_rx();
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

const char *printer_tx_check(ajl_t o)
{                               // Send and check reply
   if (error)
      return error;
   printer_tx();
   while (queue && !error)
      printer_rx_check(o);      // Catch up
   return error;
}

const char *printer_start_cmd(unsigned int cmd)
{
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

const char *printer_cmd(unsigned int cmd, ajl_t o)
{                               // Simple command and response
   printer_start_cmd(cmd);
   return printer_tx_check(o);
}

const char *set_settings(j_t s, ajl_t o)
{
   // TODO this seems to not work, maybe only works via UDP?
   if (error)
      return error;
   while (queue && !error)
      printer_rx_check(o);
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
         data[p++] = settings[i].tag;
         data[p++] = 2;
         data[p++] = 1;
         data[p++] = n;
      }
   }
   printer_start(0xF0000800, 0);
   printer_data(p, data);
   printer_tx();
   printer_rx();
   return error;
}

const char *get_settings(j_t j, ajl_t o)
{
   if (error)
      return error;
   while (queue && !error)
      printer_rx_check(o);
   printer_start(0xF0000400, 0);
   unsigned char data[SETTINGS * 4] = { };
   for (int i = 0; i < SETTINGS; i++)
      data[i * 4] = settings[i].tag;
   printer_data(sizeof(data), data);
   printer_tx();
   printer_rx();
   if (buf[7] == (sizeof(data) + 3) / 4 + 2)
   {
      memcpy(data, buf + 16, sizeof(data));
      j_t s = j_store_object(j, "settings");
      for (int i = 0; i < SETTINGS; i++)
         if (data[i * 4] == settings[i].tag && data[i * 4 + 1] == 2 && data[i * 4 + 2] == 1)
         {
            int n = data[i * 4 + 3];
            const char *v = settings[i].vals;
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
         }
   }
   return error;
}

const char *check_status(ajl_t o)
{
   if (error)
      return error;
   while (queue && !error)
      printer_rx_check(o);
   return printer_cmd(0x01020000, o);
}

const char *check_position(ajl_t o)
{
   while (queue && !error)
      printer_rx_check(o);
   if (error)
      return error;
   if (!printer_cmd(0x02020000, o))
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

const char *moveto(int newposn, ajl_t o)
{
   if (error || posn == newposn)
      return error;             // Nothing to do
   if (posn == POS_IC || posn == POS_RFID)
   {
      card_disconnect();
      printer_queue_cmd(0x0A025000);    // Disengage stations
   }
   if (posn < 0)
   {                            // not in machine
      check_status(o);
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
      printer_cmd(0x0A024000, o);       // Engage contacts, not RFID
      check_status(o);
      if ((error = card_connect(readeric, o)))
         return error;
   } else if (posn == POS_RFID)
   {
      printer_cmd(0x0A021000, o);       // Engage RFID, not contacts
      check_status(o);
      if ((error = card_connect(readerrfid, o)))
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

const char *client_tx(j_t j, ajl_t o)
{                               // Send data to client (deletes)
   if (!ss)
      return "No client";
   if (debug)
      j_err(j_write_pretty(j, stderr));
   j_err(j_send(j, o));
   j_delete(&j);
   return NULL;
}

// Main connection handling
char *job(const char *from)
{                               // This handles a connection from client, and connects to printer to perform operations for a job
   ajl_t o = ajl_write_func(ss_write_func, ss);
   // Connect to printer, get answer back, report to client
   card_check();
   count = 0;
   printer_connect();
   printer_rx_check(o);
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
      printer_tx_check(o);
   }
   check_position(o);
   j_t j = j_new();
   get_settings(j, o);
   j_store_string(j, "id", id);
   j_store_string(j, "type", type);
   j_store_int(j, "rows", rows);
   j_store_int(j, "cols", cols);
   j_store_int(j, "dpi", dpi);
   j_store_boolean(j, "ic", readeric);
   j_store_boolean(j, "rfid", readerrfid);
   if (rxerr)
      j_store_true(j, "wait");
   client_tx(j, o);
   check_status(o);
   check_position(o);
   if (posn >= 0)
   {
      if (debug)
         warnx("Unexpected card position %d", posn);
      moveto(POS_REJECT, o);
      check_status(o);
      if (debug)
         warnx("Rejected");
   }


   ajl_t i = ajl_read_func(ss_read_func, ss);
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
      {
         if (j_isobject(cmd))
            set_settings(cmd, o);
         j_t j = j_new();
         get_settings(j, o);
         client_tx(j, o);
      }
      if ((cmd = j_find(j, "mag")))
      {
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
            // 07 is JIS
            encode(0x16, j_index(cmd, 0));
            encode(0x24, j_index(cmd, 1));
            encode(0x34, j_index(cmd, 2));
         }
         if (c)
         {
            status = "Encoding";
            client_tx(j_new(), o);
            moveto(POS_MAG, o);
            check_position(o);
            printer_start_cmd(0x09000000 + ((p + 2) << 16) + c);
            printer_data(p, temp);
            printer_tx_check(o);
            status = "Encoded";
            client_tx(j_new(), o);
         }
         if (j_isnull(cmd) || j_istrue(cmd))
         {                      // Read
            status = "Reading";
            client_tx(j_new(), o);
            moveto(POS_MAG, o);
            check_position(o);
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
            client_tx(j, o);
         }
      }
      if ((cmd = j_find(j, "ic")))
      {
         moveto(POS_IC, o);
         if (j_isstring(cmd))
         {
            unsigned char *tx = NULL;
            int txlen = j_base16d(j_val(cmd), &tx);;
            unsigned char rx[256];
            DWORD rxlen = sizeof(rx);
            card_txn(txlen, tx, &rxlen, rx);
            j_t j = j_new();
            j_store_string(j, "ic", j_base16(rxlen, rx));
            client_tx(j, o);
         }
      }
      if ((cmd = j_find(j, "rfid")))
      {
         moveto(POS_RFID, o);
         if (j_isstring(cmd))
         {
            unsigned char *tx = NULL;
            int txlen = j_base16d(j_val(cmd), &tx);;
            unsigned char rx[256];
            DWORD rxlen = sizeof(rx);
            card_txn(txlen, tx, &rxlen, rx);
            j_t j = j_new();
            j_store_string(j, "rfid", j_base16(rxlen, rx));
            client_tx(j, o);
         }
      }
      if ((cmd = j_find(j, "mifare")))
      {
         moveto(POS_RFID, o);
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
            const char *add(const char *tag1, const char *tag2, int layer) {
               if (error)
                  return error;
               const char *d = j_get(panel, tag1);
               if (!d)
                  d = j_get(panel, tag2);
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
                     warnx("PNG %s%d:%ux%u (%+d/%+d) card %d/%d", tag1, side, width, height, dx, dy, cols, rows);
                  png_set_expand(png_ptr);      // Expand palette, etc
                  png_set_strip_16(png_ptr);    // Reduce to 8 bit
                  png_set_packing(png_ptr);     // Unpack
                  if (layer)
                     png_set_rgb_to_gray(png_ptr, 1, 54.0 / 256, 183.0 / 256);
                  else
                     png_set_gray_to_rgb(png_ptr);      // RGB
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
                  {             // CMY
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
                        {       // Blank
                           free(data[layer]);
                           data[layer] = NULL;
                        } else
                           found |= (1 << layer);
                     }
                     free(image);
                  } else
                  {             // K or U
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
                                    data[layer][o] = ((image[c] & 0x80) ? 0 : 0xFF);    // Black
                                 else
                                    data[layer][o] = image[c] ^ 0xFF;
                              }
                           }
                     }
                     int z;
                     for (z = 0; z < rows * cols && !data[layer][z]; z++);
                     if (z == rows * cols)
                     {          // Blank
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
            add("C", "CMY", 0);
            add("K", "K", 3);
            add("P", "PO", 5);
            add("U", "UV", 6);
            if (found)
            {
               moveto(POS_PRINT, o);    // ready to print
               if (side)
               {
                  status = "Second side";
                  client_tx(j_new(), o);
                  printer_queue_cmd(printed ? 0x07021000 : 0x05021000); // Retransfer and flip if printed, else just flip
               } else
               {
                  status = "First side";
                  client_tx(j_new(), o);
               }
               printed = 0;
               for (int p = 0; p < 8; p++)
                  if ((p < 3 && (found & 7)) || (found & (1 << p)))
                  {             // Send panel
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
                     while (queue > 3 && !error)
                        printer_rx_check(o);
                  }
               if (printed)
               {
                  if (j_test(panel, "uvsingle", 0))
                     printer_cmd(0x06020000 + printed, o);      // UV printed with rest, no special handling
                  else
                  {             // UV printed separately
                     if (printed & 0x0F)
                        printer_queue_cmd(0x06020000 + (printed & 0x0F));       // Non UV, if any
                     if (printed & 0x40)
                     {          // UV
                        if (printed & 0x0F)
                        {
                           status = "Printing";
                           client_tx(j_new(), o);
                           printer_queue_cmd(0x07020000);       // first transfer of non UV
                        }
                        status = "UV";
                        client_tx(j_new(), o);
                        printer_queue_cmd(0x06020000 + (printed & 0x40));       // UV print
                     }
                  }
               }
               check_status(o);
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
         while (queue && !error)
            printer_rx_check(o);
         if (printed)
         {
            status = "Transfer";
            client_tx(j_new(), o);
            printer_cmd(0x07020000 + (posn = POS_EJECT), o);
            status = "Printed";
            count++;
         } else
         {
            moveto(POS_EJECT, o);       // Done anyway
            status = "Unprinted";
         }
         check_status(o);
         check_position(o);
         break;
      } else if ((cmd = j_find(j, "reject")))
      {
         moveto(POS_REJECT, o);
         check_status(o);
         check_position(o);
         break;
      } else if ((cmd = j_find(j, "eject")))
      {
         moveto(POS_EJECT, o);
         check_status(o);
         check_position(o);
         break;
      }
      check_position(o);
      client_tx(j_new(), o);
   }
   j_delete(&j);
   if (!ers && error)
      ers = strdup(error);
   printer_disconnect();
   ajl_delete(&i);
   ajl_delete(&o);
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
         if (!er)
            er = job(from);
         if (debug)
            warnx("Finished %s: %s", from, er ? : "OK");
         if (er)
            free(er);
         SSL_shutdown(ss);
         SSL_free(ss);
         ss = NULL;
         return 0;
      }
      // Parent
      close(s);
      int pstatus = 0;
      waitpid(pid, &pstatus, 0);
      if (!WIFEXITED(pstatus) || WEXITSTATUS(pstatus))
         warnx("Job failed");
   }
   {
      int res;
      if ((res = SCardReleaseContext(cardctx)) != SCARD_S_SUCCESS)
         errx(1, "Cant release context (%s)", pcsc_stringify_error(res));
   }

   return 0;
}
