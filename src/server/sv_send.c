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

char sv_outputbuf[SV_OUTPUTBUF_LENGTH];

/*
 * @brief Handles Com_Print output redirection, allowing the server to send output
 * from any command to a connected client or even a foreign one.
 */
void Sv_FlushRedirect(int32_t target, const char *buffer) {

	switch (target) {
		case RD_PACKET:
			Netchan_OutOfBandPrint(NS_UDP_SERVER, &net_from, "print\n%s", buffer);
			break;
		case RD_CLIENT:
			Net_WriteByte(&sv_client->net_chan.message, SV_CMD_PRINT);
			Net_WriteByte(&sv_client->net_chan.message, PRINT_HIGH);
			Net_WriteString(&sv_client->net_chan.message, buffer);
			break;
		default:
			Com_Debug("Sv_FlushRedirect: %d\n", target);
			break;
	}
}

/*
 * @brief Sends text across to be displayed if the level filter passes.
 */
void Sv_ClientPrint(const g_entity_t *ent, const int32_t level, const char *fmt, ...) {
	sv_client_t * cl;
	va_list args;
	char string[MAX_STRING_CHARS];
	int32_t n;

	n = NUM_FOR_ENTITY(ent);
	if (n < 1 || n > sv_max_clients->integer) {
		Com_Warn("Issued to non-client %d\n", n);
		return;
	}

	cl = &svs.clients[n - 1];

	if (cl->state != SV_CLIENT_ACTIVE) {
		Com_Debug("Issued to unspawned client\n");
		return;
	}

	if (level < cl->message_level) {
		Com_Debug("Filtered by message level\n");
		return;
	}

	va_start(args, fmt);
	vsprintf(string, fmt, args);
	va_end(args);

	Net_WriteByte(&cl->net_chan.message, SV_CMD_PRINT);
	Net_WriteByte(&cl->net_chan.message, level);
	Net_WriteString(&cl->net_chan.message, string);
}

/*
 * @brief Sends text to all active clients over their unreliable channels.
 */
void Sv_BroadcastPrint(const int32_t level, const char *fmt, ...) {
	char string[MAX_STRING_CHARS];
	va_list args;
	sv_client_t * cl;
	int32_t i;

	va_start(args, fmt);
	vsprintf(string, fmt, args);
	va_end(args);

	// echo to console
	if (dedicated->value) {
		char copy[MAX_STRING_CHARS];
		int32_t j;

		// mask off high bits
		for (j = 0; j < MAX_STRING_CHARS - 1 && string[j]; j++)
			copy[j] = string[j] & 127;
		copy[j] = 0;
		Com_Print("%s", copy);
	}

	for (i = 0, cl = svs.clients; i < sv_max_clients->integer; i++, cl++) {

		if (level < cl->message_level)
			continue;

		if (cl->state != SV_CLIENT_ACTIVE)
			continue;

		Net_WriteByte(&cl->net_chan.message, SV_CMD_PRINT);
		Net_WriteByte(&cl->net_chan.message, level);
		Net_WriteString(&cl->net_chan.message, string);
	}
}

/*
 * @brief Sends text to all active clients
 */
void Sv_BroadcastCommand(const char *fmt, ...) {
	char string[MAX_STRING_CHARS];
	va_list args;

	if (!sv.state)
		return;

	va_start(args, fmt);
	vsprintf(string, fmt, args);
	va_end(args);

	Net_WriteByte(&sv.multicast, SV_CMD_CBUF_TEXT);
	Net_WriteString(&sv.multicast, string);
	Sv_Multicast(NULL, MULTICAST_ALL_R);
}

/*
 * @brief Writes to the specified datagram, noting the offset of the message.
 */
