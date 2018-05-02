/*
 * ALSA parameter test program
 * 
 * Copyright (c) 2007 Volker Schatz (alsacap at the domain volkerschatz.com)
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies. 
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *
 * This program was originally written by Volker Schatz.  Shawn Wilson
 * bundled it into an autotools package and cut and pasted some code 
 * from the alsa speaker-test command to display min/max buffer and 
 * period sizes.
 * 
 */


/*============================================================================
            Includes
============================================================================*/

#include <stdlib.h>
#include <stdio.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>


/*============================================================================
         Constant and type definitions
============================================================================*/

#define RATE_KHZ_LIMIT   200

#define HWP_END      0
#define HWP_RATE   1
#define HWP_NCH      2
#define HWP_FORMAT   3
#define SIZE_HWP   7

typedef struct {
  int recdevices, verbose, card, dev;
  char *device;
  int hwparams[SIZE_HWP];
}
aiopts;


/*============================================================================
         Global variables
============================================================================*/

static snd_ctl_t *handle= NULL;
static snd_pcm_t *pcm= NULL;
static snd_ctl_card_info_t *info;
static snd_pcm_info_t *pcminfo;
static snd_pcm_hw_params_t *pars;
static snd_pcm_format_mask_t *fmask;


/*============================================================================
         Prototypes
============================================================================*/

void usagemsg(int code);
void errnumarg(char optchar);
void errarg(char optchar);
void errtoomany();

void scancards(snd_pcm_stream_t stream, int thecard, int thedev);
int sc_errcheck(int retval, const char *doingwhat, int cardnr, int devnr);

void testconfig(snd_pcm_stream_t stream, const char *device, const int *hwpars);
void tc_errcheck(int retval, const char *doingwhat);

const char *alsaerrstr(const int errcode);
const char *dirstr(int dir);

int parse_alsaformat(const char *fmtstr);
const char *alsafmtstr(int fmtnum);

void printfmtmask(const snd_pcm_format_mask_t *fmask);


/*============================================================================
            Main program
============================================================================*/

int main(int argc, char **argv)
{
  aiopts options= { 0, 1, -1, -1, NULL };
  snd_pcm_stream_t stream;
  char *argpar;
  int argind, hwpind;

  snd_ctl_card_info_alloca(&info);
  snd_pcm_info_alloca(&pcminfo);
  snd_pcm_hw_params_alloca(&pars);
  snd_pcm_format_mask_alloca(&fmask);

  hwpind= 0;
  for( argind= 1; argind< argc; ++argind )
  {
    if( argv[argind][0]!='-' ) {
      fprintf(stderr, "Unrecognised command-line argument `%s'.\n", argv[argind]);
      usagemsg(1);
    }
    if( argv[argind][2] )   argpar= argv[argind]+2;
    else {
      if( argind+1 >= argc ) argpar= NULL;
      else argpar= argv[argind+1];
    }
    if( argv[argind][1]=='h' || !strcmp(argv[argind]+1, "-help") )
      usagemsg(0);
    else if( argv[argind][1]=='R' ) {
      options.recdevices= 1;
      argpar= NULL;   // set to NULL if unused to keep track of next arg index
    }
    else if( argv[argind][1]=='C' ) {
      if( !argpar || !isdigit(*argpar) ) errnumarg('C');
      options.card= strtol(argpar, NULL, 0);
    }
    else if( argv[argind][1]=='D' ) {
      if( !argpar || !isdigit(*argpar) ) errnumarg('D');
      options.dev= strtol(argpar, NULL, 0);
    }
    else if( argv[argind][1]=='d' ) {
      if( !argpar )   errarg('d');
      options.device= argpar;
    }
    else if( argv[argind][1]=='r' ) {
      if( !argpar || !isdigit(*argpar) ) errnumarg('r');
      if( hwpind+3 > SIZE_HWP ) errtoomany();
      options.hwparams[hwpind++]= HWP_RATE;
      options.hwparams[hwpind]= strtol(argpar, NULL, 0);
      if( options.hwparams[hwpind] <= RATE_KHZ_LIMIT )
   options.hwparams[hwpind] *= 1000;         // sanity check: Hz or kHz ?
      ++hwpind;
    }
    else if( argv[argind][1]=='c' ) {
      if( !argpar || !isdigit(*argpar) ) errnumarg('c');
      if( hwpind+3 > SIZE_HWP ) errtoomany();
      options.hwparams[hwpind++]= HWP_NCH;
      options.hwparams[hwpind++]= strtol(argpar, NULL, 0);
    }
    else if( argv[argind][1]=='f' ) {
      if( !argpar ) errarg('f');
      if( hwpind+3 > SIZE_HWP ) errtoomany();
      options.hwparams[hwpind++]= HWP_FORMAT;
      options.hwparams[hwpind++]= parse_alsaformat(argpar);
    }
    else {
      fprintf(stderr, "Unrecognised command-line option `%s'.\n", argv[argind]);
      usagemsg(1);
    }
    if( argpar && !argv[argind][2] )
      ++argind;  // additional increment if separate parameter argument was used
  }
  options.hwparams[hwpind]= HWP_END;
  if( options.dev >= 0 && options.card < 0 ) {
    fprintf(stderr, "The card has to be specified with -C if a device number is given (-D).\n");
    exit(1);
  }
  if( options.device && (options.card>=0 || options.dev>=0) ) {
    fprintf(stderr, "Specifying a device name (-d) and a card and possibly device number (-C, -D) is mutually exclusive.\n");
    exit(1);
  }
  stream= options.recdevices? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;

  if( !options.device )
    scancards(stream, options.card, options.dev);
  else
    testconfig(stream, options.device, options.hwparams);

}



