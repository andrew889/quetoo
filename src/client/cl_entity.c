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

/*
 * @return True if the delta is valid and interpolation should be used.
 */
static _Bool Cl_ValidDeltaPlayerState(player_state_t *from, player_state_t *to) {
	vec3_t delta;

#ifdef PMOVE_PRECISE
	VectorSubtract(from->pm_state.origin, to->pm_state.origin, delta);
#else
	vec3_t new_origin, old_origin;

	UnpackVector(from->pm_state.origin, old_origin);
	UnpackVector(to->pm_state.origin, new_origin);

	VectorSubtract(old_origin, new_origin, delta);
#endif

	if (VectorLength(delta) > 256.0)
		return false;

	return true;
}

/*
 * @brief Parse the player_state_t for the current frame from the server, using delta
 * compression for all fields where possible.
 */
static void Cl_ParsePlayerState(cl_frame_t *delta_frame, cl_frame_t *frame) {
	static player_state_t null_state;

	if (delta_frame) {
		if (Cl_ValidDeltaPlayerState(&delta_frame->ps, &frame->ps))
			Net_ReadDeltaPlayerState(&net_message, &delta_frame->ps, &frame->ps);
		else {
			Net_ReadDeltaPlayerState(&net_message, &null_state, &frame->ps);
			delta_frame->ps = frame->ps;
		}
	} else
		Net_ReadDeltaPlayerState(&net_message, &null_state, &frame->ps);

	if (cl.demo_server) // if playing a demo, force freeze
		frame->ps.pm_state.type = PM_FREEZE;
}

/*
 * @return True if the delta is valid and interpolation should be used.
 */
static _Bool Cl_ValidDeltaEntity(entity_state_t *from, entity_state_t *to) {
	vec3_t delta;

	if (from->model1 != to->model1)
		return false;

	VectorSubtract(from->origin, to->origin, delta);

	if (VectorLength(delta) > 256.0)
		return false;

	return true;
}

/*
 * @brief Reads deltas from the given base and adds the resulting entity to the
 * current frame.
 */
static void Cl_ReadDeltaEntity(cl_frame_t *frame, entity_state_t *from, uint16_t number,
		uint16_t bits) {

	cl_entity_t *ent = &cl.entities[number];

	entity_state_t *to = &cl.entity_states[cl.entity_state & ENTITY_STATE_MASK];
	cl.entity_state++;

	frame->num_entities++;

	Net_ReadDeltaEntity(&net_message, from, to, number, bits);

	// check to see if the delta was successful and valid
	if (ent->frame_num != cl.frame.frame_num - 1 || !Cl_ValidDeltaEntity(from, to)) {
		ent->prev = *to; // suppress interpolation
		VectorCopy(to->old_origin, ent->prev.origin);
		ent->animation1.time = ent->animation2.time = 0; // reset animations
		ent->lighting.state = LIGHTING_INIT; // and lighting
	} else { // shuffle the last state to previous
		ent->prev = ent->current;
	}

	// set the current frame number and entity state
	ent->frame_num = cl.frame.frame_num;
	ent->current = *to;
}

/*
 * @brief An svc_packetentities has just been parsed, deal with the rest of the data stream.
 */
static void Cl_ParseEntities(const cl_frame_t *delta_frame, cl_frame_t *frame) {
	entity_state_t *old_state = NULL;
	uint16_t old_number;

	frame->entity_state = cl.entity_state;
	frame->num_entities = 0;

	// delta from the entities present in old_frame
	uint32_t old_index = 0;

	if (!delta_frame)
		old_number = 0xffff;
	else {
		if (old_index >= delta_frame->num_entities)
			old_number = 0xffff;
		else {
			old_state = &cl.entity_states[(delta_frame->entity_state + old_index)
					& ENTITY_STATE_MASK];
			old_number = old_state->number;
		}
	}

	while (true) {
		const uint16_t number = Net_ReadShort(&net_message);

		if (number >= MAX_ENTITIES)
			Com_Error(ERR_DROP, "Bad number: %i\n", number);

		if (net_message.read > net_message.size)
			Com_Error(ERR_DROP, "End of message\n");

		if (!number)
			break;

		const uint16_t bits = Net_ReadShort(&net_message);

		while (old_number < number) { // one or more entities from old_frame are unchanged

			if (cl_show_net_messages->integer == 3)
				Com_Print("   unchanged: %i\n", old_number);

			Cl_ReadDeltaEntity(frame, old_state, old_number, 0);

			old_index++;

			if (old_index >= delta_frame->num_entities)
				old_number = 0xffff;
			else {
				old_state = &cl.entity_states[(delta_frame->entity_state + old_index)
						& ENTITY_STATE_MASK];
				old_number = old_state->number;
			}
		}

		if (bits & U_REMOVE) { // the entity present in the old_frame, and is not in the current frame

			if (cl_show_net_messages->integer == 3)
				Com_Print("   remove: %i\n", number);

			if (old_number != number)
				Com_Warn("U_REMOVE: %u != %u\n", old_number, number);

			old_index++;

			if (old_index >= delta_frame->num_entities)
				old_number = 0xffff;
			else {
				old_state = &cl.entity_states[(delta_frame->entity_state + old_index)
						& ENTITY_STATE_MASK];
				old_number = old_state->number;
			}
			continue;
		}

		if (old_number == number) { // delta from previous state

			if (cl_show_net_messages->integer == 3)
				Com_Print("   delta: %i\n", number);

			Cl_ReadDeltaEntity(frame, old_state, number, bits);

			old_index++;

			if (old_index >= delta_frame->num_entities)
				old_number = 0xffff;
			else {
				old_state = &cl.entity_states[(delta_frame->entity_state + old_index)
						& ENTITY_STATE_MASK];
				old_number = old_state->number;
			}
			continue;
		}

		if (old_number > number) { // delta from baseline

			if (cl_show_net_messages->integer == 3)
				Com_Print("   baseline: %i\n", number);

			Cl_ReadDeltaEntity(frame, &cl.entities[number].baseline, number, bits);
			continue;
		}
	}

	// any remaining entities in the old frame are copied over
	while (old_number != 0xffff) { // one or more entities from the old packet are unchanged

		if (cl_show_net_messages->integer == 3)
			Com_Print("   unchanged: %i\n", old_number);

		Cl_ReadDeltaEntity(frame, old_state, old_number, 0);

		old_index++;

		if (old_index >= delta_frame->num_entities)
			old_number = 0xffff;
		else {
			old_state = &cl.entity_states[(delta_frame->entity_state + old_index)
					& ENTITY_STATE_MASK];
			old_number = old_state->number;
		}
	}
}

