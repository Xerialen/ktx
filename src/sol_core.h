#ifndef SOL_CORE_H
#define SOL_CORE_H

#include <stddef.h>
#include <stdint.h>

enum
{
	SOL_CORE_INIT_V1_SIZE = 116,
	SOL_CORE_OBSERVATION_V1_SIZE = 124,
	SOL_CORE_ACTION_V1_SIZE = 33
};

/*
 * Disposable tracer wire v1. All integers are little-endian. Every f32 is
 * finite IEEE-754 binary32; negative zero is noncanonical.
 *
 * init[116] = "SLI1" | asset_sha256[32] | sensory_sha256[32]
 *             | goal_sha256[32] | goal_xyz_f32[3] | horizontal_radius_f32
 * observation[124] = "SLO1" | frame_seq_u64 | dt_us_u32 (>0)
 *             | asset_sha256[32] | sensory_sha256[32]
 *             | alive_u8(0..1) | on_ground_u8(0..1) | water_level_u8(0..3)
 *             | movement_mode_u8(0=NORMAL,1=DEAD,2=LOCKED)
 *             | origin_f32[3] | velocity_f32[3] | view_degrees_f32[3]
 *             | sight_count_u32 (must be zero in this tracer)
 * action[33] = "SLA1" | frame_seq_u64 | view_degrees_f32[3]
 *             | forward_i16 | side_i16 | up_i16 | buttons_u8
 *             | weapon_u8 (0=KEEP) | teamsay_present_u8 (must be zero)
 * The three identities are nonzero; observation asset/sensory identities must
 * equal init. Radius is positive. View pitch is in [-90,90], while yaw and
 * roll use [-180,180). Active yaw is sign-quantized toward the goal axis or
 * quadrant in 45-degree increments. Arrival compares the exact squared values
 * represented by the binary32 coordinate and radius bytes; ambient floating-
 * point state does not participate. The core deliberately ignores goal z for
 * this horizontal movement tracer but copies it into the private immutable goal.
 */

typedef struct sol_core_v1 sol_core_v1;

typedef enum sol_core_status_v1
{
	SOL_CORE_OK = 0,
	SOL_CORE_NEUTRAL = 1,
	SOL_CORE_BAD_ARGUMENT = 2,
	SOL_CORE_BAD_OBSERVATION = 3,
	SOL_CORE_OUTPUT_TOO_SMALL = 4
} sol_core_status_v1;

/* Copies a valid init wire into one private instance. NULL means invalid init
 * or allocation failure; the caller retains ownership of init_wire. */
sol_core_v1 *sol_core_create_v1(const uint8_t *init_wire, size_t init_len);

/*
 * The first accepted observation has frame_seq 0; later accepted frames are
 * contiguous. SOL_CORE_OK and SOL_CORE_NEUTRAL both return one complete
 * 33-byte action and advance the private sequence. With a nonnull action_len,
 * every error sets it to zero; no error writes action bytes or advances
 * sequence/state. A neutral action preserves the observation's valid view
 * and clears movement, buttons, weapon selection, and teamsay.
 */
sol_core_status_v1 sol_core_step_v1(sol_core_v1 *core,
									 const uint8_t *observation_wire, size_t observation_len,
									 uint8_t *action_wire, size_t action_capacity,
									 size_t *action_len);

void sol_core_destroy_v1(sol_core_v1 *core);

#endif
