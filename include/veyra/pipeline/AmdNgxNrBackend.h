// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "veyra/pipeline/AmdNrBackend.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace veyra::pipeline {

// External D3D12 NGX Feature 18 (DLSS-NR) provider on AMD Radeon GPUs:
//  - Mochizuki: mochizuki0323/DLSSNR-AMD (MIT, dlssnr_core.dll; Vulkan ACO / native D3D12)
//  - NeuralHub: user-installed danielblnc/DLSS-NR-on-AMD (no bundling/redistribution)
enum class AmdNgxNrProviderKind : uint8_t {
    Mochizuki = 0,
    NeuralHub = 1,
};

// Direct D3D12 NGX Feature 18 AMD NR backend implementing Veyra's shared
// AmdNrBackend linear-FP16 contract with RenoDX-equivalent parity encode/decode.
class AmdNgxNrBackend final : public AmdNrBackend {
public:
    AmdNgxNrBackend();
    ~AmdNgxNrBackend() override;
    AmdNgxNrBackend(const AmdNgxNrBackend&) = delete;
    AmdNgxNrBackend& operator=(const AmdNgxNrBackend&) = delete;

    // Resolves the user-installed provider directory and DLL without loading
    // or redistributing third-party binaries.
    static std::filesystem::path defaultDirectory(const std::filesystem::path& runtimeRoot,
                                                  AmdNgxNrProviderKind kind);
    static std::filesystem::path resolveRuntimeDll(const std::filesystem::path& runtimeRoot,
                                                   AmdNgxNrProviderKind kind);
    static bool runtimeAvailable(const std::filesystem::path& runtimeRoot,
                                 AmdNgxNrProviderKind kind);

    // Syncs a single [Network] key in dlssnr-amd.ini (preserving all other
    // user sections and settings). Exposed for unit testing and UI helpers.
    static bool writeMochizukiNetworkKey(const std::filesystem::path& iniPath,
                                         const std::string& key,
                                         const std::string& value);
    static std::string selectMochizukiModelName(bool performance);

    // Maximum internal NR resolution admitted by NGX-based AMD runtimes (up to 4K).
    static constexpr bool validExtent(uint32_t width, uint32_t height) {
        return width >= 64 && height >= 64 && width <= 4096 && height <= 2160 &&
               uint64_t(width) * uint64_t(height) <= 4096ull * 2160ull;
    }

    bool open(const std::filesystem::path& runtimeRoot,
              ID3D12Device* device,
              ID3D12CommandQueue* queue,
              uint32_t vendor,
              uint32_t width,
              uint32_t height,
              bool performance,
              AmdNgxNrProviderKind kind);

    // Test-only entry point allowing direct DLL path injection and optional
    // parity shader bypass on headless CI runners without compiled DXIL shaders.
    bool openForContractTest(const std::filesystem::path& dllPath,
                             ID3D12Device* device,
                             ID3D12CommandQueue* queue,
                             uint32_t width,
                             uint32_t height,
                             bool performance,
                             AmdNgxNrProviderKind kind,
                             bool useParityShaders = false);

    bool ready() const override;
    bool admits(uint32_t width, uint32_t height) const override;
    void setFrameControls(const FrameControls& controls) override;
    bool recordInputs(ID3D12GraphicsCommandList* list,
                      ID3D12Resource* color,
                      uint64_t frame,
                      bool reset,
                      float intensity) override;
    bool enqueue() override;
    bool recordOutputs(ID3D12GraphicsCommandList* list, ID3D12Resource* destination) override;
    bool retireSubmitted() override;
    const std::string& error() const override;

    void close() noexcept;

private:
    bool openInternal(const std::filesystem::path& dllPath,
                      ID3D12Device* device,
                      ID3D12CommandQueue* queue,
                      uint32_t width,
                      uint32_t height,
                      bool performance,
                      AmdNgxNrProviderKind kind,
                      bool useParityShaders);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace veyra::pipeline
