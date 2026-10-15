#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "PlaybackRules.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
constexpr UINT kMediaEvent = WM_APP + 1, kInitialize = WM_APP + 2;
constexpr UINT kTimelineMove = WM_APP + 3, kTimelineLeave = WM_APP + 4, kPreviewReady = WM_APP + 5;
constexpr UINT_PTR kTick = 1, kHoverTick = 2;
enum ControlId : int { kOpen = 100, kPlay, kBack, kForward, kMarkA, kMarkB,
    kClear, kLoop, kScreenshot, kMute, kRestart, kVolume, kTimeline, kSpeedFirst = 200 };
constexpr double kSpeedOrder[] = {8, 4, 2, 1.5, 1.25, 1, 0.75, 0.5};
constexpr wchar_t kTitle[] = L"AI Video Cutty · Windows 播放原型";
constexpr COLORREF kBackground = RGB(25, 25, 25);
constexpr COLORREF kControl = RGB(57, 57, 57);
constexpr COLORREF kBlue = RGB(10, 132, 255);
constexpr UINT kPreviewWidth = 160, kPreviewHeight = 90;

LRESULT CALLBACK timelineProcedure(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK volumeProcedure(HWND, UINT, WPARAM, LPARAM);

struct Failure : std::runtime_error {
    HRESULT code;
    explicit Failure(HRESULT value) : std::runtime_error("Windows API failure"), code(value) {}
};
void require(HRESULT hr) { if (FAILED(hr)) throw Failure(hr); }
std::wstring errorText(HRESULT hr) {
    wchar_t* system = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0,
                   reinterpret_cast<wchar_t*>(&system), 0, nullptr);
    std::wostringstream text;
    text << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    if (system) { text << L" · " << system; LocalFree(system); }
    return text.str();
}
std::wstring timeText(double time) {
    if (!std::isfinite(time) || time < 0) return L"--:--.---";
    const auto milliseconds = static_cast<long long>(std::min(time, 1.0e9) * 1000);
    std::wostringstream text;
    text << milliseconds / 60000 << L":" << std::setw(2) << std::setfill(L'0')
         << milliseconds / 1000 % 60 << L"." << std::setw(3) << milliseconds % 1000;
    return text.str();
}

struct TimelineFrame {
    UINT width = 0, height = 0;
    std::vector<BYTE> pixels;
};
struct PendingPreview {
    ULONG generation = 0;
    size_t index = 0;
    TimelineFrame frame;
};
struct PreviewState {
    std::mutex mutex;
    std::deque<PendingPreview> ready;
    std::vector<float> waveform;
    ULONG waveformGeneration = 0;
    std::atomic<ULONG> generation{0};
    std::atomic<HWND> window{nullptr};
};

void generatePreviews(std::shared_ptr<PreviewState> state, fs::path path, ULONG generation) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        ComPtr<IMFAttributes> attributes;
        require(MFCreateAttributes(&attributes, 1));
        require(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
        ComPtr<IMFByteStream> stream;
        require(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
            MF_FILEFLAGS_NONE, path.c_str(), &stream));
        ComPtr<IMFSourceReader> reader;
        require(MFCreateSourceReaderFromByteStream(stream.Get(), attributes.Get(), &reader));
        require(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        require(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE));
        ComPtr<IMFMediaType> output;
        require(MFCreateMediaType(&output));
        require(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        require(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
        require(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, kPreviewWidth, kPreviewHeight));
        require(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output.Get()));
        ComPtr<IMFMediaType> configured;
        require(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &configured));
        UINT32 width = 0, height = 0;
        require(MFGetAttributeSize(configured.Get(), MF_MT_FRAME_SIZE, &width, &height));
        if (!width || !height || width > 512 || height > 288) throw Failure(E_OUTOFMEMORY);
        PROPVARIANT length{};
        require(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length));
        const ULONGLONG duration = length.vt == VT_UI8 ? length.uhVal.QuadPart : 0;
        PropVariantClear(&length);
        if (!duration) throw Failure(MF_E_INVALIDREQUEST);
        for (size_t index = 0; index < 10 && state->generation.load() == generation; ++index) {
            PROPVARIANT position{}; position.vt = VT_I8;
            position.hVal.QuadPart = static_cast<LONGLONG>(duration / 10 * index);
            require(reader->SetCurrentPosition(GUID_NULL, position));
            ComPtr<IMFSample> sample;
            for (int attempt = 0; attempt < 30 && !sample; ++attempt) {
                DWORD streamIndex = 0, flags = 0; LONGLONG timestamp = 0;
                require(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                    &streamIndex, &flags, &timestamp, &sample));
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buffer;
            require(sample->ConvertToContiguousBuffer(&buffer));
            TimelineFrame frame; frame.width = width; frame.height = height;
            frame.pixels.resize(static_cast<size_t>(width) * height * 4);
            ComPtr<IMF2DBuffer> twoD;
            if (SUCCEEDED(buffer.As(&twoD))) {
                BYTE* top = nullptr; LONG stride = 0;
                require(twoD->Lock2D(&top, &stride));
                for (UINT y = 0; y < height; ++y)
                    memcpy(frame.pixels.data() + static_cast<size_t>(y) * width * 4,
                        top + static_cast<ptrdiff_t>(y) * stride, static_cast<size_t>(width) * 4);
                twoD->Unlock2D();
            } else {
                BYTE* bytes = nullptr; DWORD capacity = 0, lengthBytes = 0;
                require(buffer->Lock(&bytes, &capacity, &lengthBytes));
                if (lengthBytes >= frame.pixels.size())
                    memcpy(frame.pixels.data(), bytes, frame.pixels.size());
                else { buffer->Unlock(); continue; }
                buffer->Unlock();
            }
            if (state->generation.load() != generation) break;
            {
                std::lock_guard lock(state->mutex);
                if (state->generation.load() != generation) break;
                if (state->ready.size() == 10) state->ready.pop_front();
                state->ready.push_back(PendingPreview{generation, index, std::move(frame)});
            }
            if (const HWND window = state->window.load()) PostMessageW(window, kPreviewReady, 0, 0);
        }
        stream->Close();
    } catch (...) { /* Unsupported media keeps the ten bounded empty cells. */ }
    MFShutdown(); CoUninitialize();
}

