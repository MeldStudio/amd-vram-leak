# AMD GPU Texture Memory Leak Reproduction

This repository contains a minimal reproduction of a DXGI shared texture memory leak observed on AMD GPUs. The issue manifests when creating and destroying shared DXGI textures across separate D3D11 devices backed by the same physical GPU.

## The Issue

When creating `DXGI_FORMAT_R8G8B8A8_UNORM` textures with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED` flags and sharing them between devices:

1. **Factory Device**: Creates textures and returns shared NT handles
2. **Render Device**: Opens the shared handle, renders to the texture, waits for GPU completion via fence
3. **Disposal**: The shared handle is closed via `CloseHandle()`

**Expected behavior**: After `CloseHandle()` is called and all references are released, the GPU memory should be freed.

**Actual behavior on AMD**: The GPU memory is **never freed**. VRAM usage climbs continuously until the system runs out of GPU memory, even though:
- The `ID3D11Texture2D` COM objects have been released
- The shared `HANDLE` has been closed
- The GPU has finished all operations (verified via `ID3D11Fence` wait)

## The Workaround

Adding a `Flush()` call on the factory device's immediate context **after** closing the shared handle causes the memory to be properly released:

```cpp
void DestroyGpuMemoryBuffer(int id, HANDLE handle) {
    CloseHandle(handle);
    
    // THIS FIXES THE LEAK ON AMD:
    ComPtr<ID3D11DeviceContext> context;
    d3d11_device_->GetImmediateContext(&context);
    context->Flush();
}
```

## Running the Reproduction

```powershell
# Build
.\with-devenv.ps1 cmake -B out/dev -G Ninja
.\with-devenv.ps1 ninja -C out/dev

# Run WITHOUT fix - observe VRAM climbing continuously
.\out\dev\texture_repro.exe

# Run WITH fix - VRAM stays stable
.\out\dev\texture_repro.exe -flush-after-handle-close
```

## Observing the Leak

The program displays real-time VRAM usage via AMD's ADLX SDK. You can also monitor via:
- Task Manager → Performance → GPU → Dedicated GPU memory
- AMD Adrenalin software

Without `-flush-after-handle-close`:
- VRAM increases by ~31.6 MB per iteration (10 × 3840×2160×4 bytes)
- Memory is never reclaimed
- Eventually leads to out-of-memory errors

With `-flush-after-handle-close`:
- VRAM stays relatively stable
- Memory is properly recycled

## Technical Details

- **Texture size**: 3840×2160 RGBA (approx. 31.6 MB each)
- **Textures per iteration**: 10
- **Format**: `DXGI_FORMAT_R8G8B8A8_UNORM`
- **Sharing flags**: `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED`
- **GPU sync**: `ID3D11Fence` with CPU wait ensures GPU has completed before disposal

## Context

This pattern mirrors Chromium's `GpuMemoryBufferFactoryDXGI` which creates shared textures on one device and allows them to be used by other devices/processes. The leak causes issues in applications that frequently create and destroy shared GPU resources.

## System Information

Tested on:
- AMD Radeon 890M (integrated)
- Windows 11
- Latest AMD drivers

## Command Line Options

```
-help                         Show help message
-flush-after-handle-close     Enable the workaround (flush after closing handles)
```