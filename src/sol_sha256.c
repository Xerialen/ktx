#include "sol_sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
	UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
	UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
	UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
	UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
	UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
	UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
	UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
	UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
	UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
	UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
	UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
	UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
	UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
	UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
	UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
	UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
	UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
	UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
	UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
	UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
	UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
	UINT32_C(0xc67178f2)
};

static uint32_t rotate_right(uint32_t value, unsigned count)
{
	return (value >> count) | (value << (32u - count));
}

static uint32_t read_u32_be(const uint8_t *input)
{
	return ((uint32_t)input[0] << 24) |
		((uint32_t)input[1] << 16) |
		((uint32_t)input[2] << 8) |
		(uint32_t)input[3];
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
	output[0] = (uint8_t)(value >> 24);
	output[1] = (uint8_t)(value >> 16);
	output[2] = (uint8_t)(value >> 8);
	output[3] = (uint8_t)value;
}

static void write_u64_be(uint8_t *output, uint64_t value)
{
	unsigned i;

	for (i = 0; i < 8u; ++i)
		output[i] = (uint8_t)(value >> ((7u - i) * 8u));
}

static void transform(sol_sha256_context_v1 *context, const uint8_t *block)
{
	uint32_t words[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned i;

	for (i = 0; i < 16u; ++i)
		words[i] = read_u32_be(block + (i * 4u));
	for (i = 16u; i < 64u; ++i) {
		uint32_t sigma0 = rotate_right(words[i - 15u], 7u) ^
			rotate_right(words[i - 15u], 18u) ^ (words[i - 15u] >> 3);
		uint32_t sigma1 = rotate_right(words[i - 2u], 17u) ^
			rotate_right(words[i - 2u], 19u) ^ (words[i - 2u] >> 10);

		words[i] = words[i - 16u] + sigma0 + words[i - 7u] + sigma1;
	}
	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];
	for (i = 0; i < 64u; ++i) {
		uint32_t choice = (e & f) ^ ((~e) & g);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t sigma0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^
			rotate_right(a, 22u);
		uint32_t sigma1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^
			rotate_right(e, 25u);
		uint32_t temporary1 = h + sigma1 + choice + round_constants[i] +
			words[i];
		uint32_t temporary2 = sigma0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;
}

int sol_sha256_init_v1(sol_sha256_context_v1 *context)
{
	if (!context)
		return 0;
	memset(context, 0, sizeof(*context));
	context->state[0] = UINT32_C(0x6a09e667);
	context->state[1] = UINT32_C(0xbb67ae85);
	context->state[2] = UINT32_C(0x3c6ef372);
	context->state[3] = UINT32_C(0xa54ff53a);
	context->state[4] = UINT32_C(0x510e527f);
	context->state[5] = UINT32_C(0x9b05688c);
	context->state[6] = UINT32_C(0x1f83d9ab);
	context->state[7] = UINT32_C(0x5be0cd19);
	context->initialized = 1u;
	return 1;
}

int sol_sha256_update_v1(sol_sha256_context_v1 *context,
	const uint8_t *data, size_t length)
{
	size_t take;

	if (!context || context->initialized != 1u || context->finalized ||
		context->block_length >= SOL_SHA256_BLOCK_SIZE_V1 ||
		(!data && length != 0u) ||
		context->total_bytes > UINT64_MAX / UINT64_C(8) ||
		length > UINT64_MAX / UINT64_C(8) - context->total_bytes)
		return 0;
	if (length == 0u)
		return 1;
	context->total_bytes += (uint64_t)length;
	if (context->block_length != 0u) {
		take = SOL_SHA256_BLOCK_SIZE_V1 - context->block_length;
		if (take > length)
			take = length;
		memcpy(context->block + context->block_length, data, take);
		context->block_length += take;
		data += take;
		length -= take;
		if (context->block_length == SOL_SHA256_BLOCK_SIZE_V1) {
			transform(context, context->block);
			context->block_length = 0u;
		}
	}
	while (length >= SOL_SHA256_BLOCK_SIZE_V1) {
		transform(context, data);
		data += SOL_SHA256_BLOCK_SIZE_V1;
		length -= SOL_SHA256_BLOCK_SIZE_V1;
	}
	if (length != 0u) {
		memcpy(context->block, data, length);
		context->block_length = length;
	}
	return 1;
}

int sol_sha256_final_v1(sol_sha256_context_v1 *context,
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1])
{
	uint8_t result[SOL_SHA256_DIGEST_SIZE_V1];
	uint64_t bit_length;
	unsigned i;

	if (!context || !digest || context->initialized != 1u ||
		context->finalized ||
		context->block_length >= SOL_SHA256_BLOCK_SIZE_V1 ||
		context->total_bytes > UINT64_MAX / UINT64_C(8))
		return 0;
	bit_length = context->total_bytes * UINT64_C(8);
	context->block[context->block_length++] = UINT8_C(0x80);
	if (context->block_length > 56u) {
		memset(context->block + context->block_length, 0,
			SOL_SHA256_BLOCK_SIZE_V1 - context->block_length);
		transform(context, context->block);
		context->block_length = 0u;
	}
	memset(context->block + context->block_length, 0,
		56u - context->block_length);
	write_u64_be(context->block + 56u, bit_length);
	transform(context, context->block);
	for (i = 0; i < 8u; ++i)
		write_u32_be(result + (i * 4u), context->state[i]);
	memset(context->block, 0, sizeof(context->block));
	context->block_length = 0u;
	context->finalized = 1u;
	memcpy(digest, result, sizeof(result));
	return 1;
}

int sol_sha256_digest_v1(const uint8_t *data, size_t length,
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1])
{
	sol_sha256_context_v1 context;

	if (!digest || (!data && length != 0u))
		return 0;
	return sol_sha256_init_v1(&context) &&
		sol_sha256_update_v1(&context, data, length) &&
		sol_sha256_final_v1(&context, digest);
}
