#ifndef SOL_RUNTIME_H
#define SOL_RUNTIME_H

#include <stddef.h>

#include "sol_actual_command.h"

struct gedict_s;

int Sol_IsClient(const struct gedict_s *client);
int Sol_ClientConnectedEvent(struct gedict_s *client);
int Sol_ClientEntersEvent(struct gedict_s *client);
void Sol_ClientDisconnectedEvent(struct gedict_s *client);
void Sol_ServerStartFrame(void);
void Sol_StartFrame(void);

int Sol_StockPendingBotName(int skill_level, const char *team,
	char *output, size_t capacity);
int Sol_StockBotInitialized(int entity, int skill_level, const char *team,
	const char *name);
sol_actual_command_route_v1 Sol_ActualCommandLookup(uint32_t engine_slot,
	uint32_t *client_generation);
void Sol_ActualCommandFailStop(void);

int Sol_CommandBypassesBotGates(const char *command);
void Sol_EvidenceBind_f(void);
void Sol_Add_f(void);
void Sol_EvidenceClose_f(void);
int Sol_RemoveAll(void);

#endif
