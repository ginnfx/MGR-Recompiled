// Diagnostic dump of the retail Options UI (enabled by MGR_OPTIONS_DUMP=<file>).
#pragma once

#include <cstdint>

// Writes node tables, slot id arrays, widget vtables and the controller diagram sprite's
// texture buffer header to the file named by MGR_OPTIONS_DUMP. No-op when unset.
void MgrDumpOptionsLayout(uint8_t* base, uint32_t object);
