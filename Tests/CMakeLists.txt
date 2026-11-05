cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 17)
project(EngineTest VERSION 1.0.0 LANGUAGES C CXX)

add_subdirectory(../ThirdParty/juce-8.0.15 juce)

juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest" COMPANY_NAME "SKCore")
target_sources(EngineTest PRIVATE EngineTest.cpp)
target_compile_definitions(EngineTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1
)
target_compile_options(EngineTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(EngineTest PRIVATE
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
)
if (WIN32)
    target_link_libraries(EngineTest PRIVATE mfplat mfreadwrite mfuuid Ole32)
endif()
