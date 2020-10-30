// Test USB
// Copyright © Adrian Kennard, Andrews & Arnold Ltd

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>
#include <err.h>

// Notes
// SCSI Inquiry				Getting basic info
// 12 00 00 00 60 00			96 bytes (00 60)
// SCSI LogSense			?
// 4D 00 79 00 00 00 00 00 4C 00	76 bytes (00 4C)
// 4D 00 78 00 00 00 00 00 34 00	52 bytes (00 34)
// SCSI ModeSense			?
// 1A 00 68 00 40 00			64 bytes (00 40)
// 1A 00 63 00 2C 00			44 bytes (00 2C) includes string FC2221
// SCSI ReadBuffer
// 3C 02 70 00 00 00 00 00 06 00	6 bytes 90 3D 68 02 58 C3?
// SCSI WriteBuffer
// 3B 02 60 00 00 00 00 00 60 00
// Sends bulk 96 bytes, job unicode 4f0057004e00450052005f0054004f0044004f0000000000...
// SCSI PreFetch
// 34 00 00 00 00 00 00 00 00 00
// - then some unknown transfer FE? just once, maybe not related?
// SCSI RequestSense
// 03 00 00 00 14 00			? 20 bytes ?
// SCSI 0x31
// 31 01 01 00 04 00 00 00 00 00	Before the write?
// SCSI Write
// 2A 00 03 00 00 00 29 FC 80 00	LBA 03 00 00 00 len 29FC80
// 2A 00 02 00 00 00 29 FC 80 00	LBA 02 00 00 00 len 29FC80
// 2A 00 01 00 00 00 29 FC 80 00	LBA 01 00 00 00 len 29FC80
// Each 41*65536+64640 final block, total 2751616 which is image data
// SCSI 0x31
// 31 08 03 00 00 00 00 00 00 00
// SCSI Test unit ready
// 00 00 00 00 00 00
// response ends 01 (check condition)
// Several senses, response 70 00 02 00 00 00 00 0C 00 00 00 00 D4 00 00 00 00 00 00 00
// Then test unit ready response 0 (Good)
// SCSI 0x31
// 31 09 00 00 00 00 00 00 00 00
//
// The 0x31 seem to command, only 01 01 00 04, 08 03, 09
// Maybe 01 01 00 04 is load?
// Maybe 08 03 is print
// Maybe 09 00 is transfer / done
//
// From command test system :-
// Printer Status
// 12 00 00 00 60 00			done twice, returning 96 bytes with info
// 4D 00 79 00 00 00 00 00 4C 00	done once, returning 76 bytes
// 	390000480000000400000000001000040000009C00020004000000000003000400000F00000400040000009C000500040000000000060002FE000...
// 	390000480000000400000000000100040000009C000200040000000000030004FF02D100000400040000009C000500040000000000060002FE000... Door open 0x102D100
// 	390000480000000400000000000100040000009C00020004000000000003000407000F00000400040000009C000500040000000000060002FE000... Pre heating
// 	390000480000000400000000000100040000009C000200040000000000030004FF02D000000400040000009C000500040000000000060002FE000... No cards 0x102D000
// 
// 00 00 00 00 00 00 returns 00 good			Test unit ready
// Read position
// 34 00 00 00 00 00 00 00 00 00 (pre fetch)		Read position
//>04 00 00 00 00 00 00 00 no card
// 31 01 00 00 00 00 00 00 00 00			Card load (0)
// 31 0B 00 00 00 00 00 01 00 00			Card move (1)
// 31 0B 00 00 02 00 00 02 00 00			Card move (2) flip
// 31 0B 00 00 04 00 00 01 00 00			Card move (1) film-init
// 31 0B 01 00 00 00 00 01 00 00			Card move (1) immediate
// 32 00 00 00 00 00 00 00 00 00			Contact engage
// 32 01 00 00 00 00 00 00 00 00			Contact release
// 31 0B 01 00 06 00 00 02 00 00			Card move (2) immediate flip film-init
// 32 04 00 00 00 00 00 00 00 00			Non-contact engage
// 32 05 00 00 00 00 00 00 00 00			Non-contact release
// 31 0B 00 00 00 00 00 04 00 00			Card move (4)
// 31 01 00 00 02 00 00 02 00 00			Card load (2) flip
// 34 00 00 00 00 00 00 00 00 00			Read position
//>00 00 00 00 00 00 00 02				Position 2
// 31 01 01 00 00 00 00 03 00 00			Card load (3) immediate
// 2C 00 00 00 B4 00 00 25 00 00			Mag read track 2
//>B4 25 then data non ascii
// 2C 00 00 A6 B4 00 4C 25 00 00			Mag read track 1+2
//>A6 4C ... data B4 25 data...
// 2C 00 00 A6 B4 C4 4C 25 68 00			Mag Read ISO 1(6x76) 2(4x37) 3(4x104)
// 2E 00 00 00 00 07 00 00 45 00			Mag Read JIS (7x69)
// 2C 00 00 A7 B4 C7 45 25 45 00			Mag Read ISO 1(7x69) 2(4x37) 3(7x69)
// 2C 00 00 A8 B4 C7 4F 25 45 00			Mag Read ISO 1(6x79) 2(4x37) 3(7x69)
// 2D 00 00 A6 B4 00 00 00 75 00			Mag Write ISO 1+2 (note 75 is length of data sent)
//<A6 4C data B4 25 data
// 01 00 00 00 00 00					Re-init unit
// 31 08 3E 00 00 00 00 00 00 00			Print YMC+K+UV+PO buffer 0 upper right
// 31 08 3E 00 01 00 00 00 00 00			Print YMC+K+UV_PO buffer 1 upper right
// 31 08 5E 00 01 00 00 00 00 00			Print YMC+K+UV+PO buffer 1 lower left
// 31 08 02 00 01 00 00 00 00 00			Print YMC ^
// 31 08 04 00 01 00 00 00 00 00 			Print K ^
// 31 08 48 00 01 00 00 00 00 00			Print UV ^
// 31 08 10 00 01 00 00 00 00 00			Print PO ^
// 31 08 28 00 01 00 00 00 00 00			Print UV ^ upper right (so 08 is UV, and 20/40 is corner)
// 31 08 05 00 00 00 00 00 00 00			Print K, buffer 0, immediate
// 31 09 00 00 00 00 00 00 00 00			Transfer Eject
// 31 0A 00 00 00 00 00 00 00 00			Transfer Flip
// 31 0C 05 00 00 00 00 00 00 00			Print KSec (K, buffer 0, immediate)
// 31 0D 00 00 00 00 00 00 00 00			Transfer Return
// 31 09 01 00 00 00 00 00 00 00			Transfer Eject Immediate
// 


