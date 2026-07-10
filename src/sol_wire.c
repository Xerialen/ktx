#include "sol_wire.h"

#include <math.h>
#include <string.h>

#define SOL_CHANNEL_SELF_V1 0u
#define SOL_CHANNEL_SIGHT_V1 1u
#define SOL_CHANNEL_SOUND_V1 2u
#define SOL_CHANNEL_TEAMSAY_V1 3u
#define SOL_CHANNEL_KILLFEED_V1 4u

#define SOL_KIND_SELF_V1 UINT16_C(0x0001)
#define SOL_KIND_DAMAGE_V1 UINT16_C(0x0002)
#define SOL_KIND_SIGHT_V1 UINT16_C(0x0101)
#define SOL_KIND_SOUND_V1 UINT16_C(0x0201)
#define SOL_KIND_TEAMSAY_V1 UINT16_C(0x0301)
#define SOL_KIND_KILLFEED_V1 UINT16_C(0x0401)

#define SOL_MAX_EVENTS_V1 131u
#define SOL_MAX_SEEN_V1 96u
#define SOL_MAX_ANCHORS_V1 16u
#define SOL_MAX_PRINT_V1 1023u

typedef struct sol_cursor_v1 {
	const uint8_t *wire;
	size_t length;
	size_t offset;
	int valid;
} sol_cursor_v1;

static int cursor_take(sol_cursor_v1 *cursor, size_t length)
{
	if (!cursor->valid || cursor->offset > cursor->length ||
		length > cursor->length - cursor->offset) {
		cursor->valid = 0;
		return 0;
	}
	return 1;
}

static uint8_t cursor_u8(sol_cursor_v1 *cursor)
{
	if (!cursor_take(cursor, 1u))
		return 0;
	return cursor->wire[cursor->offset++];
}

static uint16_t cursor_u16(sol_cursor_v1 *cursor)
{
	uint16_t value;

	if (!cursor_take(cursor, 2u))
		return 0;
	value = (uint16_t)cursor->wire[cursor->offset] |
		((uint16_t)cursor->wire[cursor->offset + 1u] << 8);
	cursor->offset += 2u;
	return value;
}

static uint32_t cursor_u32(sol_cursor_v1 *cursor)
{
	uint32_t value;

	if (!cursor_take(cursor, 4u))
		return 0;
	value = (uint32_t)cursor->wire[cursor->offset] |
		((uint32_t)cursor->wire[cursor->offset + 1u] << 8) |
		((uint32_t)cursor->wire[cursor->offset + 2u] << 16) |
		((uint32_t)cursor->wire[cursor->offset + 3u] << 24);
	cursor->offset += 4u;
	return value;
}

static uint64_t cursor_u64(sol_cursor_v1 *cursor)
{
	uint64_t low = cursor_u32(cursor);
	uint64_t high = cursor_u32(cursor);
	return low | (high << 32);
}

static int identity_nonzero(const uint8_t *identity)
{
	uint8_t any = 0;
	size_t i;

	for (i = 0; i < 32u; ++i)
		any |= identity[i];
	return any != 0;
}

static int cursor_bool(sol_cursor_v1 *cursor)
{
	return cursor_u8(cursor) <= 1u && cursor->valid;
}

static int parse_self(sol_cursor_v1 *cursor)
{
	uint8_t alive = cursor_u8(cursor);
	uint8_t on_ground = cursor_u8(cursor);
	uint8_t water_level = cursor_u8(cursor);
	uint8_t water_type = cursor_u8(cursor);
	uint8_t movement_mode = cursor_u8(cursor);
	unsigned i;

	if (alive > 1u || on_ground > 1u || water_level > 3u ||
		water_type > 3u || movement_mode > 2u)
		return 0;
	for (i = 0; i < 10u; ++i)
		(void)cursor_u16(cursor);
	for (i = 0; i < 9u; ++i)
		(void)cursor_u32(cursor);
	(void)cursor_u16(cursor);
	(void)cursor_u8(cursor);
	(void)cursor_u16(cursor);
	if (cursor_u8(cursor) > 13u || cursor_u8(cursor) > 13u)
		return 0;
	return cursor->valid;
}

