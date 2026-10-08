/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// tr_models.cpp -- model loading and caching

#include "tr_local.h"

#include <vector>
#include <algorithm>
#include "../corepp/tiki.h"
#include "../corepp/vector.h"

#define LL(x) x = LittleLong(x)

qboolean   g_bInfoworldtris = qfalse;
// indexed by slot in the scene, which runs to MAX_REFENTITIES
static int entityNumIndexes[MAX_REFENTITIES];
static int staticModelNumIndexes[4095];

static int R_CullSkelModel(dtiki_t *tiki, refEntity_t *e, skelAnimFrame_t *newFrame, float fScale, float *vLocalOrg);
static void R_PoseSkelModel(trRefEntity_t *ent, dtiki_t *tiki, int num_tags, float tiki_scale, const vec3_t tiki_localorigin, int iRadiusCull);
static void R_AddSkelDrawSurfs(trRefEntity_t *ent, dtiki_t *tiki, qboolean personalModel);

// the mesh reaches past the joints (a skull above the head bone, a torso
// around the spine): a margin on the bounds of the bones, in world units
#define SKEL_BONE_CULL_MARGIN 32.0f


/*
** R_GetModelByHandle
*/
model_t *R_GetModelByHandle(qhandle_t hModel)
{
    model_t *mod;

    // out of range gets the default model
    if (hModel < 1 || hModel >= tr.numModels) {
        return tr.models[0];
    }

    mod = tr.models[hModel];

    return mod;
}

/*
** R_Model_GetHandle
*/
dtiki_t *R_Model_GetHandle(qhandle_t handle)
{
    model_t *model = R_GetModelByHandle(handle);

    if (model->type == MOD_TIKI) {
        return model->d.tiki;
    }

    return NULL;
}

//===============================================================================

/*
** R_FreeModel
*/
void R_FreeModel(model_t *mod)
{
    if (mod->type == MOD_TIKI) {
        ri.CG_EndTiki(mod->d.tiki);
    }

    memset(mod, 0, sizeof(*mod));
}

/*
** R_AllocModel
*/
model_t *R_AllocModel(void)
{
	model_t *mod;
    int i;

    for (i = 0; i < tr.numModels; i++) {
        mod = tr.models[i];
        if (!mod->name[0]) {
            break;
        }
    }

    if (i == tr.numModels) {
        if (i == MAX_MOD_KNOWN) {
            return NULL;
        }

        mod = (model_t*)ri.Hunk_Alloc(sizeof(*tr.models[tr.numModels]), h_low);
        tr.models[tr.numModels] = mod;
        tr.numModels++;
    } else {
        mod = tr.models[i];
    }

    mod->index = i;

    return mod;
}

/*
** RE_FreeModels
*/
void RE_FreeModels(void)
{
    int hModel;

    for (hModel = 0; hModel < tr.numModels; hModel++) {
        if (!tr.models[hModel]->name[0]) {
            continue;
        }

        R_FreeModel(tr.models[hModel]);
    }
}

/*
** R_RegisterShaders
*/
void R_RegisterShaders(model_t *mod)
{
    dtiki_t        *tiki;
    int             i, j;
    dtikisurface_t *psurface;
    shader_t       *sh;

    tiki = mod->d.tiki;

    for (i = 0; i < tiki->num_surfaces; i++) {
        psurface = &tiki->surfaces[i];

        assert(psurface->numskins <= MAX_TIKI_SHADER);
        for (j = 0; j < psurface->numskins; j++) {
            if (psurface->shader[j][0]) {
                sh                   = R_FindShader(psurface->shader[j], LIGHTMAP_NONE, qtrue);
                psurface->hShader[j] = sh->index;
            } else {
                psurface->hShader[j] = 0;
            }
        }
    }
}

/*
** RE_UnregisterServerModel
*/
void RE_UnregisterServerModel(qhandle_t hModel)
{
    if (hModel < 0 || hModel >= MAX_MOD_KNOWN) {
        return;
    }

    if (tr.models[hModel]->serveronly) {
        R_FreeModel(tr.models[hModel]);
    }
}

/*
** R_RegisterModelInternal
*/
static qhandle_t R_RegisterModelInternal(const char *name, qboolean bBeginTiki, qboolean use)
{
    model_t    *mod;
    qhandle_t   hModel;
    const char *ptr;

    if (!name || !*name) {
        ri.Printf(PRINT_ALL, "RE_RegisterModel: NULL name\n");
        return 0;
    }

    if (strlen(name) >= 128) {
        Com_Printf("Model name exceeds MAX_MODEL_NAME\n");
        return 0;
    }

    //
    // search the currently loaded models
    //
    for (hModel = 1; hModel < tr.numModels; hModel++) {
        mod = tr.models[hModel];
        if (!Q_stricmp(mod->name, name)) {
            if (mod->type == MOD_BAD) {
                return 0;
            }
            return hModel;
        }
    }

    // allocate a new model_t

    if ((mod = R_AllocModel()) == NULL) {
        ri.Printf(PRINT_WARNING, "RE_RegisterModel: R_AllocModel() failed for '%s'\n", name);
        return 0;
    }

    // only set the name after the model has been successfully loaded
    Q_strncpyz(mod->name, name, sizeof(mod->name));

    // make sure the render thread is stopped
    R_IssuePendingRenderCommands();

    mod->serveronly = qtrue;

    //
    // load the files
    //
    ptr = strrchr(name, '.');

    if (ptr) {
        ptr++;

        if (!stricmp(ptr, "spr")) {
            mod->d.sprite = SPR_RegisterSprite(name);
            Q_strncpyz(mod->name, name, sizeof(mod->name));

            if (mod->d.sprite) {
                mod->type = MOD_SPRITE;
                return mod->index;
            }
        } else if (!stricmp(ptr, "tik")) {
            mod->d.tiki = ri.TIKI_RegisterTikiFlags(name, use);
            Q_strncpyz(mod->name, name, sizeof(mod->name));

            if (mod->d.tiki) {
                mod->type = MOD_TIKI;
                R_RegisterShaders(mod);

                if (bBeginTiki) {
                    ri.CG_ProcessInitCommands(mod->d.tiki, NULL);
                }

                return mod->index;
            }
        }
    }

    ri.Printf(PRINT_ERROR, "RE_RegisterModel: Registration failed for '%s'\n", name);
    mod->type = MOD_BAD;

    return 0;
}

/*
** RE_RegisterServerModel
*/
qhandle_t RE_RegisterServerModel(const char *name)
{
    return R_RegisterModelInternal(name, qtrue, qfalse);
}

/*
** RE_SpawnEffectModel
*/
qhandle_t RE_SpawnEffectModel(const char *szModel, vec3_t vPos, vec3_t *axis)
{
    refEntity_t new_entity;

    memset(&new_entity, 0, sizeof(refEntity_t));
    memset(&new_entity.shaderRGBA, 255, sizeof(byte) * 4);

    VectorCopy(vPos, new_entity.origin);
    new_entity.scale = 1.0;

    if (axis) {
        AxisCopy(axis, new_entity.axis);
    }

    new_entity.hModel = R_RegisterModelInternal(szModel, qfalse, qtrue);

    if (new_entity.hModel) {
        tr.models[new_entity.hModel]->serveronly = qfalse;
        ri.CG_ProcessInitCommands(tr.models[new_entity.hModel]->d.tiki, &new_entity);
    }

    return new_entity.hModel;
}

/*
** RE_RegisterModel
*/
qhandle_t RE_RegisterModel(const char *name)
{
    qhandle_t handle;

    handle = R_RegisterModelInternal(name, qtrue, qtrue);

    if (handle) {
        tr.models[handle]->serveronly = qfalse;
    }
    return handle;
}

//=============================================================================

/*
===============
R_ModelInit
===============
*/
void R_ModelInit(void)
{
    model_t *mod;
    int      i;

    // leave a space for NULL model
    tr.numModels = 0;

    mod = R_AllocModel();
    Q_strncpyz(mod->name, "** BAD MODEL **", sizeof(mod->name));
    mod->type = MOD_BAD;

    for (i = 0; i < ARRAY_LEN(tr.skel_index); i++) {
        tr.skel_index[i] = -1;
    }
}

/*
================
R_Modellist_f
================
*/
void R_Modellist_f(void)
{
    int i;

    for (i = 1; i < tr.numModels; i++) {
        ri.Printf(PRINT_ALL, "%s\n", tr.models[i]->name);
    }
}

/*
====================
R_ModelRadius
====================
*/
float R_ModelRadius(qhandle_t handle)
{
    int      j;
    vec3_t   bounds[2];
    model_t *model;
    float    radius, maxRadius;
    vec3_t   tmpVec;
    float    w;

    model = R_GetModelByHandle(handle);

    switch (model->type) {
    case MOD_BRUSH:
        maxRadius = 0.0;

        VectorCopy(model->bmodel->bounds[0], bounds[0]);
        VectorCopy(model->bmodel->bounds[1], bounds[1]);

        for (j = 0; j < 8; j++) {
            tmpVec[0] = bounds[j & 1 ? 1 : 0][0];
            tmpVec[1] = bounds[j & 2 ? 1 : 0][1];
            tmpVec[2] = bounds[j & 4 ? 1 : 0][2];

            radius = VectorLength(tmpVec);

            if (maxRadius < radius) {
                maxRadius = radius;
            }
        }
        break;
    case MOD_TIKI:
        return ri.TIKI_GlobalRadius(model->d.tiki);
    case MOD_SPRITE:
        maxRadius = model->d.sprite->width * model->d.sprite->scale * 0.5;
        w         = model->d.sprite->height * model->d.sprite->scale * 0.5;

        if (maxRadius <= w) {
            maxRadius = w;
        }
        break;
    default:
        maxRadius = 0.0;
    }

    return maxRadius;
}

/*
====================
R_ModelBounds
====================
*/
void R_ModelBounds(qhandle_t handle, vec3_t mins, vec3_t maxs)
{
    model_t *model;

    model = R_GetModelByHandle(handle);

    switch (model->type) {
    default:
    case MOD_BAD:
        VectorClear(mins);
        VectorClear(maxs);
        break;
    case MOD_BRUSH:
        VectorCopy(model->bmodel->bounds[0], mins);
        VectorCopy(model->bmodel->bounds[1], maxs);
        break;
    case MOD_TIKI:
        ri.TIKI_CalculateBounds(model->d.tiki, 1.0, mins, maxs);
        break;
    case MOD_SPRITE:
        mins[0] = -model->d.sprite->width * model->d.sprite->scale * 0.5;
        mins[1] = -model->d.sprite->height * model->d.sprite->scale * 0.5;
        mins[2] = -0.0;
        maxs[0] = model->d.sprite->width * model->d.sprite->scale * 0.5;
        maxs[1] = model->d.sprite->height * model->d.sprite->scale * 0.5;
        maxs[2] = 0.0;
        break;
    }
}

#if 0
// Replaced by TIKI_FindSkelByHeader
/*
====================
GetModelPath
====================
*/
const char *GetModelPath( skelHeaderGame_t *skelmodel ) {
	int			i;
	int			num;
	skelcache_t	*cache;

	num = cache_numskel;

	for( i = 0; i < TIKI_MAX_SKELCACHE; i++ )
	{
		cache = &skelcache[ i ];

		if( cache->skel )
		{
			if( cache->skel == skelmodel ) {
				return cache->path;
			}

			num--;
			if( num < 0 ) {
				break;
			}
		}
	}

	return NULL;
}
#endif

