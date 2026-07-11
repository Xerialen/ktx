#include "sol_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void require_digest(const uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1],
	const char *expected_hex, const char *message)
{
	uint8_t expected[SOL_SHA256_DIGEST_SIZE_V1];
	size_t i;

	require(strlen(expected_hex) == SOL_SHA256_DIGEST_SIZE_V1 * 2u,
		"SHA-256 expected fixture has exact length");
	for (i = 0; i < SOL_SHA256_DIGEST_SIZE_V1; ++i) {
		unsigned value;

		require(sscanf(expected_hex + (i * 2u), "%2x", &value) == 1,
			"SHA-256 expected fixture parses");
		expected[i] = (uint8_t)value;
	}
	require(memcmp(digest, expected, sizeof(expected)) == 0, message);
}

static void test_one_shot_fips_vectors(void)
{
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1];

	require(sol_sha256_digest_v1(NULL, 0u, digest),
		"one-shot SHA-256 accepts the empty message");
	require_digest(digest,
		"e3b0c44298fc1c149afbf4c8996fb924"
		"27ae41e4649b934ca495991b7852b855",
		"empty-message SHA-256 matches FIPS 180-4");
	require(sol_sha256_digest_v1((const uint8_t *)"abc", 3u, digest),
		"one-shot SHA-256 accepts abc");
	require_digest(digest,
		"ba7816bf8f01cfea414140de5dae2223"
		"b00361a396177a9cb410ff61f20015ad",
		"abc SHA-256 matches FIPS 180-4");
}

static void test_incremental_rfc_vector_crosses_chunks(void)
{
	static const uint8_t message[] =
		"abcdbcdecdefdefgefghfghighijhijk"
		"ijkljklmklmnlmnomnopnopq";
	sol_sha256_context_v1 context;
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1];
	size_t offset = 0u;
	static const size_t chunks[] = {1u, 7u, 13u, 2u, 19u, 14u};
	size_t chunk_index = 0u;

	require(sizeof(message) - 1u == 56u,
		"incremental SHA-256 fixture has the RFC 6234 length");
	require(sol_sha256_init_v1(&context),
		"incremental SHA-256 context initializes");
	while (offset < sizeof(message) - 1u) {
		size_t chunk = chunks[chunk_index++ %
			(sizeof(chunks) / sizeof(chunks[0]))];

		if (chunk > sizeof(message) - 1u - offset)
			chunk = sizeof(message) - 1u - offset;
		require(sol_sha256_update_v1(&context, message + offset, chunk),
			"incremental SHA-256 accepts an irregular chunk");
		offset += chunk;
	}
	require(sol_sha256_final_v1(&context, digest),
		"incremental SHA-256 finalizes");
	require_digest(digest,
		"248d6a61d20638b8e5c026930c3e6039"
		"a33ce45964ff2167f6ecedd419db06c1",
		"irregular chunks match the RFC 6234 SHA-256 vector");
}

static void test_million_a_streaming_vector(void)
{
	uint8_t thousand_as[1000];
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1];
	sol_sha256_context_v1 context;
	unsigned i;

	memset(thousand_as, 'a', sizeof(thousand_as));
	require(sol_sha256_init_v1(&context),
		"million-a SHA-256 context initializes");
	for (i = 0; i < 1000u; ++i) {
		require(sol_sha256_update_v1(&context, thousand_as,
			sizeof(thousand_as)), "million-a SHA-256 accepts each chunk");
	}
	require(sol_sha256_final_v1(&context, digest),
		"million-a SHA-256 finalizes");
	require_digest(digest,
		"cdc76e5c9914fb9281a1c7e284d73e67"
		"f1809a48a497200e046d39ccc7112cd0",
		"million-a SHA-256 matches the FIPS 180-4 vector");
}

static void test_lifecycle_and_argument_guards(void)
{
	sol_sha256_context_v1 context;
	uint8_t digest[SOL_SHA256_DIGEST_SIZE_V1];
	uint8_t byte = 'a';

	require(!sol_sha256_init_v1(NULL), "SHA-256 rejects a null context");
	require(sol_sha256_init_v1(&context), "guard context initializes");
	require(sol_sha256_update_v1(&context, NULL, 0u),
		"SHA-256 accepts a null pointer for an empty update");
	require(!sol_sha256_update_v1(&context, NULL, 1u),
		"SHA-256 rejects a null pointer for a nonempty update");
	require(sol_sha256_update_v1(&context, (const uint8_t *)"a", 1u),
		"a rejected update leaves the context usable");
	require(sol_sha256_update_v1(&context, NULL, 0u),
		"empty null update is valid while a partial block is buffered");
	require(sol_sha256_update_v1(&context, (const uint8_t *)"bc", 2u),
		"streaming resumes after an empty null update");
	require(sol_sha256_final_v1(&context, digest),
		"guard context finalizes once");
	require_digest(digest,
		"ba7816bf8f01cfea414140de5dae2223"
		"b00361a396177a9cb410ff61f20015ad",
		"rejected update did not mutate the SHA-256 state");
	require(!sol_sha256_final_v1(&context, digest),
		"SHA-256 rejects a second finalization");
	require(!sol_sha256_update_v1(&context, &byte, 1u),
		"SHA-256 rejects updates after finalization");
	require(!sol_sha256_digest_v1(&byte, 1u, NULL),
		"one-shot SHA-256 rejects a null digest");
	require(!sol_sha256_digest_v1(NULL, 1u, digest),
		"one-shot SHA-256 rejects null nonempty input");
	require(sol_sha256_init_v1(&context),
		"overflow guard context initializes");
	context.total_bytes = UINT64_MAX / UINT64_C(8);
	require(!sol_sha256_update_v1(&context, &byte, 1u),
		"SHA-256 rejects byte-count overflow before mutating state");
}

int main(void)
{
	test_one_shot_fips_vectors();
	test_incremental_rfc_vector_crosses_chunks();
	test_million_a_streaming_vector();
	test_lifecycle_and_argument_guards();
	printf("sol_sha256: 4 contract tests passed\n");
	return 0;
}
