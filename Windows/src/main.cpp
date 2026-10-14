#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "PlaybackRules.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
constexpr UINT kMediaEvent = WM_APP + 1, kInitialize = WM_APP + 2;
constexpr UINT_PTR kTick = 1;
enum ControlId : int { kOpen = 100, kPlay, kBack, kForward, kMarkA, kMarkB,
    kClear, kLoop, kScreenshot, kMute, kSpeed, kSeek };
constexpr wchar_t kTitle[] = L"AI Video Cutty · Windows 播放原型";

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
    HWND help_ = nullptr, seek_ = nullptr, speed_ = nullptr;
    std::vector<HWND> buttons_;
    Player player_;
    fs::path source_;
    fs::path pendingSource_;
    cutty::Markers markers_;
    bool initialized_ = false, loaded_ = false, loop_ = true, framePending_ = false;
    bool loopSeeking_ = false, dragging_ = false, temporaryShuttle_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR savedStyle_ = 0;
    double chosenRate_ = 1.0, resumeRate_ = 1.0, jklRate_ = 0;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    std::wstring message_ = L"打开本地视频或音频，或把文件拖入窗口。";
    ULONGLONG lastUiTick_ = 0;
    std::chrono::steady_clock::time_point opened_;
    bool firstFrameRecorded_ = false, autoplayPending_ = false;
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
        information_ = control(L"STATIC", L"", SS_LEFT, 0);
        video_ = control(L"CuttyVideoSurface", L"", WS_CLIPSIBLINGS, 0);
        position_ = control(L"STATIC", L"", SS_LEFT, 0);
        help_ = control(L"STATIC", L"Space 播放/暂停 · ←/→ 逐帧/穿梭 · I/O 标记 · J/K/L · Ctrl+←/→ 跳 5% · F11 全屏", SS_LEFT, 0);
        const struct { const wchar_t* title; int id; } controls[] = {
            {L"打开…", kOpen}, {L"播放", kPlay}, {L"← 一帧", kBack}, {L"一帧 →", kForward},
            {L"A (I)", kMarkA}, {L"B (O)", kMarkB}, {L"清除 (X)", kClear},
            {L"循环 (P)", kLoop}, {L"截图…", kScreenshot}, {L"静音 (M)", kMute}};
        for (const auto& item : controls) buttons_.push_back(control(L"BUTTON", item.title, BS_PUSHBUTTON, item.id));
        speed_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, kSpeed);
        for (const auto label : {L"0.5×", L"1×", L"1.5×", L"2×", L"4×", L"8×"})
            SendMessageW(speed_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(speed_, CB_SETCURSEL, 1, 0);
        seek_ = control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, kSeek);
        SendMessageW(seek_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10000));
        DragAcceptFiles(window_, TRUE);
        layout(); update();
    }
    void updateFont() {
        HFONT next = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        for (const auto child : {information_, position_, help_, speed_, seek_})
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        for (HWND child : buttons_) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
    }
    void layout() {
        if (!video_) return;
        RECT bounds{}; GetClientRect(window_, &bounds);
        const int margin = scale(12), width = std::max(1L, bounds.right - 2 * margin);
        const int buttonHeight = scale(30), row = scale(36), bottom = scale(154);
        MoveWindow(information_, margin, margin, width, scale(24), TRUE);
        MoveWindow(video_, margin, scale(42), width, std::max<LONG>(scale(40), bounds.bottom - scale(42) - bottom), TRUE);
        const int base = bounds.bottom - bottom + scale(6);
        MoveWindow(position_, margin, base, width, scale(24), TRUE);
        MoveWindow(seek_, margin, base + scale(26), width, scale(25), TRUE);
        int x = margin;
        const int sizes[] = {72, 68, 70, 70, 58, 58, 76, 80, 72, 76};
        for (size_t i = 0; i < buttons_.size(); ++i) {
            MoveWindow(buttons_[i], x, base + scale(55), scale(sizes[i]), buttonHeight, TRUE);
            x += scale(sizes[i] + 5);
        }
        MoveWindow(speed_, x, base + scale(55), scale(76), scale(220), TRUE);
        MoveWindow(help_, margin, base + scale(55) + row, width, scale(44), TRUE);
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
        position += L"    A: " + (markers_.a ? timeText(*markers_.a) : L"片头");
        position += L"    B: " + (markers_.b ? timeText(*markers_.b) : L"片尾");
        position += L"    " + message_;
        SetWindowTextW(position_, position.c_str());
        if (!dragging_ && cutty::validDuration(duration))
            SendMessageW(seek_, TBM_SETPOS, TRUE, static_cast<LPARAM>(10000 * cutty::boundedTime(current, duration) / duration));
        for (HWND button : buttons_) {
            const int id = GetDlgCtrlID(button);
            bool enabled = id == kOpen || loaded_;
            if (id == kScreenshot || id == kBack || id == kForward)
                enabled = loaded_ && engine && engine->HasVideo() && !framePending_ && !engine->IsSeeking();
            EnableWindow(button, enabled);
        }
        EnableWindow(speed_, loaded_); EnableWindow(seek_, loaded_ && cutty::validDuration(duration));
        SetWindowTextW(GetDlgItem(window_, kPlay), loaded_ && engine && !engine->IsPaused() ? L"暂停" : L"播放");
        SetWindowTextW(GetDlgItem(window_, kLoop), loop_ ? L"循环：开" : L"循环：关");
        SetWindowTextW(GetDlgItem(window_, kMute), engine && engine->GetMuted() ? L"声音：关" : L"声音：开");
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
        KillTimer(window_, kTick); timerPeriod_ = 0;
        loaded_ = false; framePending_ = false; loopSeeking_ = false;
        temporaryShuttle_ = false; jklRate_ = 0; chosenRate_ = 1; markers_.clear();
        source_ = path; firstFrameRecorded_ = false; autoplayPending_ = true;
        SendMessageW(speed_, CB_SETCURSEL, 1, 0);
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
        require(player_.engine()->Pause());
        updateTimer(); update();
        const auto destination = chooseFile(true);
        if (destination.empty()) return;
        auto extension = destination.extension().wstring();
        for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
        if (extension != L".jpg" && extension != L".jpeg") { status(L"截图请使用 .jpg 或 .jpeg 文件名。"); return; }
        player_.screenshot(destination);
        status(L"已保存：" + destination.filename().wstring());
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
        case kScreenshot: screenshot(); break;
        default: break;
        }
    }
    void mediaEvent(DWORD event, ULONG generation) {
        if (generation != player_.generation() || !player_.engine()) return;
        auto* engine = player_.engine();
        switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            loaded_ = true; status(L"已加载，正在准备播放…"); break;
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
            if (loaded_ && autoplayPending_) { autoplayPending_ = false; playAt(chosenRate_); }
            break;
        case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
            if (!firstFrameRecorded_) {
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - opened_).count();
                status(L"首帧就绪 " + std::to_wstring(milliseconds) + L" ms（不含启动）");
                firstFrameRecorded_ = true;
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
            loaded_ = false; framePending_ = false;
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
    ~Application() { player_.close(); if (font_) DeleteObject(font_); }
    bool keyboard(const MSG& message) {
        if (message.message != WM_KEYDOWN && message.message != WM_KEYUP) return false;
        if (GetAncestor(message.hwnd, GA_ROOT) != window_) return false;
        // Preserve combo selection and any text input instead of stealing keys.
        wchar_t name[32]{}; GetClassNameW(GetFocus(), name, ARRAYSIZE(name));
        if (wcscmp(name, L"Edit") == 0 || (GetFocus() == speed_ && message.wParam != VK_ESCAPE)) return false;
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
        case 'K': jklRate_ = 0; require(player_.engine()->Pause()); updateTimer(); update(); return true;
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
        case WM_CREATE: createControls(); PostMessageW(window_, kInitialize, 0, 0); return 0;
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
            info->ptMinTrackSize = POINT{scale(980), scale(480)}; return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == kSpeed && HIWORD(wparam) == CBN_SELCHANGE && loaded_) {
                static constexpr double rates[] = {0.5, 1, 1.5, 2, 4, 8};
                const auto index = SendMessageW(speed_, CB_GETCURSEL, 0, 0);
                if (index >= 0 && index < static_cast<LRESULT>(ARRAYSIZE(rates))) playAt(rates[index]);
            } else if (HIWORD(wparam) == BN_CLICKED) { command(LOWORD(wparam)); SetFocus(window_); }
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lparam) == seek_ && loaded_) {
                const auto event = LOWORD(wparam);
                dragging_ = event == TB_THUMBTRACK;
                if (!dragging_) {
                    const double fraction = static_cast<double>(SendMessageW(seek_, TBM_GETPOS, 0, 0)) / 10000.0;
                    seek(fraction * player_.engine()->GetDuration());
                    update(); SetFocus(window_);
                }
            }
            return 0;
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wparam);
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::vector<wchar_t> name(static_cast<size_t>(length) + 1);
            DragQueryFileW(drop, 0, name.data(), length + 1); DragFinish(drop);
            if (length) open(fs::path(name.data())); SetFocus(window_); return 0;
        }
        case kMediaEvent: mediaEvent(static_cast<DWORD>(wparam), static_cast<ULONG>(lparam)); return 0;
        case WM_TIMER: if (wparam == kTick) tick(); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE && temporaryShuttle_) {
                temporaryShuttle_ = false; if (loaded_) playAt(resumeRate_, false);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kTick); player_.close(); loaded_ = false;
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
    if (message == WM_LBUTTONDOWN) { SetFocus(GetParent(window)); return 0; }
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
        cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassExW(&cls);
        cls.lpszClassName = L"CuttyVideoSurface"; cls.lpfnWndProc = videoProcedure;
        cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&cls);
        HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, L"AIVideoCuttyWindows", kTitle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            1100, 720, nullptr, nullptr, instance, &app);
        if (!window) result = 1;
        else {
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
