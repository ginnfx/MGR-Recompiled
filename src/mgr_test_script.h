// Diagnostic test driver (MGR_TEST_SCRIPT=<file>): scripted controller input
// and frame captures keyed to guest presents, so automated runs need neither
// window focus nor synthesized key events. See mgr_test_script.cpp.
#pragma once

#include <functional>
#include <string>

// Called once per guest present (VdSwap), before the SDK swap.
void MgrTestScriptOnPresent();

// Capture thread: pops the next requested capture name. Returns false when none.
bool MgrTestScriptTakeCapture(std::string& name);
// Capture thread: the capture popped last has been written (or failed).
void MgrTestScriptCaptureDone();
bool MgrTestScriptActive();
// "save" events call this (the options dialog's save) on the guest thread.
void MgrTestScriptSetSaveHandler(std::function<void()> handler);

// MGR_TEST_HIDDEN=1: hide the game window and opt out of App Nap, so a test run
// does not appear on the active desktop. Call on the UI thread after the window
// opens. Launch with SDL_MAC_BACKGROUND_APP=1 so the app never activates.
void MgrTestHideWindowIfRequested();
