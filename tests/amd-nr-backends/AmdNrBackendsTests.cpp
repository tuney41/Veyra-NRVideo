// SPDX-License-Identifier: GPL-3.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "veyra/engine/EnhancementSettings.h"
#include "veyra/engine/ExportFeaturePolicy.h"
#include "veyra/engine/GraphDescription.h"
#include "veyra/pipeline/AmdNgxNrBackend.h"
#include "veyra/pipeline/AmdNrProvider.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace veyra::engine;
using namespace veyra::pipeline;

namespace {

struct FakeStats {
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

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

bool createWarpDevice(ComPtr<ID3D12Device>& device, ComPtr<ID3D12CommandQueue>& queue) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter> warp;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) return false;
    if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return false;
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    return SUCCEEDED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
}

ComPtr<ID3D12Resource> makeLinearTex(ID3D12Device* dev, uint32_t w, uint32_t h, D3D12_RESOURCE_STATES initState) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initState, nullptr, IID_PPV_ARGS(&res)))) {
        return nullptr;
    }
    return res;
}

} // namespace

int main(int argc, char** argv) {
    // 1. Enum & policy checks for all four AMD NR runtimes.
    check(validNrRuntime(NrRuntime::LmxxfAmd) && isAmdNrRuntime(NrRuntime::LmxxfAmd), "LmxxfAmd is valid AMD NR");
    check(validNrRuntime(NrRuntime::MochizukiAmd) && isAmdNrRuntime(NrRuntime::MochizukiAmd), "MochizukiAmd is valid AMD NR");
    check(validNrRuntime(NrRuntime::RedDukeAmd) && isAmdNrRuntime(NrRuntime::RedDukeAmd), "RedDukeAmd is valid AMD NR");
    check(validNrRuntime(NrRuntime::DlssNrOnAmd) && isAmdNrRuntime(NrRuntime::DlssNrOnAmd), "DlssNrOnAmd is valid AMD NR");
    check(!validNrRuntime(static_cast<NrRuntime>(8)), "NrRuntime(8) is invalid");
    check(amdNrBoundedTo1080p(NrRuntime::LmxxfAmd) && amdNrBoundedTo1080p(NrRuntime::RedDukeAmd), "lmxxf and RedDuke bounded to 1080p");
    check(!amdNrBoundedTo1080p(NrRuntime::MochizukiAmd) && !amdNrBoundedTo1080p(NrRuntime::DlssNrOnAmd), "Mochizuki and NeuralHub allow 4K");
    check(amdNrSupportsFullControls(NrRuntime::MochizukiAmd) && amdNrSupportsFullControls(NrRuntime::DlssNrOnAmd), "Mochizuki and NeuralHub expose all 6 model controls");
    check(!amdNrSupportsFullControls(NrRuntime::LmxxfAmd) && !amdNrSupportsFullControls(NrRuntime::RedDukeAmd), "lmxxf and RedDuke keep intensity-only UI gate");

    // 2. Export & preview resolution policy: 4K input stays 4K on Mochizuki/NeuralHub, bounded on lmxxf/RedDuke.
    const Extent uhd{3840, 2160};
    const auto lmxxfExport = exportNrProcessingExtent(uhd, NrRuntime::LmxxfAmd, NrSizePolicy::Native);
    const auto redDukeExport = exportNrProcessingExtent(uhd, NrRuntime::RedDukeAmd, NrSizePolicy::Native);
    const auto mochizukiExport = exportNrProcessingExtent(uhd, NrRuntime::MochizukiAmd, NrSizePolicy::Native);
    const auto neuralHubExport = exportNrProcessingExtent(uhd, NrRuntime::DlssNrOnAmd, NrSizePolicy::Native);
    check(lmxxfExport.height <= 1080 && redDukeExport.height <= 1080, "lmxxf/RedDuke export stays <=1080p");
    check(mochizukiExport.width == 3840 && mochizukiExport.height == 2160, "Mochizuki native export retains 4K");
    check(neuralHubExport.width == 3840 && neuralHubExport.height == 2160, "NeuralHub native export retains 4K");

    // 3. Hardware provider routing checks for RX 9070 XT, RX 7900 XTX, RX 6800 XT.
    check(amdAdapterSupportsRedDuke(0x1002, L"AMD Radeon RX 9070 XT"), "RedDuke unlocked on RX 9070 XT");
    check(amdAdapterSupportsMochizuki(0x1002, L"AMD Radeon RX 9070 XT"), "Mochizuki supported on RX 9070 XT");
    check(amdAdapterSupportsDlssNrOnAmd(0x1002, L"AMD Radeon RX 9070 XT"), "NeuralHub supported on RX 9070 XT");
    check(amdAdapterSupportsDlssNrOnAmd(0x1002, L"AMD Radeon RX 6800 XT"), "NeuralHub supported on RX 6800 XT");
    check(!amdAdapterSupportsMochizuki(0x1002, L"AMD Radeon RX 6800 XT"), "Mochizuki gated off on RX 6800 XT");

    // 4. dlssnr-amd.ini [Network] Model key updater preserves unrelated sections & keys.
    const auto tempRoot = std::filesystem::temp_directory_path() / L"veyra-amd-nr-backend-tests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot, ec);
    const auto iniPath = tempRoot / L"dlssnr-amd.ini";
    {
        std::ofstream out(iniPath, std::ios::binary);
        out << "[RenoDX]\r\nIntensity = 0.85\r\n[Network]\r\nACO Mode = 1\r\nModel = CC_SILVER_AARDWOLD\r\nOptimizedLayers = 2\r\n";
    }
    check(AmdNgxNrBackend::writeMochizukiNetworkKey(iniPath, "Model", "CC_SMALL_FP8"), "writeMochizukiNetworkKey updates existing Model");
    {
        std::ifstream in(iniPath, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(body.find("Model = CC_SMALL_FP8") != std::string::npos, "updated Model written");
        check(body.find("ACO Mode = 1") != std::string::npos, "ACO Mode preserved");
        check(body.find("OptimizedLayers = 2") != std::string::npos, "OptimizedLayers preserved");
        check(body.find("Intensity = 0.85") != std::string::npos, "RenoDX section preserved");
    }

    // 5. Pointer-file discovery check for NeuralHub / Mochizuki without bundling.
    const auto runtimeRoot = tempRoot / L"runtime_local" / L"ngx";
    const auto mochizukiDir = tempRoot / L"runtime_local" / L"amd-nr-mochizuki";
    const auto neuralHubDir = tempRoot / L"runtime_local" / L"amd-nr-neuralhub";
    const auto externalHub = tempRoot / L"external-neuralhub";
    std::filesystem::create_directories(runtimeRoot, ec);
    std::filesystem::create_directories(mochizukiDir, ec);
    std::filesystem::create_directories(neuralHubDir, ec);
    std::filesystem::create_directories(externalHub, ec);
    check(!AmdNgxNrBackend::runtimeAvailable(runtimeRoot, AmdNgxNrProviderKind::Mochizuki), "missing Mochizuki DLL not advertised");
    {
        std::ofstream ptr(neuralHubDir / L"neuralhub-path.txt", std::ios::binary);
        ptr << "# user pointer\n" << externalHub.string() << "\n";
        std::ofstream dummy(externalHub / L"dlssnr_core.dll", std::ios::binary);
        dummy << "placeholder";
    }
    check(AmdNgxNrBackend::runtimeAvailable(runtimeRoot, AmdNgxNrProviderKind::NeuralHub), "pointer file resolves external NeuralHub DLL");

    // 6. Real D3D12 WARP + compiled FakeAmdNgxCore.dll end-to-end Feature 18 ABI test.
    if (argc >= 2) {
        const auto fakeDll = std::filesystem::absolute(argv[1]).lexically_normal();
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        if (createWarpDevice(device, queue)) {
            AmdNgxNrBackend backend;
            const uint32_t w = 128, h = 128;
            const bool opened = backend.openForContractTest(fakeDll, device.Get(), queue.Get(), w, h, true,
                                                            AmdNgxNrProviderKind::Mochizuki, false);
            check(opened && backend.ready() && backend.admits(w, h), "AmdNgxNrBackend opens FakeAmdNgxCore and admits extent");
            check(!backend.admits(w + 16, h), "AmdNgxNrBackend rejects mismatched extent");

            if (opened) {
                auto inputTex = makeLinearTex(device.Get(), w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                auto outputTex = makeLinearTex(device.Get(), w, h, D3D12_RESOURCE_STATE_COPY_DEST);
                ComPtr<ID3D12CommandAllocator> alloc;
                ComPtr<ID3D12GraphicsCommandList> list;
                ComPtr<ID3D12Fence> fence;
                check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
                      SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) &&
                      SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
                      "WARP D3D12 command list created");

                AmdNrBackend::FrameControls fc{};
                fc.intensity = 0.75f;
                fc.localTone = 0.625f;
                fc.localStructure = 0.875f;
                fc.skinStructure = 1.25f;
                fc.style = 2;
                fc.autoMask = 1;
                backend.setFrameControls(fc);

                check(backend.recordInputs(list.Get(), inputTex.Get(), 1, true, fc.intensity), "recordInputs succeeds");
                check(SUCCEEDED(list->Close()), "input command list closes");
                ID3D12CommandList* cmd1[] = {list.Get()};
                queue->ExecuteCommandLists(1, cmd1);
                check(backend.enqueue(), "enqueue succeeds");

                check(SUCCEEDED(queue->Signal(fence.Get(), 1)), "fence signal 1");
                while (fence->GetCompletedValue() < 1) Sleep(1);

                check(SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc.Get(), nullptr)), "command list resets for outputs");
                check(backend.recordOutputs(list.Get(), outputTex.Get()), "recordOutputs succeeds");
                check(SUCCEEDED(list->Close()), "output command list closes");
                ID3D12CommandList* cmd2[] = {list.Get()};
                queue->ExecuteCommandLists(1, cmd2);
                check(SUCCEEDED(queue->Signal(fence.Get(), 2)), "fence signal 2");
                while (fence->GetCompletedValue() < 2) Sleep(1);
                check(backend.retireSubmitted(), "retireSubmitted succeeds");

                HMODULE mod = GetModuleHandleW(fakeDll.c_str());
                using GetStatsFn = void(__cdecl*)(FakeStats*);
                auto getStats = mod ? reinterpret_cast<GetStatsFn>(GetProcAddress(mod, "FakeNgxGetStats")) : nullptr;
                FakeStats stats{};
                if (getStats) getStats(&stats);
                check(stats.initCount == 1 && stats.createCount == 1 && stats.evalCount == 1, "FakeNgx saw init=1 create=1 eval=1");
                check(stats.lastWidth == w && stats.lastHeight == h && stats.lastPreset == 2, "FakeNgx saw exact extent and preset=2");
                check(std::fabs(stats.lastIntensity - 0.75f) < 1e-4f &&
                      std::fabs(stats.lastTone - 0.625f) < 1e-4f &&
                      std::fabs(stats.lastStructure - 0.875f) < 1e-4f &&
                      std::fabs(stats.lastSkin - 1.25f) < 1e-4f &&
                      stats.lastStyle == 2 && stats.lastAutoMask == 1,
                      "FakeNgx received all 6 DLSSNR model controls accurately");

                backend.close();
            }
        } else {
            std::cerr << "SKIP: WARP D3D12 device unavailable on this runner\n";
        }
    }

    std::filesystem::remove_all(tempRoot, ec);
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
