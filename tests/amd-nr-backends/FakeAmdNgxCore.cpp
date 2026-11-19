// SPDX-License-Identifier: GPL-3.0-only
// Contract test fixture mimicking mochizuki0323/DLSSNR-AMD's dlssnr_core.dll
// (17-slot MSVC-ABI NVSDK_NGX_Parameter vtable + Feature 18 entry points).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {

constexpr uint32_t kSuccess = 0x1u;
constexpr uint32_t kFail = 0xBAD00000u;
constexpr uint32_t kInvalidParam = kFail | 5u;
constexpr uint32_t kNotSupported = kFail | 1u;
constexpr uint32_t kFeatureNr = 18u;

struct NgxHandle { unsigned int id = 0; };
struct NgxParameter;

struct NgxParameterVtbl {
    void(__cdecl* SetVoid)(NgxParameter*, const char*, void*);
    void(__cdecl* SetD3D12)(NgxParameter*, const char*, ID3D12Resource*);
    void(__cdecl* SetD3D11)(NgxParameter*, const char*, void*);
    void(__cdecl* SetInt)(NgxParameter*, const char*, int);
    void(__cdecl* SetUInt)(NgxParameter*, const char*, unsigned int);
    void(__cdecl* SetDouble)(NgxParameter*, const char*, double);
    void(__cdecl* SetFloat)(NgxParameter*, const char*, float);
    void(__cdecl* SetULL)(NgxParameter*, const char*, unsigned long long);

    uint32_t(__cdecl* GetVoid)(const NgxParameter*, const char*, void**);
    uint32_t(__cdecl* GetD3D12)(const NgxParameter*, const char*, ID3D12Resource**);
    uint32_t(__cdecl* GetD3D11)(const NgxParameter*, const char*, void**);
    uint32_t(__cdecl* GetInt)(const NgxParameter*, const char*, int*);
    uint32_t(__cdecl* GetUInt)(const NgxParameter*, const char*, unsigned int*);
    uint32_t(__cdecl* GetDouble)(const NgxParameter*, const char*, double*);
    uint32_t(__cdecl* GetFloat)(const NgxParameter*, const char*, float*);
    uint32_t(__cdecl* GetULL)(const NgxParameter*, const char*, unsigned long long*);

    void(__cdecl* Reset)(NgxParameter*);
};

struct Val {
    enum Kind { None, ULL, F, D, UI, I, Ptr } kind = None;
    unsigned long long ull = 0;
    float f = 0.0f;
    double d = 0.0;
    unsigned int ui = 0;
    int i = 0;
    void* ptr = nullptr;
};

struct Params {
    const NgxParameterVtbl* vtbl = nullptr;
    std::mutex lock;
    std::map<std::string, Val> map;
    Params();
};

Params* asParams(NgxParameter* p) { return reinterpret_cast<Params*>(p); }
const Params* asParams(const NgxParameter* p) { return reinterpret_cast<const Params*>(p); }

template <typename T>
uint32_t readNum(const NgxParameter* p, const char* n, T* out) {
    if (!p || !n || !out) return kInvalidParam;
    auto* self = const_cast<Params*>(asParams(p));
    std::lock_guard g(self->lock);
    const auto it = self->map.find(n);
    if (it == self->map.end()) return kInvalidParam;
    switch (it->second.kind) {
    case Val::ULL: *out = static_cast<T>(it->second.ull); return kSuccess;
    case Val::F: *out = static_cast<T>(it->second.f); return kSuccess;
    case Val::D: *out = static_cast<T>(it->second.d); return kSuccess;
    case Val::UI: *out = static_cast<T>(it->second.ui); return kSuccess;
    case Val::I: *out = static_cast<T>(it->second.i); return kSuccess;
    default: return kInvalidParam;
    }
}

uint32_t readPtr(const NgxParameter* p, const char* n, void** out) {
    if (!p || !n || !out) return kInvalidParam;
    auto* self = const_cast<Params*>(asParams(p));
    std::lock_guard g(self->lock);
    const auto it = self->map.find(n);
    if (it == self->map.end() || it->second.kind != Val::Ptr) return kInvalidParam;
    *out = it->second.ptr;
    return kSuccess;
}