/*
 * @brief
 */
void Cl_ParseFrame(void) {
	cl_frame_t *delta_frame;

	cl.frame.frame_num = Net_ReadLong(&net_message);

	cl.frame.delta_frame_num = Net_ReadLong(&net_message);

	cl.surpress_count = Net_ReadByte(&net_message);

	if (cl_show_net_messages->integer == 3)
		Com_Print("   frame:%i  delta:%i\n", cl.frame.frame_num, cl.frame.delta_frame_num);

	if (cl.frame.delta_frame_num <= 0) { // uncompressed frame
		delta_frame = NULL;
		cl.frame.valid = true;
	} else { // delta compressed frame
		delta_frame = &cl.frames[cl.frame.delta_frame_num & PACKET_MASK];

		if (!delta_frame->valid)
			Com_Error(ERR_DROP, "Delta from invalid frame\n");

		if (delta_frame->frame_num != cl.frame.delta_frame_num)
			Com_Error(ERR_DROP, "Delta frame too old\n");

		else if (cl.entity_state - delta_frame->entity_state > ENTITY_STATE_BACKUP - PACKET_BACKUP)
			Com_Error(ERR_DROP, "Delta parse_entities too old\n");

		cl.frame.valid = true;
	}

	const size_t len = Net_ReadByte(&net_message); // read area_bits
	Net_ReadData(&net_message, &cl.frame.area_bits, len);

	Cl_ParsePlayerState(delta_frame, &cl.frame);

	Cl_ParseEntities(delta_frame, &cl.frame);

	// set the simulation time for the frame
	cl.frame.time = cl.frame.frame_num * 1000 / cl.server_hz;

	// save the frame off in the backup array for later delta comparisons
	cl.frames[cl.frame.frame_num & PACKET_MASK] = cl.frame;

	if (cl.frame.valid) {
		// getting a valid frame message ends the connection process
		if (cls.state != CL_ACTIVE) {
			cls.state = CL_ACTIVE;

#ifdef PMOVE_PRECISE
			VectorCopy(cl.frame.ps.pm_state.origin, cl.predicted_state.origin);
#else
			UnpackVector(cl.frame.ps.pm_state.origin, cl.predicted_state.origin);
#endif

			UnpackVector(cl.frame.ps.pm_state.view_offset, cl.predicted_state.view_offset);
			UnpackAngles(cl.frame.ps.pm_state.view_angles, cl.predicted_state.view_angles);
		}

		Cl_CheckPredictionError();
	}
}

/*
 * @brief Interpolates translation and rotation for all entities within the
 * current frame. If the entity is at its most recently parsed orientation,
 * this is a no-op.
 */
void Cl_LerpEntities(void) {

	for (uint16_t i = 0; i < cl.frame.num_entities; i++) {

		const uint32_t snum = (cl.frame.entity_state + i) & ENTITY_STATE_MASK;
		cl_entity_t *ent = &cl.entities[cl.entity_states[snum].number];

		if (!VectorCompare(ent->origin, ent->current.origin)
				|| !VectorCompare(ent->angles, ent->current.angles)) {

			// mark the lighting as dirty
			ent->lighting.state = MIN(ent->lighting.state, LIGHTING_DIRTY);

			// interpolate the origin and angles
			VectorLerp(ent->prev.origin, ent->current.origin, cl.lerp, ent->origin);
			AngleLerp(ent->prev.angles, ent->current.angles, cl.lerp, ent->angles);

			if (ent->current.solid != SOLID_NOT) {
				// and for solids, update the clipping matrices
				const vec_t *angles = ent->current.solid == SOLID_BSP ? ent->angles : vec3_origin;

				Matrix4x4_CreateFromEntity(&ent->matrix, ent->origin, angles, 1.0);
				Matrix4x4_Invert_Simple(&ent->inverse_matrix, &ent->matrix);
			}
		}
	}
}

/*
 * @brief Invalidate lighting caches on media load.
 */
void Cl_UpdateEntities(void) {

	if (r_view.update) {
		uint16_t i;

		for (i = 0; i < lengthof(cl.entities); i++) {
			r_lighting_t *l = &cl.entities[i].lighting;

			memset(l, 0, sizeof(*l));

			l->state = LIGHTING_INIT;
			l->number = i;
		}
	}
}
