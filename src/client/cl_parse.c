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

#include "cl_local.h"

static char *sv_cmd_names[256] = {
		"SV_CMD_BAD",
		"SV_CMD_BASELINE",
		"SV_CMD_CBUF_TEXT",
		"SV_CMD_CONFIG_STRING",
		"SV_CMD_DISCONNECT",
		"SV_CMD_DOWNLOAD",
		"SV_CMD_FRAME",
		"SV_CMD_PRINT",
		"SV_CMD_RECONNECT",
		"SV_CMD_SERVER_DATA",
		"SV_CMD_SOUND" };

/*
 * @brief Returns true if the file exists, otherwise it attempts to start a download
 * from the server.
 */
bool Cl_CheckOrDownloadFile(const char *filename) {
	char cmd[MAX_STRING_CHARS];

	if (cls.state == CL_DISCONNECTED) {
		Com_Print("Not connected\n");
		return true;
	}

	if (IS_INVALID_DOWNLOAD(filename)) {
		Com_Warn("Refusing to download \"%s\"\n", filename);
		return true;
	}

	Com_Debug("Checking for %s\n", filename);

	if (Fs_Exists(filename)) { // it exists, no need to download
		return true;
	}

	Com_Debug("Attempting to download %s\n", filename);

	strncpy(cls.download.name, filename, sizeof(cls.download.name));

	// udp downloads to a temp name, and only renames when done
	StripExtension(cls.download.name, cls.download.tempname);
	g_strlcat(cls.download.tempname, ".tmp", sizeof(cls.download.tempname));

	// attempt an http download if available
	if (cls.download_url[0] && Cl_HttpDownload())
		return false;

	// check to see if we already have a tmp for this file, if so, try to resume
	// open the file if not opened yet

	if (Fs_Exists(cls.download.tempname)) { // a temp file exists, resume download
		int64_t len = Fs_Load(cls.download.tempname, NULL);

		if ((cls.download.file = Fs_OpenAppend(cls.download.tempname))) {

			if (Fs_Seek(cls.download.file, len - 1)) {
				// give the server the offset to start the download
				Com_Debug("Resuming %s...\n", cls.download.name);

				g_snprintf(cmd, sizeof(cmd), "download %s %u", cls.download.name, (uint32_t) len);
				Net_WriteByte(&cls.net_chan.message, CL_CMD_STRING);
				Net_WriteString(&cls.net_chan.message, cmd);

				return false;
			}
		}
	}

	// or start if from the beginning
	Com_Debug("Downloading %s...\n", cls.download.name);

	g_snprintf(cmd, sizeof(cmd), "download %s", cls.download.name);
	Net_WriteByte(&cls.net_chan.message, CL_CMD_STRING);
	Net_WriteString(&cls.net_chan.message, cmd);

	return false;
}

/*
 * @brief Manually request a download from the server.
 */
void Cl_Download_f(void) {

	if (Cmd_Argc() != 2) {
		Com_Print("Usage: %s <file_name>\n", Cmd_Argv(0));
		return;
	}

	Cl_CheckOrDownloadFile(Cmd_Argv(1));
}

/*
 * @brief The server sends this command just after server_data. Hang onto the spawn
 * count and check for the media we'll need to enter the game.
 */
void Cl_Precache_f(void) {

	if (Cmd_Argc() != 2) {
		Com_Print("Usage: %s <spawn_count>\n", Cmd_Argv(0));
		return;
	}

	cls.spawn_count = strtoul(Cmd_Argv(1), NULL, 0);

	cl.precache_check = CS_ZIP;

	Cl_RequestNextDownload();
}

/*
 * @brief
 */
static void Cl_ParseBaseline(void) {
	static entity_state_t null_state;

	const uint16_t number = Net_ReadShort(&net_message);
	const uint16_t bits = Net_ReadShort(&net_message);

	entity_state_t *state = &cl.entities[number].baseline;

	Net_ReadDeltaEntity(&net_message, &null_state, state, number, bits);
}

/*
 * @brief
 */
