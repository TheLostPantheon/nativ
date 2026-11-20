/* vitasync.cpp: see vitasync.h. The protocol's crypto and hashes are third_party/nativ-sync/nsync_proto.c, the same
 * file the Vita runs, over Monocypher.
 *
 * Files (the settings folder): vita-sync.ini (this computer's id and name, the paired Vita and its key, the games
 * turned off), vita-sync/<vita id>/<TITLE>.base (each game's save as it was at the last sync: its manifest hash).
 * Nativ's backups of a save it replaces: <library>/backups/vita-sync/<TITLE>/<time>/. */
#include "vitasync.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
extern "C" {
#include "nsync_proto.h"
}
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#endif

namespace fs = std::filesystem;
namespace vsync {

static const int PORT = 1340;
static const size_t CHUNK = NSYNC_PLAIN_MAX - 512;   /* file bytes per frame, the request line besides */
static const int SLOT_PARAM = 0x34C;
static const int KEEP_BACKUPS = 10;

/* ---- small things ---- */
static fs::path P(const std::string &s) { return fs::u8path(s); }
static std::string U(const fs::path &p) { return p.u8string(); }
static std::string hex(const uint8_t *b, size_t n) { std::string s(2 * n, 0); nsync_hex(&s[0], b, n); s.resize(2 * n); return s; }
static bool unhex(const std::string &s, uint8_t *out, size_t n) { return s.size() >= 2 * n && nsync_unhex(out, n, s.c_str()) == 0; }
static void random_bytes(uint8_t *p, size_t n) {
#ifdef _WIN32
	BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
	FILE *f = fopen("/dev/urandom", "rb"); size_t got = f ? fread(p, 1, n, f) : 0; if (f) fclose(f);
	if (got != n) abort();   /* (no randomness: never pair or talk with a guessable key) */
#endif
}
static std::string read_file(const fs::path &p) {
	std::string s; FILE *f = fopen(U(p).c_str(), "rb"); if (!f) return s;
	char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s;
}
static bool write_file(const fs::path &p, const std::string &data) {
	std::error_code ec; fs::create_directories(p.parent_path(), ec);
	FILE *f = fopen(U(p).c_str(), "wb"); if (!f) return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size(); return fclose(f) == 0 && ok;
}
static std::string blake_hex(const std::string &data) {
	uint8_t h[32]; NsyncHash x; nsync_hash_init(&x); nsync_hash_update(&x, data.data(), data.size()); nsync_hash_final(&x, h); return hex(h, 32);
}
static const std::string EMPTY_HASH = blake_hex("");

/* "YYYY-MM-DD HH:MM" as a person says it: "Today, 18:02", "Yesterday, ...", "19 Nov, ...", "19 Nov 2025, ..." */
static std::string friendly(int y, int mo, int d, int h, int mi) {
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	if (y < 2000 || mo < 1 || mo > 12) return "";
	time_t now = time(nullptr); struct tm t = *localtime(&now), yd;
	time_t yes = now - 86400; yd = *localtime(&yes);
	char b[64];
	if (y == t.tm_year + 1900 && mo == t.tm_mon + 1 && d == t.tm_mday) snprintf(b, sizeof b, "Today, %02d:%02d", h, mi);
	else if (y == yd.tm_year + 1900 && mo == yd.tm_mon + 1 && d == yd.tm_mday) snprintf(b, sizeof b, "Yesterday, %02d:%02d", h, mi);
	else if (y == t.tm_year + 1900) snprintf(b, sizeof b, "%d %s, %02d:%02d", d, months[mo - 1], h, mi);
	else snprintf(b, sizeof b, "%d %s %d, %02d:%02d", d, months[mo - 1], y, h, mi);
	return b;
}
static std::string friendly(const std::string &stamp) {   /* "YYYY-MM-DD HH:MM" */
	int y, mo, d, h, mi; return sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5 ? friendly(y, mo, d, h, mi) : "";
}
static std::string stamp_of(fs::file_time_type ft) {
	auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
	time_t tt = std::chrono::system_clock::to_time_t(sys); struct tm t = *localtime(&tt);
	char b[32]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min); return b;
}
static void set_mtime(const fs::path &p, const std::string &stamp) {   /* "YYYY-MM-DD HH:MM", local time */
	struct tm t; memset(&t, 0, sizeof t); t.tm_isdst = -1;
	if (sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min) != 5) return;
	t.tm_year -= 1900; t.tm_mon -= 1;
	time_t tt = mktime(&t); if (tt == (time_t)-1) return;
	auto ft = fs::file_time_type::clock::now() + std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::system_clock::from_time_t(tt) - std::chrono::system_clock::now());
	std::error_code ec; fs::last_write_time(p, ft, ec);
}
static std::string now_stamp(void) {
	time_t now = time(nullptr); struct tm t = *localtime(&now); char b[32];
	snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec); return b;
}
static bool same_version(const std::string &a, const std::string &b) {   /* "01.08" is "1.08" */
	int a1 = -1, a2 = -1, b1 = -2, b2 = -2;
	return sscanf(a.c_str(), "%d.%d", &a1, &a2) == 2 && sscanf(b.c_str(), "%d.%d", &b1, &b2) == 2 && a1 == b1 && a2 == b2;
}
static std::string plain_version(const std::string &v) { int a, b; char o[16]; if (sscanf(v.c_str(), "%d.%d", &a, &b) != 2) return v; snprintf(o, sizeof o, "%d.%02d", a, b); return o; }

