// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// アプリ音声取り込みの Windows 実装 (WASAPI プロセスループバック)。
// OBS 28 の「アプリケーション音声キャプチャ」と同じ API
// (ActivateAudioInterfaceAsync + AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK・
// Windows 10 2004+ / 本アプリは Win11+)。設計は Docs/internal/PLAN_アプリ音声取り込み.md。
//
// 構成:
//  - 専用キャプチャスレッド (MTA) が「対象 PID の解決 → セッション確立 → パケットループ」を
//    自己完結で回す。対象未起動 / プロセス終了 / デバイスエラーは 2 秒間隔で再試行
//    (= ブラウザ再起動で自動復帰・watchdog を別スレッドに置かない)
//  - 対象は「同名 exe の最上位祖先プロセス + 子孫ツリー (INCLUDE_TARGET_PROCESS_TREE)」。
//    Chromium 系 (chrome/edge) は音声がユーティリティ子プロセスから出るため木ごと取る
//  - キャプチャフォーマットはこちらが指定し OS が変換する (float32/2ch/エンジン SR を要求・
//    ダメなら 48k → 44.1k → int16 の順でフォールバック)。SR 差は StreamMirrorReader が吸収
//  - プロセスループバックのクライアントは GetMixFormat / GetCurrentPadding /
//    GetStreamLatency / IAudioClock をサポートしない (呼ばないこと・MS ドキュメントより)
//  - キャプチャスレッドは RT (audio thread) ではないので、スクラッチの確保等は許容。
//    ring->push は SPSC ロックフリー (reader = AudioEngine の audio thread)

#include "AppAudioCapture.h"

#if JUCE_WINDOWS

#include "AudioEngine.h"
#include "../Localisation.h"

#include <windows.h>
#include <objbase.h>
#include <objidl.h>              // IAgileObject
#include <mmreg.h>               // WAVE_FORMAT_IEEE_FLOAT
#include <mmdeviceapi.h>         // IMMDeviceEnumerator / ActivateAudioInterfaceAsync
#include <audiopolicy.h>         // IAudioSessionManager2 / IAudioSessionControl2
#include <audioclient.h>         // IAudioClient / IAudioCaptureClient
#include <tlhelp32.h>            // プロセススナップショット (親子関係 + exe 名)

#if __has_include(<audioclientactivationparams.h>)
 #include <audioclientactivationparams.h>   // AUDIOCLIENT_ACTIVATION_PARAMS (SDK 10.0.19041+)
 #define UTAWAVE_HAS_PROCESS_LOOPBACK 1
#else
 #define UTAWAVE_HAS_PROCESS_LOOPBACK 0    // 古い SDK ではビルドは通し機能だけ無効化する
#endif

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "version.lib")

#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_map>

#if UTAWAVE_HAS_PROCESS_LOOPBACK

