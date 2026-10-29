/* net.h: the host network layer's shared pieces (net.c) -- the private-server redirect http.c applies too */
#ifndef NET_H
#define NET_H
#include <stddef.h>
#include <stdint.h>

/* settings "net_redirect" (RECOMP_NET_REDIRECT wins) = "<host>=<target>[:<port>][,...]": 1 and the target (and its port, 0
 * when none is given) when `host` is redirected, else 0 */
int net_redirect_lookup(const char *host, char *target, int tsize, int *port);
int net_blocked(const char *host);   /* Sony's own hosts, unless redirected: never contacted */
int net_socket_fd(uint32_t id);   /* a guest socket id's host descriptor, -1 if none */
void net_portmap_add(const char *ip, unsigned from, unsigned to);
void net_set_guest_errno(int host_errno);   /* the calling thread's guest errno (sceNetErrnoLoc), from a host one */
int matching_local(void);   /* ssl_local.c: the game's matching server is this copy's own service (none chosen) */   /* connections to ip:from go to ip:to (the local matching service) */
/* The game plays online: it gets a network and a signed-in PSN account -- net.c, np.c and http.c answer its imports. A game
 * whose pack offers online (RECOMP_GAME_OPTIONS has "online") plays online when the player's "online" setting is on (the
 * default; the launcher's Settings, F10); RECOMP_ONLINE=1|0 decides for a test, and a headless run is offline unless it asks.
 * Otherwise those imports stay the not-implemented stubs every game had before: no network, signed out -- a game whose
 * servers are gone (Unit 13's co-op, its daily challenge) keeps its offline menus instead of opening paths nothing answers */
int net_online(void);
/* the update online play needs ("1.12"), when this copy isn't a version the pack's ONLINE_VERSIONS names (RECOMP_ONLINE_NEEDS,
 * set by vitarecomp): online stays off, and the player is told (apputil.c's network check, the F10 menu); 0 otherwise */
const char *net_online_needs(void);

/* http.c: a game server answered inside this process -- a pack's stand-in for its long-gone server (Killzone's server.c),
 * used unless the player names a server of their own for that host (settings private_server, net_redirect). The handler
 * fills the reply (status, content type, a malloc'd body http.c frees); it returns 0 for a path it doesn't serve (404).
 * headers: the request's, "Name: value" lines. Register from a constructor: before the game's first request. */
typedef struct { int status; const char *ctype; char *body; size_t n; } HttpLocalReply;
typedef int (*HttpLocalFn)(const char *method, const char *path, const char *headers, const void *body, size_t blen, HttpLocalReply *r);
void http_local_server(const char *host, HttpLocalFn fn);

#endif
