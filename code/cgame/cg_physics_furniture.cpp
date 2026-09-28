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

// Added in OPM
// DESCRIPTION:
// Furniture built from world brushes (tables, benches, crates), found by
// code/physics/phys_furniture.cpp, as bodies that move.
//
// Until one is disturbed the world draws it and collides with it as always.
// The first time it moves, its surfaces are taken out of the world into a
// model of their own, drawn where the body is, and, when the server shares
// this collision model, its brushes go from it as the stand-ins of props do.

#include "cg_physics_local.h"
#include "../physics/phys_furniture.h"

#include <set>

typedef struct {
    physFurniture_t shape;
    JPH::BodyID     id;
    JPH::RVec3      centre; // where the body was made; the model's space is the world's
    JPH::RVec3      prevPos, curPos;
    JPH::Quat       prevRot, curRot;
    qhandle_t       model;
    qboolean        everMoved;
} cgFurniture_t;

static std::vector<cgFurniture_t> pf_furniture;
static std::set<int>              pf_brushes;

cvar_t *cg_physics_furniture;

// Before the world is built: which of its brushes are furniture, so the world
// leaves them out.
void CG_PhysicsFindFurniture(const void *bsp, long len)
{
    std::vector<physFurniture_t> found;

    pf_furniture.clear();
    pf_brushes.clear();

    if (!cg_physics_furniture->integer || !cg_physics_props->integer || cgi.apiversion < 5 || !cgi.R_DetachWorldSurfaces) {
        return;
    }

    // Its brushes stay in the collision model when the server is elsewhere,
    // so, as with props in clip brushes, it would leave an invisible table
    // behind.
    if (!CG_PhysicsClippedPropsMove()) {
        return;
    }

    Phys_FindFurniture(bsp, len, &found);

    for (size_t i = 0; i < found.size(); i++) {
        cgFurniture_t f;

        f.shape     = found[i];
        f.model     = 0;
        f.everMoved = qfalse;
        pf_furniture.push_back(f);

        for (size_t b = 0; b < found[i].brushes.size(); b++) {
            pf_brushes.insert(found[i].brushes[b]);
        }
    }
}

qboolean CG_PhysicsBrushIsFurniture(int brushNum)
{
    return pf_brushes.count(brushNum) ? qtrue : qfalse;
}

void CG_PhysicsUnloadFurniture(void)
{
    if (phys_system) {
        JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

        for (size_t i = 0; i < pf_furniture.size(); i++) {
            if (!pf_furniture[i].id.IsInvalid()) {
                bodies.RemoveBody(pf_furniture[i].id);
                bodies.DestroyBody(pf_furniture[i].id);
            }
        }
    }

    pf_furniture.clear();
    pf_brushes.clear();
}

// After the world: a body for each, asleep where it stands.
void CG_PhysicsLoadFurniture(void)
{
    int made = 0, failed = 0;

    if (!phys_system) {
        return;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (size_t i = 0; i < pf_furniture.size(); i++) {
        cgFurniture_t                  *f = &pf_furniture[i];
        JPH::StaticCompoundShapeSettings compound;
        vec3_t                           centre;
        int                              pieces = 0;
        float                            density, friction, restitution;

        f->id = JPH::BodyID();

        VectorAdd(f->shape.mins, f->shape.maxs, centre);
        VectorScale(centre, 0.5f, centre);

        for (size_t h = 0; h < f->shape.hulls.size(); h++) {
            const std::vector<float>& corners = f->shape.hulls[h];
            JPH::Array<JPH::Vec3>     points;

            for (size_t c = 0; c + 2 < corners.size(); c += 3) {
                vec3_t p = {corners[c] - centre[0], corners[c + 1] - centre[1], corners[c + 2] - centre[2]};
                points.push_back(PhysToJolt(p));
            }

            JPH::ConvexHullShapeSettings    hull(points, 0.005f);
            JPH::ShapeSettings::ShapeResult result = hull.Create();
            if (result.HasError()) {
                continue;
            }

            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), result.Get());
            pieces++;
        }

        JPH::ShapeSettings::ShapeResult result = pieces ? compound.Create() : JPH::ShapeSettings::ShapeResult();
        if (!pieces || result.HasError()) {
            failed++;
            continue;
        }

        CG_PhysicsMaterialFor(f->shape.shader, &density, &friction, &restitution);

        JPH::BodyCreationSettings settings(
            result.Get(), JPH::RVec3(PhysToJolt(centre)), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, PhysLayers::PROP
        );

        {
            vec3_t          size;
            float           area;

            VectorSubtract(f->shape.maxs, f->shape.mins, size);
            VectorScale(size, PHYS_UNITS_TO_METRES, size);
            // Half what its box would weigh as a prop: a table is a top on
            // legs, not a closed box.
            area = (size[0] * size[1] + size[1] * size[2] + size[2] * size[0]);

            settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = Q_clamp_float(area * density, 1.0f, 200.0f);
        }

        settings.mFriction       = friction;
        settings.mRestitution    = restitution;
        settings.mLinearDamping  = 0.05f;
        settings.mAngularDamping = 0.1f;
        settings.mMotionQuality  = JPH::EMotionQuality::LinearCast;
        settings.mUserData       = PHYS_USERDATA_FURNITURE_BASE + i;

        f->id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (f->id.IsInvalid()) {
            failed++;
            continue;
        }

        f->centre  = settings.mPosition;
        f->curPos  = f->prevPos = settings.mPosition;
        f->curRot  = f->prevRot = settings.mRotation;
        made++;

        if (cg_physics_log->integer > 1) {
            cgi.Printf(
                "  furniture %d: %s, %d brushes, %d surfaces, %d pieces, %.0f %.0f %.0f to %.0f %.0f %.0f, %.0f kg\n",
                (int)i,
                f->shape.shader,
                (int)f->shape.brushes.size(),
                (int)f->shape.surfaces.size(),
                pieces,
                f->shape.mins[0],
                f->shape.mins[1],
                f->shape.mins[2],
                f->shape.maxs[0],
                f->shape.maxs[1],
                f->shape.maxs[2],
                settings.mMassPropertiesOverride.mMass
            );
        }
    }

    if (cg_physics_log->integer) {
        cgi.Printf("physics: %d pieces of furniture in the brushwork move, %d could not be shaped\n", made, failed);
    }
}

