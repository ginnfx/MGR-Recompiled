#include "../runtime/mgr_pipeline_metrics.h"
#include <cassert>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  assert(argc == 2);
  setenv("MGR_PIPELINE_CSV", argv[1], 1);
  mgr_pipeline::focus.store(1);
  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) workers.emplace_back([] {
    for (int j = 0; j < 1000; ++j) { mgr_pipeline::InputScope scope; }
  });
  for (auto& worker : workers) worker.join();
  assert(mgr_pipeline::input_calls.load() == 4000);
  {
    mgr_pipeline::PaintScope scope{.ui=0, .nonblocking=1, .width=1280, .height=720};
    scope.outcome = "present_requested";
  }
  assert(mgr_pipeline::input_calls.load() == 0);
  mgr_pipeline::focus.store(0);
  {
    mgr_pipeline::PaintScope scope{.ui=1, .nonblocking=0, .width=1280, .height=720};
    scope.outcome = "acquire_busy";
  }
}
