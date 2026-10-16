// The cords lamps hang from (CG_PhysicsDrawCords, cg_physics_props.cpp): a
// strip the client builds along each cord every frame, near black and unlit,
// as a dark flex is at the size it shows.
opm/lampcord
{
	surfaceparm nonsolid
	surfaceparm nomarks
	cull none
	nopicmip
	{
		map $whiteimage
		rgbGen const ( 0.03 0.03 0.03 )
	}
}
