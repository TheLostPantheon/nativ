/*
 * http.c: SceHttp on the host's libcurl -- a game's HTTP(S) to its own servers (Killzone logs in to
 * kzv.online.scee.com through it; its SSL library is only initialised, every TLS byte goes through here).
 *
 * Templates (a user agent), connections (a base URL) and requests (method, URL, headers, length) are kept as the game
 * makes them; sceHttpSendRequest performs the whole exchange on the calling thread and keeps the status, the response
 * headers and the body, which GetStatusCode, GetResponseContentLength, GetAllResponseHeaders (copied into guest memory,
 * where ParseResponseHeader's results point) and ReadData then hand out. A redirected host (net.c: the player's
 * private server, settings "private_server", or "net_redirect"):
 *  - to a server by its name (mirage.yuv.pink): the request goes to that name -- its URL's host rewritten, the game's
 *    own Host header dropped -- and its certificate is verified as any other: the name in the request, the TLS name and
 *    the certificate all match, as a Vita patched to the new name sends them (Mirage swaps the name in the game's
 *    memory; here no per-version offsets are needed);
 *  - to an address or localhost (a server on the player's own machine, servers/kzserver): it connects there with the
 *    original name kept (curl's CONNECT_TO) and without verifying a certificate made for nobody.
 * Others verify as usual.
 *
 * RECOMP_NET_LOG=1: each request and its outcome; RECOMP_NET_DUMP=<dir>: each request and response written out
 * (req_<n>.txt, resp_<n>.txt) -- how a server's protocol is learnt.
 * Without libcurl (RECOMP_CURL undefined: Windows and the self-contained Linux install have none) the servers answered in
 * this process still answer -- Killzone's login stand-in, so online play works everywhere -- and any other request fails
 * as an unreachable network would.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hle.h"
#include "net.h"
#include "settings.h"
#include "sha1.h"
#include "online/m2.h"

#ifdef RECOMP_CURL
#include <curl/curl.h>
#else   /* (no libcurl: its header list, so the requests keep their headers the same way) */
struct curl_slist { char *data; struct curl_slist *next; };
static struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s) {
	struct curl_slist *e = calloc(1, sizeof *e); if (!e || !(e->data = strdup(s))) { free(e); return l; }
	if (!l) return e;
	struct curl_slist *t = l; while (t->next) t = t->next;
	t->next = e; return l;
}
static void curl_slist_free_all(struct curl_slist *l) { while (l) { struct curl_slist *n = l->next; free(l->data); free(l); l = n; } }
#endif

static int http_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_NET_LOG") != 0; return v; }

#define MAXTMPL 16
#define MAXCONN 64
#define MAXREQ 64
typedef struct { char *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const void *d, size_t n) {
	if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
	memcpy(b->p + b->n, d, n); b->n += n; b->p[b->n] = 0;
}
static struct { int used; char ua[256]; } tmpls[MAXTMPL];
static struct { int used, tmpl; char url[1024]; } conns[MAXCONN];
static struct {
	int used, conn, method, sent, status;
	char url[1024]; struct curl_slist *headers; uint64_t content_length;
	Buf resp_head, resp_body; size_t read_at;
	uint32_t ghead, ghead_cap;   /* the response headers' guest copy */
} reqs[MAXREQ];
static pthread_mutex_t http_lock = PTHREAD_MUTEX_INITIALIZER;
static int dump_n;

static int alloc_slot(void *arr, int n, size_t stride) { for (int i = 1; i < n; i++) { int *used = (int *)((char *)arr + stride * (size_t)i); if (!*used) { *used = 1; return i; } } return -1; }

