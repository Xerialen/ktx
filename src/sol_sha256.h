#ifndef SOL_SHA256_H
#define SOL_SHA256_H

#include <stddef.h>
#include <stdint.h>

enum {
	SOL_SHA256_DIGEST_SIZE_V1 = 32,
	SOL_SHA256_BLOCK_SIZE_V1 = 64
};

typedef struct sol_sha256_context_v1 {
	uint32_t state[8];
	uint64_t total_bytes;
	uint8_t block[SOL_SHA256_BLOCK_SIZE_V1];
	size_t block_length;
	uint8_t initialized;
	uint8_t finalized;
} sol_sha256_context_v1;

/* Stack-owned streaming SHA-256. A context is single-use after finalization. */
int sol_sha256_init_v1(sol_sha256_context_v1 *context);
int sol_sha256_update_v1(sol_sha256_context_v1 *context,
	const uint8_t *data, size_t length);
int sol_sha256_final_v1(sol_sha256_context_v1 *context,
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1]);
int sol_sha256_digest_v1(const uint8_t *data, size_t length,
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1]);

#endif
