#ifndef SOL_WIRE_H
#define SOL_WIRE_H

#include <stddef.h>
#include <stdint.h>

enum {
	SOL_WIRE_MAX_BATCH_V1 = 65535,
	SOL_WIRE_ACTION_BASE_V1 = 33,
	SOL_WIRE_MAX_TEAMSAY_V1 = 80,
	SOL_WIRE_ACTION_MAX_V1 = 115,
	SOL_SOUND_SEMANTIC_MAX_V1 = 26
};

typedef enum sol_wire_status_v1 {
	SOL_WIRE_OK = 1,
	SOL_WIRE_INVALID = 0
} sol_wire_status_v1;

typedef enum sol_weapon_select_v1 {
	SOL_WEAPON_KEEP = 0,
	SOL_WEAPON_AXE = 1,
	SOL_WEAPON_SG = 2,
	SOL_WEAPON_SSG = 3,
	SOL_WEAPON_NG = 4,
	SOL_WEAPON_SNG = 5,
	SOL_WEAPON_GL = 6,
	SOL_WEAPON_RL = 7,
	SOL_WEAPON_LG = 8
} sol_weapon_select_v1;

typedef struct sol_action_response_v1 {
	uint64_t frame_seq;
	float view_angles[3];
	int16_t forwardmove;
	int16_t sidemove;
	int16_t upmove;
	uint8_t buttons;
	uint8_t weapon_select;
	uint8_t teamsay_present;
	uint16_t teamsay_length;
	uint8_t teamsay[SOL_WIRE_MAX_TEAMSAY_V1];
} sol_action_response_v1;

int sol_wire_observation_is_canonical_v1(const uint8_t *wire, size_t length);

int sol_wire_encode_action_v1(const sol_action_response_v1 *action,
	uint8_t *output, size_t capacity, size_t *output_length);
int sol_wire_decode_action_v1(const uint8_t *wire, size_t length,
	sol_action_response_v1 *output);

#endif
