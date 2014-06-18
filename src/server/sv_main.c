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

#include "sv_local.h"
#include "console.h"

sv_static_t svs; // persistent server info
sv_server_t sv; // per-level server info

sv_client_t *sv_client; // current client

cvar_t *sv_download_url;
cvar_t *sv_enforce_time;
cvar_t *sv_hostname;
cvar_t *sv_hz;
cvar_t *sv_max_clients;
cvar_t *sv_no_areas;
cvar_t *sv_public;
cvar_t *sv_rcon_password; // password for remote server commands
cvar_t *sv_timeout;
cvar_t *sv_udp_download;

/*
 * @brief Called when the player is totally leaving the server, either willingly
 * or unwillingly. This is NOT called if the entire server is quitting
 * or crashing.
 */
void Sv_DropClient(sv_client_t *cl) {
	g_entity_t *ent;

	Mem_ClearBuffer(&cl->net_chan.message);
	Mem_ClearBuffer(&cl->datagram.buffer);

	if (cl->datagram.messages) {
		g_list_free_full(cl->datagram.messages, Mem_Free);
	}

	if (cl->state > SV_CLIENT_FREE) { // send the disconnect

		if (cl->state == SV_CLIENT_ACTIVE) { // after informing the game module
			svs.game->ClientDisconnect(cl->entity);
		}

		Net_WriteByte(&cl->net_chan.message, SV_CMD_DISCONNECT);
		Netchan_Transmit(&cl->net_chan, cl->net_chan.message.data, cl->net_chan.message.size);
	}

	if (cl->download.buffer) {
		Fs_Free(cl->download.buffer);
	}

	ent = cl->entity;

	memset(cl, 0, sizeof(*cl));

	cl->entity = ent;
	cl->last_frame = -1;
}

/*
 * @brief Returns a string fit for heartbeats and status replies.
 */
static const char *Sv_StatusString(void) {
	static char status[MAX_MSG_SIZE - 16];
	int32_t i;

	g_snprintf(status, sizeof(status), "%s\n", Cvar_ServerInfo());
	size_t status_len = strlen(status);

	for (i = 0; i < sv_max_clients->integer; i++) {

		const sv_client_t *cl = &svs.clients[i];

		if (cl->state == SV_CLIENT_CONNECTED || cl->state == SV_CLIENT_ACTIVE) {
			char player[MAX_TOKEN_CHARS];
			const uint32_t ping = cl->entity->client->ping;

			g_snprintf(player, sizeof(player), "%d %u \"%s\"\n", i, ping, cl->name);
			const size_t player_len = strlen(player);

			if (status_len + player_len + 1 >= sizeof(status))
				break; // can't hold any more

			strcat(status, player);
			status_len += player_len;
		}
	}

	return status;
}

/*
 * @brief Responds with all the info that qplug or qspy can see.
 */
static void Sv_Status_f(void) {
	Netchan_OutOfBandPrint(NS_UDP_SERVER, &net_from, "print\n%s", Sv_StatusString());
}

/*
 * @brief
 */
static void Sv_Ack_f(void) {
	Com_Print("Ping acknowledge from %s\n", Net_NetaddrToString(&net_from));
}

/*
 * @brief Responds with brief info for broadcast scans.
 */
static void Sv_Info_f(void) {
	char string[MAX_MSG_SIZE];

	if (sv_max_clients->integer == 1) {
		return; // ignore in single player
	}

	const int32_t p = atoi(Cmd_Argv(1));
	if (p != PROTOCOL_MAJOR) {
		g_snprintf(string, sizeof(string), "%s: Wrong protocol: %d != %d", sv_hostname->string, p,
		PROTOCOL_MAJOR);
	} else {
		int32_t i, count = 0;

		for (i = 0; i < sv_max_clients->integer; i++) {
			if (svs.clients[i].state >= SV_CLIENT_CONNECTED)
				count++;
		}

		g_snprintf(string, sizeof(string), "%-63s\\%-31s\\%-31s\\%d\\%d", sv_hostname->string,
				sv.name, svs.game->GameName(), count, sv_max_clients->integer);
	}

	Netchan_OutOfBandPrint(NS_UDP_SERVER, &net_from, "info\n%s", string);
}

/*
 * @brief Just responds with an acknowledgment.
 */
static void Sv_Ping_f(void) {
	Netchan_OutOfBandPrint(NS_UDP_SERVER, &net_from, "ack");
}

/*
 * @brief Returns a challenge number that can be used in a subsequent client_connect
 * command.
 *
 * We do this to prevent denial of service attacks that flood the server with
 * invalid connection IPs. With a challenge, they must give a valid address.
 */
static void Sv_GetChallenge_f(void) {
	uint16_t i, oldest;
	uint32_t oldest_time;

	oldest = 0;
	oldest_time = 0x7fffffff;

	// see if we already have a challenge for this ip
	for (i = 0; i < MAX_CHALLENGES; i++) {

		if (Net_CompareClientNetaddr(&net_from, &svs.challenges[i].addr))
			break;

		if (svs.challenges[i].time < oldest_time) {
			oldest_time = svs.challenges[i].time;
			oldest = i;
		}
	}

	if (i == MAX_CHALLENGES) {
		// overwrite the oldest
		svs.challenges[oldest].challenge = Random() & 0x7fff;
		svs.challenges[oldest].addr = net_from;
		svs.challenges[oldest].time = quake2world.time;
		i = oldest;
	}

	// send it back
	Netchan_OutOfBandPrint(NS_UDP_SERVER, &net_from, "challenge %i", svs.challenges[i].challenge);
}

/*
 * @brief A connection request that did not come from the master.
 */
