/* Checks runtime/ui/padbind.h, the controller's button table: names, defaults (what the pad did before bindings existed,
 * "shoulder_buttons" included), stored values, conflicts, and a game's own set over the shared one, read through the real
 * settings.c. No SDL, no window. Build and run as dev/tests/run_all.sh does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "padbind.h"
#include "settings.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const char *none(const char *k, void *u) { (void)k; (void)u; return NULL; }
static const char *from_settings(const char *k, void *u) { return settings_has(k) ? settings_str(k, "", (char *)u, 32) : NULL; }
static const char *the_one;
static const char *just_cross(const char *k, void *u) { (void)u; return !strcmp(k, "padbind_cross") ? the_one : NULL; }

int main(void) {
	/* names: every input round-trips, in any case; Guide and nonsense are not inputs */
	for (int id = 0; id < 40; id++) if (pb_input_valid(id)) CHECK(pb_input_parse(pb_input_name(id)) == id);
	CHECK(pb_input_parse("South") == 0 && pb_input_parse("LEFTTRIGGER") == PBI_LTRIG && pb_input_parse("none") == PBI_NONE);
	CHECK(pb_input_parse("guide") == -2 && pb_input_parse("") == -2 && pb_input_parse(NULL) == -2 && pb_input_parse("a") == -2);
	CHECK(!pb_input_valid(5) && !pb_input_valid(PBI_NONE) && !pb_input_valid(99));   /* 5: SDL's Guide, the menu's */
	char k[24]; CHECK(!strcmp(pb_key(PBA_L2, k, sizeof k), "padbind_l2") && !strcmp(pb_key(PBA_CROSS, k, sizeof k), "padbind_cross"));

	/* the defaults: the old fixed mapping, then the shoulder swap */
	PadBinds b; pb_resolve(&b, none, NULL, 0);
	CHECK(b.in[PBA_CROSS] == 0 && b.in[PBA_CIRCLE] == 1 && b.in[PBA_SQUARE] == 2 && b.in[PBA_TRIANGLE] == 3);
	CHECK(b.in[PBA_START] == 6 && b.in[PBA_SELECT] == 4 && b.in[PBA_UP] == 11 && b.in[PBA_RIGHT] == 14 && b.in[PBA_L3] == 7 && b.in[PBA_R3] == 8);
	CHECK(b.in[PBA_L] == PBI_LTRIG && b.in[PBA_R] == PBI_RTRIG && b.in[PBA_L2] == 9 && b.in[PBA_R2] == 10);
	pb_resolve(&b, none, NULL, 1);
	CHECK(b.in[PBA_L] == 9 && b.in[PBA_R] == 10 && b.in[PBA_L2] == PBI_LTRIG && b.in[PBA_R2] == PBI_RTRIG);
	for (int swapped = 0; swapped < 2; swapped++) {   /* every default is bound once, and never to Guide */
		pb_resolve(&b, none, NULL, swapped);
		for (int a = 0; a < PBA_N; a++) { CHECK(pb_input_valid(b.in[a])); CHECK(pb_find(&b, b.in[a]) == a); }
	}

	/* stored values: read; an unreadable or empty one is the default; none unbinds; a duplicate loses to the earlier action */
	the_one = "east"; pb_resolve(&b, just_cross, NULL, 0); CHECK(b.in[PBA_CROSS] == 1 && b.in[PBA_CIRCLE] == PBI_NONE);   /* (Circle's default is taken: Cross is earlier) */
	the_one = "righttrigger"; pb_resolve(&b, just_cross, NULL, 0); CHECK(b.in[PBA_CROSS] == PBI_RTRIG && b.in[PBA_R] == PBI_NONE);
	the_one = "guide"; pb_resolve(&b, just_cross, NULL, 0); CHECK(b.in[PBA_CROSS] == 0);
	the_one = ""; pb_resolve(&b, just_cross, NULL, 0); CHECK(b.in[PBA_CROSS] == 0);
	the_one = "none"; pb_resolve(&b, just_cross, NULL, 0); CHECK(b.in[PBA_CROSS] == PBI_NONE && pb_find(&b, 0) == -1);

	/* rebinding: the action that had the input loses it; binding what it already has changes nothing; bad inputs are refused */
	pb_resolve(&b, none, NULL, 0);
	CHECK(pb_set(&b, PBA_CROSS, 1) == PBA_CIRCLE && b.in[PBA_CROSS] == 1 && b.in[PBA_CIRCLE] == PBI_NONE);
	CHECK(pb_set(&b, PBA_CROSS, 1) == -1 && b.in[PBA_CROSS] == 1);
	CHECK(pb_set(&b, PBA_CIRCLE, 0) == -1 && b.in[PBA_CIRCLE] == 0 && pb_find(&b, 0) == PBA_CIRCLE);   /* (free: nothing lost) */
	CHECK(pb_set(&b, PBA_L, 5) == -1 && b.in[PBA_L] == PBI_LTRIG);   /* Guide: refused */
	CHECK(pb_set(&b, PBA_N, 0) == -1 && pb_set(&b, -1, 0) == -1);
	CHECK(pb_set(&b, PBA_SELECT, PBI_RTRIG) == PBA_R && b.in[PBA_R] == PBI_NONE && b.in[PBA_SELECT] == PBI_RTRIG);

	/* shared and per-game values, through the real settings: the game's own file wins, and clearing it gives the shared one back */
	char dir[] = "/tmp/padbind_test_XXXXXX"; CHECK(mkdtemp(dir) != NULL);
	char game[256], shared[256]; snprintf(game, sizeof game, "%s/game.ini", dir); snprintf(shared, sizeof shared, "%s/shared.ini", dir);
	settings_load(game); settings_load_shared(shared);
	char v[32];
	settings_set_str("padbind_cross", "east"); settings_save();   /* a shared binding */
	pb_resolve(&b, from_settings, v, 0); CHECK(b.in[PBA_CROSS] == 1);
	settings_set_own("bindings_own", "1"); settings_set_own("padbind_cross", "north"); settings_save();   /* this game's own */
	pb_resolve(&b, from_settings, v, 0); CHECK(b.in[PBA_CROSS] == 3 && settings_int("bindings_own", 0) == 1);
	settings_set_str("padbind_circle", "west"); settings_save();   /* a shared change from elsewhere: the game's own cross stays */
	pb_resolve(&b, from_settings, v, 0); CHECK(b.in[PBA_CROSS] == 3 && b.in[PBA_CIRCLE] == 2);
	settings_clear_own("padbind_cross"); settings_clear_own("bindings_own"); settings_save();
	pb_resolve(&b, from_settings, v, 0); CHECK(b.in[PBA_CROSS] == 1 && settings_int("bindings_own", 0) == 0);
	settings_set_str("padbind_cross", ""); settings_set_str("padbind_circle", ""); settings_save();   /* the menu's reset */
	pb_resolve(&b, from_settings, v, 0); CHECK(b.in[PBA_CROSS] == 0 && b.in[PBA_CIRCLE] == 1);
	/* bindings_own lives in the game's file only */
	settings_set_str("bindings_own", "1"); settings_save();
	FILE *f = fopen(shared, "r"); char line[128]; int own_in_shared = 0;
	while (f && fgets(line, sizeof line, f)) own_in_shared |= strstr(line, "bindings_own") != NULL;
	if (f) fclose(f);
	CHECK(!own_in_shared);
	char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}

	printf("padbind: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
