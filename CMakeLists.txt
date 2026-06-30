cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)

project(Utawave VERSION 0.5.6)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── JUCE 8 ──
# 公開リポジトリには JUCE を含めない。ローカルに JUCE/ があればそれを使い (オフライン/高速)、
# 無ければ FetchContent で固定バージョンを取得する (CI / 新規 clone はこちら)。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    message(STATUS "JUCE: ローカルの JUCE/ を使用")
    add_subdirectory(JUCE)
else()
    message(STATUS "JUCE: ローカルに無いため FetchContent で取得 (8.0.12)")
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.12          # 開発時 commit は 501c0767。厳密一致が必要ならこの SHA に置換し GIT_SHALLOW を外す
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(Utawave
    PRODUCT_NAME                    "Utawave"
    COMPANY_NAME                    "Utawave"
    BUNDLE_ID                       "com.utawave.app"
    VERSION                         "0.5.6"
    ICON_BIG                        "${CMAKE_CURRENT_SOURCE_DIR}/Icon/icon_1024.png"
    ICON_SMALL                      "${CMAKE_CURRENT_SOURCE_DIR}/Icon/icon_256.png"
    MICROPHONE_PERMISSION_ENABLED   TRUE
    MICROPHONE_PERMISSION_TEXT      "Utawave は音声の録音にマイクへのアクセスを必要とします。 / Utawave needs microphone access for audio recording."
    PLIST_TO_MERGE                  "<plist version=\"1.0\"><dict><key>NSHumanReadableCopyright</key><string>Copyright (C) 2025-2026 Utawave</string></dict></plist>"
)

juce_generate_juce_header(Utawave)

target_sources(Utawave PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/MainComponent_Commands.cpp
    Source/MainComponent_Plugins.cpp
    Source/MainComponent_Project.cpp
    Source/MainComponent_Dialogs.cpp
    Source/MainComponent_Preferences.cpp
    Source/MainComponent_DialogsMisc.cpp
    Source/Localisation.cpp
    Source/Audio/AudioEngine.cpp
    Source/Audio/AudioFileImporter.cpp
    Source/Audio/AudioDeviceSettings.cpp
    Source/Audio/FileStreamVoice.cpp
    Source/Audio/AudioWorkerPool.cpp
    Source/Audio/PitchEngine.cpp
    Source/Audio/pitchcore/PitchShiftEngine.cpp
    Source/Audio/builtin/BuiltInEQ.cpp
    Source/Audio/builtin/BuiltInCompressor.cpp
    Source/Audio/builtin/BuiltInDeEsser.cpp
    Source/Audio/builtin/BuiltInGate.cpp
    Source/Audio/builtin/BuiltInMaximizer.cpp
    Source/Audio/builtin/BuiltInDelay.cpp
    Source/Audio/builtin/BuiltInReverb.cpp
    Source/Audio/builtin/BuiltInFactory.cpp
    Source/Tracks/AudioClip.cpp
    Source/Tracks/Track.cpp
    Source/Tracks/TrackManager.cpp
    Source/Recording/RecordingManager.cpp
    Source/Project/ProjectManager.cpp
    Source/Project/BackupManager.cpp
    Source/Project/RecentProjects.cpp
    Source/Project/WindowState.cpp
    Source/Project/AppPreferences.cpp
    Source/Project/CrashReporter.cpp
    Source/Project/AdService.cpp
    Source/Project/UpdateChecker.cpp
    Source/Export/ExportEngine.cpp
    Source/Export/ExportDialog.cpp
    Source/Export/Mp3EncoderWriter.cpp
    Source/VST/PluginManager.cpp
    Source/VST/PluginScannerProcess.cpp
    Source/VST/PluginChain.cpp
    Source/MIDI/MidiImporter.cpp
    Source/MIDI/MidiExporter.cpp
    Source/MIDI/MidiImportDialog.cpp
    Source/MIDI/InternalSynth.cpp
    $<$<PLATFORM_ID:Darwin>:Source/Mac/GlobalKeyMonitor.mm>
    $<$<NOT:$<PLATFORM_ID:Darwin>>:Source/Mac/GlobalKeyMonitor_stub.cpp>
    $<$<PLATFORM_ID:Darwin>:Source/Mac/AppNap.mm>
    $<$<NOT:$<PLATFORM_ID:Darwin>>:Source/Mac/AppNap_stub.cpp>
    Source/UI/PluginManagerDialog.cpp
    Source/UI/StartupComponent.cpp
    Source/UI/AdPanel.cpp
    Source/UI/Toolbar.cpp
    Source/UI/TapTempoDialog.cpp
    Source/UI/TrackHeaderPanel.cpp
    Source/UI/TrackHeaderView.cpp
    Source/UI/TrackHeaderView_Interaction.cpp
    Source/UI/TimelineView.cpp
    Source/UI/TimelineView_Drawing.cpp
    Source/UI/TimelineView_Mouse.cpp
    Source/UI/TimelineView_ContextMenu.cpp
    Source/UI/TimelineView_MidiEdit.cpp
    Source/UI/TimelineView_Edit.cpp
    Source/UI/TimelineView_EditOps.cpp
    Source/UI/TimelineView_Markers.cpp
    Source/UI/MasterPanel.cpp
    Source/UI/PianoRollEditor.cpp
    Source/UI/BuiltInEffectEditor.cpp
    Source/UI/ModernEffectEditor.cpp
    Source/UI/BuiltInEQEditor.cpp
    Source/UI/BuiltInCompressorEditor.cpp
    Source/UI/BuiltInDeEsserEditor.cpp
    Source/UI/BuiltInGateEditor.cpp
    Source/UI/BuiltInMaximizerEditor.cpp
    Source/UI/BuiltInDelayEditor.cpp
    Source/UI/BuiltInReverbEditor.cpp
    Source/UI/StatusBar.cpp
)

