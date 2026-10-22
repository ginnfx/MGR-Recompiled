// Diagnostic test driver. MGR_TEST_SCRIPT=<file> holds one event per line,
// keyed to the guest present count (frame 1 = first VdSwap):
//
//   <frame> press <BUTTON[+BUTTON...]> [hold_frames=4]
//   <frame> stick <lx> <ly> [hold_frames=4]       left stick, -32768..32767
//   <frame> capture <name>                         <name>-<frame>.ppm in MGR_CAPTURE_DIR
//                                                  (the image presented before <frame>)
//   <frame> mark <text>                            "[mgr-test] frame N mark text"
//   <frame> set <cvar> <value>                     as the options menu would
//   <frame> save                                   the options menu's Save
//   <frame> quit                                   after pending captures
//
// <frame> may be "start+every*count" to repeat an event. '#' starts a comment.
// Buttons: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT.
// Scripted input is merged into player 1's XamInputGetState result, which is
// reported as connected while the script runs.

#include "mgr_test_script.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

enum class Action { kPress, kStick, kCapture, kMark, kSet, kSave, kQuit };

struct Event {
  uint64_t frame;
  Action action;
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0;
  uint32_t hold = 4;
  std::string text;
};

constexpr uint32_t kDefaultHold = 4;
constexpr auto kQuitCaptureTimeout = std::chrono::seconds(10);

struct ButtonName {
  const char* name;
  uint16_t bit;
};
constexpr ButtonName kButtons[] = {
    {"UP", 0x0001},  {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040},  {"RS", 0x0080},
    {"LB", 0x0100},  {"RB", 0x0200},   {"A", 0x1000},    {"B", 0x2000},
    {"X", 0x4000},   {"Y", 0x8000},
};

bool ParseButtons(const std::string& spec, uint16_t& out) {
  out = 0;
  std::stringstream parts(spec);
  std::string part;
  while (std::getline(parts, part, '+')) {
    bool found = false;
    for (const ButtonName& b : kButtons) {
      if (part == b.name) {
        out |= b.bit;
        found = true;
      }
    }
    if (!found) return false;
  }
  return out != 0;
}

bool ParseAction(std::istringstream& in, Event& e) {
  std::string verb;
  in >> verb;
  if (verb == "press") {
    std::string buttons;
    in >> buttons;
    e.action = Action::kPress;
    if (!ParseButtons(buttons, e.buttons)) return false;
    if (!(in >> e.hold)) e.hold = kDefaultHold;
    return true;
  }
  if (verb == "stick") {
    int lx, ly;
    if (!(in >> lx >> ly) || lx < -32768 || lx > 32767 || ly < -32768 || ly > 32767) return false;
    e.action = Action::kStick;
    e.lx = int16_t(lx);
    e.ly = int16_t(ly);
    if (!(in >> e.hold)) e.hold = kDefaultHold;
    return true;
  }
  if (verb == "capture" || verb == "mark") {
    e.action = verb == "capture" ? Action::kCapture : Action::kMark;
    std::getline(in >> std::ws, e.text);
    return !e.text.empty();
  }
  if (verb == "set") {
    e.action = Action::kSet;
    std::getline(in >> std::ws, e.text);
    return e.text.find(' ') != std::string::npos;
  }
  if (verb == "save") {
    e.action = Action::kSave;
    return true;
  }
  if (verb == "quit") {
    e.action = Action::kQuit;
    return true;
  }
  return false;
}

// "start", or "start+every*count".
bool ParseFrames(const std::string& spec, std::vector<uint64_t>& out) {
  unsigned long long start = 0, every = 0, count = 1;
  char tail = 0;
  if (std::sscanf(spec.c_str(), "%llu+%llu*%llu%c", &start, &every, &count, &tail) == 3) {
    if (every == 0 || count == 0) return false;
  } else if (std::sscanf(spec.c_str(), "%llu%c", &start, &tail) != 1) {
    return false;
  }
  for (unsigned long long i = 0; i < count; ++i) out.push_back(start + i * every);
  return true;
}

std::vector<Event> LoadScript(const char* path) {
  std::vector<Event> events;
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "[mgr-test] FATAL: cannot open MGR_TEST_SCRIPT %s\n", path);
    std::abort();
  }
  std::string line;
  for (int line_number = 1; std::getline(file, line); ++line_number) {
    line = line.substr(0, line.find('#'));
    std::istringstream in(line);
    std::string frames_spec;
    if (!(in >> frames_spec)) continue;
    std::vector<uint64_t> frames;
    Event e{};
    if (!ParseFrames(frames_spec, frames) || !ParseAction(in, e)) {
      std::fprintf(stderr, "[mgr-test] FATAL: %s:%d: cannot parse '%s'\n", path, line_number,
                   line.c_str());
      std::abort();
    }
    for (uint64_t frame : frames) {
      Event copy = e;
      copy.frame = frame;
      events.push_back(std::move(copy));
    }
  }
  const bool captures = std::any_of(events.begin(), events.end(),
                                    [](const Event& e) { return e.action == Action::kCapture; });
  if (captures && !std::getenv("MGR_CAPTURE_DIR")) {
    std::fprintf(stderr, "[mgr-test] FATAL: %s has capture events but MGR_CAPTURE_DIR is unset\n",
                 path);
    std::abort();
  }
  std::fprintf(stderr, "[mgr-test] loaded %zu events from %s\n", events.size(), path);
  return events;
}

struct Script {
  std::vector<Event> events;
  std::atomic<uint64_t> frame{0};
  std::mutex capture_mutex;
  std::deque<std::string> captures;
  std::atomic<uint32_t> captures_in_flight{0};
  std::mutex save_mutex;
  std::function<void()> save_handler;

