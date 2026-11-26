// Sparkle updater (compiled with ARC).
#include "mgr_updater.h"

#import <AppKit/AppKit.h>
#import <Sparkle/Sparkle.h>

#include <rex/logging.h>

@interface MgrUpdaterDriver : NSObject <SPUStandardUserDriverDelegate>
@end

@implementation MgrUpdaterDriver
- (BOOL)supportsGentleScheduledUpdateReminders {
  return YES;
}

// A scheduled check may only open its window right away when Sparkle says it
// can take focus (just after launch); never in the middle of play. A skipped
// one is still offered by "Check for Updates…" and at a later launch.
- (BOOL)standardUserDriverShouldHandleShowingScheduledUpdate:(SUAppcastItem*)update
                                          andInImmediateFocus:(BOOL)immediateFocus {
  return immediateFocus;
}
@end

namespace {

// Sparkle holds its delegate weakly, so both live for the whole process.
SPUStandardUpdaterController* g_controller = nil;
MgrUpdaterDriver* g_driver = nil;

bool HasInfoString(NSString* key) {
  id value = [NSBundle.mainBundle objectForInfoDictionaryKey:key];
  return [value isKindOfClass:NSString.class] && [value length] > 0;
}

void AddMenuItem() {
  NSMenu* app_menu = NSApp.mainMenu.itemArray.firstObject.submenu;
  if (!app_menu) {
    REXLOG_ERROR("Updater: no application menu; \"Check for Updates\" is unavailable");
    return;
  }
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:@"Check for Updates…"
                                                action:@selector(checkForUpdates:)
                                         keyEquivalent:@""];
  item.target = g_controller;
  [app_menu insertItem:item atIndex:MIN(NSInteger{1}, app_menu.numberOfItems)];
}

}  // namespace

void MgrStartUpdater() {
  if (g_controller || !HasInfoString(@"SUFeedURL") || !HasInfoString(@"SUPublicEDKey")) return;
  g_driver = [[MgrUpdaterDriver alloc] init];
  g_controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                               updaterDelegate:nil
                                                            userDriverDelegate:g_driver];
  AddMenuItem();
  REXLOG_INFO("Updater: started");
}
