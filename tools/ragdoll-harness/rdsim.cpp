// Harness that runs the real cg_ragdoll.cpp solver against a synthetic biped
// standing on a flat floor, with only the engine touchpoints stubbed out.
#include "cg_local.h"
#include "rd_anims.h"
#include "cg_ragdoll.h"
#include "cg_physics_ragdoll.h"
#include <cstdio>
#include <cmath>

static const char *gWorstA = "-", *gWorstB = "-";
static const char *gPenA = "-", *gPenB = "-";
static const char *gBendAt = "-";
static int gWorstT = 0, gLateT = 0;
static const char *gLateA = "-", *gLateB = "-";
static float gFrameStretch = 0.0f;
static float gMoveThisFrame = 0.0f, gLateMove = 0.0f;

extern "C" int CG_RagdollDebugParticles(int, float*, int);
extern "C" void CG_RagdollGrabDown_f(void);
extern "C" qboolean CG_RagdollNoteBullet(const vec3_t start, const vec3_t end, int large, vec3_t stopAt);
#ifdef RD_JOLT
// The physics world the Jolt ragdoll runs in (rdjolt.cpp).
void RDJ_Init(float gravity);
void RDJ_Build(const vec3_t floorN, int box, const vec3_t mins, const vec3_t maxs);
void RDJ_Step(float frametime);
extern "C" void CG_RagdollDebugJolt(int entityNum, void (*print)(const char *fmt, ...));
static void RDJ_Print(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
#endif
clientGameImport_t cgi;
cg_t               cg;
cgs_t              cgs;
centity_t          cg_entities[MAX_GENTITIES];
cvar_t            *cg_3rd_person;

static snapshot_t  g_snap;
static dtikianim_t g_anim;
static dtiki_t     g_tiki;

// ---- synthetic skeleton -------------------------------------------------
// jointIdx is the RD_* index cg_ragdoll.cpp assigns this bone's joint, and
// radius the thickness it gives it. Both are carried here rather than in a
// parallel array so a change to the bone list cannot silently misalign them.
struct BoneDef { const char *name; float pos[3]; int parent; int jointIdx; float radius; };
static float gPileJump = 0.0f;
static int gHangFlips = 0;
static float gSeedTwist  = 0.0f;   // degrees, applied above the waist at seed
// Degrees the arms are swung up about the shoulder at seed, and the knees bent
// at seed. MOHAA death animations throw the limbs about, so a corpse very
// often starts its ragdoll with an arm over its head or a leg drawn up, and
// whether those come back down is exactly what is in question here. Seeding
// from a rest pose every time never asks that.
static float gSeedArmsUp = 0.0f;
static float gSeedKnees  = 0.0f;
// A death animation that is still moving when the ragdoll takes over. The arms
// swing in toward the chest over gClutchMs, so that at the moment the blend
// ends they carry real inward velocity, which CG_RagdollDriveFromAnim hands to
// the ragdoll on purpose. A static pose cannot show that at all, and it is the
// most likely reason a corpse brings its arms up to its chest.
static int   gBlendFrames = 0;   // rendered frames the death animation window covers
// The real death animation the corpse is playing, or -1 for the old synthetic
// standing pose. A death animation throws a body about far harder than any
// hand written seed does, and several things that were plainly wrong in game
// never showed up here until the animations themselves were driving it.
static int   gAnim = -1;
static float gClutchDeg = 0.0f;
static int   gClutchMs  = 120;

// Proportions taken from the real MOHAA player skeleton
// (models/human/allied_army_soldier/usarmy.skd), scaled by 0.42 so the figure
// keeps the ~72 unit stature the scenarios were written against. Segment
// lengths in model units, before scaling:
//
//   pelvis>spine 7.44   spine>spine1 13.26  spine1>spine2 11.80
//   spine2>neck 21.74   neck>head 6.22      spine2>clavicle 15.36
//   clavicle>upperarm 14.68   upperarm>forearm 25.60   forearm>hand 25.97
//   pelvis>thigh 9.18 (lateral)  thigh>calf 46.35  calf>foot 46.04
//   foot>toe 16.90
//
// The spine is strikingly non-uniform: spine2>neck alone is 36% of the whole
// torso, so the ribcage is one long segment with no joint in the middle of it.
// The earlier synthetic body had all five spine segments at a uniform 6 units,
// which sealed that gap by accident and hid every artifact that depends on it.
//
// The model carries no "Bip01 HeadNub" and no "Bip01 L/R Finger1", so the head
// and hand tips exercise the extrapolated-tip path, as they do in game. It does
// carry real toe bones and real clavicles.
static BoneDef g_bones[] = {
    //  name                 x      y       z      parent  joint  radius
    {"Bip01",            {  0,    0,    42.37},  -1,      -1,    0.0f},
    {"Bip01 Pelvis",     {  0,    0,    42.37},   0,       0,    5.5f},
    {"Bip01 Spine",      {  0,    0,    45.49},   1,       1,    5.0f},
    {"Bip01 Spine1",     {  0,    0,    51.06},   2,       2,    5.0f},
    {"Bip01 Spine2",     {  0,    0,    56.02},   3,       3,    4.8f},
    {"Bip01 Neck",       {  0,    0,    65.15},   4,       4,    3.0f},
    {"Bip01 Head",       {  0,    0,    67.76},   5,       5,    4.5f},
    {"Bip01 L Clavicle", {  0,    1.21, 61.80},   4,      -1,    0.0f},
    {"Bip01 L UpperArm", {  0,    7.35, 61.80},   7,       7,    4.0f},
    {"Bip01 L Forearm",  {  1.0,  7.35, 51.15},   8,       8,    3.2f},
    {"Bip01 L Hand",     {  3.0,  7.0,  40.50},   9,       9,    2.8f},
    {"Bip01 R Clavicle", {  0,   -1.21, 61.80},   4,      -1,    0.0f},
    {"Bip01 R UpperArm", {  0,   -7.35, 61.80},  11,      11,    4.0f},
    {"Bip01 R Forearm",  {  1.0, -7.35, 51.15},  12,      12,    3.2f},
    {"Bip01 R Hand",     {  3.0, -7.0,  40.50},  13,      13,    2.8f},
    {"Bip01 L Thigh",    {  0,    3.86, 42.37},   1,      15,    5.5f},
    {"Bip01 L Calf",     {  0,    3.86, 22.90},  15,      16,    4.0f},
    {"Bip01 L Foot",     {  0,    3.86,  3.57},  16,      17,    3.5f},
    {"Bip01 L Toe0",     {  6.8,  3.86,  1.50},  17,      18,    3.0f},
    {"Bip01 R Thigh",    {  0,   -3.86, 42.37},   1,      19,    5.5f},
    {"Bip01 R Calf",     {  0,   -3.86, 22.90},  19,      20,    4.0f},
    {"Bip01 R Foot",     {  0,   -3.86,  3.57},  20,      21,    3.5f},
    {"Bip01 R Toe0",     {  6.8, -3.86,  1.50},  21,      22,    3.0f},
};
static const int NUM_BONES = (int)(sizeof(g_bones)/sizeof(g_bones[0]));

static int Stub_TagNumForName(dtiki_t *, const char *name)
{
    for (int i = 0; i < NUM_BONES; i++)
        if (!Q_stricmp(g_bones[i].name, name)) return i;
    return -1;
}

// A bone's rest direction: toward its first child, or, for a leaf, continuing
// the direction its parent came in on. Same rule Stub_TIKI_Orientation uses.
static void Stub_RestAxis(int tag, vec3_t out)
{
    VectorSet(out, 1, 0, 0);
    if (tag < 0 || tag >= NUM_BONES) return;

    int child = -1;
    for (int i = 0; i < NUM_BONES; i++)
        if (g_bones[i].parent == tag) { child = i; break; }

    if (child >= 0)
        VectorSubtract(g_bones[child].pos, g_bones[tag].pos, out);
    else if (g_bones[tag].parent >= 0)
        VectorSubtract(g_bones[tag].pos, g_bones[g_bones[tag].parent].pos, out);

    if (VectorNormalize(out) < 0.001f) VectorSet(out, 1, 0, 0);
}

// The pose of a real death animation at the current time, if one is playing.
// Held on the last frame once it has run, which is what the game does with a
// death animation too.
static qboolean Stub_AnimPose(int tag, float scale, orientation_t *o)
{
    if (gAnim < 0 || gAnim >= RD_NUM_ANIMS) return qfalse;
    if (tag < 0 || tag >= RD_ANIM_BONES) return qfalse;

    int nf = rd_anims[gAnim].numFrames;
    int f  = (int)(cg.time * 0.001f / rd_anims[gAnim].frameTime);
    if (f < 0) f = 0;
    if (f >= nf) f = nf - 1;

    const float *v = rd_anims[gAnim].data + ((size_t)f * RD_ANIM_BONES + tag) * 12;
    for (int k = 0; k < 3; k++) o->origin[k] = v[k] * scale;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) o->axis[r][c] = v[3 + r*3 + c];
    return qtrue;
}

// X runs down the bone toward the first child, which is the Biped convention.
static orientation_t Stub_TIKI_Orientation(refEntity_t *m, int tag)
{
    orientation_t o;
    memset(&o, 0, sizeof(o));
    if (tag < 0 || tag >= NUM_BONES) return o;

    // Mirrors TIKI_OrientationInternal: origin is pre-scaled by the entity and
    // model scale, and carries the model's load origin.
    float sc = m->scale * g_tiki.load_scale;

    if (Stub_AnimPose(tag, sc, &o)) return o;

    vec3_t bp; VectorCopy(g_bones[tag].pos, bp);
    // Rotate the upper body about the spine, so the corpse starts wrung out
    // the way a directional death animation leaves it.
    if (gSeedTwist != 0.0f && bp[2] > 44.0f) {
        float a = gSeedTwist * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
        float x = bp[0], y = bp[1];
        bp[0] = c*x - sn*y; bp[1] = sn*x + c*y;
    }
    // The animation still running: swing the arms in toward the chest.
    if (gClutchDeg != 0.0f) {
        const char *n = g_bones[tag].name;
        int isArm = n && (strstr(n,"Forearm") || strstr(n,"L Hand") || strstr(n,"R Hand"));
        if (isArm) {
            int sh = Stub_TagNumForName(NULL, strstr(n,"L ") ? "Bip01 L UpperArm" : "Bip01 R UpperArm");
            float f = cg.time >= gClutchMs ? 1.0f : (float)cg.time / (float)gClutchMs;
            if (sh >= 0) {
                // inward, about the shoulder, in the plane across the body
                float sgn = strstr(n,"L ") ? -1.0f : 1.0f;
                float a = sgn * gClutchDeg * f * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
                float dy = bp[1]-g_bones[sh].pos[1], dz = bp[2]-g_bones[sh].pos[2];
                bp[1] = g_bones[sh].pos[1] + c*dy - sn*dz;
                bp[2] = g_bones[sh].pos[2] + sn*dy + c*dz;
            }
        }
    }
    // Swing the arm up about its own shoulder.
    if (gSeedArmsUp != 0.0f) {
        const char *n = g_bones[tag].name;
        int isArm = n && (strstr(n,"Forearm") || strstr(n,"L Hand") || strstr(n,"R Hand"));
        if (isArm) {
            int sh = Stub_TagNumForName(NULL, strstr(n,"L ") ? "Bip01 L UpperArm" : "Bip01 R UpperArm");
            if (sh >= 0) {
                float a = gSeedArmsUp * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
                float dx = bp[0]-g_bones[sh].pos[0], dz = bp[2]-g_bones[sh].pos[2];
                bp[0] = g_bones[sh].pos[0] + c*dx - sn*dz;
                bp[2] = g_bones[sh].pos[2] + sn*dx + c*dz;
            }
        }
    }
    // Draw the knee up: bend the calf and everything below it about the knee.
    if (gSeedKnees != 0.0f) {
        const char *n = g_bones[tag].name;
        int isLower = n && (strstr(n,"Foot") || strstr(n,"Toe"));
        if (isLower) {
            int kn = Stub_TagNumForName(NULL, strstr(n,"L ") ? "Bip01 L Calf" : "Bip01 R Calf");
            if (kn >= 0) {
                float a = gSeedKnees * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
                float dx = bp[0]-g_bones[kn].pos[0], dz = bp[2]-g_bones[kn].pos[2];
                bp[0] = g_bones[kn].pos[0] + c*dx - sn*dz;
                bp[2] = g_bones[kn].pos[2] + sn*dx + c*dz;
            }
        }
    }
    for (int k = 0; k < 3; k++) o.origin[k] = (bp[k] + g_tiki.load_origin[k]) * sc;

    int child = -1;
    for (int i = 0; i < NUM_BONES; i++)
        if (g_bones[i].parent == tag) { child = i; break; }

    vec3_t x;
    if (child >= 0) VectorSubtract(g_bones[child].pos, g_bones[tag].pos, x);
    else if (g_bones[tag].parent >= 0) VectorSubtract(g_bones[tag].pos, g_bones[g_bones[tag].parent].pos, x);
    else VectorSet(x, 0, 0, 1);
    if (VectorNormalize(x) < 0.001f) VectorSet(x, 0, 0, 1);

    vec3_t up = {0, 1, 0}, y, z;
    float d = DotProduct(up, x);
    VectorMA(up, -d, x, y);
    if (VectorNormalize(y) < 0.001f) { VectorSet(up, 1, 0, 0); d = DotProduct(up, x); VectorMA(up, -d, x, y); VectorNormalize(y); }
    CrossProduct(x, y, z);
    VectorCopy(x, o.axis[0]); VectorCopy(y, o.axis[1]); VectorCopy(z, o.axis[2]);
    return o;
}

static void    Stub_ForceUpdatePose(refEntity_t *) {}
// What the grabber (cg_ragdoll_grab) and the clipping measurement reach for.
// None of it has anything to do here but must not be a null call.
static long    Stub_FSReadFile(const char *, void **buf, qboolean) { if (buf) *buf = NULL; return -1; }
static void    Stub_FSFreeFile(void *) {}
static void    Stub_CmdExecute(int, const char *) {}
static qhandle_t Stub_RegisterShader(const char *) { return 1; }
static qboolean Stub_AddPoly(qhandle_t, int, const polyVert_t *, int) { return qtrue; }
static const char *Stub_TagNameForNum(dtiki_t *, int) { return "-"; }
static void    Stub_DPrintf(const char *, ...) {}
static void    Stub_DebugLine(const vec3_t, const vec3_t, float, float, float, float) {}
static void    Stub_CvarCheckRange(cvar_t *, float, float, qboolean) {}
static cvar_t *Stub_CvarGet(const char *name, const char *value, int)
{
    static cvar_t pool[64]; static char names[64][64], strs[64][64]; static int n = 0;
    for (int i = 0; i < n; i++) if (!strcmp(pool[i].name, name)) return &pool[i];
    Q_strncpyz(names[n], name, 64); Q_strncpyz(strs[n], value, 64);
    pool[n].name = names[n]; pool[n].string = strs[n];
    pool[n].value = (float)atof(value); pool[n].integer = atoi(value);
    return &pool[n++];
}

// Enough of the file API for the solver's own trace writer, so the format the
// game will produce can be exercised here rather than only in the game.
static FILE *gFiles[8];

static fileHandle_t Stub_FOpenFileWrite(const char *name)
{
    for (int i = 1; i < 8; i++) {
        if (!gFiles[i]) { gFiles[i] = fopen(name, "w"); return gFiles[i] ? i : 0; }
    }
    return 0;
}

static size_t Stub_FSWrite(const void *buf, size_t sz, fileHandle_t h)
{
    if (h <= 0 || h >= 8 || !gFiles[h]) return 0;
    return fwrite(buf, 1, sz, gFiles[h]);
}

static void Stub_FCloseFile(fileHandle_t h)
{
    if (h <= 0 || h >= 8 || !gFiles[h]) return;
    fclose(gFiles[h]); gFiles[h] = NULL;
}

static void Stub_Printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

static void Stub_CvarSet(const char *name, const char *value)
{
    cvar_t *c = Stub_CvarGet(name, value, 0);
    Q_strncpyz((char *)c->string, value, 64);
    c->value = (float)atof(value); c->integer = atoi(value);
}

// Floor plane: DotProduct(p, gFloorN) >= 0
vec3_t gFloorN = {0, 0, 1};

// An optional solid box, so a corpse can be dropped onto an edge rather than
// only ever onto open ground. Draping over a ledge is where the real trouble
// shows up, because joints end up buried inside the geometry.
int    gLedge = 0;
vec3_t gLedgeMins = {0, -200, -100};
vec3_t gLedgeMaxs = {200, 200, 18};

static int LedgeContains(const vec3_t p)
{
    if (!gLedge) return 0;
    for (int k = 0; k < 3; k++)
        if (p[k] < gLedgeMins[k] || p[k] > gLedgeMaxs[k]) return 0;
    return 1;
}
int CG_PointContents(const vec3_t p, int) { return (DotProduct(p, gFloorN) <= 0.0f || LedgeContains(p)) ? CONTENTS_SOLID : 0; }

// Sweep against the ledge box, expanded by the trace radius, taking whichever
// face is hit first.
static void LedgeTrace(trace_t *r, const vec3_t start, const vec3_t end, float off)
{
    if (!gLedge) return;

    float best = r->fraction;
    vec3_t bestN = {0,0,0};
    int    hit = 0;

    for (int axis = 0; axis < 3; axis++) {
        for (int side = 0; side < 2; side++) {
            float plane = side ? (gLedgeMaxs[axis] + off) : (gLedgeMins[axis] - off);
            float s = side ? (start[axis] - plane) : (plane - start[axis]);
            float e = side ? (end[axis]   - plane) : (plane - end[axis]);
            if (s <= 0.0f || e > 0.0f) continue;
            float f = s / (s - e);
            if (f >= best) continue;
            vec3_t hp; for (int k=0;k<3;k++) hp[k] = start[k] + (end[k]-start[k])*f;
            int inside = 1;
            for (int k=0;k<3;k++) {
                if (k == axis) continue;
                if (hp[k] < gLedgeMins[k]-off || hp[k] > gLedgeMaxs[k]+off) { inside = 0; break; }
            }
            if (!inside) continue;
            best = f; hit = 1;
            VectorClear(bestN); bestN[axis] = side ? 1.0f : -1.0f;
        }
    }

    if (hit) {
        r->fraction = best;
        for (int k=0;k<3;k++) r->endpos[k] = start[k] + (end[k]-start[k])*best;
        VectorCopy(bestN, r->plane.normal);
    }
}

