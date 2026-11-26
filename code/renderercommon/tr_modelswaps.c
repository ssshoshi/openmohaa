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

// tr_modelswaps.c -- a map's static models shown as another model by their scale
//
// A map places one model at many scales: the stock oak stands from 0.6 to 2.1 times
// its size. A remade tree is one shape at every scale, so a big old oak would be a
// young one blown up. models/static/opm_swaps.txt (from a content pak) names, a line
// each, a model, the scale from which another is shown in its place, and that model:
//
//     // model            at least   instead
//     tree_oak.tik        1.2        tree_oak_large.tik
//
// Models are matched by file name, whatever folder the map wrote (maps have both
// "static/tree_oak.tik" and "static//tree_oak.tik"); the replacement keeps that folder.
// A rule whose replacement is not in the game is left out. The last rule that applies
// wins, so they are listed from the smallest scale up.

#include "tr_common.h"

#define SWAPS_FILE "models/static/opm_swaps.txt"
#define MAX_SWAPS  64

typedef struct {
    char  model[MAX_QPATH];
    float minScale;
    char  instead[MAX_QPATH];
} modelSwap_t;

static const char *R_SwapBaseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static modelSwap_t swaps[MAX_SWAPS];
static int         numSwaps;

/*
================
R_BeginModelSwaps

Reads the rules, once a map's static models are about to be read.
================
*/
void R_BeginModelSwaps(void)
{
    char       *text;
    const char *p, *token;

    numSwaps = 0;
    if (ri.FS_ReadFileEx(SWAPS_FILE, (void **)&text, qtrue) <= 0 || !text) {
        return;
    }
    p = text;
    while (numSwaps < MAX_SWAPS) {
        modelSwap_t *s = &swaps[numSwaps];

        token = COM_ParseExt((char **)&p, qtrue);
        if (!token[0]) {
            break;
        }
        Q_strncpyz(s->model, token, sizeof(s->model));
        s->minScale = atof(COM_ParseExt((char **)&p, qfalse));
        token       = COM_ParseExt((char **)&p, qfalse);
        if (!token[0]) {
            ri.Printf(PRINT_WARNING, "%s: %s has no model to show instead\n", SWAPS_FILE, s->model);
            continue;
        }
        Q_strncpyz(s->instead, token, sizeof(s->instead));
        if (ri.FS_ReadFileEx(va("models/static/%s", s->instead), NULL, qtrue) <= 0) {
            ri.Printf(PRINT_WARNING, "%s: models/static/%s is not in the game\n", SWAPS_FILE, s->instead);
            continue;
        }
        numSwaps++;
    }
    ri.FS_FreeFile(text);
}

/*
================
R_SwapStaticModel

A static model's name (a buffer of size bytes) changed to the model the rules
show in its place at this scale. Returns whether it was.
================
*/
qboolean R_SwapStaticModel(char *name, size_t size, float scale)
{
    const modelSwap_t *use = NULL;
    char              *base;
    int                i;

    base = (char *)R_SwapBaseName(name);
    for (i = 0; i < numSwaps; i++) {
        if (!Q_stricmp(base, swaps[i].model) && scale >= swaps[i].minScale - 0.001f) {
            use = &swaps[i];
        }
    }
    if (!use || (size_t)(base - name) + strlen(use->instead) + 1 > size) {
        return qfalse;
    }
    strcpy(base, use->instead);
    return qtrue;
}
