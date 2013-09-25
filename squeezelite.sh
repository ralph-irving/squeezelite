#!/bin/ksh

INSTALL_DIR=/opt/squeezelite
export LD_LIBRARY_PATH=$INSTALL_DIR/lib

if [ ! -z $SUN_SUNRAY_TOKEN ]; then

	AUDIODEV=$(/opt/SUNWut/bin/utaudio $$ squeezelite)
	trap 'pkill -f "utaudio.*$$"; exit 1' EXIT

	UTPID=$(pgrep -f "utaudio.*$$")

	/bin/priocntl -s -c RT -i pid $UTPID
fi

DEVICES=`$INSTALL_DIR/bin/squeezelite -l`

if [ -z $DEVICES ]; then
	echo "No audio device found."
	exit 1
fi

AUDIOCTL=/usr/sfw/bin/audioctl

if [ -x ${AUDIOCTL} ]; then
	${AUDIOCTL} =headphone
	${AUDIOCTL} =46%
	${AUDIOCTL} -speaker
fi

$INSTALL_DIR/bin/squeezelite $*
