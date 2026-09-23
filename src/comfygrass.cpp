// comfygrass: moving grass for the 1.12 client.
//
// Why this looks nothing like wxl-grasswind: on WotLK the detail doodads have their own vertex shader,
// so that module disassembles the stock shader, splices a wind block into it and reassembles. The 1.12
// client ships exactly five vertex shaders (patter, rain, sand, snowpoint, Model2), and none of them is
// grass. Detail doodads are CPU-built geometry (WorldClient/DetailDoodad.cpp, buffers tagged
// "CDetailDoodad_vtx" / "CDetailDoodad_idx") pushed down a fixed-function path. There is no shader to
// patch, so comfygrass supplies one of its own.
//
// This client's WoW.exe renders through Direct3D 9, and d3d9.dll here is DXVK. WoW.exe loads d3d9.dll at
// run time and does not import it, so there is nothing to intercept. VanillaFixes loads comfygrass from
// dlls.txt. comfygrass creates a throwaway device of its own to find DXVK's shared IDirect3DDevice9
// vtable (see AttachToDxvk), patches a few slots IN PLACE, and acts on the draw calls that match the
// configured grass signature. In place matters: a private vtable copy drops the RTTI word DXVK keeps
// behind vtable[0], and the client silently gives up before CreateDevice.
//
// For a matched draw, a vs_2_0 is bound and the GPU does the displacement, reading the client's own
// vertex buffer. Nothing is read back and nothing is copied: that buffer is WRITEONLY and maps to
// uncached memory, which reads at ~20 MB/s and sank three earlier CPU designs (see README). D3D9 allows
// a programmable vertex shader alongside the fixed-function PIXEL pipeline, so the shader only has to
// reproduce vertex work: transform, lighting, fog, texcoords.
//
// Two modes:
//   probe  (F9):  dump one frame of draw calls with their signatures and a few vertices each, so the
//                 grass draws can be identified by diffing captures at /console frilldensity 1 vs 256.
//   effect (F10): reload comfygrass.ini and toggle the sway: two travelling sine waves, plus a parting
//                 around the player read out of the client, both hinged on the blade's own texture v
//                 so the roots stay planted.

#define CINTERFACE // gives the C-style IDirect3DDevice9Vtbl, so slots are patched by name, not by index
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "config.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <map>
#include <smmintrin.h>   // _mm_stream_load_si128
#include <vector>

namespace
{
    constexpr float kTwoPi  = 6.2831853f;
    constexpr float kJitter = 1.7f;   // per-blade phase spread, radians
    constexpr float kEps    = 1e-4f;

    wchar_t g_iniPath[MAX_PATH] = {};
    wchar_t g_logPath[MAX_PATH] = {};

    // One lock covers the log and hook installation.
    //
    // The log opens and closes the file for each line. It is written from DllMain, the attach thread and
    // the client's render thread, and concurrent writers silently lost lines, which made the diagnostics
    // lie.
    //
    // Hook installation runs only on the attach thread (PatchDevice, called once from AttachToDxvk). The
    // lock stays so that a second caller cannot race it: two racing installs chain hook->hook, which
    // recurses forever. That race was real in the old d3d9.dll proxy design, where the client called
    // Direct3DCreate9 from more than one thread.
    CRITICAL_SECTION g_lock;
    bool             g_lockReady = false;

    struct Guard
    {
        Guard()  { if (g_lockReady) EnterCriticalSection(&g_lock); }
        ~Guard() { if (g_lockReady) LeaveCriticalSection(&g_lock); }
    };

    // ---------------------------------------------------------------------------------------------
    // logging