static void Sv_ClientDatagramMessage(sv_client_t *cl, byte *data, size_t len) {

	if (len > MAX_MSG_SIZE) {
		Com_Error(ERR_DROP, "Single datagram message exceeded MAX_MSG_LEN\n");
	}

	sv_client_message_t *msg = Mem_Malloc(sizeof(*msg));

	msg->offset = cl->datagram.buffer.size;
	msg->len = len;

	Mem_WriteBuffer(&cl->datagram.buffer, data, len);

	if (cl->datagram.buffer.overflowed) {
		Com_Warn("Client datagram overflow for %s\n", cl->name);

		msg->offset = 0;
		cl->datagram.buffer.overflowed = false;

		g_list_free_full(cl->datagram.messages, Mem_Free);
		cl->datagram.messages = NULL;
	}

	cl->datagram.messages = g_list_append(cl->datagram.messages, msg);
}

/*
 * @brief Sends the contents of the mutlicast buffer to a single client
 */
void Sv_Unicast(const g_entity_t *ent, const bool reliable) {

	if (ent && !ent->ai) {

		const uint16_t n = NUM_FOR_ENTITY(ent);
		if (n < 1 || n > sv_max_clients->integer) {
			Com_Warn("Non-client: %s\n", etos(ent));
			return;
		}

		sv_client_t *cl = svs.clients + (n - 1);

		if (reliable) {
			Mem_WriteBuffer(&cl->net_chan.message, sv.multicast.data, sv.multicast.size);
		} else {
			Sv_ClientDatagramMessage(cl, sv.multicast.data, sv.multicast.size);
		}
	}

	Mem_ClearBuffer(&sv.multicast);
}

/*
 * @brief Sends the contents of sv.multicast to a subset of the clients,
 * then clears sv.multicast.
 *
 * MULTICAST_ALL	same as broadcast (origin can be NULL)
 * MULTICAST_PVS	send to clients potentially visible from org
 * MULTICAST_PHS	send to clients potentially hearable from org
 */
void Sv_Multicast(const vec3_t origin, multicast_t to) {
	int32_t leaf_num, cluster;
	int32_t area1, area2;
	byte *vis;

	bool reliable = false;

	if (to != MULTICAST_ALL_R && to != MULTICAST_ALL) {
		leaf_num = Cm_PointLeafnum(origin, 0);
		area1 = Cm_LeafArea(leaf_num);
	} else {
		leaf_num = 0; // just to avoid compiler warnings
		area1 = 0;
	}

	switch (to) {
		case MULTICAST_ALL_R:
			reliable = true;
			/* no break */
		case MULTICAST_ALL:
			leaf_num = 0;
			vis = NULL;
			break;

		case MULTICAST_PHS_R:
			reliable = true;
			/* no break */
		case MULTICAST_PHS:
			leaf_num = Cm_PointLeafnum(origin, 0);
			cluster = Cm_LeafCluster(leaf_num);
			vis = Cm_ClusterPHS(cluster);
			break;

		case MULTICAST_PVS_R:
			reliable = true;
			/* no break */
		case MULTICAST_PVS:
			leaf_num = Cm_PointLeafnum(origin, 0);
			cluster = Cm_LeafCluster(leaf_num);
			vis = Cm_ClusterPVS(cluster);
			break;

		default:
			Com_Warn("Bad multicast: %i\n", to);
			Mem_ClearBuffer(&sv.multicast);
			return;
	}

	// send the data to all relevant clients
	sv_client_t *cl = svs.clients;
	for (int32_t j = 0; j < sv_max_clients->integer; j++, cl++) {

		if (cl->state == SV_CLIENT_FREE)
			continue;

		if (cl->state != SV_CLIENT_ACTIVE && !reliable)
			continue;

		if (cl->entity->ai)
			continue;

		if (vis) {
			const pm_state_t *pm = &cl->entity->client->ps.pm_state;
			vec3_t org, off;

#ifdef PMOVE_PRECISE
			VectorCopy(pm->origin, org);
#else
			UnpackVector(pm->origin, org);
#endif
			UnpackVector(pm->view_offset, off);
			VectorAdd(org, off, org);

			leaf_num = Cm_PointLeafnum(org, 0);
			cluster = Cm_LeafCluster(leaf_num);
			area2 = Cm_LeafArea(leaf_num);

			if (!Cm_AreasConnected(area1, area2))
				continue;

			if (vis && (!(vis[cluster >> 3] & (1 << (cluster & 7)))))
				continue;
		}

		if (reliable) {
			Mem_WriteBuffer(&cl->net_chan.message, sv.multicast.data, sv.multicast.size);
		} else {
			Sv_ClientDatagramMessage(cl, sv.multicast.data, sv.multicast.size);
		}
	}

	Mem_ClearBuffer(&sv.multicast);
}

