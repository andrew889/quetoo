/*
 * Copyright(c) 1997-2001 id Software, Inc.
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

#include "console.h"

static console_data_t console_data;

#if BUILD_CLIENT
extern console_t cl_console;

extern void Cl_UpdateNotify(int32_t last_line);
extern void Cl_ClearNotify(void);
#endif

static cvar_t *con_ansi;

/*
 * @brief Update a console index struct, start parsing at pos
 */
static void Con_Update(console_t *con, char *pos) {
	char *wordstart;
	int32_t linelen;
	int32_t wordlen;
	int32_t i;
	int32_t curcolor;

	linelen = 0;
	wordlen = 0;
	curcolor = CON_COLOR_DEFAULT;
	con->line_start[con->last_line] = pos;
	con->line_color[con->last_line] = curcolor;

	if (con->width < 1)
		return;

	/* FIXME color at line_start is off by one line */
	wordstart = pos;
	while (*pos) {
		if (*pos == '\n') {
			while (wordlen > con->width && con->last_line < CON_MAX_LINES - 2) {
				// force wordsplit
				con->last_line++;
				con->line_start[con->last_line] = wordstart;
				con->line_color[con->last_line] = curcolor;
				wordstart = wordstart + (size_t) con->width;
				wordlen -= con->width;
			}
			if (linelen + wordlen > con->width) {
				// force linebreak
				con->last_line++;
				con->line_start[con->last_line] = wordstart;
				con->line_color[con->last_line] = curcolor;
			}
			con->last_line++;
			con->line_start[con->last_line] = pos + 1;
			curcolor = CON_COLOR_DEFAULT;
			con->line_color[con->last_line] = curcolor;
			linelen = 0;
			wordlen = 0;
			wordstart = pos + 1;
		} else if (*pos == ' ') {
			if (linelen + wordlen > con->width) {
				while (wordlen > con->width && con->last_line < CON_MAX_LINES - 2) {
					// force wordsplit
					con->last_line++;
					con->line_start[con->last_line] = wordstart;
					con->line_color[con->last_line] = curcolor;
					wordstart = wordstart + (size_t) con->width;
					wordlen -= con->width;
				}
				// force linebreak
				con->last_line++;
				con->line_start[con->last_line] = wordstart;
				con->line_color[con->last_line] = curcolor;
				linelen = wordlen + 1;
				wordlen = 0;
				wordstart = pos + 1;
			} else {
				linelen += wordlen + 1;
				wordlen = 0;
				wordstart = pos + 1;
			}
		} else if (IS_COLOR(pos)) {
			curcolor = (int32_t) *(pos + 1) - '0';
			pos++;
		} else if (IS_LEGACY_COLOR(pos)) {
			curcolor = CON_COLOR_ALT;
		} else {
			wordlen++;
		}
		pos++;

		// handle line overflow
		if (con->last_line >= CON_MAX_LINES - 4) {
			for (i = 0; i < CON_MAX_LINES - (CON_MAX_LINES >> 2); i++) {
				con->line_start[i] = con->line_start[i + (CON_MAX_LINES >> 2)];
				con->line_color[i] = con->line_color[i + (CON_MAX_LINES >> 2)];
			}
			con->last_line -= CON_MAX_LINES >> 2;
		}
	}

	// sentinel
	con->line_start[con->last_line + 1] = pos;
}

/*
 * @brief Change the width of an index, parse the console data structure if needed
 */
void Con_Resize(console_t *con, uint16_t width, uint16_t height) {
	if (!console_data.insert)
		return;

	if (con->height != height)
		con->height = height;

	if (con->width == width)
		return;

	// update the requested index
	con->width = width;
	con->last_line = 0;
	Con_Update(con, console_data.text);

#if BUILD_CLIENT
	if (!dedicated->value) {
		// clear client notification timings
		if (con == &cl_console)
			Cl_ClearNotify();
	}
#endif
}

/*
 * @brief Clear the console data buffer
 */
