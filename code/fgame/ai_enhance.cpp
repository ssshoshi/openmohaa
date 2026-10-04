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

// ai_enhance.cpp: shared code for the enemy AI improvements.

#include "g_local.h"
#include "level.h"
#include "ai_enhance.h"

void AI_Debug(const char *fmt, ...)
{
    va_list args;
    char    text[1024];

    if (!ai_debug || !ai_debug->integer) {
        return;
    }

    va_start(args, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    // The same line (numbers aside) again within 2 s is counted, not
    // printed: some checks run every frame.
    {
        static struct {
            unsigned hash;
            int      time;
            int      skipped;
        } recent[256];
        unsigned    hash = 5381;
        const char *p;
        int         slot;

        // Numbers are left out, but not digits in a name ("probe2", "ai1_0").
        bool inName = false;
        for (p = text; *p; p++) {
            bool digit = (*p >= '0' && *p <= '9');

            if (!inName && (digit || *p == '.' || *p == '-')) {
                continue;
            }
            inName = Q_isalpha(*p) || *p == '_' || (inName && digit);
            hash   = hash * 33 + (unsigned char)*p;
        }

        slot = hash & 255;
        if (recent[slot].hash == hash && level.inttime >= recent[slot].time
            && level.inttime < recent[slot].time + 2000) {
            recent[slot].skipped++;
            return;
        }

        if (recent[slot].hash == hash && recent[slot].skipped) {
            gi.Printf("ai t=%.2f: %s (x%d more)\n", level.time, text, recent[slot].skipped);
        } else {
            gi.Printf("ai t=%.2f: %s\n", level.time, text);
        }
        recent[slot].hash    = hash;
        recent[slot].time    = level.inttime;
        recent[slot].skipped = 0;
    }
}
