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

#ifndef __CG_ENTITY_TRAIL_H__
#define __CG_ENTITY_TRAIL_H__

#include "cg_types.h"

#ifdef __CG_LOCAL_H__
void Cg_BubbleTrail(const vec3_t start, const vec3_t end, vec_t density);
void Cg_TeleporterTrail(cl_entity_t *ent, const vec3_t org);
void Cg_SmokeTrail(cl_entity_t *ent, const vec3_t start, const vec3_t end);
void Cg_FlameTrail(cl_entity_t *ent, const vec3_t start, const vec3_t end);
void Cg_SteamTrail(cl_entity_t *ent, const vec3_t org, const vec3_t vel);
void Cg_EntityTrail(cl_entity_t *ent, r_entity_t *e);
#endif /* __CG_LOCAL_H__ */

#endif /* __CG_ENTITY_EFFECT_H__ */