/*============================================================================
   Usage message and command-line argument error functions
============================================================================*/

void usagemsg(int code)
{
  fprintf(stderr, "Usage: alsacap [-R] [-C <card #> [-D <device #>]]\n"
        "       alsacap [-R] -d <device name> [-r <rate>|-c <# of channels>|-f <sample format>]...\n"
      "ALSA capability lister.\n"
      "First form: Scans one or all soundcards known to ALSA for devices, \n"
      "subdevices and parameter ranges.  -R causes a scan for recording\n"
      "rather than playback devices.  The other options specify the sound\n"
      "card and possibly the device by number.\n"
      "Second form: Displays ranges of configuration parameters for the given\n"
      "ALSA device.  Unlike with the first form, a non-hardware device may be\n"
      "given.  Up to three optional command-line arguments fix the rate,\n"
      "number of channels and sample format in the order in which they are\n"
      "given.  The remaining parameter ranges are output.  If unique, the\n"
      "number of significant bits of the sample values is output.  (Some\n"
      "sound cards ignore some of the bits.)\n");
  exit(code);
}

void errnumarg(char optchar)
{
  fprintf(stderr, "The -%c option requires a numerical argument!  Aborting.\n", optchar);
  exit(1);
}

void errarg(char optchar)
{
  fprintf(stderr, "The -%c option requires an argument!  Aborting.\n", optchar);
  exit(1);
}

void errtoomany()
{
  fprintf(stderr, "Too many -r/-c/-f options given!  (Maximum is %d.)  Aborting.\n", (SIZE_HWP-1)/2);
  exit(1);
}


/*============================================================================
      Function for scanning all cards
============================================================================*/

#define HWCARDTEMPL   "hw:%d"
#define HWDEVTEMPL   "hw:%d,%d"
#define HWDEVLEN   32