// Flat floor at world z = 0.
void CG_Trace(trace_t *r, const vec3_t start, const vec3_t mins, const vec3_t maxs,
              const vec3_t end, int, int, qboolean, qboolean, const char *)
{
    memset(r, 0, sizeof(*r));
    r->fraction = 1.0f;
    VectorCopy(end, r->endpos);

    float off = -mins[2];   // treat the box as a sphere of this radius
    float s = DotProduct(start, gFloorN) - off, e = DotProduct(end, gFloorN) - off;
    int inBox = 0;
    if (gLedge) {
        inBox = 1;
        for (int k = 0; k < 3; k++)
            if (start[k] < gLedgeMins[k]-off || start[k] > gLedgeMaxs[k]+off) { inBox = 0; break; }
    }
    if (s <= 0.0f || inBox) { r->startsolid = qtrue; r->allsolid = qtrue; r->fraction = 0.0f; VectorCopy(start, r->endpos); return; }
    // Clear of the floor: the ledge can still be in the way. This used to
    // return here, so nothing short of the floor ever struck the ledge and it
    // was only met through the buried push out on the next step.
    if (e > 0.0f) { LedgeTrace(r, start, end, off); return; }

    r->fraction = s / (s - e);
    for (int i = 0; i < 3; i++) r->endpos[i] = start[i] + (end[i] - start[i]) * r->fraction;
    VectorCopy(gFloorN, r->plane.normal);
    LedgeTrace(r, start, end, off);
}

// Closest distance between two segments. Written out here rather than borrowed
// from the solver so the measurement stays independent of the code under test.
static float SegSegDist(const float *p1, const float *q1, const float *p2, const float *q2,
                        float *ta, float *tb, vec3_t dir)
{
    vec3_t d1,d2,r,c1,c2;
    VectorSubtract(q1,p1,d1); VectorSubtract(q2,p2,d2); VectorSubtract(p1,p2,r);
    float a=DotProduct(d1,d1), e=DotProduct(d2,d2), f=DotProduct(d2,r), s=0, t=0;
    if (a<1e-4f && e<1e-4f) { s=t=0; }
    else if (a<1e-4f) { s=0; t=f/e; if(t<0)t=0; if(t>1)t=1; }
    else {
        float c=DotProduct(d1,r);
        if (e<1e-4f) { t=0; s=-c/a; if(s<0)s=0; if(s>1)s=1; }
        else {
            float b=DotProduct(d1,d2), denom=a*e-b*b;
            s = denom>1e-4f ? (b*f-c*e)/denom : 0.0f; if(s<0)s=0; if(s>1)s=1;
            t = (b*s+f)/e;
            if (t<0) { t=0; s=-c/a; if(s<0)s=0; if(s>1)s=1; }
            else if (t>1) { t=1; s=(b-c)/a; if(s<0)s=0; if(s>1)s=1; }
        }
    }
    VectorMA(p1,s,d1,c1); VectorMA(p2,t,d2,c2);
    *ta=s; *tb=t; VectorSubtract(c1,c2,dir);
    return VectorLength(dir);
}

// Roll of one bone about its own length relative to another, in degrees.
//
// The reference bone's Y is carried onto the target's direction by the shortest
// rotation between the two bone axes before the two are compared. Comparing the
// Y axes directly instead mixes in how far the joint between them is bent, so a
// bent ankle reads as a rolled foot even when the foot is not rolled at all.
//
// The two bones need not be adjacent: this is how the accumulated roll from the
// pelvis to the chest is measured, which no per-joint number can show. A roll
// spread evenly down a chain is small at every joint and large end to end.
static float RollBetween(const float *ax, const float *ay, const float *bx, const float *by)
{
    vec3_t rot, ya, yb;
    CrossProduct(ax, bx, rot);
    float sn = VectorNormalize(rot);
    float cs = DotProduct(ax, bx);

    if (sn < 0.001f) VectorCopy(ay, ya);
    else RotatePointAroundVector(ya, rot, ay, (float)(atan2(sn, cs) * 180.0 / M_PI));

    VectorMA(ya, -DotProduct(ya, bx), bx, ya);
    VectorCopy(by, yb);
    VectorMA(yb, -DotProduct(yb, bx), bx, yb);

    if (VectorNormalize(ya) < 0.01f || VectorNormalize(yb) < 0.01f) return 0.0f;

    vec3_t side; CrossProduct(bx, ya, side);
    return (float)(atan2(DotProduct(yb, side), DotProduct(yb, ya)) * 180.0 / M_PI);
}

// The same angle as the renderer sees it between two drawn bone directions.
static float AngleBetween(const float *a, const float *b)
{
    float d = DotProduct(a, b);
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    return (float)(acos(d) * 180.0 / M_PI);
}

// ---- the test -----------------------------------------------------------
static float SegLen(float pos[64][3], int have[64], const char *a, const char *b)
{
    int ia = Stub_TagNumForName(NULL, a), ib = Stub_TagNumForName(NULL, b);
    if (ia < 0 || ib < 0 || !have[ia] || !have[ib]) return -1.0f;
    vec3_t d; VectorSubtract(pos[ia], pos[ib], d);
    return VectorLength(d);
}

static float SeedLen(const char *a, const char *b)
{
    int ia = Stub_TagNumForName(NULL, a), ib = Stub_TagNumForName(NULL, b);
    vec3_t d; VectorSubtract(g_bones[ia].pos, g_bones[ib].pos, d);
    return VectorLength(d);
}


struct Scenario { const char *name; float startZ; float yaw; float scale; vec3_t floorN; float pitch; float roll; float seedTwist; int ledge; float armsUp; float knees; float clutch; int anim; };

