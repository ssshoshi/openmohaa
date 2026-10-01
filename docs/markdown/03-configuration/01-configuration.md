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

### Running console commands from another program

`set com_cmddir <dir>` makes the game watch a folder in the home directory (for example `orch/cmd`, which is `main/orch/cmd` under the home path) for command files, on every platform:

- Each `<name>.txt` file is read, deleted, and run at once, one command per line or separated with `;`. A running `wait` in a config doesn't hold the commands back; `wait` lines in the file itself are ignored.
- The console output of the commands is written to `<name>.out` next to it.
- Write the file under another name first and rename it to `.txt` when it's complete, so the game never reads half of it.

The Unix-only `com_pipefile` works the other way: it queues the commands behind anything already waiting.

### Live orchestrator

The orchestrator lets an agent listen to the player while they play and change the game as it runs (see `tools/orchestrator/README.md` in the source). In game:

- `orch [on|off]` (bound to `F10` on the first run): orchestrator mode.
- `orch_shot` (bound to `MOUSE3`): a screenshot plus a description of what is under the crosshair, written to `orch/events/` in the home directory. In single player it also saves the game as `orch_<id>`; `set orch_shotsave 0` turns that off.
- `orch_freeze [here]` (bound to `F11`): single player only. Pauses the world and lets the player walk, or fly with `orch_fly` (`N`), through it. Run it again to resume where the player was, or with `here` to move the player to the camera first. `orch_return` moves the player back to where they last froze. `set orch_flyspeed 2` sets how fast flying is.
- `orch_msg [-heard] <text>`, `orch_status <state>`, `orch_state`: used by the tools to show replies and read the player's position.

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

- Entity props (crates, barrels and cans, magazines, helmets shot off) are simulated by the server, so every client sees them move. In single player so are the weapons, ammunition and health lying about, placed or dropped; in multiplayer those stay where the game puts them.
- The map's static-model clutter and furniture, and furniture built from world brushes (tables, benches, crates), are simulated by the client. Sides a prop was built without (nodraw or caulk, never meant to be seen) are covered with its own texture once it moves. That is cosmetic, except in single player: there the clip brushes that stood in for a prop are removed from collision once it moves.

