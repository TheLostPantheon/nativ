#ifndef AES128_H
#define AES128_H
#include <stdint.h>
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);   /* one block, ECB */
#endif
