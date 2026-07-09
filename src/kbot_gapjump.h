#ifndef KBOT_GAPJUMP_H
#define KBOT_GAPJUMP_H
#ifdef BOT_SUPPORT

// Public interface of the gap-jump subsystem, extracted from kbot_main.c (#72).
// Types (gedict_t, qbool, vec3_t) come from g_local.h; include it first.
qbool KBot_GapjumpFrame(gedict_t *self, qbool *jumping, qbool *firing, int *impulse, vec3_t direction);
float KBot_GJ_RouteShim(gedict_t *self, gedict_t *goal_entity, float goal_time);
void  KBot_GJ_OfferSngMega(gedict_t *self);

#endif // BOT_SUPPORT
#endif // KBOT_GAPJUMP_H
