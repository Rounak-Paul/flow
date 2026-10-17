#pragma once
#include <stddef.h>
#include <stdint.h>

/* Incremental SHA-256 (FIPS 180-4) used for content-addressed storage. */
typedef struct Fl_Sha256 {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} Fl_Sha256;

/* Reset hash to the initial state. */
void fl_sha256_init(Fl_Sha256 *hash);
/* Absorb size bytes from data; data may be NULL only when size is zero. */
void fl_sha256_update(Fl_Sha256 *hash, const void *data, size_t size);
/* Finish hashing into out; hash must be re-initialised before reuse. */
void fl_sha256_final(Fl_Sha256 *hash, unsigned char out[32]);
/* Format a digest as 64 lowercase hexadecimal characters plus a terminator. */
void fl_sha256_hex(const unsigned char digest[32], char out[65]);