// Section 5.1: Command Block Wrapper (CBW)
struct command_block_wrapper {
   uint8_t dCBWSignature[4];
   uint32_t dCBWTag;
   uint32_t dCBWDataTransferLength;
   uint8_t bmCBWFlags;
   uint8_t bCBWLUN;
   uint8_t bCBWCBLength;
   uint8_t CBWCB[16];
};

// Section 5.2: Command Status Wrapper (CSW)
struct command_status_wrapper {
   uint8_t dCSWSignature[4];
   uint32_t dCSWTag;
   uint32_t dCSWDataResidue;
   uint8_t bCSWStatus;
};

int main(int argc, const char *argv[])
{
   int debug = 0;
   const char *printer = NULL;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         { "printer", 'P', POPT_ARG_STRING, &printer, 0, "Which printer", "Name" },
         { "debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug" },
         POPT_AUTOHELP { }
      };
      optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");
      int c;
      if ((c = poptGetNextOpt(optCon)) < -1)
         errx(1, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
      if (poptPeekArg(optCon) || !printer)
      {
         poptPrintUsage(optCon, stderr, 0);
         return -1;
      }
      poptFreeContext(optCon);
   }

   int r,
    cnt;
   libusb_device **devs;

   r = libusb_init(NULL);
   if (r < 0)
      return r;

   cnt = libusb_get_device_list(NULL, &devs);
   if (cnt < 0)
   {
      libusb_exit(NULL);
      return 1;
   }
   if (debug);
   warnx("%d devices", cnt);
   int printdev = -1;

   for (int i = 0; devs[i]; i++)
   {
      int ret;
      struct libusb_device_descriptor desc;
      unsigned char string[256];
      ret = libusb_get_device_descriptor(devs[i], &desc);
      libusb_device_handle *handle = NULL;
      libusb_open(devs[i], &handle);
      if (!handle)
         continue;
      if (handle)
      {
         if (desc.iManufacturer && debug)
         {
            ret = libusb_get_string_descriptor_ascii(handle, desc.iManufacturer, string, sizeof(string));
            if (ret > 0)
               printf("  Manufacturer:              %s\n", (char *) string);
         }

         if (desc.iProduct && debug)
         {
            ret = libusb_get_string_descriptor_ascii(handle, desc.iProduct, string, sizeof(string));
            if (ret > 0)
            {
               printf("  Product:                   %s\n", (char *) string);
               if (!strcmp((char *) string, printer))
                  printdev = i;
            }
         }

         if (desc.iSerialNumber && debug)
         {
            ret = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, string, sizeof(string));
            if (ret > 0)
               printf("  Serial Number:             %s\n", (char *) string);
         }
      }
      libusb_close(handle);
   }
   if (printdev < 0)
      errx(1, "Printer not found");
   libusb_device_handle *handle = NULL;
   libusb_open(devs[printdev], &handle);
   if (!handle)
      errx(1, "Did not open");

   r = libusb_set_auto_detach_kernel_driver(handle, 1);
   if (r < 0)
      errx(1, "   detach: %s", libusb_strerror((enum libusb_error) r));
