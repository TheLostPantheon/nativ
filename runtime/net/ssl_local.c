/* np_matching2's TLS to this copy's own matching service (host/online, ONLINE.md), as plain bytes. The module logs in over
 * TLS to its matching server's port 443 through Sony's libssl (recompiled with it); to the local service the host answers
 * those calls itself (runtime/core/main.c host_preferred, while no matching server is chosen): the context, connection and BIO
 * are tokens, the handshake succeeds at once, reads and writes go straight to the socket. To any other server (settings
 * matching_server: Mirage's) Sony's libssl does real TLS as on the console. The calls and the way the module reads their
 * results: np_matching2 FUN_81027ba6 / FUN_81027d64 / FUN_81027d88 / FUN_81027dfc (3.65); OpenSSL's names for them. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#include "hle.h"
#include "net.h"
#include "settings.h"

/* the local service is in use: no matching server chosen (read when the module's imports first resolve) */
int matching_local(void) {
	const char *e = getenv("RECOMP_MATCHING_SERVER"); if (e) return !*e;
	char s[256]; settings_str("matching_server", "", s, sizeof s); return !s[0];
}

#define MAXSSL 16
static struct { int used; uint32_t sid; } ssls[MAXSSL];   /* SSL* token 0x5d000000 | i -> the guest socket */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
enum { CTX = 0x5c000001u, SSLT = 0x5d000000u, BIOT = 0x5e000000u };
static int ssl_index(uint32_t h) { const uint32_t i = h - SSLT; return h >= SSLT && i < MAXSSL && ssls[i].used ? (int)i : -1; }

static void h_zero(Cpu *c) { RET(0); }
static void h_pool(Cpu *c) { if (A0) WR32(A0, 0x5b000001u); RET(0); }           /* 2185BB05(u32 *pool) */
static void h_method(Cpu *c) { RET(0x5a000001u); }                               /* A374CD7B(): the client method */
static void h_ctx_new(Cpu *c) { RET(CTX); }                                      /* E746465E(pool, method) */
static void h_ssl_new(Cpu *c) {                                                  /* D28834E6(ctx) */
	pthread_mutex_lock(&lock);
	int i; for (i = 0; i < MAXSSL && ssls[i].used; i++) {}
	if (i < MAXSSL) { ssls[i].used = 1; ssls[i].sid = 0; }
	pthread_mutex_unlock(&lock);
	RET(i < MAXSSL ? SSLT | (uint32_t)i : 0);
}
static void h_bio_socket(Cpu *c) { RET(BIOT | (A0 & 0xffffff)); }               /* FFE2ED51(fd, close flag) */
static void h_set_bio(Cpu *c) { const int i = ssl_index(A0); if (i >= 0) ssls[i].sid = A1 & 0xffffff; }   /* 4A1557A7(ssl, rbio, wbio) */
static void h_handshake(Cpu *c) { RET(ssl_index(A0) >= 0 ? 1 : 0xffffffffu); }  /* E5EDD91B(ssl) */
static __thread int last_errno;   /* the socket error behind this thread's last read or write, for get_error */
static void h_get_error(Cpu *c) {                                                /* 2F0FC3CD(ssl, ret) */
	const int32_t r = (int32_t)A1;
	if (r > 0) { RET(0); return; }
	if (r == 0) { RET(6); return; }                                              /* ZERO_RETURN: the peer closed */
	net_set_guest_errno(last_errno);
	RET(last_errno == EAGAIN || last_errno == EWOULDBLOCK ? 2 : 5);             /* WANT_READ, or SYSCALL with the errno set */
}
static void h_write(Cpu *c) {                                                    /* F39A49C6 SSL_write(ssl, buf, len): all of it */
	const int i = ssl_index(A0), fd = i >= 0 ? net_socket_fd(ssls[i].sid) : -1;
	if (fd < 0) { last_errno = EBADF; RET(0xffffffffu); return; }
	size_t sent = 0;
	while (sent < A2) { const int n = (int)send(fd, (const char *)G(A1) + sent, (int)(A2 - sent), 0); if (n <= 0) { last_errno = errno; RET(0xffffffffu); return; } sent += (size_t)n; }
	RET(A2);
}
static void h_read(Cpu *c) {                                                     /* 4F781BB6 SSL_read(ssl, buf, len) */
	const int i = ssl_index(A0), fd = i >= 0 ? net_socket_fd(ssls[i].sid) : -1;
	if (fd < 0) { last_errno = EBADF; RET(0xffffffffu); return; }
	const int n = (int)recv(fd, (char *)G(A1), (int)A2, 0);
	if (n < 0) last_errno = errno;
	RET((uint32_t)n);
}
static void h_free(Cpu *c) { const int i = ssl_index(A0); if (i >= 0) { pthread_mutex_lock(&lock); ssls[i].used = 0; pthread_mutex_unlock(&lock); } }   /* 19B2BB58(ssl) */

const HleEntry hle_ssl_local[] = {
	{"sceSslInternalIsInitalized", h_zero}, {"SceSslInternal_2185BB05", h_pool}, {"SceSslInternal_A374CD7B", h_method},
	{"SceSslInternal_E746465E", h_ctx_new}, {"SceSslInternal_67FB1CD3", h_zero}, {"SceSslInternal_FBEB1FD7", h_zero},
	{"SceHttp_A9A39BA1", h_zero}, {"SceSslInternal_260F5A85", h_zero}, {"SceSslInternal_AF50B945", h_zero}, {"SceSslInternal_CC7F4211", h_zero},
	{"SceSslInternal_D28834E6", h_ssl_new}, {"SceSslInternal_0DB6C206", h_zero}, {"SceSslInternal_FFE2ED51", h_bio_socket},
	{"SceSslInternal_4A1557A7", h_set_bio}, {"SceSslInternal_F2C9B744", h_zero}, {"SceSslInternal_F71DB06F", h_zero},
	{"SceSslInternal_CEE46B33", h_zero}, {"SceSslInternal_E5EDD91B", h_handshake}, {"SceSslInternal_2F0FC3CD", h_get_error},
	{"sceSslInternalWrite", h_write}, {"SceSslInternal_F39A49C6", h_write}, {"SceSslInternal_4F781BB6", h_read},
	{"SceSslInternal_EC9FAF42", h_zero}, {"SceSslInternal_19B2BB58", h_free}, {"SceSslInternal_EF9EBAC7", h_zero},
	{0, 0},
};