/*
 * @brief An attenuation of 0 will play full volume everywhere in the level.
 * Larger attenuation will drop off (max 4 attenuation).
 *
 * If origin is NULL, the origin is determined from the entity origin
 * or the midpoint of the entity box for BSP sub-models.
 */
void Sv_PositionedSound(const vec3_t origin, const g_entity_t *entity, const uint16_t index,
		const uint16_t atten) {
	uint32_t flags;
	uint16_t at;
	int32_t i;
	vec3_t org;

	flags = 0;

	at = atten;
	if (at > ATTEN_STATIC) {
		Com_Warn("Bad attenuation %d\n", at);
		at = ATTEN_DEFAULT;
	}

	if (at != ATTEN_DEFAULT)
		flags |= S_ATTEN;

	// the client doesn't know that bsp models have weird origins
	// the origin can also be explicitly set
	if ((entity->sv_flags & SVF_NO_CLIENT) || (entity->solid == SOLID_BSP) || origin)
		flags |= S_ORIGIN;

	if (!(entity->sv_flags & SVF_NO_CLIENT) && NUM_FOR_ENTITY(entity))
		flags |= S_ENTNUM;

	// use the entity origin unless it is a bsp model or explicitly specified
	if (origin)
		VectorCopy(origin, org);
	else {
		if (entity->solid == SOLID_BSP) {
			for (i = 0; i < 3; i++)
				org[i] = entity->s.origin[i] + 0.5 * (entity->mins[i] + entity->maxs[i]);
		} else {
			VectorCopy(entity->s.origin, org);
		}
	}

	Net_WriteByte(&sv.multicast, SV_CMD_SOUND);
	Net_WriteByte(&sv.multicast, flags);
	Net_WriteByte(&sv.multicast, index);

	if (flags & S_ATTEN)
		Net_WriteByte(&sv.multicast, at);

	if (flags & S_ENTNUM)
		Net_WriteShort(&sv.multicast, NUM_FOR_ENTITY(entity));

	if (flags & S_ORIGIN)
		Net_WritePosition(&sv.multicast, org);

	if (atten != ATTEN_NONE)
		Sv_Multicast(org, MULTICAST_PHS);
	else
		Sv_Multicast(org, MULTICAST_ALL);
}

/*
 *
 * FRAME UPDATES
 *
 */

/*
 * @brief
 */
static void Sv_SendClientDatagram(sv_client_t *cl) {
	byte buffer[MAX_MSG_SIZE];
	mem_buf_t buf;

	Sv_BuildClientFrame(cl);

	Mem_InitBuffer(&buf, buffer, sizeof(buffer));
	buf.allow_overflow = true;

	// send over all the relevant entity_state_t and the player_state_t
	Sv_WriteClientFrame(cl, &buf);

	// the frame itself must not exceed the max message size
	if (buf.overflowed || buf.size > MAX_MSG_SIZE - 16) {
		Com_Error(ERR_DROP, "Frame exceeds MAX_MSG_SIZE (%u)\n", (uint32_t) buf.size);
	}

	// but we can packetize the remaining datagram messages
	const GList *e = cl->datagram.messages;
	while (e) {
		const sv_client_message_t *msg = (sv_client_message_t *) e->data;

		// if we would overflow the packet, flush it first
		if (buf.size + msg->len > (MAX_MSG_SIZE - 16)) {
			Com_Debug("Fragmenting datagram @ %u bytes\n", (uint32_t) buf.size);

			Netchan_Transmit(&cl->net_chan, buf.data, buf.size);
			Mem_ClearBuffer(&buf);
		}

		Mem_WriteBuffer(&buf, cl->datagram.buffer.data + msg->offset, msg->len);
		e = e->next;
	}

	// send the pending packet, which may include reliable messages
	Netchan_Transmit(&cl->net_chan, buf.data, buf.size);

	// record the total size for rate estimation
	cl->message_size[sv.frame_num % sv_hz->integer] = cl->datagram.buffer.size;
}

