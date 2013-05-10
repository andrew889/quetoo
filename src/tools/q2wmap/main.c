/*
 * Copyright(c) 1997-2001 Id Software, Inc.
 * Copyright(c) 2002 The Quakeforge Project.
 * Copyright(c) 2006 Quake2World.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <SDL/SDL.h>
#include "q2wmap.h"

quake2world_t quake2world;

char map_name[MAX_OSPATH];
char bsp_name[MAX_OSPATH];
char outbase[MAX_OSPATH];

_Bool verbose;
_Bool debug;
_Bool legacy;

/* BSP */
extern _Bool noprune;
extern _Bool nodetail;
extern _Bool fulldetail;
extern _Bool onlyents;
extern _Bool nomerge;
extern _Bool nowater;
extern _Bool nofill;
extern _Bool nocsg;
extern _Bool noweld;
extern _Bool noshare;
extern _Bool nosubdivide;
extern _Bool notjunc;
extern _Bool noopt;
extern _Bool leaktest;
extern _Bool verboseentities;

extern int32_t block_xl, block_xh, block_yl, block_yh;
extern vec_t microvolume;
extern int32_t subdivide_size;

/* VIS */
extern _Bool fastvis;
extern _Bool nosort;

/* LIGHT */
extern _Bool extra_samples;
extern float brightness;
extern float saturation;
extern float contrast;
extern float surface_scale;
extern float entity_scale;

/*
 * @brief
 */
static void Debug(const char *msg) {

	if (!debug)
		return;

	printf("%s", msg);
}

static void Shutdown(const char *msg);

/*
 * @brief
 */
static void Error(err_t err, const char *msg) __attribute__((noreturn));
static void Error(err_t err __attribute__((unused)), const char *msg) {

	fprintf(stderr, "************ ERROR ************\n");
	fprintf(stderr, "%s", msg);

	Shutdown(NULL);

	exit(err);
}

/*
 * @brief
 */
static void Verbose(const char *msg) {

	if (!verbose)
		return;

	printf("%s", msg);
}

/*
 * @brief
 */
static void Warn(const char *msg) {
	fprintf(stderr, "WARNING: %s", msg);
}

/*
 * @brief Initializes subsystems q2wmap relies on.
 */
static void Init(void) {

	Z_Init();

	Fs_Init();

	Cmd_Init();

	Cvar_Init();

	Thread_Init();

	Sem_Init();
}

/*
 * @brief Shuts down subsystems.
 */
static void Shutdown(const char *msg __attribute__((unused))) {

	Sem_Shutdown();

	Thread_Shutdown();

	Cvar_Shutdown();

	Cmd_Shutdown();

	Fs_Shutdown();

	Z_Shutdown();
}

/*
 * @brief
 */
