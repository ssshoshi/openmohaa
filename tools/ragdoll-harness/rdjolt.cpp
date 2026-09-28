// The physics world for the Jolt build of the harness (./build.sh --jolt).
//
// In the game the Jolt ragdoll (code/cgame/cg_physics_ragdoll.cpp) lives in
// the world cgame builds from the map. Here the same code runs in a world built
// from the scenario instead: the floor plane and the ledge or wall box that
// the harness's own CG_Trace answers for, as static boxes, so the particle
// solver (which carries the body through the blend) and Jolt (which carries it
// after) meet the same surfaces. The world steps at 60 Hz on the harness's
// clock, and its contacts are passed to the ragdoll as the game's listener
// passes them.

#include "../../code/cgame/cg_physics_local.h"

#include <vector>

JPH::PhysicsSystem *phys_system;

static physWorld_t              rdj_world;
static std::vector<JPH::BodyID> rdj_static;
static float                    rdj_accum;

class RdjContacts final : public JPH::ContactListener
{
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override
    {
        CG_JoltRagdollContact(a, b, m);
    }

    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override
    {
        CG_JoltRagdollContact(a, b, m);
    }
};

static RdjContacts rdj_contacts;

static void RDJ_Trace(const char *fmt, ...)
{
    (void)fmt;
}

void RDJ_Init(float gravity)
{
    Phys_RegisterJolt(RDJ_Trace);
    Phys_CreateWorld(&rdj_world, 4096);
    phys_system = rdj_world.system;
    phys_system->SetContactListener(&rdj_contacts);
    phys_system->SetGravity(JPH::Vec3(0.0f, 0.0f, -gravity * PHYS_UNITS_TO_METRES));
}

static void RDJ_AddBox(const vec3_t centre, const vec3_t half, JPH::QuatArg rot)
{
    JPH::BoxShapeSettings           box(JPH::Vec3(half[0], half[1], half[2]) * PHYS_UNITS_TO_METRES);
    JPH::ShapeSettings::ShapeResult result = box.Create();

    if (result.HasError()) {
        return;
    }

    JPH::BodyCreationSettings settings(
        result.Get(), JPH::RVec3(PhysToJolt(centre)), rot, JPH::EMotionType::Static, PhysLayers::WORLD
    );
    settings.mFriction = 0.6f;

    const JPH::BodyID id = phys_system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    if (!id.IsInvalid()) {
        rdj_static.push_back(id);
    }
}

// The scenario's world: the floor through the origin with normal floorN, and
// the box when there is one.
void RDJ_Build(const vec3_t floorN, int box, const vec3_t mins, const vec3_t maxs)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (size_t i = 0; i < rdj_static.size(); i++) {
        bodies.RemoveBody(rdj_static[i]);
        bodies.DestroyBody(rdj_static[i]);
    }
    rdj_static.clear();
    rdj_accum = 0.0f;

    {
        const float     depth = 100.0f;
        const JPH::Vec3 n     = JPH::Vec3(floorN[0], floorN[1], floorN[2]).Normalized();
        vec3_t          centre, half = {5000.0f, 5000.0f, depth};

        VectorSet(centre, -n.GetX() * depth, -n.GetY() * depth, -n.GetZ() * depth);
        RDJ_AddBox(centre, half, JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), n));
    }

    if (box) {
        vec3_t centre, half;

        for (int k = 0; k < 3; k++) {
            centre[k] = (mins[k] + maxs[k]) * 0.5f;
            half[k]   = (maxs[k] - mins[k]) * 0.5f;
        }
        RDJ_AddBox(centre, half, JPH::Quat::sIdentity());
    }

    phys_system->OptimizeBroadPhase();
}

// A frame of the harness: the world at 60 Hz, as CG_PhysicsFrame steps it.
void RDJ_Step(float frametime)
{
    const float dt = 1.0f / 60.0f;

    rdj_accum = Q_min(rdj_accum + frametime, dt * 4.0f);
    while (rdj_accum >= dt) {
        CG_JoltRagdollsStep(dt);
        phys_system->Update(dt, 1, rdj_world.temp, rdj_world.jobs);
        rdj_accum -= dt;
    }
}
