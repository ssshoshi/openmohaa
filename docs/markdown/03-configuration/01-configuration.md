# Configuration and commands

## General configuration

This documentation currently only lists new changes that were introduced in OpenMoHAA. For a list of known settings, see [Server configuration](02-configuration-server.md).

If you want to use containers, see [Creating a Docker image](../02-running/04-docker.md).

### Home directory

OpenMoHAA uses a dedicated home directory by default for user data and mods. This behavior can be customized:

- `set fs_homepath Z:\openmohaa_data`: User data will be read and written in the directory located in `Z:\openmohaa_data`
- `set fs_homepath homedata`: The subdirectory `homedata` in the game directory will be used to read and store user data
- `set fs_homepath .`: Not recommended, the game directory will be used for storing user data, just like the original MOH:AA

#### Default paths by OS:

- Windows: `%APPDATA%\openmohaa`
- Linux: `~/.openmohaa`
- macOS: `~/Library/Application Support/openmohaa`

### Configure the network components

Network settings can be adjusted to use either IPv4, IPv6, or both. By default, IPv6 is disabled on dedicated servers.

- `set net_enabled 0`: Disable networking.
- `set net_enabled 1`: Enable IPv4 only (the default for dedicated servers).
- `set net_enabled 2`: Enable IPv6 only.
- `set net_enabled 3`: Enable both IPv4 and IPv6 (the default when running the standalone game).

> [!WARNING]
> The master server (using the GameSpy protocol) does not support IPv6. If IPv4 is disabled, the server won't show up in the public server list.

### Flood protection differences with MOH: Spearhead

Flood protection is turned off by default in OpenMoHAA (`sv_floodProtection 0`).

- In MOH: Allied Assault and OpenMoHAA, it monitors all commands.
- In MOH: Spearhead 2.0 and later, it monitors only text messages.

Flood protection prevents spam but can sometimes interfere with rapid actions like reloading and checking scores within a short period of time. It can be disabled with `set sv_floodProtection 0`.