static void Check_BSP_Options(int32_t argc) {
	int32_t i;

	for (i = argc; i < Com_Argc(); i++) {
		if (!g_strcmp0(Com_Argv(i), "-noweld")) {
			Com_Verbose("noweld = true\n");
			noweld = true;
		} else if (!g_strcmp0(Com_Argv(i), "-nocsg")) {
			Com_Verbose("nocsg = true\n");
			nocsg = true;
		} else if (!g_strcmp0(Com_Argv(i), "-noshare")) {
			Com_Verbose("noshare = true\n");
			noshare = true;
		} else if (!g_strcmp0(Com_Argv(i), "-notjunc")) {
			Com_Verbose("notjunc = true\n");
			notjunc = true;
		} else if (!g_strcmp0(Com_Argv(i), "-nowater")) {
			Com_Verbose("nowater = true\n");
			nowater = true;
		} else if (!g_strcmp0(Com_Argv(i), "-noopt")) {
			Com_Verbose("noopt = true\n");
			noopt = true;
		} else if (!g_strcmp0(Com_Argv(i), "-noprune")) {
			Com_Verbose("noprune = true\n");
			noprune = true;
		} else if (!g_strcmp0(Com_Argv(i), "-nofill")) {
			Com_Verbose("nofill = true\n");
			nofill = true;
		} else if (!g_strcmp0(Com_Argv(i), "-nomerge")) {
			Com_Verbose("nomerge = true\n");
			nomerge = true;
		} else if (!g_strcmp0(Com_Argv(i), "-nosubdivide")) {
			Com_Verbose("nosubdivide = true\n");
			nosubdivide = true;
		} else if (!g_strcmp0(Com_Argv(i), "-nodetail")) {
			Com_Verbose("nodetail = true\n");
			nodetail = true;
		} else if (!g_strcmp0(Com_Argv(i), "-fulldetail")) {
			Com_Verbose("fulldetail = true\n");
			fulldetail = true;
		} else if (!g_strcmp0(Com_Argv(i), "-onlyents")) {
			Com_Verbose("onlyents = true\n");
			onlyents = true;
		} else if (!g_strcmp0(Com_Argv(i), "-micro")) {
			microvolume = atof(Com_Argv(i + 1));
			Com_Verbose("microvolume = %f\n", microvolume);
			i++;
		} else if (!g_strcmp0(Com_Argv(i), "-leaktest")) {
			Com_Verbose("leaktest = true\n");
			leaktest = true;
		} else if (!g_strcmp0(Com_Argv(i), "-verboseentities")) {
			Com_Verbose("verboseentities = true\n");
			verboseentities = true;
		} else if (!g_strcmp0(Com_Argv(i), "-subdivide")) {
			subdivide_size = atoi(Com_Argv(i + 1));
			Com_Verbose("subdivide_size = %d\n", subdivide_size);
			i++;
		} else if (!g_strcmp0(Com_Argv(i), "-block")) {
			block_xl = block_xh = atoi(Com_Argv(i + 1));
			block_yl = block_yh = atoi(Com_Argv(i + 2));
			Com_Verbose("block: %i,%i\n", block_xl, block_yl);
			i += 2;
		} else if (!g_strcmp0(Com_Argv(i), "-blocks")) {
			block_xl = atoi(Com_Argv(i + 1));
			block_yl = atoi(Com_Argv(i + 2));
			block_xh = atoi(Com_Argv(i + 3));
			block_yh = atoi(Com_Argv(i + 4));
			Com_Verbose("blocks: %i,%i to %i,%i\n", block_xl, block_yl, block_xh, block_yh);
			i += 4;
		} else if (!g_strcmp0(Com_Argv(i), "-tmpout")) {
			strcpy(outbase, "/tmp");
		} else
			break;
	}
}

/*
 * @brief
 */
static void Check_VIS_Options(int32_t argc) {
	int32_t i;

	for (i = argc; i < Com_Argc(); i++) {
		if (!g_strcmp0(Com_Argv(i), "-fast")) {
			Com_Verbose("fastvis = true\n");
			fastvis = true;
		} else if (!g_strcmp0(Com_Argv(i), "-nosort")) {
			Com_Verbose("nosort = true\n");
			nosort = true;
		} else
			break;
	}
}

/*
 * @brief
 */
static void Check_LIGHT_Options(int32_t argc) {
	int32_t i;

	for (i = argc; i < Com_Argc(); i++) {
		if (!g_strcmp0(Com_Argv(i), "-extra")) {
			extra_samples = true;
			Com_Verbose("extra samples = true\n");
		} else if (!g_strcmp0(Com_Argv(i), "-brightness")) {
			brightness = atof(Com_Argv(i + 1));
			Com_Verbose("brightness at %f\n", brightness);
			i++;
		} else if (!g_strcmp0(Com_Argv(i), "-saturation")) {
			saturation = atof(Com_Argv(i + 1));
			Com_Verbose("saturation at %f\n", saturation);
			i++;
		} else if (!g_strcmp0(Com_Argv(i), "-contrast")) {
			contrast = atof(Com_Argv(i + 1));
			Com_Verbose("contrast at %f\n", contrast);
			i++;
		} else if (!g_strcmp0(Com_Argv(i), "-surface")) {
			surface_scale *= atof(Com_Argv(i + 1));
			Com_Verbose("surface light scale at %f\n", surface_scale);
			i++;
		} else if (!g_strcmp0(Com_Argv(i), "-entity")) {
			entity_scale *= atof(Com_Argv(i + 1));
			Com_Verbose("entity light scale at %f\n", entity_scale);
			i++;
		} else
			break;
	}
}