namespace
{

// 最小 COM RAII (JUCE 内部の ComSmartPtr に依存しない・このファイル専用)
template <typename T>
struct ComPtr
{
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (p != nullptr) p->Release(); }
    T** reset() { if (p != nullptr) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

//==============================================================================
// プロセススナップショット (pid → 親 pid + exe 名)。対象解決と列挙の dedupe で使う。
struct ProcInfo { DWORD ppid = 0; juce::String exe; };

std::unordered_map<DWORD, ProcInfo> snapshotProcesses()
{
    std::unordered_map<DWORD, ProcInfo> map;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return map;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            map[pe.th32ProcessID] = { pe.th32ParentProcessID, juce::String(pe.szExeFile) };
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return map;
}

// pid から「同名 exe の親」をたどって最上位の祖先を返す。Chromium 系は音声がユーティリティ
// 子プロセスから出るため、ブラウザ本体 (木の根) を対象にして INCLUDE_TREE で全部拾う。
// PID 再利用による誤リンクは「親が同名」の条件でほぼ排除できる (OBS と同じ考え方)。
DWORD rootSameNameAncestor(DWORD pid, const std::unordered_map<DWORD, ProcInfo>& map)
{
    DWORD cur = pid;
    for (int depth = 0; depth < 32; ++depth)   // 循環ガード
    {
        const auto it = map.find(cur);
        if (it == map.end()) break;
        const DWORD ppid = it->second.ppid;
        if (ppid == 0 || ppid == cur) break;
        const auto pit = map.find(ppid);
        if (pit == map.end() || !pit->second.exe.equalsIgnoreCase(it->second.exe)) break;
        cur = ppid;
    }
    return cur;
}

// exe 名から対象 PID (同名最上位祖先) を解決する。見つからなければ 0。
// 音声セッションの有無は問わない (ブラウザを開いただけ = まだ鳴っていない状態でも
// アタッチしておき、鳴り始めたらパケットが流れる)。
DWORD resolveTargetPidByName(const juce::String& exe)
{
    const auto map = snapshotProcesses();
    DWORD best = 0;
    for (const auto& kv : map)
        if (kv.second.exe.equalsIgnoreCase(exe))
        {
            const DWORD root = rootSameNameAncestor(kv.first, map);
            if (best == 0 || root < best) best = root;
        }
    return best;
}

// exe の VersionInfo FileDescription (例 "Google Chrome")。取れなければ空 (best effort)。
juce::String fileDescriptionForProcess(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return {};
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != 0;
    CloseHandle(h);
    if (!ok) return {};

    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (size == 0) return {};
    std::vector<BYTE> buf((size_t) size);
    if (!GetFileVersionInfoW(path, 0, size, buf.data())) return {};

    struct LangCp { WORD lang, cp; };
    LangCp* trans = nullptr;
    UINT tLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**) &trans, &tLen)
        || trans == nullptr || tLen < sizeof(LangCp))
        return {};
    wchar_t sub[64] = {};
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", trans[0].lang, trans[0].cp);
    wchar_t* desc = nullptr;
    UINT dLen = 0;
    if (!VerQueryValueW(buf.data(), sub, (void**) &desc, &dLen) || desc == nullptr || dLen == 0)
        return {};
    return juce::String(desc).trim();
}

//==============================================================================
// ActivateAudioInterfaceAsync の完了待ち。ヒープ所有 + 参照カウント (呼び出し側 1 +
// async 操作が持つ分)。タイムアウト後に遅れて完了しても、dtor が client を解放するので安全。
// IAgileObject を名乗り MTA からの直接コールバックを受ける (MS サンプルの FtmBase 相当)。
class ActivationWaiter final : public IActivateAudioInterfaceCompletionHandler,
                               public IAgileObject
{
public:
    ActivationWaiter() : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    // 完了を待って IAudioClient の所有権を取り出す (失敗/タイムアウトは nullptr)
    IAudioClient* waitAndTake(DWORD timeoutMs)
    {
        if (done == nullptr || WaitForSingleObject(done, timeoutMs) != WAIT_OBJECT_0)
            return nullptr;
        return client.exchange(nullptr);
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
    {
        HRESULT hrActivate = E_FAIL;
        IUnknown* unk = nullptr;
        if (op != nullptr && SUCCEEDED(op->GetActivateResult(&hrActivate, &unk))
            && SUCCEEDED(hrActivate) && unk != nullptr)
        {
            IAudioClient* c = nullptr;
            if (SUCCEEDED(unk->QueryInterface(__uuidof(IAudioClient), (void**) &c)))
                client.store(c);
        }
        if (unk != nullptr) unk->Release();
        if (done != nullptr) SetEvent(done);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
        {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject))
        {
            *ppv = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }

private:
    ~ActivationWaiter()
    {
        if (done != nullptr) CloseHandle(done);
        if (auto* c = client.exchange(nullptr)) c->Release();
    }
    std::atomic<ULONG> refs { 1 };
    std::atomic<IAudioClient*> client { nullptr };
    HANDLE done = nullptr;
};

// 指定 PID (プロセスツリー) のループバッククライアントをアクティベートする。
IAudioClient* activateProcessLoopback(DWORD pid)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv = {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* waiter = new ActivationWaiter();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    IAudioClient* client = nullptr;
    if (SUCCEEDED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                              __uuidof(IAudioClient), &pv, waiter, op.reset())))
        client = waiter->waitAndTake(5000);
    waiter->Release();
    return client;
}