void Cl_ParseConfigString(void) {
	const uint16_t i = (uint16_t) Net_ReadShort(&net_message);

	if (i >= MAX_CONFIG_STRINGS) {
		Com_Error(ERR_DROP, "Invalid index %i\n", i);
	}

	strcpy(cl.config_strings[i], Net_ReadString(&net_message));
	const char *s = cl.config_strings[i];

	if (i > CS_MODELS && i < CS_MODELS + MAX_MODELS) {
		if (cls.state == CL_ACTIVE) {
			cl.model_precache[i - CS_MODELS] = R_LoadModel(s);
			if (*s == '*') {
				cl.cm_models[i - CS_MODELS] = Cm_Model(s);
			} else {
				cl.cm_models[i - CS_MODELS] = NULL;
			}
		}
	} else if (i >= CS_SOUNDS && i < CS_SOUNDS + MAX_SOUNDS) {
		if (cls.state == CL_ACTIVE) {
			cl.sound_precache[i - CS_SOUNDS] = S_LoadSample(s);
		}
	} else if (i >= CS_IMAGES && i < CS_IMAGES + MAX_IMAGES) {
		if (cls.state == CL_ACTIVE) {
			cl.image_precache[i - CS_IMAGES] = R_LoadImage(s, IT_PIC);
		}
	}

	cls.cgame->UpdateConfigString(i);
}

/*
 * @brief A download message has been received from the server.
 */
static void Cl_ParseDownload(void) {
	int32_t size, percent;

	// read the data
	size = Net_ReadShort(&net_message);
	percent = Net_ReadByte(&net_message);
	if (size < 0) {
		Com_Debug("Server does not have this file\n");
		if (cls.download.file) {
			// if here, we tried to resume a file but the server said no
			Fs_Close(cls.download.file);
			cls.download.file = NULL;
		}
		Cl_RequestNextDownload();
		return;
	}

	// open the file if not opened yet
	if (!cls.download.file) {

		if (!(cls.download.file = Fs_OpenWrite(cls.download.tempname))) {
			net_message.read += size;
			Com_Warn("Failed to open %s\n", cls.download.tempname);
			Cl_RequestNextDownload();
			return;
		}
	}

	Fs_Write(cls.download.file, net_message.data + net_message.read, 1, size);

	net_message.read += size;

	if (percent != 100) {
		Net_WriteByte(&cls.net_chan.message, CL_CMD_STRING);
		Net_WriteString(&cls.net_chan.message, "nextdl");
	} else {
		Fs_Close(cls.download.file);
		cls.download.file = NULL;

		// add new archives to the search path
		if (Fs_Rename(cls.download.tempname, cls.download.name)) {
			if (strstr(cls.download.name, ".zip")) {
				Fs_AddToSearchPath(cls.download.name);
			}
		} else {
			Com_Error(ERR_DROP, "Failed to rename %s\n", cls.download.name);
		}

		// get another file if needed
		Cl_RequestNextDownload();
	}
}

/*
 * @brief
 */
static void Cl_ParseServerData(void) {

	// wipe the cl_client_t struct
	Cl_ClearState();

	cls.state = CL_CONNECTED;
	cls.key_state.dest = KEY_CONSOLE;

	// parse protocol version number
	const uint16_t major = Net_ReadShort(&net_message);
	const uint16_t minor = Net_ReadShort(&net_message);

	// ensure protocol major matches
	if (major != PROTOCOL_MAJOR) {
		Com_Error(ERR_DROP, "Server is using protocol major %d\n", major);
	}

	// retrieve spawn count and packet rate
	cl.server_count = Net_ReadLong(&net_message);
	cl.server_hz = Net_ReadLong(&net_message);

	// determine if we're viewing a demo
	cl.demo_server = Net_ReadByte(&net_message);

	// game directory
	char *str = Net_ReadString(&net_message);
	if (g_strcmp0(Cvar_GetString("game"), str)) {

		Fs_SetGame(str);

		// reload the client game
		Cl_InitCgame();
	}

	// ensure protocol minor matches
	if (minor != cls.cgame->protocol) {
		Com_Error(ERR_DROP, "Server is using protocol minor %d\n", minor);
	}

	// parse client slot number, which is our entity number + 1
	cl.client_num = Net_ReadShort(&net_message);

	// get the full level name
	str = Net_ReadString(&net_message);
	Com_Print("\n");
	Com_Print("%c%s\n", 2, str);
}

/*
 * @brief
 */