target_include_directories(Utawave SYSTEM PRIVATE
    Source/ThirdParty/r8brain
)

target_compile_definitions(Utawave PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MICROPHONE_PERMISSION_ENABLED=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    # プラグインホスティング: VST3 のみ (AU は非対応)
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    # macOS で Metal レイヤレンダラを使う。既定の CoreGraphics 非同期レイヤ描画だと、AppKit が
    # ディスプレイリンク同期で CA トランザクションをフラッシュする際に CA::Transaction::commit が
    # メッセージスレッドで描画完了を待ち (DispatchGroup cond_wait)、トラック数/ズームに比例して
    # 再生バー等の UI が周期的に数百 ms 固まる (調査で確定した根本原因)。Metal は GPU ネイティブに
    # 合成するためこの待ちが消え、メモリ使用量も削減される (実機で表示不具合・スリープ/Spaces
    # 復帰も問題なしを確認済み)。macOS 専用フラグなので Darwin のみで有効化する。
    $<$<PLATFORM_ID:Darwin>:JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1>
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Utawave,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Utawave,JUCE_VERSION>"
)

# ── 起動画面の広告枠 (任意・既定 OFF) ──
# 公開ソースの既定はオフ (起動画面は従来どおり 2 列・通信なし)。公式配布ビルドだけが
# 配布ビルド時に以下を渡して 3 列の広告枠を有効化し、本番フィード URL を埋め込む:
#   cmake -DUTAWAVE_ADS_ENABLED=ON -DUTAWAVE_AD_FEED_URL="https://.../feed.json" ...
# 実効表示は (UTAWAVE_ADS_ENABLED && AppPreferences::showAds)。URL 未指定時はソースの既定値。
#
# 開発時に見た目を確認するには -DUTAWAVE_ADS_DEBUG=ON を渡す。広告枠を有効化し、サーバー無しで
# 組み込みのサンプル広告 (複数件・画像つき) を表示する (通信しない)。
option(UTAWAVE_ADS_ENABLED "Enable the startup-screen ad panel (official builds only)" OFF)
option(UTAWAVE_ADS_DEBUG   "Show built-in sample ads for development (implies ads enabled, no server)" OFF)
set(UTAWAVE_AD_FEED_URL "" CACHE STRING "Ad feed JSON URL embedded at build time (official builds set this)")
if(UTAWAVE_ADS_ENABLED OR UTAWAVE_ADS_DEBUG)
    target_compile_definitions(Utawave PRIVATE UTAWAVE_ADS_ENABLED=1)