static int RunScenario(const Scenario &sc)
{
    srand(12345);
    memset(cg_entities, 0, sizeof(cg_entities));
    CG_InitRagdoll();
    // Sleep disabled on purpose: a corpse that has been frozen cannot fidget,
    // so leaving it on would hide exactly what is being measured here.
    VectorCopy(sc.floorN, gFloorN);
    gSeedTwist  = sc.seedTwist;
    gSeedArmsUp = sc.armsUp;
    gSeedKnees  = sc.knees;
    gClutchDeg  = sc.clutch;
    gAnim       = sc.anim ? sc.anim - 1 : -1;
    gBlendFrames = cg_ragdoll_blendtime->integer / 16 + 2;
    // RD_DUMP writes the same trace file the game writes, for checking the
    // format and the reader against a known scenario.
    gLedge = sc.ledge ? 1 : 0;

    // ledge 2 is a wall standing beside the body rather than a platform under
    // it. Bodies die against walls constantly in the real game and every
    // scenario here had nothing but open floor, so a whole class of contact
    // went untested.
    if (sc.ledge == 2) {
        VectorSet(gLedgeMins, -400.0f,  10.0f, -100.0f);
        VectorSet(gLedgeMaxs,  400.0f, 400.0f,  200.0f);
    } else {
        VectorSet(gLedgeMins, 0.0f, -200.0f, -100.0f);
        VectorSet(gLedgeMaxs, 200.0f, 200.0f, 18.0f);
    }
    if (const char *bx = getenv("RD_BOX")) {
        sscanf(bx, "%f %f %f %f %f %f", &gLedgeMins[0], &gLedgeMins[1], &gLedgeMins[2], &gLedgeMaxs[0], &gLedgeMaxs[1], &gLedgeMaxs[2]);
    }
    VectorNormalize(gFloorN);
#ifdef RD_JOLT
    RDJ_Build(gFloorN, gLedge, gLedgeMins, gLedgeMaxs);
#endif

    const int  entnum = 5;
    centity_t *cent   = &cg_entities[entnum];
    cent->currentValid            = qtrue;
    cent->currentState.number     = entnum;
    cent->currentState.eFlags     = EF_DEAD;
    cent->currentState.modelindex = 3;
    cent->currentState.eType      = ET_MODELANIM;
    VectorSet(cent->lerpOrigin, 0, 0, getenv("RD_STARTZ") ? (float)atof(getenv("RD_STARTZ")) : sc.startZ); if (const char *sx = getenv("RD_STARTXY")) sscanf(sx, "%f %f", &cent->lerpOrigin[0], &cent->lerpOrigin[1]);
    VectorSet(cent->lerpAngles, sc.pitch, sc.yaw, sc.roll);

    float worstStretch = 0, lateStretch = 0, lateMove = 0, deepest = 1e9f, worstPen = -1e9f, worstHyper = -1e9f;
    float jointDev[3] = {0,0,0};
    float worstTwist = 0, worstJointTwist = 0, worstJointBend = 0;
    float twistRest0 = -1.0f;
    float bendPer[6] = {0,0,0,0,0,0};
    float kneeLateral = 0, armInTorso = -9, particleCollapse = 0;
    // Angle between the shoulder line the solver has and the shoulder line
    // that is drawn. Every other twist measure here compares the drawn pose
    // against itself and is blind to the two parting company.
    float chestErr = 0, chestErrLate = 0;
    // Jitter: the average distance a joint shifts from one frame to the next
    // once the body should have stopped. Residual motion records the worst
    // single moment, which a shove dominates; this records the shivering.
    float jitSum = 0; int jitN = 0; float jitPrev[23*3]; int jitHave = 0;
    // How far the elbow ends up folded. A living arm flexes to 150 degrees,
    // which is what the constraint permits, but that is a voluntary maximum
    // and a dead arm has nothing to hold it there.
    float elbowFold = 0;
    float limbAir = -1e9f; const char *airWho = "-";
    float sprawl  = 0.0f;
    float kneeFold = 0.0f;
    float romOver = 0.0f; const char *romWho = "-";
    float boneRoll = 0.0f; const char *rollWho = "-";
    float ankMin = 999; int ankN = 0, ankStraight = 0;
    float landAt[3] = {0,0,0}; int landed = 0; float landSlide = 0;
    static float whLast[23*3]; int whHave = 0, whSteps = 0, whips = 0; float whWorst = 0;
    // Worst stretch of any bone pair against its length at frame 20: a joint
    // hooked on something while the rest of the body is carried off.
    static const int stPair[][2] = {{0,6},{7,8},{8,9},{11,12},{12,13},{15,16},{16,17},{19,20},{20,21},{0,15},{0,19},{3,7},{3,11}};
    float stRest[13] = {0}; int stHave = 0; float stWorst = 1.0f; int stFrames = 0;
    // Spring off an impact: the first frame the centroid's fall is stopped by
    // more than 150 u/s, then over the next half second the most level speed
    // the centroid gains over what it came in with, and the most upward speed.
    float imVel[3] = {0,0,0}, imPrev[3] = {0,0,0}; int imHave = 0, imFrame = -1; float imIn = 0, imGain = 0, imUp = 0, imDv = 0, imPeakH = 0;
    float wallAwayMax = 0, wallLastY = 0; int wallHave = 0;
    float shRest = -1, shMin = 9, shSum = 0; int shN = 0;
    float clavGapSum = 0, clavGapMax = 0, clavOff[2][3] = {{0,0,0},{0,0,0}}; int clavHave[2] = {0,0}; int clavGapN = 0, clavSeen = 0;
    float limbWanderSum[2] = {0,0}, limbWanderMax = 0, limbOffSum[2] = {0,0}, limbOffW[2] = {0,0}, limbOffMax = 0; int limbWanderN[2] = {0,0}, limbWanderBig[2] = {0,0};
    float limbTwRest[4] = {999,999,999,999}, limbOffRest[4] = {999,999,999,999};
    float seedErr = 0.0f; const char *seedWho = "-";
    float rootSplit = 0.0f;
    float rollStep = 0.0f; const char *stepWho = "-"; int stepAt = -1;
    float rollStepLate = 0.0f; const char *stepLateWho = "-";
    static float prevX[64][3], prevY[64][3];
    float pelvisTwist = 0.0f, pelvisTwistRest = -999.0f;
    float rollRest[8]; for (int q=0;q<8;q++) rollRest[q]=-1.0f;
    float pRest[24] = {0};
    float footFlip = 0;
    float refLen[16] = {0};
    float elbowRise = -1e9f;
    float limbCross = -9.0f; const char *crossWho = "-";
    float travelled = 0.0f; vec3_t restedAt = {0,0,0}; int haveHandoff = 0; vec3_t handoffAt = {0,0,0};
    static float restSep[64][64];
    memset(restSep, 0, sizeof(restSep));
    int   bad = 0, settleFrame = -1, backBends = 0;
    static float prev[64][3]; int havePrev = 0;
    float lastExtent[3] = {0,0,0}, lastSeg[4] = {0,0,0,0};

    // Seed reproduction, probed on its own with the blend switched off.
    //
    // Taken inside the ordinary run this check could never fail. On the first
    // frame the blend weight is zero, so CG_RagdollBuildPose slerps the whole
    // way back to the animation and hands back exactly what it was given,
    // whatever the reconstruction made of it. What the number is meant to ask,
    // whether the rebuilt skeleton reproduces the pose it was seeded from, is
    // only visible with the blend out of the way. So it gets a frame of its own
    // and the ragdoll built for it is thrown away afterwards.
    {
        const int   saveInt = cg_ragdoll_blendtime->integer;
        const float saveVal = cg_ragdoll_blendtime->value;

        cg_ragdoll_blendtime->integer = 0;
        cg_ragdoll_blendtime->value   = 0.0f;
        cg.time = 0; cg.frametime = 16;

        refEntity_t model; memset(&model, 0, sizeof(model));
        model.entityNumber = entnum; model.tiki = &g_tiki; model.scale = sc.scale;
        VectorCopy(cent->lerpOrigin, model.origin);
        AnglesToAxis(cent->lerpAngles, model.axis);
        CG_RagdollUpdateEntity(cent, &model);

        for (int i = 0; i < model.num_bone_overrides; i++) {
            const int b = model.bone_override[i].boneIndex;
            vec3_t gx, gy, ax, ay;

            VectorClear(gx); VectorClear(gy);
            for (int k = 0; k < 3; k++) {
                VectorMA(gx, model.bone_override[i].matrix[0][k], model.axis[k], gx);
                VectorMA(gy, model.bone_override[i].matrix[1][k], model.axis[k], gy);
            }
            VectorNormalize(gx); VectorNormalize(gy);

            refEntity_t am; memset(&am, 0, sizeof(am));
            am.tiki = &g_tiki; am.scale = sc.scale;
            orientation_t ao = Stub_TIKI_Orientation(&am, b);

            VectorClear(ax); VectorClear(ay);
            for (int k = 0; k < 3; k++) {
                VectorMA(ax, ao.axis[0][k], model.axis[k], ax);
                VectorMA(ay, ao.axis[1][k], model.axis[k], ay);
            }
            VectorNormalize(ax); VectorNormalize(ay);

            const float ex = AngleBetween(ax, gx);
            const float ey = AngleBetween(ay, gy);
            const float e  = ex > ey ? ex : ey;

            if (e > seedErr) { seedErr = e; seedWho = g_bones[b].name; }
        }

        cg_ragdoll_blendtime->integer = saveInt;
        cg_ragdoll_blendtime->value   = saveVal;

        // Put everything back, so the run proper is what it would have been
        // had the probe never happened. The seed hands each particle a little
        // random velocity of its own, so the generator has to go back too.
        memset(cg_entities, 0, sizeof(cg_entities));
        CG_InitRagdoll();
        srand(12345);

        cent->currentValid            = qtrue;
        cent->currentState.number     = entnum;
        cent->currentState.eFlags     = EF_DEAD;
        cent->currentState.modelindex = 3;
        cent->currentState.eType      = ET_MODELANIM;
        VectorSet(cent->lerpOrigin, 0, 0, getenv("RD_STARTZ") ? (float)atof(getenv("RD_STARTZ")) : sc.startZ); if (const char *sx = getenv("RD_STARTXY")) sscanf(sx, "%f %f", &cent->lerpOrigin[0], &cent->lerpOrigin[1]);
        VectorSet(cent->lerpAngles, sc.pitch, sc.yaw, sc.roll);
    }

    // RD_DUMP writes the same trace the game writes, for checking the format
    // and its reader against a known scenario. Set after the seed probe above,
    // which builds and throws away a ragdoll of its own.
    if (getenv("RD_DUMP")) Stub_CvarSet("cg_ragdoll_dump", "1");

    // RD_BLAST="x y z kind" sets an explosion off at that spot on the frame the
    // body dies, so what a grenade does to a corpse can be looked at without
    // the game.
    if (const char *bl = getenv("RD_BLAST")) {
        vec3_t bp; int kind = 0;
        if (sscanf(bl, "%f %f %f %d", &bp[0], &bp[1], &bp[2], &kind) >= 3) {
            cg.time = 0;
            CG_RagdollNoteExplosion(bp, kind);
        }
    }

    // RD_CVAR="name=value;name=value" overrides any tuning cvar for the run, so
    // a sweep needs no rebuild and no variant source file.
    if (const char *ov = getenv("RD_CVAR")) {
        char buf[512]; Q_strncpyz(buf, ov, sizeof(buf));
        for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            Stub_CvarSet(tok, eq + 1);
        }
    }

    // RD_PILE=<height> drops a second body from that far above the first, which
    // is the only way to test one corpse against another: everything else here
    // simulates a single man.
    const int   ent2  = 9;
    const float pileZ = getenv("RD_PILE") ? (float)atof(getenv("RD_PILE")) : 0.0f;
    centity_t  *cent2 = &cg_entities[ent2];
    float       pileGap = 1e9f, pileWorst = 0.0f;

    if (pileZ > 0.0f) {
        cent2->currentValid            = qtrue;
        cent2->currentState.number     = ent2;
        cent2->currentState.eFlags     = EF_DEAD;
        cent2->currentState.modelindex = 3;
        cent2->currentState.eType      = ET_MODELANIM;
        VectorSet(cent2->lerpOrigin, pileZ < 20.0f ? 4 : 6, pileZ < 20.0f ? 3 : 4, sc.startZ + pileZ);
        if (const char *xy = getenv("RD_PILEXY")) { float x = 0, y = 0; sscanf(xy, "%f %f", &x, &y); cent2->lerpOrigin[0] = x; cent2->lerpOrigin[1] = y; }
        VectorSet(cent2->lerpAngles, sc.pitch, sc.yaw + 40.0f, sc.roll);
    }

    // RD_SHOVE="ms x y z kind" sets an explosion off that many milliseconds
    // after death, and RD_SHOT="ms x1 y1 z1 x2 y2 z2" fires a bullet along that
    // line, so what happens to a corpse that has already settled can be looked
    // at. Where the body was when it was hit, and where it ends up, are printed.
    float shoveT = -1, shoveP[3] = {0,0,0}; int shoveKind = 0;
    float shotT = -1, shotA[3] = {0,0,0}, shotB[3] = {0,0,0};
    if (const char *sv = getenv("RD_SHOVE"))
        sscanf(sv, "%f %f %f %f %d", &shoveT, &shoveP[0], &shoveP[1], &shoveP[2], &shoveKind);
    if (const char *sh = getenv("RD_SHOT"))
        sscanf(sh, "%f %f %f %f %f %f %f", &shotT, &shotA[0], &shotA[1], &shotA[2], &shotB[0], &shotB[1], &shotB[2]);
    vec3_t beforeHit = {0,0,0}; qboolean tookHit = qfalse; float movedAfter = 0;

    // RD_HANG="ms lift" picks the corpse up by the head with the grabber that
    // many milliseconds after death, straight down from above, raises it by
    // lift units over a second and holds it there to the end, as a player
    // does with cg_ragdoll_grab. A hanging body shows at once whatever holds
    // its limbs in the pose it died in: its legs and arms should hang.
    // A third number picks the joint to take hold of (default 6, the top of
    // the head), aimed at from straight above.
    float hangT = -1, hangLift = 0; int hanging = 0; vec3_t hangEye = {0,0,0}; int hangJoint = 6;
    // Spasm while held: how hard the joints change direction from one frame to
    // the next once the lift is done, the mean over frames and joints of the
    // second difference of position, and the worst single frame.
    float hangJerkSum = 0, hangJerkMax = 0; int hangJerkN = 0; float hangHist[2][23*3]; int hangHave = 0;
    // Spin while held: how fast the line across the hips turns about the
    // vertical, in degrees a second, averaged over the hold. And how deep an
    // arm gets inside the trunk while held, as a share of the two thicknesses,
    // from the particles.
    float hangSpin = 0; int hangSpinN = 0; float hangYawPrev = 0; int hangYawHave = 0;
    float hangArmIn = 0;
    if (const char *hg = getenv("RD_HANG")) {
        sscanf(hg, "%f %f %d", &hangT, &hangLift, &hangJoint);
        Stub_CvarSet("cg_ragdoll_grab", "1");
    }
    // RD_PUNT="ms pitch": at ms, punt the body from the side with the grabber,
    // aimed pitch degrees down, and measure how far the pelvis slides.
    float puntT = -1, puntPitch = 15; int punted = 0, puntStop = -1; vec3_t puntFrom = {0,0,0}; float puntTravel = 0;
    if (const char *pe = getenv("RD_PUNT")) {
        sscanf(pe, "%f %f", &puntT, &puntPitch);
        Stub_CvarSet("cg_ragdoll_grab", "1");
    }
    // RD_WALLDRAG="ms joint": grab that joint from above at ms, lift it and drag
    // it 40 units into the wall (+y) over a second, let go at ms+2000, and see
    // how hard the body leaves the wall.
    float wdT = -1; int wdJoint = 5, wdHeld = 0; vec3_t wdEye = {0,0,0}, wdRel = {0,0,0}; float wdPeak = 0, wdAway = 0, wdAwayMax = 0;
    if (const char *wd = getenv("RD_WALLDRAG")) { sscanf(wd, "%f %d", &wdT, &wdJoint); Stub_CvarSet("cg_ragdoll_grab", "1"); }
    // RD_DRAG="ms joint dx dy dz dur": grab that joint from above at ms and move
    // the aim by (dx,dy,dz) over dur ms, holding it there to the end. Measures
    // the legs while they are dragged: steps where a leg joint moves more than
    // 3 units off the body's mean motion, and the mean second difference.
    float drT = -1, drD[3] = {0,0,0}, drDur = 2000; int drJoint = 4, drHeld = 0; vec3_t drEye = {0,0,0};
    int drJumps = 0, drN = 0, drHave = 0; float drWorst = 0, drJerk = 0; float drHist[2][23*3];
    if (const char *dr = getenv("RD_DRAG")) { sscanf(dr, "%f %d %f %f %f %f", &drT, &drJoint, &drD[0], &drD[1], &drD[2], &drDur); Stub_CvarSet("cg_ragdoll_grab", "1"); }
    // RD_PATH="ms joint dx dy dz dx dy dz ...": grab that joint from above at ms,
    // move the aim through the waypoints (offsets from where it started), one
    // second each, and let go at the last one.
    float paT = -1; int paJoint = 6, paN = 0, paHeld = 0; float paW[8][3]; vec3_t paEye = {0,0,0};
    if (const char *pa = getenv("RD_PATH")) {
        const char *q = pa; int used = 0;
        if (sscanf(q, "%f %d%n", &paT, &paJoint, &used) == 2) {
            q += used;
            while (paN < 8 && sscanf(q, "%f %f %f%n", &paW[paN][0], &paW[paN][1], &paW[paN][2], &used) == 3) { q += used; paN++; }
        }
        Stub_CvarSet("cg_ragdoll_grab", "1");
    }
    const int numFrames = (hangT >= 0 || puntT >= 0 || wdT >= 0 || drT >= 0 || paT >= 0) ? 700 : 400;

    for (int frame = 0; frame < numFrames; frame++) {
        cg.time = frame * 16; cg.frametime = 16;
#ifdef RD_JOLT
        RDJ_Step(0.016f);
#endif

        if (wdT >= 0 && cg.time >= wdT) {
            float pp[23*3];
            if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                if (wdHeld == 0) {
                    VectorSet(wdEye, pp[wdJoint*3], pp[wdJoint*3+1], pp[wdJoint*3+2] + 40.0f);
                    VectorCopy(wdEye, cg.refdef.vieworg);
                    vec3_t down = {90, 0, 0}; AnglesToAxis(down, cg.refdef.viewaxis);
                    CG_RagdollGrabDown_f(); wdHeld = 1;
                } else if (wdHeld == 1) {
                    const float f = Q_min(1.0f, (cg.time - wdT) / 1000.0f);
                    VectorCopy(wdEye, cg.refdef.vieworg);
                    cg.refdef.vieworg[1] += 40.0f * f; cg.refdef.vieworg[2] += 30.0f * f;
                    if (cg.time >= wdT + 2000) { CG_RagdollGrabUp_f(); wdHeld = 2; VectorCopy(&pp[0], wdRel); }
                } else if (cg.time < wdT + 3000) {
                    static float last[23*3]; static int haveLast;
                    if (wdHeld == 2) { haveLast = 0; wdHeld = 3; }
                    if (haveLast) {
                        vec3_t m = {0,0,0};
                        for (int q = 0; q < 23; q++) for (int k = 0; k < 3; k++) m[k] += (pp[q*3+k] - last[q*3+k]) / 23.0f / 0.016f;
                        const float sp = VectorLength(m); if (sp > wdPeak) wdPeak = sp;
                        if (-m[1] > wdAwayMax) wdAwayMax = -m[1];
                    }
                    memcpy(last, pp, sizeof(last)); haveLast = 1;
                    wdAway = wdRel[1] - pp[1];
                }
            }
        }

        if (drT >= 0 && cg.time >= drT) {
            float pp[23*3];
            if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                if (!drHeld) {
                    VectorSet(drEye, pp[drJoint*3], pp[drJoint*3+1], pp[drJoint*3+2] + 40.0f);
                    VectorCopy(drEye, cg.refdef.vieworg);
                    vec3_t down = {90, 0, 0}; AnglesToAxis(down, cg.refdef.viewaxis);
                    CG_RagdollGrabDown_f(); drHeld = 1;
                } else {
                    // Level first, then down, so the grab never drives the body
                    // into the top of what it is lying on.
                    const float f  = Q_min(1.0f, (cg.time - drT) / (drDur * 0.5f));
                    const float fz = Q_max(0.0f, Q_min(1.0f, (cg.time - drT - drDur * 0.5f) / (drDur * 0.5f)));
                    for (int k = 0; k < 2; k++) cg.refdef.vieworg[k] = drEye[k] + drD[k] * f;
                    cg.refdef.vieworg[2] = drEye[2] + drD[2] * fz;
                    if (cg.time > drT + 200) {
                        if (drHave >= 1) {
                            float m[3] = {0,0,0};
                            for (int q = 0; q < 23; q++) for (int k = 0; k < 3; k++) m[k] += (pp[q*3+k] - drHist[1][q*3+k]) / 23.0f;
                            for (int q = 15; q < 23; q++) {
                                vec3_t r; for (int k = 0; k < 3; k++) r[k] = pp[q*3+k] - drHist[1][q*3+k] - m[k];
                                const float l = VectorLength(r); if (l > drWorst) drWorst = l; if (l > 3.0f) { drJumps++; if (getenv("RD_DRAGDBG")) printf("JUMP t=%d joint %d %.1f  p %.1f %.1f %.1f\n", cg.time, q, l, pp[q*3], pp[q*3+1], pp[q*3+2]); }
                                if (drHave >= 2) { vec3_t a; for (int k = 0; k < 3; k++) a[k] = pp[q*3+k] - 2*drHist[1][q*3+k] + drHist[0][q*3+k]; drJerk += VectorLength(a); drN++; }
                            }
                        }
                        memcpy(drHist[0], drHist[1], sizeof(drHist[0])); memcpy(drHist[1], pp, sizeof(drHist[1])); drHave++;
                    }
                }
            }
        }

        if (paT >= 0 && paN > 0 && cg.time >= paT && paHeld < 2) {
            float pp[23*3];
            if (!paHeld) {
                if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                    VectorSet(paEye, pp[paJoint*3], pp[paJoint*3+1], pp[paJoint*3+2] + 40.0f);
                    VectorCopy(paEye, cg.refdef.vieworg);
                    vec3_t down = {90, 0, 0}; AnglesToAxis(down, cg.refdef.viewaxis);
                    CG_RagdollGrabDown_f(); paHeld = 1;
                }
            } else {
                const float u = (cg.time - paT) / 1000.0f;
                const int   seg = (int)u;
                if (seg >= paN) { CG_RagdollGrabUp_f(); paHeld = 2; }
                else {
                    const float f = u - seg;
                    float a[3] = {0,0,0}; if (seg > 0) for (int k = 0; k < 3; k++) a[k] = paW[seg-1][k];
                    for (int k = 0; k < 3; k++) cg.refdef.vieworg[k] = paEye[k] + a[k] + (paW[seg][k] - a[k]) * f;
                }
            }
        }

        if (puntT >= 0 && cg.time >= puntT) {
            float pp[23*3];
            if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                if (!punted) {
                    vec3_t ang = {puntPitch, 0, 0};
                    VectorSet(cg.refdef.vieworg, pp[0] - 60.0f, pp[1], pp[2] + 60.0f * tanf(DEG2RAD(puntPitch)));
                    AnglesToAxis(ang, cg.refdef.viewaxis);
                    VectorCopy(&pp[0], puntFrom);
                    CG_RagdollPunt_f();
                    punted = 1;
                } else {
                    static float last[3];
                    const float dx = pp[0] - puntFrom[0], dy = pp[1] - puntFrom[1];
                    puntTravel = sqrtf(dx*dx + dy*dy);
                    if (punted > 3 && puntStop < 0 && Distance(&pp[0], last) < 0.05f) puntStop = cg.time - (int)puntT;
                    if (puntStop >= 0 && Distance(&pp[0], last) > 0.2f) puntStop = -1;
                    VectorCopy(&pp[0], last);
                    punted++;
                }
            }
        }

        // RD_HANGDROP=ms lets go of the hang at ms, so a body lifted past
        // something can be dropped onto it.
        static int hangDropped = 0;
        if (hangT >= 0 && hanging && !hangDropped && getenv("RD_HANGDROP") && cg.time >= atoi(getenv("RD_HANGDROP"))) {
            CG_RagdollGrabUp_f(); hangDropped = 1;
        }
        if (hangT >= 0 && cg.time >= hangT && !hangDropped) {
            if (!hanging) {
                float pp[23*3];
                if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                    // From 40 units straight above the top of the head.
                    VectorSet(hangEye, pp[hangJoint*3], pp[hangJoint*3+1], pp[hangJoint*3+2] + 40.0f);
                    VectorCopy(hangEye, cg.refdef.vieworg);
                    vec3_t down = {90, 0, 0};
                    AnglesToAxis(down, cg.refdef.viewaxis);
                    CG_RagdollGrabDown_f();
                    hanging = 1;
                }
            } else {
                const float up = hangLift * Q_min(1.0f, (cg.time - hangT) / 1000.0f);
                VectorCopy(hangEye, cg.refdef.vieworg);
                cg.refdef.vieworg[2] += up;
                if (const char *sw = getenv("RD_HANGSWING")) {
                    float amp = 60, per = 800; sscanf(sw, "%f %f", &amp, &per);
                    if (cg.time > hangT + 1000) cg.refdef.vieworg[0] += amp * sinf((cg.time - hangT - 1000) * 6.2831853f / per);
                }

                if (cg.time > hangT + 1500) {
                    float pp[23*3];
                    if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                        {
                            static float fl[2][23*3]; static int flHave;
                            if (cg.time <= hangT + 1520) flHave = 0;
                            if (flHave >= 2) for (int q = 0; q < 23; q++) {
                                vec3_t d1, d2; VectorSubtract(&fl[1][q*3], &fl[0][q*3], d1); VectorSubtract(&pp[q*3], &fl[1][q*3], d2);
                                if (VectorLength(d2) > 2.0f && VectorLength(d1) > 2.0f && DotProduct(d1, d2) < -0.5f * VectorLength(d1) * VectorLength(d2)) gHangFlips++;
                            }
                            memcpy(fl[0], fl[1], sizeof(fl[0])); memcpy(fl[1], pp, sizeof(fl[1])); if (flHave < 2) flHave++;
                        }
                        if (hangHave >= 2) {
                            float frameSum = 0;
                            for (int j = 0; j < 23; j++) {
                                vec3_t d;
                                for (int k = 0; k < 3; k++)
                                    d[k] = pp[j*3+k] - 2.0f * hangHist[1][j*3+k] + hangHist[0][j*3+k];
                                frameSum += VectorLength(d);
                            }
                            frameSum /= 23.0f;
                            hangJerkSum += frameSum; hangJerkN++;
                            if (frameSum > hangJerkMax) hangJerkMax = frameSum;
                        }
                        if (hangHave >= 1) {
                            // The whole body's turn about the vertical: each joint's
                            // sweep round the centre of the cloud, weighted by how far
                            // out it is, which does not depend on any one line through
                            // the body staying level.
                            vec3_t c = {0,0,0};
                            for (int j = 0; j < 23; j++) VectorAdd(c, &pp[j*3], c);
                            VectorScale(c, 1.0f/23.0f, c);
                            float num = 0, den = 0;
                            for (int j = 0; j < 23; j++) {
                                const float rx = pp[j*3] - c[0], ry = pp[j*3+1] - c[1];
                                const float vx = pp[j*3] - hangHist[1][j*3], vy = pp[j*3+1] - hangHist[1][j*3+1];
                                num += rx * vy - ry * vx;
                                den += rx * rx + ry * ry;
                            }
                            if (den > 1.0f) { hangSpin += fabsf(RAD2DEG(num / den)) / 0.016f; hangSpinN++; }
                        }
                        {
                            static const int arm[4][2] = {{7,8},{8,9},{11,12},{12,13}};
                            static const int trunk[4][2] = {{0,1},{1,2},{2,3},{3,4}};
                            for (int a = 0; a < 4; a++) for (int t = 0; t < 4; t++) {
                                float ta, tb; vec3_t dir;
                                float dist = SegSegDist(&pp[arm[a][0]*3], &pp[arm[a][1]*3], &pp[trunk[t][0]*3], &pp[trunk[t][1]*3], &ta, &tb, dir);
                                // An upper arm starts at the shoulder, which is on the trunk: only its far half counts.
                                if ((a == 0 || a == 2) && ta < 0.5f) continue;
                                const float want = (5.0f + 2.7f) * sc.scale;
                                const float pen = (want - dist) / want;
                                if (pen > hangArmIn) hangArmIn = pen;
                            }
                        }
                        memcpy(hangHist[0], hangHist[1], sizeof(hangHist[0]));
                        memcpy(hangHist[1], pp, sizeof(pp));
                        hangHave++;
                    }
                }

                if (getenv("RD_HANGDBG") && frame % 25 == 0) {
                    float pp[23*3];
                    if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23)
                        fprintf(stderr, "t %5d eye z %6.1f  headtip %6.1f %6.1f %6.1f  pelvis %6.1f %6.1f %6.1f  lfoot z %6.1f\n",
                                cg.time, cg.refdef.vieworg[2], pp[18], pp[19], pp[20], pp[0], pp[1], pp[2], pp[17*3+2]);
                }
            }
        }

        if (shoveT >= 0 && cg.time >= shoveT && !tookHit) {
            float pp[23*3];
            if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                VectorSet(beforeHit, pp[0], pp[1], pp[2]);
                CG_RagdollNoteExplosion(shoveP, shoveKind);
                tookHit = qtrue;
            }
        }
        if (shotT >= 0 && cg.time >= shotT && !tookHit) {
            float pp[23*3];
            if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                VectorSet(beforeHit, pp[0], pp[1], pp[2]);
                vec3_t stopAt;
                CG_RagdollNoteBullet(shotA, shotB, 0, stopAt);
                tookHit = qtrue;
            }
        }
        if (tookHit) {
            float pp[23*3];
            if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
                vec3_t d; for (int k=0;k<3;k++) d[k]=pp[k]-beforeHit[k];
                float len = VectorLength(d);
                if (len > movedAfter) movedAfter = len;
            }
        }

        if (getenv("RD_HALFFRAME") && frame > 0) {
            cg.time = frame * 16 - 8; cg.frametime = 8;
            {
            refEntity_t model; memset(&model, 0, sizeof(model));
            model.entityNumber = entnum; model.tiki = &g_tiki; model.scale = sc.scale;
            VectorCopy(cent->lerpOrigin, model.origin);
            AnglesToAxis(cent->lerpAngles, model.axis);
    
            CG_RagdollUpdateEntity(cent, &model);
            }
            cg.time = frame * 16; cg.frametime = 8;
        }
        refEntity_t model; memset(&model, 0, sizeof(model));
        model.entityNumber = entnum; model.tiki = &g_tiki; model.scale = sc.scale;
        VectorCopy(cent->lerpOrigin, model.origin);
        AnglesToAxis(cent->lerpAngles, model.axis);

        CG_RagdollUpdateEntity(cent, &model);

        static float pileDelay = -1, firstAt[3]; static int firstHave;
        if (pileDelay < 0) pileDelay = getenv("RD_PILEDELAY") ? (float)atof(getenv("RD_PILEDELAY")) : 0.0f;
        if (pileZ > 0.0f) {
            static float lastA[23*3], lastB[23*3]; static int haveLast;
            float pa[23*3], pb[23*3];
            if (frame == 0) { haveLast = 0; gPileJump = 0; }
            int na = CG_RagdollDebugParticles(entnum, pa, 23), nb = CG_RagdollDebugParticles(ent2, pb, 23);
            if (na >= 23 && nb >= 23) {
                if (haveLast && frame > 60) for (int q = 0; q < 23; q++) {
                    float da = Distance(&pa[q*3], &lastA[q*3]), db = Distance(&pb[q*3], &lastB[q*3]);
                    if (da > gPileJump) gPileJump = da; if (db > gPileJump) gPileJump = db;
                    if (getenv("JUMPDBG") && (da > 12 || db > 12)) fprintf(stderr, "JUMP t=%d joint %d body%s %.1f\n", cg.time, q, da > db ? "1" : "2", da > db ? da : db);
                }
                memcpy(lastA, pa, sizeof(pa)); memcpy(lastB, pb, sizeof(pb)); haveLast = 1;
            }
        }
        if (pileZ > 0.0f && (pileDelay > 0.0f || getenv("RD_PILEPUNT"))) {
            float pa[23*3];
            if (CG_RagdollDebugParticles(entnum, pa, 23) >= 23) {
                if (cg.time < (getenv("RD_PILEPUNT") ? atof(getenv("RD_PILEPUNT")) : pileDelay)) { VectorCopy(&pa[0], firstAt); firstHave = 1; }
                else if (frame == numFrames - 1 && firstHave) printf("%28s   first body moved %.1f units after the second arrived\n", "", Distance(&pa[0], firstAt));
            }
        }
        // RD_PILEPUNT="ms": punt the second body along the ground toward the first.
        static float pilePunt = -2; static int pilePunted;
        if (pilePunt < -1) { pilePunt = getenv("RD_PILEPUNT") ? (float)atof(getenv("RD_PILEPUNT")) : -1; pilePunted = 0; }
        if (pileZ > 0.0f && pilePunt >= 0 && cg.time >= pilePunt && !pilePunted) {
            float pa[23*3], pb[23*3];
            if (CG_RagdollDebugParticles(entnum, pa, 23) >= 23 && CG_RagdollDebugParticles(ent2, pb, 23) >= 23) {
                vec3_t dir; VectorSubtract(&pa[0], &pb[0], dir); dir[2] = 0; VectorNormalize(dir);
                vec3_t ang; vectoangles(dir, ang); ang[0] = 5;
                VectorMA(&pb[0], -60.0f, dir, cg.refdef.vieworg); cg.refdef.vieworg[2] = pb[2] + 60.0f * tanf(DEG2RAD(5));
                AnglesToAxis(ang, cg.refdef.viewaxis);
                Stub_CvarSet("cg_ragdoll_grab", "1");
                CG_RagdollPunt_f();
                pilePunted = 1;
            }
        }
        if (pileZ > 0.0f && cg.time >= pileDelay) {
            refEntity_t m2; memset(&m2, 0, sizeof(m2));
            m2.entityNumber = ent2; m2.tiki = &g_tiki; m2.scale = sc.scale;
            VectorCopy(cent2->lerpOrigin, m2.origin);
            AnglesToAxis(cent2->lerpAngles, m2.axis);
            CG_RagdollUpdateEntity(cent2, &m2);

            if (frame > 200) {
                float pa[23*3], pb[23*3];
                if (CG_RagdollDebugParticles(entnum, pa, 23) >= 23
                    && CG_RagdollDebugParticles(ent2, pb, 23) >= 23) {
                    for (int a = 0; a < 23; a++) {
                        for (int b = 0; b < 23; b++) {
                            vec3_t d;
                            for (int k = 0; k < 3; k++) d[k] = pa[a*3+k] - pb[b*3+k];
                            float len = VectorLength(d);
                            if (len < pileGap) pileGap = len;
                        }
                        vec3_t d2;
                        for (int k = 0; k < 3; k++) d2[k] = pa[a*3+k] - cent->lerpOrigin[k];
                        float far_ = VectorLength(d2);
                        if (far_ > pileWorst) pileWorst = far_;
                    }
                }
            }
        }

        if (!model.num_bone_overrides) continue;

        // Reconstruct world positions from the emitted model-space matrices,
        // the same way the renderer will.
        float wp[64][3]; float wx[64][3]; float wy[64][3]; int have[64] = {0};
        for (int i = 0; i < model.num_bone_overrides; i++) {
            int b = model.bone_override[i].boneIndex;
            const float *o = model.bone_override[i].matrix[3];
            vec3_t w; VectorCopy(model.origin, w);
            float sc2 = model.scale * g_tiki.load_scale;
            for (int k = 0; k < 3; k++) VectorMA(w, (o[k] + g_tiki.load_origin[k]) * sc2, model.axis[k], w);
            VectorCopy(w, wp[b]); have[b] = 1;
            // The bone's own X axis, in world space. Biped bones run along X,
            // so this is the direction the bone points, which is what the
            // renderer skins to and therefore what the eye sees.
            {
                vec3_t ax; VectorClear(ax);
                for (int k = 0; k < 3; k++)
                    VectorMA(ax, model.bone_override[i].matrix[0][k], model.axis[k], ax);
                VectorNormalize(ax); VectorCopy(ax, wx[b]);
                vec3_t ay; VectorClear(ay);
                for (int k = 0; k < 3; k++)
                    VectorMA(ay, model.bone_override[i].matrix[1][k], model.axis[k], ay);
                VectorNormalize(ay); VectorCopy(ay, wy[b]);
            }

            const float (*m)[3] = model.bone_override[i].matrix;
            for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) if (!std::isfinite(m[r][c])) bad++;
            for (int r = 0; r < 3; r++) {
                float n = sqrtf(m[r][0]*m[r][0]+m[r][1]*m[r][1]+m[r][2]*m[r][2]);
                if (fabsf(n-1.0f) > 1e-3f) bad++;
            }
        }

        static const char *pairs[][2] = {
            {"Bip01 Pelvis","Bip01 Spine"},{"Bip01 Spine","Bip01 Spine1"},{"Bip01 Spine1","Bip01 Spine2"},
            {"Bip01 Spine2","Bip01 Neck"},{"Bip01 Neck","Bip01 Head"},
            {"Bip01 L UpperArm","Bip01 L Forearm"},{"Bip01 L Forearm","Bip01 L Hand"},
            {"Bip01 R UpperArm","Bip01 R Forearm"},{"Bip01 R Forearm","Bip01 R Hand"},
            {"Bip01 L Thigh","Bip01 L Calf"},{"Bip01 L Calf","Bip01 L Foot"},
            {"Bip01 R Thigh","Bip01 R Calf"},{"Bip01 R Calf","Bip01 R Foot"},
        };
        // Reference lengths come from the pose as actually seeded, not from
        // the untwisted rest data: a scenario that starts the body wrung out
        // genuinely has different bone lengths, and measuring against the
        // wrong reference reports stretch that is not there.
        for (unsigned q = 0; q < sizeof(pairs)/sizeof(pairs[0]); q++) {
            int ia = Stub_TagNumForName(NULL, pairs[q][0]), ib = Stub_TagNumForName(NULL, pairs[q][1]);
            if (!have[ia] || !have[ib]) continue;
            vec3_t d; VectorSubtract(wp[ia], wp[ib], d);
            float now = VectorLength(d);
            if (refLen[q] <= 0.0f) { refLen[q] = now; continue; }
            float err = fabsf(now - refLen[q]) / refLen[q];
            if (err > worstStretch) worstStretch = err;
            if (frame > 150 && err > lateStretch) lateStretch = err;
        }

        // Self intersection, checked against the same thicknesses the solver
        // uses, so this verifies the solver reaches its own goal.
        {
            // radii and joint indices come from g_bones itself
            for (int ia = 0; ia < NUM_BONES; ia++) {
                for (int ib = ia + 1; ib < NUM_BONES; ib++) {
                    if (!have[ia] || !have[ib]) continue;
                    if (g_bones[ia].radius <= 0.0f || g_bones[ib].radius <= 0.0f) continue;
                    float want = (g_bones[ia].radius + g_bones[ib].radius) * sc.scale;
                    if (g_bones[ia].jointIdx > 6 && g_bones[ib].jointIdx > 6) want *= 0.6f;
                    vec3_t d;
                    // Rest separation taken once the death animation has
                    // finished, not at the instant of death.
                    //
                    // This matters more than it looks. A death animation that
                    // brings the arms in to the chest legitimately ends with
                    // them far closer together than they started. Baselined at
                    // t = 0, with the arms still out, every one of those poses
                    // reads as a deep intersection however clean it is, and the
                    // only way to score well is to shove the arms back out to
                    // where they were when the body was shot, which is both
                    // wrong and exactly what it looks like. Baselined after the
                    // animation, this asks the question that was meant: given
                    // where the body actually is, is anything inside anything.
                    if (frame >= gBlendFrames && restSep[ia][ib] <= 0.0f) {
                        VectorSubtract(wp[ia], wp[ib], d);
                        restSep[ia][ib] = VectorLength(d);
                    }
                    if (restSep[ia][ib] <= 0.0f) continue;
                    if (restSep[ia][ib] < want) want = restSep[ia][ib] * 0.85f;
                    if (want < 0.01f) continue;
                    VectorSubtract(wp[ia], wp[ib], d);
                    float pen = (want - VectorLength(d)) / want;
                    if (frame > 60 && pen > worstPen) {
                        worstPen = pen; gPenA = g_bones[ia].name; gPenB = g_bones[ib].name;
                    }
                }
            }
        }

        // Hyperextension: a two bone limb may never be longer than its own
        // segments added together. This is the meaningful elbow check.
        {
            static const struct { const char *a,*m,*b; } hx[] = {
                {"Bip01 L UpperArm","Bip01 L Forearm","Bip01 L Hand"},
                {"Bip01 R UpperArm","Bip01 R Forearm","Bip01 R Hand"},
                {"Bip01 L Thigh","Bip01 L Calf","Bip01 L Foot"},
                {"Bip01 R Thigh","Bip01 R Calf","Bip01 R Foot"},
            };
            for (unsigned q = 0; q < sizeof(hx)/sizeof(hx[0]); q++) {
                int a=Stub_TagNumForName(NULL,hx[q].a),m=Stub_TagNumForName(NULL,hx[q].m),b=Stub_TagNumForName(NULL,hx[q].b);
                if(!have[a]||!have[m]||!have[b]) continue;
                vec3_t d1,d2,dd;
                VectorSubtract(wp[m],wp[a],d1); VectorSubtract(wp[b],wp[m],d2); VectorSubtract(wp[b],wp[a],dd);
                float chain = VectorLength(d1)+VectorLength(d2);
                float over  = VectorLength(dd) - chain;
                if (frame > 60 && over > worstHyper) worstHyper = over;
            }
        }

        // Bend angle at the joints the eye is most sensitive to, in degrees
        // away from straight. A limp wrist or neck folded past what a body can
        // do is very obvious even when everything else looks right.
        {
            // Measured between the two bones' own X axes rather than through
            // a tip joint: the real model has no "Bip01 HeadNub" and no
            // "Bip01 L/R Finger1", so there is no tip bone to read.
            static const struct { const char *a,*b; int slot; } jt[] = {
                {"Bip01 L Forearm","Bip01 L Hand", 0},
                {"Bip01 R Forearm","Bip01 R Hand", 0},
                {"Bip01 Neck",     "Bip01 Head",   1},
                {"Bip01 Spine2",   "Bip01 Neck",   1},
                {"Bip01 L Calf",   "Bip01 L Foot", 2},
                {"Bip01 R Calf",   "Bip01 R Foot", 2},
            };
            for (unsigned q = 0; q < sizeof(jt)/sizeof(jt[0]); q++) {
                int a=Stub_TagNumForName(NULL,jt[q].a),b=Stub_TagNumForName(NULL,jt[q].b);
                if(a<0||b<0||!have[a]||!have[b]) continue;
                float dot = DotProduct(wx[a],wx[b]); if(dot>1)dot=1; if(dot<-1)dot=-1;
                float deg = acosf(dot)*180.0f/3.14159265f;
                // measured as a change from the rest pose, not an absolute angle
                vec3_t ru,rv; Stub_RestAxis(a,ru); Stub_RestAxis(b,rv);
                float rdot=DotProduct(ru,rv); if(rdot>1)rdot=1; if(rdot<-1)rdot=-1;
                float rest = acosf(rdot)*180.0f/3.14159265f;
                float dev = fabsf(deg - rest);
                if (frame > 60 && dev > jointDev[jt[q].slot]) jointDev[jt[q].slot] = dev;
            }
        }

        // Torso twist: the angle between the hip axis and the shoulder axis,
        // both measured perpendicular to the spine. This is what distance
        // constraints are blind to, and a large value here is what pinches a
        // skinned midsection into an hourglass.
        {
            int ip=Stub_TagNumForName(NULL,"Bip01 Pelvis"), is=Stub_TagNumForName(NULL,"Bip01 Spine2");
            int il=Stub_TagNumForName(NULL,"Bip01 L UpperArm"), ir=Stub_TagNumForName(NULL,"Bip01 R UpperArm");
            int jl=Stub_TagNumForName(NULL,"Bip01 L Thigh"),   jr=Stub_TagNumForName(NULL,"Bip01 R Thigh");
            if (have[ip]&&have[is]&&have[il]&&have[ir]&&have[jl]&&have[jr]) {
                vec3_t axis, sh, hp;
                VectorSubtract(wp[is], wp[ip], axis);
                if (VectorNormalize(axis) > 0.01f) {
                    VectorSubtract(wp[ir], wp[il], sh);
                    VectorSubtract(wp[jr], wp[jl], hp);
                    VectorMA(sh, -DotProduct(sh,axis), axis, sh);
                    VectorMA(hp, -DotProduct(hp,axis), axis, hp);
                    if (VectorNormalize(sh) > 0.01f && VectorNormalize(hp) > 0.01f) {
                        float dot = DotProduct(sh,hp);
                        if(dot>1)dot=1; if(dot<-1)dot=-1;
                        float deg = acosf(dot)*180.0f/3.14159265f;
                        // Measured as a change from the pose the body died in,
                        // not as an absolute angle. A death animation can leave
                        // a corpse genuinely wrung out, and counting that as the
                        // solver's doing confuses what the animation put there
                        // with what the simulation added.
                        if (twistRest0 < 0.0f) twistRest0 = deg;
                        else {
                            float dev = fabsf(deg - twistRest0);
                            if (frame > 60 && dev > worstTwist) worstTwist = dev;
                        }
                    }
                }
            }
        }

        // How much of the torso twist sits at one joint. A body may be wrung
        // out overall and still look right if the twist is spread along the
        // spine; put it all into one joint and the mesh skinned across that
        // joint nips in like a wrung towel.
        {
            static const char *chain[] = {"Bip01 Pelvis","Bip01 Spine","Bip01 Spine1","Bip01 Spine2","Bip01 Neck"};
            for (int q = 0; q + 1 < 5; q++) {
                int ia=Stub_TagNumForName(NULL,chain[q]), ib=Stub_TagNumForName(NULL,chain[q+1]);
                if (ia<0||ib<0) continue;
                // y axis of each bone, from the emitted matrices
                int oa=-1, ob=-1;
                for (int n2 = 0; n2 < model.num_bone_overrides; n2++) {
                    if (model.bone_override[n2].boneIndex==ia) oa=n2;
                    if (model.bone_override[n2].boneIndex==ib) ob=n2;
                }
                if (oa<0||ob<0) continue;
                vec3_t xa, ya, yb, axis;
                for(int k=0;k<3;k++){ xa[k]=model.bone_override[oa].matrix[0][k];
                                      ya[k]=model.bone_override[oa].matrix[1][k];
                                      yb[k]=model.bone_override[ob].matrix[1][k]; }
                VectorCopy(xa, axis);
                if (VectorNormalize(axis) < 0.01f) continue;
                VectorMA(ya,-DotProduct(ya,axis),axis,ya);
                VectorMA(yb,-DotProduct(yb,axis),axis,yb);
                if (VectorNormalize(ya)<0.01f||VectorNormalize(yb)<0.01f) continue;
                float dot=DotProduct(ya,yb); if(dot>1)dot=1; if(dot<-1)dot=-1;
                float deg=acosf(dot)*180.0f/3.14159265f;
                if (frame > 60 && deg > worstJointTwist) worstJointTwist = deg;
            }
        }

        // How far the elbow sits above the straight line from shoulder to
        // hand, in world Z. A limp arm should sag, so this should be at or
        // below zero. Positive means the elbow is being held up in an inverted
        // V with the hand hanging below it.
        {
            static const struct { const char *s,*e,*h; } arm[] = {
                {"Bip01 L UpperArm","Bip01 L Forearm","Bip01 L Hand"},
                {"Bip01 R UpperArm","Bip01 R Forearm","Bip01 R Hand"},
            };
            for (unsigned q = 0; q < 2; q++) {
                int a=Stub_TagNumForName(NULL,arm[q].s),m=Stub_TagNumForName(NULL,arm[q].e),b=Stub_TagNumForName(NULL,arm[q].h);
                if(!have[a]||!have[m]||!have[b]) continue;
                float rise = wp[m][2] - 0.5f*(wp[a][2] + wp[b][2]);
                if (frame > 300 && rise > elbowRise) elbowRise = rise;
            }
        }

        // Bend between consecutive spine bones. A mesh skinned across a joint
        // collapses in proportion to how far the two bones either side of it
        // have rotated apart, so a single joint taking the whole curve of the
        // back is what nips a waist in to an hourglass. Spreading the same
        // total curve over several joints looks completely normal.
        {
            static const char *chain[] = {"Bip01 Pelvis","Bip01 Spine","Bip01 Spine1","Bip01 Spine2","Bip01 Neck","Bip01 Head"};
            for (int q = 0; q + 1 < 6; q++) {
                int ia=Stub_TagNumForName(NULL,chain[q]), ib=Stub_TagNumForName(NULL,chain[q+1]);
                int oa=-1, ob=-1;
                for (int n2 = 0; n2 < model.num_bone_overrides; n2++) {
                    if (model.bone_override[n2].boneIndex==ia) oa=n2;
                    if (model.bone_override[n2].boneIndex==ib) ob=n2;
                }
                if (oa<0||ob<0) continue;
                vec3_t xa, xb;
                for(int k=0;k<3;k++){ xa[k]=model.bone_override[oa].matrix[0][k];
                                      xb[k]=model.bone_override[ob].matrix[0][k]; }
                if (VectorNormalize(xa)<0.01f||VectorNormalize(xb)<0.01f) continue;
                float dot=DotProduct(xa,xb); if(dot>1)dot=1; if(dot<-1)dot=-1;
                float deg=acosf(dot)*180.0f/3.14159265f;
                if (frame > 60) {
                    if (deg > worstJointBend) { worstJointBend = deg; gBendAt = chain[q]; }
                    if (deg > bendPer[q]) bendPer[q] = deg;
                }
            }
        }

        // How far a knee has swung out of the plane its leg bends in. A hinge
        // joint should stay in one plane; sideways travel here is a leg bent a
        // way a leg cannot bend.
        {
            int ip=Stub_TagNumForName(NULL,"Bip01 Pelvis"), is=Stub_TagNumForName(NULL,"Bip01 Spine1");
            int il=Stub_TagNumForName(NULL,"Bip01 L UpperArm"), ir=Stub_TagNumForName(NULL,"Bip01 R UpperArm");
            static const struct { const char *a,*m,*b; int sign; } kn[] = {
                {"Bip01 L Thigh","Bip01 L Calf","Bip01 L Foot", +1},
                {"Bip01 R Thigh","Bip01 R Calf","Bip01 R Foot", +1},
            };
            if (have[ip]&&have[is]&&have[il]&&have[ir]) {
                vec3_t tx, th, tz;
                VectorSubtract(wp[is], wp[ip], tx); VectorNormalize(tx);
                VectorSubtract(wp[ir], wp[il], th); VectorNormalize(th);
                CrossProduct(tx, th, tz);
                if (VectorNormalize(tz) > 0.1f) {
                    for (unsigned q=0;q<2;q++) {
                        int a=Stub_TagNumForName(NULL,kn[q].a),m=Stub_TagNumForName(NULL,kn[q].m),b=Stub_TagNumForName(NULL,kn[q].b);
                        if(!have[a]||!have[m]||!have[b]) continue;
                        vec3_t chord, off, want, lat;
                        VectorSubtract(wp[b],wp[a],chord);
                        if (VectorNormalize(chord)<0.01f) continue;
                        VectorScale(tz, (float)kn[q].sign, want);
                        VectorMA(want,-DotProduct(want,chord),chord,want);
                        if (VectorNormalize(want)<0.01f) continue;
                        VectorSubtract(wp[m],wp[a],off);
                        VectorMA(off,-DotProduct(off,chord),chord,off);
                        float mag=VectorLength(off);
                        if (mag<1.0f) continue;
                        CrossProduct(chord,want,lat);
                        if (VectorNormalize(lat)<0.01f) continue;
                        float outp=fabsf(DotProduct(off,lat));
                        if (frame>60 && outp>kneeLateral) kneeLateral=outp;

                        // A knee folded the wrong way. The offset from the hip
                        // to foot chord belongs on the anatomical forward side;
                        // on the other side the leg is bent a way a leg does
                        // not go. This is the "knee back" column, which counted
                        // nothing at all until now: the variable was declared
                        // and printed and never once incremented, so the gate
                        // term that tests it was passing vacuously.
                        if (frame>60 && DotProduct(off,want) < -1.0f) backBends++;
                    }
                }
            }
        }

        // How far a limb bone has got inside the trunk, measured against the
        // full body thickness with no allowance for how close the two happened
        // to be at the moment of death. That allowance is what lets an arm sink
        // into the chest near the shoulder, where the two are close anyway, so
        // it has to be left out to see the problem at all.
        {
            // Only the forearm, and only against the lower trunk. An upper arm
            // sits partly inside a sphere centred on the spine no matter what,
            // because that is where a shoulder is; counting that as an
            // intersection buries the real signal in geometry that was never
            // wrong.
            static const struct { const char *a,*b; } seg[] = {
                {"Bip01 L Forearm","Bip01 L Hand"}, {"Bip01 R Forearm","Bip01 R Hand"},
            };
            // The trunk as the solver now models it, a chain of capsules, but
            // measured at the joints' full radii rather than the reduced depth
            // the solver settles for. This is deliberately the stricter bar: it
            // asks how far the arm is inside the torso the player can see, not
            // how far it is inside the volume the solver agreed to defend.
            static const struct { const char *a,*b; float r; } trunk[] = {
                {"Bip01 Pelvis","Bip01 Spine",  5.0f},
                {"Bip01 Spine", "Bip01 Spine1", 5.0f},
                {"Bip01 Spine1","Bip01 Spine2", 4.8f},
            };
            for (unsigned q=0;q<2;q++) for (unsigned t=0;t<3;t++) {
                int a=Stub_TagNumForName(NULL,seg[q].a), b=Stub_TagNumForName(NULL,seg[q].b);
                int c=Stub_TagNumForName(NULL,trunk[t].a), dd=Stub_TagNumForName(NULL,trunk[t].b);
                if(a<0||b<0||c<0||dd<0) continue;
                if(!have[a]||!have[b]||!have[c]||!have[dd]) continue;
                float ta,tb; vec3_t dir;
                float dist=SegSegDist(wp[a],wp[b],wp[c],wp[dd],&ta,&tb,dir);
                float want=(trunk[t].r + 2.7f)*sc.scale;   // trunk radius + a limb bone
                float pen=(want-dist)/want;
                if (frame>60 && pen>armInTorso) armInTorso=pen;
            }
        }

        // The particle cloud itself. If the solver lets the particles bunch
        // up, the drawn skeleton keeps its proper bone lengths regardless and
        // simply drifts away from the simulation, so the mesh ends up standing
        // somewhere the physics is not.
        {
            float pp[23*3];
            int np = CG_RagdollDebugParticles(entnum, pp, 23);
            if (np >= 23) {
                static const int pj[][2] = {
                    {0,1},{1,2},{2,3},{3,4},{4,5},
                    {3,7},{7,8},{8,9}, {3,11},{11,12},{12,13},
                    {0,15},{15,16},{16,17}, {0,19},{19,20},{20,21},
                };
                if (frame>250) {
                    if (jitHave) {
                        float acc = 0;
                        for (int q=0;q<23;q++) {
                            vec3_t d; for(int k=0;k<3;k++) d[k]=pp[q*3+k]-jitPrev[q*3+k];
                            acc += VectorLength(d);
                        }
                        jitSum += acc/23.0f; jitN++;
                    }
                    memcpy(jitPrev, pp, sizeof(jitPrev)); jitHave = 1;
                }
                if (frame>300) {
                    for (int sd=0; sd<2; sd++) {
                        int u=Stub_TagNumForName(NULL, sd? "Bip01 R UpperArm":"Bip01 L UpperArm");
                        int e=Stub_TagNumForName(NULL, sd? "Bip01 R Forearm":"Bip01 L Forearm");
                        int hd=Stub_TagNumForName(NULL, sd? "Bip01 R Hand":"Bip01 L Hand");
                        if(u<0||e<0||hd<0||!have[u]||!have[e]||!have[hd]) continue;
                        vec3_t a,b; VectorSubtract(wp[e],wp[u],a); VectorSubtract(wp[hd],wp[e],b);
                        if(VectorNormalize(a)<0.01f||VectorNormalize(b)<0.01f) continue;
                        float c=DotProduct(a,b); if(c<-1)c=-1; if(c>1)c=1;
                        float v=(float)(acos(c)*180.0/M_PI);
                        if(v>elbowFold) elbowFold=v;
                    }
                }
                {
                    // Particle joints 7 and 11 are the upper arms; drawn bones
                    // 7 and 10 are the same two.
                    int ld=Stub_TagNumForName(NULL,"Bip01 L UpperArm");
                    int rdd=Stub_TagNumForName(NULL,"Bip01 R UpperArm");
                    if (ld>=0 && rdd>=0 && have[ld] && have[rdd]) {
                        vec3_t ps, bs;
                        for(int k=0;k<3;k++) ps[k]=pp[11*3+k]-pp[7*3+k];
                        VectorSubtract(wp[rdd], wp[ld], bs);
                        if (VectorNormalize(ps)>0.01f && VectorNormalize(bs)>0.01f) {
                            float c=DotProduct(ps,bs);
                            if(c<-1)c=-1; if(c>1)c=1;
                            float a=(float)(acos(c)*180.0/M_PI);
                            if (a>90.0f) a=180.0f-a;   // a line, not an arrow
                            if (frame>60 && a>chestErr) chestErr=a;
                            if (frame>300) chestErrLate=a;
                        }
                    }
                }
                for (unsigned q=0;q<sizeof(pj)/sizeof(pj[0]);q++) {
                    int a=pj[q][0], b=pj[q][1];
                    vec3_t d; for(int k=0;k<3;k++) d[k]=pp[a*3+k]-pp[b*3+k];
                    float now=VectorLength(d);
                    if (pRest[q] <= 0.0f) { pRest[q]=now; continue; }
                    float err=fabsf(now-pRest[q])/pRest[q];
                    if (frame>60 && err>particleCollapse) particleCollapse=err;
                }
            }
        }

        // Which way a foot is pointing relative to its own shin, compared with
        // the rest pose. A large change here is a foot that has rotated about
        // the ankle to somewhere a foot cannot go, sole upwards being the
        // obvious one.
        {
            static const struct { const char *c,*f,*t; } ank[] = {
                {"Bip01 L Calf","Bip01 L Foot","Bip01 L Toe0"},
                {"Bip01 R Calf","Bip01 R Foot","Bip01 R Toe0"},
            };
            for (unsigned q=0;q<2;q++) {
                int c=Stub_TagNumForName(NULL,ank[q].c),fo=Stub_TagNumForName(NULL,ank[q].f),t=Stub_TagNumForName(NULL,ank[q].t);
                if(!have[c]||!have[fo]||!have[t]) continue;
                vec3_t sh,ft,rsh,rft;
                VectorSubtract(wp[fo],wp[c],sh);   VectorSubtract(wp[t],wp[fo],ft);
                VectorSubtract(g_bones[fo].pos,g_bones[c].pos,rsh);
                VectorSubtract(g_bones[t].pos,g_bones[fo].pos,rft);
                if(VectorNormalize(sh)<0.01f||VectorNormalize(ft)<0.01f) continue;
                VectorNormalize(rsh); VectorNormalize(rft);
                float now=DotProduct(sh,ft), rest=DotProduct(rsh,rft);
                if(now>1)now=1; if(now<-1)now=-1; if(rest>1)rest=1; if(rest<-1)rest=-1;
                float dev=fabsf(acosf(now)-acosf(rest))*180.0f/3.14159265f;
                if (frame>60 && dev>footFlip) footFlip=dev;
            }
        }

        // Has the drawn root come adrift from the pelvis?
        //
        // "Bip01" and "Bip01 Pelvis" are two rows of the solver's bone table
        // sharing one joint, and in the rig they stand a quarter turn apart:
        // the root runs along the body's facing, the pelvis runs up the spine.
        // Hand both of them the same orientation and the angle between them
        // collapses to nothing. Measured against the angle the animation
        // actually has, so a rig where the two agree reads zero rather than
        // reading the rig itself as a fault.
        {
            int r = Stub_TagNumForName(NULL, "Bip01");
            int q = Stub_TagNumForName(NULL, "Bip01 Pelvis");

            if (r >= 0 && q >= 0 && have[r] && have[q]) {
                refEntity_t am; memset(&am, 0, sizeof(am));
                am.tiki = &g_tiki; am.scale = sc.scale;
                orientation_t ar = Stub_TIKI_Orientation(&am, r);
                orientation_t aq = Stub_TIKI_Orientation(&am, q);
                vec3_t arx, aqx; VectorClear(arx); VectorClear(aqx);
                for (int k = 0; k < 3; k++) {
                    VectorMA(arx, ar.axis[0][k], model.axis[k], arx);
                    VectorMA(aqx, aq.axis[0][k], model.axis[k], aqx);
                }
                VectorNormalize(arx); VectorNormalize(aqx);

                float dev = fabsf(AngleBetween(wx[r], wx[q]) - AngleBetween(arx, aqx));
                if (frame > 60 && dev > rootSplit) rootSplit = dev;
            }
        }

        // Roll accumulated from the pelvis to the chest, end to end.
        //
        // This is the twist a player standing over the body sees: the legs
        // facing one way and the chest another. It is deliberately not a per
        // joint number. worstJointTwist reports the worst single step, so a
        // ninety degree divergence shared out as four twenty three degree steps
        // passes its gate comfortably while looking exactly as wrong. Nothing
        // that reads particle positions can see any of it either, because the
        // particles carry no twist at all.
        {
            int a = Stub_TagNumForName(NULL, "Bip01 Pelvis");
            int b = Stub_TagNumForName(NULL, "Bip01 Spine2");

            if (a >= 0 && b >= 0 && have[a] && have[b]) {
                float deg = RollBetween(wx[a], wy[a], wx[b], wy[b]);

                // Baselined on the pose the ragdoll was handed rather than
                // taken absolutely, so what is reported is roll the
                // reconstruction added and not the twist the body died with. A
                // man shot mid turn is already twisted and that is not a fault.
                if (pelvisTwistRest < -900.0f) {
                    pelvisTwistRest = deg;
                } else {
                    float dev = fabsf(deg - pelvisTwistRest);
                    while (dev > 180.0f) dev = fabsf(dev - 360.0f);
                    if (frame > 60 && dev > pelvisTwist) pelvisTwist = dev;
                }
            }
        }

        // How far each bone is rolled about its own length relative to the one
        // above it, compared with the rest pose. This is the twist you see as a
        // shin whose boot faces the wrong way while the leg itself looks
        // reasonable: the joint positions can be perfectly sensible and the
        // bone still be spun about its own axis, so nothing that measures
        // particle positions can see it at all.
        // How far each drawn foot is off the line of its shin, once the body
        // has had time to land: near zero is a foot folded straight out.
        if (frame > 150) {
            float ap[23*3];
            if (CG_RagdollDebugParticles(entnum, ap, 23) >= 23) {
                for (int q=0;q<2;q++) {
                    const int c = q ? 20 : 16, f = q ? 21 : 17, t = q ? 22 : 18;
                    vec3_t sh, ft; VectorSubtract(&ap[f*3],&ap[c*3],sh); VectorSubtract(&ap[t*3],&ap[f*3],ft);
                    if (VectorNormalize(sh)<0.01f||VectorNormalize(ft)<0.01f) continue;
                    float dev=acosf(fmaxf(-1,fminf(1,DotProduct(sh,ft))))*180.0f/3.14159265f;
                    ankN++; if (dev<20) ankStraight++; if (dev<ankMin) ankMin=dev;
                }
            }
        }
        {
            int l=Stub_TagNumForName(NULL,"Bip01 L UpperArm"), r=Stub_TagNumForName(NULL,"Bip01 R UpperArm");
            if (l>=0&&r>=0&&have[l]&&have[r]) {
                float w = Distance(wp[l], wp[r]);
                if (shRest < 0) shRest = w;
                else if (frame > 60) { float q = w/shRest; shSum += q; shN++; if (q < shMin) shMin = q; }
            }
        }
        if (sc.ledge == 2 && frame > 60) {
            float wp2[23*3];
            if (CG_RagdollDebugParticles(entnum, wp2, 23) >= 23) {
                float cy = 0; for (int q = 0; q < 23; q++) cy += wp2[q*3+1] / 23.0f;
                if (wallHave) { const float away = (wallLastY - cy) / 0.016f; if (away > wallAwayMax) wallAwayMax = away; }
                wallLastY = cy; wallHave = 1;
            }
        }
        {
            float lp[23*3];
            if (CG_RagdollDebugParticles(entnum, lp, 23) >= 23) {
                float c[3] = {0,0,0}, lowz = 1e9f; for (int q = 0; q < 23; q++) { for (int k = 0; k < 3; k++) c[k] += lp[q*3+k] / 23.0f; if (lp[q*3+2] < lowz) lowz = lp[q*3+2]; }
                if (!landed && frame > 20 && lowz < 4.0f) { VectorCopy(c, landAt); landed = 1; }
                if (landed) { const float d = sqrtf((c[0]-landAt[0])*(c[0]-landAt[0]) + (c[1]-landAt[1])*(c[1]-landAt[1])); if (d > landSlide) landSlide = d; }
            }
        }
        {
            float ip[23*3];
            if (CG_RagdollDebugParticles(entnum, ip, 23) >= 23) {
                float c[3] = {0,0,0}; for (int q = 0; q < 23; q++) for (int k = 0; k < 3; k++) c[k] += ip[q*3+k] / 23.0f;
                if (imHave) {
                    float v[3]; for (int k = 0; k < 3; k++) v[k] = (c[k] - imPrev[k]) / 0.016f;
                    // The hardest stop of the run, taken afresh each time a harder one comes.
                    if (imHave > 1 && frame > 20 && imVel[2] < -150.0f && v[2] - imVel[2] > 150.0f && v[2] - imVel[2] > imDv) { imDv = v[2] - imVel[2]; imFrame = frame; imIn = sqrtf(imVel[0]*imVel[0] + imVel[1]*imVel[1]); imGain = 0; imUp = 0; }
                    if (imFrame >= 0 && frame > imFrame && frame <= imFrame + 30) {
                        const float h = sqrtf(v[0]*v[0] + v[1]*v[1]);
                        if (h - imIn > imGain) imGain = h - imIn;
                        if (v[2] > imUp) imUp = v[2];
                    }
                    if (frame > 20) { const float h = sqrtf(v[0]*v[0] + v[1]*v[1]); if (h > imPeakH) imPeakH = h; }
                    for (int k = 0; k < 3; k++) imVel[k] = v[k];
                }
                for (int k = 0; k < 3; k++) imPrev[k] = c[k];
                imHave++;
            }
        }
        if (frame >= 20) {
            float sp[23*3];
            if (CG_RagdollDebugParticles(entnum, sp, 23) >= 23) {
                float w = 1.0f;
                for (int q = 0; q < 13; q++) {
                    const float d = Distance(&sp[stPair[q][0]*3], &sp[stPair[q][1]*3]);
                    if (!stHave) stRest[q] = d; else if (stRest[q] > 1.0f && d / stRest[q] > w) w = d / stRest[q];
                }
                if (stHave && w > stWorst) stWorst = w;
                if (stHave && getenv("RD_STDBG") && w > 1.3f) { for (int q = 0; q < 13; q++) { const float d = Distance(&sp[stPair[q][0]*3], &sp[stPair[q][1]*3]); if (d / stRest[q] > 1.3f) printf("STRETCH t=%d pair %d-%d %.2f\n", cg.time, stPair[q][0], stPair[q][1], d / stRest[q]); } }
                if (stHave && w > 1.3f) stFrames++;
                stHave = 1;
            }
        }
        if (frame > 20) {
            float wpp[23*3];
            if (CG_RagdollDebugParticles(entnum, wpp, 23) >= 23) {
                if (whHave) {
                    float m[3] = {0,0,0}; for (int q = 0; q < 23; q++) for (int k = 0; k < 3; k++) m[k] += (wpp[q*3+k] - whLast[q*3+k]) / 23.0f;
                    whSteps++;
                    for (int q = 0; q < 23; q++) { vec3_t r; for (int k = 0; k < 3; k++) r[k] = wpp[q*3+k] - whLast[q*3+k] - m[k]; const float l = VectorLength(r); if (l > whWorst) whWorst = l; if (l > 4.0f) whips++; }
                }
                memcpy(whLast, wpp, sizeof(whLast)); whHave = 1;
            }
        }
        // Where each drawn clavicle ends against where its upper arm is drawn:
        // the helper bones that shape the shoulder hang off the clavicle, so a
        // gap here is the arm drawn broken at the shoulder.
        {
            static const char *cn[2][2] = {{"Bip01 L Clavicle","Bip01 L UpperArm"},{"Bip01 R Clavicle","Bip01 R UpperArm"}};
            for (int q=0;q<2;q++) {
                int c=Stub_TagNumForName(NULL,cn[q][0]), u=Stub_TagNumForName(NULL,cn[q][1]);
                if (c<0||u<0||!have[c]||!have[u]) continue;
                clavSeen=1;
                vec3_t d, wz; VectorSubtract(wp[u],wp[c],d); CrossProduct(wx[c],wy[c],wz);
                if (!clavHave[q]) { clavOff[q][0]=DotProduct(d,wx[c]); clavOff[q][1]=DotProduct(d,wy[c]); clavOff[q][2]=DotProduct(d,wz); clavHave[q]=1; continue; }
                vec3_t end; VectorCopy(wp[c],end); VectorMA(end,clavOff[q][0],wx[c],end); VectorMA(end,clavOff[q][1],wy[c],end); VectorMA(end,clavOff[q][2],wz,end);
                VectorSubtract(wp[u],end,d);
                float g=VectorLength(d);
                clavGapSum+=g; clavGapN++; if(g>clavGapMax)clavGapMax=g;
                if (getenv("CLVDBG") && q==1 && frame%20==0) fprintf(stderr,"CLVGAP f=%d gap %.2f dist %.2f\n",frame,g,Distance(wp[u],wp[c]));
            }
        }
        // How the drawn upper arms and thighs roll about their own length:
        // against the limb's swing from hanging straight down in the drawn
        // torso's frame (wander), and against the plane the elbow or knee is
        // drawn bending in (off hinge). Both as a change from when the ragdoll
        // took over, since the bones' own axes sit at arbitrary angles.
        {
            static const struct { const char *s,*u,*l; int leg; } lm[] = {
                {"Bip01 L UpperArm","Bip01 L Forearm","Bip01 L Hand",0}, {"Bip01 R UpperArm","Bip01 R Forearm","Bip01 R Hand",0},
                {"Bip01 L Thigh","Bip01 L Calf","Bip01 L Foot",1}, {"Bip01 R Thigh","Bip01 R Calf","Bip01 R Foot",1},
            };
            int ip=Stub_TagNumForName(NULL,"Bip01 Pelvis"), is1=Stub_TagNumForName(NULL,"Bip01 Spine1");
            int ila=Stub_TagNumForName(NULL,"Bip01 L UpperArm"), ira=Stub_TagNumForName(NULL,"Bip01 R UpperArm");
            if (have[ip]&&have[is1]&&have[ila]&&have[ira]) {
                vec3_t up, rt, fw, down;
                VectorSubtract(wp[is1],wp[ip],up); VectorNormalize(up);
                VectorSubtract(wp[ira],wp[ila],rt); VectorMA(rt,-DotProduct(rt,up),up,rt); VectorNormalize(rt);
                CrossProduct(up,rt,fw); VectorNegate(up,down);
                for (int q=0;q<4;q++) {
                    int a=Stub_TagNumForName(NULL,lm[q].s), b=Stub_TagNumForName(NULL,lm[q].u), c=Stub_TagNumForName(NULL,lm[q].l);
                    if(a<0||b<0||c<0||!have[a]||!have[b]||!have[c]) continue;
                    vec3_t dir, low, neu, ref, ax, pa, pb, cr;
                    VectorSubtract(wp[b],wp[a],dir); VectorSubtract(wp[c],wp[b],low);
                    if (VectorNormalize(dir)<0.01f||VectorNormalize(low)<0.01f) continue;
                    VectorScale(fw, lm[q].leg ? -1.0f : 1.0f, neu);
                    CrossProduct(down,dir,ax);
                    float sn=VectorNormalize(ax), cs=DotProduct(down,dir);
                    if (sn<0.001f) VectorCopy(neu,ref); else RotatePointAroundVector(ref,ax,neu,atan2f(sn,cs)*180.0f/3.14159265f);
                    float swing=acosf(fmaxf(-1,fminf(1,cs)))*180.0f/3.14159265f;
                    // signed angle from ref to the drawn Y about dir
                    VectorMA(ref,-DotProduct(ref,dir),dir,pa); VectorMA(wy[a],-DotProduct(wy[a],dir),dir,pb);
                    CrossProduct(pa,pb,cr);
                    float tw=atan2f(DotProduct(cr,dir),DotProduct(pa,pb))*180.0f/3.14159265f;
                    VectorMA(low,-DotProduct(low,dir),dir,pa);
                    CrossProduct(pb,pa,cr);
                    float off=atan2f(DotProduct(cr,dir),DotProduct(pb,pa))*180.0f/3.14159265f;
                    float bend=acosf(fmaxf(-1,fminf(1,DotProduct(dir,low))))*180.0f/3.14159265f;
                    float offAbs;
                    { vec3_t wz, lowD; CrossProduct(wx[a], wy[a], wz); VectorNormalize(wz); VectorSubtract(wp[c],wp[b],lowD); VectorNormalize(lowD);
                      offAbs = asinf(fmaxf(-1,fminf(1,DotProduct(lowD,wz))))*180.0f/3.14159265f; }
                    if (getenv("ABSDBG") && frame%8==0) fprintf(stderr,"ABSDBG f=%d q=%d bend=%.0f offAbs=%.1f\n",frame,q,bend,offAbs);
                    if (getenv("LMDBG") && q==2 && frame>=40 && frame<=75) fprintf(stderr,"LMDBG f=%d tw=%.0f off=%.0f bend=%.0f swing=%.0f\n",frame,tw,off,bend,swing);
                    if (frame<gBlendFrames) continue;
                    if (limbTwRest[q]>900) { limbTwRest[q]=tw; limbOffRest[q]=off; continue; }
                    if (swing<140) {
                        float d=fabsf(AngleNormalize180(tw-limbTwRest[q]));
                        int g=lm[q].leg; limbWanderSum[g]+=d; limbWanderN[g]++; if(d>limbWanderMax)limbWanderMax=d; if(d>90)limbWanderBig[g]++;
                    }
                    if (bend>20) {
                        float d=fabsf(offAbs);
                        int g=lm[q].leg; limbOffSum[g]+=d; limbOffW[g]+=1; if(d>limbOffMax)limbOffMax=d;
                    }
                }
            }
        }
        {
            static const struct { const char *a,*b; } pr[] = {
                {"Bip01 L Thigh","Bip01 L Calf"}, {"Bip01 R Thigh","Bip01 R Calf"},
                {"Bip01 L Calf","Bip01 L Foot"},  {"Bip01 R Calf","Bip01 R Foot"},
                {"Bip01 L UpperArm","Bip01 L Forearm"}, {"Bip01 R UpperArm","Bip01 R Forearm"},
            };
            for (unsigned q=0;q<sizeof(pr)/sizeof(pr[0]);q++) {
                int a=Stub_TagNumForName(NULL,pr[q].a), b=Stub_TagNumForName(NULL,pr[q].b);
                if(a<0||b<0||!have[a]||!have[b]) continue;
                // The parent's Y carried onto the child's direction by the
                // shortest rotation between the two bone axes, then compared
                // with the child's own Y. Comparing the two Y axes directly
                // instead mixes in how far the joint itself is bent: a bent
                // ankle then reads as a rolled foot even when the foot is not
                // rolled at all, and the number tracks the ankle angle rather
                // than the twist it is supposed to be measuring.
                vec3_t ax; VectorCopy(wx[b],ax);
                vec3_t rot, ya, yb;
                CrossProduct(wx[a], ax, rot);
                float sn = VectorNormalize(rot);
                float cs = DotProduct(wx[a], ax);
                if (sn < 0.001f) VectorCopy(wy[a], ya);
                else RotatePointAroundVector(ya, rot, wy[a], (float)(atan2(sn,cs)*180.0/M_PI));
                VectorMA(ya,-DotProduct(ya,ax),ax,ya);
                VectorMA(wy[b],-DotProduct(wy[b],ax),ax,yb);
                if(VectorNormalize(ya)<0.01f||VectorNormalize(yb)<0.01f) continue;
                float d=DotProduct(ya,yb); if(d>1)d=1; if(d<-1)d=-1;
                float deg=acosf(d)*180.0f/3.14159265f;
                // Measured as a change from the relationship the two bones have
                // when the ragdoll takes over, not as an absolute angle: a foot
                // sits across its own shin to begin with, so the absolute angle
                // between them is large and says nothing.
                if (rollRest[q] < 0.0f) { rollRest[q] = deg; continue; }
                float dev = fabsf(deg - rollRest[q]);
                if (frame>150 && dev>boneRoll) { boneRoll=dev; rollWho=pr[q].b; }
            }
        }

        // How far any joint is bent past what a real one can manage, in
        // degrees. Normative values are the AAOS ones: knee flexion 135,
        // elbow 150, ankle 20 dorsiflexion and 50 plantarflexion, which is a
        // 70 degree span. A corpse is passive and passive range runs a little
        // beyond active, so a margin is allowed on top before it counts.
        {
            static const struct { const char *a,*b,*c; float lim; } rom[] = {
                {"Bip01 L Thigh","Bip01 L Calf","Bip01 L Foot", 135.0f},
                {"Bip01 R Thigh","Bip01 R Calf","Bip01 R Foot", 135.0f},
                {"Bip01 L UpperArm","Bip01 L Forearm","Bip01 L Hand", 150.0f},
                {"Bip01 R UpperArm","Bip01 R Forearm","Bip01 R Hand", 150.0f},
            };
            for (unsigned q=0;q<sizeof(rom)/sizeof(rom[0]);q++) {
                int a=Stub_TagNumForName(NULL,rom[q].a),b=Stub_TagNumForName(NULL,rom[q].b),c=Stub_TagNumForName(NULL,rom[q].c);
                if(a<0||b<0||c<0||!have[a]||!have[b]||!have[c]) continue;
                vec3_t u,v; VectorSubtract(wp[b],wp[a],u); VectorSubtract(wp[c],wp[b],v);
                if(VectorNormalize(u)<0.01f||VectorNormalize(v)<0.01f) continue;
                float d=DotProduct(u,v); if(d>1)d=1; if(d<-1)d=-1;
                float bend=acosf(d)*180.0f/3.14159265f;   // 0 = straight
                float over=bend-(rom[q].lim+15.0f);       // 15 deg of passive margin
                if (frame>30 && over>romOver) { romOver=over; romWho=rom[q].b; }
            }
        }

        // How far the knees are folded once the body has settled, in degrees
        // away from straight. A corpse lying on the ground has its legs more or
        // less out; knees drawn right up under it, as if kneeling, is a very
        // recognisable wrong look and nothing else here was measuring it.
        {
            static const struct { const char *a,*b,*c; } leg[] = {
                {"Bip01 L Thigh","Bip01 L Calf","Bip01 L Foot"},
                {"Bip01 R Thigh","Bip01 R Calf","Bip01 R Foot"},
            };
            for (unsigned q=0;q<2;q++) {
                int a=Stub_TagNumForName(NULL,leg[q].a),b=Stub_TagNumForName(NULL,leg[q].b),c=Stub_TagNumForName(NULL,leg[q].c);
                if(a<0||b<0||c<0||!have[a]||!have[b]||!have[c]) continue;
                vec3_t u,v; VectorSubtract(wp[b],wp[a],u); VectorSubtract(wp[c],wp[b],v);
                if(VectorNormalize(u)<0.01f||VectorNormalize(v)<0.01f) continue;
                float d=DotProduct(u,v); if(d>1)d=1; if(d<-1)d=-1;
                float deg=acosf(d)*180.0f/3.14159265f;
                if (frame>150 && deg>kneeFold) kneeFold=deg;
            }
        }

        // How far the corpse has spread itself out, against the same
        // distances the living body has. A limp body dropped on its back keeps
        // its limbs roughly where they fell; one with nothing holding its shape
        // slides them out flat and ends up spread-eagled, arms wide and legs
        // apart. That is a very recognisable wrong look and nothing else here
        // was measuring it, which is how a change that removed the shape
        // memory got through as an improvement.
        {
            static const struct { const char *a,*b; } pair[] = {
                {"Bip01 L Hand",  "Bip01 R Hand"},
                {"Bip01 L Foot",  "Bip01 R Foot"},
                {"Bip01 L Hand",  "Bip01 L Foot"},
                {"Bip01 R Hand",  "Bip01 R Foot"},
                {"Bip01 L Hand",  "Bip01 Spine1"},
                {"Bip01 R Hand",  "Bip01 Spine1"},
            };
            for (unsigned q=0;q<sizeof(pair)/sizeof(pair[0]);q++) {
                int a=Stub_TagNumForName(NULL,pair[q].a), b=Stub_TagNumForName(NULL,pair[q].b);
                if(a<0||b<0||!have[a]||!have[b]) continue;
                vec3_t d; VectorSubtract(wp[a],wp[b],d);
                vec3_t r; VectorSubtract(g_bones[a].pos,g_bones[b].pos,r);
                float rest = VectorLength(r)*sc.scale;
                if (rest < 0.01f) continue;
                float ratio = VectorLength(d)/rest;
                if (frame>150 && ratio>sprawl) sprawl=ratio;
            }
        }

        // One limb inside another.
        //
        // The pair test above works on joint positions and cannot see this: two
        // bones can cross at their middles with all four ends comfortably
        // apart, so a shin passes through the other shin and nothing reports it.
        {
            static const struct { const char *a,*b; float r; } ls[] = {
                {"Bip01 L UpperArm","Bip01 L Forearm",3.2f}, {"Bip01 L Forearm","Bip01 L Hand",2.8f},
                {"Bip01 R UpperArm","Bip01 R Forearm",3.2f}, {"Bip01 R Forearm","Bip01 R Hand",2.8f},
                {"Bip01 L Thigh","Bip01 L Calf",4.0f},       {"Bip01 L Calf","Bip01 L Foot",3.5f},
                {"Bip01 R Thigh","Bip01 R Calf",4.0f},       {"Bip01 R Calf","Bip01 R Foot",3.5f},
            };
            for (unsigned q=0;q<sizeof(ls)/sizeof(ls[0]);q++) {
                for (unsigned w=q+1;w<sizeof(ls)/sizeof(ls[0]);w++) {
                    int a1=Stub_TagNumForName(NULL,ls[q].a), b1=Stub_TagNumForName(NULL,ls[q].b);
                    int a2=Stub_TagNumForName(NULL,ls[w].a), b2=Stub_TagNumForName(NULL,ls[w].b);
                    if(a1<0||b1<0||a2<0||b2<0) continue;
                    if(!have[a1]||!have[b1]||!have[a2]||!have[b2]) continue;
                    // bones meeting at a joint touch by construction
                    if(a1==a2||a1==b2||b1==a2||b1==b2) continue;
                    float ta,tb; vec3_t dir;
                    float d=SegSegDist(wp[a1],wp[b1],wp[a2],wp[b2],&ta,&tb,dir);
                    float want=(ls[q].r*0.85f+ls[w].r*0.85f)*sc.scale;
                    if(want<0.01f) continue;
                    float pen=(want-d)/want;
                    if (frame>60 && pen>limbCross) { limbCross=pen; crossWho=ls[w].b; }
                }
            }
        }

        // Limbs held up by nothing. A settled limb is resting on something:
        // the ground, or the body. This asks how far each limb end is from
        // both at once, so a hand lying on the chest scores zero however high
        // off the ground it happens to be, and only a limb genuinely floating
        // in space is counted. Comparing heights instead cannot tell those two
        // apart, which is the trap this metric replaced.
        {
            static const struct { const char *n; float r; } tip[] = {
                {"Bip01 L Hand",2.8f}, {"Bip01 R Hand",2.8f},
                {"Bip01 L Foot",3.5f}, {"Bip01 R Foot",3.5f},
            };
            // Everything a limb could plausibly come to rest on, which has to
            // include the other limbs: a boot lying across the opposite shin is
            // resting on something, and counting only the trunk and the thighs
            // reports it as hanging in mid air.
            static const struct { const char *a,*b; float r; } tor[] = {
                {"Bip01 Pelvis","Bip01 Spine",  5.0f},
                {"Bip01 Spine", "Bip01 Spine1", 5.0f},
                {"Bip01 Spine1","Bip01 Spine2", 4.8f},
                {"Bip01 Spine2","Bip01 Neck",   3.0f},
                {"Bip01 L Thigh","Bip01 L Calf",4.0f},
                {"Bip01 R Thigh","Bip01 R Calf",4.0f},
                {"Bip01 L Calf","Bip01 L Foot", 3.5f},
                {"Bip01 R Calf","Bip01 R Foot", 3.5f},
                {"Bip01 L UpperArm","Bip01 L Forearm", 3.2f},
                {"Bip01 R UpperArm","Bip01 R Forearm", 3.2f},
                {"Bip01 L Forearm","Bip01 L Hand", 2.8f},
                {"Bip01 R Forearm","Bip01 R Hand", 2.8f},
            };
            for (unsigned q=0;q<4;q++) {
                int c=Stub_TagNumForName(NULL,tip[q].n);
                if(c<0||!have[c]) continue;
                float own = tip[q].r*sc.scale;
                // Gap to whatever is under it, asked of the same world the
                // solver collides against rather than of the floor plane alone.
                // Measuring against the plane only, as this did, counts a hand
                // resting on top of the ledge or against the wall as hanging in
                // mid air, because neither is the floor: every wall and ledge
                // scenario then reports limbs floating that are lying on
                // something solid.
                float gapGround;
                {
                    trace_t tr; vec3_t from, to, zero = {0,0,0};
                    VectorCopy(wp[c], from);
                    VectorCopy(wp[c], to); to[2] -= 512.0f;
                    CG_Trace(&tr, from, zero, zero, to, 1023, 1, qfalse, qtrue, "limbAir");
                    gapGround = (tr.fraction >= 1.0f ? 512.0f : tr.fraction * 512.0f) - own;
                    if (gapGround < 0.0f) gapGround = 0.0f;
                }
                // gap to the nearest part of the body it is not attached to
                float gapBody = 1e9f;
                for (unsigned t=0;t<sizeof(tor)/sizeof(tor[0]);t++) {
                    int a2=Stub_TagNumForName(NULL,tor[t].a), b2=Stub_TagNumForName(NULL,tor[t].b);
                    if(a2<0||b2<0||!have[a2]||!have[b2]) continue;
                    if(a2==c||b2==c) continue;
                    vec3_t ab,ac,cl; VectorSubtract(wp[b2],wp[a2],ab); VectorSubtract(wp[c],wp[a2],ac);
                    float l2=DotProduct(ab,ab); if(l2<0.01f) continue;
                    float tt=DotProduct(ac,ab)/l2; if(tt<0)tt=0; if(tt>1)tt=1;
                    VectorMA(wp[a2],tt,ab,cl);
                    vec3_t d; VectorSubtract(wp[c],cl,d);
                    float dist=VectorLength(d);
                    // Only something underneath it can hold a limb up. Beside
                    // is not under: a foot floating level with the pelvis is as
                    // unsupported as one out in the open, and counting the hip
                    // beside it as what it rests on is how a foot twenty one
                    // units in the air came to score as settled. The support
                    // has to lie below the limb, within sixty degrees of
                    // straight down, or it is not carrying any weight.
                    if(dist>0.01f && DotProduct(d,gFloorN)/dist < 0.5f) continue;
                    float g=dist-own-tor[t].r*sc.scale;
                    if(g<gapBody) gapBody=g;
                }
                float unsupported = gapGround < gapBody ? gapGround : gapBody;
                if (frame>150 && unsupported>limbAir) { limbAir=unsupported; airWho=tip[q].n; }
            }
        }

        // How far a bone turns about its own length from one frame to the
        // next. A bone's roll should change about as smoothly as the joint
        // positions do; a step in it means the reconstruction changed its mind
        // about what to measure the roll against, which no amount of physics
        // asked for and which the eye reads as the limb snapping.
        for (int b = 0; b < NUM_BONES; b++) if (have[b] && havePrev) {
            float d = fabsf(RollBetween(prevX[b], prevY[b], wx[b], wy[b]));
            if (frame > 60 && d > rollStep) { rollStep = d; stepWho = g_bones[b].name; stepAt = frame; }
            if (getenv("RD_STEPDBG") && frame > 60 && d > 30.0f) fprintf(stderr, "STEP f=%d %s %.0f\n", frame, g_bones[b].name, d);
            if (frame > 150 && d > rollStepLate) { rollStepLate = d; stepLateWho = g_bones[b].name; }
            if (getenv("RD_ROLLDBG") && frame > 150 && d > 60.0f) {
                const int par = g_bones[b].parent;
                float bendToParent = (par >= 0 && have[par]) ? AngleBetween(wx[par], wx[b]) : -1.0f;
                fprintf(stderr, "roll step %.0f deg on %s at frame %d: bone swung %.0f deg this frame, %.0f deg from its parent\n",
                        d, g_bones[b].name, frame, AngleBetween(prevX[b], wx[b]), bendToParent);
            }
        }
        for (int b = 0; b < NUM_BONES; b++) if (have[b]) {
            VectorCopy(wx[b], prevX[b]); VectorCopy(wy[b], prevY[b]);
        }

        // How far the body carries itself after the animation lets go. A corpse
        // on a slope or a staircase should keep going for a while; one that
        // stops dead where it landed reads as stuck.
        {
            int ip = Stub_TagNumForName(NULL, "Bip01 Pelvis");
            if (ip >= 0 && have[ip]) {
                if (!haveHandoff && frame > gBlendFrames) { VectorCopy(wp[ip], handoffAt); haveHandoff = 1; }
                VectorCopy(wp[ip], restedAt);
            }
        }

        float mv = 0;
        for (int b = 0; b < NUM_BONES; b++) if (have[b]) {
            if (havePrev) { float d=0; for(int k=0;k<3;k++){float e=wp[b][k]-prev[b][k]; d+=e*e;} d=sqrtf(d); if(d>mv) mv=d; }
            VectorCopy(wp[b], prev[b]);
            float h = DotProduct(wp[b], gFloorN);
            if (frame > 40 && h < deepest) deepest = h;
        }
        havePrev = 1;
        if (frame > 150 && mv > lateMove) lateMove = mv;
        if (settleFrame < 0 && frame > 30 && mv < 0.1f) settleFrame = frame;

        float mn[3]={1e9f,1e9f,1e9f}, mx[3]={-1e9f,-1e9f,-1e9f};
        for (int b=0;b<NUM_BONES;b++) if (have[b]) for(int k=0;k<3;k++){ if(wp[b][k]<mn[k])mn[k]=wp[b][k]; if(wp[b][k]>mx[k])mx[k]=wp[b][k]; }
        for (int k=0;k<3;k++) lastExtent[k]=mx[k]-mn[k];
        { int a1=Stub_TagNumForName(NULL,"Bip01 Pelvis"), b1=Stub_TagNumForName(NULL,"Bip01 Head");
          vec3_t d; VectorSubtract(wp[a1],wp[b1],d); lastSeg[0]=VectorLength(d); }
        { int a1=Stub_TagNumForName(NULL,"Bip01 L Thigh"), b1=Stub_TagNumForName(NULL,"Bip01 L Foot");
          vec3_t d; VectorSubtract(wp[a1],wp[b1],d); lastSeg[1]=VectorLength(d); }
    }

