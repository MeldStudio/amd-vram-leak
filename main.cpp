// clang-format off
#include <windows.h>
#include <commctrl.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <wrl/client.h>
// clang-format on

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// AMD ADLX SDK for GPU metrics
#include "SDK/ADLXHelper/Windows/Cpp/ADLXHelper.h"
#include "SDK/Include/IPerformanceMonitoring.h"
#include "SDK/Include/ISystem2.h"

#pragma comment(linker, \
                "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

// Windows 10 1809+ dark mode support via undocumented APIs
// These are the ordinals for SetPreferredAppMode in uxtheme.dll
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };

using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
using fnRefreshImmersiveColorPolicyState = void(WINAPI*)();
using fnFlushMenuThemes = void(WINAPI*)();

// Dark mode colors - modern Windows 11 style
namespace DarkColors {
constexpr COLORREF Background = RGB(32, 32, 32);
constexpr COLORREF Surface = RGB(43, 43, 43);
constexpr COLORREF SurfaceLight = RGB(55, 55, 55);
constexpr COLORREF Border = RGB(68, 68, 68);
constexpr COLORREF Text = RGB(255, 255, 255);
constexpr COLORREF TextSecondary = RGB(180, 180, 180);
constexpr COLORREF Accent = RGB(0, 120, 212);
constexpr COLORREF AccentHover = RGB(26, 140, 230);
constexpr COLORREF AccentPressed = RGB(0, 95, 184);
constexpr COLORREF ProgressBar = RGB(0, 120, 212);
constexpr COLORREF ProgressBarBg = RGB(55, 55, 55);
constexpr COLORREF ButtonBg = RGB(55, 55, 55);
constexpr COLORREF ButtonHover = RGB(70, 70, 70);
constexpr COLORREF ButtonPressed = RGB(45, 45, 45);
constexpr COLORREF GroupBoxBorder = RGB(68, 68, 68);
// Accent button colors (for highlighting important actions)
constexpr COLORREF AccentButtonBg = RGB(0, 120, 212);
constexpr COLORREF AccentButtonHover = RGB(26, 140, 230);
constexpr COLORREF AccentButtonPressed = RGB(0, 95, 184);
}  // namespace DarkColors

namespace {

// Size of texture we will allocate every timer tick
constexpr int kTextureWidth = 3840;
constexpr int kTextureHeight = 2160;

// Window dimensions
constexpr int kWindowWidth = 500;
constexpr int kWidowHeight = 520;

// Texture size in bytes (3840 x 2160 x 4 bytes for RGBA8)
constexpr size_t kTextureSizeBytes = kTextureWidth * kTextureHeight * 4;
constexpr double kTextureSizeMB = kTextureSizeBytes / (1024.0 * 1024.0);

// Timer ID for periodic updates
constexpr UINT_PTR kTimerUpdateId = 1;
constexpr UINT kTimerUpdateIntervalMs = 100;  // Update every 100ms

// Control IDs
constexpr int ID_GPU_NAME = 1001;
constexpr int ID_DRIVER_VERSION = 1002;
constexpr int ID_VRAM_USAGE = 1003;
constexpr int ID_VRAM_PROGRESS = 1004;
constexpr int ID_GPU_ALLOCATED = 1005;
constexpr int ID_GPU_FREED = 1006;
constexpr int ID_STATUS = 1009;
constexpr int ID_START_STOP_BTN = 1010;
constexpr int ID_FLUSH_RENDER_BTN = 1011;
constexpr int ID_FLUSH_FACTORY_BTN = 1012;

// Global UI handles
constinit HWND g_hwnd = nullptr;
constinit HWND g_gpu_name_label = nullptr;
constinit HWND g_driver_version_label = nullptr;
constinit HWND g_vram_usage_label = nullptr;
constinit HWND g_vram_progress = nullptr;
constinit HWND g_gpu_allocated_label = nullptr;
constinit HWND g_gpu_freed_label = nullptr;
constinit HWND g_status_label = nullptr;
constinit HWND g_start_stop_btn = nullptr;
constinit HWND g_flush_render_btn = nullptr;
constinit HWND g_flush_factory_btn = nullptr;
constinit HFONT g_font = nullptr;
constinit HFONT g_bold_font = nullptr;

// Dark mode state and brushes
constinit HBRUSH g_bg_brush = nullptr;
constinit HBRUSH g_surface_brush = nullptr;
constinit HBRUSH g_button_brush = nullptr;
constinit HBRUSH g_button_hover_brush = nullptr;
constinit HBRUSH g_button_pressed_brush = nullptr;
constinit HBRUSH g_accent_button_brush = nullptr;
constinit HBRUSH g_accent_button_hover_brush = nullptr;
constinit HBRUSH g_accent_button_pressed_brush = nullptr;

// Button subclass tracking for hover state
struct ButtonState {
  HWND hwnd = nullptr;
  bool hovered = false;
  bool pressed = false;
  bool is_accent = false;  // Use accent (blue) styling
  WNDPROC original_proc = nullptr;
};
constexpr int kMaxButtons = 4;
ButtonState g_button_states[kMaxButtons];
int g_button_count = 0;

// Global configuration
struct Config {
  bool flush_after_handle_close = false;
};
constinit Config g_config;

// Minimal std::source_location that only stores the file name and line number.
struct SourceLocation {
  constexpr SourceLocation() = default;

  static consteval SourceLocation Here(
      const char* file = __builtin_FILE(),
      std::uint_least32_t line = __builtin_LINE()) noexcept {
    return SourceLocation{file, line};
  }

  constexpr std::string_view file_name() const noexcept { return filename_; }
  constexpr std::uint_least32_t line() const noexcept { return line_; }

 private:
  constexpr SourceLocation(const char* file, std::uint_least32_t line) noexcept
      : filename_{extract_filename(file)}, line_{line} {}

  static constexpr const char* extract_filename(const char* path) noexcept {
    const char* filename = path;
    for (const char* p = path; *p != '\0'; ++p) {
      if (*p == '/' || *p == '\\') {
        filename = p + 1;
      }
    }
    return filename;
  }