  Script() {
    const char* path = std::getenv("MGR_TEST_SCRIPT");
    if (path && *path) events = LoadScript(path);
  }
  bool active() const { return !events.empty(); }
};

Script& GetScript() {
  static Script script;
  return script;
}

void WaitForCapturesThenExit(Script& s) {
  const auto deadline = std::chrono::steady_clock::now() + kQuitCaptureTimeout;
  while (s.captures_in_flight.load() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::fprintf(stderr, "[mgr-test] frame %llu quit\n", (unsigned long long)s.frame.load());
  std::fflush(nullptr);
  _exit(0);
}

PPCFunc* ResolveSdk(const char* name) {
  auto* fn = reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
  if (!fn) {
    std::fprintf(stderr, "[mgr-test] FATAL: SDK %s not found: %s\n", name, dlerror());
    std::abort();
  }
  return fn;
}

struct GamepadOverride {
  uint16_t buttons = 0;
  bool has_stick = false;
  int16_t lx = 0, ly = 0;
};

GamepadOverride ScriptedGamepad(const Script& s, uint64_t frame) {
  GamepadOverride out;
  for (const Event& e : s.events) {
    if (frame < e.frame || frame >= e.frame + e.hold) continue;
    if (e.action == Action::kPress) out.buttons |= e.buttons;
    if (e.action == Action::kStick) {
      out.has_stick = true;
      out.lx = e.lx;
      out.ly = e.ly;
    }
  }
  return out;
}

void StoreGuestU16(uint8_t* base, uint32_t address, uint16_t value) {
  const uint16_t be = __builtin_bswap16(value);
  std::memcpy(base + address, &be, sizeof(be));
}

uint16_t LoadGuestU16(const uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap16(value);
}

constexpr uint32_t kXInputErrorSuccess = 0;
// X_INPUT_STATE: be32 packet_number, then X_INPUT_GAMEPAD.
constexpr uint32_t kStatePacketOffset = 0;
constexpr uint32_t kStateButtonsOffset = 4;
constexpr uint32_t kStateThumbLxOffset = 8;
constexpr uint32_t kStateThumbLyOffset = 10;
constexpr uint32_t kStateSize = 16;

}  // namespace

bool MgrTestScriptActive() { return GetScript().active(); }

void MgrTestScriptOnPresent() {
  Script& s = GetScript();
  if (!s.active()) return;
  const uint64_t frame = s.frame.fetch_add(1) + 1;
  bool quit = false;
  for (const Event& e : s.events) {
    if (e.frame != frame) continue;
    if (e.action == Action::kMark) {
      std::fprintf(stderr, "[mgr-test] frame %llu mark %s\n", (unsigned long long)frame,
                   e.text.c_str());
    } else if (e.action == Action::kCapture) {
      std::lock_guard lock(s.capture_mutex);
      s.captures.push_back(e.text + "-" + std::to_string(frame));
      s.captures_in_flight.fetch_add(1);
    } else if (e.action == Action::kSet) {
      const size_t space = e.text.find(' ');
      const std::string name = e.text.substr(0, space), value = e.text.substr(space + 1);
      const bool applied = rex::cvar::SetFlagByName(name, value);
      std::fprintf(stderr, "[mgr-test] frame %llu set %s = %s%s\n", (unsigned long long)frame,
                   name.c_str(), value.c_str(), applied ? "" : " (rejected)");
    } else if (e.action == Action::kSave) {
      std::lock_guard lock(s.save_mutex);
      std::fprintf(stderr, "[mgr-test] frame %llu save%s\n", (unsigned long long)frame,
                   s.save_handler ? "" : " (no handler)");
      if (s.save_handler) s.save_handler();
    } else if (e.action == Action::kQuit) {
      quit = true;
    }
  }
  if (quit) WaitForCapturesThenExit(s);
}

bool MgrTestScriptTakeCapture(std::string& name) {
  Script& s = GetScript();
  std::lock_guard lock(s.capture_mutex);
  if (s.captures.empty()) return false;
  name = std::move(s.captures.front());
  s.captures.pop_front();
  return true;
}

void MgrTestScriptCaptureDone() { GetScript().captures_in_flight.fetch_sub(1); }

void MgrTestScriptSetSaveHandler(std::function<void()> handler) {
  Script& s = GetScript();
  std::lock_guard lock(s.save_mutex);
  s.save_handler = std::move(handler);
}

extern "C" REX_FUNC(__imp__XamInputGetState) {
  static PPCFunc* const sdk_get_state = ResolveSdk("__imp__XamInputGetState");
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t state = ctx.r5.u32;
  sdk_get_state(ctx, base);
  Script& s = GetScript();
  const bool player_one = (user_index & 0xFF) == 0 || (user_index & 0xFF) == 0xFF;
  if (!s.active() || !state || !player_one) return;
  if (ctx.r3.u32 != kXInputErrorSuccess) std::memset(base + state, 0, kStateSize);
  const uint64_t frame = s.frame.load(std::memory_order_relaxed);
  const GamepadOverride pad = ScriptedGamepad(s, frame);
  // A new packet number every frame tells the game to re-read the state.
  const uint32_t packet = __builtin_bswap32(uint32_t(frame));
  std::memcpy(base + state + kStatePacketOffset, &packet, sizeof(packet));
  StoreGuestU16(base, state + kStateButtonsOffset,
                LoadGuestU16(base, state + kStateButtonsOffset) | pad.buttons);
  if (pad.has_stick) {
    StoreGuestU16(base, state + kStateThumbLxOffset, uint16_t(pad.lx));
    StoreGuestU16(base, state + kStateThumbLyOffset, uint16_t(pad.ly));
  }
  ctx.r3.u64 = kXInputErrorSuccess;
}
