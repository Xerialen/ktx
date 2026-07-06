/*
 hm_parity.c -- offline parity harness for the hm.c teamsay classifier.

 Compiles src/hm.c into this translation unit (the HMP_* parser is static)
 and stubs the engine surface, so HMP_Tokenize + HMP_Classify can run
 against the mm2 corpus without a server. Reads "player\ttext" lines on
 stdin, prints the category index (see cat_names in HMode_ParseTeamsay)
 one per line on stdout.

 Build (linux, from the repo root):
   gcc -O2 -DBOT_SUPPORT=1 -Iinclude -o tools/hm_parity/hm_parity \
       tools/hm_parity/hm_parity.c

 Driven by komodobots-mm2humanmode
 experiments/mm2_comms/scripts/hm_parity_run.py, which diffs the output
 against the Python reference (analyze2_parser.py) per corpus message.

 Only the tokenize+classify path executes here: HMP_Apply and the
 perception/emit sections are compiled but never called, so their engine
 dependencies are satisfied by no-op stubs below.
 */

#include "../../src/hm.c"

#include <stdarg.h>
#include <stdio.h>

// ---- engine globals (never read on the classify path) ----

gedict_t g_edicts[64];
globalvars_t g_globalvars;
gedict_t *world = g_edicts;
gedict_t *self = g_edicts;
gedict_t *other = g_edicts;
int timelimit, fraglimit, teamplay, deathmatch, framecount, coop, skill;
float match_in_progress;
float match_start_time;

// ---- string utils (real semantics: these DO run on the classify path) ----

int streq(const char *s1, const char *s2)
{
	return !strcmp(s1, s2);
}

int strnull(const char *s)
{
	return !s || !s[0];
}

size_t strlcpy(char *dst, const char *src, size_t siz)
{
	size_t len = strlen(src);

	if (siz)
	{
		size_t n = (len >= siz) ? siz - 1 : len;

		memcpy(dst, src, n);
		dst[n] = '\0';
	}

	return len;
}

size_t strlcat(char *dst, const char *src, size_t siz)
{
	size_t dl = strlen(dst);

	return dl + strlcpy(dst + dl, src, (siz > dl) ? siz - dl : 0);
}

char* va(char *format, ...)
{
	static char buf[4][1024];
	static int idx;
	va_list ap;

	idx = (idx + 1) & 3;
	va_start(ap, format);
	vsnprintf(buf[idx], sizeof(buf[idx]), format, ap);
	va_end(ap);

	return buf[idx];
}

float min(float a, float b)
{
	return (a < b) ? a : b;
}

float max(float a, float b)
{
	return (a > b) ? a : b;
}

float bound(float a, float b, float c)
{
	return (b < a) ? a : (b > c) ? c : b;
}

// ---- engine surface stubs (classify path never reaches these, except
//      ezinfokey/trap_cvar_string which must return empty strings) ----

char* ezinfokey(gedict_t *ed, char *key)
{
	return "";
}

void trap_cvar_string(const char *var, char *buffer, intptr_t bufsize)
{
	if (bufsize > 0)
	{
		buffer[0] = '\0';
	}
}

float cvar(const char *var)
{
	return 0;
}

void G_cprint(const char *fmt, ...)
{
}

void G_sprint(gedict_t *ed, int level, const char *fmt, ...)
{
}

char* redtext(char *format)
{
	return format;
}

float g_random(void)
{
	return 0.5f;
}

char* getteam(gedict_t *ed)
{
	return "";
}

int NUM_FOR_EDICT(gedict_t *e)
{
	return (int)(e - g_edicts);
}

qbool ISLIVE(gedict_t *e)
{
	return false;
}

gedict_t* find(gedict_t *start, int fieldoff, char *str)
{
	return NULL;
}

qbool LocationCoordsByName(const char *name, vec3_t out)
{
	return false;
}

qbool TeamplayMessageByName(gedict_t *client, const char *message)
{
	return false;
}

float VectorDistance(vec3_t v1, vec3_t v2)
{
	return 0;
}

qbool VisibleEntity(gedict_t *ent)
{
	return false;
}

qbool SameTeam(gedict_t *p1, gedict_t *p2)
{
	return false;
}

char* LocationName(float x, float y, float z)
{
	return "";
}

void TeamplayMM2Raw(gedict_t *client, char *text)
{
}

void visible_to(gedict_t *viewer, gedict_t *first, int len, byte *visible)
{
	memset(visible, 0, len);
}

float vlen(vec3_t v)
{
	return 0;
}

void traceline(float v1_x, float v1_y, float v1_z, float v2_x, float v2_y, float v2_z, int nomonst,
			   gedict_t *forent)
{
}

intptr_t trap_CmdArgc(void)
{
	return 0;
}

void trap_CmdArgv(intptr_t arg, char *valbuff, intptr_t sizebuff)
{
	if (sizebuff > 0)
	{
		valbuff[0] = '\0';
	}
}

intptr_t trap_SetBotUserInfo(intptr_t edn, const char *varname, const char *value, intptr_t flags)
{
	return 0;
}

// ---- driver ----

int main(void)
{
	char line[2048];
	static char player[256];

	while (fgets(line, sizeof(line), stdin))
	{
		char *tab = strchr(line, '\t');
		char *text, *nl;
		gedict_t sender;
		hmp_tok_t toks[HMP_MAX_TOKENS];
		int n, cat;

		if (!tab)
		{
			printf("-1\n");
			continue;
		}

		*tab = '\0';
		text = tab + 1;
		nl = strchr(text, '\n');

		if (nl)
		{
			*nl = '\0';
		}

		strlcpy(player, line, sizeof(player));
		memset(&sender, 0, sizeof(sender));
		sender.netname = player;

		n = HMP_Tokenize(text, &sender, toks);
		cat = HMP_Classify(toks, n);
		printf("%d\n", cat);
	}

	return 0;
}