#ifdef RD_JOLT
    // RD_JOLTREPORT prints what Jolt has at each joint as the run ends.
    if (getenv("RD_JOLTREPORT")) CG_RagdollDebugJolt(entnum, RDJ_Print);
#endif
    int ok = !bad && lateStretch < 0.02f && lateMove < 0.5f && deepest > -1.5f
             && worstPen < 0.34f && backBends == 0 && worstHyper < 0.5f && jointDev[0] < 70.0f && jointDev[1] < 60.0f && jointDev[2] < 60.0f && worstTwist < 50.0f && worstJointTwist < 30.0f && worstJointBend < 35.0f && kneeLateral < 5.0f && particleCollapse < 0.10f && footFlip < 55.0f;
    printf("%-26s | %5.1f%% %5.1f%% | %6.2f | %5.2f | %5d | %6.3f | %6.2f | %s\n",
           sc.name, worstStretch*100, lateStretch*100, worstPen*100, worstHyper, backBends, lateMove, deepest,
           ok ? "PASS" : "FAIL");
    {
        int ih=Stub_TagNumForName(NULL,"Bip01 Head"), ilf=Stub_TagNumForName(NULL,"Bip01 L Foot");
        int ip=Stub_TagNumForName(NULL,"Bip01 Pelvis");
        vec3_t d; VectorSubtract(prev[ih], prev[ilf], d);
        float headToFoot = VectorLength(d);
        VectorSubtract(prev[ih], prev[ip], d);
        float pelvisHead = VectorLength(d);
        // A body lying out straight has head and foot about 60 units apart at
        // scale 1; folded in half it is far less.
        printf("%28s   head-to-foot %.1f (upright %.1f)  extent %.0f x %.0f x %.0f\n", "",
               headToFoot, 60.0f*sc.scale, lastExtent[0], lastExtent[1], lastExtent[2]);
    }
    printf("%28s   elbow above shoulder-hand line: %+.1f units\n", "", elbowRise);
    printf("%28s   foot turned from rest: %.0f deg\n", "", footFlip);
    printf("%28s   particle cloud distortion: %.0f%%\n", "", particleCollapse*100);
    if (pileZ > 0.0f) {
        printf("%28s   two bodies: worst joint jump in one frame %.1f units\n", "", gPileJump);
        printf("%28s   two bodies: closest approach %.1f units, furthest particle from origin %.1f\n",
               "", pileGap, pileWorst);
    }
    if (tookHit) {
        printf("%28s   pelvis was at %.0f %.0f %.0f when hit, then moved %.1f units\n", "", beforeHit[0], beforeHit[1], beforeHit[2], movedAfter);
    }
    printf("%28s   jitter once settled: %.4f units a frame\n", "", jitN ? jitSum/jitN : 0.0f);
    printf("%28s   settled at frame %d\n", "", settleFrame);
    printf("%28s   slid after landing: %.1f units\n", "", landSlide);
    printf("%28s   whips: %d joint steps over 4 units off the body, worst %.1f\n", "", whips, whWorst);
    printf("%28s   spring: level speed gained %.0f u/s, upward %.0f u/s in the half second after the hardest impact (frame %d), peak level speed %.0f\n", "", imGain, imUp, imFrame, imPeakH);
    printf("%28s   stretch: worst bone %.2fx its length, %d frames over 1.3x\n", "", stWorst, stFrames);
    { float fp[23*3]; if (CG_RagdollDebugParticles(entnum, fp, 23) >= 23) { float cz = 0; for (int q = 0; q < 23; q++) cz += fp[q*3+2] / 23.0f; printf("%28s   ends with body centre at height %.1f\n", "", cz); } }
    // How far the body as it really is, each joint its own thickness from the
    // solver's table, ends up sunk into the floor: the trunk and the limbs.
    {
        static const float jr[23] = {5.5f,5.0f,5.0f,4.8f,3.0f,4.5f,4.0f, 4.0f,3.2f,2.8f,2.2f, 4.0f,3.2f,2.8f,2.2f, 5.5f,4.0f,3.5f,3.0f, 5.5f,4.0f,3.5f,3.0f};
        float fp[23*3];
        if (CG_RagdollDebugParticles(entnum, fp, 23) >= 23) {
            float ts = 0, tm = 0, ls = 0, lm = 0; int tn = 0, ln = 0;
            for (int q = 0; q < 23; q++) {
                const float h = DotProduct(&fp[q*3], gFloorN), r = jr[q] * sc.scale;
                if (h > r * 1.5f) continue;
                const float sink = Q_max(0.0f, r - h);
                if (q <= 4) { ts += sink; tn++; if (sink > tm) tm = sink; } else { ls += sink; ln++; if (sink > lm) lm = sink; }
            }
            printf("%28s   sunk into the floor: trunk mean %.2f max %.2f, limbs mean %.2f max %.2f\n", "", tn ? ts / tn : 0.0f, tm, ln ? ls / ln : 0.0f, lm);
        }
    }
    if (hangT >= 0) { printf("%28s   held flips (joint reversing >2 units a step): %d\n", "", gHangFlips); gHangFlips = 0; }
    if (drT >= 0) printf("%28s   drag: leg jumps over 3 units %d, worst %.1f, leg jerk %.3f\n", "", drJumps, drWorst, drN ? drJerk / drN : 0.0f);
    if (wdT >= 0) printf("%28s   wall drag: after release peak body speed %.0f, away from wall %.0f u/s, pelvis moved %.1f from the wall\n", "", wdPeak, wdAwayMax, wdAway);
    if (puntT >= 0) printf("%28s   punt: pelvis slid %.0f units, still after %d ms\n", "", puntTravel, puntStop);
    printf("%28s   elbow folded to: %.0f deg (limit 150)\n", "", elbowFold);
    printf("%28s   drawn chest vs simulated chest: %.0f deg worst, %.0f deg at rest\n", "", chestErr, chestErrLate);
    printf("%28s   limb inside trunk: %.0f%% of its thickness\n", "", armInTorso*100);
    printf("%28s   limb inside another limb: %.0f%% (%s)\n", "", limbCross*100, crossWho);
    printf("%28s   seed reproduction error: %.1f deg (%s)\n", "", seedErr, seedWho);
    printf("%28s   bone rolled about itself: %.0f deg (%s)\n", "", boneRoll, rollWho);
    printf("%28s   ankle off the shin: %.0f deg at least, %.1f%% of frames under 20\n", "", ankN?ankMin:0.0f, ankN?100.0f*ankStraight/ankN:0.0f);
    printf("%28s   drawn shoulder width vs at death: %.2f mean, %.2f narrowest\n", "", shN?shSum/shN:0.0f, shN?shMin:0.0f);
    if (sc.ledge == 2) printf("%28s   wall: peak speed away %.0f u/s, body centre ends %.1f from the wall\n", "", wallAwayMax, 10.0f - wallLastY);
    printf("%28s   clavicle end to upper arm: %s %.2f units mean, %.2f max\n", "", clavSeen ? "" : "(no clavicle drawn)", clavGapN?clavGapSum/clavGapN:0.0f, clavGapMax);
    for (int g=0; g<2; g++)
        printf("%28s   drawn %s roll: wander %.1f deg mean, %.1f%% past 90; off hinge %.1f deg mean\n", "", g ? "leg" : "arm",
               limbWanderN[g]?limbWanderSum[g]/limbWanderN[g]:0.0f, limbWanderN[g]?100.0f*limbWanderBig[g]/limbWanderN[g]:0.0f, limbOffW[g]?limbOffSum[g]/limbOffW[g]:0.0f);
    printf("%28s   joint past its range: %.0f deg (%s)\n", "", romOver, romWho);
    printf("%28s   knees folded at rest: %.0f deg\n", "", kneeFold);
    {
        // Which way each bent knee faces from forward, and each bent elbow
        // from straight back, measured from the limb's swing the way
        // CG_RagdollKneeTurn and CG_RagdollElbows measure it. Out is positive
        // for a knee. A straight joint has no direction and reads "-".
        float pp[23*3];
        if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
            char kneeTxt[2][16], elbowTxt[2][16];
            vec3_t up, right, fwd, down, back;
            VectorSubtract(&pp[2*3], &pp[0], up); VectorNormalize(up);
            VectorNegate(up, down);
            for (int side = 0; side < 2; side++) {
                // knee
                {
                    const int a = side ? 19 : 15, m = side ? 20 : 16, b = side ? 21 : 17;
                    VectorSubtract(&pp[19*3], &pp[15*3], right);
                    VectorMA(right, -DotProduct(right, up), up, right); VectorNormalize(right);
                    CrossProduct(up, right, fwd);
                    vec3_t thigh, chord, rel, off, ref, axis, outv, c1, c2, pr, po, pa;
                    VectorSubtract(&pp[m*3], &pp[a*3], thigh); VectorNormalize(thigh);
                    VectorSubtract(&pp[b*3], &pp[a*3], chord); VectorNormalize(chord);
                    VectorSubtract(&pp[m*3], &pp[a*3], rel);
                    VectorMA(rel, -DotProduct(rel, chord), chord, off);
                    float bent = VectorNormalize(off);
                    float cc = DotProduct(down, thigh); if (cc > 1) cc = 1; if (cc < -1) cc = -1;
                    CrossProduct(down, thigh, axis);
                    if (VectorNormalize(axis) < 1e-4f) VectorCopy(fwd, ref);
                    else RotatePointAroundVector(ref, axis, fwd, RAD2DEG(acos(cc)));
                    VectorScale(right, side ? 1.0f : -1.0f, outv);
                    VectorMA(ref, -DotProduct(ref, chord), chord, pr);
                    VectorMA(off, -DotProduct(off, chord), chord, po);
                    VectorMA(outv, -DotProduct(outv, chord), chord, pa);
                    CrossProduct(pr, po, c1); CrossProduct(pr, pa, c2);
                    float turn = RAD2DEG(atan2(DotProduct(c1, chord), DotProduct(pr, po)));
                    if (DotProduct(c2, chord) < 0) turn = -turn;
                    if (bent < 1.0f) snprintf(kneeTxt[side], 16, "-"); else snprintf(kneeTxt[side], 16, "%.0f", turn);
                }
                // elbow, in the chest's frame like the solver
                {
                    const int a = side ? 11 : 7, m = side ? 12 : 8, b = side ? 13 : 9;
                    vec3_t tx, ty, tz, tdown;
                    VectorSubtract(&pp[2*3], &pp[0], tx); VectorNormalize(tx);
                    VectorSubtract(&pp[11*3], &pp[7*3], ty);
                    VectorMA(ty, -DotProduct(ty, tx), tx, ty); VectorNormalize(ty);
                    CrossProduct(tx, ty, tz);
                    VectorNegate(tx, tdown); VectorNegate(tz, back);
                    vec3_t upper, chord, rel, off, ref, axis, pr, po, c1;
                    VectorSubtract(&pp[m*3], &pp[a*3], upper); VectorNormalize(upper);
                    VectorSubtract(&pp[b*3], &pp[a*3], chord); VectorNormalize(chord);
                    VectorSubtract(&pp[m*3], &pp[a*3], rel);
                    VectorMA(rel, -DotProduct(rel, chord), chord, off);
                    float bent = VectorNormalize(off);
                    float cc = DotProduct(tdown, upper); if (cc > 1) cc = 1; if (cc < -1) cc = -1;
                    CrossProduct(tdown, upper, axis);
                    if (VectorNormalize(axis) < 1e-4f) VectorCopy(back, ref);
                    else RotatePointAroundVector(ref, axis, back, RAD2DEG(acos(cc)));
                    VectorMA(ref, -DotProduct(ref, chord), chord, pr);
                    VectorMA(off, -DotProduct(off, chord), chord, po);
                    CrossProduct(pr, po, c1);
                    float turn = RAD2DEG(atan2(DotProduct(c1, chord), DotProduct(pr, po)));
                    float swing = RAD2DEG(acos(cc));
                    if (bent < 1.0f || swing > 150.0f) snprintf(elbowTxt[side], 16, "-"); else snprintf(elbowTxt[side], 16, "%.0f", fabs(turn));
                }
            }
            printf("%28s   knee faces from forward: L %s R %s deg (out +); elbow from straight back: L %s R %s deg\n",
                   "", kneeTxt[0], kneeTxt[1], elbowTxt[0], elbowTxt[1]);
        }
    }
    if (hanging && hangSpinN) {
        printf("%28s   while held: spinning %.0f deg a second, an arm %.0f%% into the trunk at worst\n",
               "", hangSpin / hangSpinN, hangArmIn * 100.0f);
    }
    if (hanging && hangJerkN) {
        printf("%28s   spasm while held: %.3f units a frame squared on average, worst frame %.3f\n",
               "", hangJerkSum / hangJerkN, hangJerkMax);
    }
    if (hanging) {
        // The pose it hangs in, from the particles: how far each thigh and upper
        // arm is from hanging straight down, how bent each knee and elbow is,
        // and how far each foot is below the pelvis.
        float pp[23*3];
        if (CG_RagdollDebugParticles(entnum, pp, 23) >= 23) {
            const vec3_t down = {0, 0, -1};
            const int limbs[4][3] = {{15,16,17},{19,20,21},{7,8,9},{11,12,13}};
            float fromDown[4], bend[4];
            for (int l = 0; l < 4; l++) {
                vec3_t a, b;
                VectorSubtract(&pp[limbs[l][1]*3], &pp[limbs[l][0]*3], a);
                VectorSubtract(&pp[limbs[l][2]*3], &pp[limbs[l][1]*3], b);
                VectorNormalize(a); VectorNormalize(b);
                fromDown[l] = AngleBetween(a, down);
                bend[l]     = AngleBetween(a, b);
            }
            printf("%28s   hanging by the head: thigh from straight down L %.0f R %.0f deg, knee bent L %.0f R %.0f deg\n",
                   "", fromDown[0], fromDown[1], bend[0], bend[1]);
            printf("%28s   hanging by the head: upper arm from straight down L %.0f R %.0f deg, elbow bent L %.0f R %.0f deg\n",
                   "", fromDown[2], fromDown[3], bend[2], bend[3]);
            printf("%28s   hanging by the head: feet below the pelvis L %.1f R %.1f units, head %.1f above the pelvis\n",
                   "", pp[0*3+2] - pp[17*3+2], pp[0*3+2] - pp[21*3+2], pp[5*3+2] - pp[0*3+2]);
        }
    }
    printf("%28s   spread vs living body: %.2fx\n", "", sprawl);
    printf("%28s   limb held up by nothing: %+.1f units (%s)\n", "", limbAir, airWho);
    if (haveHandoff) {
        vec3_t td; VectorSubtract(restedAt, handoffAt, td); td[2] = 0;
        travelled = VectorLength(td);
    }
    printf("%28s   carried itself %.1f units after the animation let go\n", "", travelled);
    printf("%28s   knee out of plane: %.1f units\n", "", kneeLateral);
    printf("%28s   spine bend per joint: pelvis %.0f  spine %.0f  spine1 %.0f  spine2 %.0f  neck %.0f (worst at %s)\n",
           "", bendPer[0], bendPer[1], bendPer[2], bendPer[3], bendPer[4], gBendAt);
    {
        float tp[23*3];
        if (CG_RagdollDebugParticles(entnum, tp, 23) >= 23) {
            vec3_t ax, hip, sho, side;
            VectorSubtract(&tp[3*3], &tp[0], ax); VectorNormalize(ax);
            VectorSubtract(&tp[19*3], &tp[15*3], hip); VectorSubtract(&tp[11*3], &tp[7*3], sho);
            VectorMA(hip, -DotProduct(hip, ax), ax, hip); VectorMA(sho, -DotProduct(sho, ax), ax, sho);
            VectorNormalize(hip); VectorNormalize(sho); CrossProduct(ax, hip, side);
            printf("%28s   final shoulders against hips: %.0f deg\n", "", atan2f(DotProduct(sho, side), DotProduct(sho, hip)) * 180.0f / 3.14159265f);
        }
    }
    printf("%28s   torso twist %.0f deg overall, %.0f deg at the worst single joint\n", "", worstTwist, worstJointTwist);
    printf("%28s   pelvis to chest roll added: %.0f deg;  root adrift from pelvis: %.0f deg\n", "", pelvisTwist, rootSplit);
    printf("%28s   worst roll step in one frame: %.1f deg (%s, frame %d); once settled %.1f deg (%s)\n", "", rollStep, stepWho, stepAt, rollStepLate, stepLateWho);
    printf("%28s   max bend from rest: wrist %.0f deg  neck %.0f deg  ankle %.0f deg\n", "",
           jointDev[0], jointDev[1], jointDev[2]);
    if (worstPen > 0.34f) printf("%28s   closest pair: %s / %s\n", "", gPenA, gPenB);
    return ok ? 0 : 1;
}