/*
====================
GetLodCutoff
====================
*/
int GetLodCutoff(skelHeaderGame_t *skelmodel, float lod_val, int renderfx)
{
    lodControl_t *LOD;
    float         f;
    float         fLODCap;

    LOD = skelmodel->pLOD;

    if (renderfx & RF_DEPTHHACK) {
        fLODCap = LOD->maxMetric + (LOD->minMetric - LOD->maxMetric) * r_lodviewmodelcap->value;
    } else {
        f       = (LOD->minMetric - LOD->maxMetric) * r_lodcap->value + LOD->maxMetric;
        fLODCap = (lod_val - LOD->maxMetric) * r_lodscale->value + LOD->maxMetric;

        if (fLODCap > f) {
            fLODCap = f;
        }
    }

    if (fLODCap >= LOD->minMetric || !r_uselod->integer) {
        return LOD->curve[0].val;
    } else if (fLODCap <= LOD->maxMetric) {
        return LOD->curve[4].val;
    } else if (fLODCap <= LOD->consts[3].cutoff) {
        return fLODCap * LOD->consts[3].scale + LOD->consts[3].base;
    } else if (fLODCap <= LOD->consts[2].cutoff) {
        return fLODCap * LOD->consts[2].scale + LOD->consts[2].base;
    } else if (fLODCap <= LOD->consts[1].cutoff) {
        return fLODCap * LOD->consts[1].scale + LOD->consts[1].base;
    } else {
        return fLODCap * LOD->consts[0].scale + LOD->consts[0].base;
    }
}

/*
===============
R_SaveLODFile
===============
*/
static void R_SaveLODFile(const char *path, lodControl_t *LOD)
{
    fileHandle_t file = ri.FS_OpenFileWrite(path);
    if (!file) {
        ri.Printf(PRINT_WARNING, "SaveLODFile: Failed to open file %s\n", path);
        return;
    }

    ri.FS_Write(LOD, sizeof(lodControl_t), file);
}

/*
====================
GetToolLodCutoff
====================
*/
int GetToolLodCutoff(skelHeaderGame_t *skelmodel, float lod_val)
{
    lodControl_t *LOD;
    float         totalRange;
    int           i;
    char          lodPath[256];
    char         *ext;

    LOD        = skelmodel->pLOD;
    totalRange = 0.0;
    for (i = 0; i < 10; i++) {
        if (skelmodel->lodIndex[i] > 0) {
            totalRange = skelmodel->lodIndex[i];
            break;
        }
    }

    if (lod_save->integer == 1) {
        ri.Cvar_Set("lod_save", "0");
        Q_strncpyz(lodPath, ri.TIKI_FindSkelByHeader(skelmodel)->path, sizeof(lodPath));
        ext = strstr(lodPath, "skd");
        strcpy(ext, "lod");
        R_SaveLODFile(lodPath, LOD);
    }

    if (lod_mesh->modified) {
        lod_mesh->modified = qfalse;
        ri.Cvar_Set("lod_minLOD", va("%f", LOD->minMetric));
        ri.Cvar_Set("lod_maxLOD", va("%f", LOD->maxMetric));
        ri.Cvar_Set("lod_LOD_slider", va("%f", 0.5));
        ri.Cvar_Set("lod_curve_0_slider", va("%f", LOD->curve[0].val / totalRange));
        ri.Cvar_Set("lod_curve_1_slider", va("%f", LOD->curve[1].val / totalRange));
        ri.Cvar_Set("lod_curve_2_slider", va("%f", LOD->curve[2].val / totalRange));
        ri.Cvar_Set("lod_curve_3_slider", va("%f", LOD->curve[3].val / totalRange));
        ri.Cvar_Set("lod_curve_4_slider", va("%f", LOD->curve[4].val / totalRange));
    }

    ri.Cvar_Set("lod_curve_0_val", va("%f", lod_curve_0_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_1_val", va("%f", lod_curve_1_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_2_val", va("%f", lod_curve_2_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_3_val", va("%f", lod_curve_3_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_4_val", va("%f", lod_curve_4_slider->value * totalRange));

    LOD->minMetric    = lod_minLOD->value;
    LOD->maxMetric    = lod_maxLOD->value;
    LOD->curve[0].val = lod_curve_0_val->value;
    LOD->curve[1].val = lod_curve_1_val->value;
    LOD->curve[2].val = lod_curve_2_val->value;
    LOD->curve[3].val = lod_curve_3_val->value;
    LOD->curve[4].val = lod_curve_4_val->value;

    ri.TIKI_CalcLodConsts(LOD);
    return GetLodCutoff(skelmodel, lod_val, 0);
}

/*
==============
R_GetTagPositionAndOrientation
==============
*/
orientation_t R_GetTagPositionAndOrientation(refEntity_t *ent, int tagnum)
{
    int           i;
    orientation_t tag_or, new_or;

    tag_or = RE_TIKI_Orientation(ent, tagnum);

    VectorCopy(ent->origin, new_or.origin);

    for (i = 0; i < 3; i++) {
        VectorMA(new_or.origin, tag_or.origin[i], ent->axis[i], new_or.origin);
    }

    MatrixMultiply(tag_or.axis, ent->axis, new_or.axis);
    return new_or;
}

/*
==============
RB_DrawSkeletor
==============
*/
void RB_DrawSkeletor(trRefEntity_t *ent)
{
    // FIXME: Unimplemented (GL2)
}

surfaceType_t skelSurface = SF_TIKI_SKEL;

/*
=============
R_FoldHeadAndArms

For RF_FIRST_PERSON_BODY, the player's own body seen through their eyes:
folds the head, where the camera sits, and the arms, which the view model
already draws, each into the joint it hangs from, and leaves the torso and
legs. The renderer has no bone names, so the joints are found by the shape of
the skeleton. The pelvis is the bone nearest the root with three children that
are chains of three bones or more (the two legs and the spine), and the spine
is the biggest of those. The chest is the first bone up the spine that
branches three ways (the neck and the two arms). With no such chest, the whole
spine is folded and only the legs are left.
=============
*/
static void R_FoldHeadAndArms(dtiki_t *tiki, int entityNumber, skelBoneCache_t *bones, int numBones)
{
    void *skeletor;
    int   parent[TIKI_MAX_BONES];
    int   size[TIKI_MAX_BONES];
    int   depth[TIKI_MAX_BONES];
    int   fold[TIKI_MAX_BONES];
    int   i, j, p;
    int   spine = -1, spineDepth = 0;
    int   chest = -1, chestDepth = 0;

    if (numBones > TIKI_MAX_BONES) {
        return;
    }

    skeletor = ri.TIKI_GetSkeletor(tiki, entityNumber);
    for (i = 0; i < numBones; i++) {
        parent[i] = ri.SKEL_GetBoneParent(skeletor, i);
        size[i]   = 0;
        depth[i]  = 0;
        fold[i]   = -1;
    }

    for (i = 0; i < numBones; i++) {
        size[i]++;
        for (p = parent[i], j = 0; p >= 0 && j < numBones; p = parent[p], j++) {
            size[p]++;
            depth[i]++;
        }
    }

    for (i = 0; i < numBones; i++) {
        int chains = 0, biggest = -1;

        for (j = 0; j < numBones; j++) {
            if (parent[j] == i && size[j] >= 3) {
                chains++;
                if (biggest < 0 || size[j] > size[biggest]) {
                    biggest = j;
                }
            }
        }

        if (chains >= 3 && (spine < 0 || depth[i] < spineDepth)) {
            spine      = biggest;
            spineDepth = depth[i];
        }
    }

    if (spine < 0) {
        return;
    }

    for (i = 0; i < numBones; i++) {
        int branches = 0;

        // only bones on the spine's side of the pelvis
        for (p = i, j = 0; p >= 0 && p != spine && j < numBones; p = parent[p], j++) {}
        if (p != spine) {
            continue;
        }

        for (j = 0; j < numBones; j++) {
            if (parent[j] == i && size[j] >= 2) {
                branches++;
            }
        }

        if (branches >= 3 && (chest < 0 || depth[i] < chestDepth)) {
            chest      = i;
            chestDepth = depth[i];
        }
    }

    // each bone folds into the root of the branch it is on
    for (i = 0; i < numBones; i++) {
        for (p = i, j = 0; p >= 0 && j < numBones; p = parent[p], j++) {
            if (chest >= 0 ? parent[p] == chest : p == spine) {
                fold[i] = p;
                break;
            }
        }
    }

    for (i = 0; i < numBones; i++) {
        if (fold[i] >= 0 && fold[i] != i) {
            VectorCopy(bones[fold[i]].offset, bones[i].offset);
        }
    }

    for (i = 0; i < numBones; i++) {
        if (fold[i] >= 0) {
            memset(bones[i].matrix, 0, sizeof(bones[i].matrix));
        }
    }
}

/*
==============
R_AddSkelSurfaces
==============
*/
void R_AddSkelSurfaces(trRefEntity_t *ent)
{
    dtiki_t         *tiki;
    qboolean         personalModel;
    float            tiki_scale;
    vec3_t           tiki_localorigin;
    vec3_t           tiki_worldorigin;
    //int tikiSurfNumOffset;
    static cvar_t   *vmEntity = NULL;
    float            radius;
    SkelVec3         centroid;
    //float range;
    //int render_count, total_tris;
    //int skinnum;
    //float target;
    Vector newDistance;
    //vec3_t org;
    int iRadiusCull = 0;
    int num_tags;
    qboolean posed;

    tiki = ent->e.tiki;

    if (!vmEntity) {
        vmEntity = ri.Cvar_Get("viewmodelentity", "", 0);
    }

    // don't add third_person objects if in a portal
    // Third person objects still go into the sun cascades and personal
    // shadows, as the md3 path in tr_mesh.c does: that is how the player's
    // own body casts a shadow in first person (cg_firstPersonShadow). They
    // stay out of the dlight cube maps, whose light is often the player's
    // own muzzle flash.
    personalModel = (ent->e.renderfx & RF_THIRD_PERSON) && !tr.viewParms.isPortal
                 && (tr.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP)) != VPF_DEPTHSHADOW;
    if (personalModel) {
        // nothing of it is drawn in this view, so don't pose it
        return;
    }

    R_UpdatePoseInternal(&ent->e);

    num_tags = ri.TIKI_GetNumChannels(tiki);

    // posed for another view of this scene already: every view draws it with
    // the bones it was given last, so posing it again changed nothing
    posed = ent->posedScene == tr.sceneCount && r_skinCache->integer;

    if (!posed && num_tags + TIKI_Skel_Bones_Index > MAX_SKELBONES) {
        ri.Printf(PRINT_DEVELOPER, "R_AddSkelSurfaces: too many skeleton models visible on '%s'\n", tiki->a->name);
        return;
    }

    tiki_scale = tiki->load_scale * ent->e.scale;
    VectorScale(tiki->load_origin, tiki_scale, tiki_localorigin);
    R_LocalPointToWorld(tiki_localorigin, tiki_worldorigin);

    radius = R_GetRadius(&ent->e);

    if (!lod_tool->integer) {
        iRadiusCull = R_CullPointAndRadius(tiki_worldorigin, radius);
        if (r_showcull->integer & 2) {
            switch (iRadiusCull) {
            case CULL_IN:
                R_DebugCircle(tiki_worldorigin, radius * 1.2f, 0, 1, 0, 0.5f, qfalse);
                break;
            case CULL_CLIP:
                R_DebugCircle(tiki_worldorigin, radius * 1.2f, 0, 1, 0, 0.5f, qfalse);
                break;
            case CULL_OUT:
                R_DebugCircle(tiki_worldorigin, radius * 1.2f + 32.f, 1, 0.2f, 0.2f, 0.5f, qfalse);
                break;
            }
        }

        switch (iRadiusCull) {
        case CULL_IN:
            tr.pc.c_sphere_cull_md3_in++;
            break;
        case CULL_CLIP:
            tr.pc.c_sphere_cull_md3_clip++;
            break;
        case CULL_OUT:
            tr.pc.c_sphere_cull_md3_out++;
            break;
        }
    }

    if (tiki->a->bIsCharacter) {
        if (tr.viewParms.isPortal) {
            ent->lodpercentage[1] = R_CalcLod(tiki_worldorigin, 92.f / ent->e.scale);
        } else {
            ent->lodpercentage[0] = R_CalcLod(tiki_worldorigin, 92.f / ent->e.scale);
        }
    } else {
        if (tr.viewParms.isPortal) {
            ent->lodpercentage[1] = R_CalcLod(tiki_worldorigin, radius / ent->e.scale);
        } else {
            ent->lodpercentage[0] = R_CalcLod(tiki_worldorigin, radius / ent->e.scale);
        }
    }

    if (!posed) {
        R_PoseSkelModel(ent, tiki, num_tags, tiki_scale, tiki_localorigin, iRadiusCull);
    }

    // Nothing here ever skipped a model outside the view: it was posed,
    // skinned and drawn regardless, and every sun cascade that takes entities
    // is a view of its own. A model is left out of a view only when both
    // the animation's own sphere and its bones are outside it: a ragdoll's
    // bones can lie well away from the sphere, which follows the entity and
    // not the body. The main view, which had been left as it was, culls
    // them too: the people behind the camera were drawn into every frame.
    // It is posed above whether drawn or not, for the views that do. The
    // view's own model, which hangs off the eye, is always drawn.
    if (r_skelCull->integer && !lod_tool->integer && iRadiusCull == CULL_OUT
        && !(ent->e.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK))) {
        vec3_t centre;

        R_LocalPointToWorld(ent->boneCentre, centre);
        if (R_CullPointAndRadius(centre, ent->boneRadius + SKEL_BONE_CULL_MARGIN) == CULL_OUT) {
            return;
        }
    }

    // Added in OPM
    //  Hidden behind what the main view's depth prepass drew, the last tests
    //  found: left out of it (R_OcclusionCulled). Its box takes in the
    //  animation's sphere and its bones'.
    if (!lod_tool->integer && !(ent->e.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK))
        && !(tr.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP)) && !tr.viewParms.isPortal
        && !tr.viewParms.isPortalSky && !(tr.refdef.rdflags & RDF_NOWORLDMODEL)) {
        vec3_t centre, mins, maxs;
        float  boneRadius = ent->boneRadius + SKEL_BONE_CULL_MARGIN;
        int    k;

        R_LocalPointToWorld(ent->boneCentre, centre);
        for (k = 0; k < 3; k++) {
            mins[k] = Q_min(tiki_worldorigin[k] - radius, centre[k] - boneRadius);
            maxs[k] = Q_max(tiki_worldorigin[k] + radius, centre[k] + boneRadius);
        }
        if (R_OcclusionCulled(ent->e.entityNumber, mins, maxs)) {
            return;
        }
    }

    // Added in OPM
    //  The realtime lights that reach the animation's sphere, or its bones.
    {
        const uint64_t candidates = R_RtViewMask();

        if (candidates != RT_MASK_ALL) {
            vec3_t   centre;
            uint64_t mask;

            R_LocalPointToWorld(ent->boneCentre, centre);
            mask = R_RtSphereMask(tiki_worldorigin, radius, candidates);
            mask |= R_RtSphereMask(centre, ent->boneRadius + SKEL_BONE_CULL_MARGIN, candidates);
            tr.rtDrawCulled = ~mask;
        }
    }

    //
    // draw all meshes
    //
    R_AddSkelDrawSurfs(ent, tiki, personalModel);
}