/*
 * @brief
 */
static void Sv_DemoCompleted(void) {
	Sv_ShutdownServer("Demo complete\n");
}

/*
 * @brief Returns true if the client is over its current bandwidth estimation
 * and should not be sent another packet.
 */
static bool Sv_RateDrop(sv_client_t *cl) {

	// never drop over the loop device
	if (cl->net_chan.remote_address.type == NA_LOOP) {
		return false;
	}

	uint32_t total = 0;

	for (int32_t i = 0; i < sv_hz->integer; i++) {
		total += cl->message_size[i];
	}

	if (total > cl->rate) {
		cl->surpress_count++;
		cl->message_size[sv.frame_num % sv_hz->integer] = 0;
		return true;
	}

	return false;
}

/*
 * @brief Reads the next frame from the current demo file into the specified buffer,
 * returning the size of the frame in bytes.
 *
 * FIXME This doesn't work with the new packetized overflow avoidance. Multiple
 * messages can constitute a frame. We need a mechanism to indicate frame
 * completion, or we need a timecode in our demos.
 */
static size_t Sv_GetDemoMessage(byte *buffer) {
	int32_t size;
	size_t r;

	r = Fs_Read(sv.demo_file, &size, sizeof(size), 1);

	if (r != 1) { // improperly terminated demo file
		Com_Warn("Failed to read demo file\n");
		Sv_DemoCompleted();
		return 0;
	}

	size = LittleLong(size);

	if (size == -1) { // properly terminated demo file
		Sv_DemoCompleted();
		return 0;
	}

	if (size > MAX_MSG_SIZE) { // corrupt demo file
		Com_Warn("%d > MAX_MSG_SIZE\n", size);
		Sv_DemoCompleted();
		return 0;
	}

	r = Fs_Read(sv.demo_file, buffer, size, 1);

	if (r != 1) {
		Com_Warn("Incomplete or corrupt demo file\n");
		Sv_DemoCompleted();
		return 0;
	}

	return size;
}

/*
 * @brief Send the frame and all pending datagram messages since the last frame.
 */
void Sv_SendClientPackets(void) {
	sv_client_t * cl;
	int32_t i;

	if (!svs.initialized)
		return;

	// send a message to each connected client
	for (i = 0, cl = svs.clients; i < sv_max_clients->integer; i++, cl++) {

		if (cl->state == SV_CLIENT_FREE) // don't bother
			continue;

		// if the client's reliable message overflowed, we must drop them
		if (cl->net_chan.message.overflowed) {
			Sv_DropClient(cl);
			Sv_BroadcastPrint(PRINT_HIGH, "%s overflowed\n", cl->name);
			continue;
		}

		if (sv.state == SV_ACTIVE_DEMO) { // send the demo packet
			byte buffer[MAX_MSG_SIZE];
			size_t size;

			if ((size = Sv_GetDemoMessage(buffer))) {
				Netchan_Transmit(&cl->net_chan, buffer, size);
			}
		} else if (cl->state == SV_CLIENT_ACTIVE) { // send the game packet

			if (!Sv_RateDrop(cl)) { // enforce rate throttle
				Sv_SendClientDatagram(cl);
			}

			// clean up for the next frame
			Mem_ClearBuffer(&cl->datagram.buffer);

			if (cl->datagram.messages) {
				g_list_free_full(cl->datagram.messages, Mem_Free);
			}

			cl->datagram.messages = NULL;

		} else { // just update reliable if needed
			if (cl->net_chan.message.size || quake2world.time - cl->net_chan.last_sent > 1000)
				Netchan_Transmit(&cl->net_chan, NULL, 0);
		}
	}
}

