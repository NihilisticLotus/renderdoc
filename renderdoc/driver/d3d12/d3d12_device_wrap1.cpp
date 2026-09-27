/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "d3d12_device.h"
#include "d3d12_resources.h"

HRESULT WrappedID3D12Device::CreatePipelineLibrary(_In_reads_(BlobLength) const void *pLibraryBlob,
                                                   SIZE_T BlobLength, REFIID riid,
                                                   _COM_Outptr_ void **ppPipelineLibrary)
{
  RDCLOG("CreatePipelineLibrary: blob=%p length=%llu iid=%s output=%p", pLibraryBlob,
         (unsigned long long)BlobLength, ToStr(riid).c_str(), ppPipelineLibrary);
  // CreatePipelineLibrary supports doing a dry run if ppPipelineLibrary receives
  // nullptr. That feature is optional and not supported in every driver, since
  // we are not supporting pipeline libraries anyway, returns the unsupported
  // driver case.
  if(ppPipelineLibrary == NULL)
    return DXGI_ERROR_UNSUPPORTED;

// Opaque disk caches hide pipeline bytecode/configuration. Reject those blobs,
// but allow an in-process library: its successful loads are recreated through
// wrapped Create*Pipeline* calls using the application's complete descriptor.
#ifndef D3D12_ERROR_DRIVER_VERSION_MISMATCH
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH _HRESULT_TYPEDEF_(0x887E0002L)
#endif

  // Serialize() emits this empty-cache token. Applications may serialize and recreate the
  // library during the same run, so accept our own token instead of reporting a driver change.
  // Native cache blobs must still be rejected: their opaque PSOs cannot be captured.
  const byte emptyCache[WrappedID3D12PipelineLibrary::DummyBytes] = {};
  const bool ownEmptyCache = pLibraryBlob && BlobLength == sizeof(emptyCache) &&
                             memcmp(pLibraryBlob, emptyCache, sizeof(emptyCache)) == 0;
  if(BlobLength > 0 && !ownEmptyCache)
  {
    RDCLOG("CreatePipelineLibrary: rejecting cached blob with DRIVER_VERSION_MISMATCH");
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
  }

  ID3D12PipelineLibrary *nativeLibrary = NULL;
  HRESULT hr = m_pDevice1->CreatePipelineLibrary(NULL, 0, __uuidof(ID3D12PipelineLibrary),
                                               (void **)&nativeLibrary);
  if(FAILED(hr))
  {
    *ppPipelineLibrary = NULL;
    return hr;
  }
  WrappedID3D12PipelineLibrary *pipeLibrary =
      new WrappedID3D12PipelineLibrary(ResourceId(), this, nativeLibrary);

  if(riid == __uuidof(ID3D12PipelineLibrary))
  {
    *ppPipelineLibrary = (ID3D12PipelineLibrary *)pipeLibrary;
  }
  else if(riid == __uuidof(ID3D12PipelineLibrary1))
  {
    *ppPipelineLibrary = (ID3D12PipelineLibrary1 *)pipeLibrary;
  }
  else
  {
    RDCERR("Unexpected interface type %s", ToStr(riid).c_str());
    pipeLibrary->Release();
    *ppPipelineLibrary = NULL;
    return E_NOINTERFACE;
  }

  RDCLOG("CreatePipelineLibrary: returning capture-visible library %p", *ppPipelineLibrary);
  return S_OK;
}

HRESULT WrappedID3D12Device::SetEventOnMultipleFenceCompletion(
    _In_reads_(NumFences) ID3D12Fence *const *ppFences,
    _In_reads_(NumFences) const UINT64 *pFenceValues, UINT NumFences,
    D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent)
{
  ID3D12Fence **unwrapped = GetTempArray<ID3D12Fence *>(NumFences);

  for(UINT i = 0; i < NumFences; i++)
    unwrapped[i] = Unwrap(ppFences[i]);

  return m_pDevice1->SetEventOnMultipleFenceCompletion(unwrapped, pFenceValues, NumFences, Flags,
                                                       hEvent);
}

HRESULT WrappedID3D12Device::SetResidencyPriority(UINT NumObjects,
                                                  _In_reads_(NumObjects)
                                                      ID3D12Pageable *const *ppObjects,
                                                  _In_reads_(NumObjects)
                                                      const D3D12_RESIDENCY_PRIORITY *pPriorities)
{
  ID3D12Pageable **unwrapped = GetTempArray<ID3D12Pageable *>(NumObjects);

  for(UINT i = 0; i < NumObjects; i++)
    unwrapped[i] = (ID3D12Pageable *)Unwrap((ID3D12DeviceChild *)ppObjects[i]);

  return m_pDevice1->SetResidencyPriority(NumObjects, unwrapped, pPriorities);
}
