/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_shade_calc.c

#include "tr_local.h"


#define	WAVEVALUE( table, base, amplitude, phase, freq )  ((base) + table[ ( (int64_t) ( ( (phase) + tess.shaderTime * (freq) ) * FUNCTABLE_SIZE ) ) & FUNCTABLE_MASK ] * (amplitude))

static float *TableForFunc( genFunc_t func ) 
{
	switch ( func )
	{
	case GF_SIN:
		return tr.sinTable;
	case GF_TRIANGLE:
		return tr.triangleTable;
	case GF_SQUARE:
		return tr.squareTable;
	case GF_SAWTOOTH:
		return tr.sawToothTable;
	case GF_INVERSE_SAWTOOTH:
		return tr.inverseSawToothTable;
	case GF_NONE:
	default:
		break;
	}

	ri.Error( ERR_DROP, "TableForFunc called with invalid function '%d' in shader '%s'", func, tess.shader->name );
	return NULL;
}

/*
** RB_ResolveWaveForm
**
** MOH:AA lets a shader leave any wave/deform parameter set to the sentinel
** 1234567, meaning "supply this at draw time". For a regular entity the value
** comes from that entity's e.surfaces[0..3]; for everything else (world
** surfaces and static models, i.e. trees and brush) it comes from the
** r_static_shaderdata* cvars, which the client updates every frame from the
** global wind state. World and static-model surfaces additionally scale every
** parameter by the matching r_static_shadermultiplier* cvar.
**
** This mirrors the GL1 renderer's EvalWaveForm() exactly, including its reuse
** of r_static_shaderdata1 (rather than 3) for the frequency term.
*/
void RB_ResolveWaveForm( const waveForm_t *in, waveForm_t *out )
{
	out->func = in->func;

	if ( in->base != 1234567.0f ) {
		out->base = in->base;
	} else if ( backEnd.currentEntity ) {
		out->base = (float)backEnd.currentEntity->e.surfaces[0] / 16.0f - 8.0f;
	} else {
		out->base = r_static_shaderdata0->value;
	}

	if ( in->amplitude != 1234567.0f ) {
		out->amplitude = in->amplitude;
	} else if ( backEnd.currentEntity ) {
		out->amplitude = (float)backEnd.currentEntity->e.surfaces[1] / 16.0f;
	} else {
		out->amplitude = r_static_shaderdata1->value;
	}

	if ( in->phase != 1234567.0f ) {
		out->phase = in->phase;
	} else if ( backEnd.currentEntity ) {
		out->phase = (float)backEnd.currentEntity->e.surfaces[2] / 16.0f - 8.0f;
	} else {
		out->phase = r_static_shaderdata2->value;
	}

	if ( in->frequency != 1234567.0f ) {
		out->frequency = in->frequency;
	} else if ( backEnd.currentEntity ) {
		out->frequency = (float)backEnd.currentEntity->e.surfaces[3] / 16.0f;
	} else {
		out->frequency = r_static_shaderdata1->value;
	}

	if ( !backEnd.currentEntity ) {
		out->base      *= r_static_shadermultiplier0->value;
		out->amplitude *= r_static_shadermultiplier1->value;
		out->phase     *= r_static_shadermultiplier2->value;
		out->frequency *= r_static_shadermultiplier3->value;
	}
}

/*
** EvalWaveForm
**
** Evaluates a given waveForm_t, referencing backEnd.refdef.time directly
*/
static float EvalWaveForm( const waveForm_t *wf )
{
	float	*table;
	waveForm_t	rwf;

	RB_ResolveWaveForm( wf, &rwf );

	table = TableForFunc( rwf.func );

	return WAVEVALUE( table, rwf.base, rwf.amplitude, rwf.phase, rwf.frequency );
}

static float EvalWaveFormClamped( const waveForm_t *wf )
{
	float glow  = EvalWaveForm( wf );

	if ( glow < 0 )
	{
		return 0;
	}

	if ( glow > 1 )
	{
		return 1;
	}

	return glow;
}

/*
** RB_CalcStretchTexMatrix
*/
void RB_CalcStretchTexMatrix( const waveForm_t *wf, float *matrix )
{
	float p;

	p = 1.0f / EvalWaveForm( wf );

	matrix[0] = p; matrix[2] = 0; matrix[4] = 0.5f - 0.5f * p;
	matrix[1] = 0; matrix[3] = p; matrix[5] = 0.5f - 0.5f * p;
}

/*
====================================================================

DEFORMATIONS

====================================================================
*/

