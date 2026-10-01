cmake_minimum_required(VERSION 3.22)
project(SimpleRecPro VERSION 1.0.0)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(SIMPLE_REC_FFMPEG_EXECUTABLE "" CACHE FILEPATH
    "Path to the redistributable FFmpeg executable used for MP3 export")
if(WIN32 AND NOT EXISTS "${SIMPLE_REC_FFMPEG_EXECUTABLE}")
    find_program(SIMPLE_REC_FFMPEG_DISCOVERED NAMES ffmpeg.exe ffmpeg)
    if(SIMPLE_REC_FFMPEG_DISCOVERED)
        set(SIMPLE_REC_FFMPEG_EXECUTABLE
            "${SIMPLE_REC_FFMPEG_DISCOVERED}" CACHE FILEPATH
            "Path to the redistributable FFmpeg executable used for MP3 export"
            FORCE)
    endif()
endif()
if(WIN32 AND NOT EXISTS "${SIMPLE_REC_FFMPEG_EXECUTABLE}")
    message(FATAL_ERROR
        "FFmpeg is required for the promised MP3 export. Set SIMPLE_REC_FFMPEG_EXECUTABLE.")
endif()

include(FetchContent)

# Dependencies are downloaded into the build directory. For an offline build,
# use CMake's explicit FETCHCONTENT_SOURCE_DIR_<NAME> overrides.
FetchContent_Declare(
    juce
    URL https://codeload.github.com/juce-framework/JUCE/tar.gz/7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2
    URL_HASH SHA256=488c4f6e2c121316d60801842dba87c44e5dbad4e661cb49bf806b49a38b6a5e)
FetchContent_MakeAvailable(juce)

# ARA lets editor-style plug-ins such as PitchNet read complete track clips
# instead of receiving only the small VST3 blocks that are currently playing.
FetchContent_Declare(
    ara_sdk
    GIT_REPOSITORY https://github.com/Celemony/ARA_SDK.git
    GIT_TAG e87afbd2dd2693937bc9795b6b5ae87460fa8c1f
    GIT_SUBMODULES ARA_API ARA_Library
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(ara_sdk)
juce_set_ara_sdk_path("${ara_sdk_SOURCE_DIR}")

# 音程を保ったままの速度変更と、独立したキー変更に使う。
FetchContent_Declare(
    signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 44c8f865af9da8c29cc4a70a2d5a3ec83639c711)
FetchContent_MakeAvailable(signalsmith_stretch)
if(NOT TARGET signalsmith-stretch)
    add_library(signalsmith-stretch INTERFACE)
    target_include_directories(signalsmith-stretch INTERFACE
        "${signalsmith_stretch_SOURCE_DIR}")
endif()

# JUCEアプリケーションの定義
juce_add_gui_app(SimpleRecPro
    PRODUCT_NAME "SimpleRec Pro"
    VERSION 1.0.0
    MICROPHONE_PERMISSION_ENABLED TRUE)

# JuceHeader.h を生成する
juce_generate_juce_header(SimpleRecPro)

# ソースファイルの追加
target_sources(SimpleRecPro PRIVATE
    Source/Main.cpp
    Source/MainComponent.h
    Source/MainComponent.cpp
    Source/AssistantBridge.cpp
    Source/AssistantController.cpp
    Source/ComparisonBarComponent.cpp
    Source/RegionAutomation.cpp
    Source/HeaderComponent.h
    Source/HeaderComponent.cpp
    Source/TransportBarComponent.h
    Source/TransportBarComponent.cpp
    Source/TrackAreaComponent.h
    Source/TrackAreaComponent.cpp
    Source/AudioEngine.h
    Source/AudioEngine.cpp
    Source/WindowsMp3Encoder.h
    Source/WindowsMp3Encoder.cpp
    Source/AraTimelineAudio.h
    Source/AraTimelineAudio.cpp
    Source/AraTrackHost.h
    Source/AraTrackHost.cpp
    Source/PitchNetArchiveFilter.h
    Source/PitchNetArchiveFilter.cpp
    Source/PlaybackTransform.h
    Source/PlaybackTransform.cpp
    Source/DawFeatureTypes.h
    Source/ClipRenderMath.h
    Source/ClipRenderMath.cpp
    Source/RhythmWarp.h
    Source/RhythmWarp.cpp
    Source/RhythmRender.h
    Source/RhythmRender.cpp
    Source/SimpleMixProcessor.h
    Source/SimpleMixProcessor.cpp
    Source/SpecialFxProcessor.h
    Source/SpecialFxProcessor.cpp
    Source/AutoMixAnalyzer.h
    Source/AutoMixAnalyzer.cpp
    Source/ReferenceMixAnalyzer.h
    Source/ReferenceMixAnalyzer.cpp
    Source/MasterBusProcessor.h
    Source/MasterBusProcessor.cpp
    Source/TempoEstimator.h
    Source/TempoEstimator.cpp
    Source/TempoMap.h
    Source/TempoMap.cpp
    Source/Vst3PluginCatalog.h
    Source/Vst3PluginCatalog.cpp
    Source/Vst3BrowserComponent.h
    Source/Vst3BrowserComponent.cpp
    Source/ExportDialogComponent.h
    Source/ExportDialogComponent.cpp
    Source/StudioPanelComponent.h
    Source/StudioPanelComponent.cpp
    Source/LatencyCalibration.h
    Source/LatencyCalibration.cpp
    Source/MetronomeEngine.h
    Source/MetronomeEngine.cpp
    Source/TrackData.h
    Source/ProjectSerializer.h
    Source/ProjectSerializer.cpp)

# モジュールのリンク (ASIO対応のため audio_devices を含める)
target_link_libraries(SimpleRecPro PRIVATE
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    signalsmith-stretch)

if(WIN32 AND EXISTS "${SIMPLE_REC_FFMPEG_EXECUTABLE}")
    get_filename_component(SIMPLE_REC_FFMPEG_BIN_DIR
        "${SIMPLE_REC_FFMPEG_EXECUTABLE}" DIRECTORY)
    get_filename_component(SIMPLE_REC_FFMPEG_ROOT_DIR
        "${SIMPLE_REC_FFMPEG_BIN_DIR}" DIRECTORY)
    add_custom_command(TARGET SimpleRecPro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SIMPLE_REC_FFMPEG_EXECUTABLE}"
            "$<TARGET_FILE_DIR:SimpleRecPro>/ffmpeg.exe")
    if(EXISTS "${SIMPLE_REC_FFMPEG_ROOT_DIR}/LICENSE")
        add_custom_command(TARGET SimpleRecPro POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SIMPLE_REC_FFMPEG_ROOT_DIR}/LICENSE"
                "$<TARGET_FILE_DIR:SimpleRecPro>/LICENSE-FFmpeg.txt")
    endif()
    if(EXISTS "${SIMPLE_REC_FFMPEG_ROOT_DIR}/README.txt")
        add_custom_command(TARGET SimpleRecPro POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SIMPLE_REC_FFMPEG_ROOT_DIR}/README.txt"
                "$<TARGET_FILE_DIR:SimpleRecPro>/README-FFmpeg.txt")
    endif()
endif()

# ASIO is an explicit opt-in; it must not depend on a developer-specific path.
option(SIMPLE_REC_ENABLE_ASIO "Enable ASIO after reviewing its SDK licence" OFF)
if(SIMPLE_REC_ENABLE_ASIO AND NOT WIN32)
    message(FATAL_ERROR "ASIO is supported only by the Windows build.")
endif()
set(USE_ASIO $<BOOL:${SIMPLE_REC_ENABLE_ASIO}>)