/* ---- JSON (the Vita's replies) ---- */
struct J {
	enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
	bool b = false; double n = 0; std::string s; std::vector<J> a; std::vector<std::pair<std::string, J>> o;
	const J &operator[](const char *k) const { static J none; for (auto &kv : o) if (kv.first == k) return kv.second; return none; }
	std::string str(void) const { return t == STR ? s : ""; }
	long long num(void) const { return t == NUM ? (long long)n : 0; }
};
struct JP {
	const char *p, *e;
	void ws(void) { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
	static void utf8(std::string &out, unsigned c) {
		if (c < 0x80) out += (char)c;
		else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 63)); }
		else if (c < 0x10000) { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | (c >> 6 & 63)); out += (char)(0x80 | (c & 63)); }
		else { out += (char)(0xF0 | c >> 18); out += (char)(0x80 | (c >> 12 & 63)); out += (char)(0x80 | (c >> 6 & 63)); out += (char)(0x80 | (c & 63)); }
	}
	bool string(std::string &out) {
		if (p >= e || *p != '"') return false;
		for (p++; p < e && *p != '"'; p++) {
			if (*p != '\\') { out += *p; continue; }
			if (++p >= e) return false;
			char c = *p;
			if (c == 'n') out += '\n'; else if (c == 't') out += '\t'; else if (c == 'r') out += '\r'; else if (c == 'b') out += '\b'; else if (c == 'f') out += '\f';
			else if (c == 'u') {
				if (e - p < 5) return false;
				unsigned u = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16); p += 4;
				if (u >= 0xD800 && u < 0xDC00 && e - p >= 7 && p[1] == '\\' && p[2] == 'u') { unsigned l = (unsigned)strtoul(std::string(p + 3, 4).c_str(), nullptr, 16); u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); p += 6; }
				utf8(out, u);
			} else out += c;
		}
		if (p >= e) return false;
		p++; return true;
	}
	bool value(J &v, int depth) {
		ws(); if (p >= e || depth > 32) return false;
		if (*p == '{') {
			v.t = J::OBJ; p++; ws(); if (p < e && *p == '}') { p++; return true; }
			for (;;) { ws(); std::string k; if (!string(k)) return false; ws(); if (p >= e || *p++ != ':') return false;
				J x; if (!value(x, depth + 1)) return false; v.o.emplace_back(k, std::move(x)); ws();
				if (p < e && *p == ',') { p++; continue; } if (p < e && *p == '}') { p++; return true; } return false; }
		}
		if (*p == '[') {
			v.t = J::ARR; p++; ws(); if (p < e && *p == ']') { p++; return true; }
			for (;;) { J x; if (!value(x, depth + 1)) return false; v.a.push_back(std::move(x)); ws();
				if (p < e && *p == ',') { p++; continue; } if (p < e && *p == ']') { p++; return true; } return false; }
		}
		if (*p == '"') { v.t = J::STR; return string(v.s); }
		if (!strncmp(p, "true", std::min<size_t>(4, e - p)) && e - p >= 4) { v.t = J::BOOL; v.b = true; p += 4; return true; }
		if (!strncmp(p, "false", std::min<size_t>(5, e - p)) && e - p >= 5) { v.t = J::BOOL; p += 5; return true; }
		if (!strncmp(p, "null", std::min<size_t>(4, e - p)) && e - p >= 4) { p += 4; return true; }
		char *end; std::string num(p, std::min<size_t>(40, e - p)); v.n = strtod(num.c_str(), &end);
		if (end == num.c_str()) return false;
		v.t = J::NUM; p += end - num.c_str(); return true;
	}
};
static bool parse(const std::string &s, J &out) { JP jp{s.data(), s.data() + s.size()}; return jp.value(out, 0); }

/* ---- state ---- */
static std::mutex mu;          /* the state below */
static std::mutex op_mu;       /* sync operations, one at a time */
static std::string g_pref, g_backups, g_pc_name;
static std::map<std::string, std::string> cfg;   /* vita-sync.ini */
static std::vector<Game> library;
static std::string playing;
static std::map<std::string, Status> statuses;
static std::set<std::string> vita_wins;          /* "keep the Vita's" answered at a launch there: take its save when the game stops */
static std::atomic<bool> is_online{false}, run{false};
static std::thread worker;
static std::string vita_account;

static fs::path cfg_path(void) { return P(g_pref) / "vita-sync.ini"; }
static void cfg_load(void) {
	cfg.clear();
	std::string s = read_file(cfg_path()); size_t at = 0;
	while (at < s.size()) {
		size_t nl = s.find('\n', at); if (nl == std::string::npos) nl = s.size();
		std::string l = s.substr(at, nl - at); at = nl + 1;
		size_t eq = l.find('='); if (eq != std::string::npos) cfg[l.substr(0, eq)] = l.substr(eq + 1);
	}
}
static void cfg_save(void) {   /* (holds the pairing key: readable by this user only) */
	std::string s; for (auto &kv : cfg) s += kv.first + "=" + kv.second + "\n";
	write_file(cfg_path(), s);
#ifndef _WIN32
	chmod(U(cfg_path()).c_str(), 0600);
#endif
}
static std::string get(const char *k) { std::lock_guard<std::mutex> l(mu); auto it = cfg.find(k); return it == cfg.end() ? "" : it->second; }
static void set(const char *k, const std::string &v) { std::lock_guard<std::mutex> l(mu); cfg[k] = v; cfg_save(); }
static void set_status(const std::string &id, const std::string &text, int tone) { std::lock_guard<std::mutex> l(mu); statuses[id] = {text, tone}; }
static fs::path base_path(const std::string &id) { return P(g_pref) / "vita-sync" / get("vita_id") / (id + ".base"); }
static std::string base_of(const std::string &id) { std::string b = read_file(base_path(id)); return b.size() >= 64 ? b.substr(0, 64) : ""; }

