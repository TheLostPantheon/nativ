/*
 * Killzone: Mercenary (PCSA00107): a stand-in for its game server, kzv.online.scee.com (shut down by Sony), answered inside
 * the game's own process (runtime/net/http.c http_local_server) so a player needs nothing else running to play online:
 * the game logs in to it before Mercenary Warfare's matchmaking. servers/kzserver/kzserver.py is the same server as its own
 * program (a community server, tests); a player who names a private server (settings private_server) reaches that one
 * instead. Written from what the game's client sends and parses (eboot 0x815a5xxx: the request builders, the SAX handlers
 * reading the replies); nothing here comes from Sony's server or anyone else's.
 *
 * Every request is a POST of an application/xml body (User-Agent killzonengp/469; after login, an x-auth-token header):
 *   /user/login                   xmlLoginRequest -> xmlLoginResponse: userProfile(valour, country, region), owner(uid),
 *                                 authToken, environmentId (the reply's elements are matched by name wherever they are)
 *   /user/heartbeat               xmlHeartbeatRequest(activity) -> xmlResult
 *   /games/createmultiplayergame  xmlCreateOnlineGameRequest -> xmlCreateOnlineGameResponse(gameId attribute)
 *   /games/joinmultiplayergame    xmlJoinOnlineGameRequest -> xmlResult
 *   /user/statUpdate              xmlStatUpdate (the game's own reports) -> xmlResult
 *   /roundhistory/<kind>/append/<id>  the round's scores, a binary blob -> xmlResult
 * Anything else is answered 404 and logged (the game reports it as the server rejecting it). The uid is kzserver's:
 * the first 16 hex digits of SHA-1(online id), so a player keeps it between this and a community kzserver. Valour stays 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include "sha1.h"

void np_online_id(char out[17]);
void plat_random(void *buf, size_t n);

#define XML_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
static int reply(HttpLocalReply *r, const char *fmt, const char *a, const char *b) {
	char x[1024]; const int n = snprintf(x, sizeof x, fmt, a ? a : "", b ? b : "");
	if (n < 0 || n >= (int)sizeof x || !(r->body = malloc((size_t)n))) return 0;
	memcpy(r->body, x, (size_t)n); r->n = (size_t)n; r->status = 200; r->ctype = "application/xml";
	return 1;
}
static void hex(const uint8_t *b, size_t n, char *out) { for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]); }
static int kz_server(const char *method, const char *path, const char *headers, const void *body, size_t blen, HttpLocalReply *r) {
	(void)method; (void)headers; (void)body; (void)blen;
	if (!strncmp(path, "/user/login", 11)) {
		char oid[17]; np_online_id(oid);
		uint8_t h[20], t[16]; char uid[33], tok[33]; sha1(oid, strlen(oid), h); hex(h, 8, uid); plat_random(t, sizeof t); hex(t, sizeof t, tok);
		fprintf(stderr, "    kz server: %s signed in (uid %s)\n", oid, uid);
		return reply(r, XML_HEAD "<xmlLoginResponse><userProfile valour=\"0\" country=\"us\" region=\"0\"/><owner uid=\"%s\"/><authToken>%s</authToken>"
		                "<environmentId>1</environmentId></xmlLoginResponse>", uid, tok);
	}
	if (!strncmp(path, "/games/createmultiplayergame", 28)) {
		uint8_t g[8]; char gid[17]; plat_random(g, sizeof g); hex(g, sizeof g, gid);
		return reply(r, XML_HEAD "<xmlCreateOnlineGameResponse gameId=\"%s\"/>", gid, 0);
	}
	if (!strncmp(path, "/user/heartbeat", 15) || !strncmp(path, "/games/joinmultiplayergame", 26) || !strncmp(path, "/user/statUpdate", 16) || !strncmp(path, "/roundhistory/", 14))
		return reply(r, XML_HEAD "<xmlResult/>", 0, 0);
	fprintf(stderr, "    kz server: %s %s isn't served here yet (%zu bytes): the game hears 404\n", method, path, blen);
	return 0;
}
__attribute__((constructor)) static void kz_server_register(void) { http_local_server("kzv.online.scee.com", kz_server); http_local_server("beta.kzv.online.scee.com", kz_server); }