void makeFormat(WAVEFORMATEX& f, DWORD rate, bool isFloat)
{
    f = {};
    f.wFormatTag     = isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    f.nChannels      = 2;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = isFloat ? 32 : 16;
    f.nBlockAlign    = (WORD) (f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    f.cbSize = 0;
}

} // namespace

//==============================================================================
struct AppAudioCapture::Impl
{
    juce::String exe;                              // 対象実行ファイル名
    AudioEngine* engine = nullptr;
    std::shared_ptr<StreamMirrorRing> ring;
    std::thread th;
    std::atomic<bool> stopRequested { false };
    std::atomic<juce::uint32> lastPacketMs { 0 };  // isReceiving 用 (0 = 未受信)
    double preferredRate = 48000.0;
    HANDLE hStop = nullptr;                        // 再試行スリープを即時に起こす手動リセットイベント

    Impl() : hStop(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Impl()
    {
        jassert(!th.joinable());                   // stop() が join 済みであること
        if (hStop != nullptr) CloseHandle(hStop);
    }

    void waitRetry() { WaitForSingleObject(hStop, 2000); }

    void threadMain()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stopRequested.load())
        {
            const DWORD pid = resolveTargetPidByName(exe);
            if (pid == 0) { waitRetry(); continue; }   // 対象未起動 → 待機 (起動したら鳴り出す)
            runSession(pid);
            if (!stopRequested.load()) waitRetry();    // プロセス終了/エラー → 再解決 (自動復帰)
        }
        CoUninitialize();
    }

    // 1 回のキャプチャセッション: アタッチ → パケットループ。抜けたら呼び出し側が再試行する。
    void runSession(DWORD pid)
    {
        // 死活監視用 (開けなくてもキャプチャ自体は可能・その場合は再解決が遅れるだけ)
        HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, pid);

        // フォーマット交渉: float32/2ch のエンジン SR → 48k → 44.1k → int16/44.1k。
        // Initialize に失敗したクライアントは再利用せず、試行ごとに再アクティベートする
        WAVEFORMATEX fmt = {};
        IAudioClient* client = nullptr;
        const DWORD wanted = (DWORD) (preferredRate > 0.0 ? preferredRate : 48000.0);
        const struct { DWORD rate; bool isFloat; } candidates[] =
            { { wanted, true }, { 48000, true }, { 44100, true }, { 44100, false } };
        for (const auto& cand : candidates)
        {
            makeFormat(fmt, cand.rate, cand.isFloat);
            IAudioClient* c = activateProcessLoopback(pid);
            if (c == nullptr) break;               // アクティベーション不可 (OS 非対応等) → 再試行へ
            const HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                             AUDCLNT_STREAMFLAGS_LOOPBACK
                                                 | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             2000000 /* 200ms (100ns 単位) */, 0, &fmt, nullptr);
            if (SUCCEEDED(hr)) { client = c; break; }
            c->Release();
        }
        if (client == nullptr)
        {
            if (hProc != nullptr) CloseHandle(hProc);
            return;
        }

        IAudioCaptureClient* capture = nullptr;
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool ok = evt != nullptr
                     && SUCCEEDED(client->SetEventHandle(evt))
                     && SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), (void**) &capture))
                     && SUCCEEDED(client->Start());
        if (ok)
        {
            // ソース SR 確定 (epoch が進み reader は溜め直しから開始 = セッション再確立の
            // 継ぎ目で古いデータを読まない)。reset は writer スレッド (= ここ) から安全
            ring->reset((double) fmt.nSamplesPerSec);
            captureLoop(*capture, fmt, evt, hProc);
            client->Stop();
        }
        if (capture != nullptr) capture->Release();
        if (evt != nullptr) CloseHandle(evt);
        client->Release();
        if (hProc != nullptr) CloseHandle(hProc);
    }

    void captureLoop(IAudioCaptureClient& capture, const WAVEFORMATEX& fmt, HANDLE evt, HANDLE hProc)
    {
        std::vector<float> dl, dr;                 // deinterleave スクラッチ (非 RT スレッドなので確保可)
        while (!stopRequested.load())
        {
            WaitForSingleObject(evt, 200);         // 200ms 上限 = 停止/死活チェックの遅延上限
            if (stopRequested.load()) break;
            if (hProc != nullptr && WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0)
                break;                             // 対象プロセス終了 → セッションを畳んで再解決へ

            for (;;)
            {
                UINT32 next = 0;
                if (FAILED(capture.GetNextPacketSize(&next))) return;   // デバイス無効化等 → 再構築
                if (next == 0) break;

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture.GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;
                if (frames > 0)
                {
                    if (dl.size() < (size_t) frames) { dl.resize((size_t) frames); dr.resize((size_t) frames); }
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
                    if (silent)
                    {
                        // 無音パケットもゼロで push して水位を保つ (枯渇 priming の往復を防ぐ)
                        std::fill_n(dl.data(), frames, 0.0f);
                        std::fill_n(dr.data(), frames, 0.0f);
                    }
                    else if (fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                    {
                        const float* s = reinterpret_cast<const float*>(data);
                        for (UINT32 i = 0; i < frames; ++i)
                        {
                            dl[i] = s[2 * i];
                            dr[i] = s[2 * i + 1];
                        }
                    }
                    else   // int16 フォールバック
                    {
                        const juce::int16* s = reinterpret_cast<const juce::int16*>(data);
                        for (UINT32 i = 0; i < frames; ++i)
                        {
                            dl[i] = (float) s[2 * i]     * (1.0f / 32768.0f);
                            dr[i] = (float) s[2 * i + 1] * (1.0f / 32768.0f);
                        }
                    }
                    ring->push(dl.data(), dr.data(), (int) frames);
                    lastPacketMs.store(juce::Time::getMillisecondCounter());
                }
                if (FAILED(capture.ReleaseBuffer(frames))) return;
            }
        }
    }
};