|Name                 |Default|Description
|---------------------|-------|-----------
|g_physics            |1      |Server props are physics bodies (takes effect on map load)
|g_physics_log        |0      |1 reports the server world and body counts, 2 also each prop and hit, 3 also every round's path
|g_physics_hitscale   |1      |Multiplies how hard hits shove server props
|cg_physics           |1      |Client physics on or off
|cg_physics_props     |1      |Small static models move (takes effect on map load)
|cg_physics_furniture |1      |Furniture built from world brushes moves (takes effect on map load)
|cg_physics_clipped   |0      |Props wrapped in clip brushes move even when the clip brushes cannot be removed (multiplayer); the clip brushes then stay where the prop was
|cg_physics_log       |0      |1 reports load and step costs, 2 also blasts, hits and each piece of furniture
|cg_ragdoll_solver    |0      |0: corpses are the particle ragdoll. 1: once the blend out of the death animation ends, a Jolt ragdoll (rigid capsules, hinged knees and elbows, a man's range at the hips and shoulders) carries the body, colliding with the map, props, people and other corpses
|cg_physics_debug     |0      |1 draws the physics world's shapes near the view; 3 draws every brush prop turned over where it stands, with the sides it was built without covered (4: without the covers), to check them

Walking into a prop pushes it, lighter props faster and nothing over 80 kg; AI push props too. The client's props and the server's collide: crates and barrels push chairs and bottles, and in single player a chair thrown into a crate shoves it. With `cg_ragdoll_grab 1` the grabber (`+rdgrab`, `rdpunt`) carries and throws props as well as bodies, in single player the server's crates, barrels and magazines too.

The console commands `phys_poke`, `phys_blast`, `phys_list` and `phys_selftest` are for testing.

#### Choosing what moves: physics.txt and the physics editor

Which objects are physics bodies is guessed: small static models move, big ones stay fixed, foliage, lights and wire are left out, and so on. Where a guess is wrong, `physics.txt` in the game's home directory (`main/physics.txt`) overrides it. Both the client and the server read it at every map load.

`phys_edit` turns on the in-game editor. Every physics object near the view is outlined: green if it moves, red if it is fixed, grey if it is left out. The one under the crosshair is yellow, and the screen says what it is, how heavy it is and what decided it. Then:

|Command                          |What it does
|---------------------------------|------------
|phys_toggle [model\|mapmodel]    |Moves if it was fixed or left out, fixed if it moved
|phys_off [model\|mapmodel]       |Left out of the physics altogether, not even solid to ragdolls (the server's objects: the same as fixed)
|phys_mass <kg> [model\|mapmodel] |How heavy it is when it moves; 0 goes back to the guess
|phys_forget [model\|mapmodel]    |Takes the rule out
|phys_reload                      |Reads `physics.txt` again, after editing it by hand

With no argument a command is about the object alone. With `model` it applies to every object with the same model (for a brush entity such as a crate, every entity of its class) on every map, and with `mapmodel` on this map only. A rule for one object wins over a rule for its model. Each change is saved to `physics.txt` straight away, and the physics is rebuilt: the client's props and furniture go back where the map put them. The server's crates, barrels and magazines can be edited in single player only. Binding the commands to keys makes the editor quicker to use, e.g. `bind j phys_toggle` and `bind k "phys_toggle model"`.

The file can be edited by hand. Each line is an optional map, what the rule is about, and settings (`moves`, `fixed`, `off`, `mass <kg>`); `//` or `#` starts a comment:

```
model static/chair.tik fixed           every static/chair.tik, on every map
m3l1b model static/chair.tik moves     but on m3l1b they move
m3l1b static 57 moves mass 12          the 57th static model on m3l1b
m3l1b furniture 712 fixed              the brushwork furniture that starts with brush 712
m3l1b entity *185 fixed                the brush entity with model *185
class func_barrel mass 30              every barrel weighs 30 kg
```

### Player

|Name     |Default|Description
|---------|-------|-----------
|g_splean |1      |Lean (the lean left and right keys) in single player too, which Allied Assault does not allow; 0 keeps the original game's no lean. Multiplayer and the expansions are unchanged

### Enemy AI

Single-player enemies get behaviour the original game's AI lacks. Each part has its own switch, and `ai_enhanced 0` turns them all off, for the original behaviour.

|Name               |Default|Description
|-------------------|-------|-----------
|ai_enhanced        |1      |All the AI improvements below on or off; also, as in Spearhead and Breakthrough, enemies only hit what is roughly where their gun points (`g_aimaxdeviation`), not something well off to the side
|ai_suppress        |1      |Enemies who lose sight of you fire at where they last saw you for a while (as in Spearhead and Breakthrough), rather than at once going quiet; and they hold fire when a squadmate is in the way
|ai_suppress_chance |50     |Percent chance an enemy suppresses when it could (takes effect for enemies spawned after it is set)
|ai_grenades        |1      |Grenade improvements: Germans whose map gives them no grenades carry `ai_grenade_ammo`; a throw can lob or land beside the target when a plain throw cannot reach (up through a window that has been shot out, under a ceiling); they hold the grenade in their hand as they wind up; throws are not perfect; and an actor with squadmates throws at all (the restored code compared a distance with its square, so none ever did)
|ai_grenade_ammo    |1      |Grenades a German carries when his map gives him none (takes effect for enemies spawned after it is set)
|ai_grenade_range   |1400   |Farthest an enemy throws
|ai_grenade_cooldown|6      |Seconds a squad waits after one of them throws before another does
|ai_grenade_fumble  |8      |Percent of throws that go badly wrong: the grenade slips and lands a few yards ahead, falls short, or goes wide (enemies get hurt by their own grenades)
|ai_grenade_drop    |1      |An enemy killed while winding up drops the grenade, live
|ai_debug           |0      |1 logs each AI decision to the console, 2 also why a grenade is not thrown

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
