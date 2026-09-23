#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

Settings g_cfg;

namespace
{
    const wchar_t* kWind    = L"wind";
    const wchar_t* kPhysics = L"physics";
    const wchar_t* kModels  = L"models";
    const wchar_t* kMatch   = L"match";
    const wchar_t* kGeneral = L"general";

    float GetF(const wchar_t* sec, const wchar_t* key, float dflt, const wchar_t* ini)
    {
        wchar_t buf[64] = {};
        wchar_t def[64];
        swprintf(def, 64, L"%.6f", dflt);
        GetPrivateProfileStringW(sec, key, def, buf, 64, ini);
        return static_cast<float>(_wtof(buf));
    }

    int GetI(const wchar_t* sec, const wchar_t* key, int dflt, const wchar_t* ini)
    {
        return static_cast<int>(GetPrivateProfileIntW(sec, key, dflt, ini));
    }

    bool GetB(const wchar_t* sec, const wchar_t* key, bool dflt, const wchar_t* ini)
    {
        return GetPrivateProfileIntW(sec, key, dflt ? 1 : 0, ini) != 0;
    }

    // Reads a value that may be written in hex ("0x142") or decimal.
    DWORD GetX(const wchar_t* sec, const wchar_t* key, DWORD dflt, const wchar_t* ini)
    {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
        if (!buf[0])
            return dflt;
        return static_cast<DWORD>(wcstoul(buf, nullptr, 0));
    }

    // Reads an ASCII string. GetPrivateProfileString keeps an inline "; comment", so it is cut here,
    // with the spaces before it.
    void GetS(const wchar_t* sec, const wchar_t* key, char* out, size_t count, const wchar_t* ini)
    {
        wchar_t buf[256] = {};
        GetPrivateProfileStringW(sec, key, L"\x1", buf, 256, ini);
        if (buf[0] == L'\x1')
            return;   // no such key: keep the default already in out
        if (wchar_t* semi = wcschr(buf, L';'))
            *semi = 0;
        size_t n = wcslen(buf);
        while (n && (buf[n - 1] == L' ' || buf[n - 1] == L'\t'))
            buf[--n] = 0;
        WideCharToMultiByte(CP_ACP, 0, buf, -1, out, static_cast<int>(count), nullptr, nullptr);
        out[count - 1] = 0;
    }
}

void ResolveIniPath(HMODULE self, wchar_t* out, size_t count)
{
    GetModuleFileNameW(self, out, static_cast<DWORD>(count));
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash)
        wcscpy_s(slash + 1, count - (slash + 1 - out), L"comfygrass.ini");
}

void LoadSettings(const wchar_t* ini)
{
    Settings s;

    s.wind.enabled         = GetB(kWind, L"enabled",         s.wind.enabled,         ini);
    s.wind.directionDeg    = GetF(kWind, L"directionDeg",    s.wind.directionDeg,    ini);
    s.wind.speed           = GetF(kWind, L"speed",           s.wind.speed,           ini);
    s.wind.amplitude       = GetF(kWind, L"amplitude",       s.wind.amplitude,       ini);
    s.wind.wavelength      = GetF(kWind, L"wavelength",      s.wind.wavelength,      ini);
    s.wind.crossAmplitude  = GetF(kWind, L"crossAmplitude",  s.wind.crossAmplitude,  ini);
    s.wind.crossWavelength = GetF(kWind, L"crossWavelength", s.wind.crossWavelength, ini);
    s.wind.crossAngleDeg   = GetF(kWind, L"crossAngleDeg",   s.wind.crossAngleDeg,   ini);
    s.wind.lean            = GetF(kWind, L"lean",            s.wind.lean,            ini);
    s.wind.variance        = GetF(kWind, L"variance",        s.wind.variance,        ini);
    s.wind.anchor          = GetF(kWind, L"anchor",          s.wind.anchor,          ini);

    s.physics.enabled     = GetB(kPhysics, L"enabled",     s.physics.enabled,     ini);
    s.physics.radius      = GetF(kPhysics, L"radius",      s.physics.radius,      ini);
    s.physics.forceCenter = GetF(kPhysics, L"forceCenter", s.physics.forceCenter, ini);
    s.physics.forceEdge   = GetF(kPhysics, L"forceEdge",   s.physics.forceEdge,   ini);
    s.physics.centerZ     = GetF(kPhysics, L"centerZ",     s.physics.centerZ,     ini);
    s.physics.zRange      = GetF(kPhysics, L"zRange",      s.physics.zRange,      ini);
    s.physics.zFade       = GetF(kPhysics, L"zFade",       s.physics.zFade,       ini);
    s.physics.camAddr     = GetX(kPhysics, L"camAddr",     s.physics.camAddr,     ini);
    s.physics.anchorAddr  = GetX(kPhysics, L"anchorAddr",  s.physics.anchorAddr,  ini);
    s.physics.objMgrAddr   = GetX(kPhysics, L"objMgrAddr",   s.physics.objMgrAddr,   ini);
    s.physics.playerPosOff = GetX(kPhysics, L"playerPosOff", s.physics.playerPosOff, ini);
    s.physics.posScanMax   = GetX(kPhysics, L"posScanMax",   s.physics.posScanMax,   ini);
    s.physics.posScanFrames = GetI(kPhysics, L"posScanFrames", s.physics.posScanFrames, ini);
    s.physics.reportAnchor = GetB(kPhysics, L"reportAnchor", s.physics.reportAnchor, ini);

    s.models.enabled     = GetB(kModels, L"enabled",     s.models.enabled,     ini);
    s.models.rigidHeight = GetF(kModels, L"rigidHeight", s.models.rigidHeight, ini);
    s.models.fillAddr    = GetX(kModels, L"fillAddr",    s.models.fillAddr,    ini);
    GetS(kModels, L"rigidNames", s.models.rigidNames, sizeof(s.models.rigidNames), ini);

    s.match.fvf      = GetX(kMatch, L"fvf",      s.match.fvf,      ini);
    s.match.stride   = GetX(kMatch, L"stride",   s.match.stride,   ini);
    s.match.minVerts = GetX(kMatch, L"minVerts", s.match.minVerts, ini);
    s.match.maxVerts = GetX(kMatch, L"maxVerts", s.match.maxVerts, ini);
    s.match.primType = GetI(kMatch, L"primType", s.match.primType, ini);
    s.match.identityRotation = GetB(kMatch, L"identityRotation", s.match.identityRotation, ini);

    s.effectEnabled = GetB(kGeneral, L"effect",        s.effectEnabled, ini);
    s.logEnabled    = GetB(kGeneral, L"log",           s.logEnabled,    ini);
    s.hook          = GetB(kGeneral, L"hook",          s.hook,          ini);
    s.probeKey      = GetI(kGeneral, L"probeKey",      s.probeKey,      ini);
    s.toggleKey     = GetI(kGeneral, L"toggleKey",     s.toggleKey,     ini);
    s.probeVertices = GetI(kGeneral, L"probeVertices", s.probeVertices, ini);
    s.scale         = GetF(kGeneral, L"scale",         s.scale,         ini);
    s.worldPhase    = GetB(kGeneral, L"worldPhase",    s.worldPhase,    ini);
    s.reportEvery      = GetX(kGeneral, L"reportEvery",      s.reportEvery,      ini);

    g_cfg = s;
}