static int parse_seen(sol_cursor_v1 *cursor, uint16_t expected_token)
{
	uint16_t token = cursor_u16(cursor);
	uint8_t kind = cursor_u8(cursor);
	uint8_t weapon_present;
	unsigned i;

	if (token != expected_token || kind > 3u)
		return 0;
	(void)cursor_u16(cursor);
	for (i = 0; i < 3u; ++i)
		(void)cursor_u16(cursor);
	(void)cursor_u8(cursor);
	(void)cursor_u8(cursor);
	if (kind == 0u) {
		for (i = 0; i < 3u; ++i)
			(void)cursor_u16(cursor);
		(void)cursor_u16(cursor);
		if (cursor_u8(cursor) > 13u || cursor_u8(cursor) > 13u)
			return 0;
		if (!cursor_bool(cursor) || !cursor_bool(cursor) || !cursor_bool(cursor) ||
			cursor_u8(cursor) > 2u)
			return 0;
		for (i = 0; i < 3u; ++i)
			(void)cursor_u16(cursor);
	}
	else {
		for (i = 0; i < 3u; ++i)
			(void)cursor_u8(cursor);
	}
	weapon_present = cursor_u8(cursor);
	if (weapon_present > 1u || (weapon_present && kind != 0u))
		return 0;
	if (weapon_present)
		(void)cursor_u16(cursor);
	return cursor->valid;
}

static int parse_sight(sol_cursor_v1 *cursor)
{
	uint8_t saturated = cursor_u8(cursor);
	uint8_t ambiguous = cursor_u8(cursor);
	uint32_t seen_count = cursor_u32(cursor);
	uint32_t anchor_count, i;
	uint16_t previous_anchor = 0;

	if (saturated > 1u || ambiguous > 1u || (ambiguous && !saturated) ||
		seen_count > SOL_MAX_SEEN_V1)
		return 0;
	for (i = 0; i < seen_count; ++i) {
		if (!parse_seen(cursor, (uint16_t)i))
			return 0;
	}
	anchor_count = cursor_u32(cursor);
	if (anchor_count > SOL_MAX_ANCHORS_V1)
		return 0;
	for (i = 0; i < anchor_count; ++i) {
		uint16_t token = cursor_u16(cursor);
		uint8_t state = cursor_u8(cursor);
		if (state > 2u || (i && token <= previous_anchor))
			return 0;
		previous_anchor = token;
	}
	return cursor->valid;
}

static int parse_text(sol_cursor_v1 *cursor, size_t maximum)
{
	uint16_t length = cursor_u16(cursor);
	const uint8_t *text;

	if (!length || length > maximum || !cursor_take(cursor, length))
		return 0;
	text = cursor->wire + cursor->offset;
	if (memchr(text, '\0', length))
		return 0;
	cursor->offset += length;
	return 1;
}

static int parse_event_payload(sol_cursor_v1 *cursor, uint8_t channel,
	uint16_t kind)
{
	unsigned i;

	switch (kind) {
	case SOL_KIND_SELF_V1:
		return channel == SOL_CHANNEL_SELF_V1 && parse_self(cursor);
	case SOL_KIND_DAMAGE_V1:
		if (channel != SOL_CHANNEL_SELF_V1)
			return 0;
		(void)cursor_u8(cursor);
		(void)cursor_u8(cursor);
		for (i = 0; i < 3u; ++i)
			(void)cursor_u16(cursor);
		return cursor->valid;
	case SOL_KIND_SIGHT_V1:
		return channel == SOL_CHANNEL_SIGHT_V1 && parse_sight(cursor);
	case SOL_KIND_SOUND_V1: {
		uint8_t semantic = cursor_u8(cursor);
		uint16_t left = cursor_u16(cursor);
		uint16_t right = cursor_u16(cursor);
		uint8_t self_originated = cursor_u8(cursor);
		return channel == SOL_CHANNEL_SOUND_V1 && semantic <= 7u &&
			(left || right) && self_originated <= 1u && cursor->valid;
	}
	case SOL_KIND_TEAMSAY_V1:
		return channel == SOL_CHANNEL_TEAMSAY_V1 &&
			parse_text(cursor, SOL_MAX_PRINT_V1);
	case SOL_KIND_KILLFEED_V1:
		return channel == SOL_CHANNEL_KILLFEED_V1 &&
			parse_text(cursor, SOL_MAX_PRINT_V1);
	default:
		return 0;
	}
}

static int event_not_before(uint8_t previous_channel, uint16_t previous_kind,
	const uint8_t *previous_payload, size_t previous_length, uint8_t channel,
	uint16_t kind, const uint8_t *payload, size_t length)
{
	size_t shared;
	int compared;

	if (previous_channel != channel)
		return previous_channel < channel;
	if (previous_kind != kind)
		return previous_kind < kind;
	shared = previous_length < length ? previous_length : length;
	compared = memcmp(previous_payload, payload, shared);
	return compared < 0 || (compared == 0 && previous_length <= length);
}

