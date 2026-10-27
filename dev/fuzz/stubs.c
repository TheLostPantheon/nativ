/* What the online service's parsers reach outside their own files, as do-nothing stand-ins for the fuzzers (dev/fuzz):
 * weak, so a harness that links the real file gets the real one. Deterministic: no sockets, threads or settings. */
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "m2_internal.h"
#include "overlay.h"
#define W __attribute__((weak))

W int m2_log(void) { return 0; }
W void plat_random(void *b, size_t n) { memset(b, 0x5a, n); }
W void plat_sleep_ns(uint64_t ns) { (void)ns; }
W int settings_int(const char *k, int d) { (void)k; return d; }
W const char *settings_str(const char *k, const char *d, char *buf, int n) { (void)k; snprintf(buf, (size_t)n, "%s", d ? d : ""); return buf; }
W void settings_set_own(const char *k, const char *v) { (void)k; (void)v; }
W void settings_save(void) {}
W void overlay_toast_titled(const char *t, const char *b, double s) { (void)t; (void)b; (void)s; }
W void np_online_id(char out[17]) { snprintf(out, 17, "ntv_fuzz"); }
W uint64_t np_account_id(void) { return 0x1234; }
W void net_host_ipv4(char *ip, size_t n) { snprintf(ip, n, "10.0.0.2"); }
W void net_portmap_add(const char *ip, unsigned from, unsigned to) { (void)ip; (void)from; (void)to; }
W void bmsg_input(const uint8_t *b, size_t n) { (void)b; (void)n; }
W int bmsg_start(void) { return 0; }
W void dht_input(const uint8_t *b, size_t n, const struct sockaddr_in *from) { (void)b; (void)n; (void)from; }
W void dht_start(const char comm[12], uint16_t port) { (void)comm; (void)port; }
W void dht_tick(void) {}
W void upnp_start(uint16_t port) { (void)port; }
W void disc_input(const uint8_t *b, size_t n, uint32_t vip) { (void)b; (void)n; (void)vip; }
W void disc_start(const char *comm) { (void)comm; }
W int disc_rooms(RemoteRoom *out, int max) { (void)out; (void)max; return 0; }
W void disc_poke(void) {}
W int disc_room_server(const char id[16], char host[64], uint16_t *port) { (void)id; (void)host; (void)port; return -1; }
W int disc_player(const char id[16], char host[64], uint16_t *port) { (void)id; (void)host; (void)port; return -1; }
W int disc_name_clash(void) { return 0; }
W int m2_rooms_snapshot(RoomAd *out, int max) { (void)out; (void)max; return 0; }
W int m2_room_port(void) { return 4000; }
W int m2_proxy_port(uint16_t world, uint16_t room, const char *host, int port) { (void)world; (void)room; (void)host; (void)port; return 0; }
W int ov_on(void) { return 0; }
W int ov_start(const char comm[12]) { (void)comm; return 0; }
W uint64_t ov_self_id(void) { return 1; }
W int ov_is_vip(uint32_t ip) { (void)ip; return 0; }
W uint32_t ov_vip_of(uint64_t id) { return (uint32_t)id; }
W void ov_allow_port(uint16_t port) { (void)port; }
W void ov_broadcast_disc(const uint8_t *d, size_t n) { (void)d; (void)n; }
W int ov_send_basic(uint32_t vip, const uint8_t *d, size_t n) { (void)vip; (void)d; (void)n; return -1; }
W void ov_probe(const char *host, uint16_t port) { (void)host; (void)port; }
W void ov_add_candidate(uint64_t id, uint32_t ip, uint16_t port) { (void)id; (void)ip; (void)port; }
W int ov_stream_open(uint32_t vip, uint16_t port) { (void)vip; (void)port; return -1; }
W void ov_probe_addr(uint32_t ip, uint16_t p) { (void)ip; (void)p; }
W void ov_send_udp(const struct sockaddr_in *to, const uint8_t *b, size_t n) { (void)to; (void)b; (void)n; }
W void online_status_tick(void) {}
W int settings_shared_dir(char *buf, int n) { (void)buf; (void)n; return 0; }
W int ov_counts(int *linked, int *known) { *linked = *known = 0; return 0; }
W int dht_counts(char where[32]) { where[0] = 0; return 0; }
W int disc_status(int *rooms) { *rooms = 0; return 0; }
