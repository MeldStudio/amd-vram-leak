#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// AMD ADLX SDK for GPU metrics
#include "SDK/ADLXHelper/Windows/Cpp/ADLXHelper.h"
#include "SDK/Include/IPerformanceMonitoring.h"

#include "fmt/color.h"
#include "fmt/format.h"
#include "fmt/xchar.h"

using Microsoft::WRL::ComPtr;
using namespace adlx;

namespace {
// Get the high performance adapter (first adapter by default)
ComPtr<IDXGIAdapter1> GetHighPerformanceAdapter() {
  ComPtr<IDXGIFactory1> dxgi_factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));
  if (FAILED(hr)) {
    printf("CreateDXGIFactory1 failed: 0x%08lx\n", hr);
    return nullptr;
  }

  ComPtr<IDXGIAdapter1> adapter;
  hr = dxgi_factory->EnumAdapters1(0, &adapter);
  if (FAILED(hr) || !adapter) {
    printf("EnumAdapters1 failed: 0x%08lx\n", hr);
    std::abort();
  }

  return adapter;
}

// Global configuration set via command line switches
struct Config {
  bool flush_after_handle_close = false;
  bool show_help = false;
};
static Config g_config;

static void PrintHelp(const char* program_name) {
  fmt::print(fmt::emphasis::bold, "Usage: {} [options]\n\n", program_name);
  fmt::print(fmt::emphasis::bold, "Options:\n");
  fmt::print("  {:30} {}\n", "-help", "Show this help message and exit");
  fmt::print("  {:30} {}\n", "-flush-after-handle-close",
             "Flush D3D11 device context after closing texture handles");
}

static void ParseCommandLine(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-flush-after-handle-close") == 0 ||
        strcmp(argv[i], "--flush-after-handle-close") == 0) {
      g_config.flush_after_handle_close = true;
    } else if (strcmp(argv[i], "-help") == 0 ||
               strcmp(argv[i], "--help") == 0 ||
               strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "/?") == 0) {
      g_config.show_help = true;
    }
  }
}

