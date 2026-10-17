// mgr - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stop_token>
#include <string>
#include <thread>

void MgrStartSessionLog(const char* build);  // mgr_perf.cpp
void MgrStopSessionLog();

class MgrApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MgrApp>(new MgrApp(ctx, "mgr",
        PPCImageConfig));
  }

 protected:
  // Diagnostic: MGR_CAPTURE_DIR=<dir> dumps the guest frontbuffer as PPM
  // every CAPTURE_INTERVAL. Pattern from XDanfr/RaymanOriginsRecomp
  // rex/src/rayman_app.h (c1bf7b9), with a joined thread instead of detached.
  void OnPostSetup() override {
    DumpGuestImageIfRequested();
    const char* perf_log = std::getenv("MGR_PERF_LOG");
    if (perf_log && perf_log[0] == '1') MgrStartSessionLog(MGR_BUILD_ID);
    const char* dir = std::getenv("MGR_CAPTURE_DIR");
    if (!dir || !*dir) return;
    capture_thread_ = std::jthread(
        [this, out_dir = std::string(dir)](std::stop_token stop) {
          CaptureLoop(stop, out_dir);
        });
  }

  void OnShutdown() override {
    MgrStopSessionLog();
    capture_thread_.request_stop();
    if (capture_thread_.joinable()) capture_thread_.join();
  }

 private:
  static constexpr uint32_t IMAGE_BASE = 0x82000000;
  static constexpr uint32_t IMAGE_END = 0x83A40000;

  // Diagnostic: MGR_DUMP_IMAGE=<file> writes the loaded (decrypted,
  // decompressed) guest image for offline function-boundary analysis.
  void DumpGuestImageIfRequested() {
    const char* path = std::getenv("MGR_DUMP_IMAGE");
    if (!path || !*path || !runtime()) return;
    FILE* f = std::fopen(path, "wb");
    if (!f) {
      std::fprintf(stderr, "[mgr-dump] cannot open %s\n", path);
      return;
    }
    const uint8_t* base = runtime()->virtual_membase() + IMAGE_BASE;
    size_t written = std::fwrite(base, 1, IMAGE_END - IMAGE_BASE, f);
    std::fclose(f);
    std::fprintf(stderr, "[mgr-dump] wrote %zu bytes to %s\n", written, path);
  }

  static constexpr auto CAPTURE_INTERVAL = std::chrono::seconds(10);
  static constexpr auto CAPTURE_POLL = std::chrono::milliseconds(200);

  void CaptureLoop(std::stop_token stop, const std::string& out_dir) {
    int index = 0;
    auto next = std::chrono::steady_clock::now() + CAPTURE_INTERVAL;
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(CAPTURE_POLL);
      if (std::chrono::steady_clock::now() < next) continue;
      next += CAPTURE_INTERVAL;
      auto* gs = runtime() ? runtime()->graphics_system() : nullptr;
      auto* presenter = gs ? gs->presenter() : nullptr;
      rex::ui::RawImage image;
      if (!presenter || !presenter->CaptureGuestOutput(image)) {
        std::fprintf(stderr, "[mgr-capture] no guest output yet\n");
        continue;
      }
      WritePpm(out_dir + "/frame_" + std::to_string(index++) + ".ppm", image);
    }
  }

  static void WritePpm(const std::string& path, const rex::ui::RawImage& image) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "[mgr-capture] cannot open %s\n", path.c_str());
      return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* row = image.data.data() + y * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) std::fwrite(row + x * 4, 1, 3, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[mgr-capture] wrote %s (%ux%u)\n", path.c_str(), image.width,
                 image.height);
  }

  std::jthread capture_thread_;
};