/*
=============
R_PoseSkelModel

Added in OPM, out of R_AddSkelSurfaces: the bones and morphs of a skeletal
model for this scene, and the bounds of its bones.
=============
*/
static void R_PoseSkelModel(trRefEntity_t *ent, dtiki_t *tiki, int num_tags, float tiki_scale, const vec3_t tiki_localorigin, int iRadiusCull)
{
    static int       poses;
    skelBoneCache_t *outbones = &TIKI_Skel_Bones[TIKI_Skel_Bones_Index];
    skelAnimFrame_t *newFrame;
    skeletor_c      *skeletor;
    vec3_t           mins, maxs, local;
    int              added;
    int              i;

    newFrame = (skelAnimFrame_t *)ri.Hunk_AllocateTempMemory(
        sizeof(skelAnimFrame_t) + ri.TIKI_GetNumChannels(tiki) * sizeof(SkelMat4)
    );
    R_GetFrame(&ent->e, newFrame);

    // the bounds of its bones, which a ragdoll's can leave well behind the
    // animation's own sphere (the shadow views cull by them)
    ClearBounds(mins, maxs);
    for (i = 0; i < num_tags; i++) {
        VectorMA(tiki_localorigin, tiki_scale, newFrame->bones[i][3], local);
        AddPointToBounds(local, mins, maxs);
    }
    if (num_tags > 0) {
        VectorAdd(mins, maxs, ent->boneCentre);
        VectorScale(ent->boneCentre, 0.5f, ent->boneCentre);
        ent->boneRadius = Distance(maxs, ent->boneCentre);
    } else {
        VectorCopy(tiki_localorigin, ent->boneCentre);
        ent->boneRadius = 0;
    }

    if (lod_tool->integer || iRadiusCull != CULL_CLIP
        || R_CullSkelModel(tiki, &ent->e, newFrame, tiki_scale, (float *)tiki_localorigin) != CULL_OUT) {
        //
        // copy bones position and axis
        //
        for (i = 0; i < num_tags; i++) {
            VectorCopy(newFrame->bones[i][3], outbones->offset);
            outbones->matrix[0][0] = newFrame->bones[i][0][0];
            outbones->matrix[0][1] = newFrame->bones[i][0][1];
            outbones->matrix[0][2] = newFrame->bones[i][0][2];
            outbones->matrix[0][3] = 0;
            outbones->matrix[1][0] = newFrame->bones[i][1][0];
            outbones->matrix[1][1] = newFrame->bones[i][1][1];
            outbones->matrix[1][2] = newFrame->bones[i][1][2];
            outbones->matrix[1][3] = 0;
            outbones->matrix[2][0] = newFrame->bones[i][2][0];
            outbones->matrix[2][1] = newFrame->bones[i][2][1];
            outbones->matrix[2][2] = newFrame->bones[i][2][2];
            outbones->matrix[2][3] = 0;
            outbones++;
        }

        if (ent->e.renderfx & RF_FIRST_PERSON_BODY) {
            R_FoldHeadAndArms(tiki, ent->e.entityNumber, outbones - num_tags, num_tags);
        }
    }

    ri.Hunk_FreeTempMemory(newFrame);

    ent->e.bonestart = TIKI_Skel_Bones_Index;
    TIKI_Skel_Bones_Index += num_tags;

    ent->e.hasMorph = qfalse;

    //
    // get the skeletor
    //
    skeletor = (skeletor_c *)ri.TIKI_GetSkeletor(tiki, ent->e.entityNumber);

    //
    // add morphs
    //
    added = ri.SKEL_GetMorphWeightFrame(
        skeletor, ent->e.frameInfo[0].index, ent->e.frameInfo[0].time, &skeletorMorphCache[skeletorMorphCacheIndex]
    );
    ent->e.morphstart = skeletorMorphCacheIndex;

    if (added) {
        // found morphs
        skeletorMorphCacheIndex += added;
        ent->e.hasMorph = qtrue;
    }

    ent->posedScene = tr.sceneCount;
    // never 0, which is no pose (RB_SkelMesh keeps nothing of it)
    if (++poses <= 0) {
        poses = 1;
    }
    ent->poseId = poses;
}

/*
=============
R_AddSkelDrawSurfs

Added in OPM, out of R_AddSkelSurfaces: the surfaces of a posed skeletal model
into this view.
=============
*/
static void R_AddSkelDrawSurfs(trRefEntity_t *ent, dtiki_t *tiki, qboolean personalModel)
{
    shader_t          *shader;
    dtikisurface_t    *dsurf;
    byte              *bsurf;
    skelSurfaceGame_t *surface;
    int                mesh;
    int                i;

    dsurf = tiki->surfaces;
    bsurf = &ent->e.surfaces[0];
    for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelHeaderGame_t *skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);

        if (!skelmodel) {
            ri.Printf(PRINT_DEVELOPER, "R_AddSkelSurfaces: couldn't get skel model in '%s'\n", tiki->a->name);
            return;
        }

        if (lod_tool->integer && !stricmp(ent->e.tiki->a->name, lod_tikiname->string)) {
            if (lod_mesh->integer > tiki->numMeshes - 1) {
                ri.Cvar_Set("lod_mesh", va("%d", tiki->numMeshes - 1));
            }

            if (mesh == lod_mesh->integer) {
                if (skelmodel->pLOD) {
                    break;
                }
            }
        }

        //
        // draw all surfaces
        //
        surface = skelmodel->pSurfaces;
        for (i = 0; i < skelmodel->numSurfaces; i++, dsurf++, bsurf++, surface = surface->pNext) {
            if (*bsurf & 4) {
                continue;
            }

            shader         = NULL;
            surface->ident = SF_TIKI_SKEL;

            // use a custom shader if specified
            if (!(ent->e.customShader) || (ent->e.renderfx & RF_CUSTOMSHADERPASS)) {
                int iShaderNum = ent->e.skinNum + (*bsurf & 3);

                if (iShaderNum >= dsurf->numskins) {
                    iShaderNum = 0;
                }
                shader = tr.shaders[dsurf->hShader[iShaderNum]];
            } else {
                shader = R_GetShaderByHandle(ent->e.customShader);
            }

            if (!personalModel) {
                if ((*bsurf & 0x40) && (dsurf->numskins > 1)) {
                    int iShaderNum = ent->e.skinNum + (*bsurf & 2);

                    R_AddDrawSurf((surfaceType_t *)surface, tr.shaders[dsurf->hShader[iShaderNum]], 0, 0, 0, 0);
                    R_AddDrawSurf((surfaceType_t *)surface, tr.shaders[dsurf->hShader[iShaderNum + 1]], 0, 0, 0, 0);
                } else {
                    R_AddDrawSurf((surfaceType_t *)surface, shader, 0, 0, 0, 0);
                }
            }

            if (!personalModel && (ent->e.customShader) && (ent->e.renderfx & RF_CUSTOMSHADERPASS)) {
                shader = R_GetShaderByHandle(ent->e.customShader);
                R_AddDrawSurf((surfaceType_t *)surface, shader, 0, 0, 0, 0);
            }
        }
    }

    // FIXME: setup LOD
}