static void Con_Clear_f(void) {
	memset(console_data.text, 0, sizeof(console_data.text));
	console_data.insert = console_data.text;

#if BUILD_CLIENT
	if (!dedicated->value) {
		// update the index for the client console
		cl_console.last_line = 0;
		Con_Update(&cl_console, console_data.insert);
	}
#endif
#if HAVE_CURSES
	// update the index for the server console
	sv_console.last_line = 0;
	Con_Update(&sv_console, console_data.insert);
	// redraw the server console
	Curses_Refresh();
#endif
}

/*
 * @brief Save the console contents to a file
 */
static void Con_Dump_f(void) {
	file_t *file;
	char *pos;

	if (Cmd_Argc() != 2) {
		Com_Print("Usage: %s <file_name>\n", Cmd_Argv(0));
		return;
	}

	if (!(file = Fs_OpenWrite(Cmd_Argv(1)))) {
		Com_Warn("Couldn't open %s\n", Cmd_Argv(1));
	} else {
		pos = console_data.text;
		while (pos < console_data.insert) {
			if (IS_COLOR(pos)) {
				pos++;
			} else if (!IS_LEGACY_COLOR(pos)) {
				if (Fs_Write(file, pos, 1, 1) != 1) {
					Com_Warn("Failed to write console dump\n");
					break;
				}
			}
			pos++;
		}
		Fs_Close(file);
		Com_Print("Dumped console text to %s.\n", Cmd_Argv(1));
	}
}

/*
 * @brief Print a color-coded string to stdout, optionally removing colors.
 */
static void Con_PrintStdout(const char *text) {
	char buf[MAX_PRINT_MSG];
	int32_t bold, color;
	uint32_t i;

	// start the string with foreground color
	memset(buf, 0, sizeof(buf));
	if (con_ansi->value) {
		strcpy(buf, "\033[0;39m");
		i = 7;
	} else {
		i = 0;
	}

	while (*text && i < sizeof(buf) - 8) {

		if (IS_LEGACY_COLOR(text)) {
			if (con_ansi->value) {
				strcpy(&buf[i], "\033[0;32m");
				i += 7;
			}
			text++;
			continue;
		}

		if (IS_COLOR(text)) {
			if (con_ansi->value) {
				bold = 0;
				color = 39;
				switch (*(text + 1)) {
					case '0': // black is mapped to bold
						bold = 1;
						break;
					case '1': // red
						color = 31;
						break;
					case '2': // green
						color = 32;
						break;
					case '3': // yellow
						bold = 1;
						color = 33;
						break;
					case '4': // blue
						color = 34;
						break;
					case '5': // cyan
						color = 36;
						break;
					case '6': // magenta
						color = 35;
						break;
					case '7': // white is mapped to foreground color
						color = 39;
						break;
					default:
						break;
				}
				g_snprintf(&buf[i], 8, "\033[%d;%dm", bold, color);
				i += 7;
			}
			text += 2;
			continue;
		}

		if (*text == '\n' && con_ansi->value) {
			strcat(buf, "\033[0;39m");
			i += 7;
		}

		buf[i++] = *text;
		text++;
	}

	if (con_ansi->value) // restore foreground color
		strcat(buf, "\033[0;39m");

	// print to stdout
	if (buf[0] != '\0')
		fputs(buf, stdout);
}

/*
 * @brief Print a message to the console data buffer
 */
void Con_Print(const char *text) {

	// this can get called before the console is initialized
	if (!console_data.insert) {
		memset(console_data.text, 0, sizeof(console_data.text));
		console_data.insert = console_data.text;
	}

	// prevent overflow, text should still have a reasonable size
	if (console_data.insert + strlen(text) >= console_data.text + sizeof(console_data.text) - 1) {
		memcpy(console_data.text, console_data.text + (sizeof(console_data.text) >> 1), sizeof(console_data.text) >> 1);
		memset(console_data.text + (sizeof(console_data.text) >> 1) ,0 , sizeof(console_data.text) >> 1);
		console_data.insert -= sizeof(console_data.text) >> 1;
#if BUILD_CLIENT
		if (!dedicated->value) {
			// update the index for the client console
			cl_console.last_line = 0;
			Con_Update(&cl_console, console_data.text);
		}
#endif
#if HAVE_CURSES
		// update the index for the server console
		sv_console.last_line = 0;
		Con_Update(&sv_console, console_data.text);
#endif
	}

	// copy the text into the console buffer
	strcpy(console_data.insert, text);

#if BUILD_CLIENT
	if (!dedicated->value) {
		const int32_t last_line = cl_console.last_line;

		// update the index for the client console
		Con_Update(&cl_console, console_data.insert);

		// update client message notification times
		Cl_UpdateNotify(last_line);
	}
#endif

#if HAVE_CURSES
	// update the index for the server console
	Con_Update(&sv_console, console_data.insert);
#endif

	console_data.insert += strlen(text);

#if HAVE_CURSES
	if (!con_curses->value) {
		// print output to stdout
		Con_PrintStdout(text);
	} else {
		// Redraw the server console
		Curses_Refresh();
	}
#else
	// print output to stdout
	Con_PrintStdout(text);
#endif
}

