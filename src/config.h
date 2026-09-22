// comfygrass.ini: the tunable rows, plus the draw-call signature that says which draws are grass.
//
// The wind/physics rows mirror wxl-grasswind's WindSettings/PhysicsSettings so the two can be compared
// directly; the [match] row has no counterpart there, because on WotLK the grass shader identifies
// itself and on 1.12 nothing does (see README).
#pragma once

#include <windows.h>

struct WindSettings
{
    bool  enabled         = true;
    float directionDeg    = 45.0f;  // wind heading in the world XY plane, degrees
    float speed           = 3.0f;   // wave travel speed, yards per second
    float amplitude       = 0.060f; // primary wave sway at the blade tip, yards
    float wavelength      = 18.0f;  // primary wave length, yards
    float crossAmplitude  = 0.020f; // secondary cross-swell sway, yards
    float crossWavelength = 6.5f;   // secondary wave length, yards
    float crossAngleDeg   = 35.0f;  // secondary heading offset from the primary, degrees
    float lean            = 0.35f;  // constant downwind lean, fraction of the primary amplitude
    float variance        = 0.6f;   // per-blade amplitude spread, 0 (uniform) .. 1 (0.5x..1.5x)
    float anchor          = 0.3f;   // fraction of the blade (from the base) that never moves, 0..0.9
};

struct PhysicsSettings
{
    bool  enabled     = true;
    float radius      = 2.0f;  // influence radius around the player, yards
    float forceCenter = 0.5f;  // lean strength at the unit
    float forceEdge   = 0.0f;  // lean strength at the radius edge

    // The object-manager position is the unit origin, i.e. the feet, so centerZ is normally 0 and
    // exists only to nudge the anchor if a blade's own base sits oddly. The Z gate only stops grass on
    // a ledge overhead or below from reacting, so it is loose and symmetric rather than wxl's ground-up
    // cone.
    float centerZ     = 0.0f;  // shift the anchor along Z from the player's feet, yards
    float zRange      = 4.0f;  // no parting past this |Z| gap between blade and anchor, yards
    float zFade       = 2.0f;  // width of the fade band at that limit, yards

    // Where the running client keeps the camera position and its look-at target, both float3 world
    // coordinates. Found by disassembling this WoW.exe (see README); overridable here because a
    // different build would move them, and 0 disables parting without touching anything else.
    DWORD camAddr     = 0x00C7CF20;
    DWORD anchorAddr  = 0x00C7D118; // camera + forward, so anchorAddr - camAddr is the view direction

    // The local player, walked out of the object manager. Every offset here except playerPosOff came
    // out of the disassembly; that one is found by geometry at run time (see comfygrass.cpp) and logged,
    // so it can be pinned here afterwards to skip the search.
    DWORD objMgrAddr   = 0x00B41414;
    DWORD playerPosOff = 0x9B8;  // found by the search below; 0 re-runs it
    DWORD posScanMax   = 0x1400; // how far into the object the search looks, bytes
    int   posScanFrames = 90;    // frames a candidate must survive before it is believed

    bool  reportAnchor = false;  // log the anchor and the chunk-grid check with the cost report
};

// Which draw calls to treat as grass. Filled in from a probe capture (F9); a field left at its
// "unset" value is not tested, so the signature can be as loose or as tight as the capture warrants.
struct MatchSettings
{
    DWORD fvf        = 0xFFFFFFFF; // stream FVF, or 0xFFFFFFFF for any
    UINT  stride     = 0;          // stream stride in bytes, or 0 for any
    UINT  minVerts   = 0;          // NumVertices lower bound
    UINT  maxVerts   = 0;          // NumVertices upper bound, 0 for no bound
    int   primType   = 0;          // D3DPRIMITIVETYPE, 0 for any
    bool  identityRotation = true; // translation-only world matrix: grass, not doodads
};

struct Settings
{
    WindSettings    wind;
    PhysicsSettings physics;
    MatchSettings   match;

    bool  effectEnabled = false; // off until a signature is configured (see README)
    bool  logEnabled    = true;
    bool  hook          = true;  // set 0 for a pure pass-through proxy (bisecting)
    int   probeKey      = VK_F9; // dump one frame of draw calls
    int   toggleKey     = VK_F10;// toggle the effect at runtime
    int   probeVertices = 8;     // vertices to dump per draw during a probe capture
    bool  worldPhase    = false; // add the draw's world translation to the wave phase.
                                 // Off: the pattern is stable but repeats per chunk. On: it is
                                 // continuous across chunks but slides as the camera moves,
                                 // because that translation is camera-relative.
    unsigned reportEvery = 300;  // log displacement cost every N frames, 0 = off
    float scale         = 1.0f;  // multiplies the wind; parting is deliberately independent of it
};

extern Settings g_cfg;

void LoadSettings(const wchar_t* iniPath);
void ResolveIniPath(HMODULE self, wchar_t* out, size_t count);
