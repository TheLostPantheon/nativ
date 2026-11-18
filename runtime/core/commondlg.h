/*
 * The message dialog's and the text-input (IME) dialog's current state (apputil.c's sceMsgDialog and sceImeDialog),
 * for the overlay to draw and answer (overlay.cpp's build_msg_dialog and build_ime_dialog), as savedlg.h does for
 * the save-data dialog. The game thread opens and closes them; the window thread reads the state under the view's
 * lock and answers. Plain C, shared by both sides; the UTF-16 helpers are here so a test can reach them.
 */
#ifndef RECOMP_COMMONDLG_H
#define RECOMP_COMMONDLG_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- guest text: UTF-16LE (the console's SceWChar16, surrogate pairs included) and UTF-8 (the overlay's) */
#define IME_MAX_TEXT 2048   /* SCE_IME_DIALOG_MAX_TEXT_LENGTH, in UTF-16 units */

/* the NUL-terminated UTF-16 string at `s` (at most `max` units) as UTF-8 into out[n]; returns the bytes written (not
 * counting the NUL). A lone surrogate becomes U+FFFD; output that would not fit whole is dropped. */
static inline size_t cdlg_utf16_to_utf8(const uint8_t *s, size_t max, char *out, size_t n) {
	size_t o = 0;
	for (size_t i = 0; s && i < max; i++) {
		uint32_t u = (uint32_t)(s[2 * i] | s[2 * i + 1] << 8);
		if (!u) break;
		if (u >= 0xd800 && u < 0xdc00 && i + 1 < max) {
			uint32_t lo = (uint32_t)(s[2 * i + 2] | s[2 * i + 3] << 8);
			if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); i++; }
			else u = 0xfffd;
		} else if (u >= 0xd800 && u < 0xe000) u = 0xfffd;
		const size_t k = u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;
		if (o + k + 1 > n) break;
		if (k == 1) out[o] = (char)u;
		else if (k == 2) { out[o] = (char)(0xc0 | u >> 6); out[o + 1] = (char)(0x80 | (u & 0x3f)); }
		else if (k == 3) { out[o] = (char)(0xe0 | u >> 12); out[o + 1] = (char)(0x80 | (u >> 6 & 0x3f)); out[o + 2] = (char)(0x80 | (u & 0x3f)); }
		else { out[o] = (char)(0xf0 | u >> 18); out[o + 1] = (char)(0x80 | (u >> 12 & 0x3f)); out[o + 2] = (char)(0x80 | (u >> 6 & 0x3f)); out[o + 3] = (char)(0x80 | (u & 0x3f)); }
		o += k;
	}
	if (n) out[o] = 0;
	return o;
}

/* the UTF-8 string as UTF-16LE into out (room for max + 1 units): at most `max` units, a pair never split, then a
 * terminating unit; returns the units written (not counting it). Bad UTF-8 bytes become U+FFFD. */
static inline size_t cdlg_utf8_to_utf16(const char *s, uint8_t *out, size_t max) {
	size_t o = 0;
	for (const uint8_t *p = (const uint8_t *)s; p && *p; ) {
		uint32_t u = *p; int k = 1;
		if (u >= 0xf0 && u < 0xf8) { k = 4; u &= 0x07; } else if (u >= 0xe0 && u < 0xf0) { k = 3; u &= 0x0f; } else if (u >= 0xc0 && u < 0xe0) { k = 2; u &= 0x1f; } else if (u >= 0x80) u = 0xfffd;
		if (u != 0xfffd && k > 1) {
			for (int j = 1; j < k; j++) { if ((p[j] & 0xc0) != 0x80) { k = j; u = 0xfffd; break; } u = u << 6 | (p[j] & 0x3fu); }
		}
		p += k;
		if (u > 0x10ffff) u = 0xfffd;
		const size_t need = u >= 0x10000 ? 2 : 1;
		if (o + need > max) break;
		if (u >= 0x10000) { u -= 0x10000; const uint32_t hi = 0xd800 + (u >> 10), lo = 0xdc00 + (u & 0x3ff);
			out[2 * o] = (uint8_t)hi; out[2 * o + 1] = (uint8_t)(hi >> 8); out[2 * o + 2] = (uint8_t)lo; out[2 * o + 3] = (uint8_t)(lo >> 8); }
		else { out[2 * o] = (uint8_t)u; out[2 * o + 1] = (uint8_t)(u >> 8); }
		o += need;
	}
	out[2 * o] = out[2 * o + 1] = 0;
	return o;
}

/* how many UTF-16 units the UTF-8 text takes (what a game's maxTextLength counts) */
static inline size_t cdlg_utf16_units(const char *s) {
	size_t n = 0;
	for (const uint8_t *p = (const uint8_t *)s; p && *p; p++) if ((*p & 0xc0) != 0x80) n += *p >= 0xf0 ? 2 : 1;
	return n;
}

/* ---- the message dialog (sceMsgDialog) */
typedef struct {
	int kind;             /* 0 none up; 1 buttons waiting on the player; 2 a card with none (the game closes it) */
	int seq;              /* changes with every dialog: the overlay resets its selection */
	int percent;          /* a progress bar's 0..100, or -1 for none */
	int nbuttons;         /* 1..3 */
	int back;             /* the button Circle / Esc presses (an index), or -1 */
	int enter_circle;     /* the console's enter button is Circle (else Cross) */
	char label[3][48];
	char msg[520];
} MsgDlgView;

/* the dialog now, under its lock: returns v->kind; every msgdlg_view must be followed by msgdlg_release */
int msgdlg_view(MsgDlgView *v);
void msgdlg_release(void);
/* the player's answer to dialog `seq` (dropped if it has gone): a button index 0..nbuttons-1 */
void msgdlg_answer(int seq, int button);

/* ---- the text-input dialog (sceImeDialog) */
typedef struct {
	int kind;             /* 1 while the dialog is up, else 0 */
	int seq;
	int type;             /* SceImeType: 0 default, 1 basic latin, 2 number, 3 extended number, 4 URL, 5 mail */
	int multiline, password;
	int enter_circle;
	unsigned maxlen;      /* UTF-16 units */
	char title[520];
	char enter_label[16]; /* the confirm button's text */
	const char *text;     /* the initial text, UTF-8: valid until imedlg_release */
} ImeDlgView;

int imedlg_view(ImeDlgView *v);
void imedlg_release(void);
/* the player's answer to dialog `seq`: OK with the text (UTF-8, cut to the game's maxTextLength), or cancelled */
void imedlg_answer(int seq, int ok, const char *text);

/* one of these two is waiting on the player's keys and pad (window.c then routes them to the overlay) */
int cdlg_active(void);

#ifdef __cplusplus
}
#endif
#endif
