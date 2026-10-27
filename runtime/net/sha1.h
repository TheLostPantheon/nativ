#pragma once
#include <stddef.h>
#include <stdint.h>
void sha1(const void *data, size_t n, uint8_t out[20]);
void hmac_sha1(const void *key, size_t kn, const void *data, size_t n, uint8_t out[20]);