static void Cl_ParseSound(void) {
	vec3_t origin;
	vec_t *org;
	uint16_t index;
	uint16_t ent_num;
	int32_t atten;
	int32_t flags;

	flags = Net_ReadByte(&net_message);

	if ((index = Net_ReadByte(&net_message)) > MAX_SOUNDS)
		Com_Error(ERR_DROP, "Bad index (%d)\n", index);

	if (flags & S_ATTEN)
		atten = Net_ReadByte(&net_message);
	else
		atten = ATTEN_DEFAULT;

	if (flags & S_ENTNUM) { // entity relative
		ent_num = Net_ReadShort(&net_message);

		if (ent_num > MAX_ENTITIES)
			Com_Error(ERR_DROP, "Bad entity number (%d)\n", ent_num);
	} else {
		ent_num = 0;
	}

	if (flags & S_ORIGIN) { // positioned in space
		Net_ReadPosition(&net_message, origin);

		org = origin;
	} else
		// use ent_num
		org = NULL;

	if (!cl.sound_precache[index])
		return;

	S_PlaySample(org, ent_num, cl.sound_precache[index], atten);
}

/*
 * @brief
 */
static bool Cl_IgnoreChatMessage(const char *msg) {

	const char *s = strtok(cl_ignore->string, " ");

	if (*cl_ignore->string == '\0')
		return false; // nothing currently filtered

	while (s) {
		if (strstr(msg, s))
			return true;
		s = strtok(NULL, " ");
	}
	return false; // msg is okay
}

/*
 * @brief
 */
static void Cl_ShowNet(const char *s) {
	if (cl_show_net_messages->integer >= 2)
		Com_Print("%3u: %s\n", (uint32_t) (net_message.read - 1), s);
}

/*
 * @brief
 */
void Cl_ParseServerMessage(void) {
	int32_t cmd, old_cmd;
	char *s;
	int32_t i;

	if (cl_show_net_messages->integer == 1)
		Com_Print("%u ", (uint32_t) net_message.size);
	else if (cl_show_net_messages->integer >= 2)
		Com_Print("------------------\n");

	cl.byte_counter += net_message.size;
	cmd = 0;

	// parse the message
	while (true) {
		if (net_message.read > net_message.size) {
			Com_Error(ERR_DROP, "Bad server message\n");
		}

		old_cmd = cmd;
		cmd = Net_ReadByte(&net_message);

		if (cmd == -1) {
			Cl_ShowNet("END OF MESSAGE");
			break;
		}

		if (cl_show_net_messages->integer >= 2 && sv_cmd_names[cmd])
			Cl_ShowNet(sv_cmd_names[cmd]);

		switch (cmd) {

			case SV_CMD_BASELINE:
				Cl_ParseBaseline();
				break;

			case SV_CMD_CBUF_TEXT:
				s = Net_ReadString(&net_message);
				Cbuf_AddText(s);
				break;

			case SV_CMD_CONFIG_STRING:
				Cl_ParseConfigString();
				break;

			case SV_CMD_DISCONNECT:
				Com_Error(ERR_DROP, "Server disconnected\n");
				break;

			case SV_CMD_DOWNLOAD:
				Cl_ParseDownload();
				break;

			case SV_CMD_FRAME:
				Cl_ParseFrame();
				break;

			case SV_CMD_PRINT:
				i = Net_ReadByte(&net_message);
				s = Net_ReadString(&net_message);
				if (i == PRINT_CHAT) {
					if (Cl_IgnoreChatMessage(s)) // filter /ignore'd chatters
						break;
					if (*cl_chat_sound->string) // trigger chat sound
						S_StartLocalSample(cl_chat_sound->string);
				} else if (i == PRINT_TEAMCHAT) {
					if (Cl_IgnoreChatMessage(s)) // filter /ignore'd chatters
						break;
					if (*cl_team_chat_sound->string) // trigger chat sound
						S_StartLocalSample(cl_team_chat_sound->string);
				}
				Com_Print("%s", s);
				break;

			case SV_CMD_RECONNECT:
				Com_Print("Server disconnected, reconnecting...\n");
				// stop download
				if (cls.download.file) {
					if (cls.download.http) // clean up http downloads
						Cl_HttpDownload_Complete();
					else
						// or just stop legacy ones
						Fs_Close(cls.download.file);
					cls.download.name[0] = '\0';
					cls.download.file = NULL;
				}
				cls.state = CL_CONNECTING;
				cls.connect_time = 0; // fire immediately
				break;

			case SV_CMD_SERVER_DATA:
				Cl_ParseServerData();
				break;

			case SV_CMD_SOUND:
				Cl_ParseSound();
				break;

			default:
				// delegate to the client game module before failing
				if (!cls.cgame->ParseMessage(cmd)) {
					Com_Error(ERR_DROP, "Illegible server message:\n"
							" %d: last command was %s\n", cmd, sv_cmd_names[old_cmd]);
				}
				break;
		}
	}

	Cl_AddNetGraph();

	Cl_WriteDemoMessage();
}