// After each step.
void CG_PhysicsFurnitureStepped(void)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterfaceNoLock();

    for (size_t i = 0; i < pf_furniture.size(); i++) {
        cgFurniture_t *f = &pf_furniture[i];

        if (f->id.IsInvalid()) {
            continue;
        }

        f->prevPos = f->curPos;
        f->prevRot = f->curRot;

        if (!bodies.IsActive(f->id)) {
            continue;
        }

        bodies.GetPositionAndRotation(f->id, f->curPos, f->curRot);

        if (f->everMoved) {
            continue;
        }

        // Moving for the first time: out of the world, into a model of its own.
        f->everMoved = qtrue;
        f->model     = cgi.R_DetachWorldSurfaces(&f->shape.surfaces[0], (int)f->shape.surfaces.size());

        if (!f->model) {
            // Nothing to draw it with: it stays where the world draws it.
            bodies.SetMotionType(f->id, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
            bodies.SetPositionAndRotation(f->id, f->centre, JPH::Quat::sIdentity(), JPH::EActivation::DontActivate);
            f->curPos = f->prevPos = f->centre;
            f->curRot = f->prevRot = JPH::Quat::sIdentity();
            continue;
        }

        if (CG_PhysicsCanRemoveStandIns()) {
            for (size_t b = 0; b < f->shape.brushes.size(); b++) {
                cgi.CM_DisableBrush(f->shape.brushes[b]);
            }
        }

        if (cg_physics_log->integer) {
            cgi.Printf(
                "physics: furniture %d (%s) moved; %d surfaces drawn apart%s\n",
                (int)i,
                f->shape.shader,
                (int)f->shape.surfaces.size(),
                CG_PhysicsCanRemoveStandIns() ? ", its brushes gone from the collision model" : ""
            );
        }
    }
}

// Every frame: the pieces that have moved, between the last two steps.
void CG_PhysicsDrawFurniture(float frac)
{
    for (size_t i = 0; i < pf_furniture.size(); i++) {
        const cgFurniture_t *f = &pf_furniture[i];
        refEntity_t          ent;
        vec3_t               pos, centre;
        int                  k;

        // cg_physics_debug 3 and 4: every piece drawn turned over where it
        // stands, to look at its underside (see cg_physics_fill.cpp).
        const qboolean turnOver = (cg_physics_debug->integer == 3 || cg_physics_debug->integer == 4) ? qtrue : qfalse;

        if (!f->model && turnOver && !f->shape.surfaces.empty()) {
            pf_furniture[i].model = cgi.R_DetachWorldSurfaces(&f->shape.surfaces[0], (int)f->shape.surfaces.size());
        }

        if (!f->model) {
            continue;
        }

        {
            const JPH::Vec3 p = JPH::Vec3(f->prevPos) + (JPH::Vec3(f->curPos) - JPH::Vec3(f->prevPos)) * frac;
            JPH::Quat       r = f->prevRot.SLERP(f->curRot, frac).Normalized();

            if (turnOver) {
                r = r * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::JPH_PI);
            }

            memset(&ent, 0, sizeof(ent));
            PhysFromJolt(p, pos);
            Phys_AxisFromQuat(r, ent.axis);
        }

        // The model is in the world's space: its centre goes to the body's.
        PhysFromJolt(JPH::Vec3(f->centre), centre);
        VectorCopy(pos, ent.origin);
        for (k = 0; k < 3; k++) {
            VectorMA(ent.origin, -centre[k], ent.axis[k], ent.origin);
        }
        VectorCopy(ent.origin, ent.oldorigin);

        ent.reType = RT_MODEL;
        ent.hModel = f->model;
        cgi.R_AddRefEntityToScene(&ent, ENTITYNUM_NONE);

        // Its sides that were never drawn.
        CG_PhysicsDrawFurnitureFill((int)i, ent.origin, ent.axis);
    }
}

const physFurniture_t *CG_PhysicsFurnitureShape(int index)
{
    if (index < 0 || index >= (int)pf_furniture.size()) {
        return NULL;
    }

    return &pf_furniture[index].shape;
}

// For blasts: every piece, its middle now, and its body.
int CG_PhysicsFurnitureCount(void)
{
    return (int)pf_furniture.size();
}

qboolean CG_PhysicsFurnitureBody(int index, JPH::BodyID *id, vec3_t middle)
{
    const cgFurniture_t *f;

    if (index < 0 || index >= (int)pf_furniture.size() || pf_furniture[index].id.IsInvalid()) {
        return qfalse;
    }

    f   = &pf_furniture[index];
    *id = f->id;
    PhysFromJolt(JPH::Vec3(f->curPos), middle);
    return qtrue;
}
