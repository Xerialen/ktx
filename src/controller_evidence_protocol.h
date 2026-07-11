#ifndef CONTROLLER_EVIDENCE_PROTOCOL_H
#define CONTROLLER_EVIDENCE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define CE_EXTENSION_NAME_V1 "ControllerEvidenceV1"
#define CE_PROTOCOL_VERSION_V1 1u
#define CE_COMMAND_BYTES_V1_SIZE 25u
#define CE_ACTION_RESPONSE_MAX_BYTES_V1 115u
#define CE_DECISION_TRACE_MAX_BYTES_V1 81u

#define CE_EPOCH_KIND_CAP 48u
#define CE_SEAT_ID_CAP 32u
#define CE_PLAYER_NAME_CAP 32u
#define CE_TEAM_CAP 32u
#define CE_CONTROLLER_ID_CAP 96u
#define CE_CONTROLLER_VERSION_CAP 64u
#define CE_SHA256_HEX_CAP 65u
#define CE_SHA256_ID_CAP 72u
#define CE_RUN_NONCE_CAP CE_SHA256_HEX_CAP
#define CE_SEAT_NONCE_CAP CE_SHA256_HEX_CAP
#define CE_BUILD_ID_CAP 128u
#define CE_WRITER_ID_CAP 64u
#define CE_WRITER_ID_MAX_LENGTH_V1 31u

/*
 * One mapped syscall takes (operation, payload pointer, payload size).
 * The operation names retain the v1 ABI wording. CE_MATCH_BEGIN/END delimit
 * a typed evidence epoch; an epoch need not claim that KTX started a match.
 */
typedef enum ce_operation_v1 {
	CE_BIND = 1,
	CE_FRAME_REQUEST = 2,
	CE_MATCH_BEGIN = 3,
	CE_MATCH_END = 4,
	CE_UNBIND = 5,
	CE_FRAME_REPLACE = 6,
	CE_FRAME_DECISION = 7,
	CE_MATCH_TIMELINE_BEGIN = 8
} ce_operation_v1;

enum {
	CE_RESULT_INVALID = -1,
	CE_RESULT_IGNORED = 0,
	CE_RESULT_OK = 1
};

typedef struct ce_payload_header_v1 {
	uint32_t protocol_version;
	uint32_t struct_size;
} ce_payload_header_v1;

typedef struct ce_command_wire_v1 {
	uint8_t bytes[CE_COMMAND_BYTES_V1_SIZE];
} ce_command_wire_v1;

typedef struct ce_epoch_begin_v1 {
	ce_payload_header_v1 header;
	char run_nonce[CE_RUN_NONCE_CAP];
	char epoch_kind[CE_EPOCH_KIND_CAP];
	uint64_t epoch_id;
} ce_epoch_begin_v1;

typedef struct ce_epoch_end_v1 {
	ce_payload_header_v1 header;
	char run_nonce[CE_RUN_NONCE_CAP];
} ce_epoch_end_v1;

/* KTX supplies only the active run identity. The engine replaces zero with
 * the exact decoded-MVD millisecond projection at the semantic match event. */
typedef struct ce_match_timeline_begin_v1 {
	ce_payload_header_v1 header;
	char run_nonce[CE_RUN_NONCE_CAP];
	uint64_t projected_mvd_origin_ms;
} ce_match_timeline_begin_v1;

_Static_assert(offsetof(ce_match_timeline_begin_v1,
	projected_mvd_origin_ms) == 80u,
	"CE match timeline timestamp ABI offset");
_Static_assert(sizeof(ce_match_timeline_begin_v1) == 88u,
	"CE match timeline payload ABI size");

typedef struct ce_bind_v1 {
	ce_payload_header_v1 header;
	uint32_t engine_slot;
	uint32_t client_generation; /* Engine-authored output. */
	char run_nonce[CE_RUN_NONCE_CAP];
	char seat_id[CE_SEAT_ID_CAP];
	char seat_nonce[CE_SEAT_NONCE_CAP];
	char observed_player_name[CE_PLAYER_NAME_CAP]; /* Engine-authored output. */
	char observed_team[CE_TEAM_CAP];               /* Engine-authored output. */
	char controller_id[CE_CONTROLLER_ID_CAP];
	char controller_version[CE_CONTROLLER_VERSION_CAP];
	char controller_digest[CE_SHA256_ID_CAP];
	char build_id[CE_BUILD_ID_CAP];
	char config_sha256[CE_SHA256_HEX_CAP];
	char treatment_digest[CE_SHA256_ID_CAP];
	char writer_id[CE_WRITER_ID_CAP];
} ce_bind_v1;

typedef struct ce_frame_request_v1 {
	ce_payload_header_v1 header;
	uint32_t engine_slot;
	uint32_t client_generation;
	ce_command_wire_v1 requested_command;
} ce_frame_request_v1;

typedef struct ce_frame_decision_v1 {
	ce_payload_header_v1 header;
	uint32_t engine_slot;
	uint32_t client_generation;
	uint32_t action_response_length;
	uint32_t decision_trace_length;
	uint8_t action_response[CE_ACTION_RESPONSE_MAX_BYTES_V1];
	uint8_t decision_trace[CE_DECISION_TRACE_MAX_BYTES_V1];
} ce_frame_decision_v1;

_Static_assert(offsetof(ce_frame_decision_v1, header) == 0u,
	"CE decision header ABI offset");
_Static_assert(offsetof(ce_frame_decision_v1, engine_slot) == 8u,
	"CE decision slot ABI offset");
_Static_assert(offsetof(ce_frame_decision_v1, client_generation) == 12u,
	"CE decision generation ABI offset");
_Static_assert(offsetof(ce_frame_decision_v1, action_response_length) == 16u,
	"CE decision SAC1 length ABI offset");
_Static_assert(offsetof(ce_frame_decision_v1, decision_trace_length) == 20u,
	"CE decision SDT1 length ABI offset");
_Static_assert(offsetof(ce_frame_decision_v1, action_response) == 24u,
	"CE decision SAC1 ABI offset");
_Static_assert(offsetof(ce_frame_decision_v1, decision_trace) == 139u,
	"CE decision SDT1 ABI offset");
_Static_assert(sizeof(ce_frame_decision_v1) == 220u,
	"CE decision payload ABI size");

typedef struct ce_unbind_v1 {
	ce_payload_header_v1 header;
	uint32_t engine_slot;
	uint32_t client_generation;
	char run_nonce[CE_RUN_NONCE_CAP];
	char seat_nonce[CE_SEAT_NONCE_CAP];
} ce_unbind_v1;

#endif