/*
=============
SkelVertGetNormal
=============
*/
inline static void SkelVertGetNormal(skeletorVertex_t *vert, skelBoneCache_t *bone, vec3_t out)
{
    out[0] = vert->normal[0] * bone->matrix[0][0] + vert->normal[1] * bone->matrix[1][0]
           + vert->normal[2] * bone->matrix[2][0];

    out[1] = vert->normal[0] * bone->matrix[0][1] + vert->normal[1] * bone->matrix[1][1]
           + vert->normal[2] * bone->matrix[2][1];

    out[2] = vert->normal[0] * bone->matrix[0][2] + vert->normal[1] * bone->matrix[1][2]
           + vert->normal[2] * bone->matrix[2][2];
}

/*
=============
SkelMorphGetXyz
=============
*/
inline static void SkelMorphGetXyz(skeletorMorph_t *morph, int *morphcache, vec3_t out)
{
    VectorMA(out, *morphcache, morph->offset, out);
}

/*
=============
SkelWeightGetXyz
=============
*/
inline static void SkelWeightGetXyz(skelWeight_t *weight, skelBoneCache_t *bone, vec3_t out)
{
    out[0] += ((weight->offset[0] * bone->matrix[0][0] + weight->offset[1] * bone->matrix[1][0]
                + weight->offset[2] * bone->matrix[2][0])
               + bone->offset[0])
            * weight->boneWeight;

    out[1] += ((weight->offset[0] * bone->matrix[0][1] + weight->offset[1] * bone->matrix[1][1]
                + weight->offset[2] * bone->matrix[2][1])
               + bone->offset[1])
            * weight->boneWeight;

    out[2] += ((weight->offset[0] * bone->matrix[0][2] + weight->offset[1] * bone->matrix[1][2]
                + weight->offset[2] * bone->matrix[2][2])
               + bone->offset[2])
            * weight->boneWeight;
}

/*
=============
SkelWeightMorphGetXyz
=============
*/
inline static void SkelWeightMorphGetXyz(skelWeight_t *weight, skelBoneCache_t *bone, vec3_t totalmorph, vec3_t out)
{
    vec3_t point;

    VectorAdd(totalmorph, weight->offset, point);

    out[0] += ((point[0] * bone->matrix[0][0] + point[1] * bone->matrix[1][0] + point[2] * bone->matrix[2][0])
               + bone->offset[0])
            * weight->boneWeight;

    out[1] += ((point[0] * bone->matrix[0][1] + point[1] * bone->matrix[1][1] + point[2] * bone->matrix[2][1])
               + bone->offset[1])
            * weight->boneWeight;

    out[2] += ((point[0] * bone->matrix[0][2] + point[1] * bone->matrix[1][2] + point[2] * bone->matrix[2][2])
               + bone->offset[2])
            * weight->boneWeight;
}

/*
=============
RE_GetSkinnedMesh

Added in OPM.
Skins every visible surface of a skeletal model the way RB_SkelMesh does, from
the same pose, but at full detail rather than the drawn LOD and out to world
space, for the client game to measure and trace against: ragdoll bullet hits
and fitting its collision to the model. Morphs are left out; they only move the
face. Each vertex also says which bone carries most of its weight.
=============
*/
int RE_GetSkinnedMesh(refEntity_t *model, skinnedVert_t *verts, int maxVerts, int *tris, int maxTris, int *numTris)
{
    dtiki_t         *tiki = model->tiki;
    skelAnimFrame_t *frame;
    const byte      *bsurf;
    float            scale;
    int              numVerts = 0;
    int              mesh, surf, i, k;

    *numTris = 0;

    if (!tiki || !tiki->numMeshes) {
        return 0;
    }

    frame = (skelAnimFrame_t *)ri.Hunk_AllocateTempMemory(
        sizeof(skelAnimFrame_t) + ri.TIKI_GetNumChannels(tiki) * sizeof(SkelMat4)
    );
    R_GetFrame(model, frame);

    scale = tiki->load_scale * model->scale;
    bsurf = &model->surfaces[0];

    for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelHeaderGame_t  *skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);
        skelSurfaceGame_t *surface;

        if (!skelmodel) {
            numVerts = 0;
            break;
        }

        surface = skelmodel->pSurfaces;
        for (surf = 0; surf < skelmodel->numSurfaces; surf++, bsurf++, surface = surface->pNext) {
            skeletorVertex_t *vert;

            // hidden, as R_AddSkelSurfaces treats it
            if (*bsurf & 4) {
                continue;
            }

            if (numVerts + surface->numVerts > maxVerts || *numTris + surface->numTriangles > maxTris) {
                numVerts = 0;
                *numTris = 0;
                goto done;
            }

            for (i = 0; i < surface->numTriangles * 3; i++) {
                tris[*numTris * 3 + i] = numVerts + surface->pTriangles[i];
            }
            *numTris += surface->numTriangles;

            vert = surface->pVerts;
            for (i = 0; i < surface->numVerts; i++) {
                skelWeight_t  *weight;
                skinnedVert_t *out = &verts[numVerts + i];
                vec3_t         local;
                float          heaviest = -1;

                weight = (skelWeight_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * vert->numMorphs);

                VectorClear(local);
                out->bone = -1;

                for (k = 0; k < vert->numWeights; k++, weight++) {
                    int boneNum;

                    if (mesh > 0) {
                        boneNum = ri.TIKI_GetLocalChannel(tiki, skelmodel->pBones[weight->boneIndex].channel);
                    } else {
                        boneNum = weight->boneIndex;
                    }

                    local[0] += (weight->offset[0] * frame->bones[boneNum][0][0]
                                 + weight->offset[1] * frame->bones[boneNum][1][0]
                                 + weight->offset[2] * frame->bones[boneNum][2][0] + frame->bones[boneNum][3][0])
                              * weight->boneWeight;
                    local[1] += (weight->offset[0] * frame->bones[boneNum][0][1]
                                 + weight->offset[1] * frame->bones[boneNum][1][1]
                                 + weight->offset[2] * frame->bones[boneNum][2][1] + frame->bones[boneNum][3][1])
                              * weight->boneWeight;
                    local[2] += (weight->offset[0] * frame->bones[boneNum][0][2]
                                 + weight->offset[1] * frame->bones[boneNum][1][2]
                                 + weight->offset[2] * frame->bones[boneNum][2][2] + frame->bones[boneNum][3][2])
                              * weight->boneWeight;

                    if (weight->boneWeight > heaviest) {
                        heaviest  = weight->boneWeight;
                        out->bone = boneNum;
                    }
                }

                VectorScale(local, scale, local);

                VectorCopy(model->origin, out->xyz);
                VectorMA(out->xyz, local[0], model->axis[0], out->xyz);
                VectorMA(out->xyz, local[1], model->axis[1], out->xyz);
                VectorMA(out->xyz, local[2], model->axis[2], out->xyz);

                vert = (skeletorVertex_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                            + sizeof(skeletorMorph_t) * vert->numMorphs
                                            + sizeof(skelWeight_t) * vert->numWeights);
            }

            numVerts += surface->numVerts;
        }
    }

done:
    ri.Hunk_FreeTempMemory(frame);

    if (!numVerts) {
        *numTris = 0;
    }

    return numVerts;
}

/*
=============
RB_SkelMesh
=============
*/
/*
=============
Skinning cache

Added in OPM. A pose (R_PoseSkelModel) is the same in every view of a scene,
so a surface skinned for one is copied into the others: the prepass, the sun
cascades and the realtime lights' shadow faces draw each character again, and
used to skin it again each time. Keyed by the pose, the surface and how many of
its vertexes the level of detail keeps; direct mapped, and started over when
full. A key never comes back, so nothing needs clearing between frames.
=============
*/
#define SKIN_CACHE_SLOTS 4096 // a power of two
#define SKIN_CACHE_VERTS (128 * 1024)

typedef struct {
    int                      poseId;
    const skelSurfaceGame_t *sf;
    unsigned int             count;
    int                      first;
    qboolean                 tangents;

    // Added in OPM: its copy on the card this frame (RB_SkinArenaStore), if
    // arenaFrame is RB_SkinArenaFrame()
    int                      arenaFrame;
    int                      arenaBase;
    int                      arenaFirstIndex;
    int                      arenaIndexes;
    qboolean                 arenaTangents;
} skinCacheSlot_t;

static skinCacheSlot_t skinSlots[SKIN_CACHE_SLOTS];
static vec4_t          skinXyz[SKIN_CACHE_VERTS];
static int16_t         skinNormal[SKIN_CACHE_VERTS][4];
static int16_t         skinTangent[SKIN_CACHE_VERTS][4];
static vec2_t          skinTexCoords[SKIN_CACHE_VERTS];
static int             skinUsed;

static skinCacheSlot_t *R_SkinCacheSlot(int poseId, const skelSurfaceGame_t *sf, unsigned int count)
{
    unsigned int h = (unsigned int)poseId * 2654435761u;

    h ^= (unsigned int)((uintptr_t)sf >> 4) * 40503u;
    h ^= count * 2246822519u;
    return &skinSlots[(h ^ (h >> 15)) & (SKIN_CACHE_SLOTS - 1)];
}

// 0: not kept; 1: copied whole; 2: copied but for the tangents, which it was
// kept without
static int R_SkinCacheFetch(int poseId, const skelSurfaceGame_t *sf, unsigned int count, int base, qboolean tangents)
{
    const skinCacheSlot_t *slot;

    if (!poseId || !r_skinCache->integer) {
        return 0;
    }

    slot = R_SkinCacheSlot(poseId, sf, count);
    if (slot->poseId != poseId || slot->sf != sf || slot->count != count) {
        return 0;
    }

    Com_Memcpy(tess.xyz[base], skinXyz[slot->first], count * sizeof(skinXyz[0]));
    Com_Memcpy(tess.normal[base], skinNormal[slot->first], count * sizeof(skinNormal[0]));
    Com_Memcpy(tess.texCoords[base], skinTexCoords[slot->first], count * sizeof(skinTexCoords[0]));
    if (!tangents) {
        return 1;
    }
    if (!slot->tangents) {
        return 2;
    }
    Com_Memcpy(tess.tangent[base], skinTangent[slot->first], count * sizeof(skinTangent[0]));
    return 1;
}

static void R_SkinCacheStore(int poseId, const skelSurfaceGame_t *sf, unsigned int count, int base, qboolean tangents)
{
    skinCacheSlot_t *slot;

    if (!poseId || !r_skinCache->integer || count > SKIN_CACHE_VERTS) {
        return;
    }

    slot = R_SkinCacheSlot(poseId, sf, count);
    if (slot->poseId == poseId && slot->sf == sf && slot->count == count) {
        // kept already, now with its tangents
        if (tangents && !slot->tangents) {
            Com_Memcpy(skinTangent[slot->first], tess.tangent[base], count * sizeof(skinTangent[0]));
            slot->tangents = qtrue;
        }
        return;
    }

    if (skinUsed + count > SKIN_CACHE_VERTS) {
        // full: start over, once a batch that takes a surface from it has
        // (RB_SkinMaterialize)
        RB_SkinMaterialize();
        Com_Memset(skinSlots, 0, sizeof(skinSlots));
        skinUsed = 0;
    }

    slot->poseId   = poseId;
    slot->sf       = sf;
    slot->count    = count;
    slot->first    = skinUsed;
    slot->tangents = tangents;
    slot->arenaFrame = 0;
    skinUsed += count;

    Com_Memcpy(skinXyz[slot->first], tess.xyz[base], count * sizeof(skinXyz[0]));
    Com_Memcpy(skinNormal[slot->first], tess.normal[base], count * sizeof(skinNormal[0]));
    Com_Memcpy(skinTexCoords[slot->first], tess.texCoords[base], count * sizeof(skinTexCoords[0]));
    if (tangents) {
        Com_Memcpy(skinTangent[slot->first], tess.tangent[base], count * sizeof(skinTangent[0]));
    }
}

