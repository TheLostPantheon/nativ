/* vitasync.h: Vita sync in the launcher. Nativ's side of Nativ Sync (TheLostPantheon/nativ-sync, PROTOCOL.md there):
 * finding a Vita on the network, pairing with the code its app shows, and keeping each game's save the same on both,
 * through the service the Vita runs. Nothing here runs until a Vita is paired. */
#pragma once
#include <string>
#include <vector>

namespace vsync {

struct Vita { std::string addr, id, name; };                  /* one found on the network */
struct Game { std::string id, name, version, save_dir; };     /* a library game: its savedata0 folder */
struct Side { std::string when, detail; };
enum Kind { IN_SYNC, PULL, PUSH, CONFLICT, SKIP };
struct Check { Kind kind = SKIP; std::string why; Side vita, pc; };
struct Status { std::string text; int tone = 0; };            /* tone: 0 quiet, 1 good, 2 needs the player */

/* setup: where its files go (the settings folder), where save backups go, this computer's name */
void init(const std::string &pref_dir, const std::string &backups_dir, const std::string &pc_name);
bool paired();
bool online();                 /* connected to the paired Vita right now */
std::string vita_name();
std::string account_id();      /* the Vita's account id, as Nativ's np_account_id writes it (%016llx), once known */
/* called (on a background thread) when a game's save is the same on both sides after a sync */
void set_on_synced(void (*f)(const std::string &id));
bool enabled();                /* sync on at all */
void set_enabled(bool on);
bool game_on(const std::string &id);
void set_game_on(const std::string &id, bool on);

std::vector<Vita> find(int ms, bool sweep = true);                 /* (blocking; sweep: each address of this computer's /24 asked too) */
std::string pair(const Vita &v, const std::string &code);         /* "" when paired, else why not (blocking) */
void unpair();

/* the library as the launcher knows it, and the game running in Nativ ("" for none): the background side never
 * touches a running game's save */
void set_library(const std::vector<Game> &games);
void set_playing(const std::string &id);

/* blocking, one at a time (call from a thread) */
Check check(const Game &g, int connect_ms);
bool pull(const Game &g, std::string &err);    /* the Vita's save replaces Nativ's (Nativ's backed up first) */
bool push(const Game &g, std::string &err);    /* Nativ's save replaces the Vita's (the Vita backs its own up) */
void mark_conflict(const Game &g, const Check &c);   /* the Vita asks too, at the game's next launch there */
void after_play(const Game &g);                /* after a game closes in Nativ */

Status status(const std::string &id);
std::string line();            /* a short summary: "Paired with ... · connected" */
void start(void);              /* the background side: connected while paired, answering the Vita's events */
void stop(void);

int cli(int argc, char **argv);   /* nativ --vita-sync ...: the same, from a terminal (tests) */
}
