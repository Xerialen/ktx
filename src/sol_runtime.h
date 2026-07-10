#ifndef SOL_RUNTIME_H
#define SOL_RUNTIME_H

struct gedict_s;

int Sol_IsClient(const struct gedict_s *client);
int Sol_ClientConnectedEvent(struct gedict_s *client);
int Sol_ClientEntersEvent(struct gedict_s *client);
void Sol_ClientDisconnectedEvent(struct gedict_s *client);
void Sol_CapturePostThink(struct gedict_s *client);
void Sol_StartFrame(void);

int Sol_CommandBypassesBotGates(const char *command);
void Sol_EvidenceBind_f(void);
void Sol_Add_f(void);
void Sol_EvidenceClose_f(void);
void Sol_RemoveAll(void);

#endif