/*
========================
RB_CalcDeformVertexes

========================
*/
void RB_CalcDeformVertexes( deformStage_t *ds )
{
	int i;
	vec3_t	offset;
	float	scale;
	float	*xyz = ( float * ) tess.xyz;
	int16_t	*normal = tess.normal[0];
	float	*table;
	waveForm_t	wf;

	// Fill in any wind-driven parameters the shader left as sentinels, so trees
	// and brush sway here the same way they do in the GL1 renderer.
	RB_ResolveWaveForm( &ds->deformationWave, &wf );

	if ( wf.frequency == 0 )
	{
		scale = WAVEVALUE( TableForFunc( wf.func ), wf.base, wf.amplitude, wf.phase, wf.frequency );

		for ( i = 0; i < tess.numVertexes; i++, xyz += 4, normal += 4 )
		{
			R_VaoUnpackNormal(offset, normal);

			xyz[0] += offset[0] * scale;
			xyz[1] += offset[1] * scale;
			xyz[2] += offset[2] * scale;
		}
	}
	else
	{
		table = TableForFunc( wf.func );

		for ( i = 0; i < tess.numVertexes; i++, xyz += 4, normal += 4 )
		{
			float off = ( xyz[0] + xyz[1] + xyz[2] ) * ds->deformationSpread;

			scale = WAVEVALUE( table, wf.base,
				wf.amplitude,
				wf.phase + off,
				wf.frequency );

			R_VaoUnpackNormal(offset, normal);

			xyz[0] += offset[0] * scale;
			xyz[1] += offset[1] * scale;
			xyz[2] += offset[2] * scale;
		}
	}
}

/*
=========================
RB_CalcDeformNormals

Wiggle the normals for wavy environment mapping
=========================
*/
void RB_CalcDeformNormals( deformStage_t *ds ) {
	int i;
	float	scale;
	float	*xyz = ( float * ) tess.xyz;
	int16_t *normal = tess.normal[0];

	for ( i = 0; i < tess.numVertexes; i++, xyz += 4, normal += 4 ) {
		vec3_t fNormal;

		R_VaoUnpackNormal(fNormal, normal);

		scale = 0.98f;
		scale = R_NoiseGet4f( xyz[0] * scale, xyz[1] * scale, xyz[2] * scale,
			tess.shaderTime * ds->deformationWave.frequency );
		fNormal[ 0 ] += ds->deformationWave.amplitude * scale;

		scale = 0.98f;
		scale = R_NoiseGet4f( 100 + xyz[0] * scale, xyz[1] * scale, xyz[2] * scale,
			tess.shaderTime * ds->deformationWave.frequency );
		fNormal[ 1 ] += ds->deformationWave.amplitude * scale;

		scale = 0.98f;
		scale = R_NoiseGet4f( 200 + xyz[0] * scale, xyz[1] * scale, xyz[2] * scale,
			tess.shaderTime * ds->deformationWave.frequency );
		fNormal[ 2 ] += ds->deformationWave.amplitude * scale;

		VectorNormalizeFast( fNormal );

		R_VaoPackNormal(normal, fNormal);
	}
}

/*
========================
RB_CalcBulgeVertexes

========================
*/
void RB_CalcBulgeVertexes( deformStage_t *ds ) {
	int i;
	const float *st = ( const float * ) tess.texCoords[0];
	float		*xyz = ( float * ) tess.xyz;
	int16_t	*normal = tess.normal[0];
	double		now;

	now = backEnd.refdef.time * 0.001 * ds->bulgeSpeed;

	for ( i = 0; i < tess.numVertexes; i++, xyz += 4, st += 2, normal += 4 ) {
		int64_t off;
		float scale;
		vec3_t fNormal;

		R_VaoUnpackNormal(fNormal, normal);

		off = (float)( FUNCTABLE_SIZE / (M_PI*2) ) * ( st[0] * ds->bulgeWidth + now );

		scale = tr.sinTable[ off & FUNCTABLE_MASK ] * ds->bulgeHeight;
			
		xyz[0] += fNormal[0] * scale;
		xyz[1] += fNormal[1] * scale;
		xyz[2] += fNormal[2] * scale;
	}
}


/*
======================
RB_CalcMoveVertexes

A deformation that can move an entire surface along a wave path
======================
*/
void RB_CalcMoveVertexes( deformStage_t *ds ) {
	int			i;
	float		*xyz;
	float		*table;
	float		scale;
	vec3_t		offset;

	table = TableForFunc( ds->deformationWave.func );

	scale = WAVEVALUE( table, ds->deformationWave.base, 
		ds->deformationWave.amplitude,
		ds->deformationWave.phase,
		ds->deformationWave.frequency );

	VectorScale( ds->moveVector, scale, offset );

	xyz = ( float * ) tess.xyz;
	for ( i = 0; i < tess.numVertexes; i++, xyz += 4 ) {
		VectorAdd( xyz, offset, xyz );
	}
}


/*
=============
DeformText

Change a polygon into a bunch of text polygons
=============
*/
void DeformText( const char *text ) {
	int		i;
	vec3_t	origin, width, height;
	int		len;
	int		ch;
	float	color[4];
	float	bottom, top;
	vec3_t	mid;
	vec3_t fNormal;

	height[0] = 0;
	height[1] = 0;
	height[2] = -1;

	R_VaoUnpackNormal(fNormal, tess.normal[0]);
	CrossProduct( fNormal, height, width );

	// find the midpoint of the box
	VectorClear( mid );
	bottom = 999999;
	top = -999999;
	for ( i = 0 ; i < 4 ; i++ ) {
		VectorAdd( tess.xyz[i], mid, mid );
		if ( tess.xyz[i][2] < bottom ) {
			bottom = tess.xyz[i][2];
		}
		if ( tess.xyz[i][2] > top ) {
			top = tess.xyz[i][2];
		}
	}
	VectorScale( mid, 0.25f, origin );

	// determine the individual character size
	height[0] = 0;
	height[1] = 0;
	height[2] = ( top - bottom ) * 0.5f;

	VectorScale( width, height[2] * -0.75f, width );

	// determine the starting position
	len = strlen( text );
	VectorMA( origin, (len-1), width, origin );

	// clear the shader indexes
	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.firstIndex = 0;

	color[0] = color[1] = color[2] = color[3] = 1.0f;

	// draw each character
	for ( i = 0 ; i < len ; i++ ) {
		ch = text[i];
		ch &= 255;

		if ( ch != ' ' ) {
			int		row, col;
			float	frow, fcol, size;

			row = ch>>4;
			col = ch&15;

			frow = row*0.0625f;
			fcol = col*0.0625f;
			size = 0.0625f;

			RB_AddQuadStampExt( origin, width, height, color, fcol, frow, fcol + size, frow + size );
		}
		VectorMA( origin, -2, width, origin );
	}
}