void scancards(snd_pcm_stream_t stream, int thecard, int thedev)
{
  char hwdev[HWDEVLEN+1];
  unsigned min, max;
  int card, err, dev, subd, nsubd;
  snd_pcm_uframes_t     period_size_min;
  snd_pcm_uframes_t     period_size_max;
  snd_pcm_uframes_t     buffer_size_min;
  snd_pcm_uframes_t     buffer_size_max;


  printf("*** Scanning for %s devices", 
         stream == SND_PCM_STREAM_CAPTURE? "recording" : "playback");
  if( thecard >= 0 )
     printf(" on card %d", thecard);
  if( thedev >= 0 )
     printf(", device %d", thedev);
  printf(" ***\n");

  hwdev[HWDEVLEN]= 0;

  if( thecard >= 0 )
     card= thecard;
  else {
     card= -1;
     if( snd_card_next(&card) < 0 )
        return;
  }


  while( card >= 0 )
  {
    snprintf(hwdev, HWDEVLEN, HWCARDTEMPL, card);
    err= snd_ctl_open(&handle, hwdev, 0);
    if( sc_errcheck(err, "opening control interface", card, -1) ) goto nextcard;
    err= snd_ctl_card_info(handle, info);
    if( sc_errcheck(err, "obtaining card info", card, -1) ) {
      snd_ctl_close(handle);
      goto nextcard;
    }
    printf("\nCard %d, ID `%s', name `%s'\n", 
           card, snd_ctl_card_info_get_id(info),
           snd_ctl_card_info_get_name(info));
    if( thedev >= 0 )
       dev= thedev;
    else {
       dev= -1;
       if( snd_ctl_pcm_next_device(handle, &dev) < 0 ) {
          snd_ctl_close(handle);
          goto nextcard;
      }
    }
    while( dev >= 0 )
    {
      snd_pcm_info_set_device(pcminfo, dev);
      snd_pcm_info_set_subdevice(pcminfo, 0);
      snd_pcm_info_set_stream(pcminfo, stream);
      err= snd_ctl_pcm_info(handle, pcminfo);

      if( thedev<0 && err == -ENOENT ) 
         goto nextdev;
      if( sc_errcheck(err, "obtaining device info", card, dev) ) 
         goto nextdev;
      nsubd= snd_pcm_info_get_subdevices_count(pcminfo);
      if( sc_errcheck(nsubd, "obtaining device info", card, dev) ) 
         goto nextdev;

      printf(
         "  Device %d, ID `%s', name `%s', %d subdevices (%d available)\n",
         dev, snd_pcm_info_get_id(pcminfo), snd_pcm_info_get_name(pcminfo),
         nsubd, snd_pcm_info_get_subdevices_avail(pcminfo));
      snprintf(hwdev, HWDEVLEN, HWDEVTEMPL, card, dev);

      err= snd_pcm_open(&pcm, hwdev, stream, SND_PCM_NONBLOCK);
      if( sc_errcheck(err, "opening sound device", card, dev) ) goto nextdev;
      err= snd_pcm_hw_params_any(pcm, pars);
      if( sc_errcheck(err, "obtaining hardware parameters", card, dev) ) {
         snd_pcm_close(pcm);
         goto nextdev;
      }
      snd_pcm_hw_params_get_channels_min(pars, &min);
      snd_pcm_hw_params_get_channels_max(pars, &max);
      if( min == max )
         if( min == 1 )   
            printf("    1 channel, ");
         else      
            printf("    %d channels, ", min);
      else      
         printf("    %u..%u channels, ", min, max);

      /* Find and print out min/max sampling rates. */
      snd_pcm_hw_params_get_rate_min(pars, &min, NULL);
      snd_pcm_hw_params_get_rate_max(pars, &max, NULL);
      printf("sampling rate %u..%u Hz\n", min, max);

      /* Find and print out possible PCM formats. */
      snd_pcm_hw_params_get_format_mask(pars, fmask);
      printf("    Sample formats: ");
      printfmtmask(fmask);
      printf("\n");

      /* Find and print out min/max buffer and period sizes. */
      err = snd_pcm_hw_params_get_buffer_size_min(
         pars, &buffer_size_min);
      err = snd_pcm_hw_params_get_buffer_size_max( 
         pars, &buffer_size_max);
      err = snd_pcm_hw_params_get_period_size_min(
         pars, &period_size_min, NULL);
      err = snd_pcm_hw_params_get_period_size_max(
         pars, &period_size_max, NULL);

      printf("    Buffer size range from %lu to %lu\n",
             buffer_size_min, buffer_size_max);
      printf("    Period size range from %lu to %lu\n",
             period_size_min, period_size_max);


      snd_pcm_close(pcm);
      pcm= NULL;

      for( subd= 0; subd< nsubd; ++subd ) {
         snd_pcm_info_set_subdevice(pcminfo, subd);
         err= snd_ctl_pcm_info(handle, pcminfo);
         if( sc_errcheck(err, "obtaining subdevice info", card, dev) ) 
            goto nextdev;

         printf("      Subdevice %d, name `%s'\n", 
                subd, snd_pcm_info_get_subdevice_name(pcminfo));
      }

      nextdev:
            if( thedev >= 0 || snd_ctl_pcm_next_device(handle, &dev) < 0 )
               break;
    }
    snd_ctl_close(handle);
    nextcard:
        if( thecard >= 0 || snd_card_next(&card) < 0 )
           break;
  }
}


