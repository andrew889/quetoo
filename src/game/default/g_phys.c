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

#include "g_local.h"

/*
 * pushmove objects do not obey gravity, and do not interact with
 * each other or trigger fields, but block normal movement and push
 * normal objects when they move.
 *
 * onground is set for toss objects when they come to a complete
 * rest.  it is set for steping or walking objects
 *
 * doors, plats, etc are SOLID_BSP, and MOVETYPE_PUSH
 * bonus items are SOLID_TRIGGER touch, and MOVETYPE_TOSS
 * crates are SOLID_BBOX and MOVETYPE_TOSS
 *
 * solid_edge items only clip against bsp models.
 */


/*
 * G_TestEntityPosition
 */
static edict_t *G_TestEntityPosition(edict_t *ent){
	trace_t trace;
	int mask;

	if(ent->clipmask)
		mask = ent->clipmask;
	else
		mask = MASK_SOLID;
	trace = gi.Trace(ent->s.origin, ent->mins, ent->maxs, ent->s.origin, ent, mask);

	if(trace.startsolid)
		return g_edicts;

	return NULL;
}


#define MAX_VELOCITY 2000

/*
 * G_CheckVelocity
 */
static void G_CheckVelocity(edict_t *ent){
	int i;

	// bound velocity
	for(i = 0; i < 3; i++){
		if(ent->velocity[i] > MAX_VELOCITY)
			ent->velocity[i] = MAX_VELOCITY;
		else if(ent->velocity[i] < -MAX_VELOCITY)
			ent->velocity[i] = -MAX_VELOCITY;
	}
}

/*
 * G_RunThink
 *
 * Runs thinking code for this frame if necessary
 */
static qboolean G_RunThink(edict_t *ent){
	float thinktime;

	thinktime = ent->nextthink;

	if(thinktime <= 0)
		return true;

	if(thinktime > level.time + 0.001)
		return true;

	ent->nextthink = 0;

	if(!ent->think)
		gi.Error("G_RunThink: No think function for ent.");
	ent->think(ent);

	return false;
}

/*
 * G_Impact
 *
 * Two entities have touched, so run their touch functions
 */
static void G_Impact(edict_t *e1, trace_t *trace){
	edict_t *e2;

	e2 = trace->ent;

	if(e1->touch && e1->solid != SOLID_NOT)
		e1->touch(e1, e2, &trace->plane, trace->surface);

	if(e2->touch && e2->solid != SOLID_NOT)
		e2->touch(e2, e1, NULL, NULL);
}


#define STOP_EPSILON	0.1

/*
 * ClipVelocity
 *
 * Slide off of the impacting object
 * returns the blocked flags (1 = floor, 2 = step / wall)
 */