void generateWaveform(std::shared_ptr<PreviewState> state, fs::path path, ULONG generation) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        ComPtr<IMFByteStream> stream;
        require(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
            MF_FILEFLAGS_NONE, path.c_str(), &stream));
        ComPtr<IMFSourceReader> reader;
        require(MFCreateSourceReaderFromByteStream(stream.Get(), nullptr, &reader));
        require(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        require(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE));
        ComPtr<IMFMediaType> output;
        require(MFCreateMediaType(&output));
        require(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        require(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
        require(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1));
        require(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 8000));
        require(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        require(output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2));
        require(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000));
        require(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, output.Get()));
        PROPVARIANT length{};
        require(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length));
        const ULONGLONG duration = length.vt == VT_UI8 ? length.uhVal.QuadPart : 0;
        PropVariantClear(&length);
        if (!duration) throw Failure(MF_E_INVALIDREQUEST);
        std::vector<float> bars(240, 0);
        int emptyReads = 0;
        while (state->generation.load() == generation) {
            DWORD streamIndex = 0, flags = 0; LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            require(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                &streamIndex, &flags, &timestamp, &sample));
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sample) { if (++emptyReads > 100) break; continue; }
            emptyReads = 0;
            ComPtr<IMFMediaBuffer> buffer;
            require(sample->ConvertToContiguousBuffer(&buffer));
            BYTE* bytes = nullptr; DWORD capacity = 0, lengthBytes = 0;
            require(buffer->Lock(&bytes, &capacity, &lengthBytes));
            int peak = 0;
            for (DWORD offset = 0; offset + 1 < lengthBytes; offset += 2) {
                const int value = static_cast<short>(bytes[offset] | (bytes[offset + 1] << 8));
                peak = std::max(peak, std::abs(value));
            }
            buffer->Unlock();
            LONGLONG sampleDuration = 0;
            sample->GetSampleDuration(&sampleDuration);
            const auto barAt = [duration](LONGLONG time) {
                return static_cast<size_t>(std::clamp(
                    static_cast<double>(std::max<LONGLONG>(time, 0)) * 240.0 / duration,
                    0.0, 239.0));
            };
            const auto first = barAt(timestamp);
            const auto last = barAt(timestamp + std::max<LONGLONG>(sampleDuration, 0));
            for (size_t index = first; index <= last; ++index)
                bars[index] = std::max(bars[index], peak / 32768.0f);
        }
        if (state->generation.load() == generation) {
            const float maximum = *std::max_element(bars.begin(), bars.end());
            if (maximum > 0) for (float& bar : bars) bar /= maximum;
            {
                std::lock_guard lock(state->mutex);
                if (state->generation.load() == generation) {
                    state->waveform = std::move(bars);
                    state->waveformGeneration = generation;
                }
            }
            if (const HWND window = state->window.load()) PostMessageW(window, kPreviewReady, 0, 0);
        }
        stream->Close();
    } catch (...) { /* Unsupported audio leaves the bounded empty timeline. */ }
    MFShutdown(); CoUninitialize();
}

// Media callbacks are on MF threads. Only the UI thread touches UI/playback state.
class Notify final : public IMFMediaEngineNotify {
    std::atomic<ULONG> refs_{1};
    std::atomic<HWND> window_;
    const ULONG generation_;
public:
    Notify(HWND window, ULONG generation) : window_(window), generation_(generation) {}
    void detach() { window_.store(nullptr); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG result = --refs_; if (!result) delete this; return result;
    }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR first, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
            SetEvent(reinterpret_cast<HANDLE>(first)); return S_OK;
        }
        if (const auto window = window_.load())
            PostMessageW(window, kMediaEvent, event, static_cast<LPARAM>(generation_));
        return S_OK;
    }
};

class Player {
    HWND video_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapchain_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<IMFByteStream> stream_;
    ComPtr<Notify> notify_;
    ComPtr<IMFMediaEngine> engine_;
    ComPtr<IMFMediaEngineEx> extended_;
    UINT token_ = 0;
    ULONG generation_ = 0;
    bool software_ = false;
public:
    ~Player() { close(); }
    IMFMediaEngine* engine() const { return engine_.Get(); }
    IMFMediaEngineEx* extended() const { return extended_.Get(); }
    ULONG generation() const { return generation_; }
    bool software() const { return software_; }

    void initialize(HWND video) {
        video_ = video;
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = video;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                           D3D_FEATURE_LEVEL_10_0};
        auto create = [&](D3D_DRIVER_TYPE type) {
            return D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, flags,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &desc, &swapchain_,
                &device_, nullptr, &context_);
        };
        HRESULT hr = create(D3D_DRIVER_TYPE_HARDWARE);
        if (FAILED(hr)) {
            device_.Reset(); context_.Reset(); swapchain_.Reset();
            require(create(D3D_DRIVER_TYPE_WARP)); software_ = true;
        }
        ComPtr<ID3D10Multithread> multithread;
        require(device_.As(&multithread)); multithread->SetMultithreadProtected(TRUE);
        require(MFCreateDXGIDeviceManager(&token_, &manager_));
        require(manager_->ResetDevice(device_.Get(), token_));
        render(true);
    }
    void close() {
        if (notify_) notify_->detach();
        if (engine_) engine_->Shutdown();
        extended_.Reset(); engine_.Reset(); notify_.Reset();
        if (stream_) stream_->Close();
        stream_.Reset();
    }
    void open(HWND owner, const fs::path& path) {
        close(); ++generation_;
        ComPtr<IMFAttributes> attributes;
        require(MFCreateAttributes(&attributes, 4));
        notify_.Attach(new Notify(owner, generation_));
        require(attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_.Get()));
        require(attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager_.Get()));
        require(attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                                     DXGI_FORMAT_B8G8R8A8_UNORM));
        ComPtr<IMFMediaEngineClassFactory> factory;
        require(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                                 CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        require(factory->CreateInstance(0, attributes.Get(), &engine_));
        require(engine_.As(&extended_));
        // Open a read-only local byte stream. No remote URL or server is used.
        require(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                            MF_FILEFLAGS_NONE, path.c_str(), &stream_));
        BSTR name = SysAllocString(path.c_str());
        if (!name) throw Failure(E_OUTOFMEMORY);
        const HRESULT hr = extended_->SetSourceFromByteStream(stream_.Get(), name);
        SysFreeString(name); require(hr);
        require(engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC));
        require(engine_->Load());
    }
    void resize() {
        if (!swapchain_) return;
        RECT client{}; GetClientRect(video_, &client);
        if (client.right < 1 || client.bottom < 1) return;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        require(swapchain_->ResizeBuffers(0, static_cast<UINT>(client.right),
                    static_cast<UINT>(client.bottom), DXGI_FORMAT_UNKNOWN, 0));
        render(true);
    }
    void render(bool force = false) {
        if (!swapchain_) return;
        RECT client{}; GetClientRect(video_, &client);
        if (client.right < 1 || client.bottom < 1) return;
        if (!force && engine_ && engine_->HasVideo()) {
            LONGLONG timestamp = 0;
            if (engine_->OnVideoStreamTick(&timestamp) != S_OK) return;
        } else if (!force) return;
        ComPtr<ID3D11Texture2D> surface;
        require(swapchain_->GetBuffer(0, IID_PPV_ARGS(&surface)));
        ComPtr<ID3D11RenderTargetView> target;
        require(device_->CreateRenderTargetView(surface.Get(), nullptr, &target));
        const float black[] = {0.025f, 0.025f, 0.025f, 1.0f};
        context_->ClearRenderTargetView(target.Get(), black);
        if (engine_ && engine_->HasVideo()) {
            MFARGB border{}; border.rgbAlpha = 255;
            // During loading, there may not be a transferable frame yet.
            engine_->TransferVideoFrame(surface.Get(), nullptr, &client, &border);
        }
        const HRESULT hr = swapchain_->Present(1, 0);
        if (hr == DXGI_STATUS_OCCLUDED) return;
        require(hr);
    }

    void screenshot(const fs::path& destination) {
        if (!engine_ || !engine_->HasVideo()) throw Failure(MF_E_INVALIDREQUEST);
        DWORD width = 0, height = 0;
        require(engine_->GetNativeVideoSize(&width, &height));
        const auto bytes = static_cast<unsigned long long>(width) * height * 4;
        if (!width || !height || width > 16384 || height > 16384 || bytes > 256ull * 1024 * 1024)
            throw Failure(E_OUTOFMEMORY);
        // Screenshots are always new files, including if the name appears mid-save.
        if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES)
            throw Failure(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> frame;
        require(device_->CreateTexture2D(&desc, nullptr, &frame));
        const RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        MFARGB border{}; border.rgbAlpha = 255;
        require(engine_->TransferVideoFrame(frame.Get(), nullptr, &bounds, &border));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(device_->CreateTexture2D(&desc, nullptr, &staging));
        context_->CopyResource(staging.Get(), frame.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bitmap;
        try {
            require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&wic)));
            require(wic->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                mapped.RowPitch, mapped.RowPitch * height, static_cast<BYTE*>(mapped.pData), &bitmap));
        } catch (...) { context_->Unmap(staging.Get(), 0); throw; }
        context_->Unmap(staging.Get(), 0);
        GUID id{}; require(CoCreateGuid(&id));
        wchar_t guid[40]{}; StringFromGUID2(id, guid, ARRAYSIZE(guid));
        const fs::path temporary = destination.parent_path() / (std::wstring(L".cutty-shot-") + guid + L".tmp");
        HANDLE placeholder = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (placeholder == INVALID_HANDLE_VALUE) throw Failure(HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(placeholder);
        try {
            // Close all WIC streams before the same-directory, no-replace publication.
            {
                ComPtr<IWICFormatConverter> converter;
                require(wic->CreateFormatConverter(&converter));
                require(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat24bppBGR,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
                ComPtr<IWICStream> output;
                require(wic->CreateStream(&output));
                require(output->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE));
                ComPtr<IWICBitmapEncoder> encoder;
                require(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
                require(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache));
                ComPtr<IWICBitmapFrameEncode> encoded;
                ComPtr<IPropertyBag2> options;
                require(encoder->CreateNewFrame(&encoded, &options));
                PROPBAG2 property{}; property.pstrName = const_cast<wchar_t*>(L"ImageQuality");
                VARIANT quality{}; quality.vt = VT_R4; quality.fltVal = 0.95f;
                require(options->Write(1, &property, &quality));
                require(encoded->Initialize(options.Get()));
                require(encoded->SetSize(width, height));
                WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
                require(encoded->SetPixelFormat(&format));
                require(encoded->WriteSource(converter.Get(), nullptr));
                require(encoded->Commit()); require(encoder->Commit());
            }
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                throw Failure(HRESULT_FROM_WIN32(GetLastError()));
        } catch (...) { DeleteFileW(temporary.c_str()); throw; }
    }
};

