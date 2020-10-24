// NTAG test code
// Copyright © Adrian Kennard, Andrews & Arnold Ltd
// Intended to help us understand NTAG

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <winscard.h>
#include <err.h>
#include <ajl.h>

// Notes see https://pcscworkgroup.com/specifications/download/
// Notes on NDEF
// Data from block 4 is a tag value
// Tag 00 is nothing, FE if end, 03 is NDEF, followed by length and data, length 00-FE, of FFnnnn
// In the NDEF blocks are multiple related NDEF records

const char *prefix[] =
    { "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
"tcpobex://", "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:" };

int main(int argc, const char *argv[])
{
   int debug = 0;
   const char *reader = NULL;
   const char *cardwrite = NULL;
   const char *cardkey = "FFFFFFFFFFFF";
   int cardread = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "reader", 'R', POPT_ARG_STRING, &reader, 0, "Which reader", "Name" },
         { "key", 'k', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &cardkey, 0, "Key", "Hex" },
         { "write", 'w', POPT_ARG_STRING, &cardwrite, 0, "Write block", "Hex" },
         { "read", 'r', POPT_ARG_NONE, &cardread, 0, "Read block" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
         POPT_AUTOHELP { }
      };
      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");
      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
      if (poptPeekArg(optCon) || (!cardread && !cardwrite))
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      poptFreeContext(optCon);
   }
   SCARDCONTEXT cardctx;
   long res;

   if ((res = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &cardctx)) != SCARD_S_SUCCESS)
      errx(1, "Cannot get PCSC context, is pcscd running?");
   DWORD len;
   if ((res = SCardListReaders(cardctx, NULL, NULL, &len)) != SCARD_S_SUCCESS)
      errx(1, "Cannot get reader list: %s", pcsc_stringify_error(res));
   char *r = NULL;
   if (!(r = malloc(len)))
      errx(1, "malloc");
   if ((res = SCardListReaders(cardctx, NULL, r, &len)) != SCARD_S_SUCCESS)
      errx(1, "Cannot list readers: %s", pcsc_stringify_error(res));
   char *p = r,
       *e = r + len;
   while (*p && p < e)          // && !error)
   {
      if (!reader || strstr(p, reader))
         reader = strdup(p);
      p += strlen(p) + 1;
   }
   free(r);

   SCARDHANDLE card;
   BYTE atr[MAX_ATR_SIZE];
   DWORD atrlen;
   DWORD proto;
   if ((res = SCardConnect(cardctx, reader, SCARD_SHARE_EXCLUSIVE, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &card, &proto)) != SCARD_S_SUCCESS)
      errx(1, "Cannot connect to %s: %s", reader, pcsc_stringify_error(res));
   atrlen = sizeof(atr);
   DWORD state;
   DWORD temp;
   if ((res = SCardStatus(card, 0, &temp, &state, &proto, atr, &atrlen)) != SCARD_S_SUCCESS)
      errx(1, "Cannot get card status: %s", pcsc_stringify_error(res));
   if (debug)
      warnx("ATR %s", j_base16(atrlen, atr));

   DWORD txlen,
    rxlen;
   BYTE rx[256],
    tx[256];
   SCARD_IO_REQUEST recvpci;
   BYTE txn(void) {
      rxlen = sizeof(rx);
      if (debug)
         warnx("Tx %s", j_base16(txlen, tx));
      if ((res = SCardTransmit(card, SCARD_PCI_T1, tx, txlen, &recvpci, rx, &rxlen)) != SCARD_S_SUCCESS)
         errx(1, "Cannot set key: %s", pcsc_stringify_error(res));
      if (debug)
         warnx("Rx %s", j_base16(rxlen, rx));
      if (rxlen < 0)
         errx(1, "Bad rx");
      return rx[rxlen - 2];
   }
   if (cardwrite)
   {                            // Make it simple, assume we can fit this in small data
      unsigned char data[1000];
      int p = 0,
          l = strlen(cardwrite);;
      data[p++] = 0x03;         // NDEF
      p++;                      // skip length for now
      data[p++] = 0xD1;         // MB, ME, SR, Well-known
      data[p++] = 0x01;         // Type len
      int tag;
      for (tag = 0; tag < sizeof(prefix) / sizeof(*prefix) && strncmp(prefix[tag], cardwrite, strlen(prefix[tag])); tag++);
      if (tag < sizeof(prefix) / sizeof(*prefix))
         l = strlen(cardwrite += strlen(prefix[tag++]));
      else
         tag = 0;
      if (l + 1 > 255)
         errx(1, "Too long");
      data[p++] = l + 1;        // Payload len (1 byte prefix)
      data[p++] = 'U';          // Type URI
      data[p++] = tag;
      memcpy(data + p, cardwrite, l);
      p += l;
      if (p - 2 > 254)
         errx(1, "Too long");
      data[1] = p - 2;
      data[p++] = 0xFE;         // End
      int q = 0;
      while (q < p)
      {
         int d = p - q;
         if (d > 16)
            d = 16;
         txlen = 0;
         tx[txlen++] = 0xFF;    // Pseudo ADPU
         tx[txlen++] = 0xD6;    // Write binary
         tx[txlen++] = ((q + 16) >> 10);        // Address of 4 byte block
         tx[txlen++] = ((q + 16) >> 2);
         tx[txlen++] = 16;      // Len
         memcpy(tx + txlen, data + q, d);
         if (d < 16)
            memset(tx + txlen + d, 0, 16 - d);
         txlen += 16;
         if ((txn() >> 4) != 9)
            errx(1, "Failed write %02X %02X", rx[0], rx[1]);
         q += d;
      }
   }
   if (cardread)
   {
      txlen = 0;
      tx[txlen++] = 0xFF;       // Pseudo ADPU
      tx[txlen++] = 0xB0;       // Write binary
      tx[txlen++] = 0x00;       // Address
      tx[txlen++] = 0x00;
      tx[txlen++] = 0x00;       // Len (all)
      if ((txn() >> 4) != 9)
         errx(1, "Failed read %02X %02X", rx[0], rx[1]);
      printf("%s\n", j_base16(rxlen - 2, rx));
      tx[3] += 4;
      if ((txn() >> 4) != 9)
         errx(1, "Failed read %02X %02X", rx[0], rx[1]);
      printf("%s\n", j_base16(rxlen - 2, rx));
      tx[3] += 4;
      if ((txn() >> 4) != 9)
         errx(1, "Failed read %02X %02X", rx[0], rx[1]);
      printf("%s\n", j_base16(rxlen - 2, rx));
   }

   if ((res = SCardReleaseContext(cardctx)) != SCARD_S_SUCCESS)
      errx(1, "Cant release context: %s", pcsc_stringify_error(res));
   return 0;
}