void __cdecl SetVoid(NgxParameter* p, const char* n, void* v) { std::lock_guard g(asParams(p)->lock); auto& s = asParams(p)->map[n]; s.kind = Val::Ptr; s.ptr = v; }
void __cdecl SetD3D12(NgxParameter* p, const char* n, ID3D12Resource* v) { SetVoid(p, n, v); }
void __cdecl SetD3D11(NgxParameter* p, const char* n, void* v) { SetVoid(p, n, v); }
void __cdecl SetInt(NgxParameter* p, const char* n, int v) { std::lock_guard g(asParams(p)->lock); auto& s = asParams(p)->map[n]; s.kind = Val::I; s.i = v; }
void __cdecl SetUInt(NgxParameter* p, const char* n, unsigned int v) { std::lock_guard g(asParams(p)->lock); auto& s = asParams(p)->map[n]; s.kind = Val::UI; s.ui = v; }
void __cdecl SetDouble(NgxParameter* p, const char* n, double v) { std::lock_guard g(asParams(p)->lock); auto& s = asParams(p)->map[n]; s.kind = Val::D; s.d = v; }
void __cdecl SetFloat(NgxParameter* p, const char* n, float v) { std::lock_guard g(asParams(p)->lock); auto& s = asParams(p)->map[n]; s.kind = Val::F; s.f = v; }
void __cdecl SetULL(NgxParameter* p, const char* n, unsigned long long v) { std::lock_guard g(asParams(p)->lock); auto& s = asParams(p)->map[n]; s.kind = Val::ULL; s.ull = v; }

uint32_t __cdecl GetVoid(const NgxParameter* p, const char* n, void** v) { return readPtr(p, n, v); }
uint32_t __cdecl GetD3D12(const NgxParameter* p, const char* n, ID3D12Resource** v) { return readPtr(p, n, reinterpret_cast<void**>(v)); }
uint32_t __cdecl GetD3D11(const NgxParameter* p, const char* n, void** v) { return readPtr(p, n, v); }
uint32_t __cdecl GetInt(const NgxParameter* p, const char* n, int* v) { return readNum(p, n, v); }
uint32_t __cdecl GetUInt(const NgxParameter* p, const char* n, unsigned int* v) { return readNum(p, n, v); }
uint32_t __cdecl GetDouble(const NgxParameter* p, const char* n, double* v) { return readNum(p, n, v); }
uint32_t __cdecl GetFloat(const NgxParameter* p, const char* n, float* v) { return readNum(p, n, v); }
uint32_t __cdecl GetULL(const NgxParameter* p, const char* n, unsigned long long* v) { return readNum(p, n, v); }
void __cdecl Reset(NgxParameter* p) { std::lock_guard g(asParams(p)->lock); asParams(p)->map.clear(); }

const NgxParameterVtbl kVtbl = {
    SetVoid, SetD3D12, SetD3D11, SetInt, SetUInt, SetDouble, SetFloat, SetULL,
    GetVoid, GetD3D12, GetD3D11, GetInt, GetUInt, GetDouble, GetFloat, GetULL,
    Reset,
};

Params::Params() : vtbl(&kVtbl) {}

struct Stats {
    uint32_t initCount = 0;
    uint32_t createCount = 0;
    uint32_t evalCount = 0;
    uint32_t releaseCount = 0;
    uint32_t shutdownCount = 0;
    uint32_t lastWidth = 0;
    uint32_t lastHeight = 0;
    int32_t lastPreset = 0;
    uint32_t lastStyle = 0;
    uint32_t lastAutoMask = 0;
    float lastIntensity = 0.0f;
    float lastTone = 0.0f;
    float lastStructure = 0.0f;
    float lastSkin = 0.0f;
};

std::mutex g_statsLock;
Stats g_stats;
std::atomic<unsigned int> g_nextId{1};

} // namespace

