#ifndef CONTROLLER_EVIDENCE_PROTOCOL_H
#define CONTROLLER_EVIDENCE_PROTOCOL_H

#include <stdint.h>

#define CE_EXTENSION_NAME_V1 "ControllerEvidenceV1"
#define CE_PROTOCOL_VERSION_V1 1u
#define CE_COMMAND_BYTES_V1_SIZE 25u

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
	CE_UNBIND = 5
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

typedef struct ce_unbind_v1 {
	ce_payload_header_v1 header;
	uint32_t engine_slot;
	uint32_t client_generation;
	char run_nonce[CE_RUN_NONCE_CAP];
	char seat_nonce[CE_SEAT_NONCE_CAP];
} ce_unbind_v1;

#endif