/*
==================
GlobalVectorToLocal
==================
*/
static void GlobalVectorToLocal( const vec3_t in, vec3_t out ) {
	out[0] = DotProduct( in, backEnd.ori.axis[0] );
	out[1] = DotProduct( in, backEnd.ori.axis[1] );
	out[2] = DotProduct( in, backEnd.ori.axis[2] );
}

/*
=====================
AutospriteDeform

Assuming all the triangles for this shader are independent
quads, rebuild them as forward facing sprites
=====================
*/
static void AutospriteDeform( void ) {
	int		i;
	int		oldVerts;
	float	*xyz;
	vec3_t	mid, delta;
	float	radius;
	vec3_t	left, up;
	vec3_t	leftDir, upDir;

	if ( tess.numVertexes & 3 ) {
		ri.Printf( PRINT_WARNING, "Autosprite shader %s had odd vertex count\n", tess.shader->name );
	}
	if ( tess.numIndexes != ( tess.numVertexes >> 2 ) * 6 ) {
		ri.Printf( PRINT_WARNING, "Autosprite shader %s had odd index count\n", tess.shader->name );
	}

	oldVerts = tess.numVertexes;
	tess.numVertexes = 0;
	tess.numIndexes = 0;
	tess.firstIndex = 0;

	if ( backEnd.currentEntity != &tr.worldEntity ) {
		GlobalVectorToLocal( backEnd.viewParms.ori.axis[1], leftDir );
		GlobalVectorToLocal( backEnd.viewParms.ori.axis[2], upDir );
	} else {
		VectorCopy( backEnd.viewParms.ori.axis[1], leftDir );
		VectorCopy( backEnd.viewParms.ori.axis[2], upDir );
	}

	for ( i = 0 ; i < oldVerts ; i+=4 ) {
		vec4_t color;
		// find the midpoint
		xyz = tess.xyz[i];

		mid[0] = 0.25f * (xyz[0] + xyz[4] + xyz[8] + xyz[12]);
		mid[1] = 0.25f * (xyz[1] + xyz[5] + xyz[9] + xyz[13]);
		mid[2] = 0.25f * (xyz[2] + xyz[6] + xyz[10] + xyz[14]);

		VectorSubtract( xyz, mid, delta );
		radius = VectorLength( delta ) * 0.707f;		// / sqrt(2)

		VectorScale( leftDir, radius, left );
		VectorScale( upDir, radius, up );

		if ( backEnd.viewParms.isMirror ) {
			VectorSubtract( vec3_origin, left, left );
		}

	  // compensate for scale in the axes if necessary
  	if ( backEnd.currentStaticModel || backEnd.currentEntity->e.nonNormalizedAxes ) {
      float axisLength;

          // OPENMOHAA-specific stuff
	      //=========================
          if (backEnd.currentStaticModel) {
              axisLength = VectorLength(backEnd.currentStaticModel->axis[0]);
          }
          else
          //=========================
		  {
              axisLength = VectorLength(backEnd.currentEntity->e.axis[0]);
          }
  		if ( !axisLength ) {
	  		axisLength = 0;
  		} else {
	  		axisLength = 1.0f / axisLength;
  		}
      VectorScale(left, axisLength, left);
      VectorScale(up, axisLength, up);
    }

		VectorScale4(tess.color[i], 1.0f / 65535.0f, color);
		RB_AddQuadStamp( mid, left, up, color );
	}
}


/*
=====================
Autosprite2Deform

Autosprite2 will pivot a rectangular quad along the center of its long axis
=====================
*/
// MOH:AA's edge ordering, not ioq3's. Autosprite2Deform (ported from the GL1
// renderer) matches each short edge against a long edge by shared vertex, and
// that comparison only lands on the right edges with this ordering. ioq3's
// {0,1}{0,2}{0,3}{1,2}{1,3}{2,3} makes it pick the diagonals as the long edges
// and mis-orient the billboard.
int edgeVerts[6][2] = {
	{ 0, 1 },
	{ 0, 3 },
	{ 0, 2 },
	{ 1, 3 },
	{ 1, 2 },
	{ 3, 2 }
};

