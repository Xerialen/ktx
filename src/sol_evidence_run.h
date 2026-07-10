#ifndef SOL_EVIDENCE_RUN_H
#define SOL_EVIDENCE_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "controller_evidence_protocol.h"

enum
{
	SOL_EVIDENCE_RUN_SEATS_V1 = 8
};

typedef enum sol_evidence_bind_record_result_v1
{
	SOL_EVIDENCE_BIND_REJECTED = 0,
	SOL_EVIDENCE_BIND_ACCEPTED = 1,
	SOL_EVIDENCE_BIND_RETAINED_CONFLICT = 2
} sol_evidence_bind_record_result_v1;

typedef enum sol_evidence_binding_lookup_result_v1
{
	SOL_EVIDENCE_BINDING_NONE = 0,
	SOL_EVIDENCE_BINDING_EXACT = 1,
	SOL_EVIDENCE_BINDING_AMBIGUOUS = 2
} sol_evidence_binding_lookup_result_v1;

typedef struct sol_evidence_run_v1 sol_evidence_run_v1;

typedef intptr_t (*sol_evidence_run_call_v1)(void *context,
	intptr_t operation, void *payload, intptr_t payload_size);
typedef void (*sol_evidence_run_remove_v1)(void *context, size_t index,
	uint32_t engine_slot);

typedef enum sol_evidence_cleanup_result_v1
{
	SOL_EVIDENCE_CLEANUP_RETRY = -1,
	SOL_EVIDENCE_CLEANUP_IDLE = 0,
	SOL_EVIDENCE_CLEANUP_COMPLETE = 1
} sol_evidence_cleanup_result_v1;

sol_evidence_run_v1 *sol_evidence_run_create_v1(void);
void sol_evidence_run_destroy_v1(sol_evidence_run_v1 *run);

int sol_evidence_run_begin_v1(sol_evidence_run_v1 *run,
	const char *run_nonce, const char *epoch_kind,
	sol_evidence_run_call_v1 call, void *call_context);
int sol_evidence_run_matches_v1(const sol_evidence_run_v1 *run,
	const char *run_nonce, const char *epoch_kind);
const char *sol_evidence_run_nonce_v1(const sol_evidence_run_v1 *run);
int sol_evidence_run_configure_seat_v1(sol_evidence_run_v1 *run,
	size_t index, const char *seat_nonce);
int sol_evidence_run_seat_configured_v1(const sol_evidence_run_v1 *run,
	size_t index);

int sol_evidence_run_record_client_v1(sol_evidence_run_v1 *run,
	size_t index, uint32_t engine_slot);
/*
 * Call only after CE_BIND returns OK.  Every syntactically valid successful
 * route is retained for safe UNBIND; RETAINED_CONFLICT reports that the route
 * conflicts with local client/run state and the caller must fail-stop.
 */
sol_evidence_bind_record_result_v1 sol_evidence_run_record_bind_v1(
	sol_evidence_run_v1 *run,
	size_t index, uint32_t engine_slot, uint32_t client_generation);
int sol_evidence_run_binding_v1(const sol_evidence_run_v1 *run,
	size_t index, uint32_t *engine_slot, uint32_t *client_generation);
int sol_evidence_run_find_client_v1(const sol_evidence_run_v1 *run,
	uint32_t engine_slot, size_t *index);
sol_evidence_binding_lookup_result_v1 sol_evidence_run_find_binding_v1(
	const sol_evidence_run_v1 *run, uint32_t engine_slot, size_t *index,
	uint32_t *client_generation);

void sol_evidence_run_request_close_v1(sol_evidence_run_v1 *run);
void sol_evidence_run_fail_stop_v1(sol_evidence_run_v1 *run);
int sol_evidence_run_note_disconnect_v1(sol_evidence_run_v1 *run,
	size_t index);
int sol_evidence_run_active_v1(const sol_evidence_run_v1 *run);
int sol_evidence_run_emissions_open_v1(const sol_evidence_run_v1 *run);
int sol_evidence_run_cleanup_pending_v1(const sol_evidence_run_v1 *run);

sol_evidence_cleanup_result_v1 sol_evidence_run_server_cleanup_v1(
	sol_evidence_run_v1 *run, sol_evidence_run_remove_v1 remove,
	void *remove_context);

#endif
