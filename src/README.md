# How comfygrass works

The development notes: what the client does, what was reverse-engineered out of it, and what did not work. For installing and using it, see the [README one level up](../README.md).

Grass that sways in the wind, for a Direct3D 9 vanilla client. Inspired by [wxl-grasswind](https://github.com/WarcraftXL/wxl-grasswind) (WotLK), but the technique is entirely different. That module's approach has nothing to attach to on 1.12.

**Status: working.** Grass bends from the root and parts around the player, trees and doodads are untouched, colour and fog match the stock rendering, and it costs **~0.18 ms/frame for ~1.2M vertices across ~340 draws** (measured at `frillDensity` 32; it was ~0.06 ms for ~460k vertices at lower density).

## Why this is not a port of wxl-grasswind

wxl-grasswind detours the vertex-shader create entry, recognises the stock detail-doodad shader by blob length, disassembles it, splices a wind block in, and reassembles. Step two has nothing to latch onto here: **the 1.12 client ships exactly five vertex shaders**, and none is grass:

```
Shaders\Vertex\patter.bls   rain.bls   sand.bls   snowpoint.bls   shaders\vertex\Model2.bls
```

`Model2.bls` drives *every* M2 in the world, so hijacking it would make characters sway too.

Grass here is the "detail doodad" / "frill" system, and the binary still describes it:

| Evidence in `WoW.exe` | Meaning |
| --- | --- |
| `E:\build\buildWoW\WoW\Source\WorldClient\DetailDoodad.cpp` | its own translation unit |
| RTTI `CDetailDoodadData`, `CDetailDoodadInst` | an instance pool |
| alloc tags `CDetailDoodad_vtx`, `CDetailDoodad_idx` | CPU-built vertex + index buffers |
| CVar `frillDensity` (1–256), global `0x00C7B494` | the density dial |
| setter `0x006725A0` → `0x006B1D20`, rebuild `0x006B1D30` | `N = min(frillDensity × 64, 8192)` |

Also note: **this client renders through Direct3D 9, not D3D8.** `WoW.exe` imports `d3d9.dll` and calls `Direct3DCreate9`; the string `Direct3DCreate8` does not appear at all. `d3d9.dll` in the client root is DXVK.

## How it works

`comfygrass.dll` is loaded by VanillaFixes from `dlls.txt`, like the client's other mods. It exports nothing and the client never links to it; it attaches by patching a handful of `IDirect3DDevice9` vtable slots.

It gets that vtable from a device of its own. Every `IDirect3DDevice9` DXVK hands out shares one class vtable, so patching a slot in place catches the client's device whatever order things happen in, including one created before we loaded. The throwaway device exists only to name the vtable and is released a millisecond later; the whole attach takes ~250 ms, once, on a background thread.

**Why not intercept the client's own D3D9 setup?** There is nothing to intercept. This `WoW.exe` does not import `d3d9.dll` at all. Its D3D9 path was patched in, and it imports neither `Direct3DCreate9` nor even `GetProcAddress`, so there is no import-table entry to redirect.

**Why not a `d3d9.dll` proxy?** That was the original design and it worked, but it meant renaming DXVK's `d3d9.dll` out of the way and forwarding its other 15 exports. That was invasive, fragile across DXVK updates, and out of step with how every other mod in this client loads. It also needed the module pinned, because the client loads and frees `d3d9.dll` more than once during start-up and the reload left our globals fresh while DXVK's vtable still held the stale hook.

**In place, never a copy.** Handing back a copied vtable drops the RTTI word DXVK keeps behind `vtable[0]`, and the client silently gives up before creating its device. There is no error and no crash, just a black window.

For grass draws only, it binds a `vs_2_0` and lets the GPU do the displacement. **The client's own vertex buffer is the input. Nothing is read back, and nothing is copied.** D3D9 permits a programmable vertex shader alongside the fixed-function *pixel* pipeline, so only vertex work is reproduced: transform, lighting, fog, texcoords.

### Identifying grass

`stride 36` + `vs = 0` + **identity world rotation**. Grass is batched per map chunk so its world matrix is a pure translation; tree and bush instances share the vertex format but carry a real rotation. Without the rotation test the wind bends trees too.

Vertex layout: `POSITION f3 @0, NORMAL f3 @12, COLOR @24, TEXCOORD0 f2 @28`.

### The bend weight

`uv.y` runs from tip to base (the same signal wxl uses), **but only across a slice of the atlas**. Measured spans vary wildly per grass texture:

```
0.0000 .. 1.0000    0.0000 .. 0.5000    0.1057 .. 0.3961
0.0007 .. 0.2456    0.3929 .. 1.0000    0.0005 .. 0.3836
```

So a bare `1 - uv.y` yields ~0.9 at the base where it should be 0, and blades slide instead of hinging. Each grass texture's span is measured once from a small sample and passed to the shader, which normalises `uv.y` before applying `saturate((tip - anchor)/(1 - anchor))²`.

`anchor` defaults to **0.70** because grass quads are **sunk into the terrain**. The visible root is well above the quad's base, so the ramp has to start higher than you would guess.

## Building

Needs VS 2022 and CMake. **32-bit only**, because the 1.12 client is x86.

```
cmake -B build -A Win32
cmake --build build --config Release
```

`comfygrass.dll` is written straight to the project root, next to `comfygrass.ini`, so the two files a player needs are always sitting together. Installing is copying them into the client and adding one line to `dlls.txt`; see the README one level up.

**F9** captures one frame of draw calls to `comfygrass.log`. **F10** reloads `comfygrass.ini` and toggles the effect. Press it twice to reload and stay on. Every tunable is live.

## What did not work, and why

Four architectures were tried before the shader. All three CPU ones died on the same fact, which is worth recording because it is invisible until measured:

**The grass vertex buffer is created `WRITEONLY` and maps to uncached memory. Reading it back runs at ~20 MB/s no matter what.** Not a bandwidth problem, not an algorithm problem. It is the kind of memory.

1. **Per-draw readback:** ~185 locks/frame, 141 ms.
2. **One snapshot per frame:** cheaper, but the client fills and draws *progressively*, so every draw after the first read a stale snapshot. This was the tearing.
3. **Mirror the client's writes** (hook `Lock`/`Unlock`): correct and tear-free, but still 45–66 ms, because the mirror reads from the same mapped memory. `MOVNTDQA` streaming loads barely helped, which is what finally proved the diagnosis.

Along the way: writing into the client's buffer at all is unsafe. It is a shared arena the client re-packs, so cached byte offsets soon address different geometry.

Two measurement lessons: a `verifyFrames` check reported the buffer "STATIC" and was a false green (it only proved the client leaves a range alone *while standing still*); and per-stage timers once attributed ~45 ms to a mirror whose own `QueryPerformanceFrequency` calls were a large part of the cost.

## Parting: finding the player

Blades lean away from the player. The shader needs the player in the grass's own space, and that space is camera-relative (below), so the quantity actually wanted is `playerPos - cameraPos`, never either one alone. Folding the draw's world translation in on the CPU leaves the shader one subtraction against `i.pos` and two extra constants.

The published 1.12.1 offset lists do not fit this binary: their camera pointer `0x00B7436C` is not referenced anywhere in it. Everything below came out of disassembling this exact `WoW.exe`.

| Address | What it is | How it was found |
| --- | --- | --- |
| `0x00C7CF20` | camera world position | `0x00680BC0` stores its two arguments here and at `0x00C7D118`, then computes `target - position` for the view direction |
| `0x00C7D118` | camera aim point, one yard out | measured `|target - camera| = 1.00`, so it is the forward **vector**, not the player |
| `0x00B41414` | object manager | `0x00468550` returns the active player GUID from `[mgr+0xC0]`; list head `[mgr+0xAC]`, link `[mgr+0xA4]` so `next = *(obj+link+4)`, GUID `[obj+0x30]` |
| `+0x9B8` | position inside the player object | not in the disassembly; found by geometry, below |

### Two checks that cannot be fooled

Neither of the two unknowns was taken on trust, because a wrong address here looks plausible for a long time before it looks wrong.

**The camera.** If `0x00C7CF20` is really the camera, then `world[3] + camera` is a chunk's true world origin, and map chunks sit on a 100/3 yard grid. So that residual has to be the same tiny number from any chunk, wherever you stand; a wrong address makes it wander as you walk. Measured: `0.001` of `33.333`, with origins landing on exact multiples (`-9933.33`, `-66.67`).

**The player.** Whatever the zoom or the pitch, a camera that orbits the player leaves the player in the vertical plane through the camera's forward vector, so `(player - camera).xy` runs parallel to `forward.xy`. Pitch only moves things in Z, which is why the test ignores Z and stays exact at every camera angle. The DLL scans the object for float3s passing that test and keeps only those that pass on *every* frame of a 90-frame window while you walk and turn. Two survived: `0x9B8` and `0x9EC`. The second is almost certainly the server-side destination, and `0x9B8` is what the public references give for 1.12.1 unit position, arrived at here without consulting them.

Set `playerPosOff = 0` in the ini to re-run that search on a different build; it logs what it finds.

### The lean itself

Same shape as wxl's: radial push away from the player, `forceCenter` at the unit falling to `forceEdge` at `radius`. Two differences. It is multiplied by the **same blade weight the wind uses**, so the roots stay planted. wxl instead builds a cone up from the player's feet, which does not survive grass quads that are sunk into the terrain. And the vertical gate is a loose symmetric `zRange`/`zFade` band rather than that cone, existing only to stop grass on a ledge overhead or below from reacting.

## The client renders camera-relative

Its view matrix has **no translation** (`view[3] = 0,0,0,1`); the camera position is folded into every draw's world matrix, so `world[3]` is `chunkOrigin - cameraPos` and changes whenever you move.

Feeding that into the wave phase made every blade slide to a new point in the wave as the camera moved, so they appeared to jump between states. So by default (`worldPhase = 0`) the phase and per-blade jitter come from chunk-local vertex positions, which are stable.

The trade-off is that the wind pattern repeats per chunk rather than running continuously across the world, and adjacent chunks can show a phase seam. Raising `wavelength` above the chunk size (~33 yards) hides it. A proper fix needs the camera's true world position read out of the client, because it is genuinely not recoverable from the D3D matrices.

## Known gaps

- **Lighting follows the fixed-function equation:** `emissive + ambientMat × (D3DRS_AMBIENT + Σ light.Ambient) + diffuseMat × sun × N·L`, with the material sources, `COLORVERTEX` and `LIGHTING` read live from the device. Only the first directional light contributes diffuse; point and spot lights are ignored. An earlier version used `(ambient + sun × N·L) × vertexColour`. It dropped the lights' Ambient term and the material, and the grass came out visibly darker than stock. The state it sees is logged once as `grass lighting:`.
- **Fog uses `pos.w`**; range fog would differ.
- **Wind repeats per chunk** unless the camera position is read from the client (above).
- **One device**, and hooks install on the first `CreateDevice`.
- **No distance attenuation.** The CPU path had a `distanceFade`; the shader does not, and the setting was removed rather than left in the ini doing nothing.
- The camera and object-manager addresses are for **this** `WoW.exe`. Another build moves them; the ini exposes all of them, and `playerPosOff = 0` re-runs the search that finds the last one.
