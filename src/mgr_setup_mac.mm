// AppKit first-run setup (compiled with ARC). Every step runs on the main
// thread; only the copy itself runs on a worker thread.
#include "mgr_setup.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <rex/logging.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "mgr_disc_source.h"
#include "mgr_game_install.h"

@interface MgrSetupCancelTarget : NSObject
@property(nonatomic) std::atomic<bool>* flag;
- (void)cancel:(id)sender;
@end

@implementation MgrSetupCancelTarget
- (void)cancel:(id)sender {
  self.flag->store(true);
  [(NSButton*)sender setEnabled:NO];
}
@end

namespace {

namespace fs = std::filesystem;

constexpr int kPermille = 1000;

NSString* ToNS(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()]; }

void ShowError(NSString* title, NSString* detail) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleWarning;
  alert.messageText = title;
  alert.informativeText = detail;
  [alert runModal];
}

struct SetupFlow : std::enable_shared_from_this<SetupFlow> {
  fs::path data_root;
  MgrSetupReason reason = MgrSetupReason::kFirstRun;
  std::function<void(bool)> done;

  std::unique_ptr<mgr_install::GameSource> source;
  std::atomic<bool> cancel{false};
  std::atomic<int> shown_permille{-1};
  std::thread worker;
  NSWindow* window = nil;
  NSProgressIndicator* bar = nil;
  MgrSetupCancelTarget* cancel_target = nil;

  // Each step is queued so the previous modal panel has fully closed.
  void Next(void (SetupFlow::*step)()) {
    auto self = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{ ((*self).*step)(); });
  }

  void Welcome() {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = reason == MgrSetupReason::kFirstRun ? @"Welcome to MGR Recompiled"
                                                            : @"Reinstall the game files";
    alert.informativeText =
        [NSString stringWithFormat:
                      @"%@MGR Recompiled plays your own copy of Metal Gear Rising: Revengeance "
                      @"for Xbox 360 (USA/Europe disc).\n\nChoose its disc image (.iso) or a "
                      @"folder with the extracted files. About %.1f GB will be copied to:\n%@",
                      reason == MgrSetupReason::kDamaged
                          ? @"The installed game files are missing, damaged or from an "
                            @"older version.\n\n"
                          : @"",
                      double(mgr_install::TotalBytes(mgr_install::Manifest())) / 1e9,
                      ToNS(data_root.string())];
    [alert addButtonWithTitle:@"Choose Game…"];
    [alert addButtonWithTitle:reason == MgrSetupReason::kRequested ? @"Play" : @"Quit"];
    if ([alert runModal] != NSAlertFirstButtonReturn) {
      done(false);
      return;
    }
    Next(&SetupFlow::Choose);
  }

  void Choose() {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = @"Choose your Metal Gear Rising: Revengeance disc image or game folder";
    panel.prompt = @"Install";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    UTType* iso = [UTType typeWithFilenameExtension:@"iso"];
    if (iso) panel.allowedContentTypes = @[ iso, UTTypeFolder ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) {
      Next(&SetupFlow::Welcome);
      return;
    }
    const fs::path chosen = panel.URL.path.fileSystemRepresentation;
    if (!Open(chosen)) {
      Next(&SetupFlow::Choose);
      return;
    }
    Install();
  }

  bool Open(const fs::path& chosen) {
    std::error_code ec;
    if (fs::is_directory(chosen, ec)) {
      if (!fs::is_regular_file(chosen / "default.xex", ec)) {
        ShowError(@"No game in this folder",
                  @"The folder must contain default.xex and the media folder from the disc.");
        return false;
      }
      source = std::make_unique<mgr_install::FolderSource>(chosen);
    } else {
      source = mgr_install::OpenDiscImage(chosen);
      if (!source) {
        ShowError(@"Not an Xbox 360 disc image",
                  @"Choose an .iso dumped from the Xbox 360 disc, or a folder with the "
                  @"extracted files.");
        return false;
      }
    }
    const auto check = mgr_install::CheckSourceXex(*source);
    REXLOG_INFO("Setup: {} -> {}", chosen.string(), mgr_install::Describe(check));
    if (check != mgr_install::XexCheck::kOk) {
      ShowError(@"This copy can't be used", ToNS(mgr_install::Describe(check)));
      source.reset();
      return false;
    }
    return true;
  }

  void ShowProgress() {
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 120)
                                         styleMask:NSWindowStyleMaskTitled
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    window.releasedWhenClosed = NO;
    window.title = @"MGR Recompiled";
    NSTextField* label = [NSTextField labelWithString:@"Copying and checking the game files…"];
    label.frame = NSMakeRect(20, 78, 400, 20);
    bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 50, 400, 20)];
    bar.indeterminate = NO;
    bar.minValue = 0;
    bar.maxValue = kPermille;
    cancel_target = [[MgrSetupCancelTarget alloc] init];
    cancel_target.flag = &cancel;
    NSButton* button = [NSButton buttonWithTitle:@"Cancel"
                                          target:cancel_target
                                          action:@selector(cancel:)];
    button.frame = NSMakeRect(330, 12, 90, 30);
    [window.contentView addSubview:label];
    [window.contentView addSubview:bar];
    [window.contentView addSubview:button];
    [window center];
    [window makeKeyAndOrderFront:nil];
  }

  void Install() {
    cancel = false;
    shown_permille = -1;
    ShowProgress();
    auto self = shared_from_this();
    worker = std::thread([self] {
      const auto result = mgr_install::InstallFrom(
          *self->source, self->data_root,
          [self](uint64_t done_bytes, uint64_t total) {
            const int permille = total ? int(done_bytes * kPermille / total) : kPermille;
            if (self->shown_permille.exchange(permille) == permille) return;
            dispatch_async(dispatch_get_main_queue(), ^{ self->bar.doubleValue = permille; });
          },
          self->cancel);
      dispatch_async(dispatch_get_main_queue(), ^{ self->Finish(result); });
    });
  }

  void Finish(const mgr_install::InstallResult& result) {
    worker.join();
    source.reset();
    [window orderOut:nil];
    window = nil;
    bar = nil;
    if (result.ok) {
      REXLOG_INFO("Setup: game files installed in {}", data_root.string());
      done(true);
    } else if (result.cancelled) {
      REXLOG_INFO("Setup: install cancelled");
      Next(&SetupFlow::Welcome);
    } else {
      REXLOG_ERROR("Setup: install failed: {}", result.error);
      ShowError(@"The game files could not be installed", ToNS(result.error));
      Next(&SetupFlow::Choose);
    }
  }
};

}  // namespace

bool MgrSetupRequested() { return ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0; }

void MgrRunSetup(const fs::path& data_root, MgrSetupReason reason,
                 std::function<void(bool)> done) {
  auto flow = std::make_shared<SetupFlow>();
  flow->data_root = data_root;
  flow->reason = reason;
  flow->done = std::move(done);
  flow->Next(&SetupFlow::Welcome);
}
