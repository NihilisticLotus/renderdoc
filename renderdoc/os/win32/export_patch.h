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
#include "common/threading.h"
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
#if !defined(_WIN64)
  // The entry stub below uses the x64 RIP-relative indirect jump encoding.
  return false;
#else
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
  struct InstalledPatch
  {
    byte *entry;
    byte *trampoline;
    void *hook;
  };
  static Threading::CriticalSection installLock;
  static rdcarray<InstalledPatch> installed;
  SCOPED_LOCK(installLock);

  // Only our own recorded installation is idempotent. An E9 at the export can
  // belong to another interceptor (e.g. Steam's overlay), not to RenderDoc.
  for(const InstalledPatch &previous : installed)
  {
    if(previous.entry != orig)
      continue;
    if(previous.hook != patch.hookFn)
      return false;
    if(patch.trampolineOut)
      *patch.trampolineOut = previous.trampoline;
    return true;
  }

  byte *previousTarget = NULL;
  const bool chainJump = orig[0] == 0xE9;
  const size_t patchBytes = chainJump ? 5 : patch.prologueBytes;
  if(!patch.trampolineOut || patchBytes < 5 || patchBytes + 5 > 64 ||
     (!chainJump && patch.expectedPrefixLen > patchBytes))
    return false;

  if(chainJump)
  {
    int32_t displacement = 0;
    memcpy(&displacement, orig + 1, sizeof(displacement));
    previousTarget = orig + 5 + displacement;
    if(previousTarget == orig)
      return false;
  }
  else if(memcmp(orig, patch.expectedPrefix, patch.expectedPrefixLen) != 0)
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

  if(chainJump)
  {
    // Preserve the existing hook and its original-function trampoline. Copying
    // its relative displacement to our new address would jump to the wrong code.
    tramp[0] = 0xFF;
    tramp[1] = 0x25;
    memset(tramp + 2, 0, 4);
    memcpy(tramp + 6, &previousTarget, sizeof(previousTarget));
  }
  else
  {
    memcpy(tramp, orig, patchBytes);
    int32_t back = int32_t((uintptr_t)(orig + patchBytes) -
                           (uintptr_t)(tramp + patchBytes + 5));
    tramp[patchBytes] = 0xE9;
    memcpy(tramp + patchBytes + 1, &back, 4);
  }

  stub[0] = 0xFF;
  stub[1] = 0x25;
  stub[2] = 0x00;
  stub[3] = 0x00;
  stub[4] = 0x00;
  stub[5] = 0x00;
  memcpy(stub + 6, &patch.hookFn, 8);

  DWORD oldProtect = 0;
  if(!VirtualProtect(orig, patchBytes, PAGE_EXECUTE_READWRITE, &oldProtect))
  {
    RDCLOG("ExportPatch: VirtualProtect failed on %s!%s", patch.moduleName, patch.exportName);
    VirtualFree(mem, 0, MEM_RELEASE);
    return false;
  }

  // Publish the original call target and flush the trampoline before exposing
  // the hook entry to other threads.
  *patch.trampolineOut = tramp;
  FlushInstructionCache(GetCurrentProcess(), mem, 128);

  int32_t fwd = int32_t((uintptr_t)stub - (uintptr_t)(orig + 5));
  orig[0] = 0xE9;
  memcpy(orig + 1, &fwd, 4);
  for(size_t i = 5; i < patchBytes; i++)
    orig[i] = 0xCC;

  FlushInstructionCache(GetCurrentProcess(), orig, patchBytes);
  VirtualProtect(orig, patchBytes, oldProtect, &oldProtect);
  installed.push_back({orig, tramp, patch.hookFn});

  RDCLOG("ExportPatch: %s!%s patched (trampoline %p, previous jump target %p)",
         patch.moduleName, patch.exportName, (void *)tramp, (void *)previousTarget);
  return true;
#endif
}
