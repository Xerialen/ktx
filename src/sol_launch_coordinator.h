#ifndef SOL_LAUNCH_COORDINATOR_H
#define SOL_LAUNCH_COORDINATOR_H

#include <stddef.h>

#include "controller_evidence_protocol.h"
#include "sol_ktx_adapter.h"

typedef struct sol_launch_coordinator_v1 sol_launch_coordinator_v1;

typedef struct sol_launch_metadata_v1
{
	char seat_nonce[CE_SEAT_NONCE_CAP];
	char controller_id[CE_CONTROLLER_ID_CAP];
	char controller_version[CE_CONTROLLER_VERSION_CAP];
	char controller_digest[CE_SHA256_ID_CAP];
	char build_id[CE_BUILD_ID_CAP];
	char config_sha256[CE_SHA256_HEX_CAP];
	char treatment_digest[CE_SHA256_ID_CAP];
	char writer_id[CE_WRITER_ID_CAP];
} sol_launch_metadata_v1;

sol_launch_coordinator_v1 *sol_launch_coordinator_create_v1(void);
void sol_launch_coordinator_destroy_v1(sol_launch_coordinator_v1 *coordinator);

int sol_launch_coordinator_configure_v1(sol_launch_coordinator_v1 *coordinator,
	size_t index, const sol_launch_metadata_v1 *metadata);
const sol_launch_metadata_v1 *sol_launch_coordinator_pending_v1(
	const sol_launch_coordinator_v1 *coordinator, size_t *index);
int sol_launch_coordinator_complete_v1(sol_launch_coordinator_v1 *coordinator,
	size_t index);
int sol_launch_coordinator_all_complete_v1(
	const sol_launch_coordinator_v1 *coordinator);
const sol_launch_metadata_v1 *sol_launch_coordinator_seat_v1(
	const sol_launch_coordinator_v1 *coordinator, size_t index);

#endif