/*
 * @brief
 */
static void Check_ZIP_Options(int32_t argc __attribute__((unused))) {
}

/*
 * @brief
 */
static void Check_MAT_Options(int32_t argc __attribute__((unused))) {
}

/*
 * @brief
 */
static void PrintHelpMessage(void) {
	Com_Print("General options\n");
	Com_Print("-v -verbose\n");
	Com_Print("-l -legacy            Compile a legacy Quake2 map\n");
	Com_Print("-d -debug\n");
	Com_Print("-t -threads <int>\n");

	Com_Print("\n");
	Com_Print("-bsp               Binary space partitioning (BSPing) options:\n");
	Com_Print(" -block <int> <int>\n");
	Com_Print(" -blocks <int> <int> <int> <int>\n");
	Com_Print(" -fulldetail - don't treat details (and trans surfaces) as details\n");
	Com_Print(" -leaktest\n");
	Com_Print(" -micro <float>\n");
	Com_Print(" -nocsg\n");
	Com_Print(" -nodetail - skip detail brushes\n");
	Com_Print(" -nofill\n");
	Com_Print(" -nomerge - skip node face merging\n");
	Com_Print(" -noopt\n");
	Com_Print(" -noprune - don't prune (or cut) nodes\n");
	Com_Print(" -noshare\n");
	Com_Print(" -nosubdivide\n");
	Com_Print(" -notjunc\n");
	Com_Print(" -nowater - skip water brushes in compilation\n");
	Com_Print(" -noweld\n");
	Com_Print(" -onlyents - modify existing bsp file with entities from map file\n");
	Com_Print(
			" -subdivide <int> -subdivide brushes for better light effects (but higher polycount)\n");
	Com_Print(" -tmpout\n");
	Com_Print(" -verboseentities - also be verbose about submodels (entities)\n");
	Com_Print("\n");
	Com_Print("-vis               VIS stage options:\n");
	Com_Print(" -fast\n");
	Com_Print(" -level\n");
	Com_Print(" -nosort\n");
	Com_Print("\n");
	Com_Print("-light             Lighting stage options:\n");
	Com_Print(" -contrast <float> - contrast factor\n");
	Com_Print(" -entity <float> - entity light scaling\n");
	Com_Print(" -extra - extra light samples\n");
	Com_Print(" -brightness <float> - brightness factor\n");
	Com_Print(" -saturation <float> - saturation factor\n");
	Com_Print(" -surface <float> - surface light scaling\n");
	Com_Print("\n");
	Com_Print("-zip               ZIP file options:\n");
	Com_Print("\n");
	Com_Print("Examples:\n");
	Com_Print("Standard full compile:\n q2wmap -bsp -vis -light maps/my.map\n");
	Com_Print("Fast vis, extra light, two threads:\n"
		"q2wmap -t 2 -bsp -vis -fast -light -extra maps/my.map\n");
	Com_Print("\n");
}

/*
 * @brief
 */
