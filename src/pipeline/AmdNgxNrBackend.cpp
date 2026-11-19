// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/pipeline/AmdNgxNrBackend.h"
#include "veyra/Log.h"
#include "veyra/pipeline/GpuPassUtils.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace veyra::pipeline {
namespace {

// ---------------------------------------------------------------------------
// Self-contained MSVC-ABI NGX Feature 18 interface.
// Matches both NVIDIA's published nvsdk_ngx_params.h MSVC vtable layout and
// mochizuki0323/DLSSNR-AMD's nr_ngx_abi.hpp (17 slots: 8 Set, 8 Get, Reset).
// ---------------------------------------------------------------------------
constexpr uint32_t kNgxSuccess = 0x1u;
constexpr uint32_t kNgxFail = 0xBAD00000u;
constexpr uint32_t kNgxFailInvalidParameter = kNgxFail | 5u;
constexpr uint32_t kNgxFailPlatformError = kNgxFail | 2u;
constexpr uint32_t kNgxVersionApi = 0x00000015u;
constexpr uint32_t kNgxFeatureNeuralRendering = 18u;

struct NgxHandle {
    unsigned int id = 0;
};

struct NgxParameter;

struct NgxParameterVtbl {
    void(__cdecl* SetVoidPtr)(NgxParameter*, const char*, void*);
    void(__cdecl* SetD3D12Resource)(NgxParameter*, const char*, ID3D12Resource*);
    void(__cdecl* SetD3D11Resource)(NgxParameter*, const char*, void*);
    void(__cdecl* SetInt)(NgxParameter*, const char*, int);
    void(__cdecl* SetUInt)(NgxParameter*, const char*, unsigned int);
    void(__cdecl* SetDouble)(NgxParameter*, const char*, double);
    void(__cdecl* SetFloat)(NgxParameter*, const char*, float);
    void(__cdecl* SetULL)(NgxParameter*, const char*, unsigned long long);

    uint32_t(__cdecl* GetVoidPtr)(const NgxParameter*, const char*, void**);
    uint32_t(__cdecl* GetD3D12Resource)(const NgxParameter*, const char*, ID3D12Resource**);
    uint32_t(__cdecl* GetD3D11Resource)(const NgxParameter*, const char*, void**);
    uint32_t(__cdecl* GetInt)(const NgxParameter*, const char*, int*);
    uint32_t(__cdecl* GetUInt)(const NgxParameter*, const char*, unsigned int*);
    uint32_t(__cdecl* GetDouble)(const NgxParameter*, const char*, double*);
    uint32_t(__cdecl* GetFloat)(const NgxParameter*, const char*, float*);
    uint32_t(__cdecl* GetULL)(const NgxParameter*, const char*, unsigned long long*);

    void(__cdecl* Reset)(NgxParameter*);
};

struct NgxParameter {
    const NgxParameterVtbl* vtbl = nullptr;

    void setVoid(const char* n, void* v) { vtbl->SetVoidPtr(this, n, v); }
    void setD3D12(const char* n, ID3D12Resource* v) { vtbl->SetD3D12Resource(this, n, v); }
    void setInt(const char* n, int v) { vtbl->SetInt(this, n, v); }
    void setUInt(const char* n, unsigned int v) { vtbl->SetUInt(this, n, v); }
    void setFloat(const char* n, float v) { vtbl->SetFloat(this, n, v); }
    void reset() { vtbl->Reset(this); }
};

static_assert(sizeof(NgxParameter) == sizeof(void*), "NgxParameter must be a single vtable pointer");
static_assert(sizeof(NgxParameterVtbl) == 17 * sizeof(void*), "NgxParameterVtbl must have 17 MSVC slots");

// Fallback host-owned parameter block if a DLL does not export
// NVSDK_NGX_D3D12_GetCapabilityParameters / AllocateParameters.
struct HostNgxParameters {
    const NgxParameterVtbl* vtbl = nullptr;
    using Value = std::variant<unsigned long long, double, void*>;
    std::unordered_map<std::string, Value> values;

    HostNgxParameters();
    NgxParameter* asNgx() { return reinterpret_cast<NgxParameter*>(this); }

    template <class T>
    uint32_t getNum(const char* n, T* out) const {
        if (!n || !out) return kNgxFailInvalidParameter;
        const auto it = values.find(n);
        if (it == values.end()) return kNgxFailInvalidParameter;
        if (const auto* d = std::get_if<double>(&it->second)) {
            *out = static_cast<T>(*d);
            return kNgxSuccess;
        }
        if (const auto* u = std::get_if<unsigned long long>(&it->second)) {
            *out = static_cast<T>(*u);
            return kNgxSuccess;
        }
        return kNgxFailInvalidParameter;
    }