    void Log(const char* fmt, ...)
    {
        if (!g_cfg.logEnabled)
            return;
        Guard g;
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_logPath, L"a") != 0 || !f)
            return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }

    // Installing a slot must never save our own hook as "the original": that is an infinite recursion.
    // The identity check below prevents it if a slot is installed twice, and g_devHooked, checked under
    // g_lock, makes PatchDevice install only once (see the note on g_lock).
    bool HookSlot(void** slot, void* hook, void** origOut)
    {
        if (*slot == hook)
            return true;               // already installed; keep the original we saved the first time

        DWORD prot = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot))
            return false;
        if (*slot != hook)             // re-check under the write mapping
        {
            *origOut = *slot;
            *slot    = hook;
        }
        VirtualProtect(slot, sizeof(void*), prot, &prot);
        return true;
    }

    double Now()
    {
        static double inv = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return 1.0 / static_cast<double>(f.QuadPart);
        }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) * inv;
    }

    // ---------------------------------------------------------------------------------------------
    // tracked device state
    //
    // D3D9 has getters, but calling them per draw would be silly; the setters are intercepted instead and
    // the few pieces the matcher and the displacement need are mirrored here.

    struct DeviceState
    {
        IDirect3DVertexBuffer9* vb          = nullptr;
        UINT                    vbOffset    = 0;   // stream 0 offset in bytes
        UINT                    stride      = 0;
        DWORD                   fvf         = 0;   // always 0 here: this client binds declarations
        IDirect3DVertexDeclaration9* decl   = nullptr;
        void*                   vshader     = nullptr; // non-null means a programmable shader is bound
        void*                   texture0    = nullptr;
        D3DMATRIX               world       = {};
        D3DMATRIX               view        = {};
        D3DMATRIX               proj        = {};
        float                   lastGrassWorld[3] = { 0, 0, 0 }; // world translation of the last
        bool                    lastGrassValid    = false;       // matched grass draw, for diagnostics
        uint64_t                frame       = 0;
        uint32_t                drawIndex   = 0;   // reset each frame, so probe lines are addressable
    };

    DeviceState g_state;

    // Mirrored fixed-function render states. The GPU path has to reproduce whatever the fixed-function
    // vertex pipeline is doing for these draws, so the probe reports the states that affect it.
    std::map<DWORD, DWORD> g_rs;

    bool g_probeArmed    = false; // capture the next frame
    bool g_probing       = false; // capturing right now
    bool g_effectOn      = false;
    bool g_probeKeyDown  = false;
    bool g_toggleKeyDown = false;

    // Direct read of a range. This is the ONLY place anything is read out of the client's buffer, and
    // only for a small sample: the probe capture, and each grass texture's uv span, measured once per
    // texture. Reading this memory per frame sank three earlier designs (see README).
    bool VbRead(IDirect3DVertexBuffer9* vb, UINT offset, UINT size, uint8_t* out)
    {
        void* p = nullptr;
        if (FAILED(vb->lpVtbl->Lock(vb, offset, size, &p, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !p)
            return false;
        memcpy(out, p, size);
        vb->lpVtbl->Unlock(vb);
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // wind parameters
    //
    // Derived once a frame and uploaded as shader constants; the motion itself happens on the GPU.

    struct WindFrame
    {
        float d1[2], d2[2];
        float k1, k2, amp1, amp2, lean, phase1, phase2;
        float variance, anchor, scale;
    };

    WindFrame g_wf;

    void BuildWindFrame()
    {
        const WindSettings& w = g_cfg.wind;

        const float t  = (GetTickCount() % 1000000u) * 0.001f;
        const float a1 = w.directionDeg * (kTwoPi / 360.0f);
        const float a2 = (w.directionDeg + w.crossAngleDeg) * (kTwoPi / 360.0f);

        g_wf.d1[0]  = cosf(a1); g_wf.d1[1] = sinf(a1);
        g_wf.d2[0]  = cosf(a2); g_wf.d2[1] = sinf(a2);
        g_wf.k1     = kTwoPi / (w.wavelength      > 0.1f ? w.wavelength      : 0.1f);
        g_wf.k2     = kTwoPi / (w.crossWavelength > 0.1f ? w.crossWavelength : 0.1f);
        g_wf.amp1   = w.enabled ? w.amplitude      : 0.0f;
        g_wf.amp2   = w.enabled ? w.crossAmplitude : 0.0f;
        g_wf.lean   = w.enabled ? w.lean           : 0.0f;
        g_wf.phase1 = -fmodf(g_wf.k1 * w.speed * t, kTwoPi);
        g_wf.phase2 = -fmodf(g_wf.k2 * w.speed * 1.37f * t, kTwoPi);

        g_wf.variance     = w.variance < 0 ? 0 : (w.variance > 1 ? 1 : w.variance);
        g_wf.anchor       = w.anchor < 0 ? 0 : (w.anchor > 0.9f ? 0.9f : w.anchor);
        g_wf.scale        = g_cfg.scale;
    }

    // ---------------------------------------------------------------------------------------------
    // vertex layout
    //
    // This client binds vertex shaders and declarations, so SetFVF never carries the layout and every
    // draw reports fvf=0. The offsets of POSITION and TEXCOORD0 within stream 0 therefore come from the
    // bound declaration, cached per declaration object.

    struct Layout
    {
        int  posOffset = -1;
        int  texOffset = -1;
        bool valid     = false;
    };

    std::map<IDirect3DVertexDeclaration9*, Layout> g_layouts;

    const Layout& LayoutFor(IDirect3DVertexDeclaration9* decl)
    {
        static Layout none;
        if (!decl)
            return none;

        auto it = g_layouts.find(decl);
        if (it != g_layouts.end())
            return it->second;

        Layout L;
        D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1] = {};
        UINT n = 0;
        if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, elems, &n)))
        {
            for (UINT i = 0; i < n; ++i)
            {
                const D3DVERTEXELEMENT9& e = elems[i];
                if (e.Stream != 0 || e.Type == D3DDECLTYPE_UNUSED)
                    continue;
                if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT3)
                    L.posOffset = e.Offset;
                if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0 &&
                    (e.Type == D3DDECLTYPE_FLOAT2 || e.Type == D3DDECLTYPE_FLOAT3 || e.Type == D3DDECLTYPE_FLOAT4))
                    L.texOffset = e.Offset;
            }
            L.valid = (L.posOffset >= 0);
        }
        return g_layouts.emplace(decl, L).first->second;
    }

    // Logs each declaration once, so a probe capture shows where position and uv are.
    void LogDeclaration(IDirect3DVertexDeclaration9* decl)
    {
        static std::map<IDirect3DVertexDeclaration9*, bool> seen;
        if (!decl || seen.count(decl))
            return;
        seen[decl] = true;

        D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1] = {};
        UINT n = 0;
        if (FAILED(decl->lpVtbl->GetDeclaration(decl, elems, &n)))
            return;
        Log("    decl %p: %u elements", decl, n);
        for (UINT i = 0; i < n; ++i)
        {
            const D3DVERTEXELEMENT9& e = elems[i];
            if (e.Type == D3DDECLTYPE_UNUSED)
                continue;
            Log("      stream=%u offset=%2u type=%u usage=%u usageIndex=%u",
                e.Stream, e.Offset, e.Type, e.Usage, e.UsageIndex);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // draw interception

    bool Matches(D3DPRIMITIVETYPE prim, UINT numVertices)
    {
        const MatchSettings& m = g_cfg.match;

        // A signature with nothing set would match every draw in the frame and displace the whole world,
        // so an unconfigured [match] never matches. Probe first, then fill it in.
        if (m.fvf == 0xFFFFFFFF && !m.stride && !m.minVerts && !m.maxVerts && !m.primType)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                Log("[match] is unconfigured, so the effect stays off. Capture a frame with the probe key first.");
            }
            return false;
        }

        if (m.fvf != 0xFFFFFFFF && g_state.fvf != m.fvf)        return false;

        // Grass is batched per map chunk, so its world matrix is a pure translation. Doodad instances
        // (trees, bushes) share the vertex format but carry a real rotation. Without this test, the wind
        // bent trees too.
        if (m.identityRotation)
        {
            const D3DMATRIX& w = g_state.world;
            for (int r = 0; r < 3; ++r)
                for (int cc = 0; cc < 3; ++cc)
                    if (fabsf(w.m[r][cc] - (r == cc ? 1.0f : 0.0f)) > 1e-3f)
                        return false;
        }
        if (m.stride && g_state.stride != m.stride)             return false;
        if (m.primType && static_cast<int>(prim) != m.primType) return false;
        if (m.minVerts && numVertices < m.minVerts)             return false;
        if (m.maxVerts && numVertices > m.maxVerts)             return false;
        return g_state.vb != nullptr && g_state.stride != 0;
    }

    void ProbeDraw(const char* kind, D3DPRIMITIVETYPE prim, UINT firstVertex, UINT numVertices, UINT primCount)
    {
        Log("  [%3u] %-16s prim=%d verts=%u first=%u prims=%u decl=%p vs=%p stride=%u vb=%p off=%u tex0=%p%s",
            g_state.drawIndex, kind, static_cast<int>(prim), numVertices, firstVertex, primCount,
            g_state.decl, g_state.vshader, g_state.stride, g_state.vb, g_state.vbOffset, g_state.texture0,
            Matches(prim, numVertices) ? "  <== MATCH" : "");
        LogDeclaration(g_state.decl);

        const int nDump = g_cfg.probeVertices;
        if (nDump <= 0 || !g_state.vb || !g_state.stride)
            return;

        const UINT stride = g_state.stride;
        const UINT count  = numVertices < static_cast<UINT>(nDump) ? numVertices : static_cast<UINT>(nDump);
        std::vector<uint8_t> buf(static_cast<size_t>(count) * stride);
        if (!VbRead(g_state.vb, g_state.vbOffset + firstVertex * stride,
                    static_cast<UINT>(buf.size()), buf.data()))
        {
            Log("        (vertex read failed)");
            return;
        }

        // For a matched (grass) draw, dump everything a vertex shader would have to reproduce: the
        // fixed-function state it renders with, the transforms, and the full vertex. The GPU path needs a
        // per-vertex bend weight the shader can read directly, so NORMAL and COLOR are the candidates --
        // grass often bakes darkening toward the base into the vertex colour.
        const bool full = Matches(prim, numVertices);
        if (full)
        {
            const Layout& L = LayoutFor(g_state.decl);
            Log("        layout: posOffset=%d texOffset=%d stride=%u", L.posOffset, L.texOffset, stride);

            static const struct { DWORD rs; const char* name; } kStates[] = {
                { D3DRS_LIGHTING,        "LIGHTING"        },
                { D3DRS_COLORVERTEX,     "COLORVERTEX"     },
                { D3DRS_AMBIENT,         "AMBIENT"         },
                { D3DRS_FOGENABLE,       "FOGENABLE"       },
                { D3DRS_FOGCOLOR,        "FOGCOLOR"        },
                { D3DRS_FOGTABLEMODE,    "FOGTABLEMODE"    },
                { D3DRS_FOGVERTEXMODE,   "FOGVERTEXMODE"   },
                { D3DRS_FOGSTART,        "FOGSTART"        },
                { D3DRS_FOGEND,          "FOGEND"          },
                { D3DRS_FOGDENSITY,      "FOGDENSITY"      },
                { D3DRS_RANGEFOGENABLE,  "RANGEFOGENABLE"  },
                { D3DRS_ALPHATESTENABLE, "ALPHATESTENABLE" },
                { D3DRS_ALPHAREF,        "ALPHAREF"        },
                { D3DRS_CULLMODE,        "CULLMODE"        },
                { D3DRS_SPECULARENABLE,  "SPECULARENABLE"  },
            };
            for (const auto& st : kStates)
            {
                auto it = g_rs.find(st.rs);
                if (it != g_rs.end())
                {
                    const float asFloat = *reinterpret_cast<const float*>(&it->second);
                    Log("        rs %-16s = %u (0x%08X, as float %.3f)", st.name, it->second, it->second, asFloat);
                }
                else
                {
                    Log("        rs %-16s = (never set: default)", st.name);
                }
            }

            const D3DMATRIX* mats[3]  = { &g_state.world, &g_state.view, &g_state.proj };
            const char*      names[3] = { "world", "view", "proj" };
            for (int mi = 0; mi < 3; ++mi)
                for (int row = 0; row < 4; ++row)
                    Log("        %s[%d] = %10.4f %10.4f %10.4f %10.4f", names[mi], row,
                        mats[mi]->m[row][0], mats[mi]->m[row][1], mats[mi]->m[row][2], mats[mi]->m[row][3]);
        }

        const int texOff = LayoutFor(g_state.decl).texOffset;
        for (UINT i = 0; i < count; ++i)
        {
            const uint8_t* v  = buf.data() + static_cast<size_t>(i) * stride;
            const int      po = LayoutFor(g_state.decl).posOffset;
            const float*   p  = reinterpret_cast<const float*>(v + (po > 0 ? po : 0));
            char uv[64] = "";
            if (texOff >= 0 && static_cast<UINT>(texOff) + 8 <= stride)
            {
                const float* t = reinterpret_cast<const float*>(v + texOff);
                sprintf_s(uv, "  uv=(%.4f, %.4f)", t[0], t[1]);
            }

            // NORMAL @12 and COLOR @24 for the stride-36 grass layout: the two candidates for a
            // shader-readable bend weight.
            char extra[96] = "";
            if (full && stride >= 28)
            {
                const float* nrm = reinterpret_cast<const float*>(v + 12);
                DWORD col = 0;
                memcpy(&col, v + 24, 4);
                sprintf_s(extra, "  n=(%.3f, %.3f, %.3f) argb=%02X%02X%02X%02X",
                          nrm[0], nrm[1], nrm[2],
                          (col >> 24) & 0xFF, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
            }
            Log("        v%-2u pos=(%10.3f, %10.3f, %10.3f)%s%s", i, p[0], p[1], p[2], uv, extra);
        }
    }

    // The original device methods, saved by HookSlot. Declared here because the displacement
    // path below re-issues draws through them.
    using PresentFn      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using ResetFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using SetTransformFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    using SetTextureFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    using SetFVFFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);
    using SetRSFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    using SetVSFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    using SetDeclFn      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
    using SetStreamFn    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
    using DrawPrimFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawIdxPrimFn  = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

    PresentFn      g_oPresent      = nullptr;
    ResetFn        g_oReset        = nullptr;
    SetTransformFn g_oSetTransform = nullptr;
    SetTextureFn   g_oSetTexture   = nullptr;
    SetFVFFn       g_oSetFVF       = nullptr;
    SetRSFn        g_oSetRS        = nullptr;
    SetVSFn        g_oSetVS        = nullptr;
    SetDeclFn      g_oSetDecl      = nullptr;
    SetStreamFn    g_oSetStream    = nullptr;
    DrawPrimFn     g_oDrawPrim     = nullptr;
    DrawIdxPrimFn  g_oDrawIdxPrim  = nullptr;
    CreateDeviceFn g_oCreateDevice = nullptr;

    // Cost of the shader path, logged every reportEvery frames, so it is judged on numbers rather than
    // on how the game feels.
    struct Cost
    {
        double   seconds = 0.0;
        uint64_t verts   = 0;
        uint32_t draws   = 0;
        uint64_t frames  = 0;
    };
    Cost g_cost;

    // ---------------------------------------------------------------------------------------------
    // GPU path
    //
    // The three CPU designs before this one read the client's vertex buffer back. That buffer is created
    // write-only and maps to uncached memory: reads run at ~20 MB/s however they are cached, batched or
    // issued, which put a floor of ~60 ms a frame under every CPU design.
    //
    // Here nothing is read. A vertex shader is bound for grass draws only, the client's own buffer is the
    // input, and the GPU does the displacement. D3D9 allows a programmable vertex shader alongside the
    // fixed-function pixel pipeline, so only vertex work has to be reproduced: transform, lighting, fog
    // and texcoords.
    //
    // The bend weight is uv.y, which on correctly-identified grass runs 0 at the tip to 1 at the base,
    // the same signal wxl-grasswind uses on WotLK. (An earlier capture suggested otherwise, but it had
    // been measuring tree doodads, which share the vertex format.)

    // Minimal ID3DBlob. d3dcommon.h's definition is awkward under CINTERFACE, and only the two
    // accessors are needed to get the compiled bytecode out.
    struct OgBlob;
    struct OgBlobVtbl
    {
        HRESULT (STDMETHODCALLTYPE* QueryInterface)(OgBlob*, REFIID, void**);
        ULONG   (STDMETHODCALLTYPE* AddRef)(OgBlob*);
        ULONG   (STDMETHODCALLTYPE* Release)(OgBlob*);
        LPVOID  (STDMETHODCALLTYPE* GetBufferPointer)(OgBlob*);
        SIZE_T  (STDMETHODCALLTYPE* GetBufferSize)(OgBlob*);
    };
    struct OgBlob { const OgBlobVtbl* lpVtbl; };

    // uv.y span per grass texture, measured once from a small sample.
    //
    // This read happens a few dozen times per session, not per frame, so it costs nothing (see VbRead).
    // Only the fallback uses it: when the fill loop is patched, each vertex carries its own height
    // (see "detail models").
    struct VSpan { float lo; float inv; };
    std::map<void*, VSpan> g_vspan;

    IDirect3DVertexShader9* g_windVS      = nullptr;
    bool                    g_windVSTried = false;

    // ---------------------------------------------------------------------------------------------
    // the player anchor
    //
    // Parting needs the player in the same space as the grass, and that space is camera-relative (see
    // the note in DrawWithWindShader). So the value needed is playerPos - cameraPos, not either one on
    // its own. 0x00680BC0 in this WoW.exe takes a camera position and a look-at target, stores them to
    // two float3 globals, then computes target - position to build the view direction.
    //
    // The target is one yard out along the view axis, so target - camera is the forward vector, not the
    // player. The player position comes from the object manager (below). Both camera addresses came out
    // of disassembling this binary, not a published offset list: the camera pointer those lists give for
    // 1.12.1 is not referenced anywhere in this build, although their object-manager global
    // (0x00B41414) is where they say.

    struct PlayerAnchor
    {
        bool  valid     = false;
        float rel[3]     = { 0, 0, 0 }; // player - camera: the player, in camera-relative world axes
        float camera[3]  = { 0, 0, 0 }; // absolute camera position, for the search and the diagnostic
        float forward[3] = { 0, 0, 0 }; // camera view direction, unit length
    };

    PlayerAnchor g_anchor;
    bool         g_anchorProbed = false;   // the module and page checks run once
    bool         g_anchorUsable = false;
    intptr_t     g_imageSlide   = 0;       // in case the image ever loads away from 0x00400000

    inline bool Finite(float v) { return v == v && v < 3.0e38f && v > -3.0e38f; }

    // Null unless the whole float3 sits in one committed, readable page of the running client.
    const float* AnchorPtr(DWORD va)
    {
        if (!va)
            return nullptr;
        const uintptr_t p = static_cast<uintptr_t>(static_cast<intptr_t>(va) + g_imageSlide);
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return nullptr;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD))
            return nullptr;
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return (p + 3 * sizeof(float) <= end) ? reinterpret_cast<const float*>(p) : nullptr;
    }

    // ---------------------------------------------------------------------------------------------
    // the local player, walked out of the object manager
    //
    // These offsets are from the disassembly, not from a published list: the manager global at
    // 0x00B41414 (the list's own accessors sit right below it), its object list head at +0xAC, the
    // link offset at +0xA4 so that next = *(obj + link + 4), each object's GUID at +0x30, and the
    // active player's GUID at +0xC0, which is what the function at 0x00468550 returns.
    // A list pointer with its low bit set is the terminator, the same test the client's own loops use.

    bool SafeCopy(uintptr_t src, void* dst, size_t n)
    {
        __try
        {
            memcpy(dst, reinterpret_cast<const void*>(src), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t FindLocalPlayerObject()
    {
        const DWORD mgrAddr = g_cfg.physics.objMgrAddr;
        if (!mgrAddr)
            return 0;

        DWORD mgr = 0;
        const uintptr_t slot = static_cast<uintptr_t>(static_cast<intptr_t>(mgrAddr) + g_imageSlide);
        if (!SafeCopy(slot, &mgr, sizeof(mgr)) || !mgr)
            return 0;

        DWORD guid[2] = {}, link = 0, obj = 0;
        if (!SafeCopy(mgr + 0xC0, guid, sizeof(guid)) || (!guid[0] && !guid[1]))
            return 0;
        if (!SafeCopy(mgr + 0xA4, &link, sizeof(link)))
            return 0;
        if (!SafeCopy(mgr + 0xAC, &obj, sizeof(obj)))
            return 0;

        for (int n = 0; n < 16384 && obj && !(obj & 1); ++n)
        {
            DWORD g[2] = {};
            if (!SafeCopy(obj + 0x30, g, sizeof(g)))
                return 0;
            if (g[0] == guid[0] && g[1] == guid[1])
                return obj;
            DWORD next = 0;
            if (!SafeCopy(obj + link + 4, &next, sizeof(next)))
                return 0;
            obj = next;
        }
        return 0;
    }

    // Where the position sits inside the object is the one thing the disassembly did not hand over, so
    // it is found from geometry instead. Whatever the zoom or the pitch, a camera that orbits the
    // player leaves the player in the vertical plane through the camera's forward vector, so
    // (player - camera).xy runs parallel to forward.xy. Pitch only moves things in Z, so the test
    // ignores Z and stays exact at every camera angle.
    //
    // Almost nothing else in the object passes that while you walk and turn, and the search keeps only
    // offsets that pass on every frame of the window, so a coincidence has to hold for ~90 frames.
    bool PlausiblePlayer(const float p[3], const float cam[3], const float fwd[3])
    {
        for (int i = 0; i < 3; ++i)
            if (!Finite(p[i]) || p[i] < -20000.0f || p[i] > 20000.0f)
                return false;

        const float vx = p[0] - cam[0], vy = p[1] - cam[1], vz = p[2] - cam[2];
        const float h2 = vx * vx + vy * vy;
        if (h2 < 0.04f || h2 > 60.0f * 60.0f)   // 0.2 .. 60 yards out horizontally
            return false;
        if (vz < -30.0f || vz > 30.0f)
            return false;

        const float f2 = fwd[0] * fwd[0] + fwd[1] * fwd[1];
        if (f2 < 0.09f)                          // camera near-vertical: the test has nothing to say
            return false;
        return (vx * fwd[0] + vy * fwd[1]) / sqrtf(h2 * f2) > 0.995f;
    }

    struct PlayerFinder
    {
        DWORD              offset  = 0;   // resolved, or 0 while still searching
        int                frames  = 0;
        bool               done    = false;
        std::vector<DWORD> cand;
    };
    PlayerFinder g_finder;

    void SearchPlayerOffset(uintptr_t obj, const float cam[3], const float fwd[3])
    {
        const PhysicsSettings& p = g_cfg.physics;

        std::vector<uint8_t> blob(p.posScanMax + 12);
        if (!SafeCopy(obj, blob.data(), blob.size()))
            return;

        std::vector<DWORD> pass;
        for (DWORD off = 0; off + 12 <= blob.size(); off += 4)
        {
            float v[3];
            memcpy(v, blob.data() + off, sizeof(v));
            if (PlausiblePlayer(v, cam, fwd))
                pass.push_back(off);
        }
        if (pass.empty())
            return;   // a frame with nothing to say (first person, camera straight down) is skipped

        if (g_finder.cand.empty() && g_finder.frames == 0)
        {
            g_finder.cand = pass;
        }
        else
        {
            std::vector<DWORD> keep;
            size_t i = 0, j = 0;
            while (i < g_finder.cand.size() && j < pass.size())
            {
                if (g_finder.cand[i] < pass[j])      ++i;
                else if (pass[j] < g_finder.cand[i]) ++j;
                else { keep.push_back(pass[j]); ++i; ++j; }
            }
            g_finder.cand.swap(keep);
        }

        if (g_finder.cand.empty())
        {
            Log("player offset: every candidate eliminated, restarting the search");
            g_finder.frames = 0;
            return;
        }

        if (++g_finder.frames >= p.posScanFrames)
        {
            g_finder.offset = g_finder.cand.front();
            g_finder.done   = true;

            char list[256] = {};
            int  n = 0;
            for (size_t k = 0; k < g_finder.cand.size() && n < 200; ++k)
                n += _snprintf_s(list + n, sizeof(list) - n, _TRUNCATE, "0x%X ", g_finder.cand[k]);
            Log("player offset: resolved to +0x%X after %d frames (survivors: %s)",
                g_finder.offset, g_finder.frames, list);
            Log("player offset: set playerPosOff = 0x%X in comfygrass.ini to skip this search",
                g_finder.offset);
        }
    }

    void UpdateAnchor()
    {
        g_anchor.valid = false;

        const PhysicsSettings& p = g_cfg.physics;
        if (!p.enabled || !p.camAddr || !p.anchorAddr)
            return;

        if (!g_anchorProbed)
        {
            g_anchorProbed = true;
            const HMODULE exe = GetModuleHandleW(nullptr);
            g_imageSlide   = reinterpret_cast<intptr_t>(exe) - static_cast<intptr_t>(0x00400000);
            g_anchorUsable = AnchorPtr(p.camAddr) != nullptr && AnchorPtr(p.anchorAddr) != nullptr;
            Log("player anchor: image at %p (slide %+d), camera 0x%08X, anchor 0x%08X: %s",
                exe, static_cast<int>(g_imageSlide), p.camAddr, p.anchorAddr,
                g_anchorUsable ? "readable" : "NOT readable, parting stays off");
        }
        if (!g_anchorUsable)
            return;

        const float* cam = AnchorPtr(p.camAddr);
        const float* tgt = AnchorPtr(p.anchorAddr);
        if (!cam || !tgt)
            return;

        const float c[3] = { cam[0], cam[1], cam[2] };
        const float t[3] = { tgt[0], tgt[1], tgt[2] };

        // Sanity, every frame rather than once: world coordinates run to about +-17066 yards, and no
        // zoom puts the camera anywhere near 100 yards from its own aim point. A loading screen, a
        // character-select screen or a mismatched build fails one of these, and parting skips the frame
        // rather than flinging blades at a garbage coordinate.
        if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f)
            return;
        for (int i = 0; i < 3; ++i)
        {
            if (!Finite(c[i]) || !Finite(t[i]))
                return;
            if (c[i] < -20000.0f || c[i] > 20000.0f || t[i] < -20000.0f || t[i] > 20000.0f)
                return;
        }

        // anchorAddr holds the point the camera aims at, one yard along the view axis, so this comes
        // out unit length: it is the camera's forward vector, which is what the offset search needs.
        const float fwd[3] = { t[0] - c[0], t[1] - c[1], t[2] - c[2] };
        if (fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2] > 100.0f * 100.0f)
            return;

        g_anchor.camera[0]  = c[0];   g_anchor.camera[1]  = c[1];   g_anchor.camera[2]  = c[2];
        g_anchor.forward[0] = fwd[0]; g_anchor.forward[1] = fwd[1]; g_anchor.forward[2] = fwd[2];

        const uintptr_t obj = FindLocalPlayerObject();
        if (!obj)
            return;

        DWORD off = p.playerPosOff ? p.playerPosOff : g_finder.offset;
        if (!off)
        {
            SearchPlayerOffset(obj, c, fwd);
            return;                        // no parting until the search settles, a second or two
        }

        float pos[3];
        if (!SafeCopy(obj + off, pos, sizeof(pos)))
            return;
        for (int i = 0; i < 3; ++i)
            if (!Finite(pos[i]) || pos[i] < -20000.0f || pos[i] > 20000.0f)
                return;

        // A resolved offset that stops making sense means the object layout was not what we settled
        // on, so drop it and search again rather than part the grass around a garbage coordinate.
        const float vx = pos[0] - c[0], vy = pos[1] - c[1], vz = pos[2] - c[2];
        if (vx * vx + vy * vy + vz * vz > 80.0f * 80.0f)
        {
            if (!p.playerPosOff)
            {
                Log("player offset: +0x%X stopped tracking the camera, searching again", off);
                g_finder = PlayerFinder();
            }
            return;
        }

        g_anchor.rel[0] = vx; g_anchor.rel[1] = vy; g_anchor.rel[2] = vz;
        g_anchor.valid  = true;
    }

    // Proves or disproves the camera address without needing anything to look right on screen.
    //
    // If 0x00C7CF20 is the camera, then (grass world translation + camera) is that chunk's true world
    // origin, and map chunks sit on a 100/3 yard grid. So the residual below has to be the same
    // small number every frame, from any chunk, wherever you stand. If the address were wrong it would
    // wander with the camera instead.
    void ReportAnchor()
    {
        if (!g_anchor.valid)
        {
            Log("  anchor: unavailable this frame (out of world, or the address is wrong)");
            return;
        }

        const float* c = g_anchor.camera;
        const float* r = g_anchor.rel;
        Log("  anchor: camera=(%.2f %.2f %.2f) player-rel=(%.2f %.2f %.2f) dist=%.2f",
            c[0], c[1], c[2], r[0], r[1], r[2],
            sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]));

        if (!g_state.lastGrassValid)
            return;

        const float kChunk = 100.0f / 3.0f;
        float origin[3], residual[2];
        for (int i = 0; i < 3; ++i)
            origin[i] = g_state.lastGrassWorld[i] + c[i];
        for (int i = 0; i < 2; ++i)
        {
            residual[i] = fmodf(origin[i], kChunk);
            if (residual[i] < 0.0f)
                residual[i] += kChunk;
        }
        Log("  anchor: chunk origin=(%.2f %.2f %.2f) grid residual=(%.3f %.3f) of %.3f",
            origin[0], origin[1], origin[2], residual[0], residual[1], kChunk);
    }

    // ---------------------------------------------------------------------------------------------
    // detail models: a height for every vertex, and rigid models
    //
    // The shader needs to know how high up its blade each vertex is: the root must stay planted and
    // the tip must move most. The draw call does not say. Two things are wrong with guessing it:
    //
    //   1. The texture v is a poor measure of height. It was the first signal (see README), but in
    //      many models the top vertices sit in the middle of v: BadGra01's top vertices have v 0.52
    //      in a span of 0.02 to 0.73, and got a weight of 0. So Badlands grass barely swayed and did
    //      not part at all.
    //   2. A ground layer's detail models include rocks, pebbles, shells and bones. The client draws
    //      them in batches, one batch per texture, and a rock and a grass tuft often share one atlas
    //      (BadRoc01 and BadGra02 both use 8des_detaildoodads01.blp). So nothing at the draw level can
    //      keep the rocks still.
    //
    // The client knows both answers while it fills a batch, so comfygrass patches the fill loop at
    // 0x006B2600 in two places:
    //
    //   006B2690  mov edx, [esi+4]            ; esi = the instance, [esi+4] = its model index
    //   006B2693  mov eax, [0x00CAFFFC]       ; the table of CDetailDoodadData, one per model
    //   006B2698  mov eax, [eax+edx*4]        ; CDetailDoodadData: +0 file name, +4 model handle
    //   006B269E  call 0x00710E20             ; model handle -> M2 header
    //   ...
    //   006B26CD  mov ebx, [ebp+ebx*4-0x24]   ; pick the lit or the shadowed colour  <- patch 1
    //   006B26D1  mov [ebp-0x18], ebx         ; the colour for every vertex of this instance
    //   006B26D4  jmp 0x006B26E0              ; into the per-vertex loop
    //   ...                                   ; ecx = the model vertex, edi = the next output vertex
    //   006B2743  mov edx, [ebp-0x18]         ;                                      <- patch 2
    //   006B2746  mov [edi-0xC], edx          ; write the colour
    //   006B2749  mov eax, [ecx+0x20]         ; the uv follows
    //
    // Patch 1 runs once per instance. At that point eax holds the M2 header: +0x44 vertex count, +0x48
    // vertices (0x30 bytes each, z at +8), +0x50 the first view (+0 index count, +4 the u16 indices).
    // It finds the model's top and decides whether the model is rigid, cached per model.
    //
    // Patch 2 runs once per vertex. It divides the model vertex's own z by the top of the model, which
    // gives 0 at the ground and 1 at the top, and stores it in 6 bits: the lowest 2 bits of red, of
    // green and of blue. A rigid model stores 0 everywhere. The alpha's lowest bit is cleared as the
    // sign that the colour carries a height. The client writes only two colours, 0xFFFFFFFF and
    // 0xFFC0C0C0 (in shadow), so the shader can put the exact colour back. The fixed-function path,
    // which draws the grass when the effect is off, sees each channel off by at most 3/255.
    //
    // A model is rigid if its top is lower than rigidHeight, or if its file name contains a word from
    // rigidNames. Measured over all 447 detail models in GroundEffectDoodad.dbc, every rock is lower
    // than 0.28 yards, and every model lower than 0.3 yards is a rock, a pebble, a shell, a low tuft or
    // a small mushroom. Grass starts at 0.31 yards. The bones in Deadwind Pass and the Eastern
    // Plaguelands are up to 0.93 yards tall, so the name catches those.
    //
    // Before it patches, comfygrass compares the whole loop, 0xE7 bytes from 0x006B2690, with the bytes
    // it expects. A different WoW.exe fails the check. Then nothing is patched, and the shader falls
    // back to the texture v, as before.

    constexpr DWORD   kFillBlockBack = 0x3D;         // the check starts at 0x006B2690
    constexpr uint8_t kFillBlock[]   = {
        0x8B, 0x56, 0x04, 0xA1, 0xFC, 0xFF, 0xCA, 0x00, 0x8B, 0x04, 0x90, 0x8B, 0x48, 0x04, 0xE8, 0x7D,
        0xE7, 0x05, 0x00, 0x8B, 0x50, 0x50, 0x8B, 0x0A, 0x89, 0x4D, 0xF8, 0x8A, 0x0E, 0x33, 0xDB, 0xF6,
        0xC1, 0x01, 0x89, 0x45, 0xEC, 0x89, 0x55, 0xF0, 0x74, 0x05, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x33,
        0xC9, 0x39, 0x4D, 0xF8, 0x89, 0x4D, 0xFC, 0x0F, 0x86, 0x9A, 0x00, 0x00, 0x00, 0x8B, 0x5C, 0x9D,
        0xDC, 0x89, 0x5D, 0xE8, 0xEB, 0x0A, 0x8B, 0x55, 0xF0, 0x8B, 0x45, 0xEC, 0x8D, 0x64, 0x24, 0x00,
        0x8B, 0x52, 0x04, 0x0F, 0xB7, 0x0C, 0x4A, 0x8B, 0x50, 0x48, 0x8D, 0x0C, 0x49, 0xC1, 0xE1, 0x04,
        0x03, 0xCA, 0x8D, 0x46, 0x20, 0x8D, 0x57, 0x0C, 0xD9, 0x01, 0x83, 0xC7, 0x24, 0xD8, 0x4E, 0x18,
        0xD9, 0x46, 0x14, 0xD8, 0x49, 0x04, 0xDE, 0xE9, 0xD8, 0x4E, 0x1C, 0xD8, 0x46, 0x08, 0xD9, 0x5F,
        0xDC, 0xD9, 0x01, 0xD8, 0x4E, 0x14, 0xD9, 0x46, 0x18, 0xD8, 0x49, 0x04, 0xDE, 0xC1, 0xD8, 0x4E,
        0x1C, 0xD8, 0x46, 0x0C, 0xD9, 0x5F, 0xE0, 0xD9, 0x41, 0x08, 0xD8, 0x4E, 0x1C, 0xD8, 0x46, 0x10,
        0xD9, 0x5F, 0xE4, 0x8B, 0x18, 0x89, 0x1A, 0x8B, 0x58, 0x04, 0x89, 0x5A, 0x04, 0x8B, 0x40, 0x08,
        0x89, 0x42, 0x08, 0x8B, 0x55, 0xE8, 0x89, 0x57, 0xF4, 0x8B, 0x41, 0x20, 0x89, 0x47, 0xF8, 0x8B,
        0x49, 0x24, 0x8B, 0x45, 0xF8, 0x89, 0x4F, 0xFC, 0x8B, 0x4D, 0xFC, 0x41, 0x3B, 0xC8, 0x89, 0x4D,
        0xFC, 0x0F, 0x82, 0x6F, 0xFF, 0xFF, 0xFF, 0x8B, 0x45, 0xF4, 0x83, 0xC6, 0x2C, 0x48, 0x89, 0x45,
        0xF4, 0x0F, 0x85, 0x19, 0xFF, 0xFF, 0xFF,
    };
    constexpr DWORD kInstanceLen    = 7;     // patch 1 replaces two instructions, 7 bytes
    constexpr DWORD kInstanceResume = 0x13;  // 0x006B26E0, where the jmp at 0x006B26D4 goes
    constexpr DWORD kVertexAt       = 0x76;  // patch 2, 0x006B2743
    constexpr DWORD kVertexLen      = 6;     // two instructions, 6 bytes
    constexpr int   kHeightSteps    = 63;    // 6 bits

    DWORD g_instanceResume = 0;    // absolute addresses the stubs return to
    DWORD g_vertexResume   = 0;
    DWORD g_modelTable     = 0;    // 0x00CAFFFC, read out of the checked code, not assumed
    bool  g_fillTried      = false; // one attempt: the check fails the same way every time
    bool  g_fillPatched    = false;

    // Written by patch 1 for the instance, read by patch 2 for each of its vertices.
    DWORD g_encode      = 0;       // 1: store the height in the colour
    float g_heightScale = 0.0f;    // kHeightSteps / top of the model, or 0 for a rigid model
    DWORD g_heightTmp   = 0;

    // Classification is cached per model. The key holds both the M2 header and the CDetailDoodadData,
    // so a model freed and replaced at the same address is not taken for the old one.
    struct ModelEntry { const void* model; const void* data; float scale; };
    ModelEntry g_modelCache[64];
    int        g_modelCount = 0;

    void ClearModelCache() { g_modelCount = 0; }

    // The CDetailDoodadData of an instance. The client read this same slot a moment before the patch
    // site, so the read cannot fault.
    const void* DetailData(const uint8_t* inst)
    {
        const uint8_t* const* table =
            *reinterpret_cast<const uint8_t* const* const*>(static_cast<uintptr_t>(g_modelTable));
        return table[*reinterpret_cast<const DWORD*>(inst + 4)];
    }

    // Reads the model's top and file name. Guarded, and with no C++ objects so that __try is allowed.
    bool ReadDetailModel(const uint8_t* model, const void* data, float* topOut,
                         char* nameOut, size_t nameCount)
    {
        __try
        {
            const DWORD     nVerts = *reinterpret_cast<const DWORD*>(model + 0x44);
            const uint8_t*  verts  = *reinterpret_cast<const uint8_t* const*>(model + 0x48);
            const uint8_t*  view   = *reinterpret_cast<const uint8_t* const*>(model + 0x50);
            const DWORD     nIdx   = *reinterpret_cast<const DWORD*>(view);
            const uint16_t* idx    = *reinterpret_cast<const uint16_t* const*>(view + 4);

            float top = -3.4e38f;
            for (DWORD k = 0; k < nIdx; ++k)
            {
                const DWORD i = idx[k];
                if (i >= nVerts)
                    continue;
                const float z = *reinterpret_cast<const float*>(verts + i * 0x30 + 8);
                if (z > top)
                    top = z;
            }

            const char* name = *reinterpret_cast<const char* const*>(data);
            *topOut = top;
            strncpy_s(nameOut, nameCount, name ? name : "", _TRUNCATE);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Case-insensitive: is one of the comma-separated words in list part of name?
    bool NameMatches(const char* name, const char* list)
    {
        char lower[128];
        size_t n = 0;
        for (; name[n] && n + 1 < sizeof(lower); ++n)
            lower[n] = static_cast<char>(tolower(static_cast<unsigned char>(name[n])));
        lower[n] = 0;

        const char* p = list;
        while (*p)
        {
            while (*p == ',' || *p == ' ' || *p == '\t')
                ++p;
            char   word[32];
            size_t w = 0;
            while (*p && *p != ',' && *p != ' ' && *p != '\t')
            {
                if (w + 1 < sizeof(word))
                    word[w++] = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
                ++p;
            }
            word[w] = 0;
            if (w && strstr(lower, word))
                return true;
        }
        return false;
    }

    // Called by patch 1 once per instance, while the client fills a batch. Sets g_encode and
    // g_heightScale for the instance's vertices.
    void __cdecl InstanceSetup(const uint8_t* model, const uint8_t* inst)
    {
        g_encode = 0;
        if (!g_cfg.models.enabled)
            return;

        const void* data = DetailData(inst);
        for (int k = 0; k < g_modelCount; ++k)
            if (g_modelCache[k].model == model && g_modelCache[k].data == data)
            {
                g_heightScale = g_modelCache[k].scale;
                g_encode      = 1;
                return;
            }

        float top = 0.0f;
        char  name[64];
        if (!ReadDetailModel(model, data, &top, name, sizeof(name)))
            return;

        const bool low   = top < g_cfg.models.rigidHeight;
        const bool named = NameMatches(name, g_cfg.models.rigidNames);
        const bool flat  = top < 0.01f;   // nothing to divide by
        const float scale = (low || named || flat) ? 0.0f : kHeightSteps / top;
        Log("detail model %s: top %.2f yards, %s", name, top,
            scale == 0.0f ? (named ? "rigid (name)" : "rigid (low)") : "moves");

        if (g_modelCount == static_cast<int>(sizeof(g_modelCache) / sizeof(g_modelCache[0])))
            g_modelCount = 0;   // more models than slots: start again, do not grow
        g_modelCache[g_modelCount++] = { model, data, scale };
        g_heightScale = scale;
        g_encode      = 1;
    }

    // Patch 1. Registers on entry: eax = M2 header, esi = instance, ebx = 0 or 1 (in shadow).
    __declspec(naked) void InstanceStub()
    {
        __asm
        {
            pushad
            push esi
            push eax
            call InstanceSetup
            add  esp, 8
            popad
            mov  ebx, [ebp + ebx * 4 - 0x24]
            mov  [ebp - 0x18], ebx
            jmp  dword ptr [g_instanceResume]
        }
    }

    // Patch 2. Registers on entry: ecx = model vertex, edi = the output vertex + 0x24. eax, ebx and
    // edx are free here: the client reloads all three before it reads them again. The x87 stack is
    // empty, and fld/fistp leave it that way.
    __declspec(naked) void VertexStub()
    {
        __asm
        {
            mov  edx, [ebp - 0x18]
            cmp  dword ptr [g_encode], 0
            je   store

            fld   dword ptr [ecx + 8]
            fmul  dword ptr [g_heightScale]
            fistp dword ptr [g_heightTmp]
            mov   eax, [g_heightTmp]
            test  eax, eax
            jge   notBelow
            xor   eax, eax            // below the ground, or out of range
        notBelow:
            cmp   eax, 63
            jle   inRange
            mov   eax, 63
        inRange:
            and  edx, 0xFEFCFCFC      // alpha 0xFE marks a height; clear 2 bits of r, g and b
            mov  ebx, eax
            and  ebx, 3
            shl  ebx, 16
            or   edx, ebx             // red:   bits 0-1 of the height
            mov  ebx, eax
            shr  ebx, 2
            and  ebx, 3
            shl  ebx, 8
            or   edx, ebx             // green: bits 2-3
            shr  eax, 4
            or   edx, eax             // blue:  bits 4-5
        store:
            mov  [edi - 0x0C], edx
            jmp  dword ptr [g_vertexResume]
        }
    }

    bool WriteJump(uintptr_t at, const void* to, DWORD len)
    {
        uint8_t code[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (at + 5));
        memcpy(code + 1, &rel, sizeof(rel));

        DWORD prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &prot))
            return false;
        memcpy(reinterpret_cast<void*>(at), code, len);
        VirtualProtect(reinterpret_cast<void*>(at), len, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
        return true;
    }

    // Runs on the render thread, from Present, so the fill loop cannot run while the bytes change.
    void InstallFillPatch()
    {
        g_fillTried = true;

        const intptr_t  slide = reinterpret_cast<intptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
        const uintptr_t site  = static_cast<uintptr_t>(static_cast<intptr_t>(g_cfg.models.fillAddr) + slide);
        uint8_t have[sizeof(kFillBlock)] = {};
        if (!g_cfg.models.fillAddr || !SafeCopy(site - kFillBlockBack, have, sizeof(have)) ||
            memcmp(have, kFillBlock, sizeof(have)) != 0)
        {
            Log("detail models: the code at 0x%08X is not the expected fill loop. Nothing is patched: "
                "rocks move, and the bend weight comes from the texture.", g_cfg.models.fillAddr);
            return;
        }
        memcpy(&g_modelTable, have + 4, sizeof(g_modelTable));
        g_instanceResume = static_cast<DWORD>(site + kInstanceResume);
        g_vertexResume   = static_cast<DWORD>(site + kVertexAt + kVertexLen);

        // Patch 2 first: until patch 1 is in, g_encode stays 0 and patch 2 writes the colour unchanged.
        if (!WriteJump(site + kVertexAt, &VertexStub, kVertexLen) ||
            !WriteJump(site, &InstanceStub, kInstanceLen))
        {
            Log("detail models: VirtualProtect failed (%lu)", GetLastError());
            return;
        }

        g_fillPatched = true;
        Log("detail models: fill loop patched at 0x%08X and 0x%08X (model table 0x%08X)",
            static_cast<DWORD>(site), static_cast<DWORD>(site + kVertexAt), g_modelTable);
    }

    const char* kWindHlsl = R"HLSL(
struct VsIn  { float3 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct VsOut { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; float fog : FOG; };

float4 c0 : register(c0);   // rows of the transposed world-view-projection
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 gW1 : register(c4);  // primary wave:   dir.xy, amplitude, wavenumber
float4 gW2 : register(c5);  // secondary wave: dir.xy, amplitude, wavenumber
float4 gPh : register(c6);  // phase1, phase2, lean, variance
float4 gSh : register(c7);  // anchor, 1/(1-anchor), scale, unused
float4 gWT : register(c8);  // world translation (the chunk origin)
float4 gFg : register(c9);  // fogStart, fogEnd, 1/(end-start), fogEnable
float4 gAm : register(c10); // global ambient rgb: D3DRS_AMBIENT plus every enabled light's Ambient
float4 gLD : register(c11); // light direction xyz, enabled
float4 gLC : register(c12); // light diffuse colour rgb
float4 gVS : register(c13); // uv.y at the blade tip, 1/(base-tip)
float4 gPC : register(c14); // parting anchor in chunk-local space (xyz), parting enabled (w)
float4 gPR : register(c15); // 1/radius, force at the anchor, force at the edge, 1/zFade
float4 gPM : register(c16); // radius, zRange, unused, unused
float4 gMD : register(c17); // material diffuse
float4 gMA : register(c18); // material ambient
float4 gME : register(c19); // material emissive
float4 gMS : register(c20); // ambient/diffuse/emissive taken from the vertex colour (1) or material (0), lighting on

VsOut main(VsIn i)
{
    VsOut o;
    float3 p = i.pos;

    // A vertex with alpha 0xFE carries its height up the model in the lowest 2 bits of r, g and b
    // (see "detail models"): 0 at the ground, 63 at the top, 0 everywhere on a rigid model. The client
    // writes only 0xFF and 0xC0 in those channels, so the exact colour comes back: clear the bits,
    // then 0xFC can only have been 0xFF.
    float4 c255   = floor(i.col * 255.0 + 0.5);
    float  marked = step(c255.a, 254.5);
    float3 bits   = c255.rgb - 4.0 * floor(c255.rgb * 0.25);
    float  height = dot(bits, float3(1.0, 4.0, 16.0)) * (1.0 / 63.0);
    float3 plain  = c255.rgb - bits;
    plain += 3.0 * step(251.5, plain);
    float4 col    = lerp(i.col, float4(plain * (1.0 / 255.0), 1.0), marked);

    // The bend weight. Squaring the height gives a stiff base with a loose tip.
    //
    // The fallback, for a vertex without a height: uv.y runs from the tip to the base, but only across
    // a slice of the atlas, so it is normalised against the span measured for the texture. anchor then
    // holds the lowest part still, because the quads are sunk into the terrain.
    float base = saturate((i.uv.y - gVS.x) * gVS.y);
    float tip  = 1.0 - base;
    float wuv  = saturate((tip - gSh.x) * gSh.y);
    float w    = lerp(wuv * wuv, height * height, marked);

    // Phase comes from the chunk-local position plus gWT, which is zero unless worldPhase is on (see
    // DrawWithWindShader). Chunk-local positions do not move with the camera, so the pattern stays put.
    float2 wxy    = p.xy + gWT.xy;
    float  jit    = frac(wxy.x * 0.737 + wxy.y * 1.311);
    float  varMul = (1.0 - gPh.w * 0.5) + gPh.w * jit;
    float  phj    = jit * 1.7;

    float s1 = sin(dot(wxy, gW1.xy) * gW1.w + gPh.x + phj);
    float s2 = sin(dot(wxy, gW2.xy) * gW2.w + gPh.y + phj);

    float2 off = gW1.xy * (gW1.z * (s1 + gPh.z)) + gW2.xy * (gW2.z * s2);
    off *= w * varMul * gSh.z;

    // Parting: blades lean away from the player. gPC arrives already expressed in this vertex's own
    // chunk-local space, so this is one subtraction, with no world transform, and the per-draw cost
    // stays a couple of constants. The same weight w that hinges the wind hinges this, so the roots stay
    // planted while the tips open up. The Z term only stops grass on a ledge above or below the player
    // from reacting.
    float3 dp    = i.pos - gPC.xyz;
    float  dd    = dot(dp.xy, dp.xy) + 1e-4;
    float  dinv  = rsqrt(dd);
    float  dist  = dd * dinv;
    float  force = lerp(gPR.y, gPR.z, saturate(dist * gPR.x));
    force *= step(dist, gPM.x) * saturate((gPM.y - abs(dp.z)) * gPR.w) * w * gPC.w;
    off += dp.xy * (dinv * force);

    p.xy += off;

    float4 wp = float4(p, 1.0);
    o.pos = float4(dot(wp, c0), dot(wp, c1), dot(wp, c2), dot(wp, c3));

    // The fixed-function lighting equation, with the material sources the client set:
    //     emissive + ambientMat * globalAmbient + diffuseMat * lightDiffuse * N.L
    // An earlier version multiplied (ambient + light) by the vertex colour instead. That drops the
    // lights' own Ambient term and the material, and darkens grass the client draws unlit. The grass
    // came out visibly darker than stock.
    float3 N   = normalize(i.nrm);
    float  ndl = saturate(dot(N, -gLD.xyz)) * gLD.w;
    float4 ambM = lerp(gMA, col, gMS.x);
    float4 difM = lerp(gMD, col, gMS.y);
    float4 emiM = lerp(gME, col, gMS.z);
    float3 lit  = saturate(emiM.rgb + ambM.rgb * gAm.rgb + difM.rgb * gLC.rgb * ndl);
    o.col = lerp(col, float4(lit, difM.a), gMS.w);

    o.uv  = i.uv;
    o.fog = lerp(1.0, saturate((gFg.y - o.pos.w) * gFg.z), gFg.w);
    return o;
}
)HLSL";

    using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR,
                                            LPCSTR, UINT, UINT, OgBlob**, OgBlob**);

    // Builds the shader once. vs_2_0 is compiled at run time from HLSL via d3dcompiler_47, which ships
    // with Windows. There is no D3DX dependency and no bytecode to hand-assemble.
    bool EnsureWindShader(IDirect3DDevice9* dev)
    {
        if (g_windVS)
            return true;
        if (g_windVSTried)
            return false;
        g_windVSTried = true;

        HMODULE comp = GetModuleHandleA("d3dcompiler_47.dll");
        if (!comp)
            comp = LoadLibraryA("d3dcompiler_47.dll");
        if (!comp)
        {
            Log("wind shader: d3dcompiler_47.dll unavailable");
            return false;
        }

        auto compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(comp, "D3DCompile"));
        if (!compile)
        {
            Log("wind shader: D3DCompile not exported");
            return false;
        }

        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        const HRESULT hr = compile(kWindHlsl, strlen(kWindHlsl), "comfygrass", nullptr, nullptr,
                                   "main", "vs_2_0", 0, 0, &code, &errs);
        if (FAILED(hr) || !code)
        {
            Log("wind shader: compile failed hr=0x%08X: %s", hr,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no message)");
            if (errs) errs->lpVtbl->Release(errs);
            if (code) code->lpVtbl->Release(code);
            return false;
        }
        if (errs) errs->lpVtbl->Release(errs);

        const HRESULT chr = dev->lpVtbl->CreateVertexShader(
            dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &g_windVS);
        code->lpVtbl->Release(code);

        if (FAILED(chr) || !g_windVS)
        {
            Log("wind shader: CreateVertexShader failed hr=0x%08X", chr);
            g_windVS = nullptr;
            return false;
        }
        Log("wind shader: compiled and created");
        return true;
    }

    void ReleaseWindShader()
    {
        if (g_windVS)
        {
            g_windVS->lpVtbl->Release(g_windVS);
            g_windVS = nullptr;
        }
        g_windVSTried = false;
    }

    // row-major multiply, matching D3D9's v * M convention
    void MatMul(D3DMATRIX& out, const D3DMATRIX& a, const D3DMATRIX& b)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
                              a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
    }

    // True when the draw was issued with the wind shader bound and must not be forwarded again.
    bool DrawWithWindShader(IDirect3DDevice9* dev, bool indexed, D3DPRIMITIVETYPE prim,
                            INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
                            UINT startIndex, UINT primCount, UINT startVertex)
    {
        if (!EnsureWindShader(dev))
            return false;

        const double t0 = g_cfg.reportEvery ? Now() : 0.0;

        D3DMATRIX wv, wvp;
        MatMul(wv,  g_state.world, g_state.view);
        MatMul(wvp, wv,            g_state.proj);

        // Upload the transpose, so each register holds a column and o.pos = dot(v, c[n]) is the
        // row-vector multiply D3D9 uses.
        float c[21][4] = {};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                c[r][k] = wvp.m[k][r];

        c[4][0] = g_wf.d1[0]; c[4][1] = g_wf.d1[1]; c[4][2] = g_wf.amp1; c[4][3] = g_wf.k1;
        c[5][0] = g_wf.d2[0]; c[5][1] = g_wf.d2[1]; c[5][2] = g_wf.amp2; c[5][3] = g_wf.k2;
        c[6][0] = g_wf.phase1; c[6][1] = g_wf.phase2; c[6][2] = g_wf.lean; c[6][3] = g_wf.variance;

        const float anchor = g_wf.anchor;
        c[7][0] = anchor; c[7][1] = 1.0f / (1.0f - anchor); c[7][2] = g_wf.scale; c[7][3] = 0.0f;

        // The client renders camera-relative: its view matrix has no translation, so each draw's world
        // matrix is (chunkOrigin - cameraPos) and therefore moves whenever the camera does. Feeding that
        // into the wave phase made every blade slide to a new point in the wave as you walked, so the
        // blades appeared to jump between states.
        //
        // Vertex positions are chunk-local and stable, so by default the phase and the per-blade jitter
        // come from those alone and nothing moves with the camera. The cost is that the wind pattern
        // repeats per chunk instead of running continuously across the world. True world coordinates
        // are p + world translation + g_anchor.camera; the camera is already read for parting, but the
        // phase does not use it yet.
        if (g_cfg.worldPhase)
        {
            c[8][0] = g_state.world.m[3][0];
            c[8][1] = g_state.world.m[3][1];
            c[8][2] = g_state.world.m[3][2];
        }

        // Fog, straight from the mirrored render states.
        float fogStart = 0.0f, fogEnd = 1.0f;
        DWORD fogEnable = 0;
        {
            auto it = g_rs.find(D3DRS_FOGSTART);
            if (it != g_rs.end()) fogStart = *reinterpret_cast<const float*>(&it->second);
            it = g_rs.find(D3DRS_FOGEND);
            if (it != g_rs.end()) fogEnd = *reinterpret_cast<const float*>(&it->second);
            it = g_rs.find(D3DRS_FOGENABLE);
            if (it != g_rs.end()) fogEnable = it->second;
        }
        const float span = (fogEnd - fogStart) > 0.001f ? (fogEnd - fogStart) : 1.0f;
        c[9][0] = fogStart; c[9][1] = fogEnd; c[9][2] = 1.0f / span;
        c[9][3] = fogEnable ? 1.0f : 0.0f;

        // Lighting, read live from the device rather than from the g_rs mirror: the client may have set
        // these before our hooks went in, and a stale default here is exactly what made the grass dark.
        // D3D9's own defaults stand in if a read fails.
        auto rs = [dev](D3DRENDERSTATETYPE st, DWORD def) {
            DWORD v = def;
            return SUCCEEDED(dev->lpVtbl->GetRenderState(dev, st, &v)) ? v : def;
        };
        const DWORD lighting    = rs(D3DRS_LIGHTING,              TRUE);
        const DWORD colorVertex = rs(D3DRS_COLORVERTEX,           TRUE);
        const DWORD ambSrc      = rs(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        const DWORD difSrc      = rs(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        const DWORD emiSrc      = rs(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        const DWORD amb         = rs(D3DRS_AMBIENT,               0);

        // The grass vertex has a diffuse colour but no specular, so only COLOR1 reads the vertex;
        // D3D falls back to the material for COLOR2, and for everything when COLORVERTEX is off.
        auto fromVertex = [colorVertex](DWORD src) { return (colorVertex && src == D3DMCS_COLOR1) ? 1.0f : 0.0f; };

        D3DMATERIAL9 mat = {};
        dev->lpVtbl->GetMaterial(dev, &mat);
        const D3DCOLORVALUE* mats[3] = { &mat.Diffuse, &mat.Ambient, &mat.Emissive };
        for (int m = 0; m < 3; ++m)
        {
            c[17 + m][0] = mats[m]->r; c[17 + m][1] = mats[m]->g;
            c[17 + m][2] = mats[m]->b; c[17 + m][3] = mats[m]->a;
        }
        c[20][0] = fromVertex(ambSrc);
        c[20][1] = fromVertex(difSrc);
        c[20][2] = fromVertex(emiSrc);
        c[20][3] = lighting ? 1.0f : 0.0f;

        c[10][0] = ((amb >> 16) & 0xFF) / 255.0f;
        c[10][1] = ((amb >>  8) & 0xFF) / 255.0f;
        c[10][2] = ((amb      ) & 0xFF) / 255.0f;

        // Every enabled light contributes its Ambient to the global ambient; the first directional
        // one also supplies the diffuse term.
        c[11][3] = 0.0f;
        for (DWORD li = 0; li < 8; ++li)
        {
            BOOL on = FALSE;
            if (FAILED(dev->lpVtbl->GetLightEnable(dev, li, &on)) || !on)
                continue;
            D3DLIGHT9 L = {};
            if (FAILED(dev->lpVtbl->GetLight(dev, li, &L)))
                continue;

            c[10][0] += L.Ambient.r; c[10][1] += L.Ambient.g; c[10][2] += L.Ambient.b;

            if (L.Type != D3DLIGHT_DIRECTIONAL || c[11][3] != 0.0f)
                continue;
            float dx = L.Direction.x, dy = L.Direction.y, dz = L.Direction.z;
            const float len = sqrtf(dx * dx + dy * dy + dz * dz);
            if (len > 1e-4f) { dx /= len; dy /= len; dz /= len; }
            c[11][0] = dx; c[11][1] = dy; c[11][2] = dz; c[11][3] = 1.0f;
            c[12][0] = L.Diffuse.r; c[12][1] = L.Diffuse.g; c[12][2] = L.Diffuse.b;
        }

        // Once per session, so a colour mismatch can be traced to the state that caused it.
        static bool litLogged = false;
        if (!litLogged)
        {
            litLogged = true;
            Log("grass lighting: LIGHTING=%u COLORVERTEX=%u src amb/dif/emi=%u/%u/%u ambient=0x%08X "
                "globalAmb=(%.2f %.2f %.2f) sun=(%.2f %.2f %.2f) on=%.0f "
                "mat dif=(%.2f %.2f %.2f) amb=(%.2f %.2f %.2f) emi=(%.2f %.2f %.2f)",
                lighting, colorVertex, ambSrc, difSrc, emiSrc, amb,
                c[10][0], c[10][1], c[10][2], c[12][0], c[12][1], c[12][2], c[11][3],
                mat.Diffuse.r, mat.Diffuse.g, mat.Diffuse.b, mat.Ambient.r, mat.Ambient.g, mat.Ambient.b,
                mat.Emissive.r, mat.Emissive.g, mat.Emissive.b);
        }

        // Blade uv.y span for this grass texture, sampled once. Not needed when the fill loop is patched.
        VSpan uvSpan{ 0.0f, 1.0f };
        if (!g_fillPatched)
        {
            auto it = g_vspan.find(g_state.texture0);
            if (it == g_vspan.end())
            {
                float lo = 3.4e38f, hi = -3.4e38f;
                const Layout& L = LayoutFor(g_state.decl);
                const UINT n = numVertices < 256 ? numVertices : 256;
                if (L.texOffset >= 0 && g_state.vb && n)
                {
                    const UINT bytes = n * g_state.stride;
                    std::vector<uint8_t> buf(bytes);
                    const UINT at = g_state.vbOffset +
                                    (indexed ? (baseVertexIndex + minVertexIndex) : startVertex) * g_state.stride;
                    if (VbRead(g_state.vb, at, bytes, buf.data()))
                    {
                        for (UINT k = 0; k < n; ++k)
                        {
                            const float v = reinterpret_cast<const float*>(
                                buf.data() + k * g_state.stride + L.texOffset)[1];
                            if (v < lo) lo = v;
                            if (v > hi) hi = v;
                        }
                    }
                }
                VSpan sp{ 0.0f, 1.0f };
                if (hi > lo && (hi - lo) > 1e-5f)
                {
                    sp.lo  = lo;
                    sp.inv = 1.0f / (hi - lo);
                }
                Log("uv span for texture %p: %.4f .. %.4f", g_state.texture0, lo, hi);
                it = g_vspan.emplace(g_state.texture0, sp).first;
            }
            uvSpan = it->second;
        }
        c[13][0] = uvSpan.lo;
        c[13][1] = uvSpan.inv;

        // Parting. The anchor is camera-relative and the vertices are chunk-local, so folding the
        // draw's world translation in here leaves the shader with a plain subtraction:
        //     (p + world) - anchorRel  ==  p - (anchorRel - world)
        const PhysicsSettings& ph = g_cfg.physics;
        c[14][0] = g_anchor.rel[0] - g_state.world.m[3][0];
        c[14][1] = g_anchor.rel[1] - g_state.world.m[3][1];
        c[14][2] = g_anchor.rel[2] - g_state.world.m[3][2] + ph.centerZ;
        c[14][3] = (ph.enabled && g_anchor.valid) ? 1.0f : 0.0f;

        const float pRadius = ph.radius > 0.01f ? ph.radius : 0.01f;
        const float pFade   = ph.zFade  > 0.01f ? ph.zFade  : 0.01f;
        c[15][0] = 1.0f / pRadius; c[15][1] = ph.forceCenter;
        c[15][2] = ph.forceEdge;   c[15][3] = 1.0f / pFade;
        c[16][0] = pRadius;        c[16][1] = ph.zRange;

        g_state.lastGrassWorld[0] = g_state.world.m[3][0];
        g_state.lastGrassWorld[1] = g_state.world.m[3][1];
        g_state.lastGrassWorld[2] = g_state.world.m[3][2];
        g_state.lastGrassValid    = true;

        if (FAILED(dev->lpVtbl->SetVertexShaderConstantF(dev, 0, &c[0][0], 21)))
            return false;
        if (FAILED(dev->lpVtbl->SetVertexShader(dev, g_windVS)))
            return false;

        if (indexed)
            g_oDrawIdxPrim(dev, prim, baseVertexIndex, minVertexIndex, numVertices, startIndex, primCount);
        else
            g_oDrawPrim(dev, prim, startVertex, primCount);

        dev->lpVtbl->SetVertexShader(dev, nullptr);   // hand the fixed-function pipeline back

        if (g_cfg.reportEvery)
        {
            g_cost.seconds += Now() - t0;
            g_cost.verts += numVertices;
            g_cost.draws += 1;
        }
        return true;
    }

    UINT VertsForPrims(D3DPRIMITIVETYPE prim, UINT primCount)
    {
        switch (prim)
        {
        case D3DPT_POINTLIST:     return primCount;
        case D3DPT_LINELIST:      return primCount * 2;
        case D3DPT_LINESTRIP:     return primCount + 1;
        case D3DPT_TRIANGLELIST:  return primCount * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:   return primCount + 2;
        default:                  return 0;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // vtable hooking
    //
    // Slots are patched *in place* rather than by copying the vtable and repointing lpVtbl. A copy looks
    // tidier but breaks DXVK: its interfaces are C++ objects whose RTTI word sits immediately before
    // vtable[0], so a copy that starts at vtable[0] loses it, and the client quietly gave up before ever
    // reaching CreateDevice. Patching in place also hooks every object of the class at once, so the
    // client's device is caught whenever it is created, including before comfygrass loaded.
    //
    // Slot addresses come from the named CINTERFACE struct fields, so there are no magic indices.



    LONG     g_devHooked  = 0;   // set under g_lock: the vtable is patched only once

    void PollKeys()
    {
        const bool probe = (GetAsyncKeyState(g_cfg.probeKey) & 0x8000) != 0;
        if (probe && !g_probeKeyDown)
        {
            g_probeArmed = true;
            Log("--- probe armed (next frame will be captured) ---");
        }
        g_probeKeyDown = probe;

        const bool tog = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
        if (tog && !g_toggleKeyDown)
        {
            LoadSettings(g_iniPath);   // reload so tuning does not need a restart
            ClearModelCache();         // [models] may have changed
            g_effectOn = !g_effectOn;
            Log("--- effect %s (settings reloaded) ---", g_effectOn ? "ON" : "OFF");
        }
        g_toggleKeyDown = tog;
    }

    HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst,
                                        HWND wnd, const RGNDATA* dirty)
    {
        if (g_probing)
        {
            Log("--- end frame %llu (%u draws) ---", g_state.frame, g_state.drawIndex);
            g_probing = false;
        }
        PollKeys();
        if (g_cfg.models.enabled && !g_fillTried)
            InstallFillPatch();

        g_state.frame++;
        g_state.drawIndex = 0;
        if (g_probeArmed)
        {
            g_probeArmed = false;
            g_probing    = true;
            Log("--- begin frame %llu capture ---", g_state.frame);
        }
        if (g_cfg.reportEvery > 0 && ++g_cost.frames % g_cfg.reportEvery == 0)
        {
            Log("cost: %.2f ms/frame over %llu frames (%llu verts/frame, %u draws/frame)",
                1000.0 * g_cost.seconds / g_cfg.reportEvery,
                static_cast<unsigned long long>(g_cfg.reportEvery),
                static_cast<unsigned long long>(g_cost.verts / g_cfg.reportEvery),
                g_cost.draws / g_cfg.reportEvery);
            g_cost.seconds = 0.0; g_cost.verts = 0; g_cost.draws = 0;

            if (g_cfg.physics.reportAnchor)
                ReportAnchor();
        }

        UpdateAnchor();
        BuildWindFrame();
        return g_oPresent(dev, src, dst, wnd, dirty);
    }

    HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
    {
        ReleaseWindShader();    // device-owned objects do not survive a reset
        g_vspan.clear();
        g_layouts.clear();
        g_state.vb     = nullptr;
        g_state.stride = 0;
        return g_oReset(dev, pp);
    }

    HRESULT STDMETHODCALLTYPE hkSetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state,
                                             const D3DMATRIX* m)
    {
        if (state == D3DTS_PROJECTION && m)
            g_state.proj = *m;
        if (state == D3DTS_WORLD && m)
            g_state.world = *m;
        if (state == D3DTS_VIEW && m)
            g_state.view = *m;   // rotation only: this client folds the camera into each world matrix
        return g_oSetTransform(dev, state, m);
    }

    HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex)
    {
        if (stage == 0)
            g_state.texture0 = tex;
        return g_oSetTexture(dev, stage, tex);
    }

    HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE st, DWORD value)
    {
        g_rs[static_cast<DWORD>(st)] = value;
        return g_oSetRS(dev, st, value);
    }

    HRESULT STDMETHODCALLTYPE hkSetFVF(IDirect3DDevice9* dev, DWORD fvf)
    {
        g_state.fvf = fvf;
        return g_oSetFVF(dev, fvf);
    }

    HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* sh)
    {
        g_state.vshader = sh;
        return g_oSetVS(dev, sh);
    }

    HRESULT STDMETHODCALLTYPE hkSetVertexDeclaration(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* d)
    {
        g_state.decl = d;
        return g_oSetDecl(dev, d);
    }

    HRESULT STDMETHODCALLTYPE hkSetStreamSource(IDirect3DDevice9* dev, UINT stream,
                                                IDirect3DVertexBuffer9* vb, UINT offset, UINT stride)
    {
        if (stream == 0)
        {
            g_state.vb       = vb;
            g_state.vbOffset = offset;
            g_state.stride   = stride;
        }
        return g_oSetStream(dev, stream, vb, offset, stride);
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim,
                                              UINT startVertex, UINT primCount)
    {
        const UINT numVertices = VertsForPrims(prim, primCount);
        if (g_probing)
            ProbeDraw("DrawPrimitive", prim, startVertex, numVertices, primCount);
        g_state.drawIndex++;
        if (g_effectOn && g_cfg.effectEnabled && Matches(prim, numVertices) &&
            DrawWithWindShader(dev, false, prim, 0, 0, numVertices, 0, primCount, startVertex))
            return S_OK;
        return g_oDrawPrim(dev, prim, startVertex, primCount);
    }

    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim,
                                                     INT baseVertexIndex, UINT minVertexIndex,
                                                     UINT numVertices, UINT startIndex, UINT primCount)
    {
        const INT first = baseVertexIndex + static_cast<INT>(minVertexIndex);
        if (g_probing)
            ProbeDraw("DrawIndexedPrim", prim, first < 0 ? 0u : static_cast<UINT>(first),
                      numVertices, primCount);
        g_state.drawIndex++;
        if (first >= 0 && g_effectOn && g_cfg.effectEnabled && Matches(prim, numVertices) &&
            DrawWithWindShader(dev, true, prim, baseVertexIndex, minVertexIndex, numVertices,
                               startIndex, primCount, 0))
            return S_OK;
        return g_oDrawIdxPrim(dev, prim, baseVertexIndex, minVertexIndex, numVertices, startIndex, primCount);
    }

    void PatchDevice(IDirect3DDevice9* dev)
    {
        if (!dev)
            return;
        Guard g;
        if (g_devHooked)
            return;
        g_devHooked = 1;
        auto* v = const_cast<IDirect3DDevice9Vtbl*>(dev->lpVtbl);

        const bool ok =
            HookSlot(reinterpret_cast<void**>(&v->Present),              &hkPresent,              reinterpret_cast<void**>(&g_oPresent))      &&
            HookSlot(reinterpret_cast<void**>(&v->Reset),                &hkReset,                reinterpret_cast<void**>(&g_oReset))        &&
            HookSlot(reinterpret_cast<void**>(&v->SetTransform),         &hkSetTransform,         reinterpret_cast<void**>(&g_oSetTransform)) &&
            HookSlot(reinterpret_cast<void**>(&v->SetTexture),           &hkSetTexture,           reinterpret_cast<void**>(&g_oSetTexture))   &&
            HookSlot(reinterpret_cast<void**>(&v->SetFVF),               &hkSetFVF,               reinterpret_cast<void**>(&g_oSetFVF))       &&
            HookSlot(reinterpret_cast<void**>(&v->SetRenderState),       &hkSetRenderState,       reinterpret_cast<void**>(&g_oSetRS))        &&
            HookSlot(reinterpret_cast<void**>(&v->SetVertexShader),      &hkSetVertexShader,      reinterpret_cast<void**>(&g_oSetVS))        &&
            HookSlot(reinterpret_cast<void**>(&v->SetVertexDeclaration), &hkSetVertexDeclaration, reinterpret_cast<void**>(&g_oSetDecl))      &&
            HookSlot(reinterpret_cast<void**>(&v->SetStreamSource),      &hkSetStreamSource,      reinterpret_cast<void**>(&g_oSetStream))    &&
            HookSlot(reinterpret_cast<void**>(&v->DrawPrimitive),        &hkDrawPrimitive,        reinterpret_cast<void**>(&g_oDrawPrim))     &&
            HookSlot(reinterpret_cast<void**>(&v->DrawIndexedPrimitive), &hkDrawIndexedPrimitive, reinterpret_cast<void**>(&g_oDrawIdxPrim));

        Log("device %s (vtable %p, Present orig=%p hook=%p)", ok ? "hooked" : "HOOK FAILED",
            v, g_oPresent, &hkPresent);
    }

    // ---------------------------------------------------------------------------------------------
    // attaching to DXVK
    //
    // comfygrass is loaded by VanillaFixes from dlls.txt, like the client's other mods, not as
    // d3d9.dll. So there is no create call of ours to intercept, and nothing in the client either: this
    // WoW.exe does not import d3d9. Its D3D9 path was patched in, and it imports neither
    // Direct3DCreate9 nor GetProcAddress, so the client's import table has nothing to hook.
    //
    // So the vtable is taken from a device of our own instead. Every IDirect3DDevice9 that DXVK hands
    // out shares one class vtable, so patching a slot in that vtable catches the client's device
    // whatever order things happen in, including a device it created before we loaded. The throwaway
    // device exists only to name the vtable and is released immediately.
    //
    // In place matters: handing back a copied vtable drops the RTTI word DXVK keeps behind vtable[0],
    // and the client silently gives up before its own CreateDevice.

    using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

    bool AttachToDxvk()
    {
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9)
            d3d9 = LoadLibraryA("d3d9.dll");
        if (!d3d9)
        {
            Log("FATAL: no d3d9.dll in this process");
            return false;
        }

        // Held for the process lifetime: our hooks live in DXVK's vtable, so if DXVK were ever unloaded
        // and reloaded the vtable would come back clean while our globals still believed they had
        // patched it. The same reasoning pins this module in DllMain.
        HMODULE pin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"d3d9.dll", &pin);

        auto create = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(d3d9, "Direct3DCreate9"));
        if (!create)
        {
            Log("FATAL: d3d9.dll at %p has no Direct3DCreate9", d3d9);
            return false;
        }

        IDirect3D9* d3d = create(D3D_SDK_VERSION);
        if (!d3d)
        {
            Log("FATAL: Direct3DCreate9 returned null");
            return false;
        }

        // A hidden 1x1 window, never shown. Using the desktop window here can drag the client's focus
        // around at start-up, which is not worth risking for something released a millisecond later.
        WNDCLASSEXA wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcA;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.lpszClassName = "comfygrass_probe";
        RegisterClassExA(&wc);
        HWND wnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 1, 1,
                                   nullptr, nullptr, wc.hInstance, nullptr);

        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed         = TRUE;
        pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth  = 1;
        pp.BackBufferHeight = 1;
        pp.hDeviceWindow    = wnd;

        IDirect3DDevice9* probe = nullptr;
        HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                               D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                               D3DCREATE_NOWINDOWCHANGES, &pp, &probe);
        if (FAILED(hr) || !probe)
        {
            Log("FATAL: probe CreateDevice failed hr=0x%08X", hr);
            d3d->lpVtbl->Release(d3d);
            if (wnd) DestroyWindow(wnd);
            return false;
        }

        PatchDevice(probe);

        probe->lpVtbl->Release(probe);
        d3d->lpVtbl->Release(d3d);
        if (wnd)
            DestroyWindow(wnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return true;
    }

    // Off the loader lock: DllMain must not load libraries or create devices, and DXVK brings up Vulkan
    // on the first device.
    DWORD WINAPI AttachThread(LPVOID)
    {
        const double t0 = Now();
        const bool ok = AttachToDxvk();
        Log("attach %s in %.0f ms", ok ? "succeeded" : "FAILED", 1000.0 * (Now() - t0));
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
        DisableThreadLibraryCalls(self);

        // Pin the module. Our hooks are function pointers written into DXVK's vtable, so unloading this
        // image would leave that vtable pointing at freed code. Pinning keeps one instance alive for the
        // process lifetime.
        HMODULE pin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&g_lock), &pin);
        ResolveIniPath(self, g_iniPath, MAX_PATH);
        wcscpy_s(g_logPath, g_iniPath);
        wcscpy_s(wcsrchr(g_logPath, L'\\') + 1, 16, L"comfygrass.log");
        DeleteFileW(g_logPath);
        LoadSettings(g_iniPath);
        g_effectOn = g_cfg.effectEnabled;
        Log("comfygrass loaded (module=%p, effect=%d)", self, g_effectOn ? 1 : 0);

        if (g_cfg.hook)
        {
            HANDLE t = CreateThread(nullptr, 0, AttachThread, nullptr, 0, nullptr);
            if (t)
                CloseHandle(t);
            else
                Log("FATAL: could not start the attach thread");
        }
        else
        {
            Log("hook = 0, so nothing is patched: comfygrass is inert this run");
        }
    }
    return TRUE;
}