  const char* filename_{nullptr};
  std::uint_least32_t line_{};
};

std::string hr_to_string(HRESULT hr) {
  switch (hr) {
    case S_OK:
      return "S_OK";
    case S_FALSE:
      return "S_FALSE";
    case E_FAIL:
      return "E_FAIL";
    case E_INVALIDARG:
      return "E_INVALIDARG";
    case E_OUTOFMEMORY:
      return "E_OUTOFMEMORY";
    case E_NOTIMPL:
      return "E_NOTIMPL";
    case E_NOINTERFACE:
      return "E_NOINTERFACE";
    case E_POINTER:
      return "E_POINTER";
    case E_UNEXPECTED:
      return "E_UNEXPECTED";
    case E_ACCESSDENIED:
      return "E_ACCESSDENIED";
    case E_HANDLE:
      return "E_HANDLE";
    case E_ABORT:
      return "E_ABORT";
    case E_PENDING:
      return "E_PENDING";
    case E_BOUNDS:
      return "E_BOUNDS";
    case E_CHANGED_STATE:
      return "E_CHANGED_STATE";
    case E_ILLEGAL_METHOD_CALL:
      return "E_ILLEGAL_METHOD_CALL";
    case DXGI_ERROR_DEVICE_REMOVED:
      return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_NOT_FOUND:
      return "DXGI_ERROR_NOT_FOUND";
    case DXGI_ERROR_INVALID_CALL:
      return "DXGI_ERROR_INVALID_CALL";
    default: {
      char buf[32];
      snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
      return buf;
    }
  }
}

inline HRESULT CHECK_RESULT(
    HRESULT hr,
    const SourceLocation& loc = SourceLocation::Here()) {
  if (FAILED(hr)) [[unlikely]] {
    char msg[256];
    snprintf(msg, sizeof(msg), "ERROR(%.*s:%u) HRESULT CHECK FAILED %s\n",
             static_cast<int>(loc.file_name().size()), loc.file_name().data(),
             loc.line(), hr_to_string(hr).c_str());
    OutputDebugStringA(msg);
    std::abort();
  }
  return hr;
}

// Get the high performance adapter. Prefer DXGI's high performance hint and
// skip software adapters.
ComPtr<IDXGIAdapter1> GetHighPerformanceAdapter() {
  // Try GPU-preference aware enumeration first (DXGI 1.6+)
  ComPtr<IDXGIFactory6> dxgi_factory6;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory6)))) {
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      if (FAILED(dxgi_factory6->EnumAdapterByGpuPreference(
              index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
              IID_PPV_ARGS(&adapter)))) {
        break;  // No more adapters
      }
      DXGI_ADAPTER_DESC1 desc;
      if (FAILED(adapter->GetDesc1(&desc))) {
        continue;
      }
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        continue;
      }
      return adapter;
    }
  }

  // Fallback: pick the first adapter.
  ComPtr<IDXGIFactory1> dxgi_factory;
  HRESULT hr = CHECK_RESULT(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)));
  ComPtr<IDXGIAdapter1> adapter;
  hr = CHECK_RESULT(dxgi_factory->EnumAdapters1(0, &adapter));
  return adapter;
}

// Class to query GPU metrics using AMD ADLX SDK
// This provides direct access to AMD GPU internal metrics
class GpuMetrics {
 public:
  explicit GpuMetrics(const LUID& adapter_luid)
      : adapter_luid_(adapter_luid),
        has_target_luid_(adapter_luid.HighPart != 0 ||
                         adapter_luid.LowPart != 0) {
    // Initialize ADLX
    ADLX_RESULT res = g_ADLX.Initialize();
    if (!ADLX_SUCCEEDED(res)) {
      printf("Warning: ADLX initialization failed (0x%08X)\n", res);
      return;
    }
    initialized_ = true;

    // Get system services
    auto* system = g_ADLX.GetSystemServices();
    if (!system) {
      printf("Warning: Failed to get ADLX system services\n");
      return;
    }

    // Get GPU list
    adlx::IADLXGPUListPtr gpus;
    res = system->GetGPUs(&gpus);
    if (!ADLX_SUCCEEDED(res) || !gpus || gpus->Size() == 0) {
      printf("Warning: No GPUs found via ADLX\n");
      return;
    }

    // Prefer the GPU that matches our DXGI adapter LUID so metrics track the
    // same device we render on. Fall back to the first
    // GPU if no better match is available.
    adlx::IADLXGPUPtr matched_gpu;

    for (adlx_uint i = 0; i < gpus->Size(); ++i) {
      adlx::IADLXGPUPtr gpu;
      res = gpus->At(i, &gpu);
      if (!ADLX_SUCCEEDED(res) || !gpu) {
        continue;
      }

      adlx::IADLXGPU2* gpu2 = nullptr;
      if (ADLX_SUCCEEDED(gpu->QueryInterface(adlx::IADLXGPU2::IID(),
                                             reinterpret_cast<void**>(&gpu2))) &&
          gpu2) {
        ADLX_LUID adlx_luid{};
        const bool luid_read =
            ADLX_SUCCEEDED(gpu2->LUID(&adlx_luid));
        const bool luid_matches =
            has_target_luid_ && luid_read &&
            adlx_luid.lowPart ==
                static_cast<adlx_ulong>(adapter_luid_.LowPart) &&
            adlx_luid.highPart ==
                static_cast<adlx_long>(adapter_luid_.HighPart);

        if (luid_matches) {
          matched_gpu = gpu;
          gpu2->Release();
          break;
        }
        gpu2->Release();
      }
    }

    if (matched_gpu) {
      gpu_ = matched_gpu;
    } else {
      res = gpus->At(0, &gpu_);
    }

    if (!gpu_) {
      printf("Warning: Failed to get GPU from ADLX\n");
      return;
    }

    // Get total VRAM for reference
    adlx_uint vram_mb = 0;
    if (ADLX_SUCCEEDED(gpu_->TotalVRAM(&vram_mb))) {
      total_vram_mb_ = vram_mb;
    }

    // Get performance monitoring services
    res = system->GetPerformanceMonitoringServices(&perf_services_);
    if (!ADLX_SUCCEEDED(res) || !perf_services_) {
      printf("Warning: Failed to get ADLX performance monitoring services\n");
      return;
    }

    // Check GPU metrics support
    res = perf_services_->GetSupportedGPUMetrics(gpu_, &gpu_metrics_support_);
    if (!ADLX_SUCCEEDED(res) || !gpu_metrics_support_) {
      printf("Warning: Failed to get GPU metrics support\n");
      return;
    }

    // Check if VRAM metric is supported
    adlx_bool vram_supported = false;
    res = gpu_metrics_support_->IsSupportedGPUVRAM(&vram_supported);
    if (!ADLX_SUCCEEDED(res) || !vram_supported) {
      printf("Warning: GPU VRAM metric not supported\n");
    }

    ready_ = true;
  }

  ~GpuMetrics() {
    // Release interfaces before terminating
    gpu_metrics_support_.Release();
    perf_services_.Release();
    gpu_.Release();

    if (initialized_) {
      g_ADLX.Terminate();
    }
  }

