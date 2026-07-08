/*
 nano_main.c -- nano-bots brain skeleton (rtx port, S0 scaffolding).

 Proves the brain seam the same way kbot_main.c did at WP2.1: Nano_MarkBot()
 flags a bot and stamps its identity into run evidence; Nano_Frame() is the
 per-frame entry point. S0 returns false (pure delegation to stock frogbot),
 so a nano-flagged bot plays identically to a frogbot -- the real rtx-port
 brain (navmesh, perception, combat, movement) is filled in by later stages.
*/
#ifdef NANO_SUPPORT

#include "g_local.h"
#include "nano.h"

// >= any mvdsv maxclients the bench uses; bound-checked at every use so an
// out-of-range edict number can never index past the array.
#define NANO_MAX_SLOTS 64

// Per-bot nano flag, indexed by edict number. Lives here (not in gedict_t) so
// the shared bot structs are unchanged and NANO_SUPPORT=OFF stays
// byte-identical to the frogbot baseline.
static int nano_state[NANO_MAX_SLOTS];

// Effective identity stamp: NANO_VERSION + k_nano_version_suffix (set by the
// bench cfg alongside --candidate-version, so the observed-identity gate
// matches stamp == roster.candidate_version).
static void Nano_StampedVersion(char *out, int out_size)
{
	char suffix[32];

	trap_cvar_string("k_nano_version_suffix", suffix, sizeof(suffix));
	snprintf(out, out_size, "%s%s", NANO_VERSION, suffix);
}

qbool Nano_IsMarked(gedict_t *self)
{
	int ent;

	if (!self || !self->isBot)
	{
		return false;
	}

	ent = NUM_FOR_EDICT(self);
	if (ent < 0 || ent >= NANO_MAX_SLOTS)
	{
		return false;
	}

	return nano_state[ent] != NANO_STATE_OFF;
}

void Nano_MarkBot(gedict_t *bot)
{
	char newname[CLIENT_NAME_LEN];
	char stamped[64];
	int entity;

	if (!bot || !bot->isBot)
	{
		return;
	}

	entity = NUM_FOR_EDICT(bot);
	if (entity < 0 || entity >= NANO_MAX_SLOTS)
	{
		return;
	}

	nano_state[entity] = NANO_STATE_MARKED;
	Nano_StampedVersion(stamped, sizeof(stamped));

	// Identity marker: the "nano" userinfo key is the identity proof (the
	// ledger maps stamps onto roster names), mirroring kbot's "kbot" key.
	trap_SetBotUserInfo(entity, "nano", stamped, 0);

	// Name: "nb:" prefix on the existing bot name unless the bench set one.
	infokey(bot, "name", bot->netname, CLIENT_NAME_LEN);
	if (strncmp(bot->netname, "nb:", 3))
	{
		snprintf(newname, sizeof(newname), "nb:%s", bot->netname);
		trap_SetBotUserInfo(entity, "name", newname, 0);
		infokey(bot, "name", bot->netname, CLIENT_NAME_LEN);
	}

	// Advertise the brain version via serverinfo once, on first nano-bot.
	{
		char infobuf[64];

		infokey(world, "nano_version", infobuf, sizeof(infobuf));
		if (strnull(infobuf))
		{
			localcmd("serverinfo nano_version %s\n", stamped);
		}
	}

	G_cprint("[nano] slot=%d name=%s version=%s\n", entity, bot->netname, stamped);
}

void Nano_ClearMark(gedict_t *bot)
{
	int entity;

	if (!bot)
	{
		return;
	}

	entity = NUM_FOR_EDICT(bot);
	if (entity < 0 || entity >= NANO_MAX_SLOTS)
	{
		return;
	}

	nano_state[entity] = NANO_STATE_OFF;
}

// Per-frame brain entry point. S0: log identity once (MARKED -> ACTIVE), then
// return false so BotsThinkTime() runs the stock frogbot logic unchanged.
qbool Nano_Frame(gedict_t *self)
{
	int entity;

	entity = NUM_FOR_EDICT(self);
	if (entity < 0 || entity >= NANO_MAX_SLOTS)
	{
		return false;
	}

	if (nano_state[entity] == NANO_STATE_MARKED)
	{
		char stamped[64];

		Nano_StampedVersion(stamped, sizeof(stamped));
		nano_state[entity] = NANO_STATE_ACTIVE;
		G_cprint("[nano] frame active slot=%d name=%s version=%s time=%f\n",
					entity, self->netname, stamped, g_globalvars.time);
	}

	return false; // S0: not handled -- fall through to stock frogbot think
}

#endif // NANO_SUPPORT
