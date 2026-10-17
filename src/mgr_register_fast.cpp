// Replacement for generated/default/mgr_register.cpp.
//
// The generated file is one ~85k-statement function that Apple Clang needs
// 30+ minutes to compile even at -O0. PPCFuncMappings (mgr_init.cpp) holds
// the same address->function table, so register from it in a loop.

#include "generated/default/mgr_init.h"

#include <rex/system/function_dispatcher.h>

void mgr_RegisterFunctions(rex::runtime::IModuleRegistrar* registrar) {
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    registrar->SetFunction(static_cast<uint32_t>(m->guest), m->host);
  }
}
