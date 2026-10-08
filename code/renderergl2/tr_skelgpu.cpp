/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

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

// tr_skelgpu.cpp -- skeletal models posed by the vertex program
//
// Added in OPM. RB_SkelMesh skins a model's surfaces on the CPU: every
// vertex, each of its weights by its bone, then copied into tess and on to the
// card, in the first view of a frame that draws it, and copied again in each
// view after. With people moving in view that was the largest part of the
// frame. Here a surface goes to the card once, as the model has it (each
// vertex's weights: where it is by each bone, and how much), the frame's bones
// go to the card once a scene (tr.skelBoneImage), and the vertex program
// poses the surface from them, and lights it as MOH:AA lights a model
// (RB_Light_Real, RB_CalcLightGridColor). Nothing of it is skinned or copied
// on the CPU but the indexes the level of detail keeps.
//
// What the program cannot do exactly as the CPU does is left to it: a vertex
// with more than eight weights, a model whose face moves (morphs), shaders that
// are not one lightall stage after another or that the CPU deforms or lights
// per vertex, fog, the debug views.

#include "tr_local.h"
#include "../tiki/tiki_mesh.h"

static cvar_t *r_skelGpu;

#define SKEL_GPU_WEIGHTS 8

// a vertex as the model has it
typedef struct {
    vec4_t weights[SKEL_GPU_WEIGHTS]; // where it is by each bone, how much of it the bone has
    float  bones[SKEL_GPU_WEIGHTS];   // the model's bones, the normal's first
    vec3_t normal;                    // as the first bone holds it
    vec2_t st;
} skelGpuVert_t;

typedef struct {
    const dtiki_t           *tiki;
    const skelSurfaceGame_t *sf;
    qboolean                 ok;        // the vertex program can pose it
    qboolean                 hasMorphs; // a vertex of it moves with the face
    const char              *why;       // not ok: why not
    int                      maxWeights;
    vao_t                    vao;
} skelGpuMesh_t;

// why a surface was left to the CPU (backEnd.pc.c_skelGpuCpu)
typedef enum {
    SKEL_CPU_OFF,      // r_skelGpu 0, or the card cannot
    SKEL_CPU_VIEW,     // fog, dynamic or personal shadow passes, debug views
    SKEL_CPU_SHADER,
    SKEL_CPU_MESH,
    SKEL_CPU_MORPHS,
    SKEL_CPU_LIGHTING,
    SKEL_CPU_COUNT
} skelGpuWhy_t;

static const char *const skelCpuNames[SKEL_CPU_COUNT] = { "off", "view", "shader", "mesh", "morphs", "lighting" };

// the shaders it was not for, and why, for skelgpuinfo
#define SKEL_GPU_REFUSED 64
static struct {
    const shader_t *shader;
    char            why[96];
} skelRefused[SKEL_GPU_REFUSED];
static int skelNumRefused;

#define SKEL_GPU_MESHES 4096 // a power of two

static skelGpuMesh_t *skelGpuMeshes[SKEL_GPU_MESHES];
static int            skelGpuNumMeshes;

// the bones on the card: up to which of them, for which scene
static int skelBonesSent;
static int skelBonesScene = -1;

void R_SkelGpuRegisterCvars(void)
{
    // 1: skeletal models are posed and lit by the vertex program; 0: on the CPU
    r_skelGpu = ri.Cvar_Get("r_skelGpu", "1", CVAR_ARCHIVE);
}

static qboolean R_SkelGpuAvailable(void)
{
    return r_skelGpu && r_skelGpu->integer && tr.skelBoneImage && tess.stream && glRefConfig.vertexArrayObject;
}

