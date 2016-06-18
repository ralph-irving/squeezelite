Squeezelite v1.8.n, Copyright 2012-2015 Adrian Smith, 2015-2016 Ralph Irving.<br>
See -t for license terms<br>
<br>
Usage: squeezelite [options]<br>
<br>
  -s <server>[:<port>]	Connect to specified server, otherwise uses autodiscovery to find server<br>
  -o <output device>	Specify output device, default "default", - = output to stdout<br>
  -l 			List output devices<br>
  -a <b>:<p>:<f>:<m>	Specify ALSA params to open output device, b = buffer time in ms or size in bytes, p = period count or size in bytes, f sample format (16|24|24_3|32), m = use mmap (0|1)<br>
  -a <f>		Specify sample format (16|24|32) of output file when using -o - to output samples to stdout (interleaved little endian only)<br>
  -b <stream>:<output>	Specify internal Stream and Output buffer sizes in Kbytes<br>
  -c <codec1>,<codec2>	Restrict codecs to those specified, otherwise load all available codecs; known codecs: flac,pcm,mp3,ogg,aac,wma,alac,dsd (mad,mpg for specific mp3 codec)<br>
  -C <timeout>		Close output device when idle after timeout seconds, default is to keep it open while player is 'on'<br>
  -d <log>=<level>	Set logging level, logs: all|slimproto|stream|decode|output|ir, level: info|debug|sdebug<br>
  -G <Rpi GPIO#>:<H/L>	Specify the BCM GPIO# to use for Amp Power Relay and if the output should be Active High or Low<br>
  -e <codec1>,<codec2>	Explicitly exclude native support of one or more codecs; known codecs: flac,pcm,mp3,ogg,aac,wma,alac,dsd (mad,mpg for specific mp3 codec)<br>
  -f <logfile>		Write debug to logfile<br>
  -i [<filename>]	Enable lirc remote control support (lirc config file ~/.lircrc used if filename not specified)<br>
  -m <mac addr>		Set mac address, format: ab:cd:ef:12:34:56<br>
  -M <modelname>	Set the squeezelite player model name sent to the server (default: SqueezeLite)<br>
  -n <name>		Set the player name<br>
  -N <filename>		Store player name in filename to allow server defined name changes to be shared between servers (not supported with -n)<br>
  -W			Read wave and aiff format from header, ignore server parameters<br>
  -p <priority>		Set real time priority of output thread (1-99)<br>
  -P <filename>		Store the process id (PID) in filename<br>
  -r <rates>[:<delay>]	Sample rates supported, allows output to be off when squeezelite is started; rates = <maxrate>|<minrate>-<maxrate>|<rate1>,<rate2>,<rate3>; delay = optional delay switching rates in ms<br>
  -S <Power Script>	Absolute path to script to launch on power commands from LMS<br>
  -R -u [params]	Resample, params = <recipe>:<flags>:<attenuation>:<precision>:<passband_end>:<stopband_start>:<phase_response>,<br>
  			 recipe = (v|h|m|l|q)(L|I|M)(s) [E|X], E = exception - resample only if native rate not supported, X = async - resample to max rate for device, otherwise to max sync rate<br>
  			 flags = num in hex,<br>
  			 attenuation = attenuation in dB to apply (default is -1db if not explicitly set),<br>
  			 precision = number of bits precision (NB. HQ = 20. VHQ = 28),<br>
  			 passband_end = number in percent (0dB pt. bandwidth to preserve. nyquist = 100%),<br>
  			 stopband_start = number in percent (Aliasing/imaging control. > passband_end),<br>
  			 phase_response = 0-100 (0 = minimum / 50 = linear / 100 = maximum)<br>
  -D [delay]		Output device supports DSD over PCM (DoP), delay = optional delay switching between PCM and DoP in ms<br>
  -v 			Visualiser support<br>
  -L 			List volume controls for output device<br>
  -U <control>		Unmute ALSA control and set to full volume (not supported with -V)<br>
  -V <control>		Use ALSA control for volume adjustment, otherwise use software volume adjustment<br>
  -z 			Daemonize<br>
  -Z <rate>		Report rate to server in helo as the maximum sample rate we can support<br>
  -t 			License terms<br>
  -? 			Display this help text<br>
<br>
Build options: LINUX ALSA EVENTFD RESAMPLE FFMPEG VISEXPORT IR GPIO DSD LINKALL<br>
