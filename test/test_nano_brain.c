/*
 test_nano_brain.c -- standalone unit tests for the S2a nano brain pure helpers.

 Compile (from repo root):
   gcc -O2 -DNANO_SUPPORT -I test/shim -I include -I src \
       test/test_nano_brain.c -lm -o test/test_nano_brain
 Run:
   ./test/test_nano_brain
*/
#define NANO_SUPPORT 1
#include "g_local.h"   // shim
#include "nano.h"
#include "nano_brain.h"

#include <stdio.h>
#include <math.h>

static int failures = 0;

static void check_nearly_equal(const char *name, float got, float want, float tol)
{
	if (fabsf(got - want) > tol)
	{
		fprintf(stderr, "FAIL %s: got %.4f want %.4f\n", name, got, want);
		failures++;
	}
}

static void test_wrap180(void)
{
	check_nearly_equal("wrap 0", Nano_Wrap180(0.0f), 0.0f, 0.001f);
	check_nearly_equal("wrap 180", Nano_Wrap180(180.0f), 180.0f, 0.001f);
	check_nearly_equal("wrap -180", Nano_Wrap180(-180.0f), 180.0f, 0.001f);
	check_nearly_equal("wrap 360", Nano_Wrap180(360.0f), 0.0f, 0.001f);
	check_nearly_equal("wrap 540", Nano_Wrap180(540.0f), 180.0f, 0.001f);
	check_nearly_equal("wrap -360", Nano_Wrap180(-360.0f), 0.0f, 0.001f);
	check_nearly_equal("wrap 200", Nano_Wrap180(200.0f), -160.0f, 0.001f);
	check_nearly_equal("wrap -200", Nano_Wrap180(-200.0f), 160.0f, 0.001f);
	printf("wrap180: %s\n", failures ? "FAIL" : "ok");
}

static void test_aim_spring(void)
{
	nano_bot_t bot;
	vec3_t look;
	float omega = 12.0f; // skill 3
	float dt = 0.014f;
	int steps, i;

	memset(&bot, 0, sizeof(bot));
	bot.air_leg = -1;
	bot.goal_cell = -1;
	bot.goal_ent = -1;
	VectorSet(bot.aim, 0.0f, 0.0f, 0.0f);
	VectorClear(bot.aim_vel);

	// Target yaw = 90; spring should converge.
	VectorSet(look, 0.0f, 90.0f, 0.0f);
	steps = (int)(0.5f / dt); // half a second
	for (i = 0; i < steps; i++)
	{
		Nano_AimSpringStep(&bot, look, omega, dt);
	}

	check_nearly_equal("aim yaw converges", bot.aim[1], 90.0f, 5.0f);
	check_nearly_equal("aim pitch stays 0", bot.aim[0], 0.0f, 0.1f);
	if (fabsf(bot.aim_vel[1]) > 50.0f)
	{
		fprintf(stderr, "FAIL aim yaw velocity too high: %.4f\n", bot.aim_vel[1]);
		failures++;
	}

	// Overshoot/wrap test: target -170, start 170.
	VectorSet(bot.aim, 0.0f, 170.0f, 0.0f);
	VectorClear(bot.aim_vel);
	VectorSet(look, 0.0f, -170.0f, 0.0f);
	for (i = 0; i < steps; i++)
	{
		Nano_AimSpringStep(&bot, look, omega, dt);
	}
	check_nearly_equal("aim yaw wrap converges", bot.aim[1], -170.0f, 5.0f);

	printf("aim_spring: %s\n", failures ? "FAIL" : "ok");
}

int main(void)
{
	test_wrap180();
	test_aim_spring();
	return failures ? 1 : 0;
}
