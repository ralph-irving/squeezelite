Squeezelite v1.8.n, Copyright 2012-2015 Adrian Smith, 2015-2016 Ralph Irving.
See -t for license terms

Usage: squeezelite [options]

  -s <server>[:<port>]	Connect to specified server, otherwise uses autodiscovery to find server
  -o <output device>	Specify output device, default "default", - = output to stdout
  -l 			List output devices
  -a <b>:<p>:<f>:<m>	Specify ALSA params to open output device, b = buffer time in ms or size in bytes, p = period count or size in bytes, f sample format (16|24|24_3|32), m = use mmap (0|1)
  -a <f>		Specify sample format (16|24|32) of output file when using -o - to output samples to stdout (interleaved little endian only)
  -b <stream>:<output>	Specify internal Stream and Output buffer sizes in Kbytes
  -c <codec1>,<codec2>	Restrict codecs to those specified, otherwise load all available codecs; known codecs: flac,pcm,mp3,ogg,aac,wma,alac,dsd (mad,mpg for specific mp3 codec)
  -C <timeout>		Close output device when idle after timeout seconds, default is to keep it open while player is 'on'
  -d <log>=<level>	Set logging level, logs: all|slimproto|stream|decode|output|ir, level: info|debug|sdebug
  -G <Rpi GPIO#>:<H/L>	Specify the BCM GPIO# to use for Amp Power Relay and if the output should be Active High or Low
  -e <codec1>,<codec2>	Explicitly exclude native support of one or more codecs; known codecs: flac,pcm,mp3,ogg,aac,wma,alac,dsd (mad,mpg for specific mp3 codec)
  -f <logfile>		Write debug to logfile
  -i [<filename>]	Enable lirc remote control support (lirc config file ~/.lircrc used if filename not specified)
  -m <mac addr>		Set mac address, format: ab:cd:ef:12:34:56
  -M <modelname>	Set the squeezelite player model name sent to the server (default: SqueezeLite)
  -n <name>		Set the player name
  -N <filename>		Store player name in filename to allow server defined name changes to be shared between servers (not supported with -n)
  -W			Read wave and aiff format from header, ignore server parameters
  -p <priority>		Set real time priority of output thread (1-99)
  -P <filename>		Store the process id (PID) in filename
  -r <rates>[:<delay>]	Sample rates supported, allows output to be off when squeezelite is started; rates = <maxrate>|<minrate>-<maxrate>|<rate1>,<rate2>,<rate3>; delay = optional delay switching rates in ms
  -S <Power Script>	Absolute path to script to launch on power commands from LMS
  -R -u [params]	Resample, params = <recipe>:<flags>:<attenuation>:<precision>:<passband_end>:<stopband_start>:<phase_response>,
  			 recipe = (v|h|m|l|q)(L|I|M)(s) [E|X], E = exception - resample only if native rate not supported, X = async - resample to max rate for device, otherwise to max sync rate
  			 flags = num in hex,
  			 attenuation = attenuation in dB to apply (default is -1db if not explicitly set),
  			 precision = number of bits precision (NB. HQ = 20. VHQ = 28),
  			 passband_end = number in percent (0dB pt. bandwidth to preserve. nyquist = 100%),
  			 stopband_start = number in percent (Aliasing/imaging control. > passband_end),
  			 phase_response = 0-100 (0 = minimum / 50 = linear / 100 = maximum)
  -D [delay]		Output device supports DSD over PCM (DoP), delay = optional delay switching between PCM and DoP in ms
  -v 			Visualiser support
  -L 			List volume controls for output device
  -U <control>		Unmute ALSA control and set to full volume (not supported with -V)
  -V <control>		Use ALSA control for volume adjustment, otherwise use software volume adjustment
  -z 			Daemonize
  -Z <rate>		Report rate to server in helo as the maximum sample rate we can support
  -t 			License terms
  -? 			Display this help text

Build options: LINUX ALSA EVENTFD RESAMPLE FFMPEG VISEXPORT IR GPIO DSD LINKALL