class Application {
    HWND window_ = nullptr, video_ = nullptr, information_ = nullptr, position_ = nullptr;
    HWND help_ = nullptr, timeline_ = nullptr, volume_ = nullptr;
    HWND speedButtons_[ARRAYSIZE(kSpeedOrder)]{};
    std::vector<HWND> buttons_;
    std::array<TimelineFrame, 10> previews_;
    std::vector<float> waveform_;
    std::shared_ptr<PreviewState> previewState_ = std::make_shared<PreviewState>();
    Player player_;
    fs::path source_;
    fs::path pendingSource_;
    cutty::Markers markers_;
    bool initialized_ = false, loaded_ = false, loop_ = true, framePending_ = false;
    bool loopSeeking_ = false, temporaryShuttle_ = false, hoverActive_ = false;
    bool oldMuted_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR savedStyle_ = 0;
    double chosenRate_ = 1.0, resumeRate_ = 1.0, jklRate_ = 0, hoverRate_ = 0;
    double hoverFraction_ = -1;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = CreateSolidBrush(kBackground);
    std::wstring message_ = L"打开本地视频或音频，或把文件拖入窗口。";
    ULONGLONG lastUiTick_ = 0;
    std::chrono::steady_clock::time_point opened_;
    bool firstFrameRecorded_ = false, autoplayPending_ = false, previewStarted_ = false;
    UINT timerPeriod_ = 0;
    ULONGLONG frameDeadline_ = 0;

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    void status(const std::wstring& message) { message_ = message; update(); }
    HWND control(const wchar_t* cls, const wchar_t* title, DWORD style, int id) {
        HWND result = CreateWindowExW(0, cls, title, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        if (!result) throw Failure(HRESULT_FROM_WIN32(GetLastError()));
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return result;
    }
    void createControls() {
        dpi_ = GetDpiForWindow(window_);
        font_ = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        information_ = control(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
        video_ = control(L"CuttyVideoSurface", L"", WS_CLIPSIBLINGS, 0);
        position_ = control(L"STATIC", L"", SS_CENTER, 0);
        help_ = control(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
        const struct { const wchar_t* title; int id; } controls[] = {
            {L"打开…", kOpen}, {L"静音", kMute}, {L"↶ 开头", kRestart},
            {L"截图", kScreenshot}, {L"循环", kLoop}, {L"清除 A-B", kClear}};
        for (const auto& item : controls) buttons_.push_back(control(L"BUTTON", item.title, BS_OWNERDRAW, item.id));
        const wchar_t* speedLabels[] = {L"8×", L"4×", L"2×", L"1.5×", L"1.25×", L"1×", L"0.75×", L"0.5×"};
        for (size_t index = 0; index < ARRAYSIZE(kSpeedOrder); ++index) {
            speedButtons_[index] = control(L"BUTTON", speedLabels[index], BS_OWNERDRAW,
                kSpeedFirst + static_cast<int>(index));
            buttons_.push_back(speedButtons_[index]);
        }
        volume_ = control(L"CuttyVolume", L"", 0, kVolume);
        timeline_ = control(L"CuttyTimeline", L"", 0, kTimeline);
        DragAcceptFiles(window_, TRUE);
        layout(); update();
    }
    void updateFont() {
        HFONT next = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        for (const auto child : {information_, position_, help_})
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        for (HWND child : buttons_) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
    }
    void layout() {
        if (!video_) return;
        RECT bounds{}; GetClientRect(window_, &bounds);
        const int margin = scale(14), width = std::max(1L, bounds.right - 2 * margin);
        const int rail = scale(136), gap = scale(12), timelineHeight = scale(76);
        const int timelineY = bounds.bottom - margin - scale(24) - timelineHeight;
        const int mediaY = scale(52), mediaBottom = timelineY - scale(29);
        const int mediaHeight = std::max(scale(40), mediaBottom - mediaY);
        const int videoX = margin + rail + gap;
        const int videoWidth = std::max(1, width - 2 * (rail + gap));
        MoveWindow(GetDlgItem(window_, kOpen), margin, scale(10), scale(96), scale(34), TRUE);
        MoveWindow(information_, margin + scale(110), scale(13), std::max(1, width - scale(110)), scale(28), TRUE);
        MoveWindow(video_, videoX, mediaY, videoWidth, mediaHeight, TRUE);
        MoveWindow(position_, margin, timelineY - scale(24), width, scale(21), TRUE);
        MoveWindow(timeline_, margin, timelineY, width, timelineHeight, TRUE);
        MoveWindow(help_, margin, bounds.bottom - margin - scale(18), width, scale(20), TRUE);

        const int volumeY = mediaY + std::max(0, (mediaHeight - scale(366)) / 2);
        const int volumeHeight = std::min(scale(270), std::max(scale(120), mediaHeight - scale(96)));
        MoveWindow(volume_, margin + scale(17), volumeY, scale(42), volumeHeight, TRUE);
        MoveWindow(GetDlgItem(window_, kMute), margin + scale(6), volumeY + volumeHeight + scale(8), scale(64), scale(40), TRUE);
        MoveWindow(GetDlgItem(window_, kRestart), margin, volumeY + volumeHeight + scale(56), scale(76), scale(40), TRUE);
        MoveWindow(GetDlgItem(window_, kScreenshot), margin + scale(83),
            mediaY + (mediaHeight - scale(46)) / 2, scale(46), scale(46), TRUE);

        const int rightX = bounds.right - margin - scale(104);
        const int buttonWidth = scale(96), spacing = scale(3);
        const int count = static_cast<int>(ARRAYSIZE(kSpeedOrder)) + 2;
        const int buttonHeight = std::min(scale(40), std::max(scale(24), (mediaHeight - spacing * (count - 1)) / count));
        int y = mediaY + std::max(0, (mediaHeight - count * (buttonHeight + spacing) + spacing) / 2);
        for (HWND button : speedButtons_) {
            MoveWindow(button, rightX, y, buttonWidth, buttonHeight, TRUE);
            y += buttonHeight + spacing;
        }
        MoveWindow(GetDlgItem(window_, kLoop), rightX, y, buttonWidth, buttonHeight, TRUE); y += buttonHeight + spacing;
        MoveWindow(GetDlgItem(window_, kClear), rightX, y, buttonWidth, buttonHeight, TRUE);
        if (initialized_) player_.resize();
    }
    void update() {
        if (!position_) return;
        auto* engine = player_.engine();
        const double duration = loaded_ && engine ? engine->GetDuration() : 0;
        const double current = loaded_ && engine ? engine->GetCurrentTime() : 0;
        std::wstring info = source_.empty() ? L"本地播放 · Windows 10 / 11 · 无需额外运行时" : source_.filename().wstring();
        if (loaded_ && engine) {
            DWORD width = 0, height = 0;
            if (engine->HasVideo() && SUCCEEDED(engine->GetNativeVideoSize(&width, &height)))
                info += L"  ·  " + std::to_wstring(width) + L" × " + std::to_wstring(height);
            info += L"  ·  " + timeText(duration);
            info += player_.software() ? L"  ·  软件绘图" : L"  ·  Direct3D";
        }
        SetWindowTextW(information_, info.c_str());
        std::wstring position = timeText(current) + L" / " + timeText(duration);
        SetWindowTextW(position_, position.c_str());
        SetWindowTextW(help_, message_.c_str());
        InvalidateRect(timeline_, nullptr, FALSE);
        const LONG_PTR volume = static_cast<LONG_PTR>(std::lround(engine ? engine->GetVolume() * 100 : 100));
        if (GetWindowLongPtrW(volume_, GWLP_USERDATA) != volume) {
            SetWindowLongPtrW(volume_, GWLP_USERDATA, volume);
            InvalidateRect(volume_, nullptr, FALSE);
        }
        for (HWND button : buttons_) {
            const int id = GetDlgCtrlID(button);
            bool enabled = id == kOpen || loaded_;
            if (id == kScreenshot)
                enabled = loaded_ && engine && engine->HasVideo() && !framePending_ && !engine->IsSeeking();
            EnableWindow(button, enabled);
            InvalidateRect(button, nullptr, FALSE);
        }
    }
    void updateTimer() {
        auto* engine = player_.engine();
        // Idle/paused windows have no continuous timer or rendering loop.
        const UINT wanted = loaded_ && engine && !IsIconic(window_) &&
            (!engine->IsPaused() || engine->IsSeeking() || framePending_)
            ? (engine->HasVideo() ? 16u : 100u) : 0u;
        if (wanted == timerPeriod_) return;
        KillTimer(window_, kTick); timerPeriod_ = 0;
        if (wanted && SetTimer(window_, kTick, wanted, nullptr)) timerPeriod_ = wanted;
    }
    void moveHover(int position) {
        if (!loaded_ || !cutty::validDuration(player_.engine()->GetDuration())) return;
        if (!hoverActive_) {
            auto* engine = player_.engine();
            hoverRate_ = engine->IsPaused() ? 0 : engine->GetPlaybackRate();
            oldMuted_ = engine->GetMuted() != FALSE;
            require(engine->Pause());
            require(engine->SetMuted(TRUE));
            hoverActive_ = true;
            updateTimer();
        }
        hoverFraction_ = std::clamp(position / 10000.0, 0.0, 1.0);
        KillTimer(window_, kHoverTick);
        SetTimer(window_, kHoverTick, 33, nullptr);
        InvalidateRect(timeline_, nullptr, FALSE);
    }
    void hoverTick() {
        KillTimer(window_, kHoverTick);
        if (!hoverActive_ || !loaded_) return;
        if (player_.engine()->IsSeeking()) { SetTimer(window_, kHoverTick, 33, nullptr); return; }
        seek(hoverFraction_ * player_.engine()->GetDuration());
        update();
    }
    void endHover() {
        KillTimer(window_, kHoverTick);
        if (!hoverActive_) return;
        hoverActive_ = false;
        hoverFraction_ = -1;
        if (loaded_) {
            require(player_.engine()->SetMuted(oldMuted_));
            if (hoverRate_ != 0) playAt(hoverRate_, false);
            else update();
        }
        InvalidateRect(timeline_, nullptr, FALSE);
    }
    void clearPreviews() {
        ++previewState_->generation;
        for (auto& frame : previews_) frame = {};
        waveform_.clear();
        {
            std::lock_guard lock(previewState_->mutex);
            previewState_->ready.clear();
            previewState_->waveform.clear();
            previewState_->waveformGeneration = 0;
        }
        if (timeline_ && IsWindow(timeline_)) InvalidateRect(timeline_, nullptr, FALSE);
    }
    void startPreviews() {
        if (!loaded_ || previewStarted_) return;
        previewStarted_ = true;
        const ULONG generation = previewState_->generation.load();
        const fs::path path = source_;
        auto state = previewState_;
        try {
            if (player_.engine()->HasVideo())
                std::thread([state, path, generation] { generatePreviews(state, path, generation); }).detach();
            else
                std::thread([state, path, generation] { generateWaveform(state, path, generation); }).detach();
        } catch (...) { previewStarted_ = false; }
    }
    void receivePreviews() {
        std::deque<PendingPreview> ready;
        {
            std::lock_guard lock(previewState_->mutex);
            ready.swap(previewState_->ready);
            if (previewState_->waveformGeneration == previewState_->generation.load() &&
                !previewState_->waveform.empty()) {
                waveform_ = std::move(previewState_->waveform);
                previewState_->waveformGeneration = 0;
            }
        }
        const ULONG generation = previewState_->generation.load();
        for (auto& item : ready)
            if (item.generation == generation && item.index < previews_.size())
                previews_[item.index] = std::move(item.frame);
        InvalidateRect(timeline_, nullptr, FALSE);
    }
    fs::path chooseFile(bool saving) {
        ComPtr<IFileDialog> dialog;
        require(CoCreateInstance(saving ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
            nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)));
        DWORD options = 0; require(dialog->GetOptions(&options));
        require(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
                                   (saving ? FOS_PATHMUSTEXIST : FOS_FILEMUSTEXIST)));
        if (saving) {
            ComPtr<IFileSaveDialog> save; require(dialog.As(&save));
            const COMDLG_FILTERSPEC filter[] = {{L"JPEG 图片", L"*.jpg"}};
            require(save->SetFileTypes(1, filter));
            require(save->SetDefaultExtension(L"jpg"));
            const auto name = source_.stem().wstring() + L"-frame.jpg";
            require(save->SetFileName(name.c_str()));
            require(save->SetTitle(L"保存当前帧（新文件，不覆盖已有文件）"));
        } else require(dialog->SetTitle(L"打开本地视频或音频"));
        const HRESULT hr = dialog->Show(window_);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
        require(hr);
        ComPtr<IShellItem> item; require(dialog->GetResult(&item));
        PWSTR name = nullptr; require(item->GetDisplayName(SIGDN_FILESYSPATH, &name));
        fs::path path(name); CoTaskMemFree(name); return path;
    }
    void open(const fs::path& input) {
        std::error_code ec;
        const auto path = fs::absolute(input, ec);
        if (ec || !fs::is_regular_file(path, ec) || ec) {
            status(L"请选择可读取的本地普通文件。"); return;
        }
        const auto native = path.native();
        if (native.rfind(L"\\\\", 0) == 0) { status(L"此原型只打开本机文件，请先复制到本地磁盘。"); return; }
        if (!initialized_) { pendingSource_ = path; return; }
        endHover();
        clearPreviews();
        KillTimer(window_, kTick); timerPeriod_ = 0;
        loaded_ = false; framePending_ = false; loopSeeking_ = false; previewStarted_ = false;
        temporaryShuttle_ = false; jklRate_ = 0; chosenRate_ = 1; markers_.clear();
        source_ = path; firstFrameRecorded_ = false; autoplayPending_ = true;
        opened_ = std::chrono::steady_clock::now();
        message_ = L"正在打开…"; update();
        try { player_.open(window_, path); }
        catch (...) { player_.close(); player_.render(true); throw; }
    }
    void playAt(double rate, bool remember = true) {
        if (!loaded_) return;
        if (!player_.extended()->IsPlaybackRateSupported(rate)) {
            status(L"当前素材不支持该播放速度或倒放。"); return;
        }
        require(player_.engine()->SetPlaybackRate(rate));
        if (remember && rate > 0) chosenRate_ = rate;
        require(player_.engine()->Play());
        message_ = L"播放中"; updateTimer(); update();
    }
    void seek(double time) {
        if (!loaded_) return;
        require(player_.extended()->SetCurrentTimeEx(
            cutty::boundedTime(time, player_.engine()->GetDuration()), MF_MEDIA_ENGINE_SEEK_MODE_NORMAL));
        updateTimer();
    }
    void togglePlay() {
        if (!loaded_) return;
        if (player_.engine()->IsPaused()) {
            if (player_.engine()->IsEnded()) seek(markers_.range(player_.engine()->GetDuration()).value_or(cutty::Range{0, 0}).start);
            playAt(chosenRate_);
        } else {
            require(player_.engine()->Pause()); message_ = L"已暂停"; updateTimer(); update();
        }
    }
    void step(bool forward) {
        if (!loaded_ || !player_.engine()->HasVideo() || framePending_ || player_.engine()->IsSeeking()) return;
        require(player_.engine()->Pause());
        require(player_.extended()->FrameStep(forward));
        framePending_ = true; frameDeadline_ = GetTickCount64() + 5000;
        message_ = L"正在逐帧…"; updateTimer(); update();
    }
    void screenshot() {
        if (!loaded_ || framePending_ || player_.engine()->IsSeeking()) return;
        endHover();
        const bool wasPlaying = !player_.engine()->IsPaused();
        const double previousRate = player_.engine()->GetPlaybackRate();
        require(player_.engine()->Pause());
        updateTimer(); update();
        fs::path destination;
        std::wstring feedback;
        try {
            destination = chooseFile(true);
            if (!destination.empty()) {
                auto extension = destination.extension().wstring();
                for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
                if (extension != L".jpg" && extension != L".jpeg") {
                    feedback = L"截图请使用 .jpg 或 .jpeg 文件名。";
                    destination.clear();
                } else player_.screenshot(destination);
            }
        } catch (...) {
            if (wasPlaying) {
                try { playAt(previousRate, false); } catch (...) {}
            }
            throw;
        }
        if (wasPlaying) playAt(previousRate, false);
        if (!feedback.empty()) status(feedback);
        else if (!destination.empty()) status(L"已保存：" + destination.filename().wstring());
    }
    void fullscreen() {
        if (!fullscreen_) {
            savedStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
            GetWindowPlacement(window_, &savedPlacement_);
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
            SetWindowLongPtrW(window_, GWL_STYLE, savedStyle_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
            SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        } else {
            SetWindowLongPtrW(window_, GWL_STYLE, savedStyle_);
            SetWindowPlacement(window_, &savedPlacement_);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        }
        fullscreen_ = !fullscreen_;
    }
    void command(int id) {
        if (id == kOpen) { const auto path = chooseFile(false); if (!path.empty()) open(path); return; }
        if (!loaded_) return;
        auto* engine = player_.engine();
        switch (id) {
        case kPlay: togglePlay(); break;
        case kBack: step(false); break;
        case kForward: step(true); break;
        case kMarkA: status(markers_.setA(engine->GetCurrentTime()) ? L"已设置 A" : L"A 必须早于 B"); break;
        case kMarkB: status(markers_.setB(engine->GetCurrentTime()) ? L"已设置 B" : L"B 必须晚于 A"); break;
        case kClear: markers_.clear(); status(L"已清除 A/B"); break;
        case kLoop: loop_ = !loop_; status(loop_ ? L"循环已开启" : L"循环已关闭"); break;
        case kMute: require(engine->SetMuted(!engine->GetMuted())); update(); break;
        case kRestart: seek(0); playAt(chosenRate_); break;
        case kScreenshot: screenshot(); break;
        default:
            if (id >= kSpeedFirst && id < kSpeedFirst + static_cast<int>(ARRAYSIZE(kSpeedOrder)))
                playAt(kSpeedOrder[id - kSpeedFirst]);
            break;
        }
    }
    void mediaEvent(DWORD event, ULONG generation) {
        if (generation != player_.generation() || !player_.engine()) return;
        auto* engine = player_.engine();
        switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            loaded_ = true; status(L"已加载，正在准备播放…");
            if (!engine->HasVideo()) startPreviews();
            break;
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
            if (loaded_ && autoplayPending_) { autoplayPending_ = false; playAt(chosenRate_); }
            break;
        case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
            if (!firstFrameRecorded_) {
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - opened_).count();
                status(L"首帧就绪 " + std::to_wstring(milliseconds) + L" ms（不含启动）");
                firstFrameRecorded_ = true;
                startPreviews();
            }
            player_.render(true); break;
        case MF_MEDIA_ENGINE_EVENT_FRAMESTEPCOMPLETED:
            framePending_ = false; message_ = L"已逐帧，保持暂停";
            player_.render(true); break;
        case MF_MEDIA_ENGINE_EVENT_SEEKED:
            loopSeeking_ = false; player_.render(true); break;
        case MF_MEDIA_ENGINE_EVENT_ENDED:
            if (loop_) {
                const auto span = markers_.range(engine->GetDuration()).value_or(cutty::Range{0, engine->GetDuration()});
                seek(engine->GetPlaybackRate() < 0 ? std::nextafter(span.end, span.start) : span.start);
                playAt(engine->GetPlaybackRate(), false);
            }
            break;
        case MF_MEDIA_ENGINE_EVENT_ERROR:
        case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR: {
            ComPtr<IMFMediaError> error;
            HRESULT code = E_FAIL;
            if (SUCCEEDED(engine->GetError(&error)) && error) code = error->GetExtendedErrorCode();
            loaded_ = false; framePending_ = false; clearPreviews();
            player_.close(); player_.render(true);
            status(L"此素材无法播放（系统编解码器或文件错误）：" + errorText(code)); break;
        }
        default: break;
        }
        updateTimer(); update();
    }
    void tick() {
        if (!loaded_) { updateTimer(); return; }
        if (framePending_ && GetTickCount64() >= frameDeadline_) {
            framePending_ = false;
            require(player_.engine()->Pause());
            status(L"逐帧未在 5 秒内完成，请重新打开该素材。"); updateTimer(); return;
        }
        auto* engine = player_.engine();
        if (!loopSeeking_ && !engine->IsSeeking() && !engine->IsPaused()) {
            const auto target = cutty::loopTarget(engine->GetCurrentTime(), engine->GetPlaybackRate(),
                                                 engine->GetDuration(), markers_, loop_);
            if (target) { loopSeeking_ = true; seek(*target); }
        }
        player_.render();
        if (GetTickCount64() - lastUiTick_ >= 100) { lastUiTick_ = GetTickCount64(); update(); }
        updateTimer();
    }

public:
    explicit Application(fs::path source) : pendingSource_(std::move(source)) {}
    ~Application() { player_.close(); if (font_) DeleteObject(font_); if (backgroundBrush_) DeleteObject(backgroundBrush_); }
    HBRUSH backgroundBrush() const { return backgroundBrush_; }
    void drawButton(const DRAWITEMSTRUCT& item) const {
        const int id = static_cast<int>(item.CtlID);
        bool active = id == kLoop && loop_;
        if (id == kMute && loaded_ && player_.engine()) active = player_.engine()->GetMuted() != FALSE;
        if (id >= kSpeedFirst && id < kSpeedFirst + static_cast<int>(ARRAYSIZE(kSpeedOrder))) {
            auto* engine = player_.engine();
            const double rate = loaded_ && engine && !engine->IsPaused() ? std::abs(engine->GetPlaybackRate()) : chosenRate_;
            active = std::abs(rate - kSpeedOrder[id - kSpeedFirst]) < 0.001;
        }
        FillRect(item.hDC, &item.rcItem, backgroundBrush_);
        const COLORREF fill = (item.itemState & ODS_DISABLED) ? RGB(39, 39, 39) :
            active ? kBlue : (item.itemState & ODS_SELECTED) ? RGB(77, 77, 77) : kControl;
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, fill);
        HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
        HGDIOBJ oldPen = SelectObject(item.hDC, pen);
        const RECT& r = item.rcItem;
        RoundRect(item.hDC, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, scale(8), scale(8));
        SelectObject(item.hDC, oldBrush); SelectObject(item.hDC, oldPen);
        DeleteObject(brush); DeleteObject(pen);
        wchar_t title[64]{}; GetWindowTextW(item.hwndItem, title, ARRAYSIZE(title));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, (item.itemState & ODS_DISABLED) ? RGB(125, 125, 125) : RGB(245, 245, 245));
        HGDIOBJ oldFont = SelectObject(item.hDC, font_);
        RECT textRect = item.rcItem;
        DrawTextW(item.hDC, title, -1, &textRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(item.hDC, oldFont);
        if (item.itemState & ODS_FOCUS) {
            RECT focus = item.rcItem; InflateRect(&focus, -scale(3), -scale(3)); DrawFocusRect(item.hDC, &focus);
        }
    }
    void paintTimeline(HDC dc, const RECT& bounds) const {
        HBRUSH base = CreateSolidBrush(RGB(31, 31, 31));
        FillRect(dc, &bounds, base); DeleteObject(base);
        const int width = bounds.right - bounds.left;
        if (!waveform_.empty()) {
            HBRUSH barBrush = CreateSolidBrush(RGB(26, 125, 209));
            const int height = bounds.bottom - bounds.top;
            for (size_t i = 0; i < waveform_.size(); ++i) {
                const int x0 = bounds.left + static_cast<int>(i * width / waveform_.size());
                const int x1 = bounds.left + static_cast<int>((i + 1) * width / waveform_.size());
                const int barHeight = std::max(scale(2), static_cast<int>(height * waveform_[i]));
                RECT bar{x0 + 1, bounds.top + (height - barHeight) / 2,
                    std::max(x0 + 2, x1 - 1), bounds.top + (height + barHeight) / 2};
                FillRect(dc, &bar, barBrush);
            }
            DeleteObject(barBrush);
        } else for (int i = 0; i < 10; ++i) {
            RECT cell{bounds.left + i * width / 10 + 1, bounds.top + 1,
                bounds.left + (i + 1) * width / 10 - 1, bounds.bottom - 1};
            const auto& frame = previews_[i];
            if (frame.width && frame.height && !frame.pixels.empty()) {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = static_cast<LONG>(frame.width);
                info.bmiHeader.biHeight = -static_cast<LONG>(frame.height);
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                const int cellWidth = cell.right - cell.left, cellHeight = cell.bottom - cell.top;
                int sourceX = 0, sourceY = 0;
                int sourceWidth = static_cast<int>(frame.width), sourceHeight = static_cast<int>(frame.height);
                if (static_cast<long long>(sourceWidth) * cellHeight > static_cast<long long>(sourceHeight) * cellWidth) {
                    sourceWidth = std::max(1, sourceHeight * cellWidth / std::max(1, cellHeight));
                    sourceX = (static_cast<int>(frame.width) - sourceWidth) / 2;
                } else {
                    sourceHeight = std::max(1, sourceWidth * cellHeight / std::max(1, cellWidth));
                    sourceY = (static_cast<int>(frame.height) - sourceHeight) / 2;
                }
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, cell.left, cell.top, cellWidth, cellHeight,
                    sourceX, sourceY, sourceWidth, sourceHeight,
                    frame.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            } else {
                HBRUSH fill = CreateSolidBrush(RGB(51, 51, 51));
                FillRect(dc, &cell, fill); DeleteObject(fill);
            }
            FrameRect(dc, &cell, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        auto* engine = player_.engine();
        const double duration = loaded_ && engine ? engine->GetDuration() : 0;
        if (!cutty::validDuration(duration)) return;
        if (markers_.a && markers_.b && *markers_.a < *markers_.b) {
            HBRUSH outside = CreateSolidBrush(RGB(35, 35, 35));
            RECT left{bounds.left, bounds.top, bounds.left + static_cast<int>(width * *markers_.a / duration), bounds.bottom};
            RECT right{bounds.left + static_cast<int>(width * *markers_.b / duration), bounds.top, bounds.right, bounds.bottom};
            FillRect(dc, &left, outside); FillRect(dc, &right, outside); DeleteObject(outside);
        }
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, font_);
        HPEN markerPen = CreatePen(PS_SOLID, std::max(1, scale(2)), RGB(255, 214, 10));
        HGDIOBJ oldPen = SelectObject(dc, markerPen);
        for (const auto& marker : {std::pair{markers_.a, L"A"}, std::pair{markers_.b, L"B"}}) {
            if (!marker.first) continue;
            const int x = bounds.left + static_cast<int>(width * *marker.first / duration);
            MoveToEx(dc, x, bounds.top, nullptr); LineTo(dc, x, bounds.bottom);
            RECT label{x + scale(3), bounds.top + scale(2), x + scale(20), bounds.top + scale(19)};
            SetTextColor(dc, RGB(255, 214, 10));
            DrawTextW(dc, marker.second, -1, &label, DT_SINGLELINE | DT_LEFT | DT_TOP);
        }
        SelectObject(dc, oldPen); DeleteObject(markerPen);
        const double fraction = hoverFraction_ >= 0 ? hoverFraction_ :
            cutty::boundedTime(engine->GetCurrentTime(), duration) / duration;
        HPEN playhead = CreatePen(PS_SOLID, std::max(1, scale(2)), RGB(240, 240, 240));
        oldPen = SelectObject(dc, playhead);
        const int x = bounds.left + static_cast<int>(width * fraction);
        MoveToEx(dc, x, bounds.top, nullptr); LineTo(dc, x, bounds.bottom);
        SelectObject(dc, oldPen); DeleteObject(playhead);
        SelectObject(dc, oldFont);
    }
    void clickVideo() { command(kPlay); }
    bool keyboard(const MSG& message) {
        if (message.message != WM_KEYDOWN && message.message != WM_KEYUP) return false;
        if (GetAncestor(message.hwnd, GA_ROOT) != window_) return false;
        // Preserve text input instead of stealing keys.
        wchar_t name[32]{}; GetClassNameW(GetFocus(), name, ARRAYSIZE(name));
        if (wcscmp(name, L"Edit") == 0) return false;
        const bool down = message.message == WM_KEYDOWN;
        const bool repeat = (message.lParam & (1ll << 30)) != 0;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const auto key = message.wParam;
        if (!down && temporaryShuttle_ && (key == VK_LEFT || key == VK_RIGHT)) {
            temporaryShuttle_ = false; if (loaded_) playAt(resumeRate_, false); return true;
        }
        if (alt) return false;
        if (ctrl) {
            if (key == 'O') { if (down && !repeat) command(kOpen); return true; }
            if (loaded_ && (key == VK_LEFT || key == VK_RIGHT)) {
                if (down) seek(player_.engine()->GetCurrentTime() +
                    (key == VK_LEFT ? -0.05 : 0.05) * player_.engine()->GetDuration());
                return true;
            }
            return false;
        }
        if (key == VK_F11) { if (down && !repeat) fullscreen(); return true; }
        if (key == VK_ESCAPE) {
            if (down && !repeat) { if (fullscreen_) fullscreen(); else DestroyWindow(window_); }
            return true;
        }
        if (!loaded_) return false;
        if (key == VK_LEFT || key == VK_RIGHT) {
            if (down && !repeat) {
                if (player_.engine()->IsPaused()) step(key == VK_RIGHT);
                else {
                    resumeRate_ = chosenRate_;
                    playAt(key == VK_RIGHT ? 1.5 : -1.5, false);
                    temporaryShuttle_ = true;
                }
            } else if (!down && temporaryShuttle_) {
                temporaryShuttle_ = false; playAt(resumeRate_, false);
            }
            return true;
        }
        if (!down) return false;
        if (key == VK_UP || key == VK_DOWN) {
            require(player_.engine()->SetVolume(std::clamp(player_.engine()->GetVolume() +
                (key == VK_UP ? 0.1 : -0.1), 0.0, 1.0))); update(); return true;
        }
        if (repeat) return key == VK_SPACE || key == 'I' || key == 'O' || key == 'P' || key == 'X' || key == 'M' || key == 'R' || key == 'J' || key == 'K' || key == 'L';
        switch (key) {
        case VK_SPACE: command(kPlay); return true;
        case 'I': command(kMarkA); return true;
        case 'O': command(kMarkB); return true;
        case 'X': command(kClear); return true;
        case 'P': command(kLoop); return true;
        case 'M': command(kMute); return true;
        case 'R': seek(0); playAt(chosenRate_); return true;
        case 'J': case 'L':
            jklRate_ = cutty::shuttleRate(jklRate_, key == 'L'); playAt(jklRate_); return true;
        case 'K': jklRate_ = 0; require(player_.engine()->Pause()); message_ = L"已暂停"; updateTimer(); update(); return true;
        default: return false;
        }
    }
    void failed(HRESULT code) {
        framePending_ = false; loopSeeking_ = false;
        if (player_.engine()) player_.engine()->Pause();
        status(L"操作未完成：" + errorText(code)); updateTimer();
    }
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            previewState_->window.store(window_);
            createControls(); PostMessageW(window_, kInitialize, 0, 0); return 0;
        case kInitialize:
            player_.initialize(video_); initialized_ = true;
            if (!pendingSource_.empty()) { const auto path = pendingSource_; pendingSource_.clear(); open(path); }
            return 0;
        case WM_SIZE: layout(); updateTimer(); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam); updateFont();
            const auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = POINT{scale(900), scale(680)}; return 0;
        }
        case WM_ERASEBKGND: {
            RECT bounds{}; GetClientRect(window_, &bounds);
            FillRect(reinterpret_cast<HDC>(wparam), &bounds, backgroundBrush_); return 1;
        }
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wparam), kBackground);
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(218, 218, 218));
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (item && item->CtlType == ODT_BUTTON) { drawButton(*item); return TRUE; }
            return FALSE;
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) { command(LOWORD(wparam)); SetFocus(window_); }
            return 0;
        case WM_VSCROLL:
            if (reinterpret_cast<HWND>(lparam) == volume_ && loaded_) {
                const double volume = std::clamp(HIWORD(wparam) / 100.0, 0.0, 1.0);
                require(player_.engine()->SetVolume(volume)); update();
            }
            return 0;
        case kTimelineMove: moveHover(static_cast<int>(wparam)); return 0;
        case kTimelineLeave: endHover(); return 0;
        case kPreviewReady: receivePreviews(); return 0;
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wparam);
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::vector<wchar_t> name(static_cast<size_t>(length) + 1);
            DragQueryFileW(drop, 0, name.data(), length + 1); DragFinish(drop);
            if (length) open(fs::path(name.data())); SetFocus(window_); return 0;
        }
        case kMediaEvent: mediaEvent(static_cast<DWORD>(wparam), static_cast<ULONG>(lparam)); return 0;
        case WM_TIMER:
            if (wparam == kTick) tick();
            else if (wparam == kHoverTick) hoverTick();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) endHover();
            if (LOWORD(wparam) == WA_INACTIVE && temporaryShuttle_) {
                temporaryShuttle_ = false; if (loaded_) playAt(resumeRate_, false);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kTick); KillTimer(window_, kHoverTick);
            previewState_->window.store(nullptr); clearPreviews();
            hoverActive_ = false; player_.close(); loaded_ = false;
            PostQuitMessage(0); return 0;
        default: return DefWindowProcW(window_, message, wparam, lparam);
        }
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try { return app->handle(message, wparam, lparam); }
        catch (const Failure& error) { app->failed(error.code); return message == WM_CREATE ? -1 : 0; }
        catch (...) { app->failed(E_FAIL); return message == WM_CREATE ? -1 : 0; }
    }
    void paintVideo() { if (initialized_) player_.render(true); }
};