#if 1
   r = libusb_reset_device(handle);
   if (r < 0)
      errx(1, "   reset: %s", libusb_strerror((enum libusb_error) r));
   sleep(1);
#endif
   r = libusb_set_configuration(handle, 1);
   if (r < 0)
      errx(1, "   config: %s", libusb_strerror((enum libusb_error) r));
   r = libusb_claim_interface(handle, 0);
   if (r < 0)
      errx(1, "   claim: %s", libusb_strerror((enum libusb_error) r));

#define RETRY_MAX                     5
#define BOMS_GET_MAX_LUN              0xFE
#define	IN 0x81
#define OUT 0x02

   uint8_t tag = 0;
#if 0
   uint8_t lun;
   r = libusb_control_transfer(handle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE, BOMS_GET_MAX_LUN, 0, 0, &lun, 1, 1000);
   if (r < 0)
      errx(1, "   Failed: %s", libusb_strerror((enum libusb_error) r));
   warnx("lun=%d", lun);
#endif

   int len = 96;
   struct command_block_wrapper cbw;
   memset(&cbw, 0, sizeof(cbw));
   cbw.dCBWSignature[0] = 'U';
   cbw.dCBWSignature[1] = 'S';
   cbw.dCBWSignature[2] = 'B';
   cbw.dCBWSignature[3] = 'C';
   cbw.dCBWTag = tag++;
   cbw.dCBWDataTransferLength = len;
   cbw.bmCBWFlags = LIBUSB_ENDPOINT_IN;
   cbw.bCBWLUN = 0;
   cbw.bCBWCBLength = 6;
   cbw.CBWCB[0] = 0x12;         // Op code Inquiry
   cbw.CBWCB[4] = len;          // Len
   for(int q=0;q<sizeof(cbw);q++)fprintf(stderr,"%02X ",((unsigned char *)&cbw)[q]);
   int i = 0,
       size = 0;
   do
   {
      // The transfer length must always be exactly 31 bytes.
      r = libusb_bulk_transfer(handle, OUT, (unsigned char *) &cbw, 31, &size, 1000);
      if (!r)
         break;
      if (r == LIBUSB_ERROR_PIPE)
         libusb_clear_halt(handle, OUT);
      i++;
      usleep(100000);
   } while ((r == LIBUSB_ERROR_PIPE) && (i < RETRY_MAX));
   if (r != LIBUSB_SUCCESS)
      errx(1, "   send_mass_storage_command: %s\n", libusb_strerror((enum libusb_error) r));
   warnx("Sent %d", size);

   uint8_t buf[len];
   r = libusb_bulk_transfer(handle, IN, (unsigned char *) buf, len, &size, 1000);
   if (r < 0)
      errx(1, "libusb_bulk_transfer failed: %s\n", libusb_error_name(r));
   for(int q=0;q<size;q++)fprintf(stderr,"%02X ",buf[q]);
   for(int q=0;q<size;q++)fputc(buf[q]<' '?' ':buf[q],stderr);
   warnx("size=%d", size);

   struct command_status_wrapper csw;
   i = 0;
   do
   {
      r = libusb_bulk_transfer(handle, IN, (unsigned char *) &csw, 13, &size, 1000);
      if(!r)break;
      if (r == LIBUSB_ERROR_PIPE)
         libusb_clear_halt(handle, IN);
      i++;
      usleep(100000);
   } while ((r == LIBUSB_ERROR_PIPE) && (i < RETRY_MAX));
   if (r != LIBUSB_SUCCESS)
   {
      errx(1, "   get_mass_storage_status: %s\n", libusb_strerror((enum libusb_error) r));
      return -1;
   }
   if (size != 13)
      errx(1, "   get_mass_storage_status: received %d bytes (expected 13)\n", size);


   libusb_release_interface(handle, 0);
   libusb_close(handle);
   libusb_free_device_list(devs, 1);
   libusb_exit(NULL);
   return 0;
}