void init(const std::string &pref_dir, const std::string &backups_dir, const std::string &pc_name) {
	std::lock_guard<std::mutex> l(mu);
	g_pref = pref_dir; g_backups = backups_dir; g_pc_name = pc_name.empty() ? "Computer" : pc_name;
	cfg_load();
	if (cfg["pc_id"].size() != 16) { uint8_t b[8]; random_bytes(b, 8); cfg["pc_id"] = hex(b, 8); cfg_save(); }
	vita_account = cfg["account"];
#ifdef _WIN32
	static bool wsa; if (!wsa) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); wsa = true; }
#endif
}
bool paired(void) { return get("key").size() == 64 && get("vita_id").size() == 16; }
bool online(void) { return is_online; }
std::string vita_name(void) { return get("vita_name"); }
std::string account_id(void) {   /* (the registry's 8 bytes, little-endian, as the service sends them: the number reads them reversed) */
	std::lock_guard<std::mutex> l(mu);
	if (vita_account.size() != 16) return "";
	std::string r; for (int i = 14; i >= 0; i -= 2) r += vita_account.substr(i, 2);
	return r;
}
static void (*on_synced)(const std::string &id);
void set_on_synced(void (*f)(const std::string &id)) { on_synced = f; }
static void synced(const std::string &id) { if (on_synced) on_synced(id); }
bool enabled(void) { return get("enabled") != "0"; }
void set_enabled(bool on) { set("enabled", on ? "1" : "0"); }
bool game_on(const std::string &id) { std::string off = "," + get("off") + ","; return off.find("," + id + ",") == std::string::npos; }
struct Conn; static bool connect_vita(Conn &c, int ms);
static void clear_guard(const std::string &id);
void set_game_on(const std::string &id, bool on) {
	std::string off = get("off"), out; size_t at = 0;
	while (at <= off.size()) { size_t c = off.find(',', at); if (c == std::string::npos) c = off.size(); std::string x = off.substr(at, c - at); if (!x.empty() && x != id) out += (out.empty() ? "" : ",") + x; at = c + 1; }
	if (!on) out += (out.empty() ? "" : ",") + id;
	set("off", out);
	if (!on && paired()) {   /* the Vita stops asking about it too */
		std::lock_guard<std::mutex> l(mu); statuses.erase(id);
		std::thread([id] { clear_guard(id); }).detach();
	}
}
void set_library(const std::vector<Game> &games) { std::lock_guard<std::mutex> l(mu); library = games; }
void set_playing(const std::string &id) { std::lock_guard<std::mutex> l(mu); playing = id; }
static std::string now_playing(void) { std::lock_guard<std::mutex> l(mu); return playing; }
Status status(const std::string &id) { std::lock_guard<std::mutex> l(mu); auto it = statuses.find(id); return it == statuses.end() ? Status{} : it->second; }
std::string line(void) {
	if (!paired()) return "Not paired";
	return "Paired with " + vita_name() + (!enabled() ? " \xC2\xB7 off" : online() ? " \xC2\xB7 connected" : " \xC2\xB7 not on the network right now");
}

/* ---- the network ---- */
static bool set_timeout(sock_t s, int ms) {
#ifdef _WIN32
	DWORD t = (DWORD)ms; return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t) == 0;
#else
	struct timeval t = {ms / 1000, (ms % 1000) * 1000}; return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t) == 0;
#endif
}
static sock_t dial(const std::string &addr, int ms) {   /* a TCP connection, or BAD_SOCK within ms */
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(PORT);
	if (inet_pton(AF_INET, addr.c_str(), &a.sin_addr) != 1) return BAD_SOCK;
	sock_t s = socket(AF_INET, SOCK_STREAM, 0); if (s == BAD_SOCK) return s;
#ifdef _WIN32
	u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
	int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
	connect(s, (struct sockaddr *)&a, sizeof a);
	fd_set w; FD_ZERO(&w); FD_SET(s, &w); struct timeval t = {ms / 1000, (ms % 1000) * 1000};
	int err = 0; socklen_t el = sizeof err;
	if (select((int)s + 1, nullptr, &w, nullptr, &t) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0 || err) { close_sock(s); return BAD_SOCK; }
#ifdef _WIN32
	nb = 0; ioctlsocket(s, FIONBIO, &nb);
#else
	fcntl(s, F_SETFL, fl);
#endif
	set_timeout(s, 30000);
	return s;
}
static bool send_all(sock_t s, const void *b, size_t n) {
	for (size_t done = 0; done < n; ) { int w = send(s, (const char *)b + done, (int)(n - done), 0); if (w <= 0) return false; done += (size_t)w; }
	return true;
}
static bool recv_all(sock_t s, void *b, size_t n) {
	for (size_t done = 0; done < n; ) { int r = recv(s, (char *)b + done, (int)(n - done), 0); if (r <= 0) return false; done += (size_t)r; }
	return true;
}
static bool recv_line(sock_t s, std::string &l) {
	l.clear(); char c;
	while (l.size() < 512) { if (recv(s, &c, 1, 0) != 1) return false; if (c == '\n') { if (!l.empty() && l.back() == '\r') l.pop_back(); return true; } l += c; }
	return false;
}
static bool send_line(sock_t s, const std::string &l) { std::string x = l + "\n"; return send_all(s, x.data(), x.size()); }

