#include "sol_launch_coordinator.h"

#include <stdlib.h>
#include <string.h>

struct sol_launch_coordinator_v1
{
	int configured[SOL_KTX_EVIDENCE_SEAT_COUNT_V1];
	size_t pending_index;
	sol_launch_metadata_v1 metadata[SOL_KTX_EVIDENCE_SEAT_COUNT_V1];
};

static int valid_string(const char *value, size_t capacity)
{
	return value && value[0] && memchr(value, '\0', capacity) != NULL;
}

static int valid_metadata(const sol_launch_metadata_v1 *metadata)
{
	return metadata
		&& valid_string(metadata->seat_nonce, sizeof(metadata->seat_nonce))
		&& valid_string(metadata->controller_id, sizeof(metadata->controller_id))
		&& valid_string(metadata->controller_version,
			sizeof(metadata->controller_version))
		&& valid_string(metadata->controller_digest,
			sizeof(metadata->controller_digest))
		&& valid_string(metadata->build_id, sizeof(metadata->build_id))
		&& valid_string(metadata->config_sha256, sizeof(metadata->config_sha256))
		&& valid_string(metadata->treatment_digest,
			sizeof(metadata->treatment_digest))
		&& valid_string(metadata->writer_id, sizeof(metadata->writer_id));
}

sol_launch_coordinator_v1 *sol_launch_coordinator_create_v1(void)
{
	sol_launch_coordinator_v1 *coordinator = calloc(1u, sizeof(*coordinator));

	if (coordinator)
	{
		coordinator->pending_index = SOL_KTX_EVIDENCE_SEAT_COUNT_V1;
	}
	return coordinator;
}

void sol_launch_coordinator_destroy_v1(sol_launch_coordinator_v1 *coordinator)
{
	if (coordinator)
	{
		memset(coordinator, 0, sizeof(*coordinator));
	}
	free(coordinator);
}

int sol_launch_coordinator_configure_v1(sol_launch_coordinator_v1 *coordinator,
	size_t index, const sol_launch_metadata_v1 *metadata)
{
	if (!coordinator || index >= SOL_KTX_EVIDENCE_SEAT_COUNT_V1
		|| coordinator->pending_index != SOL_KTX_EVIDENCE_SEAT_COUNT_V1
		|| coordinator->configured[index] || !valid_metadata(metadata))
	{
		return 0;
	}
	coordinator->metadata[index] = *metadata;
	coordinator->configured[index] = 1;
	coordinator->pending_index = index;
	return 1;
}

const sol_launch_metadata_v1 *sol_launch_coordinator_pending_v1(
	const sol_launch_coordinator_v1 *coordinator, size_t *index)
{
	if (index)
	{
		*index = SOL_KTX_EVIDENCE_SEAT_COUNT_V1;
	}
	if (!coordinator
		|| coordinator->pending_index >= SOL_KTX_EVIDENCE_SEAT_COUNT_V1)
	{
		return NULL;
	}
	if (index)
	{
		*index = coordinator->pending_index;
	}
	return &coordinator->metadata[coordinator->pending_index];
}

int sol_launch_coordinator_complete_v1(sol_launch_coordinator_v1 *coordinator,
	size_t index)
{
	if (!coordinator || index >= SOL_KTX_EVIDENCE_SEAT_COUNT_V1
		|| coordinator->pending_index != index || !coordinator->configured[index])
	{
		return 0;
	}
	coordinator->pending_index = SOL_KTX_EVIDENCE_SEAT_COUNT_V1;
	return 1;
}

int sol_launch_coordinator_all_complete_v1(
	const sol_launch_coordinator_v1 *coordinator)
{
	size_t index;

	if (!coordinator
		|| coordinator->pending_index != SOL_KTX_EVIDENCE_SEAT_COUNT_V1)
	{
		return 0;
	}
	for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
	{
		if (!coordinator->configured[index])
		{
			return 0;
		}
	}
	return 1;
}

const sol_launch_metadata_v1 *sol_launch_coordinator_seat_v1(
	const sol_launch_coordinator_v1 *coordinator, size_t index)
{
	return coordinator && index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1
		&& coordinator->configured[index] ? &coordinator->metadata[index] : NULL;
}
