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

#include "r_local.h"

/*
 * @brief Resolves the skin for the specified model. By default, we simply load
 * "skin.tga" in the model's directory.
 */
static void R_LoadMeshMaterial(r_model_t *mod) {
	char skin[MAX_QPATH];

	Dirname(mod->media.name, skin);
	strcat(skin, "skin");

	mod->mesh->material = R_LoadMaterial(skin);
}

/*
 * @brief Parses animation.cfg, loading the frame specifications for the given model.
 */
static void R_LoadMd3Animations(r_model_t *mod) {
	r_md3_t *md3;
	char path[MAX_QPATH];
	const char *buffer, *c;
	void *buf;
	uint16_t skip;

	md3 = (r_md3_t *) mod->mesh->data;

	Dirname(mod->media.name, path);
	strcat(path, "animation.cfg");

	if (Fs_Load(path, &buf) == -1) {
		Com_Warn("No animation.cfg for %s\n", mod->media.name);
		return;
	}

	md3->animations = Mem_LinkMalloc(sizeof(r_md3_animation_t) * MD3_MAX_ANIMATIONS, mod->mesh);

	buffer = (char *) buf;
	skip = 0;

	while (true) {

		c = ParseToken(&buffer);

		if (*c == '\0')
			break;

		if (!g_strcmp0(c, "footsteps")) {
			ParseToken(&buffer);
			continue;
		}

		if (!g_strcmp0(c, "headoffset")) {
			ParseToken(&buffer);
			ParseToken(&buffer);
			ParseToken(&buffer);
			continue;
		}

		if (!g_strcmp0(c, "sex")) {
			ParseToken(&buffer);
			continue;
		}

		if (*c >= '0' && *c <= '9') {
			r_md3_animation_t *a = &md3->animations[md3->num_animations];

			a->first_frame = (uint16_t) strtoul(c, NULL, 0);
			c = ParseToken(&buffer);
			a->num_frames = (uint16_t) strtoul(c, NULL, 0);
			c = ParseToken(&buffer);
			a->looped_frames = (uint16_t) strtoul(c, NULL, 0);
			c = ParseToken(&buffer);
			a->hz = (uint16_t) strtoul(c, NULL, 0);

			if (md3->num_animations == ANIM_LEGS_WALKCR)
				skip = a->first_frame - md3->animations[ANIM_TORSO_GESTURE].first_frame;

			if (md3->num_animations >= ANIM_LEGS_WALKCR)
				a->first_frame -= skip;

			if (!a->num_frames)
				Com_Warn("%s: No frames for %d\n", mod->media.name, md3->num_animations);

			if (!a->hz)
				Com_Warn("%s: No hz for %d\n", mod->media.name, md3->num_animations);

			Com_Debug("Parsed %d: %d %d %d %d\n", md3->num_animations,
					a->first_frame, a->num_frames, a->looped_frames, a->hz);

			md3->num_animations++;
			if (md3->num_animations == MD3_MAX_ANIMATIONS) {
				Com_Warn("MD3_MAX_ANIMATIONS reached: %s\n", mod->media.name);
				break;
			}
		}
	}

	Fs_Free(buf);

	Com_Debug("Loaded %d animations: %s\n", md3->num_animations, mod->media.name);
}

/*
 * @brief Loads the specified r_mesh_config_t from the file at path.
 */
static void R_LoadMeshConfig(r_mesh_config_t *config, const char *path) {
	const char *buffer, *c;
	void *buf;

	if (Fs_Load(path, &buf) == -1)
		return;

	buffer = (char *) buf;

	while (true) {

		c = ParseToken(&buffer);

		if (*c == '\0')
			break;

		if (!g_strcmp0(c, "translate")) {
			sscanf(ParseToken(&buffer), "%f %f %f", &config->translate[0], &config->translate[1],
					&config->translate[2]);
			continue;
		}

		if (!g_strcmp0(c, "scale")) {
			sscanf(ParseToken(&buffer), "%f", &config->scale);
			continue;
		}

		if (!g_strcmp0(c, "alpha_test")) {
			config->flags |= EF_ALPHATEST;
			continue;
		}

		if (!g_strcmp0(c, "blend")) {
			config->flags |= EF_BLEND;
			continue;
		}
	}

	Fs_Free(buf);
}