std::vector<Vita> find(int ms) {   /* broadcast, and the paired Vita's last address directly (some networks drop broadcasts) */
	std::vector<Vita> out;
	sock_t s = socket(AF_INET, SOCK_DGRAM, 0); if (s == BAD_SOCK) return out;
	int one = 1; setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(PORT);
	a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(s, "NSYNC1?", 7, 0, (struct sockaddr *)&a, sizeof a);
	std::string last = get("vita_addr");
	if (!last.empty() && inet_pton(AF_INET, last.c_str(), &a.sin_addr) == 1) sendto(s, "NSYNC1?", 7, 0, (struct sockaddr *)&a, sizeof a);
	auto t0 = std::chrono::steady_clock::now();
	for (;;) {
		int left = ms - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		if (left <= 0) break;
		fd_set r; FD_ZERO(&r); FD_SET(s, &r); struct timeval t = {left / 1000, (left % 1000) * 1000};
		if (select((int)s + 1, &r, nullptr, nullptr, &t) != 1) break;
		char b[256]; struct sockaddr_in from; socklen_t fl = sizeof from;
		int n = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl); if (n <= 0) continue;
		b[n] = 0; std::string m(b);
		if (m.compare(0, 7, "NSYNC1 ") || m.size() < 7 + 17) continue;
		char ip[64]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
		Vita v{ip, m.substr(7, 16), m.size() > 24 ? m.substr(24) : "PS Vita"};
		bool dup = false; for (auto &x : out) if (x.id == v.id) dup = true;
		if (!dup) out.push_back(v);
	}
	close_sock(s);
	return out;
}

std::string pair(const Vita &v, const std::string &code) {
	if (code.size() != 6 || code.find_first_not_of("0123456789") != std::string::npos) return "The code is six digits.";
	sock_t s = dial(v.addr, 4000); if (s == BAD_SOCK) return "Couldn't reach " + v.name + ".";
	std::string why, l; uint8_t pcn[16], vn[16], rnd[32], other[32], pk[32], cf[32], tr[96], tag[32], expect[32];
	NsyncPair p;
	random_bytes(pcn, 16); random_bytes(rnd, 32);
	if (!send_line(s, "NSYNC1 PAIR " + get("pc_id") + " " + hex(pcn, 16) + " " + g_pc_name) || !recv_line(s, l)) why = "The Vita didn't answer.";
	else if (l.compare(0, 12, "NSYNC1 PAIR ")) why = l.compare(0, 11, "NSYNC1 ERR ") ? "The Vita refused." : l.substr(11) + ".";
	else if (!unhex(l.substr(12), vn, 16)) why = "The Vita's answer didn't read.";
	else {
		nsync_pair_start(&p, pcn, vn, code.c_str(), rnd);
		if (!send_line(s, hex(p.mine, 32)) || !recv_line(s, l) || !unhex(l, other, 32)) why = "The Vita didn't answer.";
		else if (nsync_pair_finish(&p, 0, other, pk, cf, tr)) why = "The Vita's answer didn't read.";
		else if (!recv_line(s, l) || !unhex(l, tag, 32)) why = "The Vita didn't answer.";
		else {
			nsync_pair_tag(expect, cf, "vita", tr);
			if (crypto_verify32(tag, expect)) why = "That code didn't match. Check the code on the Vita and try again.";
			else {
				nsync_pair_tag(tag, cf, "pc", tr);
				if (!send_line(s, hex(tag, 32)) || !recv_line(s, l)) why = "The Vita didn't answer.";
				else if (l.compare(0, 3, "OK ")) why = l.compare(0, 4, "ERR ") ? "The Vita refused." : l.substr(4) + ".";
				else {
					std::lock_guard<std::mutex> g(mu);
					cfg["vita_id"] = l.substr(3, 16); cfg["vita_name"] = l.size() > 20 ? l.substr(20) : v.name; cfg["vita_addr"] = v.addr;
					cfg["key"] = hex(pk, 32); cfg["enabled"] = "1"; cfg_save();
				}
			}
		}
	}
	crypto_wipe(rnd, sizeof rnd); crypto_wipe(pk, sizeof pk); crypto_wipe(cf, sizeof cf);
	close_sock(s);
	return why;
}
void unpair(void) {
	std::lock_guard<std::mutex> l(mu);
	for (const char *k : {"vita_id", "vita_name", "vita_addr", "key", "account"}) cfg.erase(k);
	cfg_save(); statuses.clear(); vita_account.clear();
}

