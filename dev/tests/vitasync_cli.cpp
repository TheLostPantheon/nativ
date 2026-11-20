/* the launcher's Vita sync (launcher/vitasync.cpp) alone, for dev/tests/vitasync_test.py: VSYNC_PREFS and VSYNC_BACKUPS
 * name its folders, the arguments are nativ --vita-sync's */
#include "vitasync.h"
#include <cstdlib>
int main(int argc, char **argv) {
	vsync::init(getenv("VSYNC_PREFS") ? getenv("VSYNC_PREFS") : ".", getenv("VSYNC_BACKUPS") ? getenv("VSYNC_BACKUPS") : "backups", "Test PC");
	return vsync::cli(argc - 1, argv + 1);
}