LRESULT CALLBACK videoProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; BeginPaint(window, &paint);
        auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
        try { if (app) app->paintVideo(); } catch (const Failure& error) { if (app) app->failed(error.code); }
        catch (...) { if (app) app->failed(E_FAIL); }
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_LBUTTONDOWN) { SetFocus(GetParent(window)); SetCapture(window); return 0; }
    if (message == WM_LBUTTONUP) {
        if (GetCapture() == window) ReleaseCapture();
        RECT bounds{}; GetClientRect(window, &bounds);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (PtInRect(&bounds, point)) {
            auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
            try { if (app) app->clickVideo(); }
            catch (const Failure& error) { if (app) app->failed(error.code); }
            catch (...) { if (app) app->failed(E_FAIL); }
        }
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK timelineProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds);
        auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
        if (app) app->paintTimeline(dc, bounds);
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        if (message == WM_LBUTTONDOWN) { SetFocus(GetParent(window)); SetCapture(window); }
        RECT bounds{}; GetClientRect(window, &bounds);
        const int width = std::max(1L, bounds.right - bounds.left);
        const int position = std::clamp(MulDiv(GET_X_LPARAM(lparam), 10000, width), 0, 10000);
        SendMessageW(GetParent(window), kTimelineMove, static_cast<WPARAM>(position), 0);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        if (GetCapture() == window) ReleaseCapture();
        RECT bounds{}; GetClientRect(window, &bounds);
        if (!PtInRect(&bounds, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}))
            SendMessageW(GetParent(window), kTimelineLeave, 0, 0);
        return 0;
    }
    if (message == WM_MOUSELEAVE) { SendMessageW(GetParent(window), kTimelineLeave, 0, 0); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK volumeProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds);
        HBRUSH background = CreateSolidBrush(kBackground);
        FillRect(dc, &bounds, background); DeleteObject(background);
        RECT track{bounds.left + 5, bounds.top + 3, bounds.right - 5, bounds.bottom - 3};
        HBRUSH gray = CreateSolidBrush(RGB(46, 46, 46));
        HPEN outline = CreatePen(PS_SOLID, 1, RGB(46, 46, 46));
        HGDIOBJ oldBrush = SelectObject(dc, gray), oldPen = SelectObject(dc, outline);
        RoundRect(dc, track.left, track.top, track.right, track.bottom, 8, 8);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
        DeleteObject(gray); DeleteObject(outline);
        const int value = std::clamp(static_cast<int>(GetWindowLongPtrW(window, GWLP_USERDATA)), 0, 100);
        RECT filled{track.left, track.bottom - MulDiv(track.bottom - track.top, value, 100), track.right, track.bottom};
        HBRUSH blue = CreateSolidBrush(kBlue);
        FillRect(dc, &filled, blue); DeleteObject(blue);
        EndPaint(window, &paint); return 0;
    }
    auto change = [&](int y) {
        RECT bounds{}; GetClientRect(window, &bounds);
        const int value = std::clamp(MulDiv(bounds.bottom - y, 100, std::max(1L, bounds.bottom - bounds.top)), 0, 100);
        SetWindowLongPtrW(window, GWLP_USERDATA, value);
        InvalidateRect(window, nullptr, FALSE);
        SendMessageW(GetParent(window), WM_VSCROLL, MAKEWPARAM(TB_THUMBTRACK, value), reinterpret_cast<LPARAM>(window));
    };
    if (message == WM_LBUTTONDOWN) { SetFocus(GetParent(window)); SetCapture(window); change(GET_Y_LPARAM(lparam)); return 0; }
    if (message == WM_MOUSEMOVE && (wparam & MK_LBUTTON)) { change(GET_Y_LPARAM(lparam)); return 0; }
    if (message == WM_LBUTTONUP) { if (GetCapture() == window) ReleaseCapture(); return 0; }
    if (message == WM_MOUSEWHEEL) {
        const int value = std::clamp(static_cast<int>(GetWindowLongPtrW(window, GWLP_USERDATA)) +
            (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 5 : -5), 0, 100);
        SetWindowLongPtrW(window, GWLP_USERDATA, value);
        InvalidateRect(window, nullptr, FALSE);
        SendMessageW(GetParent(window), WM_VSCROLL, MAKEWPARAM(TB_THUMBTRACK, value), reinterpret_cast<LPARAM>(window));
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Windows 媒体组件无法初始化。Windows N 版本需要系统 Media Feature Pack。",
                    kTitle, MB_OK | MB_ICONERROR);
        CoUninitialize(); return 1;
    }
    int result = 0;
    {
        int count = 0; LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        fs::path source = arguments && count > 1 ? fs::path(arguments[1]) : fs::path{};
        if (arguments) LocalFree(arguments);
        Application app(std::move(source));
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = instance; cls.lpfnWndProc = Application::procedure;
        cls.lpszClassName = L"AIVideoCuttyWindows";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0));
        cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&cls);
        cls.lpszClassName = L"CuttyVideoSurface"; cls.lpfnWndProc = videoProcedure;
        cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&cls);
        cls.lpszClassName = L"CuttyTimeline"; cls.lpfnWndProc = timelineProcedure;
        RegisterClassExW(&cls);
        cls.lpszClassName = L"CuttyVolume"; cls.lpfnWndProc = volumeProcedure;
        RegisterClassExW(&cls);
        // CreateWindowExW takes physical pixels; the control layout uses DPI-scaled units.
        const UINT initialDpi = GetDpiForSystem();
        HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, L"AIVideoCuttyWindows", kTitle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            MulDiv(1100, initialDpi, 96), MulDiv(720, initialDpi, 96),
            nullptr, nullptr, instance, &app);
        if (!window) result = 1;
        else {
            // This window has no text fields. Keep playback shortcuts available under Chinese IMEs;
            // common file dialogs are separate windows and retain their normal text input.
            ImmAssociateContextEx(window, nullptr, IACE_CHILDREN);
            ShowWindow(window, show); UpdateWindow(window);
            MSG message{};
            while (true) {
                const BOOL state = GetMessageW(&message, nullptr, 0, 0);
                if (state <= 0) { result = state == -1 ? 1 : static_cast<int>(message.wParam); break; }
                try { if (app.keyboard(message)) continue; }
                catch (const Failure& error) { app.failed(error.code); continue; }
                catch (...) { app.failed(E_FAIL); continue; }
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
    }
    MFShutdown(); CoUninitialize(); return result;
}