/*
 * @brief Loads all r_mesh_config_t for the specified r_model_t. These allow
 * models to be positioned and scaled relative to their own origins, which is
 * useful because artists contribute models in almost arbitrary dimensions at
 * times.
 */
static void R_LoadMeshConfigs(r_model_t *mod) {
	char path[MAX_QPATH];

	mod->mesh->world_config = Mem_LinkMalloc(sizeof(r_mesh_config_t), mod->mesh);
	mod->mesh->view_config = Mem_LinkMalloc(sizeof(r_mesh_config_t), mod->mesh);
	mod->mesh->link_config = Mem_LinkMalloc(sizeof(r_mesh_config_t), mod->mesh);

	mod->mesh->world_config->scale = 1.0;

	Dirname(mod->media.name, path);

	R_LoadMeshConfig(mod->mesh->world_config, va("%sworld.cfg", path));

	// by default, additional configs inherit from world
	memcpy(mod->mesh->view_config, mod->mesh->world_config, sizeof(r_mesh_config_t));
	memcpy(mod->mesh->link_config, mod->mesh->world_config, sizeof(r_mesh_config_t));

	R_LoadMeshConfig(mod->mesh->view_config, va("%sview.cfg", path));
	R_LoadMeshConfig(mod->mesh->link_config, va("%slink.cfg", path));
}

/*
 * @brief Calculates tangent vectors for each MD3 vertex for per-pixel
 * lighting. See http://www.terathon.com/code/tangent.html.
 */
static void R_LoadMd3Tangents(r_md3_mesh_t *mesh) {
	vec3_t *tan1, *tan2;
	uint32_t *tri;
	int32_t i;

	tan1 = (vec3_t *) Mem_Malloc(mesh->num_verts * sizeof(vec3_t));
	tan2 = (vec3_t *) Mem_Malloc(mesh->num_verts * sizeof(vec3_t));

	tri = mesh->tris;

	// resolve the texture directional vectors

	for (i = 0; i < mesh->num_tris; i++, tri += 3) {
		vec3_t sdir, tdir;

		const uint32_t i1 = tri[0];
		const uint32_t i2 = tri[1];
		const uint32_t i3 = tri[2];

		const vec_t *v1 = mesh->verts[i1].point;
		const vec_t *v2 = mesh->verts[i2].point;
		const vec_t *v3 = mesh->verts[i3].point;

		const vec_t *w1 = mesh->coords[i1].st;
		const vec_t *w2 = mesh->coords[i2].st;
		const vec_t *w3 = mesh->coords[i3].st;

		vec_t x1 = v2[0] - v1[0];
		vec_t x2 = v3[0] - v1[0];
		vec_t y1 = v2[1] - v1[1];
		vec_t y2 = v3[1] - v1[1];
		vec_t z1 = v2[2] - v1[2];
		vec_t z2 = v3[2] - v1[2];

		vec_t s1 = w2[0] - w1[0];
		vec_t s2 = w3[0] - w1[0];
		vec_t t1 = w2[1] - w1[1];
		vec_t t2 = w3[1] - w1[1];

		vec_t r = 1.0 / (s1 * t2 - s2 * t1);

		VectorSet(sdir,
				(t2 * x1 - t1 * x2),
				(t2 * y1 - t1 * y2),
				(t2 * z1 - t1 * z2)
		);

		VectorScale(sdir, r, sdir);

		VectorSet(tdir,
				(s1 * x2 - s2 * x1),
				(s1 * y2 - s2 * y1),
				(s1 * z2 - s2 * z1)
		);

		VectorScale(tdir, r, tdir);

		VectorAdd(tan1[i1], sdir, tan1[i1]);
		VectorAdd(tan1[i2], sdir, tan1[i2]);
		VectorAdd(tan1[i3], sdir, tan1[i3]);

		VectorAdd(tan2[i1], tdir, tan2[i1]);
		VectorAdd(tan2[i2], tdir, tan2[i2]);
		VectorAdd(tan2[i3], tdir, tan2[i3]);
	}

	// calculate the tangents

	for (i = 0; i < mesh->num_verts; i++) {
		vec3_t bitangent;

		const vec_t *normal = mesh->verts[i].normal;
		vec_t *tangent = mesh->verts[i].tangent;

		TangentVectors(normal, tan1[i], tan2[i], tangent, bitangent);
	}

	Mem_Free(tan1);
	Mem_Free(tan2);
}

