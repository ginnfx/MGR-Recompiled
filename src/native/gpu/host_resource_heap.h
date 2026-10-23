/**
 * @file    gpu/host_resource_heap.h
 * @brief   Host resource objects keyed by the guest D3D object they back.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR. re:Blue allocates each host struct inside guest memory so
 * the struct itself is the guest's D3D object. MGR keeps the guest's own
 * D3D9 runtime (its Create* functions allocate and fill the real objects), so
 * host structs live in host memory and are attached to the guest object's VA:
 * Alloc creates an unattached object, Attach registers it under a guest VA
 * (selfVa), FromGuest looks it up, Free detaches and deletes it.
 */
#pragma once

#include <rex/types.h>
#include <type_traits>
#include <utility>

#include "gpu/resources.h"

namespace mgrn::gpu {

class HostResourceHeap {
public:
  template <typename T, typename... Args> static T *Alloc(Args &&...args) {
    static_assert(std::is_destructible_v<T>);
    return new T(std::forward<Args>(args)...);
  }

  // Registers 'obj' as the host side of the guest object at 'guest_va',
  // replacing any previous attachment of that VA.
  template <typename T> static void Attach(u32 guest_va, T *obj) {
    if (!obj || !guest_va)
      return;
    obj->selfVa = guest_va;
    Register(guest_va, obj->type, obj);
  }

  // Returns the ResourceType the VA was attached with. False on miss.
  static bool GetType(u32 guest_va, ResourceType *out_type);

  template <typename T> static void Free(T *host_ptr) {
    if (!host_ptr)
      return;
    Unregister(host_ptr->selfVa, host_ptr);
    delete host_ptr;
  }

  // The host struct attached to a guest VA, or nullptr. Caller must pass the
  // correct T (the registry stores the ResourceType, not RTTI).
  template <typename T> static T *FromGuest(u32 guest_va) {
    return static_cast<T *>(Lookup(guest_va));
  }

  template <typename T> static u32 ToGuest(T *host_ptr) {
    return host_ptr ? host_ptr->selfVa : 0;
  }

private:
  static void Register(u32 guest_va, ResourceType type, void *host);
  // Only removes the entry if it still names 'host' (a VA may be re-attached).
  static void Unregister(u32 guest_va, const void *host);
  static void *Lookup(u32 guest_va);
};

} // namespace mgrn::gpu