// Class to query GPU metrics using AMD ADLX SDK
// This provides direct access to AMD GPU internal metrics
class GpuMemoryMonitor {
 public:
  GpuMemoryMonitor() {
    // Initialize ADLX
    ADLX_RESULT res = g_ADLX.Initialize();
    if (!ADLX_SUCCEEDED(res)) {
      printf("Warning: ADLX initialization failed (0x%08X)\n", res);
      return;
    }
    initialized_ = true;

    // Get system services
    IADLXSystem* system = g_ADLX.GetSystemServices();
    if (!system) {
      printf("Warning: Failed to get ADLX system services\n");
      return;
    }

    // Get GPU list
    IADLXGPUListPtr gpus;
    res = system->GetGPUs(&gpus);
    if (!ADLX_SUCCEEDED(res) || !gpus || gpus->Size() == 0) {
      printf("Warning: No GPUs found via ADLX\n");
      return;
    }

    // Get the first GPU
    res = gpus->At(0, &gpu_);
    if (!ADLX_SUCCEEDED(res) || !gpu_) {
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

  ~GpuMemoryMonitor() {
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
    IADLXGPUMetricsPtr metrics;
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

 private:
  bool initialized_ = false;
  bool ready_ = false;
  adlx_uint total_vram_mb_ = 0;
  IADLXGPUPtr gpu_;
  IADLXPerformanceMonitoringServicesPtr perf_services_;
  IADLXGPUMetricsSupportPtr gpu_metrics_support_;
};

// Debug name GUID for D3D objects

// Ninja-style status line for beautiful CLI progress display
// Uses fmt text styling for colorful output
class StatusLine {
 public:
  StatusLine() {
    // Get console handle and enable VT processing (should be default on Win11)
    console_ = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(console_, &mode);
    SetConsoleMode(console_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // Get console width
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(console_, &csbi)) {
      console_width_ = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }

    // Hide cursor for smooth updates
    fmt::print("\x1b[?25l");
    std::fflush(stdout);

    start_time_ = std::chrono::steady_clock::now();
  }

  ~StatusLine() {
    ClearLine();
    // Show cursor again
    fmt::print("\x1b[?25h");
    std::fflush(stdout);
  }

  void SetAdapter(IDXGIAdapter* adapter) {
    (void)adapter;  // No longer needed, using ADLX
  }

  void SetGpuMemoryMonitor(GpuMemoryMonitor* monitor) {
    gpu_monitor_ = monitor;
  }

  void Update(int iteration,
              int texture_index,
              int total_textures,
              size_t total_created,
              std::string status) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_)
            .count();
    double elapsed_sec = elapsed / 1000.0;

    // Calculate rate
    double rate = (total_created > 0 && elapsed_sec > 0)
                      ? static_cast<double>(total_created) / elapsed_sec
                      : 0.0;

    // Format count string with fixed width
    std::string count_str = FormatCount(total_created);

    // Query VRAM usage via ADLX
    size_t vram_bytes = 0;
    if (gpu_monitor_) {
      vram_bytes = gpu_monitor_->GetDedicatedUsage();
    }
    double vram_mb = vram_bytes / (1024.0 * 1024.0);

    auto buffer = fmt::memory_buffer();
    auto out = std::back_inserter(buffer);

    // Build beautiful status line using fmt styling
    fmt::format_to(out, "\r\x1b[K");
    fmt::format_to(out, "[{:2d}/{:2d}] ", texture_index, total_textures);
    fmt::format_to(out, "iter {:5d} ", iteration);
    fmt::format_to(out, " │ ");
    fmt::format_to(out, fg(fmt::color::gold), "{:7.1f}/s", rate);
    fmt::format_to(out, fg(fmt::color::gray), " │ ");
    fmt::format_to(out, fg(fmt::color::white), "{:>8s}", count_str);
    fmt::format_to(out, fg(fmt::color::gray), " │ ");
    fmt::format_to(out, fg(fmt::color::orange), "VRAM {:5.0f}/{:}MB", vram_mb,
                   gpu_monitor_->GetTotalVRAM());
    fmt::format_to(out, fg(fmt::color::gray), " │ ");
    fmt::format_to(out, fg(fmt::color::medium_purple), "{:<20s}", status);
    fmt::print("{}", fmt::to_string(buffer).c_str());
    std::fflush(stdout);
  }

  void ClearLine() {
    fmt::print("\r\x1b[K");
    std::fflush(stdout);
  }

  void PrintLine(const char* msg) {
    ClearLine();
    fmt::print("{}\n", msg);
  }

  void PrintSuccess(const char* msg) {
    ClearLine();
    fmt::print(fg(fmt::color::green), "+");
    fmt::print(" {}\n", msg);
  }

  void PrintError(const char* msg) {
    ClearLine();
    fmt::print(fg(fmt::color::red), "×");
    fmt::print(" {}\n", msg);
  }

  void PrintHeader(const char* msg) {
    ClearLine();
    fmt::print(fmt::emphasis::bold | fg(fmt::color::dodger_blue), "{}\n", msg);
  }

 private:
  static std::string FormatCount(size_t count) {
    if (count >= 1000000) {
      return fmt::format("{:6.2f}M", count / 1000000.0);
    } else if (count >= 1000) {
      return fmt::format("{:6.1f}K", count / 1000.0);
    } else {
      return fmt::format("{:7d}", count);
    }
  }

  HANDLE console_;
  int console_width_ = 80;
  std::chrono::steady_clock::time_point start_time_;
  GpuMemoryMonitor* gpu_monitor_ = nullptr;
};

// Debug name GUID for D3D objects
static const GUID WKPDID_D3DDebugObjectName = {
    0x429b8c22,
    0x9188,
    0x4b0c,
    {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};

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

    ComPtr<IDXGIResource1> dxgi_resource;
    hr = d3d11_texture.As(&dxgi_resource);
    if (FAILED(hr)) {
      printf("Failed to get IDXGIResource1 interface: 0x%08lx\n", hr);
      return {};
    }

    HANDLE texture_handle = nullptr;
    hr = dxgi_resource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &texture_handle);
    if (FAILED(hr)) {
      printf("CreateSharedHandle failed with error 0x%08lx\n", hr);
      return {};
    }

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
      dxgi_adapter->GetDesc1(&adapter_desc);

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
        printf("D3D11CreateDevice failed: 0x%08lx\n", hr);
        return nullptr;
      }

      const char* kDebugName = "GpuMemoryBufferFactoryDXGI";
      d3d11_device_->SetPrivateData(WKPDID_D3DDebugObjectName,
                                    static_cast<UINT>(strlen(kDebugName)),
                                    kDebugName);
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
      printf("RenderDevice D3D11CreateDevice failed: 0x%08lx\n", hr);
      return false;
    }

    const char* kDebugName = "RenderDevice";
    device_->SetPrivateData(WKPDID_D3DDebugObjectName,
                            static_cast<UINT>(strlen(kDebugName)), kDebugName);

    // Get ID3D11Device5 for fence support
    hr = device_.As(&device5_);
    if (FAILED(hr)) {
      printf("Failed to get ID3D11Device5 (fence support): 0x%08lx\n", hr);
      return false;
    }

    // Get ID3D11DeviceContext4 for fence signaling
    hr = context_.As(&context4_);
    if (FAILED(hr)) {
      printf("Failed to get ID3D11DeviceContext4: 0x%08lx\n", hr);
      return false;
    }

    // Create fence for GPU synchronization
    hr = device5_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) {
      printf("CreateFence failed: 0x%08lx\n", hr);
      return false;
    }

    // Create event for CPU wait
    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
      printf("CreateEvent failed: %lu\n", GetLastError());
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
    HRESULT hr = device_.As(&device1);
    if (FAILED(hr)) {
      printf("Failed to get ID3D11Device1: 0x%08lx\n", hr);
      return false;
    }

    ComPtr<ID3D11Texture2D> texture;
    hr = device1->OpenSharedResource1(shared_handle, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
      printf("OpenSharedResource1 failed: 0x%08lx\n", hr);
      return false;
    }

    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device_->CreateRenderTargetView(texture.Get(), nullptr, &rtv);
    if (FAILED(hr)) {
      printf("CreateRenderTargetView failed: 0x%08lx\n", hr);
      return false;
    }

    float clear_color[4] = {r, g, b, a};
    context_->ClearRenderTargetView(rtv.Get(), clear_color);

    // Signal the fence after rendering
    ++fence_value_;
    hr = context4_->Signal(fence_.Get(), fence_value_);
    if (FAILED(hr)) {
      printf("Signal failed: 0x%08lx\n", hr);
      return false;
    }

    // CPU wait for GPU to complete
    if (fence_->GetCompletedValue() < fence_value_) {
      hr = fence_->SetEventOnCompletion(fence_value_, fence_event_);
      if (FAILED(hr)) {
        printf("SetEventOnCompletion failed: 0x%08lx\n", hr);
        return false;
      }
      WaitForSingleObject(fence_event_, INFINITE);
    }

    return true;
  }

  ID3D11Device* GetDevice() const { return device_.Get(); }
  ID3D11DeviceContext* GetContext() const { return context_.Get(); }

 private:
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Device5> device5_;
  ComPtr<ID3D11DeviceContext4> context4_;
  ComPtr<ID3D11Fence> fence_;
  HANDLE fence_event_ = nullptr;
  UINT64 fence_value_ = 0;
};

