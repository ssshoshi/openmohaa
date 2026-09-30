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
// In-game bug, idea and feedback reports (cg_bugreport.cpp).

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_BugReportInit(void);
    // bugreport [category|cancel]: aim, then lock and open the menu.
    void CG_BugReport_f(void);
    // br_submit [<type> <category> <title> [description]]: write the bundle.
    void CG_BugReportSubmit_f(void);
    // Once a frame before the scene: the picking and the outline.
    void CG_BugReportFrame(void);
    // Once a frame after it: the overlay and the steps of writing a report.
    void CG_BugReportDraw2D(void);

#ifdef __cplusplus
}
#endif