# ASIOを有効にするコンパイル定義
target_compile_definitions(SimpleRecPro PUBLIC
    JUCE_ASIO=${USE_ASIO}
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_ARA=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

if(MSVC)
    target_compile_options(SimpleRecPro PRIVATE /utf-8 /EHsc)
endif()

include(CTest)
if(BUILD_TESTING)
    juce_add_console_app(SimpleRecProTests
        PRODUCT_NAME "SimpleRec Pro Tests")
    juce_generate_juce_header(SimpleRecProTests)

    target_sources(SimpleRecProTests PRIVATE
        Tests/AudioEngineTests.cpp
        Tests/AssistantControllerTests.cpp
        Tests/RegionAutomationTests.cpp
        Tests/AssistantIntegrationHost.cpp
        Source/AssistantBridge.cpp
        Source/AssistantController.cpp
        Source/ComparisonBarComponent.cpp
        Source/RegionAutomation.cpp
        Tests/AraTimelineAudioTests.cpp
        Tests/LatencyCalibrationTests.cpp
        Tests/PitchNetArchiveFilterTests.cpp
        Tests/DawFeatureTests.cpp
        Tests/UiLayoutTests.cpp
        Source/HeaderComponent.cpp
        Source/TransportBarComponent.cpp
        Source/TrackAreaComponent.cpp
        Source/StudioPanelComponent.cpp
        Source/AudioEngine.h
        Source/AudioEngine.cpp
        Source/WindowsMp3Encoder.h
        Source/WindowsMp3Encoder.cpp
        Source/AraTimelineAudio.h
        Source/AraTimelineAudio.cpp
        Source/AraTrackHost.h
        Source/AraTrackHost.cpp
        Source/PitchNetArchiveFilter.h
        Source/PitchNetArchiveFilter.cpp
        Source/PlaybackTransform.h
        Source/PlaybackTransform.cpp
        Source/DawFeatureTypes.h
        Source/ClipRenderMath.h
        Source/ClipRenderMath.cpp
        Source/RhythmWarp.h
        Source/RhythmWarp.cpp
        Source/RhythmRender.h
        Source/RhythmRender.cpp
        Source/SimpleMixProcessor.h
        Source/SimpleMixProcessor.cpp
        Source/SpecialFxProcessor.h
        Source/SpecialFxProcessor.cpp
        Source/AutoMixAnalyzer.h
        Source/AutoMixAnalyzer.cpp
        Source/ReferenceMixAnalyzer.h
        Source/ReferenceMixAnalyzer.cpp
        Source/MasterBusProcessor.h
        Source/MasterBusProcessor.cpp
        Source/TempoEstimator.h
        Source/TempoEstimator.cpp
        Source/TempoMap.h
        Source/TempoMap.cpp
        Source/Vst3PluginCatalog.h
        Source/Vst3PluginCatalog.cpp
        Source/LatencyCalibration.h
        Source/LatencyCalibration.cpp
        Source/MetronomeEngine.h
        Source/MetronomeEngine.cpp
        Source/ProjectSerializer.h
        Source/ProjectSerializer.cpp
        Source/TrackData.h)

    target_link_libraries(SimpleRecProTests PRIVATE
        juce::juce_audio_utils
        signalsmith-stretch)

    if(WIN32 AND EXISTS "${SIMPLE_REC_FFMPEG_EXECUTABLE}")
        add_custom_command(TARGET SimpleRecProTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SIMPLE_REC_FFMPEG_EXECUTABLE}"
                "$<TARGET_FILE_DIR:SimpleRecProTests>/ffmpeg.exe")
    endif()

    target_compile_definitions(SimpleRecProTests PRIVATE
        JUCE_ASIO=${USE_ASIO}
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_ARA=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1)

    if(MSVC)
        target_compile_options(SimpleRecProTests PRIVATE /utf-8 /EHsc)
        target_link_options(SimpleRecProTests PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/SimpleRecProTests.map")
    endif()

    add_test(NAME AudioEngineDeterministicTests COMMAND SimpleRecProTests)
endif()