// Global flag for ctrl+c handling
static std::atomic<bool> g_running{true};
static StatusLine* g_status = nullptr;

void SignalHandler(int signal) {
  if (signal == SIGINT) {
    g_running = false;
    if (g_status) {
      // g_status->PrintLine("\n");
      fmt::print("\n\x1b[33mCtrl+C received, stopping...\x1b[0m");
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  ParseCommandLine(argc, argv);

  if (g_config.show_help) {
    PrintHelp(argv[0]);
    return 0;
  }

  // Get the high performance adapter
  auto adapter = GetHighPerformanceAdapter();
  DXGI_ADAPTER_DESC1 adapter_desc;
  adapter->GetDesc1(&adapter_desc);

  constexpr int kTextureWidth = 3840;
  constexpr int kTextureHeight = 2160;
  constexpr int kNumTextures = 10;
  fmt::println(L"Using adapter: {}", adapter_desc.Description);
  fmt::println("Creating {}x{} textures ({} per iteration)", kTextureWidth,
               kTextureHeight, kNumTextures);

  fmt::print(fmt::fg(fmt::color::yellow) | fmt::emphasis::bold,
             "\nRunning continually until Ctrl+C is pressed.\n");

  StatusLine status;
  g_status = &status;

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, SignalHandler);

  // Create GPU memory monitor (uses Windows Performance Counters like Task
  // Manager)
  GpuMemoryMonitor gpu_monitor;
  status.SetGpuMemoryMonitor(&gpu_monitor);

  // Create the factory that owns and creates textures
  // This device is ONLY used for texture creation, not rendering
  GpuMemoryBufferFactoryDXGI factory;

  // Create a separate device for rendering (backed by the same adapter)
  RenderDevice render_device;
  if (!render_device.Initialize(adapter.Get())) {
    status.PrintError("Failed to initialize render device");
    return 1;
  }

  // Define unique colors for each texture
  std::array<std::array<float, 4>, kNumTextures> colors = {{
      {1.0f, 0.0f, 0.0f, 1.0f},  // Red
      {0.0f, 1.0f, 0.0f, 1.0f},  // Green
      {0.0f, 0.0f, 1.0f, 1.0f},  // Blue
      {1.0f, 1.0f, 0.0f, 1.0f},  // Yellow
      {1.0f, 0.0f, 1.0f, 1.0f},  // Magenta
      {0.0f, 1.0f, 1.0f, 1.0f},  // Cyan
      {1.0f, 0.5f, 0.0f, 1.0f},  // Orange
      {0.5f, 0.0f, 1.0f, 1.0f},  // Purple
      {0.5f, 0.5f, 0.5f, 1.0f},  // Gray
      {1.0f, 1.0f, 1.0f, 1.0f},  // White
  }};

  int iteration = 0;
  size_t total_textures_created = 0;

  // Color names for status display
  const char* color_names[kNumTextures] = {
      "Red",  "Green",  "Blue",   "Yellow", "Magenta",
      "Cyan", "Orange", "Purple", "Gray",   "White"};

  status.PrintLine("");  // Empty line before progress

  while (g_running) {
    ++iteration;

    for (int i = 0; i < kNumTextures && g_running; ++i) {
      // Update status line: creating
      status.Update(iteration, i + 1, kNumTextures, total_textures_created,
                    fmt::format("Creating {} texture...", color_names[i]));

      // Factory creates the texture (using its private device)
      auto buffer =
          factory.CreateGpuMemoryBuffer(kTextureWidth, kTextureHeight);
      if (!buffer) {
        status.PrintError("Failed to create texture");
        continue;
      }

      // Update status line: rendering
      status.Update(iteration, i + 1, kNumTextures, total_textures_created,
                    fmt::format("Rendering {}...", color_names[i]));

      if (!render_device.RenderToSharedTexture(buffer.Get(), colors[i][0],
                                               colors[i][1], colors[i][2],
                                               colors[i][3])) {
        status.PrintError("Failed to render");
      }

      // Update status line: disposing
      status.Update(iteration, i + 1, kNumTextures, total_textures_created,
                    fmt::format("Disposing {}...", color_names[i]));

      // Buffer goes out of scope here, calling DestroyGpuMemoryBuffer
      // This happens after GPU has finished rendering (fence wait completed)
      ++total_textures_created;

      std::this_thread::sleep_for(std::chrono::milliseconds(33));  // 30 FPS
    }
  }

  // Final summary
  status.ClearLine();
  printf("\n");
  status.PrintHeader("=== Final Summary ===");

  char summary[128];
  snprintf(summary, sizeof(summary), "Total iterations: %d", iteration);
  status.PrintLine(summary);

  snprintf(summary, sizeof(summary), "Total textures created: %zu",
           total_textures_created);
  status.PrintLine(summary);

  snprintf(summary, sizeof(summary), "Render device: %p",
           render_device.GetDevice());
  status.PrintLine(summary);

  status.PrintSuccess("Done.");

  g_status = nullptr;
  return 0;
}
