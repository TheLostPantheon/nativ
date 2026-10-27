/*
 * The save-data dialog's current step (apputil.c's SceSaveDataDialog), for the overlay to draw and answer
 * (overlay.cpp's build_save_dialog). The game thread opens and advances steps; the window thread reads the
 * step under savedlg_view's lock and answers it with savedlg_answer. Plain C, shared by both sides.
 */
#ifndef RECOMP_SAVEDLG_H
#define RECOMP_SAVEDLG_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SAVEDLG_NONE, SAVEDLG_LIST, SAVEDLG_YESNO, SAVEDLG_WAIT };   /* WAIT: a card with no buttons (Saving..., a progress bar) the game closes */
enum { SAVEDLG_CANCEL = -1, SAVEDLG_YES = 1, SAVEDLG_NO = 2 };

typedef struct {
	uint32_t id;          /* the slot id the game gave */
	int exists;           /* saved data is there (else a "new saved data" row) */
	char title[100], sub[200], detail[200], date[24];   /* UTF-8; detail's line breaks made separators */
} SaveDlgRow;

typedef struct {
	int kind;             /* SAVEDLG_*: NONE while the game has nothing waiting on the player */
	int seq;              /* changes with every new step: the overlay resets its selection */
	int disp;             /* 1 save, 2 load, 3 delete */
	int focus;            /* the list's initial row (the game's focus position) */
	int percent;          /* a progress bar's 0..100, or -1 for none */
	char title[128], msg[512];   /* the list's heading; a card's message */
	int nrows; const SaveDlgRow *rows;
} SaveDlgView;

/* the step now, under the dialog's lock: returns v->kind; v->rows stays valid until savedlg_release, which
 * must follow every savedlg_view (whatever it returned) */
int savedlg_view(SaveDlgView *v);
void savedlg_release(void);
/* the player's answer to step `seq` (an answer to a step that has since moved on is dropped): LIST, a row
 * index or SAVEDLG_CANCEL; YESNO, SAVEDLG_YES, SAVEDLG_NO or SAVEDLG_CANCEL */
void savedlg_answer(int seq, int choice);

#ifdef __cplusplus
}
#endif
#endif