static void Autosprite2Deform( void ) {
	int		i, j;
	int		indexes;
	float	*xyz;
	vec3_t	forward;

	if ( tess.numVertexes & 3 ) {
		ri.Printf( PRINT_WARNING, "Autosprite2 shader %s had odd vertex count\n", tess.shader->name );
	}
	if ( tess.numIndexes != ( tess.numVertexes >> 2 ) * 6 ) {
		ri.Printf( PRINT_WARNING, "Autosprite2 shader %s had odd index count\n", tess.shader->name );
	}

	if ( backEnd.currentStaticModel || backEnd.currentEntity != &tr.worldEntity ) {
		GlobalVectorToLocal( backEnd.viewParms.ori.axis[0], forward );
	} else {
		VectorCopy( backEnd.viewParms.ori.axis[0], forward );
	}

	// MOH:AA's Autosprite2Deform, not ioq3's. The two differ in how each short
	// edge is projected: ioq3 reads triangle winding from tess.indexes, MOH:AA
	// compares the short edges against the long edges (and reorders edgeVerts to
	// match). On MOH:AA's tree-sprite meshes the ioq3 path builds a skewed quad
	// whose corners span a large depth range, so a per-vertex distance fade
	// (oneMinusDistFade) tears the billboard in half at its alphaFunc cutoff
	// instead of dissolving it uniformly.
	for ( i = 0, indexes = 0 ; i < tess.numVertexes ; i+=4, indexes+=6 ) {
		float shortLengths[2];
		int shortNums[2];
		float longLengths[2];
		int longNums[2];
		vec3_t	mid[2];
		vec3_t	major, minor;
		float	*v1, *v2, *v3, *v4;
		qboolean firstOnLeft, secondOnLeft;
		float edgeLength;

		// find the midpoint
		xyz = tess.xyz[i];

		// identify the two shortest and the two longest edges
		shortNums[0] = shortNums[1] = 0;
		shortLengths[0] = shortLengths[1] = 1000000000;
		longNums[1] = longNums[0] = 0;
		longLengths[1] = longLengths[0] = 0;

		for ( j = 0 ; j < 6 ; j++ ) {
			float	l;
			vec3_t	temp;

			v1 = xyz + 4 * edgeVerts[j][0];
			v2 = xyz + 4 * edgeVerts[j][1];

			VectorSubtract( v1, v2, temp );

			l = DotProduct( temp, temp );
			if ( l < shortLengths[0] ) {
				shortNums[1] = shortNums[0];
				shortLengths[1] = shortLengths[0];
				shortNums[0] = j;
				shortLengths[0] = l;
			} else if ( l < shortLengths[1] ) {
				shortNums[1] = j;
				shortLengths[1] = l;
			}

			if ( l > longLengths[0] ) {
				longNums[1] = longNums[0];
				longLengths[1] = longLengths[0];
				longNums[0] = j;
				longLengths[0] = l;
			} else if ( l > longLengths[1] ) {
				longNums[1] = j;
				longLengths[1] = l;
			}
		}

		for ( j = 0 ; j < 2 ; j++ ) {
			v1 = xyz + 4 * edgeVerts[shortNums[j]][0];
			v2 = xyz + 4 * edgeVerts[shortNums[j]][1];

			mid[j][0] = 0.5f * (v1[0] + v2[0]);
			mid[j][1] = 0.5f * (v1[1] + v2[1]);
			mid[j][2] = 0.5f * (v1[2] + v2[2]);
		}

		// find the vector of the major axis
		VectorSubtract( mid[1], mid[0], major );

		// cross this with the view direction to get minor axis
		CrossProduct( major, forward, minor );
		VectorNormalize( minor );

		v1 = xyz + 4 * edgeVerts[shortNums[0]][0];
		v2 = xyz + 4 * edgeVerts[shortNums[0]][1];

		// the direction each short edge projects in is decided by whether it
		// shares a vertex with the matching long edge
		if ( edgeVerts[shortNums[0]][0] == edgeVerts[longNums[0]][0]
			|| edgeVerts[shortNums[0]][0] == edgeVerts[longNums[0]][1] ) {
			firstOnLeft = qtrue;
		} else {
			firstOnLeft = qfalse;
		}

		if ( edgeVerts[shortNums[1]][0] == edgeVerts[longNums[1]][0]
			|| edgeVerts[shortNums[1]][0] == edgeVerts[longNums[1]][1] ) {
			secondOnLeft = qtrue;
		} else {
			secondOnLeft = qfalse;
		}

		if ( firstOnLeft == secondOnLeft ) {
			v3 = xyz + 4 * edgeVerts[shortNums[1]][0];
			v4 = xyz + 4 * edgeVerts[shortNums[1]][1];
		} else {
			v3 = xyz + 4 * edgeVerts[shortNums[1]][1];
			v4 = xyz + 4 * edgeVerts[shortNums[1]][0];
		}

		// re-project the points
		edgeLength = sqrt( shortLengths[0] ) * 0.5f;
		v1[0] = -edgeLength * minor[0] + mid[0][0];
		v1[1] = -edgeLength * minor[1] + mid[0][1];
		v1[2] = -edgeLength * minor[2] + mid[0][2];
		v2[0] = minor[0] * edgeLength + mid[0][0];
		v2[1] = minor[1] * edgeLength + mid[0][1];
		v2[2] = minor[2] * edgeLength + mid[0][2];

		edgeLength = sqrt( shortLengths[1] ) * 0.5f;
		v3[0] = -edgeLength * minor[0] + mid[1][0];
		v3[1] = -edgeLength * minor[1] + mid[1][1];
		v3[2] = -edgeLength * minor[2] + mid[1][2];
		v4[0] = minor[0] * edgeLength + mid[1][0];
		v4[1] = minor[1] * edgeLength + mid[1][1];
		v4[2] = minor[2] * edgeLength + mid[1][2];
	}
}