  // Returns dedicated GPU memory usage in bytes
  size_t GetDedicatedUsage() {
    if (!ready_ || !perf_services_ || !gpu_) {
      return 0;
    }

    // Get current GPU metrics
    adlx::IADLXGPUMetricsPtr metrics;
    ADLX_RESULT res = perf_services_->GetCurrentGPUMetrics(gpu_, &metrics);
    if (!ADLX_SUCCEEDED(res) || !metrics) {
      return 0;
    }

    // Get VRAM usage in MB
    adlx_int vram_mb = 0;
    res = metrics->GPUVRAM(&vram_mb);
    if (!ADLX_SUCCEEDED(res)) {
      return 0;
    }

    // Convert MB to bytes
    return static_cast<size_t>(vram_mb) * 1024 * 1024;
  }

  // Get total VRAM in MB
  adlx_uint GetTotalVRAM() const { return total_vram_mb_; }

  // Check if ADLX is working
  bool IsReady() const { return ready_; }

  // Get AMD driver version string
  const char* GetDriverVersion() {
    if (!gpu_) {
      return nullptr;
    }
    // Query IADLXGPU2 interface for driver version
    adlx::IADLXGPU2* gpu2 = nullptr;
    ADLX_RESULT res = gpu_->QueryInterface(adlx::IADLXGPU2::IID(),
                                           reinterpret_cast<void**>(&gpu2));
    if (!ADLX_SUCCEEDED(res) || !gpu2) {
      return nullptr;
    }
    const char* version = nullptr;
    res = gpu2->DriverVersion(&version);
    gpu2->Release();
    if (!ADLX_SUCCEEDED(res)) {
      return nullptr;
    }
    return version;
  }

 private:
  LUID adapter_luid_{};
  bool has_target_luid_ = false;
  bool initialized_ = false;
  bool ready_ = false;
  adlx_uint total_vram_mb_ = 0;
  adlx::IADLXGPUPtr gpu_;
  adlx::IADLXPerformanceMonitoringServicesPtr perf_services_;
  adlx::IADLXGPUMetricsSupportPtr gpu_metrics_support_;
};

// Global metrics state (updated by worker thread and destruction callback, read
// by UI)
struct MetricsState {
  std::atomic<double> gpu_allocated_mb{0.0};
  std::atomic<double> gpu_freed_mb{0.0};
  char status[64] = "Idle";
};
static MetricsState g_metrics;

// Forward declaration
class GpuMemoryBufferFactoryDXGI;

// Wrapped handle type that calls back into the factory on destruction
class GpuMemoryBufferHandle {
 public:
  GpuMemoryBufferHandle() = default;

  GpuMemoryBufferHandle(HANDLE handle,
                        int id,
                        GpuMemoryBufferFactoryDXGI* factory)
      : handle_(handle), id_(id), factory_(factory) {}

  ~GpuMemoryBufferHandle();

  GpuMemoryBufferHandle(const GpuMemoryBufferHandle&) = delete;
  GpuMemoryBufferHandle& operator=(const GpuMemoryBufferHandle&) = delete;

  GpuMemoryBufferHandle(GpuMemoryBufferHandle&& other) noexcept
      : handle_(other.handle_), id_(other.id_), factory_(other.factory_) {
    other.handle_ = nullptr;
    other.factory_ = nullptr;
  }

  GpuMemoryBufferHandle& operator=(GpuMemoryBufferHandle&& other) noexcept {
    if (this != &other) {
      Release();
      handle_ = other.handle_;
      id_ = other.id_;
      factory_ = other.factory_;
      other.handle_ = nullptr;
      other.factory_ = nullptr;
    }
    return *this;
  }

  void Release();

  HANDLE Get() const { return handle_; }
  int Id() const { return id_; }
  bool IsValid() const { return handle_ != nullptr; }
  explicit operator bool() const { return IsValid(); }

 private:
  HANDLE handle_ = nullptr;
  int id_ = 0;
  GpuMemoryBufferFactoryDXGI* factory_ = nullptr;
};

// Minimal reproduction of Chromium's GpuMemoryBufferFactoryDXGI
// Creates DXGI_FORMAT_R8G8B8A8_UNORM textures that can be shared cross-process
class GpuMemoryBufferFactoryDXGI {
 public:
  GpuMemoryBufferFactoryDXGI() = default;
  ~GpuMemoryBufferFactoryDXGI() = default;

  GpuMemoryBufferFactoryDXGI(const GpuMemoryBufferFactoryDXGI&) = delete;
  GpuMemoryBufferFactoryDXGI& operator=(const GpuMemoryBufferFactoryDXGI&) =
      delete;

  // Creates a shareable GPU memory buffer texture
  // Returns a wrapped handle that will call DestroyGpuMemoryBuffer on
  // destruction
  GpuMemoryBufferHandle CreateGpuMemoryBuffer(int width, int height) {
    auto d3d11_device = GetOrCreateD3D11Device();
    if (!d3d11_device) {
      printf("Failed to get or create D3D11 device\n");
      return {};
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    ComPtr<ID3D11Texture2D> d3d11_texture;
    HRESULT hr = d3d11_device->CreateTexture2D(&desc, nullptr, &d3d11_texture);
    if (FAILED(hr)) {
      printf("CreateTexture2D failed with error 0x%08lx\n", hr);
      return {};
    }

    // Register destruction notifier to track when texture is actually freed
    ComPtr<ID3DDestructionNotifier> notifier;
    hr = d3d11_texture.As(&notifier);
    if (SUCCEEDED(hr) && notifier) {
      UINT cookie = 0;
      notifier->RegisterDestructionCallback(
          [](void* /* pData */) {
            // Texture has been destroyed - update freed metric
            g_metrics.gpu_freed_mb =
                g_metrics.gpu_freed_mb.load() + kTextureSizeMB;
          },
          nullptr, &cookie);
    }

    // Update allocated metric
    g_metrics.gpu_allocated_mb =
        g_metrics.gpu_allocated_mb.load() + kTextureSizeMB;

    ComPtr<IDXGIResource1> dxgi_resource;
    CHECK_RESULT(d3d11_texture.As(&dxgi_resource));

    HANDLE texture_handle = nullptr;
    CHECK_RESULT(dxgi_resource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &texture_handle));

    int id = next_buffer_id_++;
    return GpuMemoryBufferHandle(texture_handle, id, this);
  }

  // Called when the GpuMemoryBufferHandle is destroyed
  // Mirrors Chromium's DestroyGpuMemoryBuffer which flushes the device context
  void DestroyGpuMemoryBuffer(int id, HANDLE handle) {
    if (handle) {
      bool result = CloseHandle(handle);
      if (!result) {
        fprintf(stderr, "ERROR: CloseHandle failed for id %d\n", id);
      }
    } else {
      fprintf(
          stderr,
          "ERROR: DestroyGpuMemoryBuffer called with null handle for id %d\n",
          id);
    }
    if (g_config.flush_after_handle_close && d3d11_device_) {
      ComPtr<ID3D11DeviceContext> d3d11_device_context;
      d3d11_device_->GetImmediateContext(&d3d11_device_context);
      d3d11_device_context->Flush();
    }
  }