/*
 * @brief Tab completion. Query various subsystems for potential
 * matches, and append an appropriate string to the input buffer. If no
 * matches are found, do nothing. If only one match is found, simply
 * append it. If multiple matches are found, append the longest possible
 * common prefix they all share.
 */
bool Con_CompleteCommand(char *input, uint16_t *pos, uint16_t len) {
	const char *pattern, *match;
	GList *matches = NULL;

	char *partial = input;
	if (*partial == '\\' || *partial == '/')
		partial++;

	if (!*partial)
		return false; // lets start with at least something

	// handle special cases for commands which accept filenames
	if (g_str_has_prefix(partial, "demo ")) {
		partial += strlen("demo ");
		pattern = va("demos/%s*.dem", partial);
		Fs_CompleteFile(pattern, &matches);
	} else if (g_str_has_prefix(partial, "exec ")) {
		partial += strlen("exec ");
		pattern = va("%s*.cfg", partial);
		Fs_CompleteFile(pattern, &matches);
	} else if (g_str_has_prefix(partial, "map ")) {
		partial += strlen("map ");
		pattern = va("maps/%s*.bsp", partial);
		Fs_CompleteFile(pattern, &matches);
	} else if (g_str_has_prefix(partial, "set ")) {
		partial += strlen("set ");
		pattern = va("%s*", partial);
		Cvar_CompleteVar(pattern, &matches);
	} else { // handle general case for commands and variables
		pattern = va("%s*", partial);
		Cmd_CompleteCommand(pattern, &matches);
		Cvar_CompleteVar(pattern, &matches);
	}

	if (g_list_length(matches) == 0)
		return false;

	if (g_list_length(matches) == 1)
		match = va("%s ", (char *) g_list_nth_data(matches, 0));
	else
		match = CommonPrefix(matches);

	g_snprintf(partial, len - (partial - input), "%s", match);
	(*pos) = strlen(input);

	g_list_free_full(matches, Mem_Free);

	return true;
}

/*
 * @brief Initialize the console subsystem. For Windows environments running
 * servers, we explicitly allocate a console and redirect stdio to and from it.
 */
void Con_Init(void) {

#if defined(_WIN32)
	if (dedicated->value) {
		if (AllocConsole()) {
			freopen("CONIN$", "r", stdin);
			freopen("CONOUT$", "w", stdout);
			freopen("CONERR$", "w", stderr);
		} else {
			Com_Warn("Failed to allocate console: %u\n", (uint32_t) GetLastError());
		}
	}

	con_ansi = Cvar_Get("con_ansi", "0", CVAR_NO_SET, NULL);
#else
	con_ansi = Cvar_Get("con_ansi", "1", CVAR_ARCHIVE, NULL);
#endif

#if HAVE_CURSES
	Curses_Init();
#endif

	Cmd_Add("clear", Con_Clear_f, 0, NULL);
	Cmd_Add("dump", Con_Dump_f, 0, NULL);
}

/*
 * @brief Shutdown the console subsystem
 */
void Con_Shutdown(void) {

	Cmd_Remove("clear");
	Cmd_Remove("dump");

#if HAVE_CURSES
	Curses_Shutdown();
#endif

#if defined(_WIN32)
	if (dedicated->value) {
		FreeConsole();
	}
#endif
}