/* a session with the paired Vita */
struct Conn {
	sock_t s = BAD_SOCK; NsyncSession ses; std::vector<uint8_t> frame, plain;
	std::string err;
	~Conn() { if (s != BAD_SOCK) close_sock(s); crypto_wipe(&ses, sizeof ses); }
	bool open(const std::string &addr, int ms) {
		uint8_t key[32], pcn[16], vn[16];
		if (s != BAD_SOCK) { close_sock(s); s = BAD_SOCK; }
		if (!unhex(get("key"), key, 32)) { err = "not paired"; return false; }
		s = dial(addr, ms); if (s == BAD_SOCK) { err = "not reachable"; return false; }
		random_bytes(pcn, 16); std::string l;
		if (!send_line(s, "NSYNC1 HELLO " + get("pc_id") + " " + hex(pcn, 16)) || !recv_line(s, l)) { err = "no answer"; return false; }
		if (l == "NSYNC1 ERR unpaired") { err = "unpaired"; return false; }
		if (l.compare(0, 13, "NSYNC1 HELLO ") || !unhex(l.substr(13), vn, 16)) { err = l; return false; }
		nsync_session(&ses, key, pcn, vn, 0); crypto_wipe(key, sizeof key);
		frame.resize(NSYNC_PLAIN_MAX + 64); plain.resize(NSYNC_PLAIN_MAX + 64);
		J h; return req_json("HELLO " + g_pc_name, h) && hello(h);
	}
	bool hello(const J &h) {
		std::string acc = h["account_id"].str();
		if (acc.size() == 16 && acc != "0000000000000000") { std::lock_guard<std::mutex> l(mu); if (cfg["account"] != acc) { cfg["account"] = acc; cfg_save(); } vita_account = acc; }
		std::string name = h["console"].str();
		if (!name.empty() && name != get("vita_name")) set("vita_name", name);
		return true;
	}
	bool req(const std::string &line, const std::string &payload, std::string &out) {   /* out: what follows "OK\n" */
		std::string msg = line; if (!payload.empty()) { msg += '\n'; msg += payload; }
		if (msg.size() > NSYNC_PLAIN_MAX) { err = "too big"; return false; }
		size_t n = nsync_seal(&ses, frame.data(), (const uint8_t *)msg.data(), msg.size());
		uint8_t head[4];
		if (!send_all(s, frame.data(), n) || !recv_all(s, head, 4)) { err = "connection lost"; return false; }
		uint32_t blen = nsync_frame_length(head);
		if (blen < 16 || blen > NSYNC_PLAIN_MAX + 16 || !recv_all(s, frame.data(), blen)) { err = "connection lost"; return false; }
		long pn = nsync_open(&ses, plain.data(), frame.data(), blen);
		if (pn < 0) { err = "a reply didn't open"; return false; }
		std::string r((const char *)plain.data(), (size_t)pn);
		if (r.compare(0, 2, "OK")) { err = r.compare(0, 4, "ERR ") ? r : r.substr(4); return false; }
		out = r.size() > 3 ? r.substr(3) : ""; return true;
	}
	bool req_json(const std::string &line, J &out, const std::string &payload = "") {
		std::string r; if (!req(line, payload, r)) return false;
		if (!parse(r, out)) { err = "a reply didn't read"; return false; }
		return true;
	}
};
static bool connect_vita(Conn &c, int ms) {   /* the last address, else where the Vita answers from now */
	std::string addr = get("vita_addr");
	if (!addr.empty() && c.open(addr, ms)) return true;
	if (c.err == "unpaired") return false;
	for (auto &v : find(std::min(ms, 1500))) if (v.id == get("vita_id")) {   /* (it moved: DHCP) */
		if (v.addr != addr) set("vita_addr", v.addr);
		return v.addr != addr && c.open(v.addr, ms);
	}
	return false;
}

static void clear_guard(const std::string &id) { std::lock_guard<std::mutex> k(op_mu); Conn c; std::string o; if (connect_vita(c, 3000)) c.req("GUARD " + id, "state\tnone\n", o); }

/* ---- Nativ's side of a save: savedata0's files (but its .slot<N>), and the slots from .slot<N> (0x34C bytes) ---- */
struct Entry { std::string path, hash, mtime; uint64_t size = 0; };
struct Manifest { std::vector<Entry> files; std::map<int, std::string> slots; std::string text, hash; };
static int slot_file(const std::string &name) {   /* ".slot<N>" -> N, else -1 */
	if (name.compare(0, 5, ".slot") || name.size() == 5 || name.size() > 8) return -1;
	int n = 0; for (size_t i = 5; i < name.size(); i++) { if (name[i] < '0' || name[i] > '9') return -1; n = n * 10 + (name[i] - '0'); }
	return n < 256 ? n : -1;
}
static std::string slot_param(const fs::path &p) { std::string s = read_file(p); s.resize(SLOT_PARAM, '\0'); return s; }
static Manifest local_manifest(const std::string &dir) {
	Manifest m; std::error_code ec; fs::path root = P(dir);
	if (fs::is_directory(root, ec)) for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (!it->is_regular_file(ec)) continue;
		std::string rel = U(fs::relative(it->path(), root, ec)); std::replace(rel.begin(), rel.end(), '\\', '/');
		if (rel.find('/') == std::string::npos) { int n = slot_file(rel); if (n >= 0) { m.slots[n] = blake_hex(slot_param(it->path())); continue; } }
		std::string data = read_file(it->path());
		m.files.push_back({rel, blake_hex(data), stamp_of(it->last_write_time(ec)), data.size()});
	}
	std::sort(m.files.begin(), m.files.end(), [](const Entry &a, const Entry &b) { return a.path < b.path; });
	for (auto &f : m.files) m.text += "F " + f.path + "\t" + std::to_string(f.size) + "\t" + f.hash + "\n";
	for (auto &s : m.slots) m.text += "S " + std::to_string(s.first) + "\t" + s.second + "\n";
	m.hash = blake_hex(m.text);
	return m;
}
static std::string size_text(uint64_t b) { char o[32]; if (b < 1024) snprintf(o, sizeof o, "%llu bytes", (unsigned long long)b); else if (b < 1 << 20) snprintf(o, sizeof o, "%.0f KB", b / 1024.0); else snprintf(o, sizeof o, "%.1f MB", b / 1048576.0); return o; }