int sc_errcheck(int retval, const char *doingwhat, int cardnr, int devnr)
{
  if( retval<0 ) {
    if( devnr>= 0 )
      fprintf(stderr, "Error %s for card %d, device %d: %s.  Skipping.\n", doingwhat, cardnr, devnr, alsaerrstr(retval));
    else
      fprintf(stderr, "Error %s for card %d: %s.  Skipping.\n", doingwhat, cardnr, alsaerrstr(retval));
    return 1;
  }
  return 0;
}



/*============================================================================
   Function for investigating device configurations
============================================================================*/

void testconfig(snd_pcm_stream_t stream, const char *device, const int *hwpars)
{
  unsigned min, max, param;
  int err, count, dir, result;
  snd_pcm_uframes_t     period_size_min;
  snd_pcm_uframes_t     period_size_max;
  snd_pcm_uframes_t     buffer_size_min;
  snd_pcm_uframes_t     buffer_size_max;

  printf("*** Exploring configuration space of device `%s' for %s ***\n", device,
     stream==SND_PCM_STREAM_CAPTURE? "recording" : "playback");
  err= snd_pcm_open(&pcm, device, stream, SND_PCM_NONBLOCK);
  tc_errcheck(err, "opening sound device");
  err= snd_pcm_hw_params_any(pcm, pars);
  tc_errcheck(err, "initialising hardware parameters");
  for( count= 0; hwpars[count]!=HWP_END; count += 2 )
    
    switch(hwpars[count])
    {
      case HWP_RATE:param= hwpars[count+1];
                err= snd_pcm_hw_params_set_rate_near(pcm, pars, &param, &result);
                if( err<0 )
            fprintf(stderr, "Could not set sampling rate to %d Hz: %s.  "
        "Continuing regardless.\n", hwpars[count+1], alsaerrstr(err));
          else
            printf("Set sampling rate %d Hz --> got %u Hz, %s requested.\n", hwpars[count+1], param, dirstr(dir));
        break;
      case HWP_NCH:err= snd_pcm_hw_params_set_channels(pcm, pars, hwpars[count+1]);
               if( err<0 )
           fprintf(stderr, "Could not set # of channels to %d: %s.  "
        "Continuing regardless.\n", hwpars[count+1], alsaerrstr(err));
          else
            printf("Set number of channels to %d.\n", hwpars[count+1]);
        break;
      case HWP_FORMAT:err= snd_pcm_hw_params_set_format(pcm, pars, hwpars[count+1]);
            if( err<0 )
              fprintf(stderr, "Could not set sample format to %s: %s."
       "  Continuing regardless.\n", alsafmtstr(hwpars[count+1]), alsaerrstr(err));
             else
           printf("Set sample format to %s.\n", alsafmtstr(hwpars[count+1]));
        break;
      default:
        break;
    }
  if( count>0 )
    printf("Parameter ranges remaining after these settings:\n");
  snd_pcm_hw_params_get_channels_min(pars, &min);
  snd_pcm_hw_params_get_channels_max(pars, &max);
  if( min==max )
    if( min==1 )
      printf("1 channel\n");
    else
      printf("%u channels\n", min);
  else
    printf("%u..%u channels\n", min, max);
  snd_pcm_hw_params_get_rate_min(pars, &min, NULL);
  snd_pcm_hw_params_get_rate_max(pars, &max, NULL);
  if( min==max )
    printf("Sampling rate %u Hz\n", min);
  else
    printf("Sampling rate %u..%u Hz\n", min, max);

  /* Find and print out possible PCM formats. */
  snd_pcm_hw_params_get_format_mask(pars, fmask);
  printf("    Sample formats: ");
  printfmtmask(fmask);
  printf("\n");

  /* Find and print out min/max buffer and period sizes. */
  err = snd_pcm_hw_params_get_buffer_size_min(
     pars, &buffer_size_min);
  err = snd_pcm_hw_params_get_buffer_size_max( 
     pars, &buffer_size_max);
  err = snd_pcm_hw_params_get_period_size_min(
     pars, &period_size_min, NULL);
  err = snd_pcm_hw_params_get_period_size_max(
     pars, &period_size_max, NULL);

  printf("    Buffer size range from %lu to %lu\n",
         buffer_size_min, buffer_size_max);
  printf("    Period size range from %lu to %lu\n",
         period_size_min, period_size_max);

  result= snd_pcm_hw_params_get_sbits(pars);
  if( result >= 0 )    // only available if bit width of all formats is the same
    printf("Significant bits: %d\n", result);
  snd_pcm_close(pcm);
}