/*
=============
Posed surfaces drawn from the card

Added in OPM. The skin cache keeps a posed surface for the frame; the frame
also keeps a copy of it on the card (RB_SkinArenaStore), which the views that
draw depth alone draw it from when it is a batch by itself
(RB_SkinArenaDraw). Such a batch need not hold the surface's vertexes at all:
RB_SkelMesh leaves them out, deferred, and RB_SkinMaterialize copies them in
from the skin cache only if the batch is drawn from tess after all.
=============
*/

static const skinCacheSlot_t *R_SkinCacheFind(int poseId, const skelSurfaceGame_t *sf, unsigned int count)
{
    const skinCacheSlot_t *slot;

    if (!poseId || !r_skinCache->integer) {
        return NULL;
    }
    slot = R_SkinCacheSlot(poseId, sf, count);
    if (slot->poseId != poseId || slot->sf != sf || slot->count != count) {
        return NULL;
    }
    return slot;
}

// The surface tess now holds at baseVertex: copied to the card if it is not
// there yet this frame, and if it is the batch's first, the batch told so.
static void RB_SkinArenaKeep(int poseId, const skelSurfaceGame_t *sf, unsigned int count, int baseVertex, int baseIndex, qboolean tangents)
{
    skinCacheSlot_t *slot = (skinCacheSlot_t *)R_SkinCacheFind(poseId, sf, count);
    const int        frame = RB_SkinArenaFrame();
    const int        numIndexes = tess.numIndexes - baseIndex;

    if (!slot || !frame) {
        return;
    }

    if (slot->arenaFrame != frame) {
        if (!RB_SkinArenaStore(baseVertex, count, baseIndex, numIndexes, tangents, &slot->arenaBase, &slot->arenaFirstIndex)) {
            return;
        }
        slot->arenaFrame    = frame;
        slot->arenaIndexes  = numIndexes;
        slot->arenaTangents = tangents;
    } else if (tangents && !slot->arenaTangents) {
        RB_SkinArenaAddTangents(slot->arenaBase, baseVertex, count);
        slot->arenaTangents = qtrue;
    }

    if (!baseVertex && !baseIndex && slot->arenaIndexes == numIndexes) {
        tess.skin.valid           = qtrue;
        tess.skin.deferred        = qfalse;
        tess.skin.tangents        = slot->arenaTangents;
        tess.skin.frame           = frame;
        tess.skin.numVertexes     = count;
        tess.skin.numIndexes      = numIndexes;
        tess.skin.arenaBase       = slot->arenaBase;
        tess.skin.arenaFirstIndex = slot->arenaFirstIndex;
    }
}

/*
=============
RB_SkinMaterialize

The vertexes of a surface RB_SkelMesh left out of tess, copied in from the
skin cache: the batch is drawn from tess after all.
=============
*/
void RB_SkinMaterialize(void)
{
    const int first = tess.skin.cacheFirst;
    const int count = tess.skin.numVertexes;

    if (!tess.skin.deferred) {
        return;
    }
    tess.skin.deferred = qfalse;

    Com_Memcpy(tess.xyz[0], skinXyz[first], count * sizeof(skinXyz[0]));
    Com_Memcpy(tess.normal[0], skinNormal[first], count * sizeof(skinNormal[0]));
    Com_Memcpy(tess.texCoords[0], skinTexCoords[first], count * sizeof(skinTexCoords[0]));
    if (tess.skin.cacheTangents) {
        Com_Memcpy(tess.tangent[0], skinTangent[first], count * sizeof(skinTangent[0]));
    }
}

void RB_SkelMesh(skelSurfaceGame_t *sf)
{
    qboolean           tangents;
    int                kept;
    unsigned int       baseIndex, baseVertex;
    unsigned int       render_count;
    unsigned int       indexes;
    float             *outXyz;
    int16_t           *outNormal;
    skelIndex_t       *triangles;
    skelIndex_t       *collapse_map;
    skeletorVertex_t  *newVerts;
    skeletorMorph_t   *morph;
    skelWeight_t      *weight;
    int                vertNum;
    int                morphNum;
    int                weightNum;
    skelBoneCache_t   *bones;
    skelBoneCache_t   *bone;
    int               *morphs;
    int               *morphcache;
    float              scale;
    dtiki_t           *tiki;
    int                mesh;
    int                surf;
    int                i;
    skelHeaderGame_t  *skelmodel;
    skelSurfaceGame_t *psurface;
    qboolean           bFound;
    short              collapse[TIKI_MAX_VERTEXES];

    if (!r_drawentitypoly->integer) {
        return;
    }

    tiki = backEnd.currentEntity->e.tiki;

    scale = tiki->load_scale * backEnd.currentEntity->e.scale;

    //
    // get the mesh associated with the surface
    //
    bFound = qfalse;
    for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);
        psurface  = skelmodel->pSurfaces;

        // find the surface
        for (surf = 0; surf < skelmodel->numSurfaces; surf++) {
            if (psurface == sf) {
                bFound = qtrue;
                break;
            }
            psurface = psurface->pNext;
        }

        if (bFound) {
            break;
        }
    }

    assert(bFound);

    //
    // Process LOD
    //
    if (skelmodel->pLOD) {
        float lod_val;
        int   renderfx;

        lod_val  = backEnd.currentEntity->lodpercentage[0];
        renderfx = backEnd.currentEntity->e.renderfx;

        if (sf->numVerts > 3) {
            skelIndex_t *collapseIndex;
            int          mid, low, high;
            int          lod_cutoff;

            if (lod_tool->integer && !strcmp(backEnd.currentEntity->e.tiki->a->name, lod_tikiname->string)
                && mesh == lod_mesh->integer) {
                lod_cutoff = GetToolLodCutoff(skelmodel, backEnd.currentEntity->lodpercentage[0]);
            } else {
                lod_cutoff = GetLodCutoff(skelmodel, backEnd.currentEntity->lodpercentage[0], renderfx);
            }

            collapseIndex = sf->pCollapseIndex;
            if (collapseIndex[2] < lod_cutoff) {
                return;
            }

            low = mid = 3;
            high      = sf->numVerts;
            while (high >= low) {
                mid = (low + high) >> 1;
                if (collapseIndex[mid] < lod_cutoff) {
                    high = mid - 1;
                    if (collapseIndex[mid - 1] >= lod_cutoff) {
                        break;
                    }
                } else {
                    mid++;
                    low = mid;
                    if (high == mid || collapseIndex[mid] < lod_cutoff) {
                        break;
                    }
                }
            }

            render_count = mid;
        } else {
            render_count = sf->numVerts;
        }

        if (!render_count) {
            return;
        }
    } else {
        render_count = sf->numVerts;
    }

    indexes = sf->numTriangles * 3;
    RB_CHECKOVERFLOW(render_count, indexes);

    collapse_map = sf->pCollapse;
    triangles    = sf->pTriangles;
    baseIndex    = tess.numIndexes;
    baseVertex   = tess.numVertexes;
    tess.numVertexes += render_count;

    outXyz    = tess.xyz[baseVertex];
    outNormal = tess.normal[baseVertex];
    newVerts  = sf->pVerts;

    if (render_count == sf->numVerts) {
        for (i = 0; i < indexes; i++) {
            tess.indexes[baseIndex + i] = baseVertex + triangles[i];
        }

        entityNumIndexes[backEnd.currentEntity - backEnd.refdef.entities] += indexes;
        tess.numIndexes += indexes;
    } else {
        assert(sf->numVerts < TIKI_MAX_VERTEXES);

        for (i = 0; i < render_count; i++) {
            collapse[i] = i;
        }

        for (i = render_count; i < sf->numVerts; i++) {
            collapse[i] = collapse[collapse_map[i]];
        }

        for (i = 0; i < indexes; i += 3) {
            assert(collapse[triangles[i]] < sf->numVerts);
            assert(collapse[triangles[i + 1]] < sf->numVerts);
            assert(collapse[triangles[i + 2]] < sf->numVerts);

            if (collapse[triangles[i]] == collapse[triangles[i + 1]]
                || collapse[triangles[i + 1]] == collapse[triangles[i + 2]]
                || collapse[triangles[i + 2]] == collapse[triangles[i]]) {
                break;
            }

            tess.indexes[baseIndex + i]     = baseVertex + collapse[triangles[i]];
            tess.indexes[baseIndex + i + 1] = baseVertex + collapse[triangles[i + 1]];
            tess.indexes[baseIndex + i + 2] = baseVertex + collapse[triangles[i + 2]];
        }

        entityNumIndexes[backEnd.currentEntity - backEnd.refdef.entities] += indexes;
        tess.numIndexes += i;
    }

    tangents = (tess.shader->vertexAttribs & ATTR_TANGENT) ? qtrue : qfalse;

    // Added in OPM
    //  The batch's first surface, on the card already this frame, in a view
    //  that draws depth alone: most likely drawn from there, so its vertexes
    //  are left out of tess unless it turns out not to be (RB_SkinMaterialize).
    if (!baseVertex && !baseIndex && backEnd.depthFill && tess.currentStageIteratorFunc == RB_StageIteratorGeneric
        && !ShaderRequiresCPUDeforms(tess.shader) && !r_showtris->integer && !r_shownormals->integer) {
        const skinCacheSlot_t *slot = R_SkinCacheFind(backEnd.currentEntity->poseId, sf, render_count);

        if (slot && slot->arenaFrame && slot->arenaFrame == RB_SkinArenaFrame()
            && slot->arenaIndexes == (int)tess.numIndexes && (!tangents || (slot->tangents && slot->arenaTangents))) {
            tess.skin.valid           = qtrue;
            tess.skin.deferred        = qtrue;
            tess.skin.tangents        = slot->arenaTangents;
            tess.skin.cacheTangents   = slot->tangents;
            tess.skin.frame           = slot->arenaFrame;
            tess.skin.numVertexes     = render_count;
            tess.skin.numIndexes      = tess.numIndexes;
            tess.skin.arenaBase       = slot->arenaBase;
            tess.skin.arenaFirstIndex = slot->arenaFirstIndex;
            tess.skin.cacheFirst      = slot->first;
            return;
        }
    }

    // skinned for another view of this scene already
    kept     = R_SkinCacheFetch(backEnd.currentEntity->poseId, sf, render_count, baseVertex, tangents);
    if (kept == 1) {
        RB_SkinArenaKeep(backEnd.currentEntity->poseId, sf, render_count, baseVertex, baseIndex, tangents);
        return;
    }
    if (kept == 2) {
        RB_CalcTangentsForRange(baseVertex, render_count, baseIndex, tess.numIndexes - baseIndex);
        R_SkinCacheStore(backEnd.currentEntity->poseId, sf, render_count, baseVertex, qtrue);
        RB_SkinArenaKeep(backEnd.currentEntity->poseId, sf, render_count, baseVertex, baseIndex, qtrue);
        return;
    }

    //
    // just copy the vertexes
    //
    bones  = &TIKI_Skel_Bones[backEnd.currentEntity->e.bonestart];
    morphs = &skeletorMorphCache[backEnd.currentEntity->e.morphstart];

    if (backEnd.currentEntity->e.hasMorph) {
        if (mesh > 0) {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;
                vec3_t totalmorph;
                int    channelNum;
                int    boneNum;

                VectorClear(out);
                VectorClear(outXyz);
                VectorClear(totalmorph);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);
                morph  = (skeletorMorph_t *)((byte *)newVerts + sizeof(skeletorVertex_t));

                for (morphNum = 0; morphNum < newVerts->numMorphs; morphNum++) {
                    morphcache = &morphs[morph->morphIndex];

                    if (*morphcache) {
                        SkelMorphGetXyz(morph, morphcache, totalmorph);
                    }

                    morph++;
                }

                if (newVerts->numMorphs) {
                    channelNum = skelmodel->pBones[morph->morphIndex].channel;
                } else {
                    channelNum = skelmodel->pBones[weight->boneIndex].channel;
                }

                boneNum = ri.TIKI_GetLocalChannel(tiki, channelNum);
                bone    = &bones[boneNum];

                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    channelNum = skelmodel->pBones[weight->boneIndex].channel;
                    boneNum    = ri.TIKI_GetLocalChannel(tiki, channelNum);
                    bone       = &bones[boneNum];

                    if (!weightNum) {
                        SkelWeightMorphGetXyz(weight, bone, totalmorph, out);
                    } else {
                        SkelWeightGetXyz(weight, bone, out);
                    }

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                tess.texCoords[baseVertex + vertNum][0] = newVerts->texCoords[0];
                tess.texCoords[baseVertex + vertNum][1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        } else {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;
                vec3_t totalmorph;

                VectorClear(out);
                VectorClear(outXyz);
                VectorClear(totalmorph);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);
                morph  = (skeletorMorph_t *)((byte *)newVerts + sizeof(skeletorVertex_t));

                for (morphNum = 0; morphNum < newVerts->numMorphs; morphNum++) {
                    morphcache = &morphs[morph->morphIndex];

                    if (*morphcache) {
                        SkelMorphGetXyz(morph, morphcache, totalmorph);
                    }

                    morph++;
                }

                if (newVerts->numMorphs) {
                    bone = &bones[morph->morphIndex];
                } else {
                    bone = &bones[weight->boneIndex];
                }

                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    bone = &bones[weight->boneIndex];

                    if (!weightNum) {
                        SkelWeightMorphGetXyz(weight, bone, totalmorph, out);
                    } else {
                        SkelWeightGetXyz(weight, bone, out);
                    }

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                tess.texCoords[baseVertex + vertNum][0] = newVerts->texCoords[0];
                tess.texCoords[baseVertex + vertNum][1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        }
    } else {
        if (mesh > 0) {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;
                int    channelNum;
                int    boneNum;

                VectorClear(out);
                VectorClear(outXyz);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);

                channelNum = skelmodel->pBones[weight->boneIndex].channel;
                boneNum    = ri.TIKI_GetLocalChannel(tiki, channelNum);
                bone       = &bones[boneNum];

                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    channelNum = skelmodel->pBones[weight->boneIndex].channel;
                    boneNum    = ri.TIKI_GetLocalChannel(tiki, channelNum);
                    bone       = &bones[boneNum];

                    SkelWeightGetXyz(weight, bone, out);

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                tess.texCoords[baseVertex + vertNum][0] = newVerts->texCoords[0];
                tess.texCoords[baseVertex + vertNum][1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        } else {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;

                VectorClear(out);
                VectorClear(outXyz);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);

                bone = &bones[weight->boneIndex];
                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    bone = &bones[weight->boneIndex];

                    SkelWeightGetXyz(weight, bone, out);

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                tess.texCoords[baseVertex + vertNum][0] = newVerts->texCoords[0];
                tess.texCoords[baseVertex + vertNum][1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        }
    }