//==============================================================================
AppAudioCapture::AppAudioCapture() : impl(std::make_unique<Impl>()) {}
AppAudioCapture::~AppAudioCapture() { stop(); }

bool AppAudioCapture::isSupported() { return true; }   // 本アプリの対応 OS (Win11+) は常に可

std::vector<AppAudioCapture::AppInfo> AppAudioCapture::listAudioApps()
{
    std::vector<AppInfo> result;
    const DWORD selfPid = GetCurrentProcessId();
    const auto procs = snapshotProcesses();

    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**) devEnum.reset())))
        return result;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(devEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.reset())))
        return result;
    UINT devCount = 0;
    devices->GetCount(&devCount);

    // 全レンダーデバイス横断で音声セッションを集め、同名 exe (最上位祖先) に dedupe する
    juce::StringArray seenExes;
    for (UINT d = 0; d < devCount; ++d)
    {
        ComPtr<IMMDevice> dev;
        if (FAILED(devices->Item(d, dev.reset()))) continue;
        ComPtr<IAudioSessionManager2> mgr;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                 (void**) mgr.reset())))
            continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(mgr->GetSessionEnumerator(sessions.reset()))) continue;
        int n = 0;
        sessions->GetCount(&n);
        for (int i = 0; i < n; ++i)
        {
            ComPtr<IAudioSessionControl> ctrl;
            if (FAILED(sessions->GetSession(i, ctrl.reset()))) continue;
            ComPtr<IAudioSessionControl2> c2;
            if (FAILED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**) c2.reset())))
                continue;
            if (c2->IsSystemSoundsSession() == S_OK) continue;
            AudioSessionState st = AudioSessionStateInactive;
            if (SUCCEEDED(ctrl->GetState(&st)) && st == AudioSessionStateExpired)
                continue;   // 終了済みセッションは除外 (Active/Inactive = 一時停止中は含める)
            DWORD pid = 0;
            if (FAILED(c2->GetProcessId(&pid)) || pid == 0 || pid == selfPid) continue;

            const DWORD root = rootSameNameAncestor(pid, procs);
            if (root == selfPid) continue;
            const auto it = procs.find(root);
            if (it == procs.end() || it->second.exe.isEmpty()) continue;
            const juce::String exe = it->second.exe;
            if (seenExes.contains(exe, true)) continue;
            seenExes.add(exe);

            AppInfo info;
            info.executable = exe;
            info.pid = (juce::uint32) root;
            const juce::String desc = fileDescriptionForProcess(root);
            info.displayName = desc.isNotEmpty() ? desc + " (" + exe + ")" : exe;
            result.push_back(std::move(info));
        }
    }

    std::sort(result.begin(), result.end(),
              [](const AppInfo& a, const AppInfo& b)
              { return a.displayName.compareIgnoreCase(b.displayName) < 0; });
    return result;
}

