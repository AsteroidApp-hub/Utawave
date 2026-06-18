// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

#include "CrashReporter.h"
#include "../Localisation.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <dbghelp.h>   // dbghelp.lib は juce_core が #pragma comment でリンク済み
#else
 #include <csignal>
#endif

namespace
{
    constexpr int kConnectTimeoutMs = 5000;

    // install() (= メッセージスレッド) で記録し、クラッシュしたスレッドの判別に使う
    juce::Thread::ThreadID messageThreadId = nullptr;

    juce::String currentAppVersion()
    {
       #ifdef JUCE_APPLICATION_VERSION_STRING
        return JUCE_APPLICATION_VERSION_STRING;
       #else
        return "unknown";
       #endif
    }

    juce::String osDescription()
    {
        return juce::SystemStats::getOperatingSystemName()
               + " / " + juce::SystemStats::getDeviceDescription();
    }

   #if JUCE_WINDOWS
    // アドレスを「モジュール名 + RVA」で表す。ASLR 下では生アドレスに意味が無く、
    // 保管した PDB でオフライン解決するにはモジュール相対オフセットが必須。
    // プラグイン DLL 内のアドレスならその DLL 名が出る (本体かプラグインかを即判別できる)。
    juce::String moduleRelativeAddress(const void* addr)
    {
        HMODULE mod = nullptr;
        if (addr != nullptr
            && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                      | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(addr), &mod)
            && mod != nullptr)
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(mod, path, MAX_PATH);
            const auto name = juce::String(path).fromLastOccurrenceOf("\\", false, false);
            const auto rva = (juce::pointer_sized_uint) addr - (juce::pointer_sized_uint) mod;
            return name + " + 0x" + juce::String::toHexString((juce::int64) rva);
        }
        return "0x" + juce::String::toHexString((juce::int64) (juce::pointer_sized_uint) addr);
    }

    // JUCE の getStackBacktrace はシンボル解決に失敗したフレームを黙って捨てるため、
    // PDB の無い配布 exe / プラグイン DLL 内のフレーム (= 一番知りたい部分) が全て消える。
    // ここでは全フレームを module+RVA で必ず出力し、シンボル名は解決できた時だけ添える。
    //
    // platformData (= LPEXCEPTION_POINTERS) があれば、その ContextRecord 起点で x64 のスタックを
    // RtlVirtualUnwind で巻き戻す。CaptureStackBackTrace はこのハンドラ自身のスタックを歩くため、
    // x64 の例外ディスパッチ境界 (KiUserExceptionDispatcher) を越えられず、戻りアドレスがずれたり
    // スタック上に残る無関係な古いアドレス (別関数・ICF 畳み込み) を拾って混入させていた。
    // ContextRecord 起点なら「落ちた命令 → 本体フレーム」を正確に辿れる。
    juce::String windowsBacktrace(void* platformData)
    {
        const HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);

        alignas(SYMBOL_INFO) char symbolBuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuf);

        auto appendFrame = [&](juce::String& out, int i, DWORD64 pc)
        {
            out << i << ": " << moduleRelativeAddress(reinterpret_cast<const void*>(pc));
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen   = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, pc, &displacement, symbol))
                out << " (" << symbol->Name << " + 0x"
                    << juce::String::toHexString((juce::int64) displacement) << ")";
            out << "\n";
        };

        juce::String out;

       #if defined(_M_X64) || defined(_M_AMD64)
        auto* ep = static_cast<LPEXCEPTION_POINTERS>(platformData);
        if (ep != nullptr && ep->ContextRecord != nullptr)
        {
            CONTEXT ctx = *ep->ContextRecord;   // 破壊しながら歩くのでコピー
            for (int i = 0; i < 128 && ctx.Rip != 0; ++i)
            {
                appendFrame(out, i, ctx.Rip);

                DWORD64 imageBase = 0;
                auto* fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (fn != nullptr)
                {
                    PVOID   handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn,
                                     &ctx, &handlerData, &establisher, nullptr);
                }
                else
                {
                    // リーフ関数 (unwind 情報なし): 戻りアドレスをスタックから手動で取り出す
                    if (ctx.Rsp == 0) break;
                    ctx.Rip  = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += sizeof(DWORD64);
                }
            }
            return out;
        }
       #endif

        // フォールバック (非 x64 / 文脈が無い場合): 従来どおりハンドラ起点でキャプチャ
        void* stack[128];
        const int frames = (int) CaptureStackBackTrace(0, 128, stack, nullptr);
        for (int i = 0; i < frames; ++i)
            appendFrame(out, i, (DWORD64) stack[i]);
        return out;
    }

    // SystemStats のハンドラは Windows では LPEXCEPTION_POINTERS を渡してくる。
    // 例外コード + 発生アドレスに加え、AV / in-page error は read/write/execute と
    // 対象アドレスも記録する (null 近傍か解放済みヒープらしい値かの判別材料)。
    juce::String exceptionDetails(void* platformData)
    {
        auto* ep = static_cast<LPEXCEPTION_POINTERS>(platformData);
        if (ep == nullptr || ep->ExceptionRecord == nullptr)
            return {};

        const auto& rec  = *ep->ExceptionRecord;
        const auto  code = (juce::uint32) rec.ExceptionCode;

        juce::String s;
        s << "exception: 0x" << juce::String::toHexString((juce::int64) code);
        const auto name = CrashReporter::exceptionCodeName(code);
        if (name.isNotEmpty())
            s << " (" << name << ")";
        s << " at " << moduleRelativeAddress(rec.ExceptionAddress) << "\n";

        if ((code == 0xC0000005u || code == 0xC0000006u) && rec.NumberParameters >= 2)
        {
            const auto kind = rec.ExceptionInformation[0];
            s << "violation: "
              << (kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "unknown")
              << " of address 0x"
              << juce::String::toHexString((juce::int64) rec.ExceptionInformation[1]) << "\n";
        }
        return s;
    }
   #else
    const char* signalName(int sig)
    {
        switch (sig)
        {
            case SIGSEGV: return "SIGSEGV";
            case SIGBUS:  return "SIGBUS";
            case SIGFPE:  return "SIGFPE";
            case SIGILL:  return "SIGILL";
            case SIGABRT: return "SIGABRT";
            case SIGSYS:  return "SIGSYS";
            default:      return "";
        }
    }
   #endif

    // クラッシュ時に呼ばれる。シグナルハンドラ内での確保は厳密には async-signal-safe では
    // ないが、ここはプロセスがどのみち落ちる直前の best-effort (JUCE 標準の作法)。
    void onCrash(void* platformData)
    {
        const auto dir = CrashReporter::crashLogDirectory();
        dir.createDirectory();
        const auto file = dir.getChildFile(
            CrashReporter::crashLogFileName(juce::Time::getCurrentTime()));

        juce::String log;
        log << "Utawave crash log\n"
            << "version: " << currentAppVersion() << "\n"
            << "os: "      << osDescription() << "\n"
            << "time: "    << juce::Time::getCurrentTime().toISO8601(true) << "\n";

        const auto tid = juce::Thread::getCurrentThreadId();
        log << "thread: 0x"
            << juce::String::toHexString((juce::int64) (juce::pointer_sized_int) tid)
            << (tid == messageThreadId ? " (message thread)" : " (background thread)") << "\n";

       #if JUCE_WINDOWS
        log << exceptionDetails(platformData)
            << "\n--- stack backtrace ---\n"
            << windowsBacktrace(platformData);
       #else
        const int sig = (int) (juce::pointer_sized_int) platformData;
        log << "signal: " << sig;
        if (signalName(sig)[0] != 0)
            log << " (" << signalName(sig) << ")";
        log << "\n\n--- stack backtrace ---\n"
            << juce::SystemStats::getStackBacktrace();
       #endif

        file.replaceWithText(log);
    }
} // namespace