  // Flush the device context
  void Flush() {
    if (d3d11_device_) {
      ComPtr<ID3D11DeviceContext> d3d11_device_context;
      d3d11_device_->GetImmediateContext(&d3d11_device_context);
      d3d11_device_context->Flush();
    }
  }

 private:
  ID3D11Device* GetDevice() const { return d3d11_device_.Get(); }

  ComPtr<ID3D11Device> GetOrCreateD3D11Device() {
    if (d3d11_device_) {
      HRESULT hr = d3d11_device_->GetDeviceRemovedReason();
      if (FAILED(hr)) {
        printf("Device removed, will recreate. Reason: 0x%08lx\n", hr);
        d3d11_device_ = nullptr;
      }
    }

    if (!d3d11_device_) {
      auto dxgi_adapter = GetHighPerformanceAdapter();

      DXGI_ADAPTER_DESC1 adapter_desc;
      CHECK_RESULT(dxgi_adapter->GetDesc1(&adapter_desc));

      // If adapter is not null, driver type must be D3D_DRIVER_TYPE_UNKNOWN
      const D3D_DRIVER_TYPE driver_type = D3D_DRIVER_TYPE_UNKNOWN;

      // Single-threaded device since this is only used for texture creation
      const UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;

      const D3D_FEATURE_LEVEL feature_levels[] = {
          D3D_FEATURE_LEVEL_11_1,
          D3D_FEATURE_LEVEL_11_0,
          D3D_FEATURE_LEVEL_10_1,
          D3D_FEATURE_LEVEL_10_0,
      };

      HRESULT hr = D3D11CreateDevice(
          dxgi_adapter.Get(), driver_type,
          nullptr,  // Software
          flags, feature_levels, static_cast<UINT>(std::size(feature_levels)),
          D3D11_SDK_VERSION, &d3d11_device_,
          nullptr,  // pFeatureLevel
          nullptr   // ppImmediateContext
      );
      if (FAILED(hr)) {
        OutputDebugStringA(
            ("D3D11CreateDevice failed: " + hr_to_string(hr) + "\n").c_str());
        return nullptr;
      }
    }

    return d3d11_device_;
  }

  ComPtr<ID3D11Device> d3d11_device_;
  int next_buffer_id_ = 1;
};

// Implementation of GpuMemoryBufferHandle destructor (after
// GpuMemoryBufferFactoryDXGI is defined)
GpuMemoryBufferHandle::~GpuMemoryBufferHandle() {
  Release();
}

void GpuMemoryBufferHandle::Release() {
  if (factory_ && handle_) {
    factory_->DestroyGpuMemoryBuffer(id_, handle_);
    handle_ = nullptr;
    factory_ = nullptr;
  }
}

// Separate device for rendering to shared textures
class RenderDevice {
 public:
  bool Initialize(IDXGIAdapter* adapter) {
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    const UINT flags = 0;  // Can use multi-threaded
    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, feature_levels,
        static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION,
        &device_, nullptr, &context_);
    if (FAILED(hr)) {
      OutputDebugStringA(("D3D11CreateDevice for RenderDevice failed: " +
                          hr_to_string(hr) + "\n")
                             .c_str());
      return false;
    }

    // Get ID3D11Device5 for fence support
    CHECK_RESULT(device_.As(&device5_));

    // Get ID3D11DeviceContext4 for fence signaling
    CHECK_RESULT(context_.As(&context4_));

    // Create fence for GPU synchronization
    CHECK_RESULT(
        device5_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));

    // Create event for CPU wait
    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
      OutputDebugStringA("CreateEvent failed\n");
      return false;
    }

    return true;
  }

  ~RenderDevice() {
    if (fence_event_) {
      CloseHandle(fence_event_);
    }
  }

  // Open shared texture and render a solid color to it
  // Returns after GPU has completed rendering (CPU wait on fence)
  bool RenderToSharedTexture(HANDLE shared_handle,
                             float r,
                             float g,
                             float b,
                             float a) {
    ComPtr<ID3D11Device1> device1;
    CHECK_RESULT(device_.As(&device1));

    ComPtr<ID3D11Texture2D> texture;
    CHECK_RESULT(
        device1->OpenSharedResource1(shared_handle, IID_PPV_ARGS(&texture)));

    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK_RESULT(device_->CreateRenderTargetView(texture.Get(), nullptr, &rtv));

    float clear_color[4] = {r, g, b, a};
    context_->ClearRenderTargetView(rtv.Get(), clear_color);

    // Signal the fence after rendering
    ++fence_value_;
    CHECK_RESULT(context4_->Signal(fence_.Get(), fence_value_));

    // CPU wait for GPU to complete
    if (fence_->GetCompletedValue() < fence_value_) {
      CHECK_RESULT(fence_->SetEventOnCompletion(fence_value_, fence_event_));
      WaitForSingleObject(fence_event_, INFINITE);
    }

    return true;
  }

  ID3D11Device* GetDevice() const { return device_.Get(); }
  ID3D11DeviceContext* GetContext() const { return context_.Get(); }

  // Flush the device context
  void Flush() {
    if (context_) {
      context_->Flush();
    }
  }

 private:
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Device5> device5_;
  ComPtr<ID3D11DeviceContext4> context4_;
  ComPtr<ID3D11Fence> fence_;
  HANDLE fence_event_ = nullptr;
  UINT64 fence_value_ = 0;
};

// Global state for the test loop
constinit std::atomic_bool g_running{};
constinit std::atomic_bool g_thread_active{};

// Global GPU info
static std::wstring g_gpu_name;
static std::string g_driver_version;
static adlx_uint g_total_vram_mb = 0;

// Global pointers for worker thread
static GpuMetrics* g_gpu_metrics = nullptr;
static GpuMemoryBufferFactoryDXGI* g_factory = nullptr;
static RenderDevice* g_render_device = nullptr;
static ComPtr<IDXGIAdapter1> g_adapter;

struct ColorWithName {
  std::array<float, 4> color;
  std::string_view name;
};

constexpr auto kColors = std::array{
    ColorWithName{{1.0f, 0.0f, 0.0f, 1.0f}, "Red"},
    ColorWithName{{0.0f, 1.0f, 0.0f, 1.0f}, "Green"},
    ColorWithName{{0.0f, 0.0f, 1.0f, 1.0f}, "Blue"},
    ColorWithName{{1.0f, 1.0f, 0.0f, 1.0f}, "Yellow"},
    ColorWithName{{1.0f, 0.0f, 1.0f, 1.0f}, "Magenta"},
    ColorWithName{{0.0f, 1.0f, 1.0f, 1.0f}, "Cyan"},
    ColorWithName{{1.0f, 0.5f, 0.0f, 1.0f}, "Orange"},
    ColorWithName{{0.5f, 0.0f, 1.0f, 1.0f}, "Purple"},
    ColorWithName{{0.5f, 0.5f, 0.5f, 1.0f}, "Gray"},
    ColorWithName{{1.0f, 1.0f, 1.0f, 1.0f}, "White"},
};