void tc_errcheck(int retval, const char *doingwhat)
{
  if( retval<0 ) {
    fprintf(stderr, "Error %s: %s.  Aborting.\n", doingwhat, alsaerrstr(retval));
    if( pcm )
      snd_pcm_close(pcm);
    exit(1);
  }
}


/*============================================================================
         String-building functions
============================================================================*/

struct alsaerr { int err; char *msg; };
struct alsaerr aelist[]= {
  -EBADFD, "PCM device is in a bad state",
  -EPIPE, "An underrun occurred",
  -ESTRPIPE, "A suspend event occurred",
  -ENOTTY, "Hotplug device has been removed",
  -ENODEV, "Hotplug device has been removed",
  -ENOENT, "Device does not exist",
  0, NULL
};
const char *alsaerrstr(const int errcode)
{
  struct alsaerr *search;

  if( errcode >= 0 )
    return "No error";
  for( search= aelist; search->msg && search->err!=errcode; ++search);
  if( search->msg )
    return search->msg;
  else
    return strerror(-errcode);
}


const char *dirstr(int dir)
{
  if( !dir )
    return "=";
  else if( dir<0 )
    return "<";
  else
    return ">";
}


/*============================================================================
      Functions for parsing and string output of ALSA sample formats
============================================================================*/

