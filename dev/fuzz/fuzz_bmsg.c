/* In-game messages between players (basic_msg.c bmsg_input), from a linked copy or the LAN. */
#include <stddef.h>
#include <stdint.h>
void bmsg_input(const uint8_t *b, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { bmsg_input(d, n); return 0; }