static std::string narrow(const std::string &s) {   /* full-width letters (Ｍａｔｔ) and the ideographic space as plain ones: fonts lack them */
	std::string o;
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char a = (unsigned char)s[i];
		if (a == 0xEF && i + 2 < s.size()) {
			unsigned cp = ((unsigned)(a & 15) << 12) | ((unsigned)((unsigned char)s[i + 1] & 63) << 6) | ((unsigned char)s[i + 2] & 63);
			if (cp >= 0xFF01 && cp <= 0xFF5E) { o += (char)(cp - 0xFF01 + 0x21); i += 2; continue; }
		}
		if (a == 0xE3 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80) { o += ' '; i += 2; continue; }
		o += s[i];
	}
	return o;
}
static std::string title_line(std::string t, const std::string &sub) {   /* a save list's two lines as one */
	if (!sub.empty()) t += (t.empty() ? "" : " \xC2\xB7 ") + sub;
	return narrow(t);
}
static Side pc_side(const Game &g, const Manifest &m) {   /* when: the newest file; what: the newest slot's texts */
	Side s; std::string latest, newest_slot; uint64_t total = 0;
	for (auto &f : m.files) { if (f.mtime > latest) latest = f.mtime; total += f.size; }
	s.when = latest.empty() ? "" : friendly(latest);
	for (auto &sl : m.slots) {
		std::string p = slot_param(P(g.save_dir) / (".slot" + std::to_string(sl.first)));
		const uint8_t *t = (const uint8_t *)p.data() + 0x30C;   /* modifiedTime: year, month, day, hour, minute (u16 each) */
		char stamp[32]; snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d", t[0] | t[1] << 8, t[2] | t[3] << 8, t[4] | t[5] << 8, t[6] | t[7] << 8, t[8] | t[9] << 8);
		auto field = [&](int off, int n) { std::string x = p.substr(off, n); x.resize(strnlen(x.c_str(), x.size())); return x; };
		std::string line = title_line(field(0x4, 64), field(0x44, 128));
		if (!line.empty() && stamp >= newest_slot) { newest_slot = stamp; s.detail = line; }
	}
	if (s.detail.empty()) s.detail = std::to_string(m.files.size()) + (m.files.size() == 1 ? " file, " : " files, ") + size_text(total);
	return s;
}
static Side vita_side(const J &m) {
	Side s; std::string latest, newest_slot; uint64_t total = 0;
	for (auto &f : m["files"].a) { if (f["mtime"].str() > latest) latest = f["mtime"].str(); total += (uint64_t)f["size"].num(); }
	for (auto &sl : m["slots"].a) {
		std::string line = title_line(sl["title"].str(), sl["subtitle"].str()), t = sl["time"].str();
		if (!line.empty() && t >= newest_slot) { newest_slot = t; s.detail = line; }
	}
	if (latest.empty()) latest = newest_slot;   /* (a service without file times: the newest slot's) */
	s.when = latest.empty() ? "" : friendly(latest);
	if (s.detail.empty()) s.detail = std::to_string(m["files"].a.size()) + (m["files"].a.size() == 1 ? " file, " : " files, ") + size_text(total);
	return s;
}

/* ---- deciding and copying ---- */
static Check decide(Conn &c, const Game &g) {
	Check r; J list, m;
	if (!c.req_json("LIST " + g.id, list)) { r.why = "The Vita didn't answer: " + c.err; return r; }
	const J *vp = nullptr; for (auto &x : list["games"].a) if (x["id"].str() == g.id) vp = &x;   /* (an older service lists every game) */
	if (!vp) { r.why = "Not on the Vita"; return r; }
	const J &v = *vp;
	if (!v["save"].b) { r.why = v["name"].str().empty() ? "Not on the Vita" : "No save on the Vita yet"; return r; }
	if (g.version.empty() || g.version == "-" || g.version == "unknown") { r.why = "Nativ doesn't know this game's version"; return r; }
	if (!same_version(v["version"].str(), g.version)) { r.why = "Version differs (Vita " + plain_version(v["version"].str()) + ", Nativ " + plain_version(g.version) + ")"; return r; }
	if (!c.req_json("MANIFEST " + g.id, m)) { r.why = c.err == "busy" ? "Playing on the Vita" : "The Vita couldn't read it: " + c.err; return r; }
	Manifest local = local_manifest(g.save_dir);
	std::string vh = m["hash"].str(), ph = local.hash, base = base_of(g.id);
	r.vita = vita_side(m); r.pc = pc_side(g, local);
	if (vh == ph) { r.kind = IN_SYNC; if (base != vh) { write_file(base_path(g.id), vh); std::string o; c.req("BASE " + g.id + " " + vh, "", o); } synced(g.id); }
	else if (ph == EMPTY_HASH) r.kind = PULL;
	else if (base.empty()) r.kind = CONFLICT;
	else if (ph == base) r.kind = PULL;
	else if (vh == base) r.kind = PUSH;
	else r.kind = CONFLICT;
	return r;
}
static void prune(const fs::path &dir) {
	std::error_code ec; std::vector<fs::path> all;
	for (auto &e : fs::directory_iterator(dir, ec)) if (e.is_directory(ec)) all.push_back(e.path());
	std::sort(all.begin(), all.end());
	for (size_t i = 0; i + KEEP_BACKUPS < all.size(); i++) fs::remove_all(all[i], ec);
}
static bool do_pull(Conn &c, const Game &g, std::string &err) {
	J m; if (!c.req_json("MANIFEST " + g.id, m)) { err = c.err == "busy" ? "the game is running on the Vita" : c.err; return false; }
	std::error_code ec; fs::path save = P(g.save_dir), fresh = P(g.save_dir + ".vita-sync-new");
	fs::remove_all(fresh, ec); fs::create_directories(fresh, ec);
	for (auto &f : m["files"].a) {
		std::string path = f["path"].str(), data; uint64_t size = (uint64_t)f["size"].num();
		if (path.empty() || path.find("..") != std::string::npos || path[0] == '/' || path.find(':') != std::string::npos) { err = "a path the Vita sent doesn't read"; return false; }
		while (data.size() < size) {
			std::string part; size_t want = (size_t)std::min<uint64_t>(CHUNK, size - data.size());
			if (!c.req("GET " + g.id + " " + path + " " + std::to_string(data.size()) + " " + std::to_string(want), "", part) || part.empty()) { err = c.err.empty() ? "short read" : c.err; return false; }
			data += part;
		}
		if (blake_hex(data) != f["hash"].str()) { err = path + " arrived damaged"; return false; }
		if (!write_file(fresh / P(path), data)) { err = "couldn't write " + path; return false; }
		set_mtime(fresh / P(path), f["mtime"].str());   /* (the time it was saved on the Vita, not copied: what a later choice shows) */
	}
	for (auto &s : m["slots"].a) {
		std::string data, n = std::to_string(s["slot"].num());
		if (!c.req("GET " + g.id + " slot/" + n + " 0 " + std::to_string(SLOT_PARAM), "", data) || data.size() != (size_t)SLOT_PARAM || blake_hex(data) != s["hash"].str()) { err = "slot " + n + " arrived damaged"; return false; }
		if (!write_file(fresh / (".slot" + n), data)) { err = "couldn't write slot " + n; return false; }
	}
	if (local_manifest(U(fresh)).hash != m["hash"].str()) { err = "the copy didn't match the Vita's"; return false; }
	fs::path bdir = P(g_backups) / g.id / now_stamp();
	if (fs::exists(save, ec)) { fs::create_directories(bdir.parent_path(), ec); fs::copy(save, bdir, fs::copy_options::recursive, ec); if (ec) { err = "couldn't back up Nativ's save: " + ec.message(); return false; } }
	fs::remove_all(save, ec); fs::create_directories(save.parent_path(), ec);
	fs::rename(fresh, save, ec);
	if (ec || local_manifest(g.save_dir).hash != m["hash"].str()) {   /* put Nativ's back */
		fs::remove_all(save, ec); if (fs::exists(bdir)) fs::copy(bdir, save, fs::copy_options::recursive, ec);
		err = "replacing Nativ's save failed; it was put back"; return false;
	}
	prune(bdir.parent_path());
	write_file(base_path(g.id), m["hash"].str());
	std::string o; c.req("BASE " + g.id + " " + m["hash"].str(), "", o);
	return true;
}
static bool do_push(Conn &c, const Game &g, std::string &err) {
	Manifest local = local_manifest(g.save_dir); J there;
	if (!c.req_json("MANIFEST " + g.id, there)) { err = c.err == "busy" ? "the game is running on the Vita" : c.err; return false; }
	std::map<std::string, std::string> have; std::map<long long, std::string> have_s;
	for (auto &f : there["files"].a) have[f["path"].str()] = f["hash"].str();
	for (auto &s : there["slots"].a) have_s[s["slot"].num()] = s["hash"].str();
	std::string o;
	for (auto &f : local.files) {
		if (have.count(f.path) && have[f.path] == f.hash) continue;
		std::string data = read_file(P(g.save_dir) / P(f.path));
		for (size_t off = 0; off < data.size() || (off == 0 && data.empty()); off += CHUNK) {
			if (!c.req("PUT " + g.id + " " + f.path + " " + std::to_string(off) + " " + std::to_string(data.size()), data.substr(off, CHUNK), o)) { err = c.err; return false; }
			if (data.empty()) break;
		}
	}
	for (auto &s : local.slots) {
		if (have_s.count(s.first) && have_s[s.first] == s.second) continue;
		if (!c.req("PUT " + g.id + " slot/" + std::to_string(s.first) + " 0 " + std::to_string(SLOT_PARAM), slot_param(P(g.save_dir) / (".slot" + std::to_string(s.first))), o)) { err = c.err; return false; }
	}
	J r; if (!c.req_json("COMMIT " + g.id + " " + local.hash, r, local.text)) { err = c.err == "busy" ? "the game is running on the Vita" : c.err; return false; }
	write_file(base_path(g.id), local.hash);
	return true;
}
static void report(const Game &g, const Check &c) {
	switch (c.kind) {
	case IN_SYNC: set_status(g.id, "In sync", 1); break;
	case PULL: set_status(g.id, "Newer on the Vita", 0); break;
	case PUSH: set_status(g.id, "Newer here", 0); break;
	case CONFLICT: set_status(g.id, "Needs a choice", 2); break;
	default: set_status(g.id, c.why, 0);
	}
}

