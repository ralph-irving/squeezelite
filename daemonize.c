/*
 *   Copyright (c) 2013 Ralph Irving
 *
 *   daemonize.c is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   daemonize.c is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with SlimProtoLib; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "squeezelite.h"

#include <syslog.h>
#include <sys/stat.h>

void fork_child_handler(int signum)
{
	switch(signum) {
	case SIGALRM: exit(EXIT_FAILURE); break;
	case SIGUSR1: exit(EXIT_SUCCESS); break;
	case SIGCHLD: exit(EXIT_FAILURE); break;
	}
}

pid_t parent;
void init_daemonize()
{
	pid_t pid, sid;

	/* already a daemon */
	if ( getppid() == 1 ) return;

	/* Trap signals that we expect to recieve */
	signal(SIGCHLD,fork_child_handler);
	signal(SIGUSR1,fork_child_handler);
	signal(SIGALRM,fork_child_handler);

	/* Fork off the parent process */
	pid = fork();
	if (pid < 0) {
		syslog( LOG_ERR, "unable to fork daemon, code=%d (%s)",
				errno, strerror(errno) );
		exit(EXIT_FAILURE);
	}

	/* If we got a good PID, then we can exit the parent process. */
	if (pid > 0) {

		/* Wait for confirmation from the child via SIGTERM or SIGCHLD, or
		   for two seconds to elapse (SIGALRM).  pause() should not return. */
		alarm(2);
		pause();

		exit(EXIT_FAILURE);
	}

	/* At this point we are executing as the child process */
	parent = getppid();

	/* Cancel certain signals */
	signal(SIGCHLD,SIG_DFL); /* A child process dies */
	signal(SIGTSTP,SIG_IGN); /* Various TTY signals */
	signal(SIGTTOU,SIG_IGN);
	signal(SIGTTIN,SIG_IGN);
	signal(SIGHUP, SIG_IGN); /* Ignore hangup signal */
	signal(SIGTERM,SIG_DFL); /* Die on SIGTERM */

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		syslog( LOG_ERR, "unable to create a new session, code %d (%s)",
				errno, strerror(errno) );
		exit(EXIT_FAILURE);
	}

	/* Change the current working directory.  This prevents the current
	   directory from being locked; hence not being able to remove it. */
	if ((chdir("/")) < 0) {
		syslog( LOG_ERR, "unable to change directory to %s, code %d (%s)",
				"/", errno, strerror(errno) );
		exit(EXIT_FAILURE);
	}
}

int daemon( int nochdir, int noclose ) {

	if ( ! noclose )
	{
		/* Redirect standard files to /dev/null */
		freopen( "/dev/null", "r", stdin);
		freopen( "/dev/null", "w", stdout);
		freopen( "/dev/null", "w", stderr);
	}

	/* Tell the parent process that we are A-okay */
	kill( parent, SIGUSR1 );

	return 0;
}
