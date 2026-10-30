#ifndef RECOMP_CONTROLLER_LAYOUT_H
#define RECOMP_CONTROLLER_LAYOUT_H

#include <string.h>

enum { CONTROLLER_LAYOUT_AUTO, CONTROLLER_LAYOUT_TOUCH, CONTROLLER_LAYOUT_BUTTONS };

static inline int controller_layout_parse(const char *value) {
	if (!value || !*value) return -1;
	if (!strcmp(value, "auto") || !strcmp(value, "0")) return CONTROLLER_LAYOUT_AUTO;
	if (!strcmp(value, "touch") || !strcmp(value, "1")) return CONTROLLER_LAYOUT_TOUCH;
	if (!strcmp(value, "buttons") || !strcmp(value, "pstv") || !strcmp(value, "2")) return CONTROLLER_LAYOUT_BUTTONS;
	return -1;
}

static inline int controller_layout_auto_buttons(const char *input_mode, int keyboard_active, int gamepad_present, int headless) {
	if (input_mode && !strcmp(input_mode, "pad")) return 1;
	if (input_mode && !strcmp(input_mode, "kbm")) return 0;
	return !headless && !keyboard_active && gamepad_present;
}

#endif