static void Sv_Connect_f(void) {
	char user_info[MAX_USER_INFO_STRING];
	sv_client_t *cl, *client;
	int32_t i;

	Com_Debug("Svc_Connect()\n");

	net_addr_t *addr = &net_from;

	const int32_t version = strtol(Cmd_Argv(1), NULL, 0);

	// resolve protocol
	if (version != PROTOCOL_MAJOR) {
		Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nServer is version %d.\n",
		PROTOCOL_MAJOR);
		return;
	}

	const uint8_t qport = strtoul(Cmd_Argv(2), NULL, 0);

	const uint32_t challenge = strtoul(Cmd_Argv(3), NULL, 0);

	// copy user_info, leave room for ip stuffing
	g_strlcpy(user_info, Cmd_Argv(4), sizeof(user_info) - 25);

	if (*user_info == '\0') { // catch empty user_info
		Com_Print("Empty user_info from %s\n", Net_NetaddrToString(addr));
		Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nConnection refused\n");
		return;
	}

	if (strchr(user_info, '\xFF')) { // catch end of message in string exploit
		Com_Print("Illegal user_info contained xFF from %s\n", Net_NetaddrToString(addr));
		Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nConnection refused\n");
		return;
	}

	if (strlen(GetUserInfo(user_info, "ip"))) { // catch spoofed ips
		Com_Print("Illegal user_info contained ip from %s\n", Net_NetaddrToString(addr));
		Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nConnection refused\n");
		return;
	}

	if (!ValidateUserInfo(user_info)) { // catch otherwise invalid user_info
		Com_Print("Invalid user_info from %s\n", Net_NetaddrToString(addr));
		Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nConnection refused\n");
		return;
	}

	// force the ip so the game can filter on it
	SetUserInfo(user_info, "ip", Net_NetaddrToString(addr));

	// enforce a valid challenge to avoid denial of service attack
	for (i = 0; i < MAX_CHALLENGES; i++) {
		if (Net_CompareClientNetaddr(addr, &svs.challenges[i].addr)) {
			if (challenge == svs.challenges[i].challenge) {
				svs.challenges[i].challenge = 0;
				break; // good
			}
			Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nBad challenge\n");
			return;
		}
	}
	if (i == MAX_CHALLENGES) {
		Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nNo challenge for address\n");
		return;
	}

	// resolve the client slot
	client = NULL;

	// first check for an ungraceful reconnect (client crashed, perhaps)
	for (i = 0, cl = svs.clients; i < sv_max_clients->integer; i++, cl++) {

		const net_chan_t *ch = &cl->net_chan;

		if (cl->state == SV_CLIENT_FREE) // not in use, not interested
			continue;

		// the base address and either the qport or real port must match
		if (Net_CompareClientNetaddr(addr, &ch->remote_address)) {

			if (addr->port == ch->remote_address.port || qport == ch->qport) {
				client = cl;
				break;
			}
		}
	}

	// otherwise, treat as a fresh connect to a new slot
	if (!client) {
		for (i = 0, cl = svs.clients; i < sv_max_clients->integer; i++, cl++) {
			if (cl->state == SV_CLIENT_FREE && !cl->entity->ai) { // we have a free one
				client = cl;
				break;
			}
		}
	}

	// no soup for you, next!!
	if (!client) {
		Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nServer is full\n");
		Com_Debug("Rejected a connection\n");
		return;
	}

	// give the game a chance to reject this connection or modify the user_info
	if (!(svs.game->ClientConnect(client->entity, user_info))) {
		const char *rejmsg = GetUserInfo(user_info, "rejmsg");

		if (strlen(rejmsg)) {
			Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\n%s\nConnection refused\n", rejmsg);
		} else {
			Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "print\nConnection refused\n");
		}

		Com_Debug("Game rejected a connection\n");
		return;
	}

	// parse some info from the info strings
	g_strlcpy(client->user_info, user_info, sizeof(client->user_info));
	Sv_UserInfoChanged(client);

	// send the connect packet to the client
	Netchan_OutOfBandPrint(NS_UDP_SERVER, addr, "client_connect %s", sv_download_url->string);

	Netchan_Setup(NS_UDP_SERVER, &client->net_chan, addr, qport);

	Mem_InitBuffer(&client->datagram.buffer, client->datagram.data, sizeof(client->datagram.data));
	client->datagram.buffer.allow_overflow = true;

	client->last_message = svs.real_time; // don't timeout

	client->state = SV_CLIENT_CONNECTED;
}

/*
 * @brief
 */
static _Bool Sv_RconAuthenticate(void) {

	// a password must be set for rcon to be available
	if (*sv_rcon_password->string == '\0')
		return false;

	// and of course the passwords must match
	if (g_strcmp0(Cmd_Argv(1), sv_rcon_password->string))
		return false;

	return true;
}

/*
 * @brief A client issued an rcon command. Shift down the remaining args and
 * redirect all output to the invoking client.
 */
static void Sv_Rcon_f(void) {
	const _Bool auth = Sv_RconAuthenticate();
	const char *addr = Net_NetaddrToString(&net_from);

	// first print to the server console
	if (auth)
		Com_Print("Rcon from %s:\n%s\n", addr, net_message.data + 4);
	else
		Com_Print("Bad rcon from %s:\n%s\n", addr, net_message.data + 4);

	// then redirect the remaining output back to the client
	Com_BeginRedirect(RD_PACKET, sv_outputbuf, SV_OUTPUTBUF_LENGTH, Sv_FlushRedirect);

	if (auth) {
		char remaining[MAX_STRING_CHARS];
		int32_t i;

		remaining[0] = 0;

		for (i = 2; i < Cmd_Argc(); i++) {
			strcat(remaining, Cmd_Argv(i));
			strcat(remaining, " ");
		}

		Cmd_ExecuteString(remaining);
	} else {
		Com_Print("Bad rcon_password\n");
	}

	Com_EndRedirect();
}

/*
 * @brief A connection-less packet has four leading 0xff bytes to distinguish
 * it from a game channel. Clients that are in the game can still send these,
 * and they will be handled here.
 */