int32_t main(int32_t argc, char **argv) {
	int32_t i;
	_Bool do_bsp = false;
	_Bool do_vis = false;
	_Bool do_light = false;
	_Bool do_mat = false;
	_Bool do_zip = false;

	printf("Quake2World Map %s %s %s\n", VERSION, __DATE__, BUILD_HOST);

	memset(&quake2world, 0, sizeof(quake2world));

	quake2world.Debug = Debug;
	quake2world.Error = Error;
	quake2world.Verbose = Verbose;
	quake2world.Warn = Warn;

	quake2world.Init = Init;
	quake2world.Shutdown = Shutdown;

	signal(SIGHUP, Sys_Signal);
	signal(SIGINT, Sys_Signal);
	signal(SIGQUIT, Sys_Signal);
	signal(SIGILL, Sys_Signal);
	signal(SIGABRT, Sys_Signal);
	signal(SIGFPE, Sys_Signal);
	signal(SIGSEGV, Sys_Signal);
	signal(SIGTERM, Sys_Signal);

	Com_Init(argc, argv);

	// general options
	for (i = 1; i < Com_Argc(); i++) {
		if (!g_strcmp0(Com_Argv(i), "-v") || !g_strcmp0(Com_Argv(i), "-verbose")) {
			verbose = true;
			continue;
		}

		if (!g_strcmp0(Com_Argv(i), "-d") || !g_strcmp0(Com_Argv(i), "-debug")) {
			debug = true;
			continue;
		}

		if (!g_strcmp0(Com_Argv(i), "-t") || !g_strcmp0(Com_Argv(i), "-threads")) {
			Cvar_Set("threads", Com_Argv(i + 1));
			if (threads->modified) {
				Thread_Shutdown();
				Thread_Init();
			}
			continue;
		}

		if (!g_strcmp0(Com_Argv(i), "-l") || !g_strcmp0(Com_Argv(i), "-legacy")) {
			legacy = true;
			continue;
		}
	}

	// read compiling options
	for (i = 1; i < Com_Argc(); i++) {
		if (!g_strcmp0(Com_Argv(i), "-h") || !g_strcmp0(Com_Argv(i), "-help")) {
			PrintHelpMessage();
			Com_Shutdown(NULL);
		}

		if (!g_strcmp0(Com_Argv(i), "-bsp")) {
			do_bsp = true;
			Check_BSP_Options(i + 1);
		}

		if (!g_strcmp0(Com_Argv(i), "-vis")) {
			do_vis = true;
			Check_VIS_Options(i + 1);
		}

		if (!g_strcmp0(Com_Argv(i), "-light")) {
			do_light = true;
			Check_LIGHT_Options(i + 1);
		}

		if (!g_strcmp0(Com_Argv(i), "-mat")) {
			do_mat = true;
			Check_MAT_Options(i + 1);
		}

		if (!g_strcmp0(Com_Argv(i), "-zip")) {
			do_zip = true;
			Check_ZIP_Options(i + 1);
		}
	}

	if (!do_bsp && !do_vis && !do_light && !do_mat && !do_zip) {
		Com_Error(ERR_FATAL, "No action specified. Try %s -help\n", Com_Argv(0));
	}

	// ugly little hack to localize global paths to game paths
	// for e.g. GtkRadiant
	const char *c = strstr(Com_Argv(Com_Argc() - 1), "/maps/");
	c = c ? c + 1 : Com_Argv(Com_Argc() - 1);

	StripExtension(c, map_name);
	g_strlcpy(bsp_name, map_name, sizeof(bsp_name));
	g_strlcat(map_name, ".map", sizeof(map_name));
	g_strlcat(bsp_name, ".bsp", sizeof(bsp_name));

	// start timer
	const time_t start = time(NULL);

	if (do_bsp)
		BSP_Main();
	if (do_vis)
		VIS_Main();
	if (do_light)
		LIGHT_Main();
	if (do_mat)
		MAT_Main();
	if (do_zip)
		ZIP_Main();

	// emit time
	const time_t end = time(NULL);
	const int32_t total_time = (int32_t) (end - start);
	Com_Print("\nTotal Time: ");
	if (total_time > 59)
		Com_Print("%d Minutes ", total_time / 60);
	Com_Print("%d Seconds\n", total_time % 60);

	Com_Shutdown(NULL);
}
