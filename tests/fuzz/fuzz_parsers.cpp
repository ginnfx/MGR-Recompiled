// libFuzzer harness for the parsers that read player-supplied files.
// Run with scripts/fuzz.sh.
#include "mgr_game_install.h"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(MGR_FUZZ_XEX)
namespace {
// A well-formed header with room for six blocks. Random bytes never get past
// the magic and size gates, so the fuzzer patches this instead.
std::vector<uint8_t> Template() {
  std::vector<uint8_t> package(0x10000, 0);
  const auto put = [&](size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) package[at + i] = uint8_t(value >> (24 - 8 * i));
  };
  put(0, 0x4C495645);  // "LIVE"
  put(0x340, 0x9720);
  put(0x344, 2);
  put(0x360, mgr_install::kTitleId);
  package[0x379 + 2] = 1;
  put(0x379 + 0x1C, 5);
  return package;
}

// Input: a 2-byte truncation amount, then (2-byte offset, 1-byte value) patches.
std::vector<uint8_t> Mutate(const uint8_t* data, size_t size) {
  std::vector<uint8_t> package = Template();
  if (size < 2) return package;
  for (size_t at = 2; at + 3 <= size; at += 3)
    package[size_t(data[at]) << 8 | data[at + 1]] = data[at + 2];
  package.resize(package.size() - (size_t(data[0]) << 8 | data[1]));
  return package;
}
}  // namespace
#endif

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
#if defined(MGR_FUZZ_XEX)
  mgr_install::CheckXex({data, size}, 0);
#else
  static const std::string path =
      (std::filesystem::temp_directory_path() / ("mgr_fuzz_dlc_" + std::to_string(getpid())))
          .string();
  const std::vector<uint8_t> package = Mutate(data, size);
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file || std::fwrite(package.data(), 1, package.size(), file) != package.size())
    std::abort();
  std::fclose(file);
  mgr_install::CheckDlcPackage(path);
#endif
  return 0;
}