/*
 * @brief Loads and populates vertex array data for the specified MD3 model.
 */
static void R_LoadMd3VertexArrays(r_model_t *mod) {
	r_md3_t *md3;
	d_md3_frame_t *frame;
	r_md3_mesh_t *mesh;
	r_md3_vertex_t *v;
	d_md3_texcoord_t *texcoords;
	int32_t i, j, vert_index, tangent_index, texcoord_index;
	uint32_t *tri;

	R_AllocVertexArrays(mod); // allocate the arrays

	md3 = (r_md3_t *) mod->mesh->data;

	frame = md3->frames;

	vert_index = tangent_index = texcoord_index = 0;

	for (i = 0, mesh = md3->meshes; i < md3->num_meshes; i++, mesh++) { // iterate the meshes

		v = mesh->verts;

		if (mod->mesh->num_frames == 1) { // for static models, build the verts and normals
			for (j = 0; j < mesh->num_verts; j++, v++) {
				VectorAdd(frame->translate, v->point, r_mesh_state.vertexes[j]);
				VectorCopy(v->normal, r_mesh_state.normals[j]);
				Vector4Copy(v->tangent, r_mesh_state.tangents[j]);
			}
		}

		tri = mesh->tris;
		texcoords = mesh->coords;

		for (j = 0; j < mesh->num_tris; j++, tri += 3) { // populate the arrays

			if (mod->mesh->num_frames == 1) {
				VectorCopy(r_mesh_state.vertexes[tri[0]], (&mod->verts[vert_index + 0]));
				VectorCopy(r_mesh_state.vertexes[tri[1]], (&mod->verts[vert_index + 3]));
				VectorCopy(r_mesh_state.vertexes[tri[2]], (&mod->verts[vert_index + 6]));

				VectorCopy(r_mesh_state.normals[tri[0]], (&mod->normals[vert_index + 0]));
				VectorCopy(r_mesh_state.normals[tri[1]], (&mod->normals[vert_index + 3]));
				VectorCopy(r_mesh_state.normals[tri[2]], (&mod->normals[vert_index + 6]));

				Vector4Copy(r_mesh_state.tangents[tri[0]], (&mod->tangents[vert_index + 0]));
				Vector4Copy(r_mesh_state.tangents[tri[1]], (&mod->tangents[vert_index + 4]));
				Vector4Copy(r_mesh_state.tangents[tri[2]], (&mod->tangents[vert_index + 8]));
			}

			memcpy(&mod->texcoords[texcoord_index + 0], &texcoords[tri[0]], sizeof(vec2_t));
			memcpy(&mod->texcoords[texcoord_index + 2], &texcoords[tri[1]], sizeof(vec2_t));
			memcpy(&mod->texcoords[texcoord_index + 4], &texcoords[tri[2]], sizeof(vec2_t));

			vert_index += 9;
			tangent_index += 12;
			texcoord_index += 6;
		}
	}
}

/*
 * @brief Loads the d_md3_t contents of buffer to the specified model.
 */
