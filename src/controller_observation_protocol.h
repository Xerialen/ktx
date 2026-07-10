#ifndef CONTROLLER_OBSERVATION_PROTOCOL_H
#define CONTROLLER_OBSERVATION_PROTOCOL_H

#include <stdint.h>

#define COV_EXTENSION_NAME_V1 "ControllerObservationV1"
#define COV_PROTOCOL_VERSION_V1 1u
#define COV_MAX_BATCH_BYTES_V1 65535u

typedef enum cov_operation_v1 {
	COV_GET_PROFILE = 1,
	COV_GET_COMMITTED = 2
} cov_operation_v1;

enum {
	COV_RESULT_INVALID = -1,
	COV_RESULT_EMPTY = 0,
	COV_RESULT_OK = 1
};

typedef struct cov_payload_header_v1 {
	uint32_t protocol_version;
	uint32_t struct_size;
} cov_payload_header_v1;

typedef struct cov_profile_v1 {
	cov_payload_header_v1 header;
	uint32_t engine_slot;
	uint32_t client_generation;
	uint8_t static_asset_set_id[32];
	uint8_t sensory_profile_id[32];
	uint32_t max_batch_bytes;
	uint32_t max_seen_entities;
	uint32_t max_static_anchors;
	uint32_t max_async_events;
} cov_profile_v1;

typedef struct cov_get_committed_v1 {
	cov_payload_header_v1 header;
	uint32_t engine_slot;
	uint32_t client_generation;
	uint64_t frame_seq;
	uint32_t dt_us;
	uint32_t output_capacity;
	uint32_t output_length;
	uint8_t batch[COV_MAX_BATCH_BYTES_V1];
} cov_get_committed_v1;

#endif