static void Sv_ConnectionlessPacket(void) {

	Net_BeginReading(&net_message);
	Net_ReadLong(&net_message); // skip the -1 marker

	const char *s = Net_ReadStringLine(&net_message);

	Cmd_TokenizeString(s);

	const char *c = Cmd_Argv(0);
	const char *a = Net_NetaddrToString(&net_from);

	Com_Debug("Packet from %s: %s\n", a, c);

	if (!g_strcmp0(c, "ping"))
		Sv_Ping_f();
	else if (!g_strcmp0(c, "ack"))
		Sv_Ack_f();
	else if (!g_strcmp0(c, "status"))
		Sv_Status_f();
	else if (!g_strcmp0(c, "info"))
		Sv_Info_f();
	else if (!g_strcmp0(c, "get_challenge"))
		Sv_GetChallenge_f();
	else if (!g_strcmp0(c, "connect"))
		Sv_Connect_f();
	else if (!g_strcmp0(c, "rcon"))
		Sv_Rcon_f();
	else
		Com_Print("Bad connectionless packet from %s:\n%s\n", a, s);
}

/*
 * @brief Updates the "ping" times for all spawned clients.
 */
static void Sv_UpdatePings(void) {
	int32_t i, j;
	sv_client_t * cl;
	int32_t total, count;

	for (i = 0; i < sv_max_clients->integer; i++) {

		cl = &svs.clients[i];

		if (cl->state != SV_CLIENT_ACTIVE)
			continue;

		total = count = 0;
		for (j = 0; j < SV_CLIENT_LATENCY_COUNT; j++) {
			if (cl->frame_latency[j] > 0) {
				total += cl->frame_latency[j];
				count++;
			}
		}

		if (!count)
			cl->entity->client->ping = 0;
		else
			cl->entity->client->ping = total / count;
	}
}

/*
 * @brief Once per second, gives all clients an allotment of 1000 milliseconds
 * for their movement commands which will be decremented as we receive
 * new information from them. If they drift by a significant margin
 * over the next interval, assume they are trying to cheat.
 */
static void Sv_CheckCommandTimes(void) {
	static uint32_t last_check_time = -9999;
	int32_t i;

	if (svs.real_time < last_check_time) { // wrap around from last level
		last_check_time = -9999;
	}

	// see if its time to check the movements
	if (svs.real_time - last_check_time < CMD_MSEC_CHECK_INTERVAL) {
		return;
	}

	last_check_time = svs.real_time;

	// inspect each client, ensuring they are reasonably in sync with us
	for (i = 0; i < sv_max_clients->integer; i++) {
		sv_client_t *cl = &svs.clients[i];

		if (cl->state < SV_CLIENT_ACTIVE) {
			continue;
		}

		if (sv_enforce_time->value) { // check them

			if (cl->cmd_msec > CMD_MSEC_ALLOWABLE_DRIFT) { // irregular movement
				cl->cmd_msec_errors++;

				Com_Debug("%s drifted %dms\n", Sv_NetaddrToString(cl), cl->cmd_msec);

				if (cl->cmd_msec_errors >= sv_enforce_time->value) {
					Com_Warn("Too many errors from %s\n", Sv_NetaddrToString(cl));
					Sv_KickClient(cl, "Irregular movement");
					continue;
				}
			} else { // normal movement

				if (cl->cmd_msec_errors) {
					cl->cmd_msec_errors--;
				}
			}
		}

		cl->cmd_msec = 0; // reset for next cycle
	}
}

/*
 * @brief
 */