#define FAKE_EXPORT extern "C" __declspec(dllexport)

FAKE_EXPORT uint32_t __cdecl NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t*, ID3D12Device* dev, uint32_t, const void*) {
    if (!dev) return kInvalidParam;
    std::lock_guard g(g_statsLock);
    ++g_stats.initCount;
    return kSuccess;
}

FAKE_EXPORT uint32_t __cdecl NVSDK_NGX_D3D12_GetCapabilityParameters(NgxParameter** out) {
    if (!out) return kInvalidParam;
    *out = reinterpret_cast<NgxParameter*>(new Params());
    return kSuccess;
}

FAKE_EXPORT uint32_t __cdecl NVSDK_NGX_D3D12_DestroyParameters(NgxParameter* p) {
    delete asParams(p);
    return kSuccess;
}

FAKE_EXPORT uint32_t __cdecl NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* cmd, uint32_t feature,
                                                           NgxParameter* p, NgxHandle** out) {
    if (!cmd || !p || !out) return kInvalidParam;
    if (feature != kFeatureNr) return kNotSupported;
    unsigned int w = 0, h = 0;
    int preset = 0;
    if (GetUInt(p, "DLSSNR.Width", &w) != kSuccess || GetUInt(p, "DLSSNR.Height", &h) != kSuccess || !w || !h) {
        return kInvalidParam;
    }
    GetInt(p, "DLSSNR.Hint.Render.Preset", &preset);
    auto* handle = new NgxHandle{g_nextId++};
    *out = handle;
    std::lock_guard g(g_statsLock);
    ++g_stats.createCount;
    g_stats.lastWidth = w;
    g_stats.lastHeight = h;
    g_stats.lastPreset = preset;
    return kSuccess;
}

FAKE_EXPORT uint32_t __cdecl NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* cmd, const NgxHandle* handle,
                                                             const NgxParameter* p, void*) {
    if (!cmd || !handle || !p) return kInvalidParam;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* mvec = nullptr;
    ID3D12Resource* depth = nullptr;
    if (GetD3D12(p, "DLSSNR.Color", &color) != kSuccess || GetD3D12(p, "DLSSNR.Output", &output) != kSuccess || !color || !output) {
        return kInvalidParam;
    }
    GetD3D12(p, "DLSSNR.MVec", &mvec);
    GetD3D12(p, "DLSSNR.Depth", &depth);
    if (!mvec || !depth) return kInvalidParam;

    // Mimic real D3D12 NR seed copy on the submitted command list.
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition = {output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST};
    cmd->ResourceBarrier(2, b);
    cmd->CopyResource(output, color);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    cmd->ResourceBarrier(2, b);

    std::lock_guard g(g_statsLock);
    ++g_stats.evalCount;
    GetFloat(p, "DLSSNR.Intensity", &g_stats.lastIntensity);
    GetUInt(p, "DLSSNR.Style", &g_stats.lastStyle);
    GetFloat(p, "DLSSNR.LocalToneStrength", &g_stats.lastTone);
    GetFloat(p, "DLSSNR.LocalStructureStrength", &g_stats.lastStructure);
    GetFloat(p, "DLSSNR.SkinStructureStrength", &g_stats.lastSkin);
    GetUInt(p, "DLSSNR.UseAutoMask", &g_stats.lastAutoMask);
    return kSuccess;
}

FAKE_EXPORT uint32_t __cdecl NVSDK_NGX_D3D12_ReleaseFeature(NgxHandle* handle) {
    delete handle;
    std::lock_guard g(g_statsLock);
    ++g_stats.releaseCount;
    return kSuccess;
}

FAKE_EXPORT uint32_t __cdecl NVSDK_NGX_D3D12_Shutdown1(ID3D12Device*) {
    std::lock_guard g(g_statsLock);
    ++g_stats.shutdownCount;
    return kSuccess;
}

FAKE_EXPORT void __cdecl FakeNgxGetStats(Stats* out) {
    if (!out) return;
    std::lock_guard g(g_statsLock);
    *out = g_stats;
}
