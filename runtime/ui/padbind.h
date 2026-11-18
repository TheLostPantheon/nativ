/* padbind.h: the controller's side of the button mapping, with no SDL in it (window.c uses it, the launcher reads the
 * names, runtime/tests/padbind_test.c checks it). Each Vita input a game can see is an action; each action has one
 * controller input: an SDL gamepad button (by SDL_GamepadButton's own name: "south", "leftshoulder", so a binding
 * means the same on every brand) or a trigger ("lefttrigger"). Stored as the settings keys "padbind_<action>", shared
 * by default; "bindings_own" = 1 in a game's own file gives that game its own set (window.c: the keys are read through
 * the usual game-then-shared lookup, so that is all an override takes). A key that is absent or unreadable is the
 * default, which is what the controller did before bindings existed, "shoulder_buttons" included.
 *
 * The button ids are SDL_GamepadButton's own values (window.c checks them against SDL's enum), the triggers 32 and 33.
 * The Guide button is not in the table, so it can never be bound: it opens the menu. */
#ifndef PADBIND_H
#define PADBIND_H
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { PBA_CROSS, PBA_CIRCLE, PBA_SQUARE, PBA_TRIANGLE, PBA_L, PBA_R, PBA_START, PBA_SELECT, PBA_UP, PBA_DOWN, PBA_LEFT, PBA_RIGHT,
       PBA_L2, PBA_R2, PBA_L3, PBA_R3, PBA_N };   /* L2/R2: a PS TV layout's, else the touch cursor's front/rear modifiers */
enum { PBI_NONE = -1, PBI_LTRIG = 32, PBI_RTRIG = 33 };
typedef struct { int in[PBA_N]; } PadBinds;

static const struct { int id; const char *name; } pb_inputs[] = {
	{0, "south"}, {1, "east"}, {2, "west"}, {3, "north"}, {4, "back"}, {6, "start"}, {7, "leftstick"}, {8, "rightstick"},
	{9, "leftshoulder"}, {10, "rightshoulder"}, {11, "dpup"}, {12, "dpdown"}, {13, "dpleft"}, {14, "dpright"},
	{15, "misc1"}, {20, "touchpad"}, {PBI_LTRIG, "lefttrigger"}, {PBI_RTRIG, "righttrigger"},
};
static const char *const pb_action_names[PBA_N] = {"cross", "circle", "square", "triangle", "l", "r", "start", "select", "up", "down", "left", "right", "l2", "r2", "l3", "r3"};

static inline const char *pb_input_name(int id) {
	for (unsigned i = 0; i < sizeof pb_inputs / sizeof pb_inputs[0]; i++) if (pb_inputs[i].id == id) return pb_inputs[i].name;
	return "none";
}
static inline int pb_input_valid(int id) { return strcmp(pb_input_name(id), "none") != 0; }
/* a stored name: an input id, PBI_NONE for "none", -2 for anything else */
static inline int pb_input_parse(const char *s) {
	if (!s) return -2;
	if (!strcasecmp(s, "none")) return PBI_NONE;
	for (unsigned i = 0; i < sizeof pb_inputs / sizeof pb_inputs[0]; i++) if (!strcasecmp(pb_inputs[i].name, s)) return pb_inputs[i].id;
	return -2;
}
static inline const char *pb_key(int a, char *buf, int n) { snprintf(buf, (size_t)n, "padbind_%s", pb_action_names[a]); return buf; }
/* what each action is with nothing stored: the triggers are L and R and the bumpers the touch cursor's pair, or the
 * other way round with "shoulder_buttons" = bumpers (swapped) */
static inline int pb_default(int a, int swapped) {
	switch (a) {
	case PBA_CROSS: return 0; case PBA_CIRCLE: return 1; case PBA_SQUARE: return 2; case PBA_TRIANGLE: return 3;
	case PBA_START: return 6; case PBA_SELECT: return 4;
	case PBA_UP: return 11; case PBA_DOWN: return 12; case PBA_LEFT: return 13; case PBA_RIGHT: return 14;
	case PBA_L: return swapped ? 9 : PBI_LTRIG; case PBA_R: return swapped ? 10 : PBI_RTRIG;
	case PBA_L2: return swapped ? PBI_LTRIG : 9; case PBA_R2: return swapped ? PBI_RTRIG : 10;
	case PBA_L3: return 7; default: return 8;   /* PBA_R3 */
	}
}
static inline int pb_find(const PadBinds *b, int input) {   /* the action on that input (-1: none) */
	if (input == PBI_NONE) return -1;
	for (int a = 0; a < PBA_N; a++) if (b->in[a] == input) return a;
	return -1;
}
/* the table from the stored values: get(key, user) gives a key's stored text or NULL if it has none. A value that
 * isn't a known input is the default; an input two actions claim (a hand-edited file, or "shoulder_buttons" changed
 * after a rebind) stays with the first, the later one is unbound */
static inline void pb_resolve(PadBinds *b, const char *(*get)(const char *key, void *user), void *user, int swapped) {
	for (int a = 0; a < PBA_N; a++) {
		char k[24]; const char *v = get(pb_key(a, k, sizeof k), user); int in = v && *v ? pb_input_parse(v) : -2;
		b->in[a] = in == -2 ? pb_default(a, swapped) : in;
	}
	for (int a = 0; a < PBA_N; a++) for (int c = 0; c < a; c++) if (b->in[a] != PBI_NONE && b->in[a] == b->in[c]) { b->in[a] = PBI_NONE; break; }
}
/* action a onto an input: the action that had it loses it (returned, -1 if none; the keyboard's rebinding does the same) */
static inline int pb_set(PadBinds *b, int a, int input) {
	if (a < 0 || a >= PBA_N || !pb_input_valid(input)) return -1;
	const int other = pb_find(b, input);
	if (other >= 0 && other != a) b->in[other] = PBI_NONE; else if (other == a) return -1;
	b->in[a] = input;
	return other;
}
#endif