#if 0
	if( backEnd.currentEntity->e.staticModelIndex ) {
		mstaticModel_t *sm;
		color4ub_t *out;
		color3ub_t *in;
		int cdofs;
		skdSurface_t *sf2;

		sm = &tr.world->staticModels[ backEnd.currentEntity->e.staticModelIndex - 1 ];

		tess.useStaticModelVertexColors = qtrue;

		cdofs = 0;
		sf2 = tiki->surfs;
		while( sf2 != sf ) {
			cdofs += sf2->numVerts;
			sf2 = ( skdSurface_t* )( ( ( byte* )sf2 ) + sf2->ofsEnd );
		}

		in = &tr.world->smColors[ sm->firstVert + cdofs ];
		out = tess.vertexColors + baseVertex;
		for( i = 0; i < sf->numVerts; i++, in++, out++ ) {
#    if 1
			( *out )[ 0 ] = ( *in )[ 0 ];
			( *out )[ 1 ] = ( *in )[ 1 ];
			( *out )[ 2 ] = ( *in )[ 2 ];
			( *out )[ 3 ] = 255;
#    elif 0
			( ( int* )out ) = tr.identityLightByte;
#    else		
			// su44: set it to something special so
			// I can debug vertex colors rendering
			( *out )[ 0 ] = 255;
			( *out )[ 1 ] = 0;
			( *out )[ 2 ] = 0;
			( *out )[ 3 ] = 255;
#    endif
		}
	} //else
#endif
    //{
    //	// an attemp to fix bizarre vertex colors bug
    //	color4ub_t *col;
    //
    //	col = &tess.vertexColors[baseVertex];
    //	for(i = 0; i < sf->numVerts; i++,col++) {
    //		(*col)[0] = 255;
    //		(*col)[1] = 255;
    //		(*col)[2] = 255;
    //		(*col)[3] = 255;
    //	}
    //}
    //tess.numVertexes += sf->numVerts;

    if (tangents) {
        // the indexes written: fewer than the surface's when the level of
        // detail dropped some
        RB_CalcTangentsForRange(baseVertex, render_count, baseIndex, tess.numIndexes - baseIndex);
    }
    R_SkinCacheStore(backEnd.currentEntity->poseId, sf, render_count, baseVertex, tangents);
    RB_SkinArenaKeep(backEnd.currentEntity->poseId, sf, render_count, baseVertex, baseIndex, tangents);
}

/*
=============
RB_StaticMesh
=============
*/
/*
=============
Static model vertex cache

Added in OPM. A static model's surface is built into tess in every pass that
draws it -- the depth prepass, each sun cascade, the main pass -- and every
frame, though it never changes: each normal packed again, and, for a shader
with a normal map (most props, since tools/matgen), its tangents worked out
again from every triangle. Those depend on the surface alone and on how many
of its vertexes its level of detail keeps, not on which copy of the model it
is or where it stands, so they are kept here once, keyed by both, and copied.
Direct mapped, started over when full, as the skin cache is.
=============
*/
#define STATIC_CACHE_SLOTS 4096 // a power of two
#define STATIC_CACHE_VERTS (256 * 1024)

typedef struct {
    const skelSurfaceGame_t *sf;
    int                      count;
    int                      first;
    qboolean                 tangents;
} staticCacheSlot_t;

static staticCacheSlot_t staticSlots[STATIC_CACHE_SLOTS];
static int16_t           staticNormal[STATIC_CACHE_VERTS][4];
static int16_t           staticTangent[STATIC_CACHE_VERTS][4];
static int               staticUsed;

static staticCacheSlot_t *R_StaticCacheSlot(const skelSurfaceGame_t *sf, int count)
{
    unsigned int h = (unsigned int)((uintptr_t)sf >> 4) * 2654435761u;

    h ^= (unsigned int)count * 40503u;
    return &staticSlots[(h ^ (h >> 15)) & (STATIC_CACHE_SLOTS - 1)];
}

void RB_StaticMesh(staticSurface_t *staticSurf)
{
    int                i, j;
    dtiki_t           *tiki;
    skelSurfaceGame_t *surf;
    int                meshNum;
    skelHeaderGame_t  *skelmodel;
    int                render_count;
    skelIndex_t       *collapse_map;
    skelIndex_t       *triangles;
    int                indexes;
    int                baseIndex, baseVertex;
    short              collapse[1000];

    if (!r_drawstaticmodelpoly->integer) {
        return;
    }

    assert(backEnd.currentStaticModel);
    tiki = backEnd.currentStaticModel->tiki;
    surf = staticSurf->surface;

    assert(surf->pStaticXyz);
    if (!surf->pStaticXyz) {
        return;
    }

    meshNum   = staticSurf->meshNum;
    skelmodel = ri.TIKI_GetSkel(tiki->mesh[meshNum]);

    //
    // Process LOD
    //
    if (skelmodel->pLOD && r_staticlod->integer) {
        float lod_val;

        lod_val = backEnd.currentStaticModel->lodpercentage[0];

        if (surf->numVerts > 3) {
            skelIndex_t *collapseIndex;
            int          mid, low, high;
            int          lod_cutoff;

            if (lod_tool->integer && !strcmp(backEnd.currentStaticModel->tiki->a->name, lod_tikiname->string)
                && meshNum == lod_mesh->integer) {
                lod_cutoff = GetToolLodCutoff(skelmodel, backEnd.currentStaticModel->lodpercentage[0]);
            } else {
                lod_cutoff = GetLodCutoff(skelmodel, backEnd.currentStaticModel->lodpercentage[0], 0);
            }

            collapseIndex = surf->pCollapseIndex;
            if (collapseIndex[2] < lod_cutoff) {
                return;
            }

            low = mid = 3;
            high      = surf->numVerts;
            while (high >= low) {
                mid = (low + high) >> 1;
                if (collapseIndex[mid] < lod_cutoff) {
                    high = mid - 1;
                    if (collapseIndex[mid - 1] >= lod_cutoff) {
                        break;
                    }
                } else {
                    mid++;
                    low = mid;
                    if (high == mid || collapseIndex[mid] < lod_cutoff) {
                        break;
                    }
                }
            }

            render_count = mid;
        } else {
            render_count = surf->numVerts;
        }

        if (!render_count) {
            return;
        }
    } else {
        render_count = surf->numVerts;
    }

    indexes = surf->numTriangles * 3;
    // ends a batch the VAO cache started, see RB_SurfaceVaoCached
    RB_CheckVao(tess.vao);
    RB_CHECKOVERFLOW(render_count, surf->numTriangles);

    collapse_map = surf->pCollapse;
    triangles    = surf->pTriangles;
    baseIndex    = tess.numIndexes;
    baseVertex   = tess.numVertexes;
    tess.numVertexes += render_count;

    if (render_count == surf->numVerts) {
        for (j = 0; j < indexes; j++) {
            tess.indexes[baseIndex + j] = baseVertex + triangles[j];
        }

        staticModelNumIndexes[backEnd.currentStaticModel - backEnd.refdef.staticModels] += indexes;
        tess.numIndexes += indexes;
    } else {
        for (i = 0; i < render_count; i++) {
            collapse[i] = i;
        }
        for (i = render_count; i < surf->numVerts; i++) {
            collapse[i] = collapse[collapse_map[i]];
        }

        for (j = 0; j < indexes; j += 3) {
            if (collapse[triangles[j]] == collapse[triangles[j + 1]]
                || collapse[triangles[j + 1]] == collapse[triangles[j + 2]]
                || collapse[triangles[j + 2]] == collapse[triangles[j]]) {
                break;
            }

            tess.indexes[baseIndex + j]     = baseVertex + collapse[triangles[j]];
            tess.indexes[baseIndex + j + 1] = baseVertex + collapse[triangles[j + 1]];
            tess.indexes[baseIndex + j + 2] = baseVertex + collapse[triangles[j + 2]];
        }

        staticModelNumIndexes[backEnd.currentStaticModel - backEnd.refdef.staticModels] += j;
        tess.numIndexes += j;
    }

    // Added in OPM: its normals packed, and its tangents, as kept from before
    staticCacheSlot_t *cached = NULL;
    {
        staticCacheSlot_t *slot = R_StaticCacheSlot(surf, render_count);

        if (slot->sf != surf || slot->count != render_count) {
            if (render_count > STATIC_CACHE_VERTS) {
                slot = NULL;
            } else {
                if (staticUsed + render_count > STATIC_CACHE_VERTS) {
                    // full: start over
                    Com_Memset(staticSlots, 0, sizeof(staticSlots));
                    staticUsed = 0;
                    slot = R_StaticCacheSlot(surf, render_count);
                }
                slot->sf       = surf;
                slot->count    = render_count;
                slot->first    = staticUsed;
                slot->tangents = qfalse;
                staticUsed += render_count;
                for (j = 0; j < render_count; j++) {
                    // tess.normal is packed int16 (32767 = 1.0), not float -- a plain
                    // copy of the float model normal truncates every component to
                    // 0/+-1, which zeroes out any normal-driven CPU deform (flap/wave)
                    // so foliage never sways.
                    R_VaoPackNormal(staticNormal[slot->first + j], surf->pStaticNormal[j]);
                }
            }
        }
        cached = slot;
    }

    if (cached) {
        Com_Memcpy(tess.normal[baseVertex], staticNormal[cached->first], render_count * sizeof(tess.normal[0]));
    } else {
        for (j = 0; j < render_count; j++) {
            R_VaoPackNormal(tess.normal[baseVertex + j], surf->pStaticNormal[j]);
        }
    }

    for (j = 0; j < render_count; j++) {
        Vector4Copy(surf->pStaticXyz[j], tess.xyz[baseVertex + j]);
        tess.texCoords[baseVertex + j][0]   = surf->pStaticTexCoords[j][0][0];
        tess.texCoords[baseVertex + j][1]   = surf->pStaticTexCoords[j][0][1];
        tess.lightCoords[baseVertex + j][0] = surf->pStaticTexCoords[j][1][0];
        tess.lightCoords[baseVertex + j][1] = surf->pStaticTexCoords[j][1][1];
    }

    if (backEndData->staticModelData) {
        const size_t offset =
            backEnd.currentStaticModel->firstVertexData + staticSurf->ofsStaticData * sizeof(color4ub_t);
        assert(offset < tr.world->numStaticModelData * sizeof(color4ub_t));
        assert(offset + render_count * sizeof(color4ub_t) <= tr.world->numStaticModelData * sizeof(color4ub_t));

        const color4ub_t *in = (const color4ub_t *)&backEndData->staticModelData[offset];

        for (i = 0; i < render_count; i++) {
            tess.color[baseVertex + i][0] = in[i][0] * 0xffff / 0xff;
            tess.color[baseVertex + i][1] = in[i][1] * 0xffff / 0xff;
            tess.color[baseVertex + i][2] = in[i][2] * 0xffff / 0xff;
            tess.color[baseVertex + i][3] = in[i][3] * 0xffff / 0xff;
        }
    } else {
        for (i = 0; i < render_count; i++) {
            tess.color[baseVertex + i][0] = 0xffff;
            tess.color[baseVertex + i][1] = 0xffff;
            tess.color[baseVertex + i][2] = 0xffff;
            tess.color[baseVertex + i][3] = 0xffff;
        }
    }

    // Added in OPM
    //  Its tangents: none in a pass that draws depth alone, which reads none;
    //  else as kept, or worked out once and kept.
    if ((tess.shader->vertexAttribs & ATTR_TANGENT) && !backEnd.depthFill
        && !(backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))) {
        if (cached && cached->tangents) {
            Com_Memcpy(tess.tangent[baseVertex], staticTangent[cached->first], render_count * sizeof(tess.tangent[0]));
        } else {
            RB_CalcTangentsForRange(baseVertex, render_count, baseIndex, tess.numIndexes - baseIndex);
            if (cached) {
                Com_Memcpy(staticTangent[cached->first], tess.tangent[baseVertex], render_count * sizeof(tess.tangent[0]));
                cached->tangents = qtrue;
            }
        }
    }
}