void R_LoadMd3Model(r_model_t *mod, void *buffer) {
	int32_t i, j, l;
	d_md3_t *in_md3;
	r_md3_t *out_md3;
	d_md3_frame_t *in_frame, *out_frame;
	d_md3_tag_t *in_tag;
	r_md3_tag_t *out_tag;
	d_md3_orientation_t orient;
	d_md3_mesh_t *in_mesh;
	r_md3_mesh_t *out_mesh;
	d_md3_texcoord_t *in_coord, *out_coord;
	d_md3_vertex_t *in_vert;
	r_md3_vertex_t *out_vert;
	uint32_t *inindex, *out_index;
	vec_t lat, lng;
	size_t size;

	in_md3 = (d_md3_t *) buffer;

	const int32_t version = LittleLong(in_md3->version);
	if (version != MD3_VERSION) {
		Com_Error(ERR_DROP, "%s has wrong version number "
				"(%i should be %i)\n", mod->media.name, version, MD3_VERSION);
	}

	mod->mesh = Mem_LinkMalloc(sizeof(r_mesh_model_t), mod);
	mod->mesh->data = out_md3 = Mem_LinkMalloc(sizeof(r_md3_t), mod->mesh);

	// byte swap the header fields and sanity check
	in_md3->ofs_frames = LittleLong(in_md3->ofs_frames);
	in_md3->ofs_tags = LittleLong(in_md3->ofs_tags);
	in_md3->ofs_meshes = LittleLong(in_md3->ofs_meshes);

	mod->mesh->num_frames = out_md3->num_frames = LittleLong(in_md3->num_frames);
	out_md3->num_tags = LittleLong(in_md3->num_tags);
	out_md3->num_meshes = LittleLong(in_md3->num_meshes);

	if (out_md3->num_frames < 1) {
		Com_Error(ERR_DROP, "%s has no frames\n", mod->media.name);
	}

	if (out_md3->num_frames > MD3_MAX_FRAMES) {
		Com_Error(ERR_DROP, "%s has too many frames\n", mod->media.name);
	}

	if (out_md3->num_tags > MD3_MAX_TAGS) {
		Com_Error(ERR_DROP, "%s has too many tags\n", mod->media.name);
	}

	if (out_md3->num_meshes > MD3_MAX_MESHES) {
		Com_Error(ERR_DROP, "%s has too many meshes\n", mod->media.name);
	}

	// load the frames
	in_frame = (d_md3_frame_t *) ((byte *) in_md3 + in_md3->ofs_frames);
	size = out_md3->num_frames * sizeof(d_md3_frame_t);
	out_md3->frames = out_frame = Mem_LinkMalloc(size, mod->mesh);

	ClearBounds(mod->mins, mod->maxs);

	for (i = 0; i < out_md3->num_frames; i++, in_frame++, out_frame++) {
		for (j = 0; j < 3; j++) {
			out_frame->mins[j] = LittleFloat(in_frame->mins[j]);
			out_frame->maxs[j] = LittleFloat(in_frame->maxs[j]);
			out_frame->translate[j] = LittleFloat(in_frame->translate[j]);
		}

		AddPointToBounds(out_frame->mins, mod->mins, mod->maxs);
		AddPointToBounds(out_frame->maxs, mod->mins, mod->maxs);
	}

	// load the tags
	if (out_md3->num_tags) {

		in_tag = (d_md3_tag_t *) ((byte *) in_md3 + in_md3->ofs_tags);
		size = out_md3->num_tags * out_md3->num_frames * sizeof(r_md3_tag_t);
		out_md3->tags = out_tag = Mem_LinkMalloc(size, mod->mesh);

		for (i = 0; i < out_md3->num_frames; i++) {
			for (l = 0; l < out_md3->num_tags; l++, in_tag++, out_tag++) {
				memcpy(out_tag->name, in_tag->name, MD3_MAX_PATH);

				for (j = 0; j < 3; j++) {
					orient.origin[j] = LittleFloat(in_tag->orient.origin[j]);
					orient.axis[0][j] = LittleFloat(in_tag->orient.axis[0][j]);
					orient.axis[1][j] = LittleFloat(in_tag->orient.axis[1][j]);
					orient.axis[2][j] = LittleFloat(in_tag->orient.axis[2][j]);
				}

				Matrix4x4_FromVectors(&out_tag->matrix, orient.axis[0], orient.axis[1],
						orient.axis[2], orient.origin);
			}
		}
	}

	// load the meshes
	in_mesh = (d_md3_mesh_t *) ((byte *) in_md3 + in_md3->ofs_meshes);
	size = out_md3->num_meshes * sizeof(r_md3_mesh_t);
	out_md3->meshes = out_mesh = Mem_LinkMalloc(size, mod->mesh);

	for (i = 0; i < out_md3->num_meshes; i++, out_mesh++) {
		memcpy(out_mesh->name, in_mesh->name, MD3_MAX_PATH);

		in_mesh->ofs_tris = LittleLong(in_mesh->ofs_tris);
		in_mesh->ofs_skins = LittleLong(in_mesh->ofs_skins);
		in_mesh->ofs_tcs = LittleLong(in_mesh->ofs_tcs);
		in_mesh->ofs_verts = LittleLong(in_mesh->ofs_verts);
		in_mesh->size = LittleLong(in_mesh->size);

		out_mesh->flags = LittleLong(in_mesh->flags);
		out_mesh->num_skins = LittleLong(in_mesh->num_skins);
		out_mesh->num_tris = LittleLong(in_mesh->num_tris);
		out_mesh->num_verts = LittleLong(in_mesh->num_verts);

		if (out_mesh->num_skins > MD3_MAX_SHADERS) {
			Com_Error(ERR_DROP, "%s: %s has too many skins\n", mod->media.name, out_mesh->name);
		}

		if (out_mesh->num_tris > MD3_MAX_TRIANGLES) {
			Com_Error(ERR_DROP, "%s: %s has too many triangles\n", mod->media.name, out_mesh->name);
		}

		if (out_mesh->num_verts > MD3_MAX_VERTS) {
			Com_Error(ERR_DROP, "%s: %s has too many vertexes\n", mod->media.name, out_mesh->name);
		}

		// load the triangle indexes
		inindex = (uint32_t *) ((byte *) in_mesh + in_mesh->ofs_tris);
		size = out_mesh->num_tris * sizeof(uint32_t) * 3;
		out_mesh->tris = out_index = Mem_LinkMalloc(size, mod->mesh);

		for (j = 0; j < out_mesh->num_tris; j++, inindex += 3, out_index += 3) {
			out_index[0] = (uint32_t) LittleLong(inindex[0]);
			out_index[1] = (uint32_t) LittleLong(inindex[1]);
			out_index[2] = (uint32_t) LittleLong(inindex[2]);
		}

		// load the texcoords
		in_coord = (d_md3_texcoord_t *) ((byte *) in_mesh + in_mesh->ofs_tcs);
		size = out_mesh->num_verts * sizeof(d_md3_texcoord_t);
		out_mesh->coords = out_coord = Mem_LinkMalloc(size, mod->mesh);

		for (j = 0; j < out_mesh->num_verts; j++, in_coord++, out_coord++) {
			out_coord->st[0] = LittleFloat(in_coord->st[0]);
			out_coord->st[1] = LittleFloat(in_coord->st[1]);
		}

		// load the verts and norms
		in_vert = (d_md3_vertex_t *) ((byte *) in_mesh + in_mesh->ofs_verts);
		size = out_md3->num_frames * out_mesh->num_verts * sizeof(r_md3_vertex_t);
		out_mesh->verts = out_vert = Mem_LinkMalloc(size, mod->mesh);

		for (l = 0; l < out_md3->num_frames; l++) {
			for (j = 0; j < out_mesh->num_verts; j++, in_vert++, out_vert++) {
				out_vert->point[0] = LittleShort(in_vert->point[0]) * MD3_XYZ_SCALE;
				out_vert->point[1] = LittleShort(in_vert->point[1]) * MD3_XYZ_SCALE;
				out_vert->point[2] = LittleShort(in_vert->point[2]) * MD3_XYZ_SCALE;

				lat = (in_vert->norm >> 8) & 0xff;
				lng = (in_vert->norm & 0xff);

				lat *= M_PI / 128.0;
				lng *= M_PI / 128.0;

				out_vert->normal[0] = cos(lat) * sin(lng);
				out_vert->normal[1] = sin(lat) * sin(lng);
				out_vert->normal[2] = cos(lng);
			}
		}

		R_LoadMd3Tangents(out_mesh);

		Com_Debug("%s: %s: %d triangles\n", mod->media.name, out_mesh->name, out_mesh->num_tris);

		in_mesh = (d_md3_mesh_t *) ((byte *) in_mesh + in_mesh->size);
	}

	// load the skin for objects, and the animations for players
	if (!strstr(mod->media.name, "players/"))
		R_LoadMeshMaterial(mod);

	else if (strstr(mod->media.name, "/upper"))
		R_LoadMd3Animations(mod);

	// and the configs
	R_LoadMeshConfigs(mod);

	// and finally load the arrays
	R_LoadMd3VertexArrays(mod);

	Com_Debug("%s\n  %d meshes\n  %d frames\n  %d tags\n  %d vertexes\n", mod->media.name,
			out_md3->num_meshes, out_md3->num_frames, out_md3->num_tags, mod->num_verts);
}