// Worker thread function
void TextureTestThread() {
  g_thread_active = true;
  snprintf(g_metrics.status, sizeof(g_metrics.status), "Running...");

  uint32_t frame_idx = 0;

  while (g_running) {
    int i = frame_idx % static_cast<uint32_t>(kColors.size());
    ++frame_idx;
    auto color = kColors[i];
    snprintf(g_metrics.status, sizeof(g_metrics.status), "Creating %s...",
             color.name.data());

    // Factory creates the texture (also updates gpu_allocated_mb via
    // destruction notifier)
    auto buffer =
        g_factory->CreateGpuMemoryBuffer(kTextureWidth, kTextureHeight);
    if (!buffer) {
      snprintf(g_metrics.status, sizeof(g_metrics.status), "Create failed!");
      continue;
    }

    snprintf(g_metrics.status, sizeof(g_metrics.status), "Rendering %s...",
             color.name.data());

    if (!g_render_device->RenderToSharedTexture(buffer.Get(), color.color[0],
                                                color.color[1], color.color[2],
                                                color.color[3])) {
      snprintf(g_metrics.status, sizeof(g_metrics.status), "Render failed!");
    }

    snprintf(g_metrics.status, sizeof(g_metrics.status), "Disposing %s...",
             color.name.data());

    // Buffer goes out of scope here - destruction notifier will update
    // gpu_freed_mb

    std::this_thread::sleep_for(std::chrono::milliseconds(33));  // ~30 FPS
  }

  snprintf(g_metrics.status, sizeof(g_metrics.status), "Stopped");
  g_thread_active = false;
}

// Initialize dark mode for the application
void InitDarkMode() {
  HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!uxtheme)
    return;

  // Ordinal 135 = SetPreferredAppMode (Windows 10 1903+)
  auto SetPreferredAppMode =
      reinterpret_cast<fnSetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
  if (SetPreferredAppMode) {
    SetPreferredAppMode(PreferredAppMode::ForceDark);
  }

  // Ordinal 136 = FlushMenuThemes
  auto FlushMenuThemes =
      reinterpret_cast<fnFlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
  if (FlushMenuThemes) {
    FlushMenuThemes();
  }
}

// Enable dark mode for a specific window
void EnableDarkModeForWindow(HWND hwnd) {
  // Use DWMWA_USE_IMMERSIVE_DARK_MODE (value 20) for Windows 10 20H1+
  // For Windows 10 1809-19H2, use value 19
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

  // Also try the older attribute for compatibility
  DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

  // Try uxtheme AllowDarkModeForWindow
  HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll");
  if (uxtheme) {
    auto AllowDarkModeForWindow =
        reinterpret_cast<fnAllowDarkModeForWindow>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
    if (AllowDarkModeForWindow) {
      AllowDarkModeForWindow(hwnd, true);
    }
  }
}

// Create GDI brushes for dark theme
void CreateDarkBrushes() {
  g_bg_brush = CreateSolidBrush(DarkColors::Background);
  g_surface_brush = CreateSolidBrush(DarkColors::Surface);
  g_button_brush = CreateSolidBrush(DarkColors::ButtonBg);
  g_button_hover_brush = CreateSolidBrush(DarkColors::ButtonHover);
  g_button_pressed_brush = CreateSolidBrush(DarkColors::ButtonPressed);
  g_accent_button_brush = CreateSolidBrush(DarkColors::AccentButtonBg);
  g_accent_button_hover_brush = CreateSolidBrush(DarkColors::AccentButtonHover);
  g_accent_button_pressed_brush = CreateSolidBrush(DarkColors::AccentButtonPressed);
}

// Clean up GDI brushes
void DestroyDarkBrushes() {
  if (g_bg_brush) DeleteObject(g_bg_brush);
  if (g_surface_brush) DeleteObject(g_surface_brush);
  if (g_button_brush) DeleteObject(g_button_brush);
  if (g_button_hover_brush) DeleteObject(g_button_hover_brush);
  if (g_button_pressed_brush) DeleteObject(g_button_pressed_brush);
  if (g_accent_button_brush) DeleteObject(g_accent_button_brush);
  if (g_accent_button_hover_brush) DeleteObject(g_accent_button_hover_brush);
  if (g_accent_button_pressed_brush) DeleteObject(g_accent_button_pressed_brush);
}

// Draw a rounded rectangle with optional fill and border
void DrawRoundedRect(HDC hdc, const RECT& rect, int radius, HBRUSH fill, COLORREF border_color) {
  HPEN border_pen = CreatePen(PS_SOLID, 1, border_color);
  HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, border_pen));
  HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, fill ? fill : static_cast<HBRUSH>(GetStockObject(NULL_BRUSH))));

  RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);

  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(border_pen);
}

// Custom button subclass procedure for dark theme
LRESULT CALLBACK DarkButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  ButtonState* state = nullptr;
  for (int i = 0; i < g_button_count; ++i) {
    if (g_button_states[i].hwnd == hwnd) {
      state = &g_button_states[i];
      break;
    }
  }
  if (!state) return DefWindowProc(hwnd, msg, wParam, lParam);

  switch (msg) {
    case WM_MOUSEMOVE:
      if (!state->hovered) {
        state->hovered = true;
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;

    case WM_MOUSELEAVE:
      state->hovered = false;
      state->pressed = false;
      InvalidateRect(hwnd, nullptr, FALSE);
      break;

    case WM_LBUTTONDOWN:
      state->pressed = true;
      InvalidateRect(hwnd, nullptr, FALSE);
      break;

    case WM_LBUTTONUP:
      state->pressed = false;
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
  }

  return CallWindowProc(state->original_proc, hwnd, msg, wParam, lParam);
}

// Subclass a button for dark theme
void SubclassButton(HWND hwnd, bool is_accent = false) {
  if (g_button_count >= kMaxButtons) return;

  ButtonState& state = g_button_states[g_button_count++];
  state.hwnd = hwnd;
  state.hovered = false;
  state.pressed = false;
  state.is_accent = is_accent;
  state.original_proc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DarkButtonProc)));
}

// Get button state for a window
ButtonState* GetButtonState(HWND hwnd) {
  for (int i = 0; i < g_button_count; ++i) {
    if (g_button_states[i].hwnd == hwnd) {
      return &g_button_states[i];
    }
  }
  return nullptr;
}

