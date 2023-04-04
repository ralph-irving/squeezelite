/*
 *  SSL symbols dynamic loader
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

#if USE_SSL && !LINKALL && !NO_SSLSYM

#if WIN
#define dlclose FreeLibrary
#else
#include <dlfcn.h>
#endif

#include "openssl/ssl.h"
#include "openssl/err.h"
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>
#include <openssl/aes.h>
#include <openssl/bio.h>

static void *SSLhandle = NULL;
static void *CRYPThandle = NULL;

#if WIN
static char *LIBSSL[] 		= { "libssl.dll",
							"ssleay32.dll", NULL };
static char *LIBCRYPTO[] 	= { "libcrypto.dll",
							"libeay32.dll", NULL };
#elif OSX
static char *LIBSSL[] 		= { "libssl.dylib", NULL };
static char *LIBCRYPTO[] 	= { "libcrypto.dylib", NULL };
#else
static char *LIBSSL[] 		= {	"libssl.so",
							"libssl.so.1.1",
							"libssl.so.1.0.2",
							"libssl.so.1.0.1",
							"libssl.so.1.0.0", NULL };
static char *LIBCRYPTO[] 	= {	"libcrypto.so",
							"libcrypto.so.1.1",
							"libcrypto.so.1.0.2",
							"libcrypto.so.1.0.1",
							"libcrypto.so.1.0.0", NULL };
#endif

#define P0() void
#define P1(t1, p1) t1 p1
#define P2(t1, p1, t2, p2) t1 p1, t2 p2
#define P3(t1, p1, t2, p2, t3, p3) t1 p1, t2 p2, t3 p3
#define P4(t1, p1, t2, p2, t3, p3, t4, p4) t1 p1, t2 p2, t3 p3, t4 p4
#define P5(t1, p1, t2, p2, t3, p3, t4, p4, t5, p5) t1 p1, t2 p2, t3 p3, t4 p4, t5 p5
#define P6(t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6) t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6
#define V0()
#define V1(t1, p1) p1
#define V2(t1, p1, t2, p2) p1, p2
#define V3(t1, p1, t2, p2, t3, p3) p1, p2, p3
#define V4(t1, p1, t2, p2, t3, p3, t4, p4) p1, p2, p3, p4
#define V5(t1, p1, t2, p2, t3, p3, t4, p4, t5, p5) p1, p2, p3, p4, p5
#define V6(t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6) p1, p2, p3, p4, p5, p6

#define P(n, ...) P##n(__VA_ARGS__)
#define V(n, ...) V##n(__VA_ARGS__)

#define SYM(fn) dlsym_##fn
#define SYMDECL(fn, ret, n, ...) 			\
	static ret (*dlsym_##fn)(P(n,__VA_ARGS__));		\
	ret fn(P(n,__VA_ARGS__)) {				\
		return (*dlsym_##fn)(V(n,__VA_ARGS__));	\
	}

#define SYMDECLVOID(fn, n, ...) 			\
	static void (*dlsym_##fn)(P(n,__VA_ARGS__));		\
	void fn(P(n,__VA_ARGS__)) {				\
		(*dlsym_##fn)(V(n,__VA_ARGS__));	\
	}

#define SYMLOAD(h, fn) dlsym_##fn = dlsym(h, #fn)

SYMDECL(SSL_read, int, 3, SSL*, s, void*, buf, int, len);
SYMDECL(SSL_write, int, 3, SSL*, s, const void*, buf, int, len);
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
SYMDECL(TLS_client_method, const SSL_METHOD*, 0);
SYMDECL(OPENSSL_init_ssl, int, 2, uint64_t, opts, const OPENSSL_INIT_SETTINGS*, settings);
#else
SYMDECL(SSLv23_client_method, const SSL_METHOD*, 0);
SYMDECL(SSL_library_init, int, 0);
#endif
SYMDECL(SSL_CTX_new, SSL_CTX*, 1, const SSL_METHOD *, meth);
SYMDECL(SSL_CTX_ctrl, long, 4, SSL_CTX *, ctx, int, cmd, long, larg, void*, parg);
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
SYMDECL(SSL_CTX_set_options, unsigned long, 2, SSL_CTX*, ctx, unsigned long, op);
#endif
SYMDECL(SSL_new, SSL*, 1, SSL_CTX*, s);
SYMDECL(SSL_connect, int, 1, SSL*, s);
SYMDECL(SSL_shutdown, int, 1, SSL*, s);
SYMDECL(SSL_get_fd, int, 1, const SSL*, s);
SYMDECL(SSL_set_fd, int, 2, SSL*, s, int, fd);
SYMDECL(SSL_get_error, int, 2, const SSL*, s, int, ret_code);
SYMDECL(SSL_ctrl, long, 4, SSL*, ssl, int, cmd, long, larg, void*, parg);
SYMDECL(SSL_pending, int, 1, const SSL*, s);
SYMDECLVOID(SSL_free, 1, SSL*, s);
SYMDECLVOID(SSL_CTX_free, 1, SSL_CTX *, ctx);
SYMDECL(ERR_get_error, unsigned long, 0);
SYMDECLVOID(ERR_clear_error, 0);

static void *dlopen_try(char **filenames, int flag) {
	void *handle;
	for (handle = NULL; !handle && *filenames; filenames++) handle = dlopen(*filenames, flag);
	return handle;
}

bool load_ssl_symbols(void) {
	CRYPThandle = dlopen_try(LIBCRYPTO, RTLD_NOW);
	SSLhandle = dlopen_try(LIBSSL, RTLD_NOW);

	if (!SSLhandle || !CRYPThandle) {
		free_ssl_symbols();
		return false;
	}

	SYMLOAD(SSLhandle, SSL_CTX_new);
	SYMLOAD(SSLhandle, SSL_CTX_ctrl);
	SYMLOAD(SSLhandle, SSL_CTX_free);
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
	SYMLOAD(SSLhandle, SSL_CTX_set_options);
#endif
	SYMLOAD(SSLhandle, SSL_ctrl);
	SYMLOAD(SSLhandle, SSL_free);
	SYMLOAD(SSLhandle, SSL_new);
	SYMLOAD(SSLhandle, SSL_connect);
	SYMLOAD(SSLhandle, SSL_get_fd);
	SYMLOAD(SSLhandle, SSL_set_fd);
	SYMLOAD(SSLhandle, SSL_get_error);
	SYMLOAD(SSLhandle, SSL_shutdown);
	SYMLOAD(SSLhandle, SSL_read);
	SYMLOAD(SSLhandle, SSL_write);
	SYMLOAD(SSLhandle, SSL_pending);
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
	SYMLOAD(SSLhandle, TLS_client_method);
	SYMLOAD(SSLhandle, OPENSSL_init_ssl);
#else
	SYMLOAD(SSLhandle, SSLv23_client_method);
	SYMLOAD(SSLhandle, SSL_library_init);
#endif

	SYMLOAD(CRYPThandle, ERR_clear_error);
	SYMLOAD(CRYPThandle, ERR_get_error);

	return true;
}

void free_ssl_symbols(void) {
	if (SSLhandle) dlclose(SSLhandle);
	if (CRYPThandle) dlclose(CRYPThandle);
}

#endif
