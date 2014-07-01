/************************************************************************/
/* setrpath		 						*/
/*									*/
/* By Davin Milun (milun@cs.buffalo.edu)				*/
/* Last modified: Sun Feb 26 12:21:06 EST 1995				*/
/*									*/
/* Program to set the RPATH in an ELF executable.			*/
/* However, it cannot set the RPATH longer than the RPATH set at        */
/* compile time.							*/
/*									*/
/* Send any bug reports/fixes/suggestions to milun@cs.buffalo.edu       */
/************************************************************************/

/************************************************************************/
/* Copyright (C) 1995, Davin Milun					*/
/* Permission to use and modify this software for any purpose other	*/
/* than its incorporation into a commercial product is hereby granted   */
/* without fee.								*/
/*									*/
/* Permission to copy and distribute this software only for		*/
/* non-commercial use is also granted without fee, provided, however,	*/
/* that the above copyright notice appear in all copies, that both that */
/* copyright notice and this permission notice appear in supporting	*/
/* documentation.  The author makes no representations about the	*/
/* suitability of this software for any purpose.  It is provided 	*/
/* ``as is'' without express or implied warranty.			*/
/*									*/
/* Compile: gcc -o setrpath -lelf -O setrpath.c				*/
/************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libelf.h>
#include <link.h>

#define USAGE "USAGE:\t%s [-f] <ELF executable> <new RPATH>\n\
\t%s -r <ELF executable>\n"

#define ORNULL(s)  (s?s:"(null)")

int
main(int argc, char *argv[])
{

int file;
Elf *elf;
Elf_Scn *scn, *strscn;
Elf32_Shdr *scn_shdr;
Elf32_Ehdr *scn_ehdr;
Elf_Data *data, *strdata;
Elf32_Dyn *dyn;
size_t strscnndx;
int oldlen, newlen=0, extra_space;
char *oldrpath, *newrpath=NULL;
unsigned char *strbuffer;
int strbuffersize;
int forceflag=0, readonly=0;
int hasrpath=0;

extern char *optarg;
extern int optind;
int c;

while ((c = getopt(argc, argv, "fr")) != EOF){
  switch(c){
    case 'f': forceflag=1;
	      break;
    case 'r': readonly=1;
	      break;
    case '?': fprintf(stderr, USAGE, argv[0], argv[0]);
	      break;
    }
  }

if (argc != (optind+2-readonly)) {
  fprintf(stderr,"Wrong number of arguments\n");
  fprintf(stderr, USAGE, argv[0], argv[0]);
  exit(1);
  }

if (!readonly){
  newrpath = strdup(argv[optind+1]);
  newlen = strlen(newrpath);
  }

if (elf_version(EV_CURRENT) == EV_NONE) {
  fprintf(stderr,"Old version of ELF.\n");
  exit(2);
  }

if ((file = open(argv[optind],(readonly?O_RDONLY:O_RDWR))) == -1) {
  fprintf(stderr,"Cannot open %s for %s\n",(readonly?"reading":"writing"),
					   ORNULL(argv[optind]));
  perror("open");
  exit(3);
  }

elf = elf_begin(file, (readonly?ELF_C_READ:ELF_C_RDWR), (Elf *)NULL);

if (elf_kind(elf) != ELF_K_ELF) {
  fprintf(stderr,"%s is not an ELF file.\n",argv[optind]);
  exit(4);
  }

if ((scn_ehdr = elf32_getehdr(elf)) == 0) {
  fprintf(stderr,"elf32_getehdr failed.\n");
  exit(5);
  }

scn = NULL;

/* Process sections */
while ((scn = elf_nextscn(elf, scn)) != NULL) {
  scn_shdr = elf32_getshdr(scn);

  /* Only look at SHT_DYNAMIC section */
  if (scn_shdr->sh_type == SHT_DYNAMIC) {
    data = NULL;

    /* Process data blocks in the section */
    while ((data = elf_getdata(scn, data)) != NULL) {
      dyn = (Elf32_Dyn *) data->d_buf;

      /* Process entries in dynamic linking table */
      while (dyn->d_tag != DT_NULL) {
	/* Look at DT_RPATH entry */
	if (dyn->d_tag == DT_RPATH) {
	  hasrpath++;
	  scn_shdr = elf32_getshdr(scn);
	  strscnndx = scn_shdr->sh_link;
	  oldrpath = elf_strptr(elf, strscnndx, dyn->d_un.d_ptr);
	  printf("%s RPATH: %s\n",(readonly?"Current":"Old"),ORNULL(oldrpath));
	  oldlen = strlen(oldrpath);

	  if (readonly) { /* Quit now if readonly*/
	    elf_end(elf);
	    exit(0);
	    }

	  /* Load the section that contains the strings */
	  strscn = elf_getscn(elf,strscnndx);
	  strdata = NULL;
	  while ((strdata = elf_getdata(strscn, strdata)) != NULL) {
	    strbuffersize = strdata->d_size;
	    strbuffer = strdata->d_buf;

	    /* Get next data block if needed */
	    if ((dyn->d_un.d_ptr > (strdata->d_off + strdata->d_size)) || 
	        (dyn->d_un.d_ptr < strdata->d_off)) {
	      fprintf(stderr,"The string table is not in one data block\n");
	      fprintf(stderr,"This is not handled by this program\n");
	      exit(6);
	      }

	    /* See if there is "slack" after end of RPATH */
	    extra_space = strdata->d_size - (dyn->d_un.d_ptr + oldlen + 1);

	    /* Mark the data block as dirty */
	    elf_flagdata(strdata,ELF_C_SET,ELF_F_DIRTY);

            if (newlen > (oldlen + extra_space)) {
	      fprintf(stderr,"New RPATH would be longer than current RPATH \
plus any extra space.\n");
	      fprintf(stderr,"Aborting...\n");
	      exit(7);
	      }

            if ((newlen > oldlen) && !forceflag) {
	      fprintf(stderr,"New RPATH would be longer than current RPATH.\n");
	      fprintf(stderr,"(Use -f to use any extra space in string table)\n");
	      exit(8);
	      }

	    /* Since it will fit in the old place, we can do it */
	    memmove(&strbuffer[dyn->d_un.d_ptr], newrpath, newlen);
	    strbuffer[dyn->d_un.d_ptr+newlen] = 0;

	    } /* while elf_getdata on the string table */

	  } /* if (dyn->d_tag == DT_RPATH) */
	dyn++;
	} /* while (dyn->d_tag != DT_NULL) */
      } /* while elf_getdata */
    } /* if SHT_DYNAMIC */
  } /* while elf_nextscn */

if (readonly) {
  printf("ELF file \"%s\" contains no RPATH.\n",argv[optind]);
  exit(0);
  }

if (hasrpath) {
  if (elf_update(elf, ELF_C_WRITE) == -1 ) {
    fprintf(stderr,"elf_update failed.\n");
    exit(9);
    }

  printf("New RPATH set to: %s\n", ORNULL(newrpath));
  } else {
    fprintf(stderr,"ELF file \"%s\" contains no RPATH - cannot set one.\n",argv[optind]);
    exit(10);
    }

elf_end(elf);

exit(0);
}
