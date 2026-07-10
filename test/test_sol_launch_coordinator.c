#include "sol_launch_coordinator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static sol_launch_metadata_v1 metadata_for(size_t index)
{
	sol_launch_metadata_v1 metadata;

	memset(&metadata, 0, sizeof(metadata));
	snprintf(metadata.seat_nonce, sizeof(metadata.seat_nonce), "%064u",
		(unsigned) index + 1u);
	strcpy(metadata.controller_id, "controller");
	strcpy(metadata.controller_version, "v1");
	strcpy(metadata.controller_digest,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111");
	strcpy(metadata.build_id,
		"sha256:2222222222222222222222222222222222222222222222222222222222222222");
	strcpy(metadata.config_sha256,
		"3333333333333333333333333333333333333333333333333333333333333333");
	strcpy(metadata.treatment_digest,
		"sha256:4444444444444444444444444444444444444444444444444444444444444444");
	strcpy(metadata.writer_id, "writer");
	return metadata;
}

static void test_alternating_eight_launches_allow_only_one_pending_seat(void)
{
	static const size_t launch_order[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
		0u, 4u, 1u, 5u, 2u, 6u, 3u, 7u
	};
	sol_launch_coordinator_v1 *coordinator = sol_launch_coordinator_create_v1();
	size_t launched;

	require(coordinator != NULL, "launch coordinator allocation succeeds");
	for (launched = 0; launched < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++launched)
	{
		size_t index = launch_order[launched];
		size_t pending = SOL_KTX_EVIDENCE_SEAT_COUNT_V1;
		sol_launch_metadata_v1 metadata = metadata_for(index);
		sol_launch_metadata_v1 blocked = metadata_for((index + 1u) %
			SOL_KTX_EVIDENCE_SEAT_COUNT_V1);
		const sol_launch_metadata_v1 *stored;

		require(sol_launch_coordinator_configure_v1(coordinator, index, &metadata),
				"alternating candidate/control seat becomes the sole launch pending");
		require(!sol_launch_coordinator_configure_v1(coordinator,
				(index + 1u) % SOL_KTX_EVIDENCE_SEAT_COUNT_V1, &blocked),
				"a second evidencebind cannot overlap the pending selector launch");
		stored = sol_launch_coordinator_pending_v1(coordinator, &pending);
		require(stored != NULL && pending == index
			&& !memcmp(stored, &metadata, sizeof(metadata)),
				"pending selector retains the exact sealed metadata for its seat");
		require(sol_launch_coordinator_complete_v1(coordinator, index)
			&& sol_launch_coordinator_pending_v1(coordinator, &pending) == NULL
			&& pending == SOL_KTX_EVIDENCE_SEAT_COUNT_V1,
				"successful selector launch clears only the pending marker");
	}
	for (launched = 0; launched < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++launched)
	{
		sol_launch_metadata_v1 duplicate = metadata_for(launched);

		require(sol_launch_coordinator_seat_v1(coordinator, launched) != NULL
			&& !sol_launch_coordinator_configure_v1(coordinator, launched, &duplicate),
				"all eight configured seats remain immutable after launch completion");
	}
	sol_launch_coordinator_destroy_v1(coordinator);
}

int main(void)
{
	test_alternating_eight_launches_allow_only_one_pending_seat();
	printf("sol_launch_coordinator: alternating eight-seat contract passed\n");
	return 0;
}