/*
=============
R_InfoWorldTris_f
=============
*/
void R_InfoWorldTris_f(void)
{
    int i;

    g_bInfoworldtris = qtrue;

    for (i = 0; i < ARRAY_LEN(entityNumIndexes); i++) {
        entityNumIndexes[i] = 0;
    }
    for (i = 0; i < ARRAY_LEN(staticModelNumIndexes); i++) {
        staticModelNumIndexes[i] = 0;
    }
}

/*
=============
R_PrintInfoWorldtris
=============
*/
void R_PrintInfoWorldtris(void)
{
    int               i;
    int               numTris;
    int               totalNumTris;
    dtiki_t          *tiki;
    skelHeaderGame_t *skelmodel;

    totalNumTris = 0;

    for (i = 0; i < ARRAY_LEN(entityNumIndexes); i++) {
        numTris = entityNumIndexes[i] / 3;
        if (!numTris) {
            continue;
        }

        totalNumTris += numTris;
        tiki      = backEnd.refdef.entities[i].e.tiki;
        skelmodel = ri.TIKI_GetSkel(tiki->mesh[0]);
        Com_Printf("ent: %i, tris: %i, %s, version: %i\n", i, numTris, tiki->a->name, skelmodel->version);
    }

    Com_Printf("total entity tris: %i\n\n", totalNumTris);

    totalNumTris = 0;

    for (i = 0; i < ARRAY_LEN(entityNumIndexes); i++) {
        numTris = staticModelNumIndexes[i] / 3;
        if (!numTris) {
            continue;
        }

        totalNumTris += numTris;
        tiki      = backEnd.refdef.staticModels[i].tiki;
        skelmodel = ri.TIKI_GetSkel(tiki->mesh[0]);
        Com_Printf("sm: %i, tris: %i, %s, version: %i\n", i, numTris, tiki->a->name, skelmodel->version);
    }

    Com_Printf("total static model tris: %i\n\n", totalNumTris);
}

/*
=============
RE_SetFrameNumber
=============
*/
void RE_SetFrameNumber(int frameNumber)
{
    tr.frame_skel_index = frameNumber;
}

/*
=============
R_UpdatePoseInternal
=============
*/
void R_UpdatePoseInternal(refEntity_t *model)
{
    if (model->entityNumber != ENTITYNUM_NONE) {
        if (tr.skel_index[model->entityNumber] == tr.frame_skel_index) {
            return;
        }
        tr.skel_index[model->entityNumber] = tr.frame_skel_index;
    }

    ri.TIKI_SetPoseInternal(
        ri.TIKI_GetSkeletor(model->tiki, model->entityNumber),
        model->frameInfo,
        model->bone_tag,
        model->bone_quat,
        model->actionWeight,
        model->bone_override,
        model->num_bone_overrides
    );
}

/*
=============
RE_ForceUpdatePose
=============
*/
void RE_ForceUpdatePose(refEntity_t *model)
{
    if (model->entityNumber != ENTITYNUM_NONE) {
        tr.skel_index[model->entityNumber] = tr.frame_skel_index;
    }

    ri.TIKI_SetPoseInternal(
        ri.TIKI_GetSkeletor(model->tiki, model->entityNumber),
        model->frameInfo,
        model->bone_tag,
        model->bone_quat,
        model->actionWeight,
        model->bone_override,
        model->num_bone_overrides
    );
}

/*
=============
RE_TIKI_Orientation
=============
*/
orientation_t RE_TIKI_Orientation(refEntity_t *model, int tagnum)
{
    R_UpdatePoseInternal(model);
    return ri.TIKI_OrientationInternal(model->tiki, model->entityNumber, tagnum, model->scale);
}

/*
=============
RE_TIKI_IsOnGround
=============
*/
qboolean RE_TIKI_IsOnGround(refEntity_t *model, int tagnum, float threshold)
{
    R_UpdatePoseInternal(model);
    return ri.TIKI_IsOnGroundInternal(model->tiki, model->entityNumber, tagnum, threshold);
}

/*
=============
R_GetRadius
=============
*/
float R_GetRadius(refEntity_t *model)
{
    R_UpdatePoseInternal(model);
    return ri.GetRadiusInternal(model->tiki, model->entityNumber, model->scale);
}

/*
=============
R_GetFrame
=============
*/
void R_GetFrame(refEntity_t *model, struct skelAnimFrame_s *newFrame)
{
    R_UpdatePoseInternal(model);
    ri.GetFrameInternal(model->tiki, model->entityNumber, newFrame);
}

/*
=============
R_DebugSkeleton
=============
*/
void R_DebugSkeleton(void)
{
    // FIXME: unimplemented (GL2)
}

/*
=============
ProjectRadius
=============
*/
static float ProjectRadius(float r, const vec3_t location)
{
    Vector separation;
    float  projectedRadius;

    separation      = Vector(tr.viewParms.ori.origin) - Vector(location);
    projectedRadius = separation.length();

    return fabs(r) * (100.0 / tr.viewParms.fovX) / projectedRadius;
}

/*
=============
R_CalcLod
=============
*/
float R_CalcLod(const vec3_t origin, float radius)
{
    return ProjectRadius(radius, origin);
}

/*
=============
R_CullTIKI
=============
*/
static int R_CullSkelModel(dtiki_t *tiki, refEntity_t *e, skelAnimFrame_t *newFrame, float fScale, float *vLocalOrg)
{
    vec3_t bounds[2];
    vec3_t delta;
    int    i;
    int    cull;

    // FIXME: not working properly
    return CULL_IN;

    if (tr.currentEntity->e.renderfx & RF_FRAMELERP) {
        VectorSubtract(e->origin, e->oldorigin, delta);
    } else {
        VectorClear(delta);
    }

    for (i = 0; i < 3; i++) {
        bounds[0][i] = newFrame->bounds[0][i] * fScale + vLocalOrg[i];
        bounds[1][i] = newFrame->bounds[1][i] * fScale + vLocalOrg[i];

        if (delta[i] > 0) {
            bounds[1][i] += delta[i];
        } else {
            bounds[0][i] += delta[i];
        }
    }

    cull = R_CullLocalBox(bounds);

    if (r_showcull->integer & 1) {
        float  fR, fG, fB;
        vec3_t vAngles;

        switch (cull) {
        case CULL_IN:
            fR = 0;
            fG = 1;
            fB = 0;
            break;
        case CULL_CLIP:
            fR = 1;
            fG = 1;
            fB = 0;
            break;
        case CULL_OUT:
            fR = 1;
            fG = 0.2f;
            fB = 0.2f;

            for (i = 0; i < 3; i++) {
                bounds[0][i] -= 16;
                bounds[1][i] += 16;
            }
            break;
        }

        MatrixToEulerAngles(tr.ori.axis, vAngles);
        R_DebugRotatedBBox(tr.ori.origin, vAngles, bounds[0], bounds[1], fR, fG, fB, 0.5);
    }

    switch (cull) {
    case CULL_IN:
        tr.pc.c_box_cull_md3_in++;
        return CULL_IN;
    case CULL_CLIP:
        tr.pc.c_box_cull_md3_clip++;
        return CULL_CLIP;
    case CULL_OUT:
    default:
        tr.pc.c_box_cull_md3_out++;
        return CULL_OUT;
    }
}