endif()
if(UTAWAVE_ADS_DEBUG)
    target_compile_definitions(Utawave PRIVATE UTAWAVE_ADS_DEBUG=1)
    message(STATUS "Utawave: startup ad panel ENABLED (debug sample mode / no server)")
elseif(UTAWAVE_ADS_ENABLED)
    message(STATUS "Utawave: startup ad panel ENABLED")
else()
    message(STATUS "Utawave: startup ad panel disabled (default / public source)")
endif()
if(UTAWAVE_AD_FEED_URL)
    target_compile_definitions(Utawave PRIVATE UTAWAVE_AD_FEED_URL="${UTAWAVE_AD_FEED_URL}")
    message(STATUS "Utawave: ad feed URL = ${UTAWAVE_AD_FEED_URL}")
endif()

# ── 起動画面のアップデート通知 (公式サイトの version JSON) ──
# 公式ビルドは配布サイトの version JSON / ダウンロードページ URL をビルド時に埋め込む:
#   cmake -DUTAWAVE_VERSION_URL="https://.../version.json" \
#         -DUTAWAVE_DOWNLOAD_PAGE_URL="https://.../download" ...
# 未指定時はソースのプレースホルダ (サーバー未稼働なら取得失敗 → 通知を出さない)。
# JSON 仕様は Docs/UPDATE_SETUP.md を参照。
set(UTAWAVE_VERSION_URL "" CACHE STRING "Update-check version JSON URL embedded at build time (official builds set this)")
set(UTAWAVE_DOWNLOAD_PAGE_URL "" CACHE STRING "Fallback download page URL when the version JSON has no url field")
if(UTAWAVE_VERSION_URL)
    target_compile_definitions(Utawave PRIVATE UTAWAVE_VERSION_URL="${UTAWAVE_VERSION_URL}")
    message(STATUS "Utawave: version check URL = ${UTAWAVE_VERSION_URL}")
endif()
if(UTAWAVE_DOWNLOAD_PAGE_URL)
    target_compile_definitions(Utawave PRIVATE UTAWAVE_DOWNLOAD_PAGE_URL="${UTAWAVE_DOWNLOAD_PAGE_URL}")
    message(STATUS "Utawave: download page URL = ${UTAWAVE_DOWNLOAD_PAGE_URL}")
endif()

# ── クラッシュレポート送信先 (任意・公式ビルドのみ) ──
# 指定すると、起動時にクラッシュログを検出した際「送信してもよいか」の同意ダイアログを出し、
# 同意時のみ JSON を POST する。未指定 (公開ソースの既定) は送信機能ごと無効で、
# ローカルのログ確認のみになる。詳細は Source/Project/CrashReporter.h。
set(UTAWAVE_CRASH_REPORT_URL "" CACHE STRING "Crash report POST endpoint embedded at build time (official builds set this)")
if(UTAWAVE_CRASH_REPORT_URL)
    target_compile_definitions(Utawave PRIVATE UTAWAVE_CRASH_REPORT_URL="${UTAWAVE_CRASH_REPORT_URL}")
    message(STATUS "Utawave: crash report URL = ${UTAWAVE_CRASH_REPORT_URL}")
endif()

