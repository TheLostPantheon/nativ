/* UPnP port mapping (ONLINE.md, milestone 3): the home router forwards the overlay's UDP port to this copy, so other
 * copies reach it without hole punching (and a copy behind a strict NAT, a phone's carrier's, still reaches one with
 * a mapping). The router is found by SSDP (M-SEARCH for an InternetGatewayDevice), its description read for the
 * WANIPConnection (or WANPPPConnection) control URL, and AddPortMapping asked for: the same external port when free,
 * else the next ones; a lease of an hour, renewed every 30 minutes, removed when the game exits (a crash leaves at most
 * the hour). Its own little HTTP client (no libcurl: Windows has none). Opt-in (settings online_upnp=1): a mapping opens
 * the player's machine to the internet on that port, the player's call to make. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
#endif
#include "m2_internal.h"

int settings_int(const char *key, int def);
void dht_set_port(uint16_t port);   /* (dht.c: the port it announces) */

static char ctl_host[64], ctl_path[256], service[80], lan_ip[32]; static int ctl_port;
static uint16_t internal_port, external_port; static int mapped;

static int http(const char *host, int port, const char *req, char *out, size_t cap) {   /* the response, or -1 */
	struct addrinfo hints, *res = 0; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
	char ps[8]; snprintf(ps, sizeof ps, "%d", port);
	if (getaddrinfo(host, ps, &hints, &res) || !res) return -1;
	const int s = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0 || connect(s, res->ai_addr, (socklen_t)res->ai_addrlen)) { if (s >= 0) CLOSESOCK(s); freeaddrinfo(res); return -1; }
	freeaddrinfo(res);
	send(s, req, (int)strlen(req), 0);
	size_t n = 0;
	for (;;) {
		struct pollfd pf = { s, POLLIN, 0 };
#ifdef _WIN32
		if (WSAPoll(&pf, 1, 3000) <= 0) break;
#else
		if (poll(&pf, 1, 3000) <= 0) break;
#endif
		const int k = (int)recv(s, out + n, (int)(cap - 1 - n), 0);
		if (k <= 0) break;
		n += (size_t)k; if (n >= cap - 1) break;
	}
	CLOSESOCK(s); out[n] = 0;
	return (int)n;
}
static int split_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn) {   /* http://host[:port]/path */
	if (strncasecmp(url, "http://", 7)) return -1;
	const char *h = url + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
	if (!slash) slash = h + strlen(h);
	if (colon && colon < slash) { snprintf(host, hn, "%.*s", (int)(colon - h), h); *port = atoi(colon + 1); }
	else { snprintf(host, hn, "%.*s", (int)(slash - h), h); *port = 80; }
	snprintf(path, pn, "%s", *slash ? slash : "/");
	return 0;
}
static const char *tag(const char *xml, const char *name, char *out, size_t n) {   /* the text of <name>...</name> */
	char open[64]; snprintf(open, sizeof open, "<%s>", name);
	const char *a = strstr(xml, open); if (!a) return 0;
	a += strlen(open); const char *b = strchr(a, '<'); if (!b) return 0;
	snprintf(out, n, "%.*s", (int)(b - a), a); return a;
}
static int soap(const char *action, const char *args, char *out, size_t cap) {   /* the HTTP status */
	char body[1024], req[2048];
	snprintf(body, sizeof body, "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
	         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
	         action, service, args, action);
	snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n"
	         "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", ctl_path, ctl_host, ctl_port, service, action, strlen(body), body);
	if (http(ctl_host, ctl_port, req, out, cap) <= 0) return -1;
	return strncmp(out, "HTTP/1.", 7) ? -1 : atoi(out + 9);
}
static int find_gateway(void) {   /* SSDP, then the description: 0 when a control URL was found */
	const int s = (int)socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return -1;
	struct sockaddr_in g; memset(&g, 0, sizeof g); g.sin_family = AF_INET; g.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &g.sin_addr);
	const char *m = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
	sendto(s, m, (int)strlen(m), 0, (struct sockaddr *)&g, sizeof g);
	char b[2048], loc[256] = "";
	for (int k = 0; k < 4 && !loc[0]; k++) {
		struct pollfd pf = { s, POLLIN, 0 };
#ifdef _WIN32
		if (WSAPoll(&pf, 1, 1000) <= 0) continue;
#else
		if (poll(&pf, 1, 1000) <= 0) continue;
#endif
		struct sockaddr_in from; socklen_t fl = sizeof from;
		const int n = (int)recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl); if (n <= 0) continue; b[n] = 0;
		for (char *l = b; l && *l; ) {
			char *e = strstr(l, "\r\n"); if (e) *e = 0;
			if (!strncasecmp(l, "LOCATION:", 9)) { const char *v = l + 9; while (*v == ' ') v++; snprintf(loc, sizeof loc, "%s", v); }
			l = e ? e + 2 : 0;
		}
		if (loc[0]) {   /* (the address the router answered from: our LAN address toward it) */
			const int u = (int)socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in me; socklen_t ml = sizeof me;
			if (u >= 0 && !connect(u, (struct sockaddr *)&from, sizeof from) && !getsockname(u, (struct sockaddr *)&me, &ml)) inet_ntop(AF_INET, &me.sin_addr, lan_ip, sizeof lan_ip);
			if (u >= 0) CLOSESOCK(u);
		}
	}
	CLOSESOCK(s);
	if (!loc[0] || !lan_ip[0]) return -1;
	char host[64], path[256]; int port;
	if (split_url(loc, host, sizeof host, &port, path, sizeof path)) return -1;
	static char desc[65536]; char req[512];
	snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
	if (http(host, port, req, desc, sizeof desc) <= 0) return -1;
	static const char *const types[] = { "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANPPPConnection:1" };
	for (size_t t = 0; t < sizeof types / sizeof *types; t++) {
		const char *at = strstr(desc, types[t]); if (!at) continue;
		char url[256]; if (!tag(at, "controlURL", url, sizeof url)) continue;
		snprintf(service, sizeof service, "%s", types[t]);
		if (!strncasecmp(url, "http://", 7)) split_url(url, ctl_host, sizeof ctl_host, &ctl_port, ctl_path, sizeof ctl_path);
		else { snprintf(ctl_host, sizeof ctl_host, "%s", host); ctl_port = port; snprintf(ctl_path, sizeof ctl_path, "%s%s", url[0] == '/' ? "" : "/", url); }
		return 0;
	}
	return -1;
}
static int add_mapping(uint16_t ext, int lease) {   /* the HTTP status (200: done) */
	char args[512], out[4096];
	snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
	         "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
	         "<NewPortMappingDescription>Project Nativ</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>", ext, internal_port, lan_ip, lease);
	const int st = soap("AddPortMapping", args, out, sizeof out);
	if (st != 200 && m2_log()) { char code[16] = "?"; tag(out, "errorCode", code, sizeof code); fprintf(stderr, "    m2: upnp: port %u: refused (HTTP %d, error %s)\n", ext, st, code); }
	return st;
}
static void unmap(void) {
	if (!mapped) return;
	char args[256], out[2048]; snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>", external_port);
	soap("DeletePortMapping", args, out, sizeof out); mapped = 0;
}
static void *upnp_main(void *arg) {
	(void)arg;
	if (find_gateway()) { fprintf(stderr, "    m2: upnp: no router answered (the overlay punches through instead)\n"); return 0; }
	int lease = 3600;
	for (uint16_t e = internal_port; e < internal_port + 8 && !mapped; e++) {
		int st = add_mapping(e, lease);
		if (st == 500 && lease) st = add_mapping(e, lease = 0);   /* (a router that only takes permanent leases) */
		if (st == 200) { mapped = 1; external_port = e; }
	}
	if (!mapped) { fprintf(stderr, "    m2: upnp: the router at %s:%d refused a mapping\n", ctl_host, ctl_port); return 0; }
	char out[4096], ext_ip[64] = "?"; if (soap("GetExternalIPAddress", "", out, sizeof out) == 200) tag(out, "NewExternalIPAddress", ext_ip, sizeof ext_ip);
	fprintf(stderr, "    m2: upnp: the router forwards UDP %s:%u to this copy (%s:%u)\n", ext_ip, external_port, lan_ip, internal_port);
	dht_set_port(external_port);
	atexit(unmap);
	for (;;) {   /* (renewed before the lease ends) */
		struct timespec t = { 1800, 0 }; nanosleep(&t, 0);
		if (lease) add_mapping(external_port, lease);
	}
	return 0;
}
void upnp_start(uint16_t port) {
	const char *e = getenv("RECOMP_ONLINE_UPNP");
	if (e ? atoi(e) <= 0 : settings_int("online_upnp", 0) <= 0) return;
	internal_port = port;
	pthread_t t; pthread_create(&t, 0, upnp_main, 0); pthread_detach(t);
}
