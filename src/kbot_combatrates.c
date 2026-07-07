/*
 kbot_combatrates.c -- Milton-derived combat-decision rates (dm3 corpus)

 Replaces two frogbot combat RNG decisions with the reference player's
 empirical rates (15 tVS dm3 4on4 MVDs, 2506 combat contacts, 770 cold hits --
 komodobots experiments/combat_rates/milton_combat_rates_dm3.json):

   1. Evade arming (BotEvadeLogic): flat CHANCE_EVADE_DUEL/.NONDUEL (.08/.10)
      -> Milton's disengage rate CR_EVADE_CHANCE (.358). His commitment is
      flat across stack, so a single scalar is the honest replacement.
   2. Pain switch (BotDamageInflictedEvent): the omniscient comparator
      "look_object->fb.firepower < attacker->fb.firepower" (reads hidden
      loadout of two players; audit mini-leak) -> a CR_PAINSWITCH_CHANCE
      (.639) roll: Milton turns on a cold-hit attacker ~2/3 of the time.

 Master cvar k_kbot_combatrates: 0 off (default; bit-identical vanilla),
 1 = pain-switch roll only, 2 = + Milton evade rate. Non-kbots always
 vanilla. (Split after R12: the 3.6x evade rate made strong bots passive --
 -31 mean vs the -11.5 plateau -- while the pain-switch replacement is the
 audit-driven de-omniscience piece worth keeping if margin-neutral.)

 Expects g_local.h to have been included first (KTX header convention).
 */

#ifdef BOT_SUPPORT

#include "g_local.h"
#include "kbot.h"
#include "kbot_combatrates.h"
#include "kbot_combatrates_dm3.h"

static qbool CR_Active(gedict_t *self, int tier)
{
	return self->isBot && self->fb.kbot && (cvar("k_kbot_combatrates") >= tier);
}

// BotEvadeLogic consumer: the evade-arming probability. Vanilla constant for
// stock bots and cvar < 2.
float KBot_CR_EvadeChance(gedict_t *self, float vanilla_chance)
{
	if (!CR_Active(self, 2))
	{
		return vanilla_chance;
	}

	return CR_EVADE_CHANCE;
}

// BotDamageInflictedEvent consumer: should the hurt bot switch its look to
// the attacker? Vanilla decides by comparing hidden firepower (omniscient);
// Milton turns on a cold-hit attacker ~64% of the time regardless.
qbool KBot_CR_PainSwitch(gedict_t *targ, gedict_t *attacker)
{
	if (!CR_Active(targ, 1))
	{
		return targ->fb.look_object
				&& (targ->fb.look_object->fb.firepower < attacker->fb.firepower);
	}

	return targ->fb.look_object && (g_random() < CR_PAINSWITCH_CHANCE);
}

#endif // BOT_SUPPORT
