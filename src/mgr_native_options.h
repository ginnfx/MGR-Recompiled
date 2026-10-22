#pragma once

#include <cstdint>
#include <filesystem>

struct PPCContext;

// Called only from the guest cOptionMenu lifecycle, never from the host UI.
void MgrNativeOptionsConfigure(const std::filesystem::path& config_path);
void MgrNativeOptionsBegin(PPCContext& ctx, uint8_t* base, uint32_t object);
bool MgrNativeOptionsUpdate(PPCContext& ctx, uint8_t* base, uint32_t object);
void MgrNativeOptionsAfterUpdate(PPCContext& ctx, uint8_t* base, uint32_t object);
void MgrNativeOptionsEnd(uint8_t* base, uint32_t object);