/*
 * @brief http://www.terathon.com/code/tangent.html
 */
static void R_LoadObjModelTangents(r_obj_t *obj) {
	vec3_t *tan1, *tan2;
	r_obj_tri_t *tri;
	int32_t i, j;

	tan1 = (vec3_t *) Mem_Malloc(obj->num_verts * sizeof(vec3_t));
	tan2 = (vec3_t *) Mem_Malloc(obj->num_verts * sizeof(vec3_t));

	tri = obj->tris;

	// resolve the texture directional vectors

	for (i = 0; i < obj->num_tris; i++, tri++) {
		vec3_t sdir, tdir;

		const int32_t i1 = tri->verts[0].vert - 1;
		const int32_t i2 = tri->verts[1].vert - 1;
		const int32_t i3 = tri->verts[2].vert - 1;

		const vec_t *v1 = &obj->verts[i1 * 3];
		const vec_t *v2 = &obj->verts[i2 * 3];
		const vec_t *v3 = &obj->verts[i3 * 3];

		const int32_t j1 = tri->verts[0].texcoord - 1;
		const int32_t j2 = tri->verts[1].texcoord - 1;
		const int32_t j3 = tri->verts[2].texcoord - 1;

		const vec_t *w1 = &obj->texcoords[j1 * 2];
		const vec_t *w2 = &obj->texcoords[j2 * 2];
		const vec_t *w3 = &obj->texcoords[j3 * 2];

		vec_t x1 = v2[0] - v1[0];
		vec_t x2 = v3[0] - v1[0];
		vec_t y1 = v2[1] - v1[1];
		vec_t y2 = v3[1] - v1[1];
		vec_t z1 = v2[2] - v1[2];
		vec_t z2 = v3[2] - v1[2];

		vec_t s1 = w2[0] - w1[0];
		vec_t s2 = w3[0] - w1[0];
		vec_t t1 = w2[1] - w1[1];
		vec_t t2 = w3[1] - w1[1];

		vec_t r = 1.0 / (s1 * t2 - s2 * t1);

		VectorSet(sdir,
				(t2 * x1 - t1 * x2),
				(t2 * y1 - t1 * y2),
				(t2 * z1 - t1 * z2)
		);

		VectorScale(sdir, r, sdir);

		VectorSet(tdir,
				(s1 * x2 - s2 * x1),
				(s1 * y2 - s2 * y1),
				(s1 * z2 - s2 * z1)
		);

		VectorScale(tdir, r, tdir);

		VectorAdd(tan1[i1], sdir, tan1[i1]);
		VectorAdd(tan1[i2], sdir, tan1[i2]);
		VectorAdd(tan1[i3], sdir, tan1[i3]);

		VectorAdd(tan2[i1], tdir, tan2[i1]);
		VectorAdd(tan2[i2], tdir, tan2[i2]);
		VectorAdd(tan2[i3], tdir, tan2[i3]);
	}

	// calculate the tangents

	tri = obj->tris;

	for (i = 0; i < obj->num_tris; i++, tri++) {

		const r_obj_vert_t *v = tri->verts;

		for (j = 0; j < 3; j++, v++) { // each vert
			vec3_t bitangent;

			const vec_t *normal = &obj->normals[(v->normal - 1) * 3];
			vec_t *tangent = &obj->tangents[(v->vert - 1) * 4];

			TangentVectors(normal, tan1[v->vert - 1], tan2[v->vert - 1], tangent, bitangent);
		}
	}

	Mem_Free(tan1);
	Mem_Free(tan2);
}

