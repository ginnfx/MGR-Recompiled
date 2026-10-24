/**
 * @file    gpu/host_resource_heap.cpp
 * @brief   Guest VA -> host resource registry backing FromGuest resolution.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR (see host_resource_heap.h).
 */
#include "gpu/host_resource_heap.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace mgrn::gpu {

namespace {

struct Entry {
  ResourceType type;
  void *host;
};

std::shared_mutex g_registry_mutex;
std::unordered_map<u32, Entry> g_registry;

} // namespace

void HostResourceHeap::Register(u32 guest_va, ResourceType type, void *host) {
  std::unique_lock lock(g_registry_mutex);
  g_registry[guest_va] = {type, host};
}

void HostResourceHeap::Unregister(u32 guest_va, const void *host) {
  std::unique_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it != g_registry.end() && it->second.host == host)
    g_registry.erase(it);
}

void *HostResourceHeap::Lookup(u32 guest_va) {
  if (!guest_va)
    return nullptr;
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  return it == g_registry.end() ? nullptr : it->second.host;
}

bool HostResourceHeap::GetType(u32 guest_va, ResourceType *out_type) {
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it == g_registry.end())
    return false;
  *out_type = it->second.type;
  return true;
}

} // namespace mgrn::gpu