/*
=====================
RB_DeformTessGeometry

=====================
*/
/*
========================
RB_CalcFlapVertexes

Used by MOH:AA for cloth that flaps in the wind (flags, banners, tent
canvas). Each vertex is pushed along its normal by a wave, scaled by that
vertex's S or T texture coordinate so one edge stays pinned.

Any wave parameter left at the sentinel 1234567 is supplied by the entity, or
by the r_static_shaderdata cvars for world surfaces.
========================
*/
static void RB_CalcFlapVertexes( deformStage_t *ds, texDirection_t coordsToUse )
{
	int      i;
	float   *xyz    = ( float * ) tess.xyz;
	int16_t *normal = tess.normal[0];
	float   *st     = ( float * ) tess.texCoords;
	float   *table;
	vec3_t   offset;
	float    scale, vertexScale;
	float    base, amplitude, phase, frequency;
	float    min = ds->bulgeWidth;
	float    max = ds->bulgeHeight;

	if ( ds->deformationWave.base != 1234567.0f ) {
		base = ds->deformationWave.base;
	} else if ( backEnd.currentEntity ) {
		base = ( float )backEnd.currentEntity->e.surfaces[0] / 16.0f - 8.0f;
	} else {
		base = r_static_shaderdata0->value;
	}

	if ( ds->deformationWave.amplitude != 1234567.0f ) {
		amplitude = ds->deformationWave.amplitude;
	} else if ( backEnd.currentEntity ) {
		amplitude = ( float )backEnd.currentEntity->e.surfaces[1] / 16.0f;
	} else {
		amplitude = r_static_shaderdata1->value;
	}

	if ( ds->deformationWave.phase != 1234567.0f ) {
		phase = ds->deformationWave.phase;
	} else if ( backEnd.currentEntity ) {
		phase = ( float )backEnd.currentEntity->e.surfaces[2] / 16.0f - 8.0f;
	} else {
		phase = r_static_shaderdata2->value;
	}

	if ( ds->deformationWave.frequency != 1234567.0f ) {
		frequency = ds->deformationWave.frequency;
	} else if ( backEnd.currentEntity ) {
		frequency = ( float )backEnd.currentEntity->e.surfaces[3] / 16.0f;
	} else {
		frequency = r_static_shaderdata3->value;
	}

	if ( !backEnd.currentEntity ) {
		base      *= r_static_shadermultiplier0->value;
		amplitude *= r_static_shadermultiplier1->value;
		phase     *= r_static_shadermultiplier2->value;
		frequency *= r_static_shadermultiplier3->value;
	}

	if ( frequency ) {
		table = TableForFunc( ds->deformationWave.func );

		// Note: the GL1 renderer computes a per-vertex spread offset here and
		// then never applies it, so the whole surface waves in phase. Kept as
		// is, since changing it would make the two renderers disagree.
		scale = WAVEVALUE( table, base, amplitude, phase, frequency );

		for ( i = 0; i < tess.numVertexes; i++, xyz += 4, st += 2, normal += 4 ) {
			R_VaoUnpackNormal( offset, normal );

			vertexScale = ( max - min ) * st[coordsToUse] + min;

			xyz[0] += scale * vertexScale * offset[0];
			xyz[1] += scale * vertexScale * offset[1];
			xyz[2] += scale * vertexScale * offset[2];
		}
	} else {
		scale = EvalWaveForm( &ds->deformationWave );

		for ( i = 0; i < tess.numVertexes; i++, xyz += 4, st += 2, normal += 4 ) {
			R_VaoUnpackNormal( offset, normal );

			vertexScale = ( max - min ) * st[coordsToUse] + min;

			xyz[0] += scale * vertexScale * offset[0];
			xyz[1] += scale * vertexScale * offset[1];
			xyz[2] += scale * vertexScale * offset[2];
		}
	}
}