static void h_http_init(Cpu *c) {
#ifdef RECOMP_CURL
	static int g; if (!g) { curl_global_init(CURL_GLOBAL_DEFAULT); g = 1; }
#endif
	RET(0);
}
static void h_http_term(Cpu *c) { RET(0); }
static void h_http_pool_stats(Cpu *c) { if (A0) { WR32(A0, 0x40000); WR32(A0 + 4, 0); WR32(A0 + 8, 0); } RET(0); }
static void h_https_load_cert(Cpu *c) { RET(0); }
static void h_http_create_template(Cpu *c) {   /* (userAgent, httpVer, autoProxyConf) */
	pthread_mutex_lock(&http_lock);
	int i = alloc_slot(tmpls, MAXTMPL, sizeof tmpls[0]);
	if (i > 0) snprintf(tmpls[i].ua, sizeof tmpls[i].ua, "%s", A0 ? (const char *)G(A0) : "");
	pthread_mutex_unlock(&http_lock);
	RET(i > 0 ? (uint32_t)i : 0x80431022u);
}
static void h_http_create_connection(Cpu *c) {   /* (tmplId, url, keepalive) */
	if (A0 >= MAXTMPL || !tmpls[A0].used || !A1) { RET(0x80431100u); return; }
	pthread_mutex_lock(&http_lock);
	int i = alloc_slot(conns, MAXCONN, sizeof conns[0]);
	if (i > 0) { conns[i].tmpl = (int)A0; snprintf(conns[i].url, sizeof conns[i].url, "%s", (const char *)G(A1)); }
	pthread_mutex_unlock(&http_lock);
	RET(i > 0 ? (uint32_t)i : 0x80431022u);
}
static void h_http_create_request(Cpu *c) {   /* (connId, method, url, contentLength u64 on the stack) */
	if (A0 >= MAXCONN || !conns[A0].used || !A2) { RET(0x80431100u); return; }
	pthread_mutex_lock(&http_lock);
	int i = alloc_slot(reqs, MAXREQ, sizeof reqs[0]);
	if (i > 0) {
		reqs[i].conn = (int)A0; reqs[i].method = (int)A1; reqs[i].sent = 0; reqs[i].status = 0; reqs[i].headers = 0;
		reqs[i].content_length = (uint64_t)STK(0) | (uint64_t)STK(1) << 32;
		snprintf(reqs[i].url, sizeof reqs[i].url, "%s", (const char *)G(A2));
		reqs[i].resp_head.n = reqs[i].resp_body.n = 0; reqs[i].read_at = 0;
	}
	pthread_mutex_unlock(&http_lock);
	RET(i > 0 ? (uint32_t)i : 0x80431022u);
}
static void h_http_add_header(Cpu *c) {   /* (id, name, value, mode: 0 overwrite, 1 add) */
	if (A0 >= MAXREQ || !reqs[A0].used || !A1) { RET(0x80431100u); return; }
	char line[1024]; snprintf(line, sizeof line, "%s: %s", (const char *)G(A1), A2 ? (const char *)G(A2) : "");
	pthread_mutex_lock(&http_lock);
	if (A3 == 0) {   /* overwrite: drop an earlier header of that name */
		struct curl_slist *keep = 0; size_t nl = strlen((const char *)G(A1));
		for (struct curl_slist *s = reqs[A0].headers; s; s = s->next) if (strncasecmp(s->data, (const char *)G(A1), nl) || s->data[nl] != ':') keep = curl_slist_append(keep, s->data);
		curl_slist_free_all(reqs[A0].headers); reqs[A0].headers = keep;
	}
	reqs[A0].headers = curl_slist_append(reqs[A0].headers, line);
	pthread_mutex_unlock(&http_lock);
	RET(0);
}
#ifdef RECOMP_CURL
static size_t on_body(char *d, size_t sz, size_t n, void *u) { buf_add((Buf *)u, d, sz * n); return sz * n; }
static size_t on_head(char *d, size_t sz, size_t n, void *u) {
	Buf *b = (Buf *)u;
	if (n * sz >= 5 && !strncmp(d, "HTTP/", 5)) b->n = 0;   /* a new response (after a 100 Continue or a redirect): its headers only */
	buf_add(b, d, sz * n); return sz * n;
}
#endif
static int url_host(const char *url, char *host, size_t n, int *port) {   /* scheme://host[:port]/... */
	const char *p = strstr(url, "://"); p = p ? p + 3 : url;
	size_t l = strcspn(p, ":/?#"); if (!l || l >= n) return -1;
	memcpy(host, p, l); host[l] = 0;
	*port = p[l] == ':' ? atoi(p + l + 1) : (!strncmp(url, "https", 5) ? 443 : 80);
	return 0;
}
static int by_name(const char *t) {   /* a server's name (verified like any site) rather than an address or localhost */
	if (!strcasecmp(t, "localhost") || strchr(t, ':')) return 0;   /* (':': an IPv6 address) */
	for (const char *p = t; *p; p++) if (isalpha((unsigned char)*p)) return strchr(t, '.') != 0;
	return 0;   /* digits and dots: an IPv4 address */
}
static void dump(const char *kind, int n, const char *head, const void *body, size_t blen) {
	const char *dir = getenv("RECOMP_NET_DUMP"); if (!dir) return;
	char path[512]; snprintf(path, sizeof path, "%s/%s_%03d.txt", dir, kind, n);
	FILE *f = fopen(path, "wb"); if (!f) return;
	fputs(head, f); fputs("\n", f); if (body && blen) fwrite(body, 1, blen, f); fclose(f);
}
/* ---- the matching server list. Sony's own np_matching2 (recompiled: KZ_NP_MODULES) learns where its matching server is
 * from http://static-resource.<env>.community.playstation.net/np/resource/psp2-title/<id>/matching/<id>-matching.xml:
 *   <!--Sce-Np-Signature: <40 hex digits>-->            (exactly 65 bytes, then \n)
 *   <matching2 status="alive" titleid="<id>"><agent-fqdn id="1" port="<port>" status="alive"><host></agent-fqdn></matching2>
 * the signature an HMAC-SHA1 of what follows the first line, zero-padded to a multiple of 64 bytes, under a 20-byte key
 * the module keeps XORed with 0x7f (np_matching2's FUN_810219fe / FUN_81021470 / FUN_81021c8c..81021e26). Sony's list is
 * never fetched (net_blocked); this one names the matching server the player chose (settings "matching_server" =
 * <host>:<port>, RECOMP_MATCHING_SERVER wins) and is signed here with the key read from the module in memory, so no
 * Sony bytes ship with the project. With no server chosen it names this copy's own matching service (host/online,
 * ONLINE.md). */