static void Sv_ReadPackets(void) {
	int32_t i;
	sv_client_t * cl;
	byte qport;

	while (Net_ReceiveDatagram(NS_UDP_SERVER, &net_from, &net_message)) {

		// check for connectionless packet (0xffffffff) first
		if (*(uint32_t *) net_message.data == 0xffffffff) {
			Sv_ConnectionlessPacket();
			continue;
		}

		// read the qport out of the message so we can fix up
		// stupid address translating routers
		Net_BeginReading(&net_message);

		Net_ReadLong(&net_message); // sequence number
		Net_ReadLong(&net_message); // sequence number

		qport = Net_ReadByte(&net_message) & 0xff;

		// check for packets from connected clients
		for (i = 0, cl = svs.clients; i < sv_max_clients->integer; i++, cl++) {

			if (cl->state == SV_CLIENT_FREE)
				continue;

			if (!Net_CompareClientNetaddr(&net_from, &cl->net_chan.remote_address))
				continue;

			if (cl->net_chan.qport != qport)
				continue;

			if (cl->net_chan.remote_address.port != net_from.port) {
				Com_Warn("Fixing up a translated port\n");
				cl->net_chan.remote_address.port = net_from.port;
			}

			// this is a valid, sequenced packet, so process it
			if (Netchan_Process(&cl->net_chan, &net_message)) {
				cl->last_message = svs.real_time; // nudge timeout
				Sv_ParseClientMessage(cl);
			}

			// we've processed the packet for the correct client, so break
			break;
		}
	}
}

/*
 * @brief
 */
static void Sv_CheckTimeouts(void) {
	sv_client_t * cl;
	int32_t i;

	const uint32_t timeout = svs.real_time - 1000 * sv_timeout->value;

	if (timeout > svs.real_time) {
		// the server is just starting, don't bother
		return;
	}

	for (i = 0, cl = svs.clients; i < sv_max_clients->integer; i++, cl++) {

		if (cl->state == SV_CLIENT_FREE)
			continue;

		// enforce timeouts by dropping the client
		if (cl->last_message < timeout) {
			Sv_BroadcastPrint(PRINT_HIGH, "%s timed out\n", cl->name);
			Sv_DropClient(cl);
		}
	}
}

/*
 * @brief Resets entity flags and other state which should only last one frame.
 */
static void Sv_ResetEntities(void) {

	if (sv.state != SV_ACTIVE_GAME)
		return;

	for (int32_t i = 0; i < svs.game->num_entities; i++) {
		g_entity_t *edict = ENTITY_FOR_NUM(i);

		// events only last for a single message
		edict->s.event = 0;
	}
}

/*
 * @brief Updates the game module's time and runs its frame function once per
 * server frame.
 */
static void Sv_RunGameFrame(void) {

	sv.frame_num++;
	sv.time = sv.frame_num * 1000 / svs.frame_rate;

	if (sv.time < svs.real_time) {
		Com_Debug("Sv_RunGameFrame: High clamp: %dms\n", svs.real_time - sv.time);
		svs.real_time = sv.time;
	}

	if (sv.state == SV_ACTIVE_GAME) {
		svs.game->Frame();
	}
}

/*
 * @brief
 */
static void Sv_InitMasters(void) {

	memset(&svs.masters, 0, sizeof(svs.masters));

	// set default master server
	Net_StringToNetaddr(IP_MASTER, &svs.masters[0]);
	svs.masters[0].port = htons(PORT_MASTER);
}

#define HEARTBEAT_SECONDS 300

/*
 * @brief Sends heartbeat messages to master servers every 300s.
 */
static void Sv_HeartbeatMasters(void) {
	const char *string;
	int32_t i;

	if (!dedicated->value)
		return; // only dedicated servers report to masters

	if (!sv_public->value)
		return; // a private dedicated game

	if (!svs.initialized) // we're not up yet
		return;

	if (svs.next_heartbeat > svs.real_time)
		return; // not time to send yet

	svs.next_heartbeat = svs.real_time + HEARTBEAT_SECONDS * 1000;

	// send the same string that we would give for a status command
	string = Sv_StatusString();

	// send to each master server
	for (i = 0; i < MAX_MASTERS; i++) {
		if (svs.masters[i].port) {
			Com_Print("Sending heartbeat to %s\n", Net_NetaddrToString(&svs.masters[i]));
			Netchan_OutOfBandPrint(NS_UDP_SERVER, &svs.masters[i], "heartbeat\n%s", string);
		}
	}
}

