/*
 * The message dialog's current state (apputil.c's sceMsgDialog), for the overlay to draw and answer (overlay.cpp's
 * build_msg_dialog), as savedlg.h does for the save-data dialog. The game thread opens and closes it; the window
 * thread reads the state under the view's lock and answers. Plain C, shared by both sides.
 */
#ifndef RECOMP_COMMONDLG_H
#define RECOMP_COMMONDLG_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* a message dialog is waiting on the player's keys and pad (window.c then routes them to the overlay) */
int cdlg_active(void);

#ifdef __cplusplus
}
#endif
#endif
