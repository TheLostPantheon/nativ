#include <stdio.h>
#include "../input/controller_layout.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main(void) {
	CHECK(controller_layout_parse("auto") == CONTROLLER_LAYOUT_AUTO);
	CHECK(controller_layout_parse("0") == CONTROLLER_LAYOUT_AUTO);
	CHECK(controller_layout_parse("touch") == CONTROLLER_LAYOUT_TOUCH);
	CHECK(controller_layout_parse("1") == CONTROLLER_LAYOUT_TOUCH);
	CHECK(controller_layout_parse("buttons") == CONTROLLER_LAYOUT_BUTTONS);
	CHECK(controller_layout_parse("pstv") == CONTROLLER_LAYOUT_BUTTONS);
	CHECK(controller_layout_parse("2") == CONTROLLER_LAYOUT_BUTTONS);
	CHECK(controller_layout_parse("") < 0);
	CHECK(controller_layout_parse("invalid") < 0);

	CHECK(controller_layout_auto_buttons("pad", 1, 0, 1));
	CHECK(!controller_layout_auto_buttons("kbm", 0, 1, 0));
	CHECK(controller_layout_auto_buttons("auto", 0, 1, 0));
	CHECK(!controller_layout_auto_buttons("auto", 1, 1, 0));
	CHECK(!controller_layout_auto_buttons("auto", 0, 0, 0));
	CHECK(!controller_layout_auto_buttons("auto", 0, 1, 1));

	printf("controller layout: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
