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
// Server-side rigid body physics for the map's entity props: crates, barrels,
// cans, the small interactive objects. See g_physics.cpp.

#pragma once

class Entity;
class Vector;

// Builds the level's physics world from its BSP. Called as entities spawn.
void G_PhysicsInitLevel(const char *mapfile);
// Forgets the level's world and every body in it.
void G_PhysicsShutdown(void);
// After a save game has brought the level's entities back: the world again,
// and every prop a body again, asleep where it was saved.
void G_PhysicsRestoreLevel(void);
// Advances the simulation by the frame, and moves the entities it owns.
void G_PhysicsFrame(float frametime);

// Hands an entity to the physics: from now on its body decides where it is.
// Brush models take their shape from their brushes, others a box from their
// bounds. Returns false if it could not be shaped.
bool G_PhysicsAddEntity(Entity *ent);
// Takes an entity back from the physics (destroyed, removed). Whatever was
// resting on it is woken so it falls.
void G_PhysicsRemoveEntity(Entity *ent);
// Whether the physics owns this entity.
bool G_PhysicsOwns(const Entity *ent);

// Pushes a physics entity: an impulse in kilograms times units a second, at a
// point in the world.
void G_PhysicsImpulse(Entity *ent, const Vector& point, const Vector& impulse);
// The push a hit gives: damage along its direction, at where it struck.
void G_PhysicsDamaged(Entity *ent, float damage, const Vector& position, const Vector& direction);
// Someone moving into a physics entity (it blocked his movement): pushed along
// the way he is going, at up to his speed, less the heavier it is.
void G_PhysicsPushedBy(Entity *ent, Entity *pusher, const Vector& direction, float speed);
