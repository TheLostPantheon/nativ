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