int main(void)
{
    memset(&cgi, 0, sizeof(cgi));
    cgi.Tag_NumForName   = Stub_TagNumForName;
    cgi.TIKI_Orientation = Stub_TIKI_Orientation;
    cgi.ForceUpdatePose  = Stub_ForceUpdatePose;
    cgi.R_DebugLine      = Stub_DebugLine;
    cgi.Cvar_Get         = Stub_CvarGet;
    cgi.Cvar_CheckRange  = Stub_CvarCheckRange;
    cgi.Cvar_Set         = Stub_CvarSet;
    cgi.Printf           = Stub_Printf;
    cgi.FS_FOpenFileWrite = Stub_FOpenFileWrite;
    cgi.FS_Write          = Stub_FSWrite;
    cgi.FS_FCloseFile     = Stub_FCloseFile;
    cgi.FS_ReadFile       = Stub_FSReadFile;
    cgi.FS_FreeFile       = Stub_FSFreeFile;
    cgi.Cmd_Execute       = Stub_CmdExecute;
    cgi.R_RegisterShader  = Stub_RegisterShader;
    cgi.R_AddPolyToScene  = Stub_AddPoly;
    cgi.Tag_NameForNum    = Stub_TagNameForNum;
    cgi.DPrintf           = Stub_DPrintf;

    memset(&cg, 0, sizeof(cg)); memset(&cgs, 0, sizeof(cgs)); memset(&g_snap, 0, sizeof(g_snap));
    g_snap.ps.gravity = 800; g_snap.ps.clientNum = 0;
    cg.snap = &g_snap; cg.nextSnap = NULL;
    cg_3rd_person = Stub_CvarGet("cg_3rd_person", "0", 0);
    g_anim.bIsCharacter = qtrue; g_anim.name = (char *)"models/human/german.tik";
    memset(&g_tiki, 0, sizeof(g_tiki)); g_tiki.a = &g_anim; g_tiki.load_scale = 1.0f;
#ifdef RD_JOLT
    // The Jolt build carries bodies with the Jolt ragdoll once the blend
    // ends; RD_SOLVER=0 runs the particles in the same binary, to compare.
    RDJ_Init(800.0f);
    Stub_CvarSet("cg_ragdoll_solver", getenv("RD_SOLVER") ? getenv("RD_SOLVER") : "1");
    printf("solver: %s\n", Stub_CvarGet("cg_ragdoll_solver", "1", 0)->integer == 1 ? "jolt" : "particles");
#endif

    printf("%-26s | %-12s | %-6s | %-5s | %-5s | %-6s | %-6s |\n",
           "scenario", "bone stretch", "selfX%", "hyper", "knee", "move", "deep");
    printf("%-26s | %-12s | %-6s | %-5s | %-5s | %-6s | %-6s |\n",
           "", "peak   late", "pen", "ext", "back", "", "");
    printf("---------------------------+--------------+--------+-------+-------+--------+--------+-----\n");

    Scenario scen[] = {
        {"flat, standing",        0,     0,   1.0f, {0,0,1},      0,   0, 0},
        {"draped over a ledge",   34,     0,   1.0f, {0,0,1},     20,   0, 0, 1},
        {"onto a ledge sideways", 34,    70,   1.0f, {0,0,1},      0,  40, 0, 1},
        {"ledge, pitched back",   40,   110,   1.0f, {0,0,1},    -25,  10, 0, 1},
        {"wrung out 60 deg",       4,     0,   1.0f, {0,0,1},      0,   0, 60},
        {"wrung out 90 deg",       4,    30,   1.0f, {0,0,1},     20,   0, 90},
        {"wrung out 120 deg",      4,    30,   1.0f, {0,0,1},     20,   0, 120},
        {"wrung out 160 deg",      4,    30,   1.0f, {0,0,1},      0,   0, 160},
        {"toppling forward",       6,     0,   1.0f, {0,0,1},     35,   0, 0},
        {"falling sideways",       6,    20,   1.0f, {0,0,1},      0,  45, 0},
        {"pitched back, moving",  10,    70,   1.0f, {0,0,1},    -30,  15, 0},
        {"flat, dropped from 64", 64,    0,   1.0f, {0,0,1}, 0, 0, 0},
        {"already on ground",     0,     0,   1.0f, {0,0,1}, 0, 0, 0},
        {"yaw 135 deg",           0,   135,   1.0f, {0,0,1}, 0, 0, 0},
        {"yaw 47, scale 1.35",    0,    47,   1.35f,{0,0,1}, 0, 0, 0},
        {"scale 0.6",             0,     0,   0.6f, {0,0,1}, 0, 0, 0},
        {"slope 20 deg",          8,     0,   1.0f, {0.34f,0,1}, 0, 0, 0},
        {"slope 35 deg, yaw 90",  8,    90,   1.0f, {0,0.70f,1}, 0, 0, 0},
        {"steep slope 45 deg",   16,    20,   1.0f, {0.9f,0.3f,1}, 0, 0, 0},

        // Seeded the way a death animation leaves a body: limbs already thrown
        // clear of the torso. These are the ones that ask whether a raised arm
        // or a drawn up knee comes back down again.
        //  name                    z  yaw  scale  floorN   pitch roll twist ledge armsUp knees
        {"arms up, standing",      2,    0,  1.0f, {0,0,1},   0,   0,   0,   0,   90,   0},
        {"arms up, toppling",      8,   40,  1.0f, {0,0,1},  30,   0,   0,   0,   70,   0},
        {"arms up, on back",       6,    0,  1.0f, {0,0,1}, -35,   0,   0,   0,  100,   0},
        {"knees drawn up",         4,    0,  1.0f, {0,0,1},  20,   0,   0,   0,    0,  75},
        {"arms up and knees up",   8,   20,  1.0f, {0,0,1}, -20,  10,   0,   0,   85,  60},

        // The death animation still swinging the arms inward as the ragdoll
        // takes over, so the arms arrive at the chest with momentum.
        //  name                    z  yaw  scale  floorN   pitch roll twist ledge armsUp knees clutch
        {"clutching, standing",    2,    0,  1.0f, {0,0,1},   0,   0,   0,   0,    0,   0,  120},
        {"clutching, toppling",    8,   30,  1.0f, {0,0,1},  30,   0,   0,   0,    0,   0,  70},
        {"clutching, on back",     6,    0,  1.0f, {0,0,1}, -30,   0,   0,   0,    0,   0,  60},

        // Driven by the real death animations shipped with the game, which is
        // the only thing here that puts a body into the poses it actually dies
        // in. anim is 1 based so that 0 keeps the synthetic pose.
        //  name                     z  yaw  scale  floorN   p   r  tw  ldg au  kn  cl  anim
        {"anim death_back1",         0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   1},
        {"anim death_chest",         0,  40, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   2},
        {"anim death_collapse",      0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   3},
        {"anim death_fall_back",     0, 110, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   4},
        {"anim death_left",          0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   5},
        {"anim death_right",         0,  70, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   6},
        {"anim death_choke",         0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   7},
        {"anim death_backgrenade",   0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   8},
        {"anim death_crotch",        0, 135, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,   9},
        {"anim death_fall_to_knees", 0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  10},
        {"anim back_death01",        0,  20, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  11},
        {"anim on a slope",          8,   0, 1.0f, {0.34f,0,1}, 0, 0, 0, 0, 0, 0, 0,   3},
        {"anim onto a ledge",        34,  70, 1.0f, {0,0,1}, 0,  0,  0,  1,  0,  0,  0,   4},
        {"anim wall, back1",         0,   0, 1.0f, {0,0,1}, 0,  0,  0,  2,  0,  0,  0,   1},
        {"anim wall, collapse",      0,  90, 1.0f, {0,0,1}, 0,  0,  0,  2,  0,  0,  0,   3},
        {"anim wall, fall_back",     0, 180, 1.0f, {0,0,1}, 0,  0,  0,  2,  0,  0,  0,   4},
        {"anim wall, right",         0,  45, 1.0f, {0,0,1}, 0,  0,  0,  2,  0,  0,  0,   6},
        {"anim death_run01",         0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  12},
        {"anim death_run02",         0,  55, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  13},
        {"anim death_run03",         0, 140, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  14},
        {"anim death_twist",         0,   0, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  15},
        {"anim death_knockedup",     0,  20, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  16},
        {"anim death_prone1",        0,  90, 1.0f, {0,0,1}, 0,  0,  0,  0,  0,  0,  0,  17},
        {"anim run01, wall",         0,   0, 1.0f, {0,0,1}, 0,  0,  0,  2,  0,  0,  0,  12},
    };

    int rc = 0;
    // RD_ONLY runs a single scenario by name, for when something needs to be
    // looked at closely rather than counted.
    const char *only = getenv("RD_ONLY");
    for (unsigned i = 0; i < sizeof(scen)/sizeof(scen[0]); i++) {
        if (only && strcmp(only, scen[i].name)) continue;
        rc |= RunScenario(scen[i]);
    }
    printf("\n%s\n", rc ? "SOME SCENARIOS FAILED" : "ALL SCENARIOS PASS");
    return rc;
}

