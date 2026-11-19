// Added in OPM: r_motionBlur (tr_motionblur.c), the longest smear about each
// tile of MB_TILE by MB_TILE pixels: in each MB_TILE pixels of a row (MB_ROWS),
// in each tile from those, then in it and the eight round it (MB_NEIGHBOUR).
// The gather reads along that smear, so what moves smears over what is round
// it, not only over itself. (Each tile read whole in one pass was 0.44 ms at
// 1440p: too few tiles to share the reading.)

uniform sampler2D u_MbVelocityMap; // the smears (mbvelocity_fp.glsl), or the tiles' so far
uniform vec4      u_MbParams;      // where the view starts in the image read, in pixels; where it ends (the last pixel in it)

// Each tile keeps the longest smear (xy), how long the shortest is (z), and
// whether the view weapon is in it (w): where the longest and the shortest
// are near the same and there is no weapon, the gather need not tell one
// pixel from another (a turn of the camera, mostly).
vec2  best;
float bestLen;
float shortest;
float weapon;

void Take(ivec2 at)
{
	vec4 v = texelFetch(u_MbVelocityMap, at, 0);
#if defined(MB_ROWS)
	// a pixel's smear and distance, -1 for the view weapon's
	if (v.z < 0.0)
	{
		weapon = 1.0;
		return;
	}
	float l = length(v.xy);
	shortest = min(shortest, l);
#else
	float l = length(v.xy);
	shortest = min(shortest, v.z);
	weapon   = max(weapon, v.w);
#endif
	if (l > bestLen)
	{
		bestLen = l;
		best    = v.xy;
	}
}

void main()
{
	ivec2 tile = ivec2(gl_FragCoord.xy);
	ivec2 lo   = ivec2(u_MbParams.xy);
	ivec2 hi   = ivec2(u_MbParams.zw);

	best     = vec2(0.0);
	bestLen  = 0.0;
	shortest = 1.0e4;
	weapon   = 0.0;

#if defined(MB_NEIGHBOUR)
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			Take(clamp(tile + ivec2(x, y), lo, hi));
		}
	}
#else
	// across MB_TILE pixels of a row (MB_ROWS: into a column of tiles' rows),
	// then down MB_TILE rows of those (into the tiles)
  #if defined(MB_ROWS)
	ivec2 first = ivec2(lo.x + tile.x * MB_TILE, lo.y + tile.y);
	ivec2 step  = ivec2(1, 0);
  #else
	ivec2 first = ivec2(tile.x, tile.y * MB_TILE);
	ivec2 step  = ivec2(0, 1);
  #endif
	for (int i = 0; i < MB_TILE; i++)
	{
		Take(min(first + step * i, hi));
	}
#endif

	gl_FragColor = vec4(best, shortest, weapon);
}
