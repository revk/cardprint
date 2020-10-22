// MIFARE Classic test code
// Copyright © Adrian Kennard, Andrews & Arnold Ltd
// Intended to help us understand MIFARE Classic

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

int main(int argc, const char *argv[])
{
   int debug = 0;
   const char *reader = NULL;
   const char *cardwrite = NULL;
   const char *cardkey = "FFFFFFFFFFFF";
   int cardread = 0;
   int sector = -1;
   int block = -1;
   int keyb = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "reader", 'R', POPT_ARG_STRING, &reader, 0, "Which reader", "Name" },
         { "sector", 's', POPT_ARG_INT, &sector, 0, "Sector", "N" },
         { "block", 'b', POPT_ARG_INT, &block, 0, "Block", "N" },
         { "key", 'k', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &cardkey, 0, "Key", "Hex" },
         { "key-B", 'B', POPT_ARG_NONE, &keyb, 0, "KeyB" },
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
      if (poptPeekArg(optCon) || (!cardread && !cardwrite) || sector < 0 || block < 0)
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
      return rx[rxlen-2];
   }

   // Set key
   unsigned char *data;
   ssize_t l = j_based(cardkey, &data, JBASE16, 4);
   if (l < 6 || l > 16)
      errx(1, "Bad key %s", cardkey);
   txlen = 0;
   tx[txlen++] = 0xFF;          // Pseudo ADPU
   tx[txlen++] = 0x82;          // Load key
   tx[txlen++] = 0x20;          // Key structure (reader/card, secure/plain, non-vol/vol, RFU, 4 bit reader key)
   tx[txlen++] = 0x00;          // Key number
   tx[txlen++] = l;
   memcpy(tx + txlen, data, l);
   txlen += l;
   free(data);
   if ((txn() >> 4) != 9)
      errx(1, "Failed key set %02X %02X", rx[0], rx[1]);
   // 69 82=NS card key, 83=NS reader key, 84=NS plain, 85=NS secure, 86=NS vol, 87=NS non=vol, 88=key number bad, 89=key len bad
   txlen = 0;
   tx[txlen++] = 0xFF;          // Pseudo ADPU
   tx[txlen++] = 0x86;          // General authenticate
   tx[txlen++] = 0x00;
   tx[txlen++] = 0x00;
   tx[txlen++] = 0x05;          // Len
   tx[txlen++] = 0x01;          // Version 1
   tx[txlen++] = (sector >> 2); // Address
   tx[txlen++] = (sector << 2) + 3;     // 
   tx[txlen++] = (keyb ? 0x61 : 0x60);  // Key type (0x60 Mifare key A, 0x61 Mifare key B)
   tx[txlen++] = 0x00;          // Key number
   if ((txn() >> 4) != 9)
      errx(1, "Failed auth %02X %02X", rx[0], rx[1]);

   if (cardwrite)
   {
      l = j_based(cardwrite, &data, JBASE16, 4);
      if (l < 0 || l > 16)
         errx(1, "Bad write data");
      txlen = 0;
      tx[txlen++] = 0xFF;       // Pseudo ADPU
      tx[txlen++] = 0xD6;       // Write binary
      tx[txlen++] = (sector >> 2);      // Address
      tx[txlen++] = (sector << 2) + (block & 3);        //
      tx[txlen++] = l;          // Len
      memcpy(tx + txlen, data, l);
      txlen += l;
      free(data);
      // 62 81=corrupted, 82=end of file, 65 81=mem fail, 69 81=incompatible, 82=bad security, 86=not allowed, 6A 81=NS, 82=Not found
      if ((txn() >> 4) != 9)
         errx(1, "Failed write %02X %02X", rx[0], rx[1]);
   }
   if (cardread)
   {
      txlen = 0;
      tx[txlen++] = 0xFF;       // Pseudo ADPU
      tx[txlen++] = 0xB0;       // Write binary
      tx[txlen++] = (sector >> 2);      // Address
      tx[txlen++] = (sector << 2) + (block & 3);        //
      tx[txlen++] = 0x10;       // Len
      // How... 62 81=corrupted, 82=end of file, 69 81=incompatible, 82=bad security, 86=not allowed, 6C XX wrong len - should be XX
      txn();
      if ((txn() >> 4) != 9)
         errx(1, "Failed read %02X %02X", rx[0], rx[1]);
      printf("%s\n", j_base16(rxlen - 2, rx));
   }

   if ((res = SCardReleaseContext(cardctx)) != SCARD_S_SUCCESS)
      errx(1, "Cant release context: %s", pcsc_stringify_error(res));
   return 0;
}