/*
========================
LightGlowDeform

MOH:AA's light coronas. Replaces each incoming quad with a camera facing quad
of the same size, pulled toward the viewer so it is not buried in the surface
it sits on.
========================
*/
static void LightGlowDeform( void )
{
	int    i;
	int    oldVerts;
	float *xyz;
	vec3_t mid, delta;
	float  radius, dist, ofs;
	vec3_t forward, left, up;
	vec3_t leftDir, upDir;

	if ( tess.numVertexes & 3 ) {
		ri.Printf( PRINT_WARNING, "LightGlowDeform shader %s had odd vertex count\n", tess.shader->name );
	}
	if ( tess.numIndexes != ( tess.numVertexes >> 2 ) * 6 ) {
		ri.Printf( PRINT_WARNING, "LightGlowDeform shader %s had odd index count\n", tess.shader->name );
	}

	oldVerts = tess.numVertexes;
	tess.numVertexes = 0;
	tess.numIndexes = 0;

	if ( backEnd.currentEntity == &tr.worldEntity ) {
		VectorCopy( backEnd.viewParms.ori.axis[1], leftDir );
		VectorCopy( backEnd.viewParms.ori.axis[2], upDir );
	} else {
		GlobalVectorToLocal( backEnd.viewParms.ori.axis[1], leftDir );
		GlobalVectorToLocal( backEnd.viewParms.ori.axis[2], upDir );
	}

	for ( i = 0; i < oldVerts; i += 4 ) {
		vec4_t color;

		xyz = tess.xyz[i];

		mid[0] = ( xyz[0] + xyz[4] + xyz[8]  + xyz[12] ) * 0.25f;
		mid[1] = ( xyz[1] + xyz[5] + xyz[9]  + xyz[13] ) * 0.25f;
		mid[2] = ( xyz[2] + xyz[6] + xyz[10] + xyz[14] ) * 0.25f;

		VectorSubtract( xyz, mid, delta );

		radius = VectorLength( delta ) * 0.707f;
		VectorAdd( mid, backEnd.ori.origin, delta );
		VectorSubtract( backEnd.viewParms.ori.origin, delta, forward );

		dist = VectorNormalize( forward ) - 4.0f;

		VectorScale( forward, radius, forward );
		VectorScale( leftDir, radius, left );
		VectorScale( upDir, radius, up );

		if ( backEnd.viewParms.isMirror ) {
			VectorSubtract( vec3_origin, forward, forward );
			VectorSubtract( vec3_origin, left, left );
		}

		if ( backEnd.currentStaticModel || backEnd.currentEntity->e.nonNormalizedAxes ) {
			float axisLength;

			if ( backEnd.currentStaticModel ) {
				axisLength = VectorLength( backEnd.currentStaticModel->axis[0] );
			} else {
				axisLength = VectorLength( backEnd.currentEntity->e.axis[0] );
			}

			if ( axisLength != 0.0f ) {
				VectorScale( forward, axisLength, forward );
				VectorScale( left, axisLength, left );
				VectorScale( up, axisLength, up );
			} else {
				VectorClear( forward );
				VectorClear( left );
				VectorClear( up );
			}
		}

		ofs = VectorLength( forward );
		if ( ofs > dist ) {
			VectorNormalizeFast( forward );
			VectorScale( forward, dist, forward );
		}

		VectorAdd( mid, forward, mid );

		// tess colors are 16 bit here, RB_AddQuadStamp wants normalized floats
		color[0] = tess.color[i][0] / 65535.0f;
		color[1] = tess.color[i][1] / 65535.0f;
		color[2] = tess.color[i][2] / 65535.0f;
		color[3] = tess.color[i][3] / 65535.0f;

		RB_AddQuadStamp( mid, left, up, color );
	}
}


void RB_DeformTessGeometry( void ) {
	int		i;
	deformStage_t	*ds;

	if(!ShaderRequiresCPUDeforms(tess.shader))
	{
		// we don't need the following CPU deforms
		return;
	}

	for ( i = 0 ; i < tess.shader->numDeforms ; i++ ) {
		ds = &tess.shader->deforms[ i ];

		switch ( ds->deformation ) {
        case DEFORM_NONE:
            break;
		case DEFORM_NORMALS:
			RB_CalcDeformNormals( ds );
			break;
		case DEFORM_WAVE:
			RB_CalcDeformVertexes( ds );
			break;
		case DEFORM_BULGE:
			RB_CalcBulgeVertexes( ds );
			break;
		case DEFORM_MOVE:
			RB_CalcMoveVertexes( ds );
			break;
		case DEFORM_PROJECTION_SHADOW:
			RB_ProjectionShadowDeform();
			break;
		case DEFORM_AUTOSPRITE:
			AutospriteDeform();
			break;
		case DEFORM_AUTOSPRITE2:
			Autosprite2Deform();
			break;
		//
		// OPENMOHAA-specific stuff
		//=========================
		case DEFORM_LIGHTGLOW:
			LightGlowDeform();
			break;
		case DEFORM_FLAP_S:
			RB_CalcFlapVertexes( ds, USE_S_COORDS );
			break;
		case DEFORM_FLAP_T:
			RB_CalcFlapVertexes( ds, USE_T_COORDS );
			break;
		//=========================
		case DEFORM_TEXT0:
		case DEFORM_TEXT1:
		case DEFORM_TEXT2:
		case DEFORM_TEXT3:
		case DEFORM_TEXT4:
		case DEFORM_TEXT5:
		case DEFORM_TEXT6:
		case DEFORM_TEXT7:
			DeformText( backEnd.refdef.text[ds->deformation - DEFORM_TEXT0] );
			break;
		}
	}
}

/*
====================================================================

COLORS

====================================================================
*/


/*
** RB_CalcWaveColorSingle
*/
float RB_CalcWaveColorSingle( const waveForm_t *wf )
{
	float glow;

	if ( wf->func == GF_NOISE ) {
		glow = wf->base + R_NoiseGet4f( 0, 0, 0, ( tess.shaderTime + wf->phase ) * wf->frequency ) * wf->amplitude;
	} else {
		glow = EvalWaveForm( wf ) * tr.identityLight;
	}
	
	if ( glow < 0 ) {
		glow = 0;
	}
	else if ( glow > 1 ) {
		glow = 1;
	}

	return glow;
}

/*
** RB_CalcWaveAlphaSingle
*/
float RB_CalcWaveAlphaSingle( const waveForm_t *wf )
{
	return EvalWaveFormClamped( wf );
}