void CrashReporter::install()
{
    // 先にフォルダを作っておく (クラッシュハンドラ内の仕事を減らす + 手動確認しやすくする)
    crashLogDirectory().createDirectory();
    messageThreadId = juce::Thread::getCurrentThreadId();
    juce::SystemStats::setApplicationCrashHandler(onCrash);
}

juce::String CrashReporter::exceptionCodeName(juce::uint32 code)
{
    // 値は winnt.h の定数だが、非 Windows でもテストできるようリテラルで持つ
    switch (code)
    {
        case 0x80000003u: return "EXCEPTION_BREAKPOINT";
        case 0x80000004u: return "EXCEPTION_SINGLE_STEP";
        case 0xC0000005u: return "EXCEPTION_ACCESS_VIOLATION";
        case 0xC0000006u: return "EXCEPTION_IN_PAGE_ERROR";
        case 0xC000001Du: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case 0xC0000025u: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case 0xC000008Cu: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case 0xC000008Eu: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case 0xC0000090u: return "EXCEPTION_FLT_INVALID_OPERATION";
        case 0xC0000094u: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case 0xC00000FDu: return "EXCEPTION_STACK_OVERFLOW";
        case 0xC0000374u: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN";
        case 0xE06D7363u: return "C++ exception";
        default:          return {};
    }
}

juce::File CrashReporter::crashLogDirectory()
{
    // 設定/最近使ったプロジェクト置き場 (~/Library/Utawave) を汚さないよう、ログは OS の
    // 標準ログ置き場に分離する。macOS は ~/Library/Logs/Utawave (Console.app からも見える)
   #if JUCE_MAC
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
               .getChildFile("Library/Logs/Utawave");
   #else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("Utawave").getChildFile("Logs");
   #endif
}

juce::String CrashReporter::crashLogFileName(juce::Time when)
{
    return "crash-" + when.formatted("%Y%m%d_%H%M%S") + ".log";
}

std::vector<juce::File> CrashReporter::pendingLogs(const juce::File& dir)
{
    std::vector<juce::File> out;
    if (!dir.isDirectory()) return out;
    for (const auto& e : dir.findChildFiles(juce::File::findFiles, false, "crash-*.log"))
        out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const juce::File& a, const juce::File& b)
              { return a.getLastModificationTime() > b.getLastModificationTime(); });
    return out;
}

