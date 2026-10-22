#include "mgr_ot_key.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  // Every key the registrar accepts round-trips through the executor's decode.
  for (uint32_t layer = 0; layer < 4; ++layer) {
    for (uint32_t priority = 0; priority < 128; ++priority) {
      for (uint32_t sub = 0; sub < 3; ++sub) {
        for (uint32_t user : {0u, 1u, 0x400u, 0xFFFFFu}) {
          const mgr_ot::Key key{layer, priority, sub, user};
          assert(mgr_ot::Unpack(mgr_ot::Pack(key)) == key);
        }
      }
    }
  }
  // Registrations seen in the image: final gather (sub_82BEC110) at layer 1,
  // priority 97, sub 0; the glare-bearing composite (sub_82BEC8C0) at 1.90.2.
  assert(mgr_ot::Pack({1, 97, 0, 0}) == 0x70800000u);
  assert(mgr_ot::Pack({1, 90, 2, 0}) == 0x6D200000u);
  // r7 (user) bits beyond 20 never reach the key.
  assert(mgr_ot::Pack({0, 0, 0, 0x100400u}) == 0x400u);

  const auto none = mgr_ot::ParseAddressList(nullptr);
  assert(none.empty());
  const auto two = mgr_ot::ParseAddressList("82BEB018,828559d0");
  assert(two.size() == 2 && two.count(0x82BEB018u) && two.count(0x828559D0u));
  const auto stops = mgr_ot::ParseAddressList("82BEB018,zz,828559D0");
  assert(stops.size() == 1 && stops.count(0x82BEB018u));
  std::cout << "ot_key: all checks passed\n";
  return 0;
}