// Draw a custom dark theme button
void DrawDarkButton(HDC hdc, const RECT& rect, const char* text, bool hovered, bool pressed, bool focused, bool is_accent = false) {
  // Choose background color based on state
  HBRUSH bg_brush;
  COLORREF border_color;

  if (is_accent) {
    // Accent button uses blue background
    if (pressed) {
      bg_brush = g_accent_button_pressed_brush;
      border_color = DarkColors::AccentPressed;
    } else if (hovered) {
      bg_brush = g_accent_button_hover_brush;
      border_color = DarkColors::AccentHover;
    } else {
      bg_brush = g_accent_button_brush;
      border_color = DarkColors::Accent;
    }
  } else {
    // Normal button
    if (pressed) {
      bg_brush = g_button_pressed_brush;
      border_color = DarkColors::Accent;
    } else if (hovered) {
      bg_brush = g_button_hover_brush;
      border_color = DarkColors::Border;
    } else {
      bg_brush = g_button_brush;
      border_color = DarkColors::Border;
    }
  }

  // Draw rounded rectangle background
  DrawRoundedRect(hdc, rect, 6, bg_brush, border_color);

  // Draw focus indicator
  if (focused && !pressed && !hovered) {
    RECT focus_rect = rect;
    InflateRect(&focus_rect, -2, -2);
    DrawRoundedRect(hdc, focus_rect, 4, nullptr, is_accent ? DarkColors::Text : DarkColors::Accent);
  }

  // Draw text
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, DarkColors::Text);
  SelectObject(hdc, g_font);
  DrawTextA(hdc, text, -1, const_cast<RECT*>(&rect),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Draw a custom dark theme group box
void DrawDarkGroupBox(HDC hdc, const RECT& rect, const char* text) {
  // Measure text
  SIZE text_size;
  SelectObject(hdc, g_font);
  GetTextExtentPoint32A(hdc, text, static_cast<int>(strlen(text)), &text_size);

  int text_offset = 10;
  int text_padding = 4;

  // Draw the border (with gap for text)
  HPEN border_pen = CreatePen(PS_SOLID, 1, DarkColors::GroupBoxBorder);
  HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, border_pen));
  HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));

  // Calculate text area to skip
  int text_start = rect.left + text_offset;
  int text_end = text_start + text_size.cx + text_padding * 2;
  int top_line_y = rect.top + text_size.cy / 2;

  // Draw left portion of top border
  MoveToEx(hdc, rect.left + 4, top_line_y, nullptr);
  LineTo(hdc, text_start, top_line_y);

  // Draw right portion of top border
  MoveToEx(hdc, text_end, top_line_y, nullptr);
  LineTo(hdc, rect.right - 4, top_line_y);

  // Draw right border (rounded corner)
  LineTo(hdc, rect.right - 1, top_line_y + 4);
  LineTo(hdc, rect.right - 1, rect.bottom - 4);

  // Draw bottom border (rounded corner)
  LineTo(hdc, rect.right - 4, rect.bottom - 1);
  LineTo(hdc, rect.left + 4, rect.bottom - 1);

  // Draw left border (rounded corner)
  LineTo(hdc, rect.left, rect.bottom - 4);
  LineTo(hdc, rect.left, top_line_y + 4);
  LineTo(hdc, rect.left + 4, top_line_y);

  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(border_pen);

  // Draw text
  RECT text_rect = {text_start + text_padding, rect.top,
                    text_end - text_padding, rect.top + text_size.cy};
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, DarkColors::TextSecondary);
  DrawTextA(hdc, text, -1, &text_rect, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

// Draw a custom dark theme progress bar
void DrawDarkProgressBar(HDC hdc, const RECT& rect, int percent) {
  // Draw background
  HBRUSH bg_brush = CreateSolidBrush(DarkColors::ProgressBarBg);
  HPEN border_pen = CreatePen(PS_SOLID, 1, DarkColors::Border);

  HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, border_pen));
  HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, bg_brush));

  RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 4, 4);

  // Draw progress fill
  if (percent > 0) {
    int fill_width = (rect.right - rect.left - 2) * percent / 100;
    RECT fill_rect = {rect.left + 1, rect.top + 1,
                      rect.left + 1 + fill_width, rect.bottom - 1};

    HBRUSH fill_brush = CreateSolidBrush(DarkColors::ProgressBar);
    SelectObject(hdc, fill_brush);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, fill_rect.left, fill_rect.top, fill_rect.right, fill_rect.bottom, 3, 3);
    DeleteObject(fill_brush);
  }

  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(border_pen);
  DeleteObject(bg_brush);
}

