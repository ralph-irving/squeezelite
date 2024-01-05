/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2024, ralph_irving@hotmail.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * gpio.c (c) Paul Hermann, 2015-2024 under the same license terms
 *   -Control of Raspberry pi GPIO for amplifier power
 *   -Launch script on power status change from LMS
 */

#if GPIO

#include "squeezelite.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if RPI
#include <gpiod.h>
#endif  // RPI

static int gpio_state = -1;
static bool initialized = false;
static int power_state = -1;

#if RPI
static struct gpiod_line_request *gpioline = NULL;

static struct gpiod_line_request *request_output_line(const char *chip_path, unsigned int lineno,
		    enum gpiod_line_value value, const char *consumer) {

	struct gpiod_request_config *req_cfg = NULL;
	struct gpiod_line_request *req = NULL;
	struct gpiod_line_settings *settings;
	struct gpiod_line_config *line_cfg;
	struct gpiod_chip *chip;

	chip = gpiod_chip_open(chip_path);
	if (!chip)
		return req;

	settings = gpiod_line_settings_new();
	if (!settings)
		goto close_chip;

	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings, value);

	line_cfg = gpiod_line_config_new();
	if (!line_cfg)
		goto free_settings;

	int ret = gpiod_line_config_add_line_settings(line_cfg, &lineno, 1, settings);
	if (ret)
		goto free_line_config;

	if (consumer) {
		req_cfg = gpiod_request_config_new();
		if (!req_cfg)
			goto free_line_config;

		gpiod_request_config_set_consumer(req_cfg, consumer);
	}

	req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	gpiod_request_config_free(req_cfg);

free_line_config:
	gpiod_line_config_free(line_cfg);

free_settings:
	gpiod_line_settings_free(settings);

close_chip:
	gpiod_chip_close(chip);

	return req;
}

bool gpio_init(){
	// Set up gpio using kernel interface
	if (!initialized){
		char chpath[50];
		snprintf(chpath, 50, "/dev/gpiochip%d", gpio_chip);

		enum gpiod_line_value value = GPIOD_LINE_VALUE_INACTIVE^gpio_active_low;
		gpioline = request_output_line(chpath, gpio_pin, value, MODEL_NAME_STRING);

		if (!gpioline){
			LOG_ERROR("Unable to open GPIO Chip:%d line:%d for output", gpio_chip, gpio_pin);
		} else {
			initialized = true;
		}
	}
	return initialized;
}

void gpio_close(){
	if (gpioline){
		gpiod_line_request_release(gpioline);
	}
}
#endif //RPI

void relay( int state) {
#ifdef RPI
    gpio_state = state;
    int status = 0;

	if (initialized){
		if(gpio_state == 1)
			status = gpiod_line_request_set_value(gpioline, gpio_pin, GPIOD_LINE_VALUE_ACTIVE^gpio_active_low);
		else if(gpio_state == 0)
			status = gpiod_line_request_set_value(gpioline, gpio_pin, GPIOD_LINE_VALUE_INACTIVE^gpio_active_low);

		if (status != 0)
			LOG_ERROR("Unable to write to GPIO Pin:%d, Error Code:%d", gpio_pin, status);
	}
  // Done!
#endif //RPI
}

char *cmdline;
int argloc;

void relay_script( int state) {
	gpio_state = state;
	int err;

  // Call script with init parameter
	if (!initialized){
		int strsize = strlen(power_script);
		cmdline = (char*) malloc(strsize+3);
		argloc = strsize + 1;

		strcpy(cmdline, power_script);
		strcat(cmdline, " 2");
		if ((err = system(cmdline)) != 0){
			fprintf (stderr, "%s exit status = %d\n", cmdline, err);
		}
		else{
			initialized = true;
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
