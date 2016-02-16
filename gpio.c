
/*
 * gpio_relay.c - example of driving a relay using the GPIO peripheral on a BCM2835 (Raspberry Pi)
 *
 * Copyright 2012 Kevin Sangeelee.
 * Released as GPLv2, see <http://www.gnu.org/licenses/>
 *
 * This is intended as an example of using Raspberry Pi hardware registers to drive a relay using GPIO. Use at your own
 * risk or not at all. As far as possible, I've omitted anything that doesn't relate to the Raspi registers. There are more
 * conventional ways of doing this using kernel drivers.
 *
 * Additions (c) Paul Hermann, 2015-2016 under the same license terms
 *   -Control of Raspberry pi GPIO for amplifier power
 *   -Launch script on power status change from LMS
 */

#if GPIO

#define BCM2708_PERI_BASE        0x20000000
#define GPIO_BASE                (BCM2708_PERI_BASE + 0x200000) /* GPIO controller */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "squeezelite.h"

#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

int  mem_fd;
void *gpio_map;

// I/O access
volatile unsigned *gpio;


// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0

void setup_io();

int gpio_state = -1;
int initialized = -1;
int power_state = -1;

void relay( int state) {
    gpio_state = state;

  // Set up gpi pointer for direct register access
  if (initialized == -1){
	setup_io();
	initialized = 1;
	INP_GPIO(gpio_pin); // must use INP_GPIO before we can use OUT_GPIO
     	OUT_GPIO(gpio_pin);
  }

  // Set GPIO pin to output

    if(gpio_state == 1)
        GPIO_CLR = 1<<gpio_pin;
    else if(gpio_state == 0)
	GPIO_SET = 1<<gpio_pin;

    usleep(1);    // Delay to allow any change in state to be reflected in the LEVn, register bit.

    // Done!
}

//
// Set up a memory regions to access GPIO
//
void setup_io()
{
   /* open /dev/mem */
   if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
      printf("can't open /dev/mem \n");
      exit(-1);
   }

   /* mmap GPIO */
   gpio_map = mmap(
      NULL,             //Any adddress in our space will do
      BLOCK_SIZE,       //Map length
      PROT_READ|PROT_WRITE,// Enable reading & writting to mapped memory
      MAP_SHARED,       //Shared with other processes
      mem_fd,           //File to map
      GPIO_BASE         //Offset to GPIO peripheral
   );

   close(mem_fd); //No need to keep mem_fd open after mmap

   if (gpio_map == MAP_FAILED) {
      printf("mmap error %d\n", (int)gpio_map);//errno also set!
      exit(-1);
   }

   // Always use volatile pointer!
   gpio = (volatile unsigned *)gpio_map;


} // setup_io

char *cmdline;
int argloc;

void relay_script( int state) {
	gpio_state = state;
	int err;
   
  // Call script with init parameter
	if (initialized == -1){
		int strsize = strlen(power_script);
		cmdline = (char*) malloc(strsize+3);
		argloc = strsize + 1;

		strcpy(cmdline, power_script);
		strcat(cmdline, " 2");
		if ((err = system(cmdline)) != 0){ 
			fprintf (stderr, "%s exit status = %d\n", cmdline, err);
		}
		else{
			initialized = 1;
		}
  	}

  // Call Script to turn on or off  on = 1, off = 0
  // Checks current status to avoid calling script excessivly on track changes where alsa re-inits.

	if( (gpio_state == 1) && (power_state != 1)){
		cmdline[argloc] = '1';
		if ((err = system(cmdline)) != 0){ 
			fprintf (stderr, "%s exit status = %d\n", cmdline, err);
		}
		else {
			power_state = 1;
		}
	}
	else if( (gpio_state == 0) && (power_state != 0)){
		cmdline[argloc] = '0';
		if ((err = system(cmdline)) != 0){ 
			fprintf (stderr, "%s exit status = %d\n", cmdline, err);
		}
		else {
			power_state = 0;
		}
	}
// Done!
}

#endif // GPIO