juce::String AppAudioCapture::start(const juce::String& executable, AudioEngine& engine)
{
    stop();
    const juce::String exe = executable.trim();
    if (exe.isEmpty())
        return tr(u8"取り込むアプリを選択してください");

    impl->exe = exe;
    impl->engine = &engine;
    impl->preferredRate = engine.getSampleRate();
    impl->ring = std::make_shared<StreamMirrorRing>();
    // ソース SR はセッション確立時にキャプチャスレッドが reset(実フォーマット SR) で設定する。
    // それまで srcRate = 0 で reader は無音 (= 「待機」状態が構造的に安全)
    engine.setAppCaptureRing(impl->ring);

    impl->stopRequested.store(false);
    impl->lastPacketMs.store(0);
    if (impl->hStop != nullptr) ResetEvent(impl->hStop);
    impl->th = std::thread([im = impl.get()] { im->threadMain(); });
    return {};
}

void AppAudioCapture::stop()
{
    if (impl->th.joinable())
    {
        impl->stopRequested.store(true);
        if (impl->hStop != nullptr) SetEvent(impl->hStop);
        impl->th.join();
        impl->th = {};
    }
    if (impl->engine != nullptr)
        impl->engine->setAppCaptureRing(nullptr);
    impl->engine = nullptr;
    impl->ring.reset();
    impl->lastPacketMs.store(0);
}

bool AppAudioCapture::isRunning() const { return impl->th.joinable(); }

bool AppAudioCapture::isReceiving() const
{
    const juce::uint32 last = impl->lastPacketMs.load();
    return isRunning() && last != 0
        && (juce::Time::getMillisecondCounter() - last) < 1000;
}

juce::String AppAudioCapture::getTargetExecutable() const { return impl->exe; }

#else // UTAWAVE_HAS_PROCESS_LOOPBACK == 0 (SDK が古い場合のフォールバック・機能無効)

struct AppAudioCapture::Impl {};
AppAudioCapture::AppAudioCapture() : impl(std::make_unique<Impl>()) {}
AppAudioCapture::~AppAudioCapture() = default;
bool AppAudioCapture::isSupported() { return false; }
std::vector<AppAudioCapture::AppInfo> AppAudioCapture::listAudioApps() { return {}; }
juce::String AppAudioCapture::start(const juce::String&, AudioEngine&)
{
    return tr(u8"この環境ではアプリ音声の取り込みに対応していません");
}
void AppAudioCapture::stop() {}
bool AppAudioCapture::isRunning() const   { return false; }
bool AppAudioCapture::isReceiving() const { return false; }
juce::String AppAudioCapture::getTargetExecutable() const { return {}; }

#endif // UTAWAVE_HAS_PROCESS_LOOPBACK

#endif // JUCE_WINDOWS