/*
 * @brief Informs master servers that this server is halting.
 */
static void Sv_ShutdownMasters(void) {
	int32_t i;

	if (!dedicated->value)
		return; // only dedicated servers send heartbeats

	if (!sv_public->value)
		return; // a private dedicated game

	// send to group master
	for (i = 0; i < MAX_MASTERS; i++) {
		if (svs.masters[i].port) {
			Com_Print("Sending shutdown to %s\n", Net_NetaddrToString(&svs.masters[i]));
			Netchan_OutOfBandPrint(NS_UDP_SERVER, &svs.masters[i], "shutdown");
		}
	}
}

/*
 * @brief
 */
void Sv_KickClient(sv_client_t *cl, const char *msg) {
	char buf[MAX_STRING_CHARS], name[32];

	if (!cl)
		return;

	if (cl->state < SV_CLIENT_CONNECTED)
		return;

	if (*cl->name == '\0') // force a name to kick
		strcpy(name, "player");
	else
		g_strlcpy(name, cl->name, sizeof(name));

	memset(buf, 0, sizeof(buf));

	if (msg && *msg != '\0')
		g_snprintf(buf, sizeof(buf), ": %s", msg);

	Sv_ClientPrint(cl->entity, PRINT_HIGH, "You were kicked%s\n", buf);

	Sv_DropClient(cl);

	Sv_BroadcastPrint(PRINT_HIGH, "%s was kicked%s\n", name, buf);
}

/*
 * @brief A convenience function for printing out client addresses.
 */
const char *Sv_NetaddrToString(const sv_client_t *cl) {
	return Net_NetaddrToString(&cl->net_chan.remote_address);
}

#define MIN_RATE 8000
#define DEFAULT_RATE 20000

/*
 * @brief Enforces safe user_info data before passing onto game module.
 */
void Sv_UserInfoChanged(sv_client_t *cl) {
	char *val;
	size_t i;

	if (*cl->user_info == '\0') { // catch empty user_info
		Com_Print("Empty user_info from %s\n", Sv_NetaddrToString(cl));
		Sv_KickClient(cl, "Bad user info");
		return;
	}

	if (strchr(cl->user_info, '\xFF')) { // catch end of message exploit
		Com_Print("Illegal user_info contained xFF from %s\n", Sv_NetaddrToString(cl));
		Sv_KickClient(cl, "Bad user info");
		return;
	}

	if (!ValidateUserInfo(cl->user_info)) { // catch otherwise invalid user_info
		Com_Print("Invalid user_info from %s\n", Sv_NetaddrToString(cl));
		Sv_KickClient(cl, "Bad user info");
		return;
	}

	// call game code to allow overrides
	svs.game->ClientUserInfoChanged(cl->entity, cl->user_info);

	// name for C code, mask off high bit
	g_strlcpy(cl->name, GetUserInfo(cl->user_info, "name"), sizeof(cl->name));
	for (i = 0; i < sizeof(cl->name); i++) {
		cl->name[i] &= 127;
	}

	// rate command
	val = GetUserInfo(cl->user_info, "rate");
	if (*val != '\0') {
		cl->rate = atoi(val);

		if (cl->rate > CLIENT_RATE_MAX)
			cl->rate = CLIENT_RATE_MAX;
		else if (cl->rate < CLIENT_RATE_MIN)
			cl->rate = CLIENT_RATE_MIN;
	}

	// limit the print messages the client receives
	val = GetUserInfo(cl->user_info, "message_level");
	if (*val != '\0') {
		cl->message_level = atoi(val);
	}
}

/*
 * @brief
 */