/*
** RB_CalcModulateColorsByFog
*/
void RB_CalcModulateColorsByFog( unsigned char *colors ) {
	int		i;
	float	texCoords[SHADER_MAX_VERTEXES][2] = {{0.0f}};

	// calculate texcoords so we can derive density
	// this is not wasted, because it would only have
	// been previously called if the surface was opaque
	RB_CalcFogTexCoords( texCoords[0] );

	for ( i = 0; i < tess.numVertexes; i++, colors += 4 ) {
		float f = 1.0 - R_FogFactor( texCoords[i][0], texCoords[i][1] );
		colors[0] *= f;
		colors[1] *= f;
		colors[2] *= f;
	}
}


/*
====================================================================

TEX COORDS

====================================================================
*/

/*
========================
RB_CalcFogTexCoords

To do the clipped fog plane really correctly, we should use
projected textures, but I don't trust the drivers and it
doesn't fit our shader data.
========================
*/
void RB_CalcFogTexCoords( float *st ) {
	int			i;
	float		*v;
	float		s, t;
	float		eyeT;
	qboolean	eyeOutside;
	fog_t		*fog;
	vec3_t		local;
	vec4_t		fogDistanceVector, fogDepthVector = {0, 0, 0, 0};

	fog = tr.world->fogs + tess.fogNum;

	// all fogging distance is based on world Z units
	VectorSubtract( backEnd.ori.origin, backEnd.viewParms.ori.origin, local );
	fogDistanceVector[0] = -backEnd.ori.modelMatrix[2];
	fogDistanceVector[1] = -backEnd.ori.modelMatrix[6];
	fogDistanceVector[2] = -backEnd.ori.modelMatrix[10];
	fogDistanceVector[3] = DotProduct( local, backEnd.viewParms.ori.axis[0] );

	// scale the fog vectors based on the fog's thickness
	fogDistanceVector[0] *= fog->tcScale;
	fogDistanceVector[1] *= fog->tcScale;
	fogDistanceVector[2] *= fog->tcScale;
	fogDistanceVector[3] *= fog->tcScale;

	// rotate the gradient vector for this orientation
	if ( fog->hasSurface ) {
		fogDepthVector[0] = fog->surface[0] * backEnd.ori.axis[0][0] + 
			fog->surface[1] * backEnd.ori.axis[0][1] + fog->surface[2] * backEnd.ori.axis[0][2];
		fogDepthVector[1] = fog->surface[0] * backEnd.ori.axis[1][0] + 
			fog->surface[1] * backEnd.ori.axis[1][1] + fog->surface[2] * backEnd.ori.axis[1][2];
		fogDepthVector[2] = fog->surface[0] * backEnd.ori.axis[2][0] + 
			fog->surface[1] * backEnd.ori.axis[2][1] + fog->surface[2] * backEnd.ori.axis[2][2];
		fogDepthVector[3] = -fog->surface[3] + DotProduct( backEnd.ori.origin, fog->surface );

		eyeT = DotProduct( backEnd.ori.viewOrigin, fogDepthVector ) + fogDepthVector[3];
	} else {
		eyeT = 1;	// non-surface fog always has eye inside
	}

	// see if the viewpoint is outside
	// this is needed for clipping distance even for constant fog

	if ( eyeT < 0 ) {
		eyeOutside = qtrue;
	} else {
		eyeOutside = qfalse;
	}

	fogDistanceVector[3] += 1.0/512;

	// calculate density for each point
	for (i = 0, v = tess.xyz[0] ; i < tess.numVertexes ; i++, v += 4) {
		// calculate the length in fog
		s = DotProduct( v, fogDistanceVector ) + fogDistanceVector[3];
		t = DotProduct( v, fogDepthVector ) + fogDepthVector[3];

		// partially clipped fogs use the T axis		
		if ( eyeOutside ) {
			if ( t < 1.0 ) {
				t = 1.0/32;	// point is outside, so no fogging
			} else {
				t = 1.0/32 + 30.0/32 * t / ( t - eyeT );	// cut the distance at the fog plane
			}
		} else {
			if ( t < 0 ) {
				t = 1.0/32;	// point is outside, so no fogging
			} else {
				t = 31.0/32;
			}
		}

		st[0] = s;
		st[1] = t;
		st += 2;
	}
}

/*
** RB_CalcTurbulentFactors
*/
void RB_CalcTurbulentFactors( const waveForm_t *wf, float *amplitude, float *now )
{
	*now = wf->phase + tess.shaderTime * wf->frequency;
	*amplitude = wf->amplitude;
}

/*
** RB_CalcScaleTexMatrix
*/
void RB_CalcScaleTexMatrix( const float scale[2], float *matrix )
{
	matrix[0] = scale[0]; matrix[2] = 0.0f;     matrix[4] = 0.0f;
	matrix[1] = 0.0f;     matrix[3] = scale[1]; matrix[5] = 0.0f;
}

/*
** RB_CalcScrollTexMatrix
*/
void RB_CalcScrollTexMatrix( const float scrollSpeed[2], float *matrix )
{
	double timeScale = tess.shaderTime;
	double adjustedScrollS, adjustedScrollT;

	adjustedScrollS = scrollSpeed[0] * timeScale;
	adjustedScrollT = scrollSpeed[1] * timeScale;

	// clamp so coordinates don't continuously get larger, causing problems
	// with hardware limits
	adjustedScrollS = adjustedScrollS - floor( adjustedScrollS );
	adjustedScrollT = adjustedScrollT - floor( adjustedScrollT );

	matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = adjustedScrollS;
	matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = adjustedScrollT;
}