int sol_wire_observation_is_canonical_v1(const uint8_t *wire, size_t length)
{
	sol_cursor_v1 cursor = {wire, length, 0, 1};
	uint8_t first_tag, last_tag, previous_channel = 0;
	uint16_t previous_kind = 0;
	uint64_t first_seq, last_seq;
	uint32_t count, i;
	const uint8_t *previous_payload = NULL;
	size_t previous_length = 0;
	unsigned self_count = 0, damage_count = 0, sight_count = 0;

	if (!wire || length < 102u || memcmp(wire, "SOB1", 4u))
		return SOL_WIRE_INVALID;
	cursor.offset = 4u;
	(void)cursor_u64(&cursor);
	if (!cursor_u32(&cursor) || !cursor_take(&cursor, 64u) ||
		!identity_nonzero(cursor.wire + cursor.offset) ||
		!identity_nonzero(cursor.wire + cursor.offset + 32u))
		return SOL_WIRE_INVALID;
	cursor.offset += 64u;
	first_tag = cursor_u8(&cursor);
	first_seq = cursor_u64(&cursor);
	last_tag = cursor_u8(&cursor);
	last_seq = cursor_u64(&cursor);
	count = cursor_u32(&cursor);
	if (!cursor.valid || first_tag != 1u || last_tag != 1u || count < 2u ||
		count > SOL_MAX_EVENTS_V1 || last_seq < first_seq ||
		last_seq - first_seq != (uint64_t)count - 1u)
		return SOL_WIRE_INVALID;
	for (i = 0; i < count; ++i) {
		uint64_t sequence = cursor_u64(&cursor);
		uint8_t channel = cursor_u8(&cursor);
		uint16_t kind = cursor_u16(&cursor);
		const uint8_t *payload = cursor.wire + cursor.offset;
		size_t payload_length;

		if (!cursor.valid || sequence != first_seq + i ||
			!parse_event_payload(&cursor, channel, kind))
			return SOL_WIRE_INVALID;
		payload_length = (size_t)(cursor.wire + cursor.offset - payload);
		if (i && !event_not_before(previous_channel, previous_kind,
			previous_payload, previous_length, channel, kind, payload,
			payload_length))
			return SOL_WIRE_INVALID;
		previous_channel = channel;
		previous_kind = kind;
		previous_payload = payload;
		previous_length = payload_length;
		self_count += kind == SOL_KIND_SELF_V1;
		damage_count += kind == SOL_KIND_DAMAGE_V1;
		sight_count += kind == SOL_KIND_SIGHT_V1;
	}
	return cursor.valid && cursor.offset == length && self_count == 1u &&
		damage_count <= 1u && sight_count == 1u ? SOL_WIRE_OK : SOL_WIRE_INVALID;
}

static void write_u16(uint8_t *output, uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *output, uint32_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
	output[2] = (uint8_t)(value >> 16);
	output[3] = (uint8_t)(value >> 24);
}

static void write_u64(uint8_t *output, uint64_t value)
{
	write_u32(output, (uint32_t)value);
	write_u32(output + 4u, (uint32_t)(value >> 32));
}

static int canonical_view(const float view[3])
{
	return isfinite(view[0]) && isfinite(view[1]) && isfinite(view[2]) &&
		view[0] >= -90.0f && view[0] <= 90.0f &&
		view[1] >= -180.0f && view[1] < 180.0f &&
		view[2] >= -180.0f && view[2] < 180.0f;
}

static int reserved_first_token(const uint8_t *text, size_t length)
{
	static const char *reserved[] = {"s-p", "s-l", "s-r", "s-t", "s-m"};
	size_t start = 0, end, i;

	while (start < length && text[start] == ' ')
		++start;
	end = start;
	while (end < length && text[end] != ' ')
		++end;
	for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
		if (end - start == 3u && !memcmp(text + start, reserved[i], 3u))
			return 1;
	}
	return 0;
}

static int teamsay_valid(const uint8_t *text, size_t length)
{
	size_t i;
	int has_nonspace = 0;

	if (!text || !length || length > SOL_WIRE_MAX_TEAMSAY_V1)
		return 0;
	for (i = 0; i < length; ++i) {
		if (text[i] < 0x20u || text[i] > 0x7eu || text[i] == '"' ||
			text[i] == '\\' || text[i] == ';' || text[i] == '$')
			return 0;
		has_nonspace |= text[i] != ' ';
	}
	return has_nonspace && !reserved_first_token(text, length);
}

