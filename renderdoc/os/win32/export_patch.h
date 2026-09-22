/******************************************************************************
 * The MIT License (MIT)
 *
 * Modified build utility: export-byte level hot-patching.
 *
 * RenderDoc's default windows hooking covers IAT tables and hooked
 * GetProcAddress. Programs that resolve function pointers by manually walking
 * a module's export table bypass all of that and get the real, unhooked
 * functions. ApplyExportPatch() closes that hole for a chosen set of exports
 * by writing a jump to the hook at the function entry, with a trampoline that
 * replays the displaced original instructions.
 *
 * Only very simple, verified prologues may be relocated (see callers).
 ******************************************************************************/

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "hooks/hooks.h"
#include "common/formatting.h"

struct ExportPatch
{
  const char *moduleName;
  const char *exportName;
  void *hookFn;
  void **trampolineOut;
  // byte pattern the export must start with (relocation safety check)
  const byte *expectedPrefix;
  size_t expectedPrefixLen;
  // number of prologue bytes to relocate; must land on an instruction
  // boundary, be >= 5, and cover at least the expected prefix
  size_t prologueBytes;
};

inline byte *ExportPatch_AllocateNear(HMODULE mod, size_t size)
{
  uintptr_t centre = (uintptr_t)mod;
  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  const uintptr_t GB = 1024ULL * 1024ULL * 1024ULL;
  for(uintptr_t delta = si.dwAllocationGranularity; delta < 2ULL * GB;
      delta += si.dwAllocationGranularity * 64)
  {
    for(int sign = -1; sign <= 1; sign += 2)
    {
      uintptr_t addr = centre + sign * delta;
      addr &= ~(uintptr_t)(si.dwAllocationGranularity - 1);
      void *p = VirtualAlloc((void *)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
      if(p)
        return (byte *)p;
    }
  }
  return NULL;
}

inline bool ApplyExportPatch(HMODULE mod, const ExportPatch &patch)
{
  void *proc = NULL;
  {
    ScopedSuppressHooking suppress;
    proc = (void *)GetProcAddress(mod, patch.exportName);
  }
  if(!proc)
  {
    RDCLOG("ExportPatch: %s has no export %s", patch.moduleName, patch.exportName);
    return false;
  }

  byte *orig = (byte *)proc;

  if(orig[0] == 0xE9)
  {
    RDCLOG("ExportPatch: %s!%s appears already patched", patch.moduleName, patch.exportName);
    return true;
  }

  if(memcmp(orig, patch.expectedPrefix, patch.expectedPrefixLen) != 0)
  {
    RDCLOG("ExportPatch: %s!%s prologue does not match expected pattern "
           "(got %02x %02x %02x %02x %02x %02x), skipping",
           patch.moduleName, patch.exportName, orig[0], orig[1], orig[2], orig[3], orig[4],
           orig[5]);
    return false;
  }

  // layout: [0..64) trampoline (original bytes + jmp back), [64..) stub (jmp [rip+0]; hookFn)
  byte *mem = ExportPatch_AllocateNear(mod, 128);
  if(!mem)
  {
    RDCLOG("ExportPatch: failed to allocate trampoline near %s", patch.moduleName);
    return false;
  }

  byte *tramp = mem;
  byte *stub = mem + 64;

  memcpy(tramp, orig, patch.prologueBytes);
  int32_t back = int32_t((uintptr_t)(orig + patch.prologueBytes) -
                         (uintptr_t)(tramp + patch.prologueBytes + 5));
  tramp[patch.prologueBytes] = 0xE9;
  memcpy(tramp + patch.prologueBytes + 1, &back, 4);

  stub[0] = 0xFF;
  stub[1] = 0x25;
  stub[2] = 0x00;
  stub[3] = 0x00;
  stub[4] = 0x00;
  stub[5] = 0x00;
  memcpy(stub + 6, &patch.hookFn, 8);

  DWORD oldProtect = 0;
  if(!VirtualProtect(orig, patch.prologueBytes, PAGE_EXECUTE_READWRITE, &oldProtect))
  {
    RDCLOG("ExportPatch: VirtualProtect failed on %s!%s", patch.moduleName, patch.exportName);
    VirtualFree(mem, 0, MEM_RELEASE);
    return false;
  }

  int32_t fwd = int32_t((uintptr_t)stub - (uintptr_t)(orig + 5));
  orig[0] = 0xE9;
  memcpy(orig + 1, &fwd, 4);
  for(size_t i = 5; i < patch.prologueBytes; i++)
    orig[i] = 0xCC;

  FlushInstructionCache(GetCurrentProcess(), orig, patch.prologueBytes);
  FlushInstructionCache(GetCurrentProcess(), mem, 128);

  VirtualProtect(orig, patch.prologueBytes, oldProtect, &oldProtect);

  if(patch.trampolineOut)
    *patch.trampolineOut = tramp;

  RDCLOG("ExportPatch: %s!%s patched with hotpatch jump (trampoline %p)", patch.moduleName,
         patch.exportName, (void *)tramp);
  return true;
}
