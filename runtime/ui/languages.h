/* The languages a Vita game can be told to run in (the SCE_SYSTEM_PARAM_LANG_* ids, which are also the table's order):
 * the settings value "game_language" (apputil.c answers the game's system-language query with it), and the menus' list
 * (the launcher's Game settings, the F10 menu). "auto" is not in the table: what the host answered before the setting, English (US). */
#pragma once
typedef struct { const char *key, *label; } GameLanguage;
static const GameLanguage game_languages[] = {
	{"japanese", "Japanese"}, {"english_us", "English (US)"}, {"french", "French"}, {"spanish", "Spanish"}, {"german", "German"},
	{"italian", "Italian"}, {"dutch", "Dutch"}, {"portuguese_pt", "Portuguese (Portugal)"}, {"russian", "Russian"}, {"korean", "Korean"},
	{"chinese_t", "Chinese (Traditional)"}, {"chinese_s", "Chinese (Simplified)"}, {"finnish", "Finnish"}, {"swedish", "Swedish"},
	{"danish", "Danish"}, {"norwegian", "Norwegian"}, {"polish", "Polish"}, {"portuguese_br", "Portuguese (Brazil)"},
	{"english_gb", "English (UK)"}, {"turkish", "Turkish"},
};
static const int game_language_count = (int)(sizeof game_languages / sizeof game_languages[0]);
/* the languages this game was checked in (its pack's LANGUAGES, passed as RECOMP_GAME_LANGUAGES="a,b"): a game that
 * lacks a language can fall back to English or fail to start (Uncharted US crashed at boot in French on its EU data),
 * so only these are offered, and a stored choice outside them is ignored */
#include <stdlib.h>
#include <string.h>
static inline int game_language_allowed(const char *key) {
	const char *l = getenv("RECOMP_GAME_LANGUAGES"); size_t n = strlen(key);
	for (const char *p = l; p && *p; ) { const char *e = strchr(p, ','); size_t m = e ? (size_t)(e - p) : strlen(p); if (m == n && !strncmp(p, key, n)) return 1; p = e ? e + 1 : p + m; }
	return 0;
}
