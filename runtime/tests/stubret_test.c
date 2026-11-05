/* stubret_test.c -- what an unimplemented import returns (runtime/core/stubret.h). The device/network families the
 * host doesn't emulate answer their "not initialised" error for operational calls, but their set-up/tear-down calls
 * and every common dialog's GetStatus answer 0: a dialog the host never opens is SCE_COMMON_DIALOG_STATUS_NONE.
 * sceNpMessageDialogGetStatus / sceNpProfileDialogGetStatus used to answer 0x80550002, which a game polling every
 * dialog before opening its own read as "a dialog is still running" -- its save-data dialog never opened.
 *
 * The dialog cases are the fail-before / pass-after cases; -DSTUBRET_UNDER_TEST='"<file>"' compiles the fixture
 * against another copy of the policy (the pre-fix rules) for the fail-before run. The rest pin the existing policy.
 */
#include <stdint.h>
#include <stdio.h>
#ifndef STUBRET_UNDER_TEST
#define STUBRET_UNDER_TEST "../core/stubret.h"
#endif
#include STUBRET_UNDER_TEST

static int fails;
static void check(const char *name, int zero, uint32_t want) {
	uint32_t got = stub_family_result(name, zero);
	printf("%-44s zero=%d -> %#010x %s\n", name, zero, got, got == want ? "ok" : "FAILED");
	if (got != want) fails++;
}

int main(void) {
	/* fail-before / pass-after: dialogs never opened have status NONE */
	check("sceNpMessageDialogGetStatus", 0, 0);
	check("sceNpProfileDialogGetStatus", 0, 0);
	check("sceNpSnsFacebookDialogGetStatus", 0, 0);
	check("sceNpFriendListDialogGetStatus", 0, 0);
	/* unchanged: operational NP/network calls report the family error */
	check("sceNpManagerGetAccountRegion", 0, 0x80550002u);
	check("sceNpMessageDialogGetResult", 0, 0x80550002u);
	check("sceNetCtlInetGetState", 0, 0x80412101u);
	check("sceNetSocket", 0, 0x80410101u);
	check("sceHttpsLoadCert", 0, 0x80431001u);
	check("sceRudpBind", 0, 0x80770001u);
	/* unchanged: set-up / tear-down succeed, other families and RECOMP_STUB_ZERO answer 0 */
	check("sceNpInit", 0, 0);
	check("sceNpMessageDialogInit", 0, 0);
	check("sceNpMessageDialogTerm", 0, 0);
	check("sceNpRegisterServiceStateCallback", 0, 0);
	check("sceImeUpdate", 0, 0);
	check("sceNpManagerGetAccountRegion", 1, 0);

	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