uint32_t module_export_addr(const char *name);
static int m2_key(uint8_t key[20]) {   /* the key's address: the movw/movt pair just before the decode loop's eors #0x7f */
	static int found = -1; static uint8_t k[20];
	if (found < 0) {
		found = 0;
		const uint32_t init = module_export_addr("sceNpMatching2Init");
		const uint32_t lo = init & ~0xffffffu, hi = lo + 0x80000;   /* (each module at its own 16 MB base: vitarecomp mod_base) */
		for (uint32_t a = lo; init && a + 4 <= hi && !found; a += 2) {
			if (RD16(a) != 0xf090 || (RD16(a + 2) & 0xf0ff) != 0x007f) continue;   /* eors.w rD, r0, #0x7f (any rD) */
			uint32_t mw = 0, mt = 0;
			for (uint32_t b = a - 2; b + 24 > a && b > lo; b -= 2) {
				const uint16_t h = RD16(b), l = RD16(b + 2);
				const uint32_t imm = (uint32_t)(h & 0xf) << 12 | (uint32_t)(h >> 10 & 1) << 11 | (uint32_t)(l >> 12 & 7) << 8 | (l & 0xff);
				if ((h & 0xfbf0) == 0xf2c0 && !mt) mt = imm | 0x10000;   /* movt */
				else if ((h & 0xfbf0) == 0xf240 && mt && !mw) mw = imm | 0x10000;   /* movw */
			}
			if (mw && mt) { const uint32_t at = (mt & 0xffff) << 16 | (mw & 0xffff); for (int i = 0; i < 20; i++) k[i] = RD8(at + (uint32_t)i) ^ 0x7f; found = 1; if (http_log()) fprintf(stderr, "    http: matching list key at %#x\n", at); }
		}
		if (!found) fprintf(stderr, "    http: np_matching2's list key not found: no matching server list\n");
	}
	if (found) memcpy(key, k, 20);
	return found;
}
static int matching_list(int id, const char *host) {
	const char *url = reqs[id].url, *m = strstr(url, "/matching/");
	if (strncasecmp(host, "static-resource.", 16) || !strstr(host, ".community.playstation.net") || !m) return 0;
	char comm[32]; if (sscanf(m + 10, "%31[^-/]-matching.xml", comm) != 1) return 0;
	char srv[256]; const char *e = getenv("RECOMP_MATCHING_SERVER");
	if (e) snprintf(srv, sizeof srv, "%s", e); else settings_str("matching_server", "", srv, sizeof srv);
	char *colon = strrchr(srv, ':'); int port = colon ? atoi(colon + 1) : 0; if (colon) *colon = 0;
	if (!srv[0]) { port = m2_service_port(); snprintf(srv, sizeof srv, "127.0.0.1"); }   /* none chosen: this copy's own (host/online) */
	uint8_t key[20];
	if (port <= 0 || !m2_key(key)) return 0;
	char xml[1024];
	const int xl = snprintf(xml, sizeof xml, "<matching2 status=\"alive\" titleid=\"%s\">\n<agent-fqdn id=\"1\" port=\"%d\" status=\"alive\">%s</agent-fqdn>\n</matching2>\n", comm, port, srv);
	uint8_t padded[1088] = { 0 }, mac[20]; memcpy(padded, xml, (size_t)xl);
	hmac_sha1(key, 20, padded, ((size_t)xl + 63) & ~(size_t)63, mac);
	char line[80]; int ll = snprintf(line, sizeof line, "<!--Sce-Np-Signature: ");
	for (int i = 0; i < 20; i++) ll += snprintf(line + ll, sizeof line - (size_t)ll, "%02x", mac[i]);
	ll += snprintf(line + ll, sizeof line - (size_t)ll, "-->\n");
	reqs[id].resp_head.n = reqs[id].resp_body.n = 0; reqs[id].read_at = 0;
	buf_add(&reqs[id].resp_body, line, (size_t)ll); buf_add(&reqs[id].resp_body, xml, (size_t)xl);
	char head[160]; const int hl = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %zu\r\n\r\n", reqs[id].resp_body.n);
	buf_add(&reqs[id].resp_head, head, (size_t)hl);
	reqs[id].status = 200; reqs[id].sent = 1;
	fprintf(stderr, "    http: %s -> the matching server list, naming %s:%d\n", url, srv, port);
	return 1;
}
/* ---- game servers answered in this process (net.h http_local_server): a pack's stand-in */
static struct { char host[96]; HttpLocalFn fn; } locals[8]; static int nlocals;
void http_local_server(const char *host, HttpLocalFn fn) {
	if (nlocals < 8) { snprintf(locals[nlocals].host, sizeof locals[nlocals].host, "%s", host); locals[nlocals++].fn = fn; }
}
static int local_answer(int id, const char *host, const char *method, const void *body, size_t blen) {
	HttpLocalFn fn = 0;
	for (int i = 0; i < nlocals && !fn; i++) if (!strcasecmp(locals[i].host, host)) fn = locals[i].fn;
	if (!fn) return 0;
	const char *p = strstr(reqs[id].url, "://"); p = p ? p + 3 : reqs[id].url;
	const char *path = p + strcspn(p, "/?#"); if (!*path) path = "/";
	Buf h = {0};
	for (struct curl_slist *sl = reqs[id].headers; sl; sl = sl->next) { buf_add(&h, sl->data, strlen(sl->data)); buf_add(&h, "\n", 1); }
	HttpLocalReply r = { 404, "text/plain", 0, 0 };
	if (!fn(method, path, h.p ? h.p : "", body, blen, &r)) { free(r.body); r = (HttpLocalReply){ 404, "text/plain", 0, 0 }; }
	free(h.p);
	reqs[id].resp_head.n = reqs[id].resp_body.n = 0; reqs[id].read_at = 0;
	if (r.n && r.body) buf_add(&reqs[id].resp_body, r.body, r.n);
	const char *why = r.status == 200 ? "OK" : r.status == 400 ? "Bad Request" : r.status == 404 ? "Not Found" : "Error";
	char head[256]; const int hl = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", r.status, why, r.ctype ? r.ctype : "text/plain", reqs[id].resp_body.n);
	buf_add(&reqs[id].resp_head, head, (size_t)hl);
	reqs[id].status = r.status; reqs[id].sent = 1;
	if (http_log() || r.status != 200) fprintf(stderr, "    http: %s %s -> answered here (the game's server stand-in), status %d, %zu bytes\n", method, reqs[id].url, r.status, reqs[id].resp_body.n);
	free(r.body);
	return 1;
}
static void h_http_send(Cpu *c) {   /* (reqId, postData, size) */
	uint32_t id = A0;
	if (id >= MAXREQ || !reqs[id].used) { RET(0x80431100u); return; }
	static const char *const methods[8] = { "GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE", "TRACE", "CONNECT" };
	const char *method = reqs[id].method >= 0 && reqs[id].method < 8 ? methods[reqs[id].method] : "GET";
	const char *ua = tmpls[conns[reqs[id].conn].tmpl].ua;
	char host[256], target[256]; int port = 0, tport = 0;
	const int parsed = !url_host(reqs[id].url, host, sizeof host, &port);
	int redirected = parsed && net_redirect_lookup(host, target, sizeof target, &tport);
	if (parsed && matching_list((int)id, host)) { RET(0); return; }
	if (parsed && !redirected && local_answer((int)id, host, method, A1 ? G(A1) : 0, A1 ? A2 : 0)) { RET(0); return; }   /* (the player's own server wins) */
	if (parsed && !redirected && net_blocked(host)) { RET(0x80436007u); return; }   /* (Sony's: as a name that doesn't resolve, RESOLVER_ENOHOST) */
#ifndef RECOMP_CURL   /* (only this process's servers above are reachable) */
	(void)ua; (void)by_name; (void)dump; (void)dump_n;
	fprintf(stderr, "    http: %s %s -> not sent: this build has no HTTP client (libcurl)\n", method, reqs[id].url);
	RET(0x80431063u);   /* NETWORK */
#else
	CURL *e = curl_easy_init();
	if (!e) { RET(0x80431022u); return; }
	reqs[id].resp_head.n = reqs[id].resp_body.n = 0; reqs[id].read_at = 0;
	char url[2048]; snprintf(url, sizeof url, "%s", reqs[id].url);
	const int rewrite = redirected && by_name(target);
	struct curl_slist *hdrs = reqs[id].headers, *own = 0;
	if (rewrite) {   /* scheme://<target>[:port]<path>: the game's request, to the server by its own name */
		const char *p = strstr(reqs[id].url, "://"); p = p ? p + 3 : reqs[id].url;
		const char *path = p + strcspn(p, "/?#");
		int sl = (int)(p - reqs[id].url);
		if (tport) snprintf(url, sizeof url, "%.*s%s:%d%s", sl, reqs[id].url, target, tport, path);
		else snprintf(url, sizeof url, "%.*s%s%s", sl, reqs[id].url, target, path);
		for (struct curl_slist *h = reqs[id].headers; h; h = h->next) if (strncasecmp(h->data, "Host:", 5)) own = curl_slist_append(own, h->data);
		hdrs = own;
	}
	curl_easy_setopt(e, CURLOPT_URL, url);
	curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, method);
	if (!strcmp(method, "HEAD")) curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
	if (A1 && A2) { curl_easy_setopt(e, CURLOPT_POSTFIELDS, (const char *)G(A1)); curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)A2); }
	if (ua[0]) curl_easy_setopt(e, CURLOPT_USERAGENT, ua);
	curl_easy_setopt(e, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body); curl_easy_setopt(e, CURLOPT_WRITEDATA, &reqs[id].resp_body);
	curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_head); curl_easy_setopt(e, CURLOPT_HEADERDATA, &reqs[id].resp_head);
	curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L); curl_easy_setopt(e, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
	struct curl_slist *ct = 0;
	if (redirected && !rewrite) {
		char m[600]; snprintf(m, sizeof m, "%s:%d:%s:%d", host, port, target, tport ? tport : port);
		ct = curl_slist_append(0, m); curl_easy_setopt(e, CURLOPT_CONNECT_TO, ct);
		curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L); curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	const int n = __atomic_add_fetch(&dump_n, 1, __ATOMIC_RELAXED);
	if (getenv("RECOMP_NET_DUMP")) {
		char head[4096]; int hl = snprintf(head, sizeof head, "%s %s\nUser-Agent: %s\n", method, reqs[id].url, ua);
		for (struct curl_slist *s = reqs[id].headers; s && hl < (int)sizeof head - 2; s = s->next) hl += snprintf(head + hl, sizeof head - (size_t)hl, "%s\n", s->data);
		dump("req", n, head, A1 ? G(A1) : 0, A1 ? A2 : 0);
	}
	CURLcode rc = curl_easy_perform(e);
	long status = 0; curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(e); curl_slist_free_all(ct); curl_slist_free_all(own);
	reqs[id].status = (int)status; reqs[id].sent = 1;
	if (http_log() || redirected) fprintf(stderr, "    http: %s %s%s%s%s -> %s, status %ld, %zu bytes\n", method, reqs[id].url, redirected ? (rewrite ? " (as " : " (via ") : "", redirected ? (rewrite ? url : target) : "", redirected ? ")" : "", rc ? curl_easy_strerror(rc) : "ok", status, reqs[id].resp_body.n);
	if (!rc) dump("resp", n, reqs[id].resp_head.p ? reqs[id].resp_head.p : "", reqs[id].resp_body.p, reqs[id].resp_body.n);
	uint32_t err = 0;
	switch (rc) {
	case CURLE_OK: break;
	case CURLE_COULDNT_RESOLVE_HOST: err = 0x80436007u; break;   /* RESOLVER_ENOHOST */
	case CURLE_OPERATION_TIMEDOUT: err = 0x80431068u; break;     /* TIMEOUT */
	case CURLE_SSL_CONNECT_ERROR: err = 0x80435061u; break;      /* HTTPS HANDSHAKE */
	case CURLE_PEER_FAILED_VERIFICATION: err = 0x80435060u; break;   /* HTTPS CERT */
	case CURLE_COULDNT_CONNECT: case CURLE_SEND_ERROR: case CURLE_RECV_ERROR: err = 0x80431063u; break;   /* NETWORK */
	default: err = 0x80431081u; break;                           /* UNKNOWN */
	}
	RET(err);
#endif
}
static void h_http_status(Cpu *c) {
	if (A0 >= MAXREQ || !reqs[A0].used) { RET(0x80431100u); return; }
	if (!reqs[A0].sent) { RET(0x80431065u); return; }
	if (A1) WR32(A1, (uint32_t)reqs[A0].status);
	RET(0);
}
static void h_http_content_length(Cpu *c) {
	if (A0 >= MAXREQ || !reqs[A0].used) { RET(0x80431100u); return; }
	if (!reqs[A0].sent) { RET(0x80431065u); return; }
	if (A1) WR64(A1, (uint64_t)reqs[A0].resp_body.n);
	RET(0);
}
static void h_http_read(Cpu *c) {   /* (reqId, buf, size): the bytes copied, 0 at the end */
	if (A0 >= MAXREQ || !reqs[A0].used) { RET(0x80431100u); return; }
	if (!reqs[A0].sent) { RET(0x80431065u); return; }
	size_t left = reqs[A0].resp_body.n - reqs[A0].read_at, n = left < A2 ? left : A2;
	if (n && A1) { memcpy(G(A1), reqs[A0].resp_body.p + reqs[A0].read_at, n); reqs[A0].read_at += n; }
	RET((uint32_t)n);
}
static void h_http_all_headers(Cpu *c) {   /* (reqId, char **header, unsigned *size): a guest copy */
	if (A0 >= MAXREQ || !reqs[A0].used) { RET(0x80431100u); return; }
	if (!reqs[A0].sent) { RET(0x80431065u); return; }
	uint32_t n = (uint32_t)reqs[A0].resp_head.n;
	if (reqs[A0].ghead_cap < n + 1) { reqs[A0].ghead_cap = n + 1 > 0x4000 ? n + 1 : 0x4000; reqs[A0].ghead = guest_alloc(reqs[A0].ghead_cap, 16); }
	if (n) memcpy(G(reqs[A0].ghead), reqs[A0].resp_head.p, n);
	WR8(reqs[A0].ghead + n, 0);
	if (A1) WR32(A1, reqs[A0].ghead);
	if (A2) WR32(A2, n);
	RET(0);
}
static void h_http_parse_header(Cpu *c) {   /* (header, headerLen, fieldStr, const char **value, unsigned *valueLen) */
	if (!A0 || !A2) { RET(0x804321FEu); return; }
	const char *h = (const char *)G(A0), *f = (const char *)G(A2); size_t hl = A1, fl = strlen(f);
	for (size_t i = 0; i < hl; ) {
		size_t e = i; while (e < hl && h[e] != '\n') e++;
		if (e - i > fl && !strncasecmp(h + i, f, fl) && h[i + fl] == ':') {
			size_t v = i + fl + 1; while (v < e && (h[v] == ' ' || h[v] == '\t')) v++;
			size_t ve = e; while (ve > v && (h[ve - 1] == '\r' || h[ve - 1] == ' ')) ve--;
			if (A3) WR32(A3, A0 + (uint32_t)v);
			if (STK(0)) WR32(STK(0), (uint32_t)(ve - v));
			RET(0); return;
		}
		i = e + 1;
	}
	RET(0x80432025u);   /* PARSE_HTTP_NOT_FOUND */
}
static void h_http_delete_request(Cpu *c) {
	if (A0 < MAXREQ && reqs[A0].used) { pthread_mutex_lock(&http_lock); curl_slist_free_all(reqs[A0].headers); reqs[A0].headers = 0; reqs[A0].used = 0; pthread_mutex_unlock(&http_lock); }
	RET(0);
}
static void h_http_delete_connection(Cpu *c) { if (A0 < MAXCONN) conns[A0].used = 0; RET(0); }
static void h_http_delete_template(Cpu *c) { if (A0 < MAXTMPL) tmpls[A0].used = 0; RET(0); }
static void h_http_abort(Cpu *c) { RET(0); }

