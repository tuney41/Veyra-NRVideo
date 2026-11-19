// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <string_view>
namespace veyra::pipeline {
enum class AmdNrProvider { Unsupported, LmxxfRdna4, RedDukeRdna3, Rdna2 };
// Hardware routing only. Runtime/driver initialization is checked separately.
constexpr bool amdAdapterSeriesDigit(uint32_t vendor,std::wstring_view name,wchar_t first,wchar_t second=0){
    if(vendor!=0x1002)return false;
    for(size_t i=0;i+2<name.size();++i){
        if((name[i]!=L'R'&&name[i]!=L'r')||(name[i+1]!=L'X'&&name[i+1]!=L'x'))continue;
        size_t n=i+2;while(n<name.size()&&(name[n]==L' '||name[n]==L'\t'))++n;
        if(second){
            if(n+1<name.size()&&name[n]==first&&name[n+1]==second)return true;
        }else{
            if(n<name.size()&&name[n]==first)return true;
        }
    }
    return false;
}
constexpr bool amdAdapterIsRx9000(uint32_t vendor,std::wstring_view name){return amdAdapterSeriesDigit(vendor,name,L'9',L'0');}
constexpr bool amdAdapterIsRx7000(uint32_t vendor,std::wstring_view name){return amdAdapterSeriesDigit(vendor,name,L'7');}
constexpr bool amdAdapterIsRx6000(uint32_t vendor,std::wstring_view name){return amdAdapterSeriesDigit(vendor,name,L'6');}
constexpr AmdNrProvider amdNrProvider(uint32_t vendor,std::wstring_view name){
    if(amdAdapterIsRx7000(vendor,name))return AmdNrProvider::RedDukeRdna3;
    if(amdAdapterIsRx9000(vendor,name))return AmdNrProvider::LmxxfRdna4;
    return AmdNrProvider::Unsupported;
}
// RedDukeDev's ZLUDA/HIP worker supports both RX 7000 (RDNA3) and RX 9000 (RDNA4).
constexpr bool amdAdapterSupportsRedDuke(uint32_t vendor,std::wstring_view name){
    return amdAdapterIsRx7000(vendor,name)||amdAdapterIsRx9000(vendor,name);
}
// Mochizuki DLSSNR-AMD targets RX 9000 (RDNA4, FP8 WMMA) with experimental RX 7000 (RDNA3, FP16) support.
constexpr bool amdAdapterSupportsMochizuki(uint32_t vendor,std::wstring_view name){
    return amdAdapterIsRx9000(vendor,name)||amdAdapterIsRx7000(vendor,name);
}
// danielblnc/DLSS-NR-on-AMD (NeuralHub) supports RX 9000 (RDNA4), RX 7000 (RDNA3), and RX 6000 (RDNA2).
constexpr bool amdAdapterSupportsDlssNrOnAmd(uint32_t vendor,std::wstring_view name){
    return amdAdapterIsRx9000(vendor,name)||amdAdapterIsRx7000(vendor,name)||amdAdapterIsRx6000(vendor,name);
}
}