For more details on preventing message spamming, check out the [Chat](#chat) section below.

### Updates

The game periodically checks for new versions in the GitHub project page in the background. Updates are not applied automatically, they must be downloaded and installed manually.

Update checking is enabled by default, but can be disabled with:
- `set net_enabled 0`, disables networking as mentioned aboe
- `set com_updatechecker_enabled 0`
- Compiling the project without libcurl support

If disabled, remember to check the project page for new versions. Updates can improve security and provide important fixes against exploits.

## Graphics

### Choosing a renderer

OpenMoHAA ships the original OpenGL 1 renderer and, optionally, an OpenGL 2 renderer derived
from [ioquake3](https://ioquake3.org/). The renderer is selected with `cl_renderer`, which is
latched, so it takes effect after a `vid_restart`:

```cpp
set cl_renderer opengl1 // The default, and the only renderer built by default
set cl_renderer opengl2 // Experimental
vid_restart
```

If the requested renderer cannot be loaded, the game falls back to the default one rather than
failing to start.

The renderer only affects how the game is drawn on your machine. It is not visible to servers
and has no effect on gameplay, so it can be changed freely while playing online.

### The OpenGL 2 renderer

> [!WARNING]
> The OpenGL 2 renderer is a work in progress and is **not** built by default. It does not yet
> render all MOH:AA content correctly. Use `opengl1` if you want the reference behaviour.

To build it, configure with `-DBUILD_RENDERER_GL2=ON`. It requires `-DUSE_RENDERER_DLOPEN=ON`
unless the OpenGL 1 renderer is disabled, because a statically linked build compiles the
renderer directly into the client and only one can be linked at a time.

It adds the modern rendering features from ioquake3's OpenGL 2 renderer, all of which are
disabled or conservative by default. The settings below are latched unless noted otherwise:

- `set r_ext_framebuffer_multisample x`: Multisample anti-aliasing, `0` (off) to `16`.
- `set r_ext_compressed_textures x`: `0` none, `1` DXT/RGTC, `2` BPTC. Reduces video memory use.
- `set r_hdr 1`: Render the scene in high dynamic range, which reduces colour banding.
- `set r_toneMap 1` / `set r_autoExposure 1`: Tone mapping and automatic exposure. Both require
  `r_hdr` and `r_postProcess`, and neither is latched.
- `set r_normalMapping 1` / `set r_specularMapping 1`: Use normal and specular maps for
  materials that provide them. Enabled by default when the renderer is built.
- `set r_parallaxMapping x`: `0` off, `1` parallax occlusion mapping, `2` relief mapping.
- `set r_sunShadows 1`: Sunlight and cascaded shadow maps, tuned with `r_shadowMapSize` and
  `r_shadowFilter`. The sun is taken from the map's own `worldspawn` (`sundirection` and
  `suncolor`/`sunlight`), so this works on stock maps with no modified assets.
- `set r_sunShadowScale x`: How much a shadowed surface is darkened, `0.85` by default. MOH:AA
  already bakes the sun into its lightmaps, so shadows only need to suggest themselves; lower
  values (ioq3 uses `0.5`) give stronger, darker shadows.
- `set r_ssao 1`: Screen-space ambient occlusion. Costs performance.
- `set r_drawSunRays 1`: Light shafts from the sun. Needs `r_sunShadows`.
- `set r_genNormalMaps 1`: Derive rough normal maps from the diffuse textures, for surfaces
  that have no authored `_n` image. A fallback, not a substitute for real normal maps.
- `set r_cubeMapping 1`: Image-based reflections. Needs cubemaps generated per map, and does
  nothing without them.
- `set r_imageUpsample x`: Interpolate textures to a higher resolution, `0` off, `1` 2x, `2` 4x.

Normal and specular maps are picked up automatically: for a texture `foo.jpg`, the renderer
looks for `foo_n` (normal map), `foo_nh` (normal map with height in the alpha channel, for
parallax mapping) and `foo_s` (specular map). Because these are ordinary extra files, they can
be shipped in a separate pk3 without modifying any stock game content. `tools/matgen` generates
such a pk3 from the installed textures.

Materials can also be described explicitly in `.mtr` files, which live alongside `.shader`
files in `scripts/` and use the same syntax. A `.mtr` file replaces the `.shader` file of the
same name, but *only* when the OpenGL 2 renderer is active, so adding one cannot change how the
game looks under OpenGL 1.

Inside a `.mtr`, a stage can say what its image is for:

```cpp
textures/example/wall
{
    {
        map textures/example/wall.jpg
    }
    {
        stage normalmap          // or normalparallaxmap, if alpha holds height
        map textures/example/wall_n.jpg
        normalScale 1 1          // strength; negative values flip an axis
    }
    {
        stage specularmap
        map textures/example/wall_s.jpg
        specularReflectance 0.04 // how metallic, 0.04 suits most materials
        specularExponent 16      // how sharp the highlight is
    }
    {
        map $lightmap
        blendfunc GL_DST_COLOR GL_ZERO
    }
}
```

Normal and specular maps affect the diffuse stage declared before them, so a surface that
blends two diffuse layers can give each its own.

Note that a texture only picks up normal or specular mapping if its shader is lit -- it needs a
lightmap, or one of the vertex lit `rgbGen` modes. Fullbright and purely additive effect
shaders are drawn as they always were.

## Server configuration

### Optimization / Antichams

A new variable, `sv_netoptimize`, enables a feature that optimizes network bandwidth by not sending players information about others they can't see. For each client, the server optimizes by only transmitting data about players within their view. Clients will not receive information about players they can't see. This feature also helps protect against cheaters:

- `set sv_netoptimize 0`: Disable optimization - the default
- `set sv_netoptimize 1`: Enable optimization for entities that are moving
- `set sv_netoptimize 2`: Enable optimization, always

This option exists since **Medal of Honor: Allied Assault Breakthrough** 2.30, however it was improved in OpenMoHAA: sounds like footsteps will be sent so players don't get confused.

### Managing bans

Thanks to the [ioquake3](https://ioquake3.org/) project, IP bans are supported. Bans are saved in `serverbans.dat` by default, (modifiable with `sv_banFile` varaiable):

|Name       |Parameters                                      |Description
|-----------|------------------------------------------------|-----------
|rehashbans |                                                |Loads saved bans from the banlist file
|listbans   |                                                |Lists all banned IP addresses
|banaddr    |ip[*/subnet*] \| clientnum [*subnet*] [reason]  |Bans an IP through its address or through a client number, a subnet can be specified to ban a network range
|exceptaddr |ip[*/subnet*] \| clientnum [*subnet*]           |Adds an IP as an exception, for example IP ranges can be banned but one or more exceptions can be added
|bandel     |ip[*/subnet*] \| num                            |Unbans an IP address or a subnet, the entry number can be specified as an alternative
|exceptdel  |ip[*/subnet*] \| num                            |Removes a ban exception
|flushbans  |                                                |Removes all bans

Examples:

- `banaddr 192.168.5.2` bans IP address **192.168.5.2**.
- `banaddr 192.168.1.0/24` bans all **192.168.1.x** IP addresses (in the range **192.168.1.0**-**192.168.1.255**).
- `banaddr 2` bans the IP address of the client **#2**.
- `banaddr 4 24` bans the subnet of client **#4** - i.e if client .**#4** has IP **192.168.8.4**, then it bans all IPs ranging from **192.168.8.0**-**192.168.8.255**.
- `exceptaddr 3` ads the IP of client **#3** as an exception.
- `bandel 192.168.8.4` unbans **192.168.8.4**.
- `bandel 192.168.1.0/24` unbans the entire **192.168.1.0** subnet (IP ranging from **192.168.1.0**-**192.168.1.255**).

To calculate IP subnets, search for `IP subnet calculator` on Internet.

## Game settings

### Physics

Props are rigid bodies (Jolt Physics) that bullets, explosions and falling bodies knock about.

- Entity props (crates, barrels and cans, magazines) are simulated by the server, so every client sees them move.
- The map's static-model clutter and furniture, and furniture built from world brushes (tables, benches, crates), are simulated by the client. Sides a prop was built without (nodraw or caulk, never meant to be seen) are covered with its own texture once it moves. That is cosmetic, except in single player: there the clip brushes that stood in for a prop are removed from collision once it moves.

|Name                 |Default|Description
|---------------------|-------|-----------
|g_physics            |1      |Server props are physics bodies (takes effect on map load)
|g_physics_log        |0      |1 reports the server world and body counts, 2 also each prop and hit
|g_physics_hitscale   |1      |Multiplies how hard hits shove server props
|cg_physics           |1      |Client physics on or off
|cg_physics_props     |1      |Small static models move (takes effect on map load)
|cg_physics_furniture |1      |Furniture built from world brushes moves (takes effect on map load)
|cg_physics_clipped   |0      |Props wrapped in clip brushes move even when the clip brushes cannot be removed (multiplayer); the clip brushes then stay where the prop was
|cg_physics_log       |0      |1 reports load and step costs, 2 also blasts, hits and each piece of furniture
|cg_physics_debug     |0      |1 draws the physics world's shapes near the view; 3 draws every brush prop turned over where it stands, with the sides it was built without covered (4: without the covers), to check them

Walking into a prop pushes it, lighter props faster and nothing over 80 kg; AI push props too. With `cg_ragdoll_grab 1` the grabber (`+rdgrab`, `rdpunt`) carries and throws props as well as bodies.

The console commands `phys_poke`, `phys_blast`, `phys_list` and `phys_selftest` are for testing.

### Chat

Chat messages are logged to console and in the logfile by default, without requiring to set the `developer` variable.

The in-game chat behavior can be adjusted:

- `set g_instamsg_allowed 0`: Disable voice instant messages.
- `set g_instamsg_minDelay x`: Minimum delay (ms) between voice messages (default 1000)
- `set g_textmsg_allowed 0`: Disable all text messages. `All`, `team` and `private` messages will be disabled.
- `set g_textmsg_minDelay x`: Minimum delay (ms) between text messages (default 1000)

Temporarily disabling text messages can be useful in situations where tensions arise in the chat. Otherwise, it's best to keep them enabled under normal circumstances.

### Balancing teams

This prevents players from joining teams with more players than others. Disabled by default.

It can be enabled with: `set g_teambalance 1`.

This feature is passive: it only checks the team sizes when someone tries to join, so it won't automatically balance teams during the game.

> [!NOTE]
> This check doesn't apply in server scripts; it only works when clients join teams directly.

### Bots

OpenMoHAA introduced multiplayer bots which can be used for entertainment or for testing purposes. They appear in the scoreboard with their ping set to **bot**.

> [!NOTE]
> Bots work best on maps without dynamic objects. Currently, they have difficulty getting around obstacles such as vehicles placed in the middle of maps.

Configure bots with the following variables:

- `set sv_maxbots x`: **Required**, max number of bots allowed. The game can only handle a total of 64 players (clients), it will be limited to 64 minus the number of real players (`sv_maxclients`). For example, if you set `sv_maxclients` to 48, the maximum number of bots (sv_maxbots) can be 16.
- `set sv_numbots x`: Number of bots to spawn (capped at `sv_maxbots`).
- `set sv_minPlayers x`: Configure the minimum number of players required. If the number of real players in a team is below the specified value, the game will automatically add bots to fill the gap. For example, if `sv_minPlayers` is set to 8 and only 5 real players are active, the game will spawn 3 bots to make sure there are always 8 players in the game.

For more settings, see this [documentation](./03-configuration-bots.md).

Bots can be spawned with a name, by setting `g_botx_name` variables where `x` is the bot number:

```cpp
set g_bot0_name customname // The first bot spawned will be named customname
set g_bot1_name "Fast beat" // The second bot spawned will be named Fast beat
```

Bots will keep their name between restarts and new maps.

Example with the requirement of 6 players:
```cpp
set sv_maxbots 16 // Reserve 16 slots for bots
set sv_minPlayers 6 // Ensure each team has at least 6 players (bots are added if there are fewer players active)
```

Example with 4 bots playing:
```cpp
set sv_maxbots 16 // Reserve 16 slots for bots
set sv_numbots 4 // Spawn 4 bots
```

> [!NOTE]
> Bots have their ping set to **bot** in the scoreboard to avoid confusion with human or cheaters.
> 
> Since OpenMoHAA 0.82.0, the navigation path is generated automatically using [Recast](https://recastnav.com/) for any map, including custom maps.
> If the Recast-based navigation system is not working correctly or if you are running a version below 0.82.0:
> 1. Get the [mp-navigation](https://github.com/openmoh/mp-navigation) pk3 (it only covers stock maps) and place it inside your game's `main` folder.
> 2. Append `set g_navigation_legacy 1` somewhere, like in your `server.cfg` file.

#### Known issues with bots

- Bots may not properly detect or avoid minefields.
- Bots won't complete objectives. They only navigate the map and attack other players.
- Minefields may fully block bot paths. For example, on Omaha Beach, bots spawning at the West axis spawn may get stuck in the spawn area.
- Some obstacles might completely block bot paths.