    template <class T>
    uint32_t getPtr(const char* n, T** out) const {
        if (!n || !out) return kNgxFailInvalidParameter;
        const auto it = values.find(n);
        if (it == values.end()) return kNgxFailInvalidParameter;
        if (const auto* p = std::get_if<void*>(&it->second)) {
            *out = static_cast<T*>(*p);
            return kNgxSuccess;
        }
        return kNgxFailInvalidParameter;
    }
};

namespace host_vt {
HostNgxParameters* self(NgxParameter* p) { return reinterpret_cast<HostNgxParameters*>(p); }
const HostNgxParameters* self(const NgxParameter* p) { return reinterpret_cast<const HostNgxParameters*>(p); }

void __cdecl SetVoid(NgxParameter* p, const char* n, void* v) { if (n) self(p)->values[n] = v; }
void __cdecl SetD3D12(NgxParameter* p, const char* n, ID3D12Resource* v) { if (n) self(p)->values[n] = static_cast<void*>(v); }
void __cdecl SetD3D11(NgxParameter* p, const char* n, void* v) { if (n) self(p)->values[n] = v; }
void __cdecl SetInt(NgxParameter* p, const char* n, int v) { if (n) self(p)->values[n] = double(v); }
void __cdecl SetUInt(NgxParameter* p, const char* n, unsigned int v) { if (n) self(p)->values[n] = double(v); }
void __cdecl SetDouble(NgxParameter* p, const char* n, double v) { if (n) self(p)->values[n] = v; }
void __cdecl SetFloat(NgxParameter* p, const char* n, float v) { if (n) self(p)->values[n] = double(v); }
void __cdecl SetULL(NgxParameter* p, const char* n, unsigned long long v) { if (n) self(p)->values[n] = v; }

uint32_t __cdecl GetVoid(const NgxParameter* p, const char* n, void** v) { return self(p)->getPtr(n, v); }
uint32_t __cdecl GetD3D12(const NgxParameter* p, const char* n, ID3D12Resource** v) { return self(p)->getPtr(n, v); }
uint32_t __cdecl GetD3D11(const NgxParameter* p, const char* n, void** v) { return self(p)->getPtr(n, v); }
uint32_t __cdecl GetInt(const NgxParameter* p, const char* n, int* v) { return self(p)->getNum(n, v); }
uint32_t __cdecl GetUInt(const NgxParameter* p, const char* n, unsigned int* v) { return self(p)->getNum(n, v); }
uint32_t __cdecl GetDouble(const NgxParameter* p, const char* n, double* v) { return self(p)->getNum(n, v); }
uint32_t __cdecl GetFloat(const NgxParameter* p, const char* n, float* v) { return self(p)->getNum(n, v); }
uint32_t __cdecl GetULL(const NgxParameter* p, const char* n, unsigned long long* v) {
    if (!n || !v) return kNgxFailInvalidParameter;
    void* ptr = nullptr;
    if (self(p)->getPtr(n, &ptr) == kNgxSuccess) {
        *v = reinterpret_cast<unsigned long long>(ptr);
        return kNgxSuccess;
    }
    return self(p)->getNum(n, v);
}
void __cdecl Reset(NgxParameter* p) { self(p)->values.clear(); }

const NgxParameterVtbl kTable = {
    SetVoid, SetD3D12, SetD3D11, SetInt, SetUInt, SetDouble, SetFloat, SetULL,
    GetVoid, GetD3D12, GetD3D11, GetInt, GetUInt, GetDouble, GetFloat, GetULL,
    Reset,
};
} // namespace host_vt

HostNgxParameters::HostNgxParameters() : vtbl(&host_vt::kTable) {}

using InitExtFn = uint32_t(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, uint32_t, const void*);
using InitFn = uint32_t(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, uint32_t);
using GetCapsParamsFn = uint32_t(__cdecl*)(NgxParameter**);
using DestroyParamsFn = uint32_t(__cdecl*)(NgxParameter*);
using CreateFeatureFn = uint32_t(__cdecl*)(ID3D12GraphicsCommandList*, uint32_t, NgxParameter*, NgxHandle**);
using EvaluateFeatureFn = uint32_t(__cdecl*)(ID3D12GraphicsCommandList*, const NgxHandle*, NgxParameter*, void*);
using ReleaseFeatureFn = uint32_t(__cdecl*)(NgxHandle*);
using Shutdown1Fn = uint32_t(__cdecl*)(ID3D12Device*);
using ShutdownFn = uint32_t(__cdecl*)();

uint32_t safeInitExt(InitExtFn fn, ID3D12Device* device, const wchar_t* dataDir, unsigned& seh) {
    __try {
        return fn(1ull, dataDir, device, kNgxVersionApi, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return kNgxFailPlatformError;
    }
}

uint32_t safeInit(InitFn fn, ID3D12Device* device, const wchar_t* dataDir, unsigned& seh) {
    __try {
        return fn(1ull, dataDir, device, nullptr, kNgxVersionApi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return kNgxFailPlatformError;
    }
}

uint32_t safeCreate(CreateFeatureFn fn, ID3D12GraphicsCommandList* cmd, NgxParameter* params, NgxHandle** out, unsigned& seh) {
    __try {
        return fn(cmd, kNgxFeatureNeuralRendering, params, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return kNgxFailPlatformError;
    }
}

uint32_t safeEvaluate(EvaluateFeatureFn fn, ID3D12GraphicsCommandList* cmd, const NgxHandle* handle, NgxParameter* params, unsigned& seh) {
    __try {
        return fn(cmd, handle, params, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return kNgxFailPlatformError;
    }
}

uint32_t safeRelease(ReleaseFeatureFn fn, NgxHandle* handle, unsigned& seh) {
    __try {
        return fn(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return kNgxFailPlatformError;
    }
}

uint32_t safeShutdown(Shutdown1Fn s1, ShutdownFn s0, ID3D12Device* device, unsigned& seh) {
    __try {
        if (s1) return s1(device);
        if (s0) return s0();
        return kNgxSuccess;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return kNgxFailPlatformError;
    }
}

struct SharedNgxCore {
    HMODULE module = nullptr;
    ID3D12Device* device = nullptr;
    bool initialized = false;
    GetCapsParamsFn getCaps = nullptr;
    GetCapsParamsFn allocParams = nullptr;
    DestroyParamsFn destroyParams = nullptr;
    CreateFeatureFn create = nullptr;
    EvaluateFeatureFn evaluate = nullptr;
    ReleaseFeatureFn release = nullptr;
    Shutdown1Fn shutdown1 = nullptr;
    ShutdownFn shutdown0 = nullptr;

    ~SharedNgxCore() {
        if (initialized && (shutdown1 || shutdown0)) {
            unsigned seh = 0;
            const uint32_t res = safeShutdown(shutdown1, shutdown0, device, seh);
            log::info("amd-nr-ngx", std::format("Shutdown last device owner result=0x{:X} seh=0x{:X}", res, seh));
        }
        if (module) FreeLibrary(module);
        if (device) device->Release();
    }
};

std::mutex g_coreMutex;
std::map<std::pair<ID3D12Device*, std::wstring>, std::weak_ptr<SharedNgxCore>> g_cores;

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}

std::wstring readEnvW(const wchar_t* name) {
    wchar_t buf[32768]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    if (!n || n >= std::size(buf)) return {};
    return std::wstring(buf, n);
}

std::string readEnvA(const char* name) {
    char buf[1024]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, DWORD(std::size(buf)));
    if (!n || n >= std::size(buf)) return {};
    return std::string(buf, n);
}

std::filesystem::path readPointerFile(const std::filesystem::path& pointerFile) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pointerFile, ec)) return {};
    std::ifstream in(pointerFile, std::ios::binary);
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        const auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        const auto end = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(start, end - start + 1);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
        const int needed = MultiByteToWideChar(CP_UTF8, 0, trimmed.data(), int(trimmed.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring wide(size_t(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, trimmed.data(), int(trimmed.size()), wide.data(), needed);
        const std::filesystem::path candidate(wide);
        if (candidate.is_absolute()) return candidate.lexically_normal();
        return {};
    }
    return {};
}

std::filesystem::path findCandidateDll(const std::filesystem::path& dir, AmdNgxNrProviderKind kind) {
    std::error_code ec;
    if (dir.empty()) return {};
    if (std::filesystem::is_regular_file(dir, ec)) return dir.lexically_normal();
    if (!std::filesystem::is_directory(dir, ec)) return {};

    const wchar_t* const mochizukiNames[] = {L"dlssnr_core.dll", L"_nvngx.dll", L"nvngx.dll"};
    const wchar_t* const neuralHubNames[] = {L"dlssnr_core.dll", L"_nvngx.dll", L"nvngx.dll", L"nvngx_dlssnr.dll"};

    const auto searchIn = [&](const std::filesystem::path& folder) -> std::filesystem::path {
        if (kind == AmdNgxNrProviderKind::Mochizuki) {
            for (const wchar_t* name : mochizukiNames) {
                const auto p = folder / name;
                if (std::filesystem::is_regular_file(p, ec)) return p.lexically_normal();
            }
        } else {
            for (const wchar_t* name : neuralHubNames) {
                const auto p = folder / name;
                if (std::filesystem::is_regular_file(p, ec)) return p.lexically_normal();
            }
        }
        return {};
    };

    if (auto direct = searchIn(dir); !direct.empty()) return direct;
    if (kind == AmdNgxNrProviderKind::NeuralHub) {
        for (const wchar_t* sub : {L"runtime", L"bin", L"dlss"}) {
            if (auto nested = searchIn(dir / sub); !nested.empty()) return nested;
        }
    }
    return {};
}

const char* providerTag(AmdNgxNrProviderKind kind) {
    return kind == AmdNgxNrProviderKind::Mochizuki ? "amd-nr-mochizuki" : "amd-nr-neuralhub";
}

bool waitFenceCpu(ID3D12Fence* fence, uint64_t value, DWORD timeoutMs) {
    if (fence->GetCompletedValue() >= value) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const bool ok = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
                    WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    return ok;
}

} // namespace

struct AmdNgxNrBackend::Impl {
    std::shared_ptr<SharedNgxCore> core;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> proxy;
    ComPtr<ID3D12Resource> neural;
    ComPtr<ID3D12Resource> decoded;
    ComPtr<ID3D12Resource> zeroMotion;
    ComPtr<ID3D12Resource> zeroDepth;
    ComputePass encode;
    ComputePass decode;
    DescriptorStager views;
    ID3D12Resource* boundColor = nullptr;
    bool bound = false;
    bool useParityShaders = true;

    NgxParameter* params = nullptr;
    bool paramsOwnedByCore = false;
    std::unique_ptr<HostNgxParameters> hostParams;
    NgxHandle* handle = nullptr;

    uint32_t width = 0;
    uint32_t height = 0;
    int32_t preset = 2;
    AmdNgxNrProviderKind kind = AmdNgxNrProviderKind::Mochizuki;
    FrameControls controls{};

    bool opened = false;
    bool prepared = false;
    bool enqueued = false;
    bool outputsRecorded = false;
    std::string error;

    bool fail(std::string message) {
        error = std::move(message);
        log::error(providerTag(kind), error);
        return false;
    }

    void releaseFeatureAndParams() noexcept {
        if (handle && core && core->release) {
            unsigned seh = 0;
            const uint32_t res = safeRelease(core->release, handle, seh);
            if (res != kNgxSuccess || seh) {
                log::warn(providerTag(kind), std::format("ReleaseFeature result=0x{:X} seh=0x{:X}", res, seh));
            }
        }
        handle = nullptr;
        if (params && paramsOwnedByCore && core && core->destroyParams) {
            core->destroyParams(params);
        }
        params = nullptr;
        paramsOwnedByCore = false;
        hostParams.reset();
    }

    ~Impl() {
        releaseFeatureAndParams();
        std::lock_guard lock(g_coreMutex);
        core.reset();
    }
};

AmdNgxNrBackend::AmdNgxNrBackend() : impl_(std::make_unique<Impl>()) {}
AmdNgxNrBackend::~AmdNgxNrBackend() { close(); }

std::filesystem::path AmdNgxNrBackend::defaultDirectory(const std::filesystem::path& runtimeRoot,
                                                        AmdNgxNrProviderKind kind) {
    const wchar_t* folder = kind == AmdNgxNrProviderKind::Mochizuki ? L"amd-nr-mochizuki" : L"amd-nr-neuralhub";
    return (runtimeRoot / L".." / folder).lexically_normal();
}

std::filesystem::path AmdNgxNrBackend::resolveRuntimeDll(const std::filesystem::path& runtimeRoot,
                                                         AmdNgxNrProviderKind kind) {
    const wchar_t* envKey = kind == AmdNgxNrProviderKind::Mochizuki
        ? L"VEYRA_AMD_NR_MOCHIZUKI_DIR"
        : L"VEYRA_AMD_NR_NEURALHUB_DIR";
    if (const auto envVal = readEnvW(envKey); !envVal.empty()) {
        const std::filesystem::path envPath(envVal);
        if (envPath.is_absolute()) {
            if (auto found = findCandidateDll(envPath, kind); !found.empty()) return found;
        }
    }

    const auto baseDir = defaultDirectory(runtimeRoot, kind);
    const wchar_t* pointerName = kind == AmdNgxNrProviderKind::Mochizuki ? L"runtime-path.txt" : L"neuralhub-path.txt";
    if (const auto pointed = readPointerFile(baseDir / pointerName); !pointed.empty()) {
        if (auto found = findCandidateDll(pointed, kind); !found.empty()) return found;
    }

    if (auto found = findCandidateDll(baseDir, kind); !found.empty()) return found;

    if (kind == AmdNgxNrProviderKind::NeuralHub) {
        if (const auto localAppData = readEnvW(L"LOCALAPPDATA"); !localAppData.empty()) {
            const auto hubDir = std::filesystem::path(localAppData) / L"NeuralHub";
            if (auto found = findCandidateDll(hubDir, kind); !found.empty()) return found;
        }
    }
    return {};
}

bool AmdNgxNrBackend::runtimeAvailable(const std::filesystem::path& runtimeRoot,
                                       AmdNgxNrProviderKind kind) {
    return !resolveRuntimeDll(runtimeRoot, kind).empty();
}

std::string AmdNgxNrBackend::selectMochizukiModelName(bool performance) {
    if (const auto overrideName = readEnvA("VEYRA_MOCHIZUKI_MODEL"); !overrideName.empty()) {
        return overrideName;
    }
    if (performance) {
        const auto exp = readEnvA("VEYRA_MOCHIZUKI_EXPERIMENTAL");
        if (exp == "1") return "CC_SMALL_FP8_EXPERIMENTAL";
        return "CC_SMALL_FP8";
    }
    return "CC_SILVER_AARDWOLD";
}

bool AmdNgxNrBackend::writeMochizukiNetworkKey(const std::filesystem::path& iniPath,
                                               const std::string& key,
                                               const std::string& value) {
    if (iniPath.empty() || key.empty() || value.empty()) return false;
    std::string text;
    {
        std::ifstream in(iniPath, std::ios::binary);
        if (in) {
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const auto trim = [](const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string{};
        const size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };

    const std::string wantedKey = lower(key);
    std::string out;
    out.reserve(text.size() + 64);
    std::string section;
    bool updated = false;
    for (size_t at = 0; at < text.size();) {
        const size_t eol = text.find('\n', at);
        const std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at + 1);
        at = eol == std::string::npos ? text.size() : eol + 1;
        const size_t commentPos = line.find_first_of(";#");
        const std::string stripped = trim(commentPos == std::string::npos ? line : line.substr(0, commentPos));
        if (!stripped.empty() && stripped.front() == '[' && stripped.back() == ']') {
            if (!updated && lower(section) == "network") {
                out += key + " = " + value + "\r\n";
                updated = true;
            }
            section = trim(stripped.substr(1, stripped.size() - 2));
        } else if (!updated && lower(section) == "network") {
            const size_t eq = stripped.find('=');
            if (eq != std::string::npos && lower(trim(stripped.substr(0, eq))) == wantedKey) {
                out += key + " = " + value + "\r\n";
                updated = true;
                continue;
            }
        }
        out += line;
    }
    if (!updated) {
        if (!out.empty() && out.back() != '\n') out += "\r\n";
        if (lower(section) != "network") out += "[Network]\r\n";
        out += key + " = " + value + "\r\n";
    }

    std::error_code ec;
    if (iniPath.has_parent_path()) std::filesystem::create_directories(iniPath.parent_path(), ec);
    std::ofstream file(iniPath, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(out.data(), std::streamsize(out.size()));
    return file.good();
}

bool AmdNgxNrBackend::open(const std::filesystem::path& runtimeRoot,
                           ID3D12Device* device,
                           ID3D12CommandQueue* queue,
                           uint32_t vendor,
                           uint32_t width,
                           uint32_t height,
                           bool performance,
                           AmdNgxNrProviderKind kind) {
    close();
    impl_ = std::make_unique<Impl>();
    impl_->kind = kind;
    if (vendor != 0x1002 || !device || !queue || !runtimeRoot.is_absolute() || !validExtent(width, height)) {
        return impl_->fail("AMD NGX NR requires an AMD adapter, valid D3D12 device/queue, absolute runtime path, and extent <= 4096x2160");
    }
    const auto dllPath = resolveRuntimeDll(runtimeRoot, kind);
    if (dllPath.empty()) {
        return impl_->fail(kind == AmdNgxNrProviderKind::Mochizuki
            ? "Mochizuki DLSSNR-AMD runtime missing: stage dlssnr_core.dll into runtime_local/amd-nr-mochizuki or set VEYRA_AMD_NR_MOCHIZUKI_DIR"
            : "DLSS-NR-on-AMD (NeuralHub) runtime missing: install NeuralHub locally and run stage-neuralhub-runtime.py or set VEYRA_AMD_NR_NEURALHUB_DIR");
    }
    return openInternal(dllPath, device, queue, width, height, performance, kind, true);
}

bool AmdNgxNrBackend::openForContractTest(const std::filesystem::path& dllPath,
                                          ID3D12Device* device,
                                          ID3D12CommandQueue* queue,
                                          uint32_t width,
                                          uint32_t height,
                                          bool performance,
                                          AmdNgxNrProviderKind kind,
                                          bool useParityShaders) {
    close();
    impl_ = std::make_unique<Impl>();
    impl_->kind = kind;
    if (!device || !queue || !dllPath.is_absolute() || !validExtent(width, height)) {
        return impl_->fail("invalid contract test arguments for AmdNgxNrBackend");
    }
    return openInternal(dllPath, device, queue, width, height, performance, kind, useParityShaders);
}

bool AmdNgxNrBackend::openInternal(const std::filesystem::path& dllPath,
                                   ID3D12Device* device,
                                   ID3D12CommandQueue* queue,
                                   uint32_t width,
                                   uint32_t height,
                                   bool performance,
                                   AmdNgxNrProviderKind kind,
                                   bool useParityShaders) {
    auto& s = *impl_;
    s.kind = kind;
    s.device = device;
    s.queue = queue;
    s.width = width;
    s.height = height;
    s.preset = performance ? 2 : 1;
    s.useParityShaders = useParityShaders;

    const auto normalizedDll = std::filesystem::absolute(dllPath).lexically_normal();
    const auto dllDir = normalizedDll.parent_path();

    // Sync [Network] Model in dlssnr-amd.ini before feature creation for Mochizuki
    // so the user's Performance switch (CC_SMALL_FP8 vs CC_SILVER_AARDWOLD) takes
    // effect while preserving ACO Mode, OptimizedLayers, and all other INI settings.
    if (kind == AmdNgxNrProviderKind::Mochizuki) {
        const auto modelName = selectMochizukiModelName(performance);
        const auto iniPath = dllDir / L"dlssnr-amd.ini";
        if (std::filesystem::exists(iniPath)) {
            if (!writeMochizukiNetworkKey(iniPath, "Model", modelName)) {
                log::warn(providerTag(kind), std::format("could not update [Network] Model={} in {}", modelName, iniPath.string()));
            }
        }
    }

    // Acquire or initialize the device-scoped NGX core module.
    {
        std::lock_guard lock(g_coreMutex);
        for (auto it = g_cores.begin(); it != g_cores.end();) {
            if (it->second.expired()) it = g_cores.erase(it);
            else ++it;
        }
        const auto key = std::make_pair(device, normalizedDll.wstring());
        s.core = g_cores[key].lock();
        if (!s.core) {
            auto core = std::make_shared<SharedNgxCore>();
            core->module = LoadLibraryExW(normalizedDll.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!core->module) {
                return s.fail(std::format("{} load failed win32={}", normalizedDll.filename().string(), GetLastError()));
            }
            const auto initExt = reinterpret_cast<InitExtFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_Init_Ext"));
            const auto initBasic = reinterpret_cast<InitFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_Init"));
            core->getCaps = reinterpret_cast<GetCapsParamsFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
            core->allocParams = reinterpret_cast<GetCapsParamsFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_AllocateParameters"));
            core->destroyParams = reinterpret_cast<DestroyParamsFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_DestroyParameters"));
            core->create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_CreateFeature"));
            core->evaluate = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_EvaluateFeature"));
            core->release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_ReleaseFeature"));
            core->shutdown1 = reinterpret_cast<Shutdown1Fn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_Shutdown1"));
            core->shutdown0 = reinterpret_cast<ShutdownFn>(GetProcAddress(core->module, "NVSDK_NGX_D3D12_Shutdown"));
            if ((!initExt && !initBasic) || !core->create || !core->evaluate || !core->release) {
                return s.fail(std::format("{} is missing required NVSDK_NGX_D3D12_* Feature 18 exports", normalizedDll.filename().string()));
            }
            unsigned seh = 0;
            const uint32_t initResult = initExt
                ? safeInitExt(initExt, device, dllDir.c_str(), seh)
                : safeInit(initBasic, device, dllDir.c_str(), seh);
            if (initResult != kNgxSuccess || seh) {
                return s.fail(std::format("NVSDK_NGX_D3D12_Init failed result=0x{:X} seh=0x{:X}", initResult, seh));
            }
            core->initialized = true;
            core->device = device;
            device->AddRef();
            s.core = core;
            g_cores[key] = core;
        }
    }

    // Allocate the NGX parameter block (prefer the core's own block so
    // dlssnr_core.dll takes its fast verified Reader::is_ours() path).
    if (s.core->getCaps && s.core->getCaps(&s.params) == kNgxSuccess && s.params) {
        s.paramsOwnedByCore = true;
    } else if (s.core->allocParams && s.core->allocParams(&s.params) == kNgxSuccess && s.params) {
        s.paramsOwnedByCore = true;
    } else {
        s.hostParams = std::make_unique<HostNgxParameters>();
        s.params = s.hostParams->asNgx();
        s.paramsOwnedByCore = false;
    }

    // Allocate textures for display-encoded NGX Feature 18 I/O and linear decode.
    const DXGI_FORMAT proxyFmt = useParityShaders ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    s.proxy = makeTexture(device, width, height, proxyFmt, true);
    s.neural = makeTexture(device, width, height, proxyFmt, true);
    s.decoded = makeTexture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    s.zeroMotion = makeTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, true);
    s.zeroDepth = makeTexture(device, width, height, DXGI_FORMAT_R32_FLOAT, true);
    if (!s.proxy || !s.neural || !s.decoded || !s.zeroMotion || !s.zeroDepth) {
        return s.fail("AMD NGX NR texture allocation failed");
    }

    if (useParityShaders) {
        std::vector<uint8_t> code;
        if (!s.encode.loadShader("ParityEncode.dxil", code) || !s.encode.create(device, code, 2, 1, 1) ||
            !s.decode.loadShader("ParityDecode.dxil", code) || !s.decode.create(device, code, 4, 3, 1) ||
            !s.views.initialize(device, 8)) {
            return s.fail("AMD NGX NR parity shader setup failed (ParityEncode.dxil / ParityDecode.dxil)");
        }
    }

    // Populate Feature 18 creation parameters and execute CreateFeature on a
    // short-lived direct command list, then drain initialization cleanly.
    ComPtr<ID3D12CommandAllocator> initAlloc;
    ComPtr<ID3D12GraphicsCommandList> initList;
    ComPtr<ID3D12Fence> initFence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&initAlloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, initAlloc.Get(), nullptr, IID_PPV_ARGS(&initList))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&initFence)))) {
        return s.fail("AMD NGX NR init command list creation failed");
    }

    s.params->setUInt("DLSSNR.Width", width);
    s.params->setUInt("DLSSNR.Height", height);
    s.params->setInt("DLSSNR.Hint.Render.Preset", s.preset);
    s.params->setUInt("DLSSNR.UICorrection", 1u);
    s.params->setFloat("DLSSNR.Intensity", 1.0f);
    s.params->setUInt("DLSSNR.Style", 0u);
    s.params->setFloat("DLSSNR.LocalToneStrength", 1.0f);
    s.params->setFloat("DLSSNR.LocalStructureStrength", 1.0f);
    s.params->setFloat("DLSSNR.SkinStructureStrength", -1.0f);
    s.params->setUInt("DLSSNR.UseAutoMask", 0u);
    s.params->setUInt("CreationNodeMask", 1u);
    s.params->setUInt("VisibilityNodeMask", 1u);

    unsigned seh = 0;
    const uint32_t createResult = safeCreate(s.core->create, initList.Get(), s.params, &s.handle, seh);
    if (createResult != kNgxSuccess || !s.handle || seh) {
        initList->Close();
        return s.fail(std::format("NVSDK_NGX_D3D12_CreateFeature(18) failed result=0x{:X} seh=0x{:X}", createResult, seh));
    }

    if (FAILED(initList->Close())) {
        return s.fail("AMD NGX NR init command list close failed");
    }
    ID3D12CommandList* submitted[] = {initList.Get()};
    queue->ExecuteCommandLists(1, submitted);
    if (FAILED(queue->Signal(initFence.Get(), 1)) || !waitFenceCpu(initFence.Get(), 1, 30000)) {
        return s.fail("AMD NGX NR init GPU drain timed out");
    }

    s.opened = true;
    log::info(providerTag(kind), std::format("opened {} internal={}x{} preset={} parity={} handleId={}",
        normalizedDll.filename().string(), width, height, s.preset, useParityShaders ? 1 : 0, s.handle->id));
    return true;
}