# MSVC は既定で Shift-JIS としてソースを読むため u8 リテラル等が壊れる。
# /utf-8 でソース・実行ファイル両方を UTF-8 解釈に統一する。
# /wd4819 はコードページ警告を抑制。NOMINMAX は Windows.h の min/max マクロ衝突回避。
if(MSVC)
    target_compile_options(Utawave PRIVATE /utf-8 /wd4819)
    target_compile_definitions(Utawave PRIVATE NOMINMAX=1 _USE_MATH_DEFINES=1)
    # Release でも PDB を生成する (クラッシュログの「モジュール名 + RVA」を関数/行へオフライン
    # 解決するため)。Utawave.pdb は配布 zip に入れず、リリースごとに手元へ保管する
    # (手順は Docs/CRASH_REPORT_SETUP.md / RELEASE.md)。/OPT:REF /OPT:ICF は /DEBUG が
    # 無効化するリンカ最適化を Release 相当に戻す指定。
    target_compile_options(Utawave PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(Utawave PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

# ── Windows: ASIO SDK が置かれていれば自動的に ASIO サポートを有効化 ──
# Steinberg ASIO SDK は再配布制限があるため Git には含めず、
# 各自で https://www.steinberg.net/developers/ から取得し
# Source/ThirdParty/asiosdk/ に配置する。
# 配置されていない環境では WASAPI / DirectSound のみが有効になる。
set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/ThirdParty/asiosdk")
if(WIN32)
    if(EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
        message(STATUS "ASIO SDK found at ${ASIO_SDK_DIR} — ASIO サポート有効")
        target_compile_definitions(Utawave PRIVATE JUCE_ASIO=1)
        target_include_directories(Utawave SYSTEM PRIVATE
            "${ASIO_SDK_DIR}/common"
            "${ASIO_SDK_DIR}/host"
            "${ASIO_SDK_DIR}/host/pc"
        )
    else()
        message(STATUS "ASIO SDK not found at ${ASIO_SDK_DIR} — ASIO 無効 (WASAPI/DirectSound のみ)")
    endif()
endif()

# ── 内蔵 LAME (libmp3lame) を静的ライブラリとしてビルド ──
file(GLOB LAME_LIBMP3LAME_SOURCES Source/ThirdParty/lame/libmp3lame/*.c)
file(GLOB LAME_LIBMP3LAME_VECTOR  Source/ThirdParty/lame/libmp3lame/vector/*.c)
file(GLOB LAME_MPGLIB_SOURCES     Source/ThirdParty/lame/mpglib/*.c)
add_library(lame_static STATIC
    ${LAME_LIBMP3LAME_SOURCES}
    ${LAME_LIBMP3LAME_VECTOR}
    ${LAME_MPGLIB_SOURCES}
)
target_include_directories(lame_static PRIVATE
    Source/ThirdParty/lame
    Source/ThirdParty/lame/libmp3lame
    Source/ThirdParty/lame/mpglib
)
target_include_directories(lame_static PUBLIC
    Source/ThirdParty/lame/include
)
target_compile_definitions(lame_static PRIVATE
    HAVE_CONFIG_H=1
    LAME_LIBRARY_BUILD=1
)
if(NOT MSVC)
    target_compile_options(lame_static PRIVATE
        -Wno-everything                # サードパーティの警告は抑制
    )
else()
    target_compile_options(lame_static PRIVATE
        /wd4244 /wd4267 /wd4305 /wd4554 /wd4018 /wd4101 /wd4146 /wd4133 /wd4996
        /wd4819   # コードページ警告 (C4819) 抑制
        /utf-8    # ソースを UTF-8 として解釈
        # 静的ライブラリは /Z7 (オブジェクト埋め込み) でデバッグ情報を持たせ、
        # 本体リンク時の PDB に取り込む (クラッシュログの RVA 解決用)
        $<$<CONFIG:Release>:/Z7>
    )
endif()

target_link_libraries(Utawave PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    # LTO は無効化 (juce_recommended_lto_flags を外した)。有効だとメモリの少ない環境で
    # LTO リンクがハング/極端に遅くなるため。性能差は配布用途では軽微。
    juce::juce_recommended_warning_flags
    lame_static
)

# ── 使い方ドキュメント (HTML) をアプリに同梱 ──
# ヘルプメニュー「使い方ドキュメント...」が既定ブラウザで開く単一 HTML ファイル。
# macOS: .app/Contents/Resources/help.html に配置 (Xcode の Copy Bundle Resources)。
# Windows: 実行ファイル隣へ POST_BUILD でコピー。
# 言語別ヘルプ (ja=基底 help.html / 英・簡体・繁体・韓国)。showDocumentation() が現在の
# 言語のファイルを開き、無ければ help.html へフォールバックする。
set(UTAWAVE_HELP_DOCS
    "${CMAKE_CURRENT_SOURCE_DIR}/Docs/help.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/Docs/help.en.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/Docs/help.zh-Hans.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/Docs/help.zh-Hant.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/Docs/help.ko.html")
target_sources(Utawave PRIVATE ${UTAWAVE_HELP_DOCS})
set_source_files_properties(${UTAWAVE_HELP_DOCS} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
if(WIN32)
    foreach(_helpdoc ${UTAWAVE_HELP_DOCS})
        get_filename_component(_helpname "${_helpdoc}" NAME)
        add_custom_command(TARGET Utawave POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_helpdoc}" "$<TARGET_FILE_DIR:Utawave>/${_helpname}"
            VERBATIM)
    endforeach()
endif()

# ── ユニットテスト (コンソールアプリ UtawaveTests) ──
# 書き出し経路 (ExportEngine + AudioEngine + シーン構築) をオーディオデバイス無しで検証する。
# juce::UnitTest / UnitTestRunner ベース。`-DUTAWAVE_BUILD_TESTS=OFF` で無効化可能。
option(UTAWAVE_BUILD_TESTS "Build UtawaveTests unit tests" ON)
if(UTAWAVE_BUILD_TESTS)
    juce_add_console_app(UtawaveTests PRODUCT_NAME "UtawaveTests")
    juce_generate_juce_header(UtawaveTests)

    target_sources(UtawaveTests PRIVATE
        Tests/ExportEngineTests.cpp
        Tests/AudioEngineTests.cpp
        Tests/MidiIoTests.cpp
        Tests/BackupManagerTests.cpp
        Tests/AudioClipTests.cpp
        Tests/RecordingTests.cpp
        Tests/AppSettingsTests.cpp
        Tests/MidiClipTests.cpp
        Tests/SilenceDetectorTests.cpp
        Tests/ClipInsertGeometryTests.cpp
        Tests/RulerRangeMathTests.cpp
        Tests/EditActionsTests.cpp
        Tests/PluginActionsTests.cpp
        Tests/LufsMeterTests.cpp
        Tests/BpmDetectorTests.cpp
        Tests/ProjectManagerTests.cpp
        Tests/InternalSynthTests.cpp
        Tests/MeterDrawTests.cpp
        Tests/TrackManagerTests.cpp
        Tests/LocalisationTests.cpp
        Tests/AudioFileImporterTests.cpp
        Tests/AppStorageTests.cpp
        Tests/AdServiceTests.cpp
        Tests/UpdateCheckerTests.cpp
        Tests/CrashReporterTests.cpp
        Tests/PitchEngineTests.cpp
        Tests/BuiltInEffectsTests.cpp
        Tests/FileStreamVoiceTests.cpp
        Tests/AudioWorkerPoolTests.cpp
        Source/Audio/AudioFileImporter.cpp
        Source/Audio/FileStreamVoice.cpp
        Source/Audio/AudioWorkerPool.cpp
        Source/Audio/PitchEngine.cpp
        Source/Audio/pitchcore/PitchShiftEngine.cpp
        Source/Audio/builtin/BuiltInEQ.cpp
        Source/Audio/builtin/BuiltInCompressor.cpp
        Source/Audio/builtin/BuiltInDeEsser.cpp
        Source/Audio/builtin/BuiltInGate.cpp
        Source/Audio/builtin/BuiltInMaximizer.cpp
        Source/Audio/builtin/BuiltInDelay.cpp
        Source/Audio/builtin/BuiltInReverb.cpp
        Source/Audio/builtin/BuiltInFactory.cpp
        Source/UI/BuiltInEffectEditor.cpp
        Source/UI/ModernEffectEditor.cpp
        Source/UI/BuiltInEQEditor.cpp
        Source/UI/BuiltInCompressorEditor.cpp
        Source/UI/BuiltInDeEsserEditor.cpp
        Source/UI/BuiltInGateEditor.cpp
        Source/UI/BuiltInMaximizerEditor.cpp
        Source/UI/BuiltInDelayEditor.cpp
        Source/UI/BuiltInReverbEditor.cpp
        Source/Project/AdService.cpp
        Source/Project/UpdateChecker.cpp
        Source/Project/CrashReporter.cpp
        Source/Project/RecentProjects.cpp
        Source/Project/WindowState.cpp
        Source/Project/BackupManager.cpp
        Source/Project/ProjectManager.cpp
        Source/VST/PluginManager.cpp
        Source/VST/PluginScannerProcess.cpp
        Source/Export/ExportEngine.cpp
        Source/Export/Mp3EncoderWriter.cpp
        Source/Audio/AudioEngine.cpp
        Source/Audio/AudioDeviceSettings.cpp
        Source/Recording/RecordingManager.cpp
        Source/Tracks/Track.cpp
        Source/Tracks/AudioClip.cpp
        Source/Tracks/TrackManager.cpp
        Source/VST/PluginChain.cpp
        Source/MIDI/InternalSynth.cpp
        Source/MIDI/MidiImporter.cpp
        Source/MIDI/MidiExporter.cpp
        Source/Localisation.cpp
    )

    target_include_directories(UtawaveTests SYSTEM PRIVATE
        Source/ThirdParty/r8brain
    )

    # 本体と同じプラグインホスト定義に揃える (共有する JUCE モジュールの構成不一致を防ぐ)。
    target_compile_definitions(UtawaveTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=0
        JUCE_APPLICATION_NAME_STRING="UtawaveTests"
        JUCE_APPLICATION_VERSION_STRING="0.1.0"
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=0
    )

    if(MSVC)
        target_compile_options(UtawaveTests PRIVATE /utf-8 /wd4819)
        target_compile_definitions(UtawaveTests PRIVATE NOMINMAX=1 _USE_MATH_DEFINES=1)
    endif()

    target_link_libraries(UtawaveTests PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        lame_static
    )

    # ── キー変更 CLI ツール (開発用) ──
    # 自社製ピッチシフト (pitchcore) で単発変換する。品質チューニング時の聴き比べ素材
    # 生成と、ライブラリ単体配布時の使用例を兼ねる。
    # 使い方: UtawavePitchTool <audiofile> <semitones> [outDir]
    juce_add_console_app(UtawavePitchTool PRODUCT_NAME "UtawavePitchTool")
    juce_generate_juce_header(UtawavePitchTool)
    target_sources(UtawavePitchTool PRIVATE
        Tests/PitchToolMain.cpp
        Source/Audio/PitchEngine.cpp
        Source/Audio/pitchcore/PitchShiftEngine.cpp
    )
    target_include_directories(UtawavePitchTool SYSTEM PRIVATE
        Source/ThirdParty/r8brain
    )
    target_compile_definitions(UtawavePitchTool PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_APPLICATION_NAME_STRING="UtawavePitchTool"
        JUCE_APPLICATION_VERSION_STRING="0.1.0"
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=0
    )
    if(MSVC)
        target_compile_options(UtawavePitchTool PRIVATE /utf-8 /wd4819)
        target_compile_definitions(UtawavePitchTool PRIVATE NOMINMAX=1 _USE_MATH_DEFINES=1)
    endif()
    target_link_libraries(UtawavePitchTool PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_events
        juce::juce_recommended_config_flags
    )
endif()