/*
 * @brief
 */
static void R_LoadObjModelVertexArrays(r_model_t *mod) {
	const r_obj_t *obj;
	const r_obj_tri_t *t;
	int32_t i, j, vert_index, tangent_index, texcoord_index;

	R_AllocVertexArrays(mod);

	obj = (r_obj_t *) mod->mesh->data;

	vert_index = tangent_index = texcoord_index = 0;

	t = obj->tris;

	for (i = 0; i < obj->num_tris; i++, t++) { // build the arrays

		const r_obj_vert_t *v = t->verts;

		for (j = 0; j < 3; j++, v++) { // each vert

			VectorCopy((&obj->verts[(v->vert - 1) * 3]),
					(&mod->verts[vert_index + j * 3]));

			Vector4Copy((&obj->tangents[(v->vert - 1) * 4]),
					(&mod->tangents[tangent_index + j * 4]));

			if (v->normal) {
				VectorCopy((&obj->normals[(v->normal - 1) * 3]),
						(&mod->normals[vert_index + j * 3]));
			}

			if (v->texcoord) {
				memcpy(&mod->texcoords[texcoord_index + j * 2],
						&obj->texcoords[(v->texcoord - 1) * 2], sizeof(vec2_t));
			}
		}

		vert_index += 9;
		tangent_index += 12;
		texcoord_index += 6;
	}
}