Check check(const Game &g, int connect_ms) {
	std::lock_guard<std::mutex> l(op_mu);
	Check r; Conn c;
	if (!paired() || !enabled() || !game_on(g.id)) { r.why = "Off"; return r; }
	if (!connect_vita(c, connect_ms)) { r.why = vita_name() + " isn't on the network"; return r; }
	r = decide(c, g); report(g, r);
	return r;
}
bool pull(const Game &g, std::string &err) {
	std::lock_guard<std::mutex> l(op_mu); Conn c;
	if (!connect_vita(c, 4000)) { err = vita_name() + " isn't on the network"; return false; }
	bool ok = do_pull(c, g, err);
	if (ok) { set_status(g.id, "In sync", 1); std::string o; c.req("GUARD " + g.id, "state\tnone\n", o); { std::lock_guard<std::mutex> k(mu); vita_wins.erase(g.id); } synced(g.id); }
	return ok;
}
bool push(const Game &g, std::string &err) {
	std::lock_guard<std::mutex> l(op_mu); Conn c;
	if (!connect_vita(c, 4000)) { err = vita_name() + " isn't on the network"; return false; }
	bool ok = do_push(c, g, err);
	if (ok) { set_status(g.id, "In sync", 1); std::string o; c.req("GUARD " + g.id, "state\tnone\n", o); synced(g.id); }
	return ok;
}
static std::string clean(std::string s) { for (auto &ch : s) if (ch == '\t' || ch == '\n') ch = ' '; return s; }
void mark_conflict(const Game &g, const Check &k) {
	std::lock_guard<std::mutex> l(op_mu); Conn c;
	set_status(g.id, "Needs a choice", 2);
	if (!connect_vita(c, 3000)) return;
	std::string o, body = "state\tconflict\ngame\t" + clean(g.name) + "\npc_when\t" + clean(k.pc.when) + "\npc_detail\t" + clean(k.pc.detail) +
		"\nvita_when\t" + clean(k.vita.when) + "\nvita_detail\t" + clean(k.vita.detail) + "\n";
	c.req("GUARD " + g.id, body, o);
}
void after_play(const Game &g) {
	Check k = check(g, 3000); std::string err;
	if (k.kind == PUSH) { if (!push(g, err)) set_status(g.id, "Couldn't send: " + err, 2); }
	else if (k.kind == PULL) { if (!pull(g, err)) set_status(g.id, "Couldn't fetch: " + err, 2); }
	else if (k.kind == CONFLICT) mark_conflict(g, k);
}