/*
** RB_CalcTransformTexMatrix
*/
void RB_CalcTransformTexMatrix( const texModInfo_t *tmi, float *matrix  )
{
	matrix[0] = tmi->matrix[0][0]; matrix[2] = tmi->matrix[1][0]; matrix[4] = tmi->translate[0];
	matrix[1] = tmi->matrix[0][1]; matrix[3] = tmi->matrix[1][1]; matrix[5] = tmi->translate[1];
}

/*
** RB_CalcTransWaveTexMatrix
**
** MOH:AA's tcMod wavetrans and wavetrant: s, or t, slides by a waveform.
*/
void RB_CalcTransWaveTexMatrix( const waveForm_t *wf, qboolean onT, float *matrix )
{
	float p = EvalWaveForm( wf );

	matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = onT ? 0.0f : p;
	matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = onT ? p : 0.0f;
}

/*
** RB_CalcParallaxTexMatrix
**
** MOH:AA's tcMod parallax: the texture slides with the viewer's position.
*/
void RB_CalcParallaxTexMatrix( const float *rate, float *matrix )
{
	matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = backEnd.refdef.vieworg[0] * rate[0];
	matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = backEnd.refdef.vieworg[1] * rate[1];
}

/*
** RB_CalcOffsetTexMatrix
**
** MOH:AA's tcMod offset. Either value may be "fromEntity", stored as 1234567,
** which takes it from the entity's shader data, or for a static model from the
** r_static_shaderdata cvars. As in GL1, only the first value is looked at to
** decide that, for both.
*/
void RB_CalcOffsetTexMatrix( const float *offset, float *matrix )
{
	float offsetS, offsetT;

	if ( offset[0] != 1234567 ) {
		offsetS = offset[0];
		offsetT = offset[1];
	} else if ( backEnd.currentEntity ) {
		offsetS = backEnd.currentEntity->e.shader_data[0];
		offsetT = backEnd.currentEntity->e.shader_data[1];
	} else {
		offsetS = r_static_shaderdata0->value;
		offsetT = r_static_shaderdata1->value;
	}

	if ( !backEnd.currentEntity ) {
		offsetS *= r_static_shadermultiplier0->value;
		offsetT *= r_static_shadermultiplier1->value;
	}

	matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = offsetS;
	matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = offsetT;
}

/*
** RB_CalcRotateTexMatrix
*/
void RB_CalcRotateTexMatrix( float degsPerSecond, float *matrix )
{
	double timeScale = tess.shaderTime;
	double degs;
	int64_t index;
	float sinValue, cosValue;

	degs = -degsPerSecond * timeScale;
	index = degs * ( FUNCTABLE_SIZE / 360.0f );

	sinValue = tr.sinTable[ index & FUNCTABLE_MASK ];
	cosValue = tr.sinTable[ ( index + FUNCTABLE_SIZE / 4 ) & FUNCTABLE_MASK ];

	matrix[0] = cosValue; matrix[2] = -sinValue; matrix[4] = 0.5 - 0.5 * cosValue + 0.5 * sinValue;
	matrix[1] = sinValue; matrix[3] = cosValue;  matrix[5] = 0.5 - 0.5 * sinValue - 0.5 * cosValue;
}

//
// OPENMOHAA-specific stuff
//

void RB_CalcLightGridColor(unsigned char* colors)
{
    int i;

    if (!backEnd.currentEntity) {
        for (i = 0; i < tess.numVertexes; i++) {
            colors[i * 4] = ((byte*)&backEnd.currentStaticModel->iGridLighting)[0];
            colors[i * 4 + 1] = ((byte*)&backEnd.currentStaticModel->iGridLighting)[1];
            colors[i * 4 + 2] = ((byte*)&backEnd.currentStaticModel->iGridLighting)[2];
            colors[i * 4 + 3] = ((byte*)&backEnd.currentStaticModel->iGridLighting)[3];
        }
    }
    else if (backEnd.currentEntity != &tr.worldEntity) {
        for (i = 0; i < tess.numVertexes; i++) {
            colors[i * 4] = ((byte*)&backEnd.currentEntity->iGridLighting)[0];
            colors[i * 4 + 1] = ((byte*)&backEnd.currentEntity->iGridLighting)[1];
            colors[i * 4 + 2] = ((byte*)&backEnd.currentEntity->iGridLighting)[2];
            colors[i * 4 + 3] = ((byte*)&backEnd.currentEntity->iGridLighting)[3];
        }
    }
    else {
        ri.Printf(PRINT_ALL,
            "##### shader '%s' incorrectly uses rgbGen lightingGrid or lightingSpherical; was rgbGen vertex intended?\n",
            tess.shader->name);

        for (i = 0; i < tess.numVertexes; i++) {
            colors[i * 4] = 0xFF;
            colors[i * 4 + 1] = 0xFF;
            colors[i * 4 + 2] = 0xFF;
            colors[i * 4 + 3] = 0xFF;
        }
    }
}