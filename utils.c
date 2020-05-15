/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2017, ralph_irving@hotmail.com
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
 */

#include "squeezelite.h"

#if LINUX || OSX || FREEBSD
#include <sys/ioctl.h>
#include <net/if.h>
#include <netdb.h>
#if FREEBSD
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif
#endif
#if SUN
#include <sys/socket.h>
#include <sys/sockio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif
#if WIN
#include <iphlpapi.h>
#if USE_SSL
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#endif
#endif
#if OSX
#include <net/if_dl.h>
#include <net/if_types.h>
#include <ifaddrs.h>
#include <netdb.h>
#endif

#include <fcntl.h>

// logging functions
const char *logtime(void) {
	static char buf[100];
#if WIN
	SYSTEMTIME lt;
	GetLocalTime(&lt);
	sprintf(buf, "[%02d:%02d:%02d.%03d]", lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	strftime(buf, sizeof(buf), "[%T.", localtime(&tv.tv_sec));
	sprintf(buf+strlen(buf), "%06ld]", (long)tv.tv_usec);
#endif
	return buf;
}

void logprint(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

// cmdline parsing
char *next_param(char *src, char c) {
	static char *str = NULL;
	char *ptr, *ret;
	if (src) str = src;
	if (str && (ptr = strchr(str, c))) {
		ret = str;
		*ptr = '\0';
		str = ptr + 1;
	} else {
		ret = str;
		str = NULL;
	}

	return ret && ret[0] ? ret : NULL;
}

// clock
u32_t gettime_ms(void) {
#if WIN
	return GetTickCount();
#else
#if LINUX || FREEBSD
	struct timespec ts;
#ifdef CLOCK_MONOTONIC
	if (!clock_gettime(CLOCK_MONOTONIC, &ts)) {
#else
	if (!clock_gettime(CLOCK_REALTIME, &ts)) {
#endif
		return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}
#endif
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

// mac address
#if LINUX && !defined(SUN)
// search first 4 interfaces returned by IFCONF
void get_mac(u8_t mac[]) {
	char *utmac;
	struct ifconf ifc;
	struct ifreq *ifr, *ifend;
	struct ifreq ifreq;
	struct ifreq ifs[4];

	utmac = getenv("UTMAC");
	if (utmac)
	{
		if ( strlen(utmac) == 17 )
		{
			if (sscanf(utmac,"%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
				&mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) == 6)
			{
				return;
			}
		}

	}

	mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0;

	int s = socket(AF_INET, SOCK_DGRAM, 0);

	ifc.ifc_len = sizeof(ifs);
	ifc.ifc_req = ifs;

	if (ioctl(s, SIOCGIFCONF, &ifc) == 0) {
		ifend = ifs + (ifc.ifc_len / sizeof(struct ifreq));

		for (ifr = ifc.ifc_req; ifr < ifend; ifr++) {
			if (ifr->ifr_addr.sa_family == AF_INET) {

				strncpy(ifreq.ifr_name, ifr->ifr_name, sizeof(ifreq.ifr_name) - 1);
				if (ioctl (s, SIOCGIFHWADDR, &ifreq) == 0) {
					memcpy(mac, ifreq.ifr_hwaddr.sa_data, 6);
					if (mac[0]+mac[1]+mac[2] != 0) {
						break;
					}
				}
			}
		}
	}

	close(s);
}
#endif

#if SUN
void get_mac(u8_t mac[]) {
	struct  arpreq          parpreq;
	struct  sockaddr_in     *psa;
	struct  in_addr         inaddr;
	struct  hostent         *phost;
	char                    hostname[MAXHOSTNAMELEN];
	char                    **paddrs;
	char                    *utmac;
	int                     sock;
	int                     status=0;

	utmac = getenv("UTMAC");
	if (utmac)
	{
		if ( strlen(utmac) == 17 )
		{
			if (sscanf(utmac,"%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
				&mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) == 6)
			{
				return;
			}
		}

	}

	mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0;

	gethostname(hostname,  MAXHOSTNAMELEN);

	phost = gethostbyname(hostname);

	paddrs = phost->h_addr_list;
	memcpy(&inaddr.s_addr, *paddrs, sizeof(inaddr.s_addr));

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if(sock == -1)
	{
		mac[5] = 1;
		return;
	}

	memset(&parpreq, 0, sizeof(struct arpreq));
	psa = (struct sockaddr_in *) &parpreq.arp_pa;
	memset(psa, 0, sizeof(struct sockaddr_in));
	psa->sin_family = AF_INET;
	memcpy(&psa->sin_addr, *paddrs, sizeof(struct in_addr));

	status = ioctl(sock, SIOCGARP, &parpreq);

	if(status == -1)
	{
		mac[5] = 2;
		return;
	}

	mac[0] = (unsigned char) parpreq.arp_ha.sa_data[0];
	mac[1] = (unsigned char) parpreq.arp_ha.sa_data[1];
	mac[2] = (unsigned char) parpreq.arp_ha.sa_data[2];
	mac[3] = (unsigned char) parpreq.arp_ha.sa_data[3];
	mac[4] = (unsigned char) parpreq.arp_ha.sa_data[4];
	mac[5] = (unsigned char) parpreq.arp_ha.sa_data[5];
}
#endif

#if OSX || FREEBSD
void get_mac(u8_t mac[]) {
	struct ifaddrs *addrs, *ptr;
	const struct sockaddr_dl *dlAddr;
	const unsigned char *base;
	
	mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0;
	
	if (getifaddrs(&addrs) == 0) {
		ptr = addrs;
		while (ptr) {
			if (ptr->ifa_addr->sa_family == AF_LINK && ((const struct sockaddr_dl *) ptr->ifa_addr)->sdl_type == IFT_ETHER) {
				dlAddr = (const struct sockaddr_dl *)ptr->ifa_addr;
				base = (const unsigned char*) &dlAddr->sdl_data[dlAddr->sdl_nlen];
				memcpy(mac, base, min(dlAddr->sdl_alen, 6));
				break;
			}
			ptr = ptr->ifa_next;
		}
		freeifaddrs(addrs);
	}
}
#endif

#if WIN
#pragma comment(lib, "IPHLPAPI.lib")
void get_mac(u8_t mac[]) {
	IP_ADAPTER_INFO AdapterInfo[16];
	DWORD dwBufLen = sizeof(AdapterInfo);
	DWORD dwStatus = GetAdaptersInfo(AdapterInfo, &dwBufLen);
	
	mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0;

	if (GetAdaptersInfo(AdapterInfo, &dwBufLen) == ERROR_SUCCESS) {
		memcpy(mac, AdapterInfo[0].Address, 6);
	}
}
#endif

void set_nonblock(sockfd s) {
#if WIN
	u_long iMode = 1;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

// connect for socket already set to non blocking with timeout in seconds
int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout) {
	fd_set w, e;
	struct timeval tval;

	if (connect(sock, addr, addrlen) < 0) {
#if !WIN
		if (last_error() != EINPROGRESS) {
#else
		if (last_error() != WSAEWOULDBLOCK) {
#endif
			return -1;
		}
	}

	FD_ZERO(&w);
	FD_SET(sock, &w);
	e = w;
	tval.tv_sec = timeout;
	tval.tv_usec = 0;

	// only return 0 if w set and sock error is zero, otherwise return error code
	if (select(sock + 1, NULL, &w, &e, timeout ? &tval : NULL) == 1 && FD_ISSET(sock, &w)) {
		int	error = 0;
		socklen_t len = sizeof(error);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&error, &len);
		return error;
	}

	return -1;
}

void server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr) {
	struct addrinfo *res = NULL;
	struct addrinfo hints;
	const char *port = NULL;
	
	if (strtok(server, ":")) {
		port = strtok(NULL, ":");
		if (port) {
			*port_ptr = atoi(port);
		}
	}

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	
	getaddrinfo(server, NULL, &hints, &res);
	
	if (res && res->ai_addr) {
		*ip_ptr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
	} 
	
	if (res) {
		freeaddrinfo(res);
	}
}

void set_readwake_handles(event_handle handles[], sockfd s, event_event e) {
#if WINEVENT
	handles[0] = WSACreateEvent();
	handles[1] = e;
	WSAEventSelect(s, handles[0], FD_READ | FD_CLOSE);
#elif SELFPIPE
	handles[0].fd = s;
	handles[1].fd = e.fds[0];
	handles[0].events = POLLIN;
	handles[1].events = POLLIN;
#else
	handles[0].fd = s;
	handles[1].fd = e;
	handles[0].events = POLLIN;
	handles[1].events = POLLIN;
#endif
}

event_type wait_readwake(event_handle handles[], int timeout) {
#if WINEVENT
	int wait = WSAWaitForMultipleEvents(2, handles, FALSE, timeout, FALSE);
	if (wait == WSA_WAIT_EVENT_0) {
		WSAResetEvent(handles[0]);
		return EVENT_READ;
	} else if (wait == WSA_WAIT_EVENT_0 + 1) {
		return EVENT_WAKE;
	} else {
		return EVENT_TIMEOUT;
	}
#else
	if (poll(handles, 2, timeout) > 0) {
		if (handles[0].revents) {
			return EVENT_READ;
		}
		if (handles[1].revents) {
			wake_clear(handles[1].fd);
			return EVENT_WAKE;
		}
	}
	return EVENT_TIMEOUT;
#endif
}

// pack/unpack to network byte order
void packN(u32_t *dest, u32_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr)   = (val >> 24) & 0xFF; *(ptr+1) = (val >> 16) & 0xFF; *(ptr+2) = (val >> 8) & 0xFF;	*(ptr+3) = val & 0xFF;
}

void packn(u16_t *dest, u16_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr) = (val >> 8) & 0xFF; *(ptr+1) = val & 0xFF;
}

u32_t unpackN(u32_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 24 | *(ptr+1) << 16 | *(ptr+2) << 8 | *(ptr+3);
} 

u16_t unpackn(u16_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 8 | *(ptr+1);
} 

#if OSX
void set_nosigpipe(sockfd s) {
	int set = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
}
#endif

#if WIN
void winsock_init(void) {
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 2);
	int WSerr = WSAStartup(wVersionRequested, &wsaData);
	if (WSerr != 0) {
		LOG_ERROR("Bad winsock version");
		exit(1);
	}
}