const HleEntry hle_http[] = {
	{"sceHttpInit", h_http_init}, {"sceHttpTerm", h_http_term}, {"sceHttpGetMemoryPoolStats", h_http_pool_stats}, {"sceHttpsLoadCert", h_https_load_cert},
	{"sceHttpCreateTemplate", h_http_create_template}, {"sceHttpCreateConnectionWithURL", h_http_create_connection}, {"sceHttpCreateRequestWithURL", h_http_create_request},
	{"sceHttpAddRequestHeader", h_http_add_header}, {"sceHttpSendRequest", h_http_send}, {"sceHttpGetStatusCode", h_http_status},
	{"sceHttpGetResponseContentLength", h_http_content_length}, {"sceHttpReadData", h_http_read}, {"sceHttpGetAllResponseHeaders", h_http_all_headers},
	{"sceHttpParseResponseHeader", h_http_parse_header}, {"sceHttpDeleteRequest", h_http_delete_request}, {"sceHttpDeleteConnection", h_http_delete_connection},
	{"sceHttpDeleteTemplate", h_http_delete_template}, {"sceHttpAbortRequest", h_http_abort},
	{"sceHttpSetConnectTimeOut", h_http_abort}, {"sceHttpSetSendTimeOut", h_http_abort}, {"sceHttpSetRecvTimeOut", h_http_abort},   /* (curl's own timeouts) */
	{"sceHttpRemoveRequestHeader", h_http_abort},   /* (np_matching2 drops Accept, Accept-Encoding, Accept-Charset: curl sends none of its own but Accept) */
	{"sceSslInit", h_http_term}, {"sceSslTerm", h_http_term}, {"sceSslGetMemoryPoolStats", h_http_pool_stats},
	{0, 0},
};
