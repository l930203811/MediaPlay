#include "VisualizerComponent.h"

//==============================================================================
VisualizerComponent::VisualizerComponent (MediaPlayAudioProcessor& p)
    : processor (p)
{
    setOpaque (true);
    buildBubbles();
    std::fill (barLevels.begin(), barLevels.end(), 0.0f);
    std::fill (barPeaks.begin(), barPeaks.end(), 0.0f);
}

void VisualizerComponent::buildBubbles()
{
    juce::Random rng (20261105);
    bubbles.clear();
    for (int i = 0; i < 40; ++i)
    {
        Bubble b;
        b.x = rng.nextFloat();
        b.y = rng.nextFloat();
        b.speed = 0.5f + rng.nextFloat() * 1.5f;       // 上升速度（px/帧）
        b.r = 1.5f + rng.nextFloat() * 3.5f;           // 半径
        b.alpha = 0.1f + rng.nextFloat() * 0.4f;       // 0.1..0.5
        b.phase = rng.nextFloat() * juce::MathConstants<float>::twoPi;
        bubbles.push_back (b);
    }
}

//==============================================================================
void VisualizerComponent::timerTicked()
{
    timeAccum += 1.0 / 30.0;

    for (int i = 0; i < MediaPlayAudioProcessor::numBars; ++i)
    {
        float v = processor.getBar (i);
        barLevels[(size_t) i] = barLevels[(size_t) i] * 0.55f + v * 0.45f;   // 平滑
        float peak = barPeaks[(size_t) i] - 0.008f;                          // 峰值缓降
        barPeaks[(size_t) i] = juce::jmax (v, peak);
    }

    auto ease = [] (float cur, float target) { return cur + (target - cur) * 0.35f; };
    lowSmooth  = ease (lowSmooth,  processor.getLowEnergy());
    midSmooth  = ease (midSmooth,  processor.getMidEnergy());
    highSmooth = ease (highSmooth, processor.getHighEnergy());

    // 气泡上升，出顶后从底部重生（ZenFile 逻辑）
    const float hPix = (float) getHeight();
    for (auto& b : bubbles)
    {
        b.y -= b.speed / juce::jmax (1.0f, hPix);
        if (b.y < -0.02f)
        {
            b.y = 1.02f;
            b.x = juce::Random::getSystemRandom().nextFloat();
        }
    }

    repaint();
}

void VisualizerComponent::setLevels (const float* newBars, int n, float low, float mid, float high)
{
    for (int i = 0; i < MediaPlayAudioProcessor::numBars && i < n; ++i)
        barLevels[(size_t) i] = newBars[i];
    lowSmooth = low; midSmooth = mid; highSmooth = high;
}

//==============================================================================
void VisualizerComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const float w = bounds.getWidth();
    const float h = bounds.getHeight();

    // ---------- 深青渐变背景（ZenFile 播放页底色） ----------
    juce::ColourGradient bg (juce::Colour (0xff0c2d3a), 0.0f, 0.0f,
                             juce::Colour (0xff06161f), 0.0f, h, false);
    bg.addColour (0.55, juce::Colour (0xff092230));
    g.setGradientFill (bg);
    g.fillRect (bounds);

    // ---------- 上升气泡（accent 蓝，随时间水平微摆 + 高光点） ----------
    for (const auto& b : bubbles)
    {
        const float bx = (b.x + 0.012f * std::sin ((float) timeAccum * 0.9f + b.phase)) * w;
        const float by = b.y * h;
        g.setColour (juce::Colour (0xff29b6f6).withAlpha (b.alpha * 0.85f));
        g.fillEllipse (bx - b.r, by - b.r, b.r * 2.0f, b.r * 2.0f);
        g.setColour (juce::Colours::white.withAlpha (b.alpha * 0.30f));
        g.fillEllipse (bx - b.r * 0.35f, by - b.r * 0.5f, b.r * 0.7f, b.r * 0.7f);
    }

    // ---------- LED 频谱柱 + 倒影（基线其下为倒影区） ----------
    const float baseline = h - 96.0f;
    const float barTop = h * 0.34f;
    drawLedBars (g, w, barTop, baseline);

    // 倒影：绕基线垂直镜像重画，再叠深色遮罩压到 ~25% 可见度（对齐 ZenFile reflex）
    const float reflectH = h - baseline;
    if (reflectH > 4.0f)
    {
        g.saveState();
        g.reduceClipRegion (juce::Rectangle<int> (0, (int) baseline + 1, (int) w, (int) reflectH - 2));
        g.addTransform (juce::AffineTransform::scale (1.0f, -1.0f).translated (0.0f, 2.0f * baseline));
        drawLedBars (g, w, barTop, baseline);
        g.restoreState();
        g.setColour (juce::Colour (0xff082531).withAlpha (0.75f));
        g.fillRect (0.0f, baseline + 1.0f, w, reflectH - 2.0f);
    }

    // ---------- 基线（进度条同款 accent 细线） ----------
    g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.55f));
    g.fillRect (0.0f, baseline, w, 2.0f);
}

// LED 分段频谱：5px 节距（段高 3.5 + 缝 1.5），画布级垂直渐变
// 底绿 0xFF33D433 → 0.6 黄 0xFFEED500 → 顶红 0xFFE4390E，峰值帽 2px（对齐 ZenFile）
void VisualizerComponent::drawLedBars (juce::Graphics& g, float w, float barTop, float baseline)
{
    const int nBars = MediaPlayAudioProcessor::numBars;
    const float unit = w / (float) nBars;
    const float barW = unit * 0.9f;
    const float barInset = (unit - barW) * 0.5f;
    const float maxBar = baseline - barTop;
    if (maxBar < 12.0f) return;

    // 高度渐变着色器：颜色只随 y 变化，所有柱共享「底绿顶红」
    juce::ColourGradient vu (juce::Colour (0xff33d433), 0.0f, baseline,
                             juce::Colour (0xffe4390e), 0.0f, barTop, false);
    vu.addColour (0.6, juce::Colour (0xffeed500));
    g.setGradientFill (vu);

    const float segPitch = 5.0f;
    const float segHeight = 3.5f;

    for (int i = 0; i < nBars; ++i)
    {
        const float x = (float) i * unit + barInset;
        const float hgt = juce::jlimit (0.0f, 1.0f, barLevels[(size_t) i]) * maxBar;

        // 点亮的 LED 段：自底向上堆叠
        if (hgt >= 1.0f)
        {
            float yLo = 0.0f;
            while (yLo + 0.5f < hgt)
            {
                g.fillRect (x, baseline - yLo - segHeight, barW, segHeight);
                yLo += segPitch;
            }
        }

        // 峰值帽：峰值高度上方 2px 横条
        const float peakH = juce::jlimit (0.0f, 1.0f, barPeaks[(size_t) i]) * maxBar;
        if (peakH >= 1.0f)
            g.fillRect (x, baseline - peakH - 2.0f, barW, 2.0f);
    }
}