/*
==================
R_CountTikiLodTris

Computes the number of triangles to be rendered for the given TIKI model
based on the given LoD percentage, and passes it back with render_tris.
The total number of triangles in the TIKI model are passed back with total_tris.
Currently only used for debugging purposes.

FIXME: Shares some code with RB_StaticMesh, common parts could be extracted.
According to debug symbols, this was originally in tiki_mesh.cpp
==================
*/
void R_CountTikiLodTris(dtiki_t *tiki, float lodpercentage, int *render_tris, int *total_tris)
{
    *render_tris = 0;
    *total_tris  = 0;
    int numtris = 0, totaltris = 0;

    for (int i = 0; i < tiki->numMeshes; i++) {
        skelHeaderGame_t  *skelmodel = ri.TIKI_GetSkel(tiki->mesh[i]);
        skelSurfaceGame_t *surface   = skelmodel->pSurfaces;

        for (int j = 0; j < skelmodel->numSurfaces; j++) {
            int render_count = 0;
            if (skelmodel->pLOD) {
                int          lod_cutoff    = GetLodCutoff(skelmodel, lodpercentage, 0);
                skelIndex_t *collapseIndex = surface->pCollapseIndex;
                render_count               = surface->numVerts;

                // Determine the number of vertices to be rendered based on the LOD cutoff
                while (render_count > 0 && collapseIndex[render_count - 1] < lod_cutoff) {
                    render_count--;
                }
            } else {
                // The surf mesh doesn't have a LOD model, so all vertices will be rendered
                render_count = surface->numVerts;
            }

            skelIndex_t *triangles    = surface->pTriangles;
            int          indexes      = surface->numTriangles * 3; // 3 vertex/index for each tri
            skelIndex_t *collapse_map = surface->pCollapse;
            totaltris += surface->numTriangles;
            skelIndex_t collapse[4096] {};

            // Initialize array for collapsed indices
            int k;
            for (k = 0; k < render_count; ++k) {
                collapse[k] = k;
            }

            // Map remaining vertices to their collapsed indices using the collapse map
            for (k = render_count; k < surface->numVerts; ++k) {
                collapse[k] = collapse[collapse_map[k]];
            }

            // Check the first two collapsed indices of each triangle
            for (k = 0; k < indexes; k += 3) {
                if (collapse[triangles[k]] == collapse[triangles[k + 1]]) {
                    // The collapsed indices of the two vertices are the same:
                    // this, and any subsequent tris do not need to be rendered
                    // because the collapsed vertices (for this surface) coincide from here.
                    break;
                }
            }

            surface = surface->pNext;
            numtris += k / 3;
        }
    }

    *render_tris = numtris;
    *total_tris  = totaltris;
}

/*
==================
R_LerpTag
==================
*/
int R_LerpTag(orientation_t *tag, qhandle_t handle, int startFrame, int endFrame, float frac, const char *tagName)
{
    // stub
    return 0;
}
/*
=============
R_RtIsCharacter

Added in OPM. Is the entity a character (a skinned body): the realtime lights
(tr_rtlight.c) may leave those out of their shadows.
=============
*/
qboolean R_RtIsCharacter(const refEntity_t *ent)
{
    const model_t *model = R_GetModelByHandle(ent->hModel);

    return (model && model->type == MOD_TIKI && model->d.tiki && model->d.tiki->a && model->d.tiki->a->bIsCharacter) ? qtrue : qfalse;
}

/*
=============
R_RtCharacterCapsules

Added in OPM. A character's body as capsules, in the world: out[i * 2] is one
end and the radius, out[i * 2 + 1] the other. The realtime lights
(tr_rtlight.c) shade with them, which costs nothing like drawing the body
again into every shadow face it is in. Returns how many.

Each capsule is fitted once per model to the vertices a bone moves most: along
the bone as far as they reach, as thick as they spread around it. So they
follow the mesh, whatever the skeleton.
=============
*/
#define RT_BODY_CAPSULES 16

typedef struct {
    int    bone;
    vec3_t a, b; // in the bone's space
    float  radius;
} rtBodyCapsule_t;

typedef struct {
    dtiki_t        *tiki;
    int             numCapsules;
    rtBodyCapsule_t capsules[RT_BODY_CAPSULES];
} rtBody_t;

static rtBody_t rt_bodies[64];
static int      rt_numBodies;

// A capsule around points: along the way they spread the most, as long as they
// reach and as thick as they are around it. False if they make no capsule.
static bool R_RtFitCapsule(const std::vector<float> &pts, rtBodyCapsule_t *c)
{
    const size_t n = pts.size() / 3;
    double       mean[3] = {0, 0, 0}, cov[3][3] = {{0}};
    double       axis[3] = {0, 0, 0}, len, sumD2 = 0;
    float        lo = 1e9f, hi = -1e9f, radius;
    size_t       v;
    int          i, j, it, widest = 0;

    for (v = 0; v < n; v++) {
        for (i = 0; i < 3; i++) {
            mean[i] += pts[v * 3 + i];
        }
    }
    for (i = 0; i < 3; i++) {
        mean[i] /= n;
    }
    for (v = 0; v < n; v++) {
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                cov[i][j] += (pts[v * 3 + i] - mean[i]) * (pts[v * 3 + j] - mean[j]);
            }
        }
    }

    // the most spread: power iteration, from the widest of the three axes
    for (i = 1; i < 3; i++) {
        if (cov[i][i] > cov[widest][widest]) {
            widest = i;
        }
    }
    axis[widest] = 1;
    for (it = 0; it < 32; it++) {
        double next[3] = {0, 0, 0};

        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                next[i] += cov[i][j] * axis[j];
            }
        }
        len = sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (len < 1e-9) {
            break;
        }
        for (i = 0; i < 3; i++) {
            axis[i] = next[i] / len;
        }
    }

    for (v = 0; v < n; v++) {
        double d[3], t = 0, d2 = 0;

        for (i = 0; i < 3; i++) {
            d[i] = pts[v * 3 + i] - mean[i];
            t += d[i] * axis[i];
        }
        for (i = 0; i < 3; i++) {
            const double perp = d[i] - axis[i] * t;
            d2 += perp * perp;
        }
        sumD2 += d2;
        lo = Q_min(lo, (float)t);
        hi = Q_max(hi, (float)t);
    }

    // the vertices are on the skin: their distance from the axis is its radius
    radius = (float)sqrt(sumD2 / n);
    if (radius < 1.0f) {
        return false;
    }
    // the ends are round, and reach that far past the core
    lo += radius * 0.5f;
    hi -= radius * 0.5f;
    if (hi < lo) {
        lo = hi = (lo + hi) * 0.5f;
    }
    for (i = 0; i < 3; i++) {
        c->a[i] = (float)(mean[i] + axis[i] * lo);
        c->b[i] = (float)(mean[i] + axis[i] * hi);
    }
    c->radius = radius;
    return true;
}

static const rtBody_t *R_RtBody(dtiki_t *tiki)
{
    rtBody_t                        *body;
    std::vector<std::vector<float> > points; // each bone's heaviest vertices, in its space
    std::vector<int>                 order;
    int                              numChannels, mesh, surf, i, k;

    for (k = 0; k < rt_numBodies; k++) {
        if (rt_bodies[k].tiki == tiki) {
            return &rt_bodies[k];
        }
    }

    // a small cache: start over when full
    if (rt_numBodies == (int)(sizeof(rt_bodies) / sizeof(rt_bodies[0]))) {
        rt_numBodies = 0;
    }
    body              = &rt_bodies[rt_numBodies++];
    body->tiki        = tiki;
    body->numCapsules = 0;

    numChannels = ri.TIKI_GetNumChannels(tiki);
    if (numChannels <= 0) {
        return body;
    }
    points.resize(numChannels);

    for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelHeaderGame_t  *skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);
        skelSurfaceGame_t *surface;

        if (!skelmodel) {
            continue;
        }

        surface = skelmodel->pSurfaces;
        for (surf = 0; surf < skelmodel->numSurfaces; surf++, surface = surface->pNext) {
            skeletorVertex_t *vert = surface->pVerts;

            for (i = 0; i < surface->numVerts; i++) {
                skelWeight_t *weight = (skelWeight_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                                        + sizeof(skeletorMorph_t) * vert->numMorphs);
                skelWeight_t *heaviest = NULL;
                int           bone;

                for (k = 0; k < vert->numWeights; k++) {
                    if (!heaviest || weight[k].boneWeight > heaviest->boneWeight) {
                        heaviest = &weight[k];
                    }
                }

                if (heaviest) {
                    bone = mesh > 0 ? ri.TIKI_GetLocalChannel(tiki, skelmodel->pBones[heaviest->boneIndex].channel)
                                    : heaviest->boneIndex;
                    if (bone >= 0 && bone < numChannels) {
                        points[bone].insert(points[bone].end(), heaviest->offset, heaviest->offset + 3);
                    }
                }

                vert = (skeletorVertex_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                            + sizeof(skeletorMorph_t) * vert->numMorphs
                                            + sizeof(skelWeight_t) * vert->numWeights);
            }
        }
    }

    // the bones that move the most of the body
    std::vector<rtBodyCapsule_t> fitted;
    std::vector<float>           area;

    for (i = 0; i < numChannels; i++) {
        rtBodyCapsule_t c;

        if (points[i].size() >= 8 * 3 && R_RtFitCapsule(points[i], &c)) {
            c.bone = i;
            fitted.push_back(c);
            // what of the body it hides: its outline, side on
            area.push_back((Distance(c.a, c.b) + 2 * c.radius) * 2 * c.radius);
            order.push_back((int)order.size());
        }
    }
    // the parts that hide the most, not the ones with the most vertices (the
    // face and the hands have plenty and hide little)
    std::sort(order.begin(), order.end(), [&](int x, int y) { return area[x] > area[y]; });

    for (size_t n = 0; n < order.size() && body->numCapsules < RT_BODY_CAPSULES; n++) {
        body->capsules[body->numCapsules++] = fitted[order[n]];
    }

    return body;
}

int R_RtCharacterCapsules(refEntity_t *ent, vec4_t *out, int max)
{
    const rtBody_t  *body;
    skelAnimFrame_t *frame;
    float            scale;
    int              count = 0, i, k;

    if (!ent->tiki || max <= 0) {
        return 0;
    }

    body = R_RtBody(ent->tiki);
    if (!body->numCapsules) {
        return 0;
    }

    frame = (skelAnimFrame_t *)ri.Hunk_AllocateTempMemory(
        sizeof(skelAnimFrame_t) + ri.TIKI_GetNumChannels(ent->tiki) * sizeof(SkelMat4)
    );
    R_GetFrame(ent, frame);
    scale = ent->tiki->load_scale * ent->scale;

    for (i = 0; i < body->numCapsules && count < max; i++) {
        const rtBodyCapsule_t *c     = &body->capsules[i];
        const SkelMat4        &bone  = frame->bones[c->bone];
        const float           *ends[2] = {c->a, c->b};

        for (k = 0; k < 2; k++) {
            const float *p = ends[k];
            vec3_t       local;
            int          j;

            for (j = 0; j < 3; j++) {
                local[j] = (p[0] * bone[0][j] + p[1] * bone[1][j] + p[2] * bone[2][j] + bone[3][j]) * scale;
            }
            VectorCopy(ent->origin, out[count * 2 + k]);
            for (j = 0; j < 3; j++) {
                VectorMA(out[count * 2 + k], local[j], ent->axis[j], out[count * 2 + k]);
            }
        }
        out[count * 2][3]     = c->radius * scale;
        out[count * 2 + 1][3] = 0;
        count++;
    }

    ri.Hunk_FreeTempMemory(frame);
    return count;
}