void Sv_Frame(const uint32_t msec) {

	// if server is not active, do nothing
	if (!svs.initialized)
		return;

	// update time reference
	svs.real_time += msec;

	// check timeouts
	Sv_CheckTimeouts();

	// get packets from clients
	Sv_ReadPackets();

	const uint32_t frame_millis = 1000 / svs.frame_rate;

	// keep the game module's time in sync with reality
	if (!time_demo->value && svs.real_time < sv.time) {

		// if the server has fallen far behind the game, try to catch up
		if (sv.time - svs.real_time > frame_millis) {
			Com_Debug("Sv_Frame: Low clamp: %dms.\n", (sv.time - svs.real_time - frame_millis));
			svs.real_time = sv.time - frame_millis;
		} else { // wait until its time to run the next frame
			Net_Sleep(sv.time - svs.real_time);
			return;
		}
	}

	// update ping based on the last known frame from all clients
	Sv_UpdatePings();

	// give the clients some timeslices
	Sv_CheckCommandTimes();

	// let everything in the world think and move
	Sv_RunGameFrame();

	// send messages back to the clients that had packets read this frame
	Sv_SendClientPackets();

	// send a heartbeat to the master if needed
	Sv_HeartbeatMasters();

	// clear entity flags, etc for next frame
	Sv_ResetEntities();

#if HAVE_CURSES
	Curses_Frame(msec);
#endif
}

/*
 * @brief
 */
static void Sv_InitLocal(void) {

	sv_rcon_password = Cvar_Get("rcon_password", "", 0, NULL);

	sv_download_url = Cvar_Get("sv_download_url", "", CVAR_SERVER_INFO, NULL);
	sv_enforce_time = Cvar_Get("sv_enforce_time", va("%d", CMD_MSEC_MAX_DRIFT_ERRORS), 0, NULL);

	sv_hostname = Cvar_Get("sv_hostname", "Quake2World", CVAR_SERVER_INFO | CVAR_ARCHIVE, NULL);
	sv_hz = Cvar_Get("sv_hz", va("%d", SV_HZ), CVAR_SERVER_INFO | CVAR_LATCH, NULL);

	sv_no_areas = Cvar_Get("sv_no_areas", "0", CVAR_LATCH, "Disable server-side area management\n");

	sv_public = Cvar_Get("sv_public", "0", 0, "Set to 1 to to advertise to the master server\n");

	if (dedicated->value)
		sv_max_clients = Cvar_Get("sv_max_clients", "8", CVAR_SERVER_INFO | CVAR_LATCH, NULL);
	else
		sv_max_clients = Cvar_Get("sv_max_clients", "1", CVAR_SERVER_INFO | CVAR_LATCH, NULL);

	sv_timeout = Cvar_Get("sv_timeout", va("%d", SV_TIMEOUT), 0, NULL);
	sv_udp_download = Cvar_Get("sv_udp_download", "1", CVAR_ARCHIVE, NULL);

	// set this so clients and server browsers can see it
	Cvar_Get("sv_protocol", va("%i", PROTOCOL_MAJOR), CVAR_SERVER_INFO | CVAR_NO_SET, NULL);
}

/*
 * @brief Only called at Quake2World startup, not for each game.
 */
void Sv_Init(void) {

	memset(&svs, 0, sizeof(svs));

	Cm_LoadBspModel(NULL, NULL);

	Sv_InitLocal();

	Sv_InitAdmin();

	Sv_InitMasters();

	net_message.size = 0;

	Net_Config(NS_UDP_SERVER, true);
}

/*
 * @brief Called when server is shutting down due to error or an explicit `quit`.
 */
void Sv_Shutdown(const char *msg) {

	Sv_ShutdownServer(msg);

	Sv_ShutdownMasters();

	Net_Config(NS_UDP_SERVER, false);

	net_message.size = 0;

	memset(&svs, 0, sizeof(svs));

	Cmd_RemoveAll(CMD_SERVER);

	Mem_FreeTag(MEM_TAG_SERVER);
}