struct fmtdef { char *fmtname; int format; };
static struct fmtdef fmtlist[]= {
  "S8", SND_PCM_FORMAT_S8,
  "U8", SND_PCM_FORMAT_U8,
  "S16_LE", SND_PCM_FORMAT_S16_LE,
  "S16_BE", SND_PCM_FORMAT_S16_BE,
  "U16_LE", SND_PCM_FORMAT_U16_LE,
  "U16_BE", SND_PCM_FORMAT_U16_BE,
  "S24_LE", SND_PCM_FORMAT_S24_LE,
  "S24_BE", SND_PCM_FORMAT_S24_BE,
  "U24_LE", SND_PCM_FORMAT_U24_LE,
  "U24_BE", SND_PCM_FORMAT_U24_BE,
  "S32_LE", SND_PCM_FORMAT_S32_LE,
  "S32_BE", SND_PCM_FORMAT_S32_BE,
  "U32_LE", SND_PCM_FORMAT_U32_LE,
  "U32_BE", SND_PCM_FORMAT_U32_BE,
  "FLOAT_LE", SND_PCM_FORMAT_FLOAT_LE,
  "FLOAT_BE", SND_PCM_FORMAT_FLOAT_BE,
  "FLOAT64_LE", SND_PCM_FORMAT_FLOAT64_LE,
  "FLOAT64_BE", SND_PCM_FORMAT_FLOAT64_BE,
  "IEC958_SUBFRAME_LE", SND_PCM_FORMAT_IEC958_SUBFRAME_LE,
  "IEC958_SUBFRAME_BE", SND_PCM_FORMAT_IEC958_SUBFRAME_BE,
  "MU_LAW", SND_PCM_FORMAT_MU_LAW,
  "A_LAW", SND_PCM_FORMAT_A_LAW,
  "IMA_ADPCM", SND_PCM_FORMAT_IMA_ADPCM,
  "MPEG", SND_PCM_FORMAT_MPEG,
  "GSM", SND_PCM_FORMAT_GSM,
  "SPECIAL", SND_PCM_FORMAT_SPECIAL,
  "S24_3LE", SND_PCM_FORMAT_S24_3LE,
  "S24_3BE", SND_PCM_FORMAT_S24_3BE,
  "U24_3LE", SND_PCM_FORMAT_U24_3LE,
  "U24_3BE", SND_PCM_FORMAT_U24_3BE,
  "S20_3LE", SND_PCM_FORMAT_S20_3LE,
  "S20_3BE", SND_PCM_FORMAT_S20_3BE,
  "U20_3LE", SND_PCM_FORMAT_U20_3LE,
  "U20_3BE", SND_PCM_FORMAT_U20_3BE,
  "S18_3LE", SND_PCM_FORMAT_S18_3LE,
  "S18_3BE", SND_PCM_FORMAT_S18_3BE,
  "U18_3LE", SND_PCM_FORMAT_U18_3LE,
  "U18_3BE", SND_PCM_FORMAT_U18_3BE,
  "S16", SND_PCM_FORMAT_S16,
  "U16", SND_PCM_FORMAT_U16,
  "S24", SND_PCM_FORMAT_S24,
  "U24", SND_PCM_FORMAT_U24,
  "S32", SND_PCM_FORMAT_S32,
  "U32", SND_PCM_FORMAT_U32,
  "FLOAT", SND_PCM_FORMAT_FLOAT,
  "FLOAT64", SND_PCM_FORMAT_FLOAT64,
  "IEC958_SUBFRAME", SND_PCM_FORMAT_IEC958_SUBFRAME,
  NULL, 0
};

int parse_alsaformat(const char *fmtstr)
{
  struct fmtdef *search;

  for( search= fmtlist; search->fmtname && strcmp(search->fmtname, fmtstr); ++search );
  if( !search->fmtname ) {
    fprintf(stderr, "Unknown sample format `%s'.  Aborting.\n", fmtstr);
    exit(1);
  }
  return search->format;
}

const char *alsafmtstr(int fmtnum)
{
  struct fmtdef *search;

  for( search= fmtlist; search->fmtname && search->format!=fmtnum; ++search );
  if( !search->fmtname )
    return "(unknown)";
  else
    return search->fmtname;
}


/*============================================================================
               Printout functions
============================================================================*/

void printfmtmask(const snd_pcm_format_mask_t *fmask)
{
  int fmt, prevformat= 0;

  for( fmt= 0; fmt <= SND_PCM_FORMAT_LAST; ++fmt )
    if( snd_pcm_format_mask_test(fmask, (snd_pcm_format_t)fmt) ) {
      if( prevformat )
   printf(", ");
      printf("%s", snd_pcm_format_name((snd_pcm_format_t)fmt));
      prevformat= 1;
    }
  if( !prevformat )
    printf("(none)");
}


