# How comfygrass works

Development notes: what the client does, what was reverse-engineered from it, and what did not work. To install and use comfygrass, see the [README one level up](../README.md).

comfygrass makes grass sway in the wind in a Direct3D 9 vanilla client. It was inspired by [wxl-grasswind](https://github.com/WarcraftXL/wxl-grasswind) (WotLK), but uses a different technique: wxl-grasswind's approach has nothing to attach to on 1.12.

**Status: working.** Grass bends from the root and parts around the player. Trees and doodads do not move. Colour and fog match the stock rendering. Cost: **~0.18 ms/frame for ~1.2M vertices across ~340 draws** at `frillDensity` 32 (~0.06 ms for ~460k vertices at a lower density).

## Why this is not a port of wxl-grasswind

wxl-grasswind detours the vertex-shader create entry, recognises the stock detail-doodad shader by blob length, disassembles it, splices a wind block in, and reassembles it. Step two has nothing to find here: **the 1.12 client ships exactly five vertex shaders**, and none is grass:

```
Shaders\Vertex\patter.bls   rain.bls   sand.bls   snowpoint.bls   shaders\vertex\Model2.bls
```

`Model2.bls` drives *every* M2 in the world, so changing it would make characters sway too.

Grass here is the "detail doodad" / "frill" system. The binary still describes it:

| Evidence in `WoW.exe` | Meaning |
| --- | --- |
| `E:\build\buildWoW\WoW\Source\WorldClient\DetailDoodad.cpp` | its own translation unit |
| RTTI `CDetailDoodadData`, `CDetailDoodadInst` | an instance pool |
| alloc tags `CDetailDoodad_vtx`, `CDetailDoodad_idx` | CPU-built vertex + index buffers |
| CVar `frillDensity` (1–256), global `0x00C7B494` | the density dial |
| setter `0x006725A0` → `0x006B1D20`, rebuild `0x006B1D30` | `N = min(frillDensity × 64, 8192)` |

**This client renders through Direct3D 9, not D3D8.** `WoW.exe` contains the strings `d3d9.dll` and `Direct3DCreate9` and loads `d3d9.dll` at run time; it does not import it. The strings `Direct3DCreate8` and `d3d8.dll` do not appear. `d3d9.dll` in the client root is DXVK.

## How it works

VanillaFixes loads `comfygrass.dll` from `dlls.txt`, like the client's other mods. The DLL exports nothing, and the client never links to it. It attaches by patching a few `IDirect3DDevice9` vtable slots.

It gets the vtable from a device of its own. Every `IDirect3DDevice9` from DXVK shares one class vtable, so patching a slot in place catches the client's device in any order, including a device created before comfygrass loaded. The throwaway device only names the vtable and is released a millisecond later. The attach takes ~250 ms, once, on a background thread.

**Why not intercept the client's own D3D9 setup?** There is nothing to intercept. This `WoW.exe` does not import `d3d9.dll`. Its D3D9 path was patched in, and it imports neither `Direct3DCreate9` nor `GetProcAddress`, so there is no import-table entry to redirect.

**Why not a `d3d9.dll` proxy?** That was the first design, and it worked. But it meant renaming DXVK's `d3d9.dll` and forwarding its other 15 exports: invasive, fragile across DXVK updates, and unlike how the client's other mods load. It also needed the module pinned. The client loads and frees `d3d9.dll` more than once at start-up, and after the reload our globals were fresh while DXVK's vtable still held the stale hook.

**In place, never a copy.** A copied vtable drops the RTTI word DXVK keeps behind `vtable[0]`. The client then gives up before it creates its device. It shows a black window, with no error and no crash.

For grass draws only, comfygrass binds a `vs_2_0`, and the GPU does the displacement. **The input is the client's own vertex buffer. Nothing is read back or copied.** D3D9 allows a programmable vertex shader with the fixed-function *pixel* pipeline, so only vertex work is reproduced: transform, lighting, fog, texcoords.

### Identifying grass

`stride 36` + `vs = 0` + **identity world rotation**. Grass is batched per map chunk, so its world matrix is a pure translation. Tree and bush instances share the vertex format but have a real rotation. Without the rotation test, the wind bends trees too.

Vertex layout: `POSITION f3 @0, NORMAL f3 @12, COLOR @24, TEXCOORD0 f2 @28`.

### The bend weight

`uv.y` runs from tip to base (the signal wxl uses), **but only across a slice of the atlas**. The span differs per grass texture:

```
0.0000 .. 1.0000    0.0000 .. 0.5000    0.1057 .. 0.3961
0.0007 .. 0.2456    0.3929 .. 1.0000    0.0005 .. 0.3836
```

So a bare `1 - uv.y` gives ~0.9 at the base instead of 0, and blades slide instead of hinging. comfygrass measures each grass texture's span once from a small sample. The shader normalises `uv.y` with it, then applies `saturate((tip - anchor)/(1 - anchor))²`.

`anchor` defaults to **0.70** because grass quads are **sunk into the terrain**. The visible root is well above the quad's base, so the ramp must start higher than expected.

## Building

VS 2022 and CMake. **32-bit only**, because the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```

`comfygrass.dll` is written to the project root, next to `comfygrass.ini`, so the two files a player needs are together. To install, copy them into the client and add one line to `dlls.txt`; see the README one level up.

**F9** captures one frame of draw calls to `comfygrass.log`. **F10** reloads `comfygrass.ini` and toggles the effect; press it twice to reload and stay on. All tunables are live.

## What did not work, and why

Four architectures were tried before the shader. The three CPU designs failed on the same fact, which cannot be seen until it is measured:

**The grass vertex buffer is created `WRITEONLY` and maps to uncached memory. Reading it back runs at ~20 MB/s regardless.** The cause is the kind of memory, not bandwidth or the algorithm.

1. **Per-draw readback:** ~185 locks/frame, 141 ms.
2. **One snapshot per frame:** cheaper, but the client fills and draws *progressively*, so every draw after the first read a stale snapshot. This caused the tearing.
3. **Mirror the client's writes** (hook `Lock`/`Unlock`): correct and tear-free, but still 45–66 ms, because the mirror reads from the same mapped memory. `MOVNTDQA` streaming loads barely helped, which proved the diagnosis.

Writing into the client's buffer is also unsafe. It is a shared arena that the client re-packs, so cached byte offsets soon point at different geometry.

Two measurement lessons:

- A `verifyFrames` check reported the buffer "STATIC" and was a false green. It only proved that the client leaves a range alone *while standing still*.
- Per-stage timers once attributed ~45 ms to a mirror whose own `QueryPerformanceFrequency` calls were a large part of the cost.

## Parting: finding the player

Blades lean away from the player. The shader needs the player in the grass's own space, which is camera-relative (below). So the value needed is `playerPos - cameraPos`, not either one alone. Folding the draw's world translation in on the CPU leaves the shader one subtraction against `i.pos` and two extra constants.

The published 1.12.1 offset lists do not fit this binary: their camera pointer `0x00B7436C` is not referenced in it. Everything below came from disassembling this `WoW.exe`.

| Address | What it is | How it was found |
| --- | --- | --- |
| `0x00C7CF20` | camera world position | `0x00680BC0` stores its two arguments here and at `0x00C7D118`, then computes `target - position` for the view direction |
| `0x00C7D118` | camera aim point, one yard out | measured `|target - camera| = 1.00`, so it is the forward **vector**, not the player |
| `0x00B41414` | object manager | `0x00468550` returns the active player GUID from `[mgr+0xC0]`; list head `[mgr+0xAC]`, link `[mgr+0xA4]` so `next = *(obj+link+4)`, GUID `[obj+0x30]` |
| `+0x9B8` | position inside the player object | not in the disassembly; found by geometry, below |

### Two checks that cannot be fooled

Neither unknown was taken on trust: a wrong address here looks plausible for a long time.

**The camera.** If `0x00C7CF20` is the camera, then `world[3] + camera` is a chunk's true world origin, and map chunks sit on a 100/3 yard grid. The residual must then be the same small number from any chunk, wherever you stand. A wrong address makes it wander as you walk. Measured: `0.001` of `33.333`, with origins on exact multiples (`-9933.33`, `-66.67`).

**The player.** A camera that orbits the player keeps the player in the vertical plane through the camera's forward vector, at any zoom or pitch. So `(player - camera).xy` is parallel to `forward.xy`. Pitch only changes Z, so the test ignores Z and holds at every camera angle. The DLL scans the object for float3s that pass the test, and keeps only those that pass on *every* frame of a 90-frame window while you walk and turn. Two survived: `0x9B8` and `0x9EC`. The second is almost certainly the server-side destination. `0x9B8` matches the public references for 1.12.1 unit position, found here without them.

Set `playerPosOff = 0` in the ini to run the search again on a different build. It logs what it finds.

### The lean itself

Same shape as wxl's: a radial push away from the player, `forceCenter` at the unit falling to `forceEdge` at `radius`. Two differences:

- It is multiplied by the **same blade weight the wind uses**, so the roots stay planted. wxl builds a cone up from the player's feet, which fails on grass quads sunk into the terrain.
- The vertical gate is a loose symmetric `zRange`/`zFade` band, not a cone. It only stops grass on a ledge above or below from reacting.

## The client renders camera-relative

Its view matrix has **no translation** (`view[3] = 0,0,0,1`). The camera position is folded into every draw's world matrix, so `world[3]` is `chunkOrigin - cameraPos` and changes whenever you move.

Using that for the wave phase made every blade jump to a new point in the wave as the camera moved. So by default (`worldPhase = 0`), the phase and per-blade jitter come from chunk-local vertex positions, which are stable.

The trade-off: the wind pattern repeats per chunk instead of running across the world, and adjacent chunks can show a phase seam. A `wavelength` above the chunk size (~33 yards) hides it. A proper fix adds the camera's world position to the phase. The D3D matrices do not contain it, but comfygrass already reads it from the client for parting (`0x00C7CF20`, above). The wave phase does not use it yet.

## Known gaps

- **Lighting follows the fixed-function equation:** `emissive + ambientMat × (D3DRS_AMBIENT + Σ light.Ambient) + diffuseMat × sun × N·L`, with the material sources, `COLORVERTEX` and `LIGHTING` read live from the device. Only the first directional light adds diffuse; point and spot lights are ignored. An earlier version used `(ambient + sun × N·L) × vertexColour`, which dropped the lights' Ambient term and the material and made the grass darker than stock. The state is logged once as `grass lighting:`.
- **Fog uses `pos.w`**; range fog would differ.
- **Wind repeats per chunk.** comfygrass already reads the camera position for parting, but the wave phase does not use it yet (above).
- **One device.** The hooks are in DXVK's shared device vtable, so every device goes through them, but comfygrass tracks the state of only one device.
- **No distance attenuation.** The CPU path had a `distanceFade`. The shader does not, so the setting was removed from the ini.
- The camera and object-manager addresses are for **this** `WoW.exe`. Another build moves them. The ini exposes all of them, and `playerPosOff = 0` runs the search for the last one again.