/*
================
R_SkelGpuInitImage

The bones, as TIKI_Skel_Bones has them: where the bone is, then its three
axes, a texel each, SKEL_GPU_BONE_ROW texels a row. Fetched texel by texel,
so the program needs GLSL 1.30.
================
*/
void R_SkelGpuInitImage(void)
{
    const int rows = (MAX_SKELBONES * 4 + SKEL_GPU_BONE_ROW - 1) / SKEL_GPU_BONE_ROW;

    tr.skelBoneImage = NULL;
    skelBonesScene   = -1;
    if (glRefConfig.glslMajorVersion < 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion < 30)) {
        return;
    }

    tr.skelBoneImage = R_CreateImage(
        "*skelbones", NULL, SKEL_GPU_BONE_ROW, rows, IMGTYPE_COLORALPHA, (imgFlags_t)(IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE), GL_RGBA32F
    );
    qglTextureParameterfEXT(tr.skelBoneImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    qglTextureParameterfEXT(tr.skelBoneImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

// The bones up to end on the card, those of the scene the front end posed
// last: what RB_SkelMesh would read on the CPU now.
static void RB_SkelGpuSendBones(int end)
{
    int t0, t1;

    if (skelBonesScene != tr.sceneCount) {
        skelBonesScene = tr.sceneCount;
        skelBonesSent  = 0;
    }

    // all posed so far, while about it
    end = Q_min(Q_max(end, TIKI_Skel_Bones_Index), MAX_SKELBONES);
    if (end <= skelBonesSent) {
        return;
    }

    t0 = skelBonesSent * 4;
    t1 = end * 4;
    while (t0 < t1) {
        const int row = t0 / SKEL_GPU_BONE_ROW;
        const int x   = t0 - row * SKEL_GPU_BONE_ROW;
        int       w, h;

        if (x || t1 - t0 < SKEL_GPU_BONE_ROW) {
            // part of a row
            w = Q_min(SKEL_GPU_BONE_ROW - x, t1 - t0);
            h = 1;
        } else {
            // whole rows
            w = SKEL_GPU_BONE_ROW;
            h = (t1 - t0) / SKEL_GPU_BONE_ROW;
        }
        qglTextureSubImage2DEXT(
            tr.skelBoneImage->texnum, GL_TEXTURE_2D, 0, x, row, w, h, GL_RGBA, GL_FLOAT, (const float *)TIKI_Skel_Bones + (size_t)t0 * 4
        );
        t0 += w * h;
    }
    skelBonesSent = end;
}

/*
================
The surfaces on the card
================
*/

static skelGpuMesh_t **R_SkelGpuSlot(const dtiki_t *tiki, const skelSurfaceGame_t *sf)
{
    unsigned int h = (unsigned int)((uintptr_t)tiki >> 4) * 2654435761u ^ (unsigned int)((uintptr_t)sf >> 4) * 40503u;
    int          i;

    for (i = 0; i < SKEL_GPU_MESHES; i++) {
        skelGpuMesh_t **slot = &skelGpuMeshes[(h + i) & (SKEL_GPU_MESHES - 1)];

        if (!*slot || ((*slot)->tiki == tiki && (*slot)->sf == sf)) {
            return slot;
        }
    }
    return NULL;
}

static void R_SkelGpuAttrib(vao_t *vao, int attribIndex, int count, int offset)
{
    vaoAttrib_t *vAtb = &vao->attribs[attribIndex];

    vAtb->enabled    = 1;
    vAtb->count      = count;
    vAtb->type       = GL_FLOAT;
    vAtb->normalized = GL_FALSE;
    vAtb->stride     = sizeof(skelGpuVert_t);
    vAtb->offset     = offset;
}

// A surface as the model has it, to the card: not if a vertex of it has more
// than four weights, or a bone the model does not.
static void R_SkelGpuBuild(skelGpuMesh_t *m, dtiki_t *tiki, skelSurfaceGame_t *sf, int mesh, skelHeaderGame_t *skelmodel)
{
    skelGpuVert_t    *verts;
    skeletorVertex_t *v = sf->pVerts;
    vao_t            *vao = &m->vao;
    int               i, k;

    m->ok  = qfalse;
    m->why = "no vertexes";
    if (sf->numVerts <= 0) {
        return;
    }

    verts = (skelGpuVert_t *)ri.Malloc(sf->numVerts * sizeof(skelGpuVert_t));
    Com_Memset(verts, 0, sf->numVerts * sizeof(skelGpuVert_t));

    for (i = 0; i < sf->numVerts; i++) {
        skelGpuVert_t      *out    = &verts[i];
        const skelWeight_t *weight = (const skelWeight_t *)((byte *)v + sizeof(skeletorVertex_t) + sizeof(skeletorMorph_t) * v->numMorphs);

        m->maxWeights = Q_max(m->maxWeights, v->numWeights);
        if (v->numWeights < 1 || v->numWeights > SKEL_GPU_WEIGHTS) {
            m->why = v->numWeights < 1 ? "a vertex with no weights" : "a vertex with more than eight weights";
            ri.Free(verts);
            return;
        }
        if (v->numMorphs) {
            m->hasMorphs = qtrue;
        }

        for (k = 0; k < v->numWeights; k++) {
            int bone = weight[k].boneIndex;

            // a mesh past the model's first names its bones by channel (RB_SkelMesh)
            if (mesh > 0) {
                if (bone < 0 || bone >= skelmodel->numBones) {
                    m->why = "a bone past the mesh's";
                    ri.Free(verts);
                    return;
                }
                bone = ri.TIKI_GetLocalChannel(tiki, skelmodel->pBones[bone].channel);
            }
            if (bone < 0) {
                m->why = "a bone the model does not have";
                ri.Free(verts);
                return;
            }
            VectorCopy(weight[k].offset, out->weights[k]);
            out->weights[k][3] = weight[k].boneWeight;
            out->bones[k]      = bone;
        }
        for (; k < SKEL_GPU_WEIGHTS; k++) {
            out->bones[k] = out->bones[0];
        }
        VectorCopy(v->normal, out->normal);
        out->st[0] = v->texCoords[0];
        out->st[1] = v->texCoords[1];

        v = (skeletorVertex_t *)((byte *)v + sizeof(skeletorVertex_t) + sizeof(skeletorMorph_t) * v->numMorphs
                                 + sizeof(skelWeight_t) * v->numWeights);
    }

    Q_strncpyz(vao->name, "skelGpu_VAO", sizeof(vao->name));
    qglGenVertexArrays(1, &vao->vao);
    qglBindVertexArray(vao->vao);
    glState.currentVao = vao;

    vao->vertexesSize = sf->numVerts * sizeof(skelGpuVert_t);
    qglGenBuffers(1, &vao->vertexesVBO);
    qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
    qglBufferData(GL_ARRAY_BUFFER, vao->vertexesSize, verts, GL_STATIC_DRAW);
    ri.Free(verts);

    // the arrays the vertex program reads the weights from (lightall_vp.glsl)
    R_SkelGpuAttrib(vao, ATTR_INDEX_POSITION, 4, offsetof(skelGpuVert_t, weights[0]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_POSITION2, 4, offsetof(skelGpuVert_t, weights[1]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_NORMAL2, 4, offsetof(skelGpuVert_t, weights[2]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_TANGENT2, 4, offsetof(skelGpuVert_t, weights[3]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_TANGENT, 4, offsetof(skelGpuVert_t, weights[4]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_LIGHTCOORD, 4, offsetof(skelGpuVert_t, weights[5]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_PAINTCOLOR, 4, offsetof(skelGpuVert_t, weights[6]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_COLOR, 4, offsetof(skelGpuVert_t, weights[7]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_BONE_INDEXES, 4, offsetof(skelGpuVert_t, bones[0]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_BONE_WEIGHTS, 4, offsetof(skelGpuVert_t, bones[4]));
    R_SkelGpuAttrib(vao, ATTR_INDEX_NORMAL, 3, offsetof(skelGpuVert_t, normal));
    R_SkelGpuAttrib(vao, ATTR_INDEX_TEXCOORD, 2, offsetof(skelGpuVert_t, st));
    Vao_SetVertexPointers(vao);
    GL_CheckErrors();

    m->ok  = qtrue;
    m->why = NULL;
}

void R_SkelGpuFree(void)
{
    int i;

    for (i = 0; i < SKEL_GPU_MESHES; i++) {
        skelGpuMesh_t *m = skelGpuMeshes[i];

        if (!m) {
            continue;
        }
        if (m->vao.vao) {
            if (glState.currentVao == &m->vao) {
                R_BindNullVao();
            }
            qglDeleteVertexArrays(1, &m->vao.vao);
            qglDeleteBuffers(1, &m->vao.vertexesVBO);
        }
        ri.Free(m);
        skelGpuMeshes[i] = NULL;
    }
    skelGpuNumMeshes = 0;
    skelBonesScene   = -1;
    skelNumRefused   = 0;
}

/*
================
Shaders and lighting
================
*/

// Every stage the lightall program, none with a map that needs a tangent
// frame or the lightmap's coordinates, and nothing worked out per vertex on
// the CPU.
static const char *R_SkelGpuShaderWhyNot(const shader_t *shader)
{
    int i, n = 0;

    if (shader->numDeforms) {
        return "deformVertexes";
    }
    if (shader->needsCPUVertexAlpha) {
        return "an alpha worked out per vertex";
    }
    if (shader->isSky || shader->isPortal) {
        return "a sky or portal";
    }
    for (i = 0; i < MAX_SHADER_STAGES; i++) {
        const shaderStage_t *stage = shader->stages[i];
        int                  index;

        if (!stage || !stage->active) {
            break;
        }
        if (stage->glslShaderGroup != tr.lightallShader) {
            return va("stage %d is not drawn by lightall", i);
        }
        index = stage->glslShaderIndex;
        if ((index & LIGHTDEF_LIGHTTYPE_MASK) == LIGHTDEF_USE_LIGHTMAP || stage->bundle[0].tcGen == TCGEN_LIGHTMAP) {
            return va("stage %d is lightmapped", i);
        }
        // a normal map needs the tangent frame, which is not posed here; a
        // specular map is read by the fragment program alone
        if (index & (LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_PARALLAXMAP)) {
            return va("stage %d has a normal map", i);
        }
        n++;
    }
    return n ? NULL : "no stages";
}

static qboolean R_SkelGpuShader(shader_t *shader)
{
    const char *why;

    if (shader->skelGpu) {
        return shader->skelGpu > 0;
    }

    why = R_SkelGpuShaderWhyNot(shader);
    shader->skelGpu = why ? -1 : 1;
    if (why) {
        // once a shader, so a log says which without asking (skelgpuinfo)
        ri.Printf(PRINT_ALL, "r_skelGpu: models with shader %s are posed on the CPU: %s\n", shader->name, why);
    }
    if (why && skelNumRefused < SKEL_GPU_REFUSED) {
        skelRefused[skelNumRefused].shader = shader;
        Q_strncpyz(skelRefused[skelNumRefused].why, why, sizeof(skelRefused[skelNumRefused].why));
        skelNumRefused++;
    }
    return !why;
}

static void R_SkelGpuColour(vec4_t out, const byte *rgba)
{
    VectorSet4(out, rgba[0], rgba[1], rgba[2], rgba[3]);
}

// How the model is lit, as RB_ComputeEntityLightColors would light it: false
// for what the program does not do.
static qboolean RB_SkelGpuLighting(void)
{
    const shader_t *shader = tess.shader;
    sphereor_t     *sphere = backEnd.currentSphere;
    vec4_t         *params = &tess.skelGpu.params;
    int             i;

    (*params)[2] = 0;
    (*params)[3] = 0;
    VectorSet4(tess.skelGpu.ambient, 255, 255, 255, 255);

    // depth alone: the colour is only ever tested for its alpha, which the
    // lights leave whole
    if (backEnd.depthFill) {
        return qtrue;
    }
    if (!shader->needsLSpherical && !shader->needsLGrid) {
        return qtrue;
    }
    if (!shader->needsLGrid && !r_drawspherelights->integer) {
        return qtrue;
    }
    if (!sphere || !sphere->TessFunction) {
        return qtrue;
    }

    if (sphere->TessFunction == RB_Light_Fullbright) {
        (*params)[2] = 1;
        return qtrue;
    }
    if (sphere->TessFunction == RB_CalcLightGridColor) {
        (*params)[2] = 1;
        R_SkelGpuColour(tess.skelGpu.ambient, (const byte *)&backEnd.currentEntity->iGridLighting);
        return qtrue;
    }
    if (sphere->TessFunction != RB_Light_Real || sphere->bUsesCubeMap || sphere->numRealLights > SKEL_GPU_MAX_LIGHTS) {
        return qfalse;
    }

    R_SkelGpuColour(tess.skelGpu.ambient, sphere->ambient.level);
    (*params)[2] = sphere->numRealLights ? 2 : 1;
    (*params)[3] = sphere->numRealLights;
    for (i = 0; i < sphere->numRealLights; i++) {
        const reallightinfo_t *l   = &sphere->light[i];
        vec4_t                *out = &tess.skelGpu.lights[i * 3];

        VectorCopy(l->color, out[0]);
        out[0][3] = l->eType;
        VectorCopy(l->vOrigin, out[1]);
        out[1][3] = l->fSpotConst;
        VectorCopy(l->vDirection, out[2]);
        out[2][3] = l->fSpotScale;
    }
    return qtrue;
}

/*
================
RB_SkelGpuUsable

The surface on the card, if this draw of it can be posed by the vertex
program; NULL to skin it on the CPU. The lighting it takes is left in
tess.skelGpu for RB_SkelGpuSubmit.
================
*/
void *RB_SkelGpuUsable(dtiki_t *tiki, skelSurfaceGame_t *sf, int mesh, skelHeaderGame_t *skelmodel)
{
    trRefEntity_t  *ent = backEnd.currentEntity;
    skelGpuMesh_t **slot;
    skelGpuMesh_t  *m;

    if (!R_SkelGpuAvailable()) {
        backEnd.pc.c_skelGpuCpu[SKEL_CPU_OFF]++;
        return NULL;
    }
    // what reads tess's vertexes after the stages: the fog, dynamic light and
    // personal shadow passes, the shadow map views drawn in colour; the debug
    // views
    if (!ent || ent == &tr.worldEntity || tess.currentStageIteratorFunc != RB_StageIteratorGeneric
        || tess.fogNum || tess.dlightBits || tess.pshadowBits || ((backEnd.viewParms.flags & VPF_SHADOWMAP) && !backEnd.depthFill)
        || r_showtris->integer || r_shownormals->integer) {
        backEnd.pc.c_skelGpuCpu[SKEL_CPU_VIEW]++;
        return NULL;
    }
    if (!R_SkelGpuShader(tess.shader)) {
        backEnd.pc.c_skelGpuCpu[SKEL_CPU_SHADER]++;
        return NULL;
    }

    slot = R_SkelGpuSlot(tiki, sf);
    if (!slot) {
        backEnd.pc.c_skelGpuCpu[SKEL_CPU_MESH]++;
        return NULL;
    }
    if (!*slot) {
        m = (skelGpuMesh_t *)ri.Malloc(sizeof(*m));
        Com_Memset(m, 0, sizeof(*m));
        m->tiki = tiki;
        m->sf   = sf;
        *slot   = m;
        skelGpuNumMeshes++;
        R_SkelGpuBuild(m, tiki, sf, mesh, skelmodel);
    }
    m = *slot;
    if (!m->ok) {
        backEnd.pc.c_skelGpuCpu[SKEL_CPU_MESH]++;
        return NULL;
    }
    if (ent->e.hasMorph && m->hasMorphs) {
        backEnd.pc.c_skelGpuCpu[SKEL_CPU_MORPHS]++;
        return NULL;
    }

    if (!RB_SkelGpuLighting()) {
        backEnd.pc.c_skelGpuCpu[SKEL_CPU_LIGHTING]++;
        return NULL;
    }
    tess.skelGpu.params[0] = ent->e.bonestart;
    tess.skelGpu.params[1] = tiki->load_scale * ent->e.scale;
    tess.skelGpu.numBones  = ri.TIKI_GetNumChannels(tiki);
    return m;
}

/*
================
RB_SkelGpuSubmit

The surface RB_SkelMesh has put the indexes of in tess, alone, drawn as a
batch of its own.
================
*/
void RB_SkelGpuSubmit(void *mesh)
{
    tess.skelGpu.active = qtrue;
    tess.skelGpu.mesh   = mesh;
    RB_EndSurface();
    RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);
}

/*
================
RB_SkelGpuBind

For RB_StageIteratorGeneric: the surface's vertex array object bound, the
batch's indexes into the tess ring, the bones on the card.
================
*/
void RB_SkelGpuBind(void)
{
    skelGpuMesh_t *m = (skelGpuMesh_t *)tess.skelGpu.mesh;
    GLuint         buffer;
    int            first;

    RB_SkelGpuSendBones((int)tess.skelGpu.params[0] + tess.skelGpu.numBones);

    R_BindVao(&m->vao);
    first = RB_StreamIndexes(tess.indexes, tess.numIndexes, &buffer);
    qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    m->vao.indexesIBO = buffer;

    GL_BindToTMU(tr.skelBoneImage, TMU_SKELBONES);

    tess.streamVao        = &m->vao;
    tess.streamBaseVertex = 0;
    tess.streamFirstIndex = first;
    backEnd.pc.c_skelGpuDraws++;
}

/*
================
R_SkelGpuReport

With the GPU timers' report (r_gpuTimers): the skeletal surfaces of the
last frame left to the CPU, by why.
================
*/
void R_SkelGpuReport(void)
{
    char line[256];
    int  i, total = 0;

    Com_sprintf(line, sizeof(line), "gpu skeletal surfaces posed on the CPU:");
    for (i = 0; i < SKEL_CPU_COUNT; i++) {
        total += backEnd.pc.c_skelGpuCpu[i];
        Q_strcat(line, sizeof(line), va(" %s %d", skelCpuNames[i], backEnd.pc.c_skelGpuCpu[i]));
    }
    ri.Printf(PRINT_ALL, "%s (%d; skelgpuinfo says which)\n", line, total);
}

/*
================
R_SkelGpuInfo_f

The surfaces and shaders the vertex program could not pose, and why.
================
*/
void R_SkelGpuInfo_f(void)
{
    int i, ok = 0, refused = 0;

    ri.Printf(PRINT_ALL, "Skeletal models posed by the vertex program: %s\n",
        R_SkelGpuAvailable() ? "on" : (r_skelGpu && r_skelGpu->integer ? "not available (needs GLSL 1.30, buffer storage and r_tessStream)" : "off (r_skelGpu 0)"));

    for (i = 0; i < SKEL_GPU_MESHES; i++) {
        const skelGpuMesh_t *m = skelGpuMeshes[i];

        if (!m) {
            continue;
        }
        if (m->ok) {
            ok++;
            continue;
        }
        refused++;
        ri.Printf(PRINT_ALL, "  surface %s of %s: %s (%d weights at most)\n",
            m->sf->name, m->tiki->name ? m->tiki->name : "?", m->why ? m->why : "?", m->maxWeights);
    }
    ri.Printf(PRINT_ALL, "  %d surfaces on the card, %d left to the CPU\n", ok, refused);

    for (i = 0; i < skelNumRefused; i++) {
        ri.Printf(PRINT_ALL, "  shader %s: %s\n", skelRefused[i].shader->name, skelRefused[i].why);
    }
    ri.Printf(PRINT_ALL, "  %d shaders left to the CPU%s\n", skelNumRefused, skelNumRefused == SKEL_GPU_REFUSED ? " (the first ones)" : "");
}