// Create a label with optional bold font
HWND CreateLabel(HWND parent,
                 const char* text,
                 int x,
                 int y,
                 int w,
                 int h,
                 int id,
                 bool bold = false) {
  HWND hwnd = CreateWindowExA(
      0, "STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandle(nullptr), nullptr);
  SendMessage(hwnd, WM_SETFONT,
              reinterpret_cast<WPARAM>(bold ? g_bold_font : g_font), TRUE);
  return hwnd;
}

// Create a selectable (read-only edit) label for copy/paste support
HWND CreateSelectableLabel(HWND parent,
                           const char* text,
                           int x,
                           int y,
                           int w,
                           int h,
                           int id) {
  HWND hwnd = CreateWindowExA(
      0, "EDIT", text,
      WS_CHILD | WS_VISIBLE | ES_LEFT | ES_READONLY | ES_AUTOHSCROLL,
      x, y, w, h, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandle(nullptr), nullptr);
  SendMessage(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return hwnd;
}


// Update the UI with current metrics
void UpdateUI() {
  if (!g_hwnd)
    return;

  // Update VRAM usage
  size_t vram_bytes = 0;
  if (g_gpu_metrics) {
    vram_bytes = g_gpu_metrics->GetDedicatedUsage();
  }
  double vram_mb = vram_bytes / (1024.0 * 1024.0);

  char buf[256];
  snprintf(buf, sizeof(buf), "%.0f MB / %u MB", vram_mb, g_total_vram_mb);
  SetWindowTextA(g_vram_usage_label, buf);

  // Update progress bar (we store the value and draw it ourselves in WM_PAINT)
  if (g_total_vram_mb > 0) {
    int percent = static_cast<int>((vram_mb / g_total_vram_mb) * 100);
    int old_percent = static_cast<int>(SendMessage(g_vram_progress, PBM_GETPOS, 0, 0));
    if (percent != old_percent) {
      SendMessage(g_vram_progress, PBM_SETPOS, percent, 0);
      // Invalidate the progress bar area to redraw with new value
      int margin = 15;
      int row_height = 25;
      int prog_y = margin + 110 + 25 + row_height;
      RECT prog_rect = {margin + 10, prog_y, kWindowWidth - margin - 26, prog_y + 18};
      InvalidateRect(g_hwnd, &prog_rect, FALSE);
    }
  }

  // Update GPU Allocated MB
  snprintf(buf, sizeof(buf), "%.2f MB", g_metrics.gpu_allocated_mb.load());
  SetWindowTextA(g_gpu_allocated_label, buf);

  // Update GPU Freed MB
  snprintf(buf, sizeof(buf), "%.2f MB", g_metrics.gpu_freed_mb.load());
  SetWindowTextA(g_gpu_freed_label, buf);

  // Update status
  SetWindowTextA(g_status_label, g_metrics.status);
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE: {
      // Create dark mode brushes
      CreateDarkBrushes();

      // Create fonts
      g_font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
      g_bold_font = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

      int margin = 15;
      int row_height = 25;
      int label_width = 130;
      int value_width = 300;

      // === GPU Information Group ===
      // Group boxes are drawn custom in WM_PAINT for dark theme

      int y = margin + 25;
      CreateLabel(hwnd, "GPU:", margin + 10, y, label_width, row_height, 0,
                  true);
      g_gpu_name_label = CreateSelectableLabel(hwnd, "", margin + label_width, y,
                                               value_width, row_height, ID_GPU_NAME);

      y += row_height;
      CreateLabel(hwnd, "Driver Version:", margin + 10, y, label_width,
                  row_height, 0, true);
      g_driver_version_label =
          CreateSelectableLabel(hwnd, "", margin + label_width, y, value_width,
                                row_height, ID_DRIVER_VERSION);

      // === VRAM Usage Group ===
      y = margin + 110 + 25;
      CreateLabel(hwnd, "Usage:", margin + 10, y, label_width, row_height, 0,
                  true);
      g_vram_usage_label =
          CreateLabel(hwnd, "0 MB / 0 MB", margin + label_width, y, value_width,
                      row_height, ID_VRAM_USAGE);

      y += row_height;
      // Create a hidden progress bar to store the value (we draw our own)
      g_vram_progress = CreateWindowExA(
          0, PROGRESS_CLASSA, nullptr, WS_CHILD,  // Not visible
          margin + 10, y, kWindowWidth - margin * 2 - 36, 18, hwnd,
          reinterpret_cast<HMENU>(ID_VRAM_PROGRESS), GetModuleHandle(nullptr),
          nullptr);
      SendMessage(g_vram_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

      // === Gpu Metrics Group ===
      y = margin + 200 + 25;
      CreateLabel(hwnd, "Allocated:", margin + 10, y, label_width, row_height,
                  0, true);
      g_gpu_allocated_label =
          CreateLabel(hwnd, "0.00 MB", margin + label_width, y, value_width,
                      row_height, ID_GPU_ALLOCATED);

      y += row_height;
      CreateLabel(hwnd, "Freed:", margin + 10, y, label_width, row_height, 0,
                  true);
      g_gpu_freed_label = CreateLabel(hwnd, "0.00 MB", margin + label_width, y,
                                      value_width, row_height, ID_GPU_FREED);

      // === Controls ===
      y = margin + 290;
      CreateLabel(hwnd, "Status:", margin, y, 60, row_height, 0, true);
      g_status_label =
          CreateLabel(hwnd, "Idle", margin + 60, y, 200, row_height, ID_STATUS);

      // Create owner-drawn buttons for dark theme
      g_start_stop_btn = CreateWindowExA(
          0, "BUTTON", "Start Test",
          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
          kWindowWidth - margin - 116, y - 5, 100, 30, hwnd,
          reinterpret_cast<HMENU>(ID_START_STOP_BTN), GetModuleHandle(nullptr),
          nullptr);
      SubclassButton(g_start_stop_btn);

      // Flush buttons row
      y += 35;
      g_flush_render_btn = CreateWindowExA(
          0, "BUTTON", "Flush Render Device",
          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
          margin, y, 150, 28, hwnd,
          reinterpret_cast<HMENU>(ID_FLUSH_RENDER_BTN),
          GetModuleHandle(nullptr), nullptr);
      SubclassButton(g_flush_render_btn);

      g_flush_factory_btn = CreateWindowExA(
          0, "BUTTON", "Flush Factory Device",
          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
          margin + 160, y, 160, 28, hwnd,
          reinterpret_cast<HMENU>(ID_FLUSH_FACTORY_BTN),
          GetModuleHandle(nullptr), nullptr);
      SubclassButton(g_flush_factory_btn, true);  // Accent style - this is the key action

      // Set initial GPU info
      char gpu_name_ansi[256];
      WideCharToMultiByte(CP_UTF8, 0, g_gpu_name.c_str(), -1, gpu_name_ansi,
                          sizeof(gpu_name_ansi), nullptr, nullptr);
      SetWindowTextA(g_gpu_name_label, gpu_name_ansi);
      SetWindowTextA(g_driver_version_label, g_driver_version.c_str());

      // Start the timer for periodic updates
      SetTimer(hwnd, kTimerUpdateId, kTimerUpdateIntervalMs, nullptr);
      return 0;
    }

    case WM_ERASEBKGND: {
      // Paint dark background
      HDC hdc = reinterpret_cast<HDC>(wParam);
      RECT rect;
      GetClientRect(hwnd, &rect);
      FillRect(hdc, &rect, g_bg_brush);
      return 1;
    }

    case WM_CTLCOLORSTATIC: {
      // Set colors for static controls (labels) and group boxes
      HDC hdc = reinterpret_cast<HDC>(wParam);
      HWND ctrl_hwnd = reinterpret_cast<HWND>(lParam);

      SetBkMode(hdc, TRANSPARENT);

      // Check if this is the GPU name or driver version (value labels)
      if (ctrl_hwnd == g_gpu_name_label || ctrl_hwnd == g_driver_version_label ||
          ctrl_hwnd == g_vram_usage_label || ctrl_hwnd == g_gpu_allocated_label ||
          ctrl_hwnd == g_gpu_freed_label || ctrl_hwnd == g_status_label) {
        SetTextColor(hdc, DarkColors::Text);
      } else {
        // Bold labels (titles) use secondary color for subtle contrast
        SetTextColor(hdc, DarkColors::TextSecondary);
      }

      return reinterpret_cast<LRESULT>(g_bg_brush);
    }

    case WM_CTLCOLOREDIT: {
      // Set colors for read-only edit controls (selectable labels)
      HDC hdc = reinterpret_cast<HDC>(wParam);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, DarkColors::Text);
      SetBkColor(hdc, DarkColors::Background);
      return reinterpret_cast<LRESULT>(g_bg_brush);
    }

    case WM_DRAWITEM: {
      DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (dis->CtlType == ODT_BUTTON) {
        // Get button text
        char text[64];
        GetWindowTextA(dis->hwndItem, text, sizeof(text));

        // Get button state
        ButtonState* state = GetButtonState(dis->hwndItem);
        bool hovered = state ? state->hovered : false;
        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        bool focused = (dis->itemState & ODS_FOCUS) != 0;
        bool is_accent = state ? state->is_accent : false;

        // Fill background first
        FillRect(dis->hDC, &dis->rcItem, g_bg_brush);

        // Draw the button
        DrawDarkButton(dis->hDC, dis->rcItem, text, hovered, pressed, focused, is_accent);
        return TRUE;
      }
      break;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);

      // Draw group boxes with dark theme
      int margin = 15;
      int row_height = 25;

      // GPU Information group box
      RECT gpu_group = {margin, margin, kWindowWidth - margin - 16, margin + 100};
      DrawDarkGroupBox(hdc, gpu_group, "GPU Information");

      // VRAM Usage group box
      RECT vram_group = {margin, margin + 110, kWindowWidth - margin - 16, margin + 190};
      DrawDarkGroupBox(hdc, vram_group, "VRAM Usage");

      // GPU Metrics group box
      RECT metrics_group = {margin, margin + 200, kWindowWidth - margin - 16, margin + 280};
      DrawDarkGroupBox(hdc, metrics_group, "GPU Metrics");

      // How to Reproduce group box
      RECT help_group = {margin, margin + 370, kWindowWidth - margin - 16, margin + 500};
      DrawDarkGroupBox(hdc, help_group, "How to Reproduce");

      // Draw help text
      SelectObject(hdc, g_font);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, DarkColors::TextSecondary);

      const char* help_lines[] = {
          "1. Press Start Test - observe VRAM usage grows unbounded",
          "2. Press Stop Test - VRAM remains allocated (leaked)",
          "3. Press Flush Render Device - no effect on VRAM",
          "4. Press Flush Factory Device - leaked VRAM is freed!"
      };

      int help_y = margin + 370 + 20;
      for (const char* line : help_lines) {
        RECT line_rect = {margin + 10, help_y, kWindowWidth - margin - 26, help_y + 20};
        DrawTextA(hdc, line, -1, &line_rect, DT_LEFT | DT_SINGLELINE);
        help_y += 22;
      }

      // Draw custom progress bar
      int prog_y = margin + 110 + 25 + row_height;  // Same position as in WM_CREATE
      RECT prog_rect = {margin + 10, prog_y, kWindowWidth - margin - 26, prog_y + 18};
      int percent = static_cast<int>(SendMessage(g_vram_progress, PBM_GETPOS, 0, 0));
      DrawDarkProgressBar(hdc, prog_rect, percent);

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_TIMER:
      if (wParam == kTimerUpdateId) {
        UpdateUI();
      }
      return 0;

    case WM_COMMAND:
      if (LOWORD(wParam) == ID_START_STOP_BTN) {
        if (g_running) {
          // Stop the test
          g_running = false;
          SetWindowTextA(g_start_stop_btn, "Start Test");
          snprintf(g_metrics.status, sizeof(g_metrics.status), "Stopping...");
        } else {
          // Start the test
          g_running = true;
          SetWindowTextA(g_start_stop_btn, "Stop Test");
          std::thread(TextureTestThread).detach();
        }
      } else if (LOWORD(wParam) == ID_FLUSH_RENDER_BTN) {
        if (g_render_device) {
          g_render_device->Flush();
          snprintf(g_metrics.status, sizeof(g_metrics.status),
                   "Flushed Render (no effect expected)");
        }
      } else if (LOWORD(wParam) == ID_FLUSH_FACTORY_BTN) {
        if (g_factory) {
          g_factory->Flush();
          snprintf(g_metrics.status, sizeof(g_metrics.status),
                   "Flushed Factory - leaked VRAM freed!");
        }
      }
      return 0;

    case WM_DESTROY:
      g_running = false;
      // Wait for thread to finish
      while (g_thread_active) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      KillTimer(hwnd, kTimerUpdateId);
      if (g_font)
        DeleteObject(g_font);
      if (g_bold_font)
        DeleteObject(g_bold_font);
      DestroyDarkBrushes();
      PostQuitMessage(0);
      return 0;

    default:
      return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
  // Initialize dark mode before creating any windows
  InitDarkMode();

  // Initialize common controls
  INITCOMMONCONTROLSEX icc;
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&icc);

  // Get the high performance adapter
  g_adapter = GetHighPerformanceAdapter();
  DXGI_ADAPTER_DESC1 adapter_desc;
  g_adapter->GetDesc1(&adapter_desc);
  g_gpu_name = adapter_desc.Description;

  // Create GPU metrics
  static GpuMetrics gpu_metrics(adapter_desc.AdapterLuid);
  g_gpu_metrics = &gpu_metrics;
  if (const char* driver_version = gpu_metrics.GetDriverVersion()) {
    g_driver_version = driver_version;
  } else {
    g_driver_version = "Unknown";
  }
  g_total_vram_mb = gpu_metrics.GetTotalVRAM();

  // Create the factory
  static GpuMemoryBufferFactoryDXGI factory;
  g_factory = &factory;

  // Create render device
  static RenderDevice render_device;
  if (!render_device.Initialize(g_adapter.Get())) {
    MessageBoxA(nullptr, "Failed to initialize render device", "Error",
                MB_ICONERROR);
    return 1;
  }
  g_render_device = &render_device;

  // Register window class
  WNDCLASSEXA wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // We handle background painting in WM_ERASEBKGND
  wc.lpszClassName = "AMDTextureReproClass";
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

  if (!RegisterClassExA(&wc)) {
    MessageBoxA(nullptr, "Failed to register window class", "Error",
                MB_ICONERROR);
    return 1;
  }

  // Calculate window size to account for borders
  RECT rect = {0, 0, kWindowWidth, kWidowHeight};
  AdjustWindowRect(
      &rect, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);

  // Create window
  g_hwnd = CreateWindowExA(
      0, "AMDTextureReproClass", "AMD VRAM Leak Reproducer",
      (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)), CW_USEDEFAULT,
      CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
      nullptr, hInstance, nullptr);

  if (!g_hwnd) {
    MessageBoxA(nullptr, "Failed to create window", "Error", MB_ICONERROR);
    return 1;
  }

  // Enable dark mode for the window title bar
  EnableDarkModeForWindow(g_hwnd);

  ShowWindow(g_hwnd, nCmdShow);
  UpdateWindow(g_hwnd);

  // Message loop
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  return static_cast<int>(msg.wParam);
}
