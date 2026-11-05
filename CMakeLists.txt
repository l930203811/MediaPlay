cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 17)
project(VstMediaPlayer VERSION 1.0.0 LANGUAGES C CXX)

add_subdirectory(ThirdParty/juce-8.0.15)

juce_add_plugin(VstMediaPlayer
    PRODUCT_NAME "MediaPlay"
    COMPANY_NAME "SKCore"
    BUNDLE_ID "com.skcore.mediaplay"
    PLUGIN_MANUFACTURER_CODE Skcd
    PLUGIN_CODE MdPl
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Fx
)

target_sources(VstMediaPlayer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/VisualizerComponent.cpp
    Source/Lyrics.h
)

juce_generate_juce_header(VstMediaPlayer)

# 源码为 UTF-8（含中文界面文本），强制 MSVC 按 UTF-8 解析
target_compile_options(VstMediaPlayer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(VstMediaPlayer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_USE_MP3AUDIOFORMAT=1
)

target_link_libraries(VstMediaPlayer PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
)

# Windows Media Foundation 解码（MP4/M4A/AAC/WMA 兜底）
if (WIN32)
    target_link_libraries(VstMediaPlayer PRIVATE mfplat mfuuid mfreadwrite)
endif()
