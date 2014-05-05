#!/bin/ksh

INSTALL_DIR=/opt/squeezelite
UTSET=/opt/SUNWut/bin/utset

if [ ! -z $SUN_SUNRAY_TOKEN ]; then

        AUDIODEV=$(/opt/SUNWut/bin/utaudio $$ squeezelite)
        trap 'pkill -f "utaudio.*$$"; exit 1' EXIT

	UTPID=$(pgrep -f "utaudio.*$$")
	if [ ! -z $UTPID ]; then
		/bin/priocntl -s -c RT -i pid $UTPID
	fi

	if [ -x ${UTSET} ]; then
                ${UTSET} -o s=h,v=15
        fi

	if [ ! -z $AUDIODEV ]; then
        	DEV="-o $AUDIODEV"
	else
        	DEV=""
	fi
fi

$INSTALL_DIR/bin/squeezelite $DEV $*
