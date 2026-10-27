/*
 * Unit 13 (PCSA00014): a stand-in for Zipper Interactive's online files, answered inside the game's own process
 * (runtime/net/http.c http_local_server). The game fetches them over plain HTTP from Amazon S3 buckets Zipper ran, which
 * are long gone (403); answering here also means nothing goes to S3.
 *   /zipperint/zp3/netsettings.xml  its co-op session's ping settings, read by FUN_81079bc0 as children of one root
 *                                   element: PingInterval (a float, +0x34), PingWindowSize (an integer, at most 32, +0xb8),
 *                                   PingDisconnectThreshold (an integer, +0xcc). Without them two copies connected and
 *                                   then said nothing, the host's DEPLOY greyed (2026-10-26). The values are ours: Zipper's
 *                                   file isn't available. Units unknown, so they are safe either way: an interval of 0.5
 *                                   (twice a second, or every frame), a threshold of 15000 (15 s, or never)
 *   anything else (zp3/motd/motd.xml, the message of the day; zp3_eula/index.xml, the EULA)  404, as the game met them
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"

static const char netsettings[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<NetSettings>\n"
	"  <PingInterval>0.5</PingInterval>\n"
	"  <PingWindowSize>8</PingWindowSize>\n"
	"  <PingDisconnectThreshold>15000</PingDisconnectThreshold>\n"
	"</NetSettings>\n";

static int u13_zipper(const char *method, const char *path, const char *headers, const void *body, size_t blen, HttpLocalReply *r) {
	(void)method; (void)headers; (void)body; (void)blen;
	if (!strcmp(path, "/zipperint/zp3/netsettings.xml")) {
		const size_t n = sizeof netsettings - 1;
		if (!(r->body = malloc(n))) return 0;
		memcpy(r->body, netsettings, n); r->n = n; r->status = 200; r->ctype = "text/xml";
		return 1;
	}
	return 0;   /* (404: the message of the day and the EULA the game goes on without) */
}
__attribute__((constructor)) static void u13_zipper_register(void) { http_local_server("s3.amazonaws.com", u13_zipper); }
