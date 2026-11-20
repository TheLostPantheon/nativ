/*
 * Unit harness for runtime/net/net.c offline (net_online() 0): the network a Vita has with its Wi-Fi off. The byte-order
 * and address helpers work; netctl says DISCONNECTED and has no connection to describe; the resolver has no DNS; a
 * socket bound to "any address" is bound to this machine's loopback, never a port on the player's network; a datagram or
 * a connection to anywhere else is ENETUNREACH; and two sockets on this machine still talk. runtime/net/http.c too:
 * a template, connection and request are made, and sending it fails as an unreachable network. It includes net.c and
 * http.c with RECOMP_ONLINE=0 and supplies what they take from the rest of the runtime (the overlay, settings, STUN, the
 * matching service: off). Uses real loopback sockets. Build and run: see README.md in this directory.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "../net/net.c"
#include "../net/http.c"

/* ---- the runtime net.c expects */
uint8_t *recomp_mem;
static uint32_t heap = 0x81000000;
uint32_t guest_alloc(uint32_t size, uint32_t align) { heap = (heap + align - 1) & ~(align - 1); uint32_t a = heap; heap += size; return a; }
void plat_random(void *buf, size_t n) { memset(buf, 7, n); }
int settings_game_option(const char *k) { (void)k; return 0; }
int settings_int(const char *k, int def) { (void)k; return def; }
const char *settings_str(const char *k, const char *def, char *buf, int n) { (void)k; snprintf(buf, (size_t)n, "%s", def ? def : ""); return buf; }
int matching_local(void) { return 0; }
int stun_start(void) { return 0; }
int ov_on(void) { return 0; }
int ov_is_vip(uint32_t a) { (void)a; return 0; }
uint32_t ov_self_vip(void) { return 0; }
int ov_stream_open(uint32_t vip, uint16_t port) { (void)vip; (void)port; return -1; }
int ov_send_dgram(uint32_t vip, uint16_t dport, uint16_t sport, const uint8_t *d, size_t n) { (void)vip; (void)sport; (void)dport; (void)d; (void)n; return -1; }
void ov_set_dgram_sink(OvDgramSink f) { (void)f; }
void ov_allow_port(uint16_t port) { (void)port; }
int m2_service_port(void) { return 0; }
void hmac_sha1(const void *key, size_t kn, const void *data, size_t n, uint8_t out[20]) { (void)key; (void)kn; (void)data; (void)n; memset(out, 0, 20); }
uint32_t module_export_addr(const char *name) { (void)name; return 0; }

static int checks, fails;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

static uint32_t call(HleFn f, int n, ...) {   /* r0-r3, then the stack words from 0x80f00000 */
	static Cpu c; memset(&c, 0, sizeof c);
	c.r[13] = 0x80f00000;
	va_list ap; va_start(ap, n);
	for (int i = 0; i < n; i++) { uint32_t v = va_arg(ap, uint32_t); if (i < 4) c.r[i] = v; else WR32(c.r[13] + 4u * (uint32_t)(i - 4), v); }
	va_end(ap);
	f(&c);
	return c.r[0];
}
static uint32_t gzero(uint32_t n) { uint32_t a = guest_alloc(n, 8); memset(G(a), 0, n); return a; }
static uint32_t gaddr(const char *ip, uint16_t port) {   /* a SceNetSockaddrIn: len, family, port and address in network order */
	uint32_t a = gzero(16); G(a)[0] = 16; G(a)[1] = 2;
	uint16_t p = htons(port); memcpy(G(a + 2), &p, 2);
	struct in_addr in; inet_pton(AF_INET, ip, &in); memcpy(G(a + 4), &in, 4);
	return a;
}
static uint32_t gstr(const char *s) { uint32_t a = guest_alloc((uint32_t)strlen(s) + 1, 4); strcpy((char *)G(a), s); return a; }
enum { ENETUNREACH_R = 0x80410100u | 51 };

