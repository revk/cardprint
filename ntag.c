// NTAG test code
// Copyright © Adrian Kennard, Andrews & Arnold Ltd
// Intended to help us understand NTAG cards

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
   int cardread = 0;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
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


   if ((res = SCardReleaseContext(cardctx)) != SCARD_S_SUCCESS)
      errx(1, "Cant release context: %s", pcsc_stringify_error(res));
   return 0;
}