static int action_valid(const sol_action_response_v1 *action)
{
	if (!action || !canonical_view(action->view_angles) ||
		(action->buttons & UINT8_C(0xf0)) || action->weapon_select > SOL_WEAPON_LG ||
		action->teamsay_present > 1u)
		return 0;
	if (!action->teamsay_present)
		return action->teamsay_length == 0u;
	return action->teamsay_length <= SOL_WIRE_MAX_TEAMSAY_V1 &&
		teamsay_valid(action->teamsay, action->teamsay_length);
}

static void write_f32_canonical(uint8_t *output, float value)
{
	uint32_t bits;

	if (value == 0.0f)
		value = 0.0f;
	memcpy(&bits, &value, sizeof(bits));
	write_u32(output, bits);
}

int sol_wire_encode_action_v1(const sol_action_response_v1 *action,
	uint8_t *output, size_t capacity, size_t *output_length)
{
	size_t required, offset = 0;
	unsigned i;

	if (output_length)
		*output_length = 0;
	if (!output_length || !output || !action_valid(action))
		return SOL_WIRE_INVALID;
	required = SOL_WIRE_ACTION_BASE_V1 +
		(action->teamsay_present ? 2u + action->teamsay_length : 0u);
	if (capacity < required)
		return SOL_WIRE_INVALID;
	memcpy(output + offset, "SAC1", 4u);
	offset += 4u;
	write_u64(output + offset, action->frame_seq);
	offset += 8u;
	for (i = 0; i < 3u; ++i) {
		write_f32_canonical(output + offset, action->view_angles[i]);
		offset += 4u;
	}
	write_u16(output + offset, (uint16_t)action->forwardmove);
	offset += 2u;
	write_u16(output + offset, (uint16_t)action->sidemove);
	offset += 2u;
	write_u16(output + offset, (uint16_t)action->upmove);
	offset += 2u;
	output[offset++] = action->buttons;
	output[offset++] = action->weapon_select;
	output[offset++] = action->teamsay_present;
	if (action->teamsay_present) {
		write_u16(output + offset, action->teamsay_length);
		offset += 2u;
		memcpy(output + offset, action->teamsay, action->teamsay_length);
		offset += action->teamsay_length;
	}
	*output_length = offset;
	return offset == required ? SOL_WIRE_OK : SOL_WIRE_INVALID;
}

static int cursor_f32(sol_cursor_v1 *cursor, float *output)
{
	uint32_t bits = cursor_u32(cursor);

	if (!cursor->valid || (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) ||
		bits == UINT32_C(0x80000000))
		return 0;
	memcpy(output, &bits, sizeof(bits));
	return 1;
}

int sol_wire_decode_action_v1(const uint8_t *wire, size_t length,
	sol_action_response_v1 *output)
{
	sol_cursor_v1 cursor = {wire, length, 0, 1};
	sol_action_response_v1 decoded = {0};
	unsigned i;

	if (output)
		memset(output, 0, sizeof(*output));
	if (!wire || !output || length < SOL_WIRE_ACTION_BASE_V1 ||
		memcmp(wire, "SAC1", 4u))
		return SOL_WIRE_INVALID;
	cursor.offset = 4u;
	decoded.frame_seq = cursor_u64(&cursor);
	for (i = 0; i < 3u; ++i) {
		if (!cursor_f32(&cursor, &decoded.view_angles[i]))
			return SOL_WIRE_INVALID;
	}
	decoded.forwardmove = (int16_t)cursor_u16(&cursor);
	decoded.sidemove = (int16_t)cursor_u16(&cursor);
	decoded.upmove = (int16_t)cursor_u16(&cursor);
	decoded.buttons = cursor_u8(&cursor);
	decoded.weapon_select = cursor_u8(&cursor);
	decoded.teamsay_present = cursor_u8(&cursor);
	if (decoded.teamsay_present == 1u) {
		decoded.teamsay_length = cursor_u16(&cursor);
		if (decoded.teamsay_length > SOL_WIRE_MAX_TEAMSAY_V1 ||
			!cursor_take(&cursor, decoded.teamsay_length))
			return SOL_WIRE_INVALID;
		memcpy(decoded.teamsay, cursor.wire + cursor.offset,
			decoded.teamsay_length);
		cursor.offset += decoded.teamsay_length;
	}
	else if (decoded.teamsay_present != 0u) {
		return SOL_WIRE_INVALID;
	}
	if (!cursor.valid || cursor.offset != length || !action_valid(&decoded))
		return SOL_WIRE_INVALID;
	*output = decoded;
	return SOL_WIRE_OK;
}