void winsock_close(void) {
	WSACleanup();
}

void *dlopen(const char *filename, int flag) {
	SetLastError(0);
	return LoadLibrary((LPCTSTR)filename);
}

void *dlsym(void *handle, const char *symbol) {
	SetLastError(0);
	return (void *)GetProcAddress(handle, symbol);
}

char *dlerror(void) {
	static char ret[32];
	int last = GetLastError();
	if (last) {
		sprintf(ret, "code: %i", last);
		SetLastError(0);
		return ret;
	}
	return NULL;
}

// this only implements numfds == 1
int poll(struct pollfd *fds, unsigned long numfds, int timeout) {
	fd_set r, w;
	struct timeval tv;
	int ret;
	
	FD_ZERO(&r);
	FD_ZERO(&w);
	
	if (fds[0].events & POLLIN) FD_SET(fds[0].fd, &r);
	if (fds[0].events & POLLOUT) FD_SET(fds[0].fd, &w);
	
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = 1000 * (timeout % 1000);
	
	ret = select(fds[0].fd + 1, &r, &w, NULL, &tv);

	if (ret < 0) return ret;
	
	fds[0].revents = 0;
	if (FD_ISSET(fds[0].fd, &r)) fds[0].revents |= POLLIN;
	if (FD_ISSET(fds[0].fd, &w)) fds[0].revents |= POLLOUT;
	
	return ret;
}

#endif

#if LINUX || FREEBSD
void touch_memory(u8_t *buf, size_t size) {
	u8_t *ptr;
	for (ptr = buf; ptr < buf + size; ptr += sysconf(_SC_PAGESIZE)) {
		*ptr = 0;
	}
}
#endif

#if WIN && USE_SSL
char *strcasestr(const char *haystack, const char *needle) {
	size_t length_needle;
	size_t length_haystack;
	size_t i;

	if (!haystack || !needle)
		return NULL;

	length_needle = strlen(needle);
	length_haystack = strlen(haystack) - length_needle + 1;

	for (i = 0; i < length_haystack; i++)
	{
		size_t j;

		for (j = 0; j < length_needle; j++)
		{
			unsigned char c1;
			unsigned char c2;

			c1 = haystack[i+j];
			c2 = needle[j];
			if (toupper(c1) != toupper(c2))
				goto next;
		}
		return (char *) haystack + i;
		next:
			;
	}

	return NULL;
}
#endif