bool AmdNgxNrBackend::ready() const {
    return impl_ && impl_->opened && impl_->handle != nullptr;
}

bool AmdNgxNrBackend::admits(uint32_t width, uint32_t height) const {
    return ready() && validExtent(width, height) && width == impl_->width && height == impl_->height;
}

void AmdNgxNrBackend::setFrameControls(const FrameControls& controls) {
    if (impl_) impl_->controls = controls;
}

bool AmdNgxNrBackend::recordInputs(ID3D12GraphicsCommandList* list,
                                   ID3D12Resource* color,
                                   uint64_t frame,
                                   bool reset,
                                   float intensity) {
    auto& s = *impl_;
    if (!ready() || s.prepared || !list || !color || !frame || !std::isfinite(intensity)) {
        return s.fail("invalid/unretired AMD NGX NR frame transaction");
    }
    const auto desc = color->GetDesc();
    if (desc.Width != s.width || desc.Height != s.height || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        return s.fail("AMD NGX NR requires linear RGBA16F matching the layer extent");
    }

    if (s.useParityShaders) {
        if (s.bound && s.boundColor != color) {
            return s.fail("AMD NGX NR input resource changed without rebuilding the provider");
        }
        if (!s.bound) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            auto bindSrv = [&](ComputePass& pass, UINT slot, ID3D12Resource* res) {
                srv.Format = res->GetDesc().Format;
                s.views.stageSrv(res, &srv, pass.heap.Get(), slot);
            };
            auto bindUav = [&](ComputePass& pass, UINT slot, ID3D12Resource* res) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Format = res->GetDesc().Format;
                auto cpu = pass.heap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += SIZE_T(slot) * pass.increment;
                s.device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
            };
            bindSrv(s.encode, 0, color);
            bindUav(s.encode, 1, s.proxy.Get());
            bindSrv(s.decode, 0, color);
            bindSrv(s.decode, 1, s.proxy.Get());
            bindSrv(s.decode, 2, s.neural.Get());
            bindUav(s.decode, 3, s.decoded.Get());
            s.bound = true;
            s.boundColor = color;
        }
        barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        float constants[8] = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        std::memcpy(constants + 4, &s.width, sizeof(uint32_t));
        std::memcpy(constants + 5, &s.height, sizeof(uint32_t));
        const auto gpu = s.encode.heap->GetGPUDescriptorHandleForHeapStart();
        s.encode.bind(list, constants, gpu.ptr, gpu.ptr + s.encode.increment);
        list->Dispatch((s.width + 15) / 16, (s.height + 15) / 16, 1);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = s.proxy.Get();
        list->ResourceBarrier(1, &uav);
        barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
        barrier(list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(s.proxy.Get(), color);
        barrier(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    barrier(list, s.neural.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12Resource* motionRes = s.controls.motion ? s.controls.motion : s.zeroMotion.Get();
    ID3D12Resource* depthRes = s.controls.depth ? s.controls.depth : s.zeroDepth.Get();
    const bool usingOwnMotion = motionRes == s.zeroMotion.Get();
    const bool usingOwnDepth = depthRes == s.zeroDepth.Get();
    if (usingOwnMotion) barrier(list, motionRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (usingOwnDepth) barrier(list, depthRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const float clampedIntensity = std::clamp(intensity, 0.0f, 1.0f);
    const float clampedTone = std::isfinite(s.controls.localTone) ? std::clamp(s.controls.localTone, 0.0f, 1.0f) : 1.0f;
    const float clampedStructure = std::isfinite(s.controls.localStructure) ? std::clamp(s.controls.localStructure, 0.0f, 1.0f) : 1.0f;
    const float clampedSkin = std::isfinite(s.controls.skinStructure)
        ? (s.controls.skinStructure < 0.0f ? -1.0f : std::clamp(s.controls.skinStructure, 0.0f, 2.0f))
        : -1.0f;
    const unsigned int clampedStyle = static_cast<unsigned int>(std::clamp(s.controls.style, 0, 2));
    const unsigned int autoMask = s.controls.autoMask ? 1u : 0u;
    const unsigned int uiCorr = s.controls.uiCorrection ? 1u : 0u;

    s.params->setUInt("DLSSNR.Width", s.width);
    s.params->setUInt("DLSSNR.Height", s.height);
    s.params->setD3D12("DLSSNR.Color", s.proxy.Get());
    s.params->setD3D12("DLSSNR.Output", s.neural.Get());
    s.params->setD3D12("DLSSNR.MVec", motionRes);
    s.params->setD3D12("DLSSNR.Depth", depthRes);
    s.params->setUInt("DLSSNR.ColorSubrectBaseX", 0u);
    s.params->setUInt("DLSSNR.ColorSubrectBaseY", 0u);
    s.params->setUInt("DLSSNR.ColorSubrectWidth", s.width);
    s.params->setUInt("DLSSNR.ColorSubrectHeight", s.height);
    s.params->setUInt("DLSSNR.OutputSubrectBaseX", 0u);
    s.params->setUInt("DLSSNR.OutputSubrectBaseY", 0u);
    s.params->setUInt("DLSSNR.OutputSubrectWidth", s.width);
    s.params->setUInt("DLSSNR.OutputSubrectHeight", s.height);
    s.params->setUInt("DLSSNR.MVecSubrectBaseX", 0u);
    s.params->setUInt("DLSSNR.MVecSubrectBaseY", 0u);
    s.params->setUInt("DLSSNR.MVecSubrectWidth", s.width);
    s.params->setUInt("DLSSNR.MVecSubrectHeight", s.height);
    s.params->setUInt("DLSSNR.DepthSubrectBaseX", 0u);
    s.params->setUInt("DLSSNR.DepthSubrectBaseY", 0u);
    s.params->setUInt("DLSSNR.DepthSubrectWidth", s.width);
    s.params->setUInt("DLSSNR.DepthSubrectHeight", s.height);
    s.params->setUInt("DLSSNR.DepthInverted", 0u);
    s.params->setUInt("DLSSNR.Reset", reset ? 1u : 0u);
    s.params->setFloat("DLSSNR.MVecScaleX", 1.0f);
    s.params->setFloat("DLSSNR.MVecScaleY", 1.0f);
    s.params->setFloat("DLSSNR.Intensity", clampedIntensity);
    s.params->setUInt("DLSSNR.Style", clampedStyle);
    s.params->setFloat("DLSSNR.LocalToneStrength", clampedTone);
    s.params->setFloat("DLSSNR.LocalStructureStrength", clampedStructure);
    s.params->setFloat("DLSSNR.SkinStructureStrength", clampedSkin);
    s.params->setUInt("DLSSNR.UseAutoMask", autoMask);
    s.params->setUInt("DLSSNR.UICorrection", uiCorr);

    unsigned seh = 0;
    const uint32_t evalResult = safeEvaluate(s.core->evaluate, list, s.handle, s.params, seh);

    if (usingOwnMotion) barrier(list, motionRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (usingOwnDepth) barrier(list, depthRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    if (evalResult != kNgxSuccess || seh) {
        barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(list, s.neural.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        return s.fail(std::format("NVSDK_NGX_D3D12_EvaluateFeature(18) failed result=0x{:X} seh=0x{:X}", evalResult, seh));
    }

    D3D12_RESOURCE_BARRIER neuralBarrier{};
    neuralBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    neuralBarrier.UAV.pResource = s.neural.Get();
    list->ResourceBarrier(1, &neuralBarrier);
    barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(list, s.neural.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    s.prepared = true;
    s.enqueued = false;
    s.outputsRecorded = false;
    s.error.clear();
    return true;
}

bool AmdNgxNrBackend::enqueue() {
    auto& s = *impl_;
    if (!ready() || !s.prepared || s.enqueued) {
        return s.fail("AMD NGX NR enqueue called without a prepared frame");
    }
    // Work was recorded on Veyra's direct command list and submitted by submitGraph().
    s.enqueued = true;
    return true;
}

bool AmdNgxNrBackend::recordOutputs(ID3D12GraphicsCommandList* list, ID3D12Resource* destination) {
    auto& s = *impl_;
    if (!ready() || !s.enqueued || s.outputsRecorded || !list || !destination) {
        return s.fail("AMD NGX NR recordOutputs called without an evaluated frame");
    }
    if (s.useParityShaders) {
        barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list, s.neural.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list, s.decoded.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        float constants[8] = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        std::memcpy(constants + 4, &s.width, sizeof(uint32_t));
        std::memcpy(constants + 5, &s.height, sizeof(uint32_t));
        const auto gpu = s.decode.heap->GetGPUDescriptorHandleForHeapStart();
        s.decode.bind(list, constants, gpu.ptr, gpu.ptr + 3 * s.decode.increment);
        list->Dispatch((s.width + 15) / 16, (s.height + 15) / 16, 1);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = s.decoded.Get();
        list->ResourceBarrier(1, &uav);
        barrier(list, s.decoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(destination, s.decoded.Get());
        barrier(list, s.decoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(list, s.proxy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(list, s.neural.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    } else {
        barrier(list, s.neural.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(destination, s.neural.Get());
        barrier(list, s.neural.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    s.outputsRecorded = true;
    return true;
}

bool AmdNgxNrBackend::retireSubmitted() {
    auto& s = *impl_;
    if (!s.outputsRecorded) return true;
    if (!ready()) return false;
    s.prepared = false;
    s.enqueued = false;
    s.outputsRecorded = false;
    return true;
}

const std::string& AmdNgxNrBackend::error() const {
    return impl_->error;
}

void AmdNgxNrBackend::close() noexcept {
    if (!impl_) return;
    impl_->releaseFeatureAndParams();
    {
        std::lock_guard lock(g_coreMutex);
        impl_->core.reset();
    }
    impl_->opened = false;
    impl_->prepared = false;
    impl_->enqueued = false;
    impl_->outputsRecorded = false;
    impl_->bound = false;
    impl_->boundColor = nullptr;
}

} // namespace veyra::pipeline