/*
 * @brief Triangulation of arbitrary polygons. Assembles count tris on the model
 * from the specified array of verts. All tris will share the first vert.
 */
static void R_LoadObjModelTris(r_obj_t *obj, const r_obj_vert_t *verts, int32_t count) {
	int32_t i;

	if (!obj->tris)
		return;

	for (i = 0; i < count; i++) { // walk around the polygon

		const int32_t v0 = 0;
		const int32_t v1 = 1 + i;
		const int32_t v2 = 2 + i;

		r_obj_tri_t *t = &obj->tris[obj->num_tris_parsed + i];

		t->verts[0] = verts[v0];
		t->verts[1] = verts[v1];
		t->verts[2] = verts[v2];
	}
}

#define MAX_OBJ_FACE_VERTS 128

/*
 * @brief Each line consists of 3 or more vertex definitions, e.g.
 *
 *   57/13/31 58/14/32 59/15/33 21/15/19
 *
 * Tokenize the line with Com_Parse, and parse each vertex definition.
 * Faces with more than 3 vertices must be broken down into triangles.
 *
 * Returns the number of triangles produced for the specified line.
 */
static int32_t R_LoadObjModelFace(const r_model_t *mod, r_obj_t *obj, const char *line) {
	r_obj_vert_t *v, verts[MAX_OBJ_FACE_VERTS];
	const char *d;
	char *e, tok[32];
	int32_t i, tris;

	memset(verts, 0, sizeof(verts));
	i = 0;

	while (true) {

		const char *c = ParseToken(&line);

		if (*c == '\0') // done
			break;

		if (i == MAX_OBJ_FACE_VERTS) {
			Com_Error(ERR_DROP, "%s has too many vertexes\n", mod->media.name);
		}

		if (!obj->tris) { // simply count verts
			i++;
			continue;
		}

		d = c;
		v = &verts[i++];

		memset(tok, 0, sizeof(tok));
		e = tok;

		while (*d) { // parse the vertex definition

			if (*d == '/') { // index delimiter, parse the token

				if (!v->vert)
					v->vert = atoi(tok);

				else if (!v->texcoord)
					v->texcoord = atoi(tok);

				else if (!v->normal)
					v->normal = atoi(tok);

				memset(tok, 0, sizeof(tok));
				e = tok;

				d++;
				continue;
			}

			*e++ = *d++;
		}

		// parse whatever is left in the token

		if (!v->vert)
			v->vert = atoi(tok);

		else if (!v->texcoord)
			v->texcoord = atoi(tok);

		else if (!v->normal)
			v->normal = atoi(tok);
	}

	tris = i - 2; // number of triangles from parsed verts

	if (tris < 1)
		Com_Error(ERR_DROP, "%s has too few vertexes\n", mod->media.name);

	R_LoadObjModelTris(obj, verts, tris); // break verts up into tris

	return tris;
}

/*
 * @brief Parse the object file line. If the structures have been allocated,
 * populate them. Otherwise simply accumulate counts.
 */