// Minimal Com_* stubs so q_shared.c links in isolation.
// Added 2026-09-26: the solver now reports corpse hits for blood and sound,
// and wakes corpses when doors near them move. Neither has anything to do here.
extern "C" void CG_AddCorpseFleshImpact(const vec3_t, const vec3_t, int) {}
extern "C" int CG_GetBrushEntitiesInBounds(int, centity_t **, const vec3_t, const vec3_t) { return 0; }
// cg_props.cpp logs through the physics cvar too; the harness has no physics.
cvar_t *cg_physics_log;
// The grabber also takes hold of physics props; the harness has none.
extern "C" qboolean CG_PhysicsGrabCandidate(const vec3_t, const vec3_t, float, float *) { return qfalse; }
extern "C" qboolean CG_PhysicsGrabStart(const vec3_t, const vec3_t, float, float) { return qfalse; }
extern "C" qboolean CG_PhysicsGrabHeld(void) { return qfalse; }
extern "C" void CG_PhysicsGrabRelease(void) {}
extern "C" float CG_PhysicsGrabDistance(void) { return 0.0f; }
extern "C" void CG_PhysicsGrabSetDistance(float) {}
extern "C" void CG_PhysicsGrabDenied(void) {}
extern "C" void CG_PhysicsGrabSetTarget(const vec3_t) {}
extern "C" void CG_PhysicsGrabPoint(vec3_t out) { VectorClear(out); }
extern "C" qboolean CG_PhysicsPunt(const vec3_t, const vec3_t, float, float) { return qfalse; }
#ifndef RD_JOLT
// Without --jolt the Jolt ragdoll is not built: making one fails, and the
// particles carry every body.
int CG_JoltRagdollCreate(const vec3_t[RD_NUM_JOINTS], const vec3_t[RD_NUM_JOINTS], const float[RD_NUM_JOINTS], float) { return 0; }
void CG_JoltRagdollDestroy(int) {}
qboolean CG_JoltRagdollRead(int, vec3_t[RD_NUM_JOINTS], vec3_t[RD_NUM_JOINTS], qboolean[RD_NUM_JOINTS], vec3_t[RD_NUM_JOINTS], int *) { return qfalse; }
void CG_JoltRagdollAddVelocity(int, const vec3_t[RD_NUM_JOINTS]) {}
void CG_JoltRagdollHold(int, int, const vec3_t) {}
qboolean CG_JoltRagdollAwake(int) { return qfalse; }
void CG_JoltRagdollSleep(int) {}
void CG_JoltRagdollWake(int) {}
void CG_JoltRagdollReport(int, void (*)(const char *, ...)) {}
#endif
extern "C" void Com_Printf(const char *fmt, ...) { (void)fmt; }
extern "C" void Com_Error(int level, const char *fmt, ...) { (void)level; printf("Com_Error: %s\n", fmt); exit(1); }
extern "C" void Com_DPrintf(const char *fmt, ...) { (void)fmt; }