static int ClipVelocity(vec3_t in, vec3_t normal, vec3_t out, float overbounce){
	float backoff;
	float change;
	int i, blocked;

	blocked = 0;
	if(normal[2] > 0)
		blocked |= 1;  // floor
	if(!normal[2])
		blocked |= 2;  // step

	backoff = DotProduct(in, normal) * overbounce;

	for(i = 0; i < 3; i++){
		change = normal[i] * backoff;
		out[i] = in[i] - change;
		if(out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0.0;
	}

	return blocked;
}


/*
 * G_AddGravity
 */
static void G_AddGravity(edict_t *ent){
	ent->velocity[2] -= ent->gravity * level.gravity * gi.serverframe;
}


/*
 *
 * PUSHMOVE
 *
 */


/*
 * G_PushEntity
 *
 * Does not change the entity's velocity at all
 */
trace_t G_PushEntity(edict_t *ent, vec3_t push){
	trace_t trace;
	vec3_t start;
	vec3_t end;
	int mask;

	VectorCopy(ent->s.origin, start);
	VectorAdd(start, push, end);

retry:
	if(ent->clipmask)
		mask = ent->clipmask;
	else
		mask = MASK_SOLID;

	trace = gi.Trace(start, ent->mins, ent->maxs, end, ent, mask);

	VectorCopy(trace.endpos, ent->s.origin);
	gi.LinkEntity(ent);

	if(trace.fraction != 1.0){
		G_Impact(ent, &trace);

		// if the pushed entity went away and the pusher is still there
		if(!trace.ent->inuse && ent->inuse){
			// move the pusher back and try again
			VectorCopy(start, ent->s.origin);
			gi.LinkEntity(ent);
			goto retry;
		}
	}

	if(ent->inuse && ent->client && ent->health > 0)
		G_TouchTriggers(ent);

	return trace;
}


typedef struct {
	edict_t *ent;
	vec3_t origin;
	vec3_t angles;
	float deltayaw;
} pushed_t;

pushed_t pushed[MAX_EDICTS], *pushed_p;

edict_t *obstacle;

/*
 * G_Push
 *
 * Objects need to be moved back on a failed push,
 * otherwise riders would continue to slide.
 */
static qboolean G_Push(edict_t *pusher, vec3_t move, vec3_t amove){
	int i, e;
	edict_t *check, *block;
	vec3_t mins, maxs;
	pushed_t *p;
	vec3_t org, org2, move2, forward, right, up;

	// clamp the move to 1/8 units, so the position will
	// be accurate for client side prediction
	for(i = 0; i < 3; i++){
		float temp;
		temp = move[i] * 8.0;
		if(temp > 0.0)
			temp += 0.5;
		else
			temp -= 0.5;
		move[i] = 0.125 * (int)temp;
	}

	// find the bounding box
	for(i = 0; i < 3; i++){
		mins[i] = pusher->absmin[i] + move[i];
		maxs[i] = pusher->absmax[i] + move[i];
	}

	// we need this for pushing things later
	VectorSubtract(vec3_origin, amove, org);
	AngleVectors(org, forward, right, up);

	// save the pusher's original position
	pushed_p->ent = pusher;
	VectorCopy(pusher->s.origin, pushed_p->origin);
	VectorCopy(pusher->s.angles, pushed_p->angles);
	if(pusher->client)
		pushed_p->deltayaw = pusher->client->ps.pmove.delta_angles[YAW];
	pushed_p++;

	// move the pusher to it's final position
	VectorAdd(pusher->s.origin, move, pusher->s.origin);
	VectorAdd(pusher->s.angles, amove, pusher->s.angles);
	gi.LinkEntity(pusher);

	// see if any solid entities are inside the final position
	check = g_edicts + 1;
	for(e = 1; e < globals.num_edicts; e++, check++){

		if(!check->inuse)
			continue;

		if(check->movetype == MOVETYPE_PUSH
				|| check->movetype == MOVETYPE_STOP
				|| check->movetype == MOVETYPE_NONE
				|| check->movetype == MOVETYPE_NOCLIP)
			continue;

		if(!check->area.prev)
			continue;  // not linked in anywhere

		// if the entity is standing on the pusher, it will definitely be moved
		if(check->groundentity != pusher){

			// do not push entities which are beside us
			if(check->item)
				continue;

			// see if the ent needs to be tested
			if(check->absmin[0] >= maxs[0]
					|| check->absmin[1] >= maxs[1]
					|| check->absmin[2] >= maxs[2]
					|| check->absmax[0] <= mins[0]
					|| check->absmax[1] <= mins[1]
					|| check->absmax[2] <= mins[2])
				continue;

			// see if the ent's bbox is inside the pusher's final position
			if(!G_TestEntityPosition(check))
				continue;
		}

		if((pusher->movetype == MOVETYPE_PUSH) || (check->groundentity == pusher)){
			// move this entity
			pushed_p->ent = check;
			VectorCopy(check->s.origin, pushed_p->origin);
			VectorCopy(check->s.angles, pushed_p->angles);
			pushed_p++;

			// try moving the contacted entity
			VectorAdd(check->s.origin, move, check->s.origin);
			if(check->client){  // disable stair prediction
				check->client->ps.pmove.pm_flags |= PMF_PUSHED;
				check->client->ps.pmove.delta_angles[YAW] += amove[YAW];
			}

			// figure movement due to the pusher's move
			VectorSubtract(check->s.origin, pusher->s.origin, org);
			org2[0] = DotProduct(org, forward);
			org2[1] = -DotProduct(org, right);
			org2[2] = DotProduct(org, up);
			VectorSubtract(org2, org, move2);
			VectorAdd(check->s.origin, move2, check->s.origin);

			// may have pushed them off an edge
			if(check->groundentity != pusher)
				check->groundentity = NULL;

			block = G_TestEntityPosition(check);
			if(!block){  // pushed okay
				gi.LinkEntity(check);
				continue;
			}

			// if it is okay to leave in the old position, do it
			// this is only relevant for riding entities, not pushed
			// FIXME: this doesn't account for rotation
			VectorSubtract(check->s.origin, move, check->s.origin);
			block = G_TestEntityPosition(check);
			if(!block){
				pushed_p--;
				continue;
			}
		}

		// save off the obstacle so we can call the block function
		obstacle = check;

		// move back any entities we already moved
		// go backwards, so if the same entity was pushed
		// twice, it goes back to the original position
		for(p = pushed_p - 1; p >= pushed; p--){
			VectorCopy(p->origin, p->ent->s.origin);
			VectorCopy(p->angles, p->ent->s.angles);
			if(p->ent->client){
				p->ent->client->ps.pmove.delta_angles[YAW] = p->deltayaw;
			}
			gi.LinkEntity(p->ent);
		}
		return false;
	}

	//FIXME: is there a better way to handle this?
	// see if anything we moved has touched a trigger
	for(p = pushed_p - 1; p >= pushed; p--){
		if(p->ent->inuse && p->ent->client && p->ent->health > 0)
			G_TouchTriggers(p->ent);
	}

	return true;
}


/*
 * G_Physics_Pusher
 *
 * Bmodel objects don't interact with each other, but push all box objects
 */
static void G_Physics_Pusher(edict_t *ent){
	vec3_t move, amove;
	edict_t *part, *mv;

	// if not a team captain, so movement will be handled elsewhere
	if(ent->flags & FL_TEAMSLAVE)
		return;

	// make sure all team slaves can move before commiting
	// any moves or calling any think functions
	// if the move is blocked, all moved objects will be backed out
	//retry:
	pushed_p = pushed;
	for(part = ent; part; part = part->teamchain){
		if(part->velocity[0] || part->velocity[1] || part->velocity[2] ||
				part->avelocity[0] || part->avelocity[1] || part->avelocity[2]
		  ){  // object is moving
			VectorScale(part->velocity, gi.serverframe, move);
			VectorScale(part->avelocity, gi.serverframe, amove);

			if(!G_Push(part, move, amove))
				break;  // move was blocked
		}
	}

	if(pushed_p > &pushed[MAX_EDICTS])
		gi.Error("G_Physics_Pusher: MAX_EDICTS exceeded.");

	if(part){
		// the move failed, bump all nextthink times and back out moves
		for(mv = ent; mv; mv = mv->teamchain){
			if(mv->nextthink > 0)
				mv->nextthink += gi.serverframe;
		}

		// if the pusher has a "blocked" function, call it
		// otherwise, just stay in place until the obstacle is gone
		if(part->blocked)
			part->blocked(part, obstacle);

	} else {
		// the move succeeded, so call all think functions
		for(part = ent; part; part = part->teamchain){
			G_RunThink(part);
		}
	}
}


/*
 * G_Physics_None
 *
 * Non moving objects can only think
 */
static void G_Physics_None(edict_t *ent){
	// regular thinking
	G_RunThink(ent);
}


/*
 * G_Physics_Noclip
 *
 * A moving object that doesn't obey physics
 */
static void G_Physics_Noclip(edict_t *ent){

	if(!G_RunThink(ent))
		return;

	VectorMA(ent->s.angles, gi.serverframe, ent->avelocity, ent->s.angles);
	VectorMA(ent->s.origin, gi.serverframe, ent->velocity, ent->s.origin);

	gi.LinkEntity(ent);
}


/*
 * G_Physics_Toss
 *
 * Toss, bounce, and fly movement.  When on ground, do nothing.
 */
static void G_Physics_Toss(edict_t *ent){
	trace_t trace;
	vec3_t org, move;
	edict_t *slave;
	qboolean wasinwater;
	qboolean isinwater;

	// regular thinking
	G_RunThink(ent);

	// if not a team captain, so movement will be handled elsewhere
	if(ent->flags & FL_TEAMSLAVE)
		return;

	// check for the ground entity going away
	if(ent->groundentity){
		if(!ent->groundentity->inuse)
			ent->groundentity = NULL;
		else if(ent->velocity[2] > ent->groundentity->velocity[2] + 0.1)
			ent->groundentity = NULL;
		else return;
	}

	// if on ground, or intentionally floating, return without moving
	if(ent->groundentity || (ent->item && (ent->spawnflags & 4)))
		return;

	// enforce max velocity values
	G_CheckVelocity(ent);

	// add gravity
	if(ent->movetype != MOVETYPE_FLY)
		G_AddGravity(ent);

	// move angles
	VectorMA(ent->s.angles, gi.serverframe, ent->avelocity, ent->s.angles);

	// move origin
	VectorCopy(ent->s.origin, org);
	VectorScale(ent->velocity, gi.serverframe, move);

	// push through the world, interacting with triggers and other ents
	trace = G_PushEntity(ent, move);

	if(!ent->inuse)
		return;

	if(trace.fraction < 1.0){  // move was blocked

		// if it was a floor, we might bounce or come to rest
		if(ClipVelocity(ent->velocity, trace.plane.normal, ent->velocity, 1.3) == 1){

			VectorSubtract(ent->s.origin, org, move);

			// if we're approaching a stop, clear our velocity and set ground
			if(VectorLength(move) < STOP_EPSILON) {

				VectorClear(ent->velocity);

				ent->groundentity = trace.ent;
				ent->groundentity_linkcount = trace.ent->linkcount;
			}
			else {
				// bounce and slide along the floor
				float bounce, speed = VectorLength(ent->velocity);
				bounce = sqrt(speed);

				if(ent->velocity[2] < bounce)
					ent->velocity[2] = bounce;
			}
		}

		// all impacts reduce velocity and angular velocity
		VectorScale(ent->velocity, 0.9, ent->velocity);
		VectorScale(ent->avelocity, 0.9, ent->avelocity);
	}

	// check for water transition
	wasinwater = (ent->watertype & MASK_WATER);
	ent->watertype = gi.PointContents(ent->s.origin);
	isinwater = ent->watertype & MASK_WATER;

	if(isinwater)
		ent->waterlevel = 1;
	else
		ent->waterlevel = 0;

	if(!wasinwater && isinwater)
		gi.PositionedSound(ent->s.origin, g_edicts, gi.SoundIndex("world/water_in"), ATTN_NORM);
	else if(wasinwater && !isinwater)
		gi.PositionedSound(ent->s.origin, g_edicts, gi.SoundIndex("world/water_out"), ATTN_NORM);

	// move teamslaves
	for(slave = ent->teamchain; slave; slave = slave->teamchain){
		VectorCopy(ent->s.origin, slave->s.origin);
		gi.LinkEntity(slave);
	}
}


/*
 * G_RunEntity
 */
void G_RunEntity(edict_t *ent){
	if(ent->prethink)
		ent->prethink(ent);

	switch((int)ent->movetype){
		case MOVETYPE_PUSH:
		case MOVETYPE_STOP:
			G_Physics_Pusher(ent);
			break;
		case MOVETYPE_NONE:
			G_Physics_None(ent);
			break;
		case MOVETYPE_NOCLIP:
			G_Physics_Noclip(ent);
			break;
		case MOVETYPE_FLY:
		case MOVETYPE_TOSS:
			G_Physics_Toss(ent);
			break;
		default:
			gi.Error("G_RunEntity: Bad movetype %i.", (int)ent->movetype);
	}
}