bool CrashReporter::markHandled(const juce::File& logFile)
{
    if (!logFile.existsAsFile()) return false;
    return logFile.moveFileTo(logFile.withFileExtension("handled"));
}

juce::String CrashReporter::buildReportJson(const juce::String& appVersion,
                                            const juce::String& osDesc,
                                            const juce::String& logFileName,
                                            const juce::String& logContent)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("app",     "Utawave");
    obj->setProperty("version", appVersion);
    obj->setProperty("os",      osDesc);
    obj->setProperty("file",    logFileName);
    obj->setProperty("log",     logContent);
    return juce::JSON::toString(juce::var(obj));
}

bool CrashReporter::reportingCompiledIn()
{
   #ifdef UTAWAVE_CRASH_REPORT_URL
    return true;
   #else
    return false;
   #endif
}

juce::String CrashReporter::defaultReportUrl()
{
   #ifdef UTAWAVE_CRASH_REPORT_URL
    return UTAWAVE_CRASH_REPORT_URL;
   #else
    return {};
   #endif
}

void CrashReporter::sendAsync(const juce::String& url, const juce::String& json,
                              std::function<void(bool)> onDone)
{
    // AdService / UpdateChecker と同じ作法: 値コピーのデタッチスレッドで通信し、
    // 結果だけを callAsync でメッセージスレッドへ返す (呼び出し元の生存に依存しない)。
    juce::Thread::launch([url, json, onDone = std::move(onDone)]
    {
        bool ok = false;
        {
            int statusCode = 0;
            const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                                  .withConnectionTimeoutMs(kConnectTimeoutMs)
                                  .withExtraHeaders("Content-Type: application/json\r\n"
                                                    "User-Agent: Utawave")
                                  .withStatusCode(&statusCode);
            juce::URL u = juce::URL(url).withPOSTData(json);
            if (auto in = u.createInputStream(opts))
            {
                // 応答ボディは読み捨て (サイズ/時間上限つき)
                char buf[1024];
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
                while (in->read(buf, (int)sizeof(buf)) > 0
                       && juce::Time::getMillisecondCounterHiRes() < deadline) {}
                ok = (statusCode >= 200 && statusCode < 300);
            }
        }
        if (onDone)
            juce::MessageManager::callAsync([onDone, ok] { onDone(ok); });
    });
}

void CrashReporter::offerPendingReports()
{
    const auto pending = pendingLogs(crashLogDirectory());
    if (pending.empty()) return;

    // 複数たまっていても送るのは最新 1 件 (残りもまとめて処理済みにして再ナグを防ぐ)
    const juce::File newest = pending.front();
    auto markAllHandled = [pending]
    {
        for (const auto& f : pending) markHandled(f);
    };

    if (!reportingCompiledIn())
    {
        // 送信先が無いビルド (公開ソースの既定): ローカル確認のみ提示
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(tr(u8"前回終了時に問題が発生しました"))
                .withMessage(tr(u8"クラッシュログが保存されています。内容を確認しますか？"))
                .withButton(tr(u8"ログを表示"))
                .withButton(tr(u8"閉じる")),
            [newest, markAllHandled](int result)
            {
                if (result == 1) newest.revealToUser();
                markAllHandled();
            });
        return;
    }

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(tr(u8"前回終了時に問題が発生しました"))
            .withMessage(tr(u8"アプリの改善のため、エラーログを開発者に送信してもよろしいですか？\n"
                            u8"ログにはクラッシュ時の技術情報 (スタックトレース・アプリのバージョン・OS) のみが含まれ、\n"
                            u8"音声データや個人情報は含まれません。"))
            .withButton(tr(u8"送信する"))
            .withButton(tr(u8"送信しない"))
            .withButton(tr(u8"ログを表示")),
        [newest, markAllHandled](int result)
        {
            if (result == 3)   // ログを表示 (処理はまだ確定しない → 次回また聞く)
            {
                newest.revealToUser();
                return;
            }
            if (result == 1)   // 送信する
            {
                const auto json = buildReportJson(currentAppVersion(), osDescription(),
                                                  newest.getFileName(),
                                                  newest.loadFileAsString());
                sendAsync(defaultReportUrl(), json, [](bool ok)
                {
                    if (ok) return;   // 成功時は静かに完了
                    juce::AlertWindow::showAsync(
                        juce::MessageBoxOptions()
                            .withIconType(juce::MessageBoxIconType::InfoIcon)
                            .withTitle(tr(u8"送信できませんでした"))
                            .withMessage(tr(u8"エラーログを送信できませんでした。\n"
                                            u8"インターネット接続を確認してください (ログはローカルに残っています)。"))
                            .withButton("OK"),
                        nullptr);
                });
            }
            // 「送信する」(送信試行後) /「送信しない」とも処理済みにして再ナグしない
            markAllHandled();
        });
}
