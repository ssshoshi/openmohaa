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
// The joints of the ragdoll rig, shared by the particle solver
// (cg_ragdoll.cpp) and the Jolt one (cg_physics_ragdoll.cpp).

#pragma once

enum {
    RD_PELVIS,
    RD_SPINE,
    RD_SPINE1,
    RD_SPINE2,
    RD_NECK,
    RD_HEAD,
    RD_HEADTIP,
    RD_LUARM,
    RD_LFARM,
    RD_LHAND,
    RD_LHANDTIP,
    RD_RUARM,
    RD_RFARM,
    RD_RHAND,
    RD_RHANDTIP,
    RD_LTHIGH,
    RD_LCALF,
    RD_LFOOT,
    RD_LTOE,
    RD_RTHIGH,
    RD_RCALF,
    RD_RFOOT,
    RD_RTOE,

    RD_NUM_JOINTS
};
