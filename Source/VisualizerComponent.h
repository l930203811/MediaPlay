#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

// ZenFile 风格可视化：深青渐变背景 + 上升气泡 + LED 分段频谱柱（底绿→黄→顶红，
// 峰值帽 + 基线倒影），对齐 ZenFile 播放器 LED VU 表样式
class VisualizerComponent : public juce::Component
{
public:
    explicit VisualizerComponent (MediaPlayAudioProcessor& p);
    ~VisualizerComponent() override = default;

    void paint (juce::Graphics&) override;
    void resized() override {}
    void timerTicked();   // 由编辑器 Timer 调用：拉取频谱数据并平滑、推进气泡

    void setLevels (const float* newBars, int n, float low, float mid, float high);

private:
    MediaPlayAudioProcessor& processor;

    std::array<float, MediaPlayAudioProcessor::numBars> barLevels {};
    std::array<float, MediaPlayAudioProcessor::numBars> barPeaks {};

    float lowSmooth = 0.0f, midSmooth = 0.0f, highSmooth = 0.0f;
    double timeAccum = 0.0;

    // 上升气泡（ZenFile audio_particles_widget：40 个，恒速上升 + 水平微摆）
    struct Bubble { float x, y, r, speed, alpha, phase; };
    std::vector<Bubble> bubbles;
    void buildBubbles();

    void drawLedBars (juce::Graphics& g, float w, float barTop, float baseline);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualizerComponent)
};