static void R_LoadObjModelLine(const r_model_t *mod, r_obj_t *obj, const char *line) {

	if (!line || !line[0]) // don't bother
		return;

	if (!strncmp(line, "v ", 2)) { // vertex

		if (obj->verts) { // parse it
			vec_t *f = obj->verts + obj->num_verts_parsed * 3;

			if (sscanf(line + 2, "%f %f %f", &f[0], &f[2], &f[1]) != 3)
				Com_Error(ERR_DROP, "Malformed vertex for %s: %s\n", mod->media.name, line);

			obj->num_verts_parsed++;
		} else
			// or just count it
			obj->num_verts++;
	} else if (!strncmp(line, "vn ", 3)) { // normal

		if (obj->normals) { // parse it
			vec_t *f = obj->normals + obj->num_normals_parsed * 3;

			if (sscanf(line + 3, "%f %f %f", &f[0], &f[1], &f[2]) != 3)
				Com_Error(ERR_DROP, "Malformed normal for %s: %s\n", mod->media.name, line);

			obj->num_normals_parsed++;
		} else
			// or just count it
			obj->num_normals++;

	} else if (!strncmp(line, "vt ", 3)) { // texcoord

		if (obj->texcoords) { // parse it
			vec_t *f = obj->texcoords + obj->num_texcoords_parsed * 2;

			if (sscanf(line + 3, "%f %f", &f[0], &f[1]) != 2)
				Com_Error(ERR_DROP, "Malformed texcoord for %s: %s\n", mod->media.name, line);

			f[1] = -f[1];
			obj->num_texcoords_parsed++;
		} else
			// or just count it
			obj->num_texcoords++;
	} else if (!strncmp(line, "f ", 2)) { // face

		if (obj->tris) // parse it
			obj->num_tris_parsed += R_LoadObjModelFace(mod, obj, line + 2);
		else
			// or just count it
			obj->num_tris += R_LoadObjModelFace(mod, obj, line + 2);
	}

	// else we just ignore it
}

/*
 * @brief Drives the actual parsing of the object file. The file is read twice:
 * once to acquire primitive counts, and a second time to load them.
 */
static void R_LoadObjModel_(r_model_t *mod, r_obj_t *obj, const void *buffer) {
	char line[MAX_STRING_CHARS];
	const char *c;
	bool comment;
	int32_t i;

	c = buffer;
	comment = false;
	i = 0;

	memset(&line, 0, sizeof(line));

	while (*c) {

		if (*c == '#') {
			comment = true;
			c++;
			continue;
		}

		if (*c == '\r' || *c == '\n') { // end of line

			if (i && !comment)
				R_LoadObjModelLine(mod, obj, g_strstrip(line));

			c++;
			comment = false;
			i = 0;

			memset(&line, 0, sizeof(line));

			continue;
		}

		line[i++] = *c++;
	}
}

/*
 * @brief
 */
void R_LoadObjModel(r_model_t *mod, void *buffer) {
	r_obj_t *obj;
	const vec_t *v;
	int32_t i;

	mod->mesh = Mem_LinkMalloc(sizeof(r_mesh_model_t), mod);
	mod->mesh->data = obj = Mem_LinkMalloc(sizeof(r_obj_t), mod->mesh);

	R_LoadObjModel_(mod, obj, buffer); // resolve counts

	if (!obj->num_verts) {
		Com_Error(ERR_DROP, "Failed to resolve vertex data: %s\n", mod->media.name);
	}

	mod->mesh->num_frames = 1;

	// allocate the arrays
	obj->verts = Mem_LinkMalloc(obj->num_verts * sizeof(vec_t) * 3, mod->mesh);
	obj->normals = Mem_LinkMalloc(obj->num_normals * sizeof(vec_t) * 3, mod->mesh);
	obj->texcoords = Mem_LinkMalloc(obj->num_texcoords * sizeof(vec_t) * 2, mod->mesh);
	obj->tris = Mem_LinkMalloc(obj->num_tris * sizeof(r_obj_tri_t), mod->mesh);

	// including the tangents
	obj->tangents = Mem_LinkMalloc(obj->num_verts * sizeof(vec_t) * 4, mod->mesh);

	R_LoadObjModel_(mod, obj, buffer); // load it

	R_LoadObjModelTangents(obj);

	ClearBounds(mod->mins, mod->maxs);

	v = obj->verts;
	for (i = 0; i < obj->num_verts; i++, v += 3) { // resolve mins/maxs
		AddPointToBounds(v, mod->mins, mod->maxs);
	}

	// load the material
	R_LoadMeshMaterial(mod);

	// and configs
	R_LoadMeshConfigs(mod);

	// and finally the arrays
	R_LoadObjModelVertexArrays(mod);
}