static void test_helpers(void) {
	CHECKEQ(call(h_net_htonl, 1, 0x01020304u), 0x04030201u);
	CHECKEQ(call(h_net_htons, 1, 0x1234u), 0x3412u);
	uint32_t dst = gzero(4);
	CHECKEQ(call(h_net_inet_pton, 3, 2, gstr("10.0.0.44"), dst), 1);
	CHECKEQ(RD32(dst), 0x2c00000au);
}
static void test_netctl(void) {
	uint32_t st = gzero(4); WR32(st, 0xdeadbeef);
	CHECKEQ(call(h_netctl_state, 1, st), 0); CHECKEQ(RD32(st), 0);   /* DISCONNECTED */
	CHECKEQ(call(h_netctl_info, 2, 15, gzero(0x100)), 0x80412101u);   /* no IP address to give */
	CHECKEQ(call(h_net_resolver_ntoa, 6, 1, gstr("example.com"), gzero(4), 0, 0, 0), 0x804101E1u);   /* no DNS */
}
static void test_sockets(void) {
	const uint32_t name = gstr("t");
	uint32_t a = call(h_net_socket, 4, name, 2, 2, 0), b = call(h_net_socket, 4, name, 2, 2, 0);   /* two UDP sockets */
	CHECK(a < 0x80000000u && b < 0x80000000u);
	CHECKEQ(call(h_net_bind, 3, b, gaddr("0.0.0.0", 0), 16), 0);
	uint32_t got = gzero(16), len = gzero(4); WR32(len, 16);
	CHECKEQ(call(h_net_getsockname, 3, b, got, len), 0);
	CHECKEQ(RD32(got + 4), htonl(INADDR_LOOPBACK));   /* "any address" was this machine's loopback */
	uint16_t port; memcpy(&port, G(got + 2), 2);
	CHECK(port != 0);
	const uint32_t msg = gstr("hello"), out = gzero(16), from = gzero(16), fromlen = gzero(4);
	CHECKEQ(call(h_net_sendto, 6, a, msg, 5, 0, gaddr("8.8.8.8", 53), 16), ENETUNREACH_R);   /* the internet: unreachable */
	CHECKEQ(call(h_net_sendto, 6, a, msg, 5, 0, gaddr("192.168.1.10", 9), 16), ENETUNREACH_R);   /* the LAN too */
	CHECKEQ(call(h_net_sendto, 6, a, msg, 5, 0, gaddr("127.0.0.1", ntohs(port)), 16), 5);   /* this machine: delivered */
	WR32(fromlen, 16);
	CHECKEQ(call(h_net_recvfrom, 6, b, out, 16, 0, from, fromlen), 5);
	CHECK(!memcmp(G(out), "hello", 5));
	uint32_t t = call(h_net_socket, 4, name, 2, 1, 0);   /* TCP */
	CHECKEQ(call(h_net_connect, 3, t, gaddr("1.2.3.4", 80), 16), ENETUNREACH_R);
	call(h_net_close, 1, a); call(h_net_close, 1, b); call(h_net_close, 1, t);
}

static void test_http(void) {
	CHECKEQ(call(h_http_init, 1, 0x10000), 0);
	const uint32_t t = call(h_http_create_template, 3, gstr("Nativ"), 1, 0);
	CHECK(t >= 1 && t < 0x80000000u);
	const uint32_t cn = call(h_http_create_connection, 3, t, gstr("http://example.com/"), 1);
	CHECK(cn >= 1 && cn < 0x80000000u);
	const uint32_t rq = call(h_http_create_request, 6, cn, 0, gstr("http://example.com/x"), 0, 0, 0);
	CHECK(rq >= 1 && rq < 0x80000000u);
	CHECKEQ(call(h_http_send, 3, rq, 0, 0), 0x80431063u);   /* NETWORK: never sent */
	CHECKEQ(call(h_http_status, 2, rq, gzero(4)), 0x80431065u);   /* (no response to read) */
}

int main(void) {
	setenv("RECOMP_ONLINE", "0", 1);
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	CHECK(!net_online());
	test_helpers(); test_netctl(); test_sockets(); test_http();
	fprintf(stderr, "%d checks, %d failed\n", checks, fails);
	if (!fails) fprintf(stderr, "all passed\n");
	return fails ? 1 : 0;
}