/* ---- the background side ---- */
static bool find_game(const std::string &id, Game &out) { std::lock_guard<std::mutex> l(mu); for (auto &g : library) if (g.id == id) { out = g; return true; } return false; }
static void catch_up(void) {   /* on connecting: bring each game up to date where that's safe, flag the rest */
	std::vector<Game> games; { std::lock_guard<std::mutex> l(mu); games = library; }
	for (auto &g : games) {
		if (!run || !game_on(g.id) || g.id == now_playing()) continue;
		Check k = check(g, 3000); std::string err;
		if (k.kind == PULL) { if (!pull(g, err)) set_status(g.id, "Couldn't fetch: " + err, 2); }
		else if (k.kind == PUSH) { if (!push(g, err)) set_status(g.id, "Couldn't send: " + err, 2); }
		else if (k.kind == CONFLICT) mark_conflict(g, k);
	}
}
static void on_event(Conn &ev, const J &e) {
	std::string kind = e["kind"].str(), id = e["id"].str(), choice = e["choice"].str(), err; Game g;
	if (!find_game(id, g) || !game_on(id)) return;
	if (kind == "started") set_status(id, "Playing on the Vita", 0);
	else if (kind == "choice" && choice == "pc") {   /* the launch waits there for this save; the commit lets it go */
		bool ok = id != now_playing() && push(g, err);
		std::string o; if (!ok) ev.req("RELEASE " + id + " 0", "", o);
		if (!ok) set_status(id, "Couldn't send: " + (err.empty() ? "it's running in Nativ" : err), 2);
	} else if (kind == "choice" && choice == "vita") { std::lock_guard<std::mutex> l(mu); vita_wins.insert(id); set_status(id, "Taking the Vita's save after it closes", 0); }
	else if (kind == "stopped" && id != now_playing()) {
		bool wins; { std::lock_guard<std::mutex> l(mu); wins = vita_wins.count(id) > 0; }
		if (wins) { if (!pull(g, err)) set_status(id, "Couldn't fetch: " + err, 2); return; }
		Check k = check(g, 3000);
		if (k.kind == PULL) { if (!pull(g, err)) set_status(id, "Couldn't fetch: " + err, 2); }
		else if (k.kind == PUSH) { if (!push(g, err)) set_status(id, "Couldn't send: " + err, 2); }
		else if (k.kind == CONFLICT) mark_conflict(g, k);
	}
}
static void loop(void) {
	int idle = 0;
	while (run) {
		if (!paired() || !enabled()) { is_online = false; std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
		Conn ev;
		if (!connect_vita(ev, 3000)) {
			is_online = false;
			for (int i = 0; i < (idle < 6 ? 5 : 30) && run && paired(); i++) std::this_thread::sleep_for(std::chrono::seconds(1));   /* (then every 30 s) */
			idle++; continue;
		}
		idle = 0; is_online = true;
		catch_up();
		J r;
		while (run && paired() && enabled() && ev.req_json("EVENTS 4000", r)) {
			for (auto &e : r["events"].a) on_event(ev, e);
			r = J();
		}
		is_online = false;
	}
}
void start(void) { if (run) return; run = true; worker = std::thread(loop); }
void stop(void) { run = false; if (worker.joinable()) worker.detach(); }   /* (it may be waiting on the Vita for seconds: the process ending takes it) */

/* ---- nativ --vita-sync <find | pair <address> <code> | status | check|pull|push <TITLE> <version> <savedata0>> ---- */
int cli(int argc, char **argv) {
	std::string cmd = argc > 0 ? argv[0] : "";
	if (cmd == "find") { for (auto &v : find(2000)) printf("%s %s %s\n", v.addr.c_str(), v.id.c_str(), v.name.c_str()); return 0; }
	if (cmd == "pair" && argc >= 3) {
		Vita v{argv[1], "", argv[1]}; for (auto &x : find(1500)) if (x.addr == v.addr) v = x;
		std::string why = pair(v, argv[2]); printf("%s\n", why.empty() ? ("paired with " + vita_name()).c_str() : why.c_str()); return why.empty() ? 0 : 1;
	}
	if (cmd == "unpair") { unpair(); return 0; }
	if (cmd == "status") { Conn c; printf("%s\n", line().c_str()); if (paired() && connect_vita(c, 3000)) printf("connected; account %s\n", account_id().c_str()); else if (paired()) printf("not connected: %s\n", c.err.c_str()); return 0; }
	if ((cmd == "check" || cmd == "pull" || cmd == "push") && argc >= 4) {
		Game g{argv[1], argv[1], argv[2], argv[3]};
		if (cmd == "check") {
			static const char *const kinds[] = {"in sync", "newer on the Vita (pull)", "newer here (push)", "conflict", "skip"};
			Check k = check(g, 3000);
			printf("%s%s%s\n  vita: %s | %s\n  here: %s | %s\n", kinds[k.kind], k.why.empty() ? "" : ": ", k.why.c_str(), k.vita.when.c_str(), k.vita.detail.c_str(), k.pc.when.c_str(), k.pc.detail.c_str());
			return 0;
		}
		std::string err; bool ok = cmd == "pull" ? pull(g, err) : push(g, err);
		printf("%s%s\n", ok ? "done" : "failed: ", err.c_str()); return ok ? 0 : 1;
	}
	printf("nativ --vita-sync find | pair <address> <code> | unpair | status | check|pull|push <TITLE> <version> <savedata0 folder>\n");
	return 2;
}
}
