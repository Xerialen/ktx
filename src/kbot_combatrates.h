/*
 kbot_combatrates.h -- Milton-derived combat-decision rates (see .c banner).
 Master cvar k_kbot_combatrates: 0 off (bit-identical), 1 on for kbots.
 */
#ifndef KTX_KBOT_COMBATRATES_H
#define KTX_KBOT_COMBATRATES_H

#ifdef BOT_SUPPORT

// Evade-arming probability for BotEvadeLogic: vanilla constant for stock
// bots / cvar 0, Milton's empirical disengage rate for active kbots.
float KBot_CR_EvadeChance(gedict_t *self, float vanilla_chance);

// Pain-switch decision for BotDamageInflictedEvent: vanilla = omniscient
// firepower comparator; active kbots = Milton's P(turn-and-fight) roll.
qbool KBot_CR_PainSwitch(gedict_t *targ, gedict_t *attacker);

#endif // BOT_SUPPORT

#endif // KTX_KBOT_COMBATRATES_H
