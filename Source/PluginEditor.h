#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "VisualizerComponent.h"

//==============================================================================
// 胶囊按钮（侧边栏样式，参考截图）
class PillButton : public juce::Button
{
public:
    PillButton (const juce::String& text, bool toggling = false)
        : juce::Button (text)
    {
        setClickingTogglesState (toggling);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();

        juce::Colour bg = on ? juce::Colour (0xff123a4d)
                             : (over ? juce::Colour (0xff164457) : juce::Colour (0xff10303e));
        if (down) bg = bg.brighter (0.12f);

        g.setColour (bg);
        g.fillRoundedRectangle (b, 6.0f);

        if (on)
        {
            g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.9f));
            g.drawRoundedRectangle (b.reduced (1.0f), 6.0f, 1.2f);
        }

        g.setColour (on ? juce::Colours::white : juce::Colour (0xffcfe3ee));
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 14.0f, juce::Font::plain)));
        g.drawText (getButtonText(), b, juce::Justification::centred);
    }
};

//==============================================================================
// 传输图标按钮（▶ / ⏸ / ⏮ / ⏭），主按钮为青色圆底
class IconButton : public juce::Button
{
public:
    enum Icon { Play, Pause, Prev, Next };

    IconButton (Icon ic, bool mainButton = false)
        : juce::Button ("icon"), icon (ic), main (mainButton) {}

    void setIcon (Icon ic)
    {
        if (icon != ic) { icon = ic; repaint(); }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat();
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float d = juce::jmin (b.getWidth(), b.getHeight());
        const float s = d * 0.34f;   // 图标半径

        if (main)
        {
            auto bg = juce::Colour (0xff29b6f6);
            if (down) bg = bg.darker (0.15f);
            else if (over) bg = bg.brighter (0.12f);
            g.setColour (bg);
            g.fillEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d);
        }
        else
        {
            if (over || down)
            {
                g.setColour (juce::Colour (0x2effffff));
                g.fillEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d);
            }
        }

        juce::Path p;
        switch (icon)
        {
            case Play:
                p.addTriangle (cx - s * 0.7f, cy - s, cx - s * 0.7f, cy + s, cx + s, cy);
                break;
            case Pause:
                p.addRoundedRectangle (cx - s * 0.75f, cy - s, s * 0.5f, s * 2.0f, 1.5f);
                p.addRoundedRectangle (cx + s * 0.25f, cy - s, s * 0.5f, s * 2.0f, 1.5f);
                break;
            case Prev:
                p.addRoundedRectangle (cx - s * 0.8f, cy - s, s * 0.32f, s * 2.0f, 1.0f);
                p.addTriangle (cx + s * 0.8f, cy - s, cx + s * 0.8f, cy + s, cx - s * 0.35f, cy);
                break;
            case Next:
                p.addTriangle (cx - s * 0.8f, cy - s, cx - s * 0.8f, cy + s, cx + s * 0.35f, cy);
                p.addRoundedRectangle (cx + s * 0.48f, cy - s, s * 0.32f, s * 2.0f, 1.0f);
                break;
        }

        g.setColour (main ? juce::Colour (0xff082430) : juce::Colour (0xfff0ecff));
        g.fillPath (p);
    }

private:
    Icon icon;
    bool main;
};

//==============================================================================
// 侧边栏曲目列表（当前曲目高亮，点击切歌）
class PlaylistComponent : public juce::ListBox, private juce::ListBoxModel
{
public:
    std::function<void (int)> onTrackChosen;

    explicit PlaylistComponent (MediaPlayAudioProcessor& p) : proc (p)
    {
        setModel (this);
        setRowHeight (26);
        setMultipleSelectionEnabled (false);
        setClickingTogglesRowSelection (false);
        setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        setOpaque (false);
        updateContent();
    }

    int getNumRows() override { return proc.getPlaylistSize(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        const bool cur = (row == proc.getCurrentTrackIndex());
        if (cur)
        {
            g.setColour (juce::Colour (0xff123a4d));
            g.fillRoundedRectangle (2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 5.0f);
        }
        g.setColour (cur ? juce::Colour (0xff29b6f6) : juce::Colour (0xffcfe3ee));
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::plain)));
        g.drawText (juce::String (row + 1) + ". " + proc.getPlaylistItem (row),
                    juce::Rectangle<int> (10, 0, w - 16, h),
                    juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        // 单击只负责选中；右键弹出菜单
        if (e.mods.isPopupMenu())
            showContextMenu (row);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && onTrackChosen)
            onTrackChosen (row);
    }

    // 列表空白处右键：只提供「清空列表」
    void backgroundClicked (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showContextMenu (-1);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff0c2b39));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f);
        ListBox::paint (g);
        if (getNumRows() == 0)
        {
            g.setColour (juce::Colour (0xff5f7f92));
            g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::plain)));
            g.drawText (MediaPlayAudioProcessor::zh ("播放列表为空"),
                        getLocalBounds(), juce::Justification::centred);
        }
    }

private:
    void showContextMenu (int row)
    {
        juce::PopupMenu menu;
        if (row >= 0)
            menu.addItem (1, MediaPlayAudioProcessor::zh ("删除选中"), true, false);
        menu.addItem (2, MediaPlayAudioProcessor::zh ("清空列表"), true, false);

        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (*this)
                                .withMousePosition()
                                .withDeletionCheck (*this),
                            [this, row] (int result)
                            {
                                if (result == 1 && row >= 0) proc.removeTrack (row);
                                else if (result == 2)        proc.clearPlaylist();
                                updateContent();
                                deselectAllRows();
                                repaint();
                            });
    }

    MediaPlayAudioProcessor& proc;
};

//==============================================================================
// 播放器滑条外观（细轨 + 圆点）
class PlayerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider&) override
    {
        auto trackColour = juce::Colour (0xff2a3b46);
        auto fillColour = juce::Colour (0xff29b6f6);

        if (style == juce::Slider::LinearVertical)
        {
            const float cx = (float) x + (float) width * 0.5f;
            const float trackW = 6.0f;

            g.setColour (trackColour);
            g.fillRoundedRectangle (cx - trackW * 0.5f, (float) y, trackW, (float) height, 3.0f);

            const float fillH = (float) height - sliderPos;
            g.setColour (fillColour.withAlpha (0.9f));
            g.fillRoundedRectangle (cx - trackW * 0.5f, (float) y + sliderPos, trackW, fillH, 3.0f);

            g.setColour (juce::Colours::white);
            g.fillEllipse (cx - 7.0f, (float) y + sliderPos - 7.0f, 14.0f, 14.0f);
        }
        else // LinearHorizontal（进度条 / 音量条）
        {
            const float cy = (float) y + (float) height * 0.5f;
            const float trackH = 4.0f;

            g.setColour (trackColour);
            g.fillRoundedRectangle ((float) x, cy - trackH * 0.5f, (float) width, trackH, 2.0f);

            const float fillW = sliderPos - (float) x;
            g.setColour (juce::Colour (0xff29b6f6));
            g.fillRoundedRectangle ((float) x, cy - trackH * 0.5f, fillW, trackH, 2.0f);

            g.setColour (juce::Colours::white);
            g.fillEllipse (sliderPos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
        }
    }
};

//==============================================================================
// KTV 歌词浮层：双行显示（当前行稍靠左带渐染，下一行稍靠右）
// 有逐字时间轴按字渐染，否则整行按时间比例渐染
class LyricsOverlay : public juce::Component
{
public:
    explicit LyricsOverlay (MediaPlayAudioProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        auto& lyr = proc.getLyrics();
        if (! lyr.isLoaded()) return;

        const double t = proc.getCurrentPosition();
        const int cur = lyr.lineIndexAt (t);

        auto bounds = getLocalBounds().toFloat();
        const float w = bounds.getWidth(), h = bounds.getHeight();
        const auto row1 = juce::Rectangle<float> (0.0f, h * 0.02f, w, h * 0.44f);
        const auto row2 = juce::Rectangle<float> (0.0f, h * 0.58f, w, h * 0.40f);
        const float cx1 = w * 0.42f;   // 第一行更靠左
        const float cx2 = w * 0.63f;   // 第二行更靠右

        if (cur >= 0)
        {
            drawKtvLine (g, lyr.line (cur), t, row1, cx1, true);
            drawKtvLine (g, lyr.line (cur + 1), t, row2, cx2, false);
        }
        else if (lyr.getLineCount() > 0)
        {
            drawKtvLine (g, lyr.line (0), t, row2, cx2, false);   // 尚未开唱，先在下行预告
        }
    }

private:
    static void drawKtvLine (juce::Graphics& g, const LyricLine* L, double t,
                             juce::Rectangle<float> area, float centreX, bool current)
    {
        if (L == nullptr || L->text.trim().isEmpty()) return;

        const float baseH = current ? 48.0f : 36.0f;
        juce::Font font (juce::FontOptions ("Microsoft YaHei UI", baseH, juce::Font::bold));
        float textW = juce::GlyphArrangement::getStringWidth (font, L->text);
        // 限制宽度：不超过区域，也不越过自身中心到左右边缘（错位排布时不越界/不重叠）
        const float maxW = juce::jmin (area.getWidth() * 0.94f,
                                       2.0f * juce::jmin (centreX, area.getWidth() - centreX) * 0.96f);
        if (textW > maxW)
        {
            font = font.withHeight (baseH * maxW / textW);
            textW = maxW;
        }

        const float textX = centreX - textW * 0.5f;
        const auto textRect = juce::Rectangle<float> (textX, area.getY(), textW + 4.0f, area.getHeight());

        // 深色衬底：把歌词和频谱/山峦隔开，保证任何背景下都醒目
        const auto backing = textRect.expanded (16.0f, 8.0f);
        g.setColour (juce::Colour (0xa8061a24));
        g.fillRoundedRectangle (backing, (float) backing.getHeight() * 0.30f);

        g.setFont (font);
        // 阴影提升可读性
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawText (L->text, textRect.translated (2.0f, 2.0f), juce::Justification::centredLeft, false);
        // 未唱底色（黄色）
        g.setColour (current ? juce::Colour (0xff8bc34a) : juce::Colour (0xd9ffffff));
        g.drawText (L->text, textRect, juce::Justification::centredLeft, false);

        // 已唱渐染（亮黄，经典 KTV 色，与暗衬底强对比）
        float filled = 0.0f;
        if (current)
        {
            if (! L->words.empty())
            {
                for (const auto& wd : L->words)
                {
                    if (t >= wd.t1)
                        filled += juce::GlyphArrangement::getStringWidth (font, wd.text);
                    else if (t > wd.t0)
                    {
                        const float f = (float) ((t - wd.t0) / juce::jmax (0.05, wd.t1 - wd.t0));
                        filled += juce::GlyphArrangement::getStringWidth (font, wd.text) * juce::jlimit (0.0f, 1.0f, f);
                        break;
                    }
                    else break;
                }
            }
            else if (L->t1 > L->t0)
            {
                filled = textW * (float) juce::jlimit (0.0, 1.0, (t - L->t0) / (L->t1 - L->t0));
            }
            filled = juce::jlimit (0.0f, textW, filled);
        }

        if (filled > 0.5f)
        {
            g.saveState();
            g.reduceClipRegion (juce::Rectangle<float> (textX - 2.0f, area.getY() - 4.0f,
                                                        filled + 2.0f, area.getHeight() + 8.0f)
                                    .toNearestInt());
            g.setColour (juce::Colour (0xff4fc3f7));   // 已唱天蓝色
            g.drawText (L->text, textRect, juce::Justification::centredLeft, false);
            g.restoreState();
        }
    }

    MediaPlayAudioProcessor& proc;
};

//==============================================================================
// 歌词偏移参考线弹窗：点击「偏移」按钮后在其上方弹出
// 垂直参考线，中点=默认歌词时间(0s)；把手向上拖=歌词提前，向下拖=歌词延后，±10s
class LyricOffsetPopup : public juce::Component
{
public:
    std::function<void (double)> onOffsetChanged;   // 拖动时回调（秒，正=推迟）
    std::function<void()> onCloseRequest;           // 点击弹窗外区域请求关闭

    explicit LyricOffsetPopup (MediaPlayAudioProcessor& p) : proc (p)
    {
        setAlwaysOnTop (true);
    }

    // 非拖动状态下同步显示当前歌曲的偏移（切歌后调用）
    void syncFromProcessor()
    {
        if (! dragging) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float w = b.getWidth(), h = b.getHeight();

        // 深色面板 + accent 描边
        g.setColour (juce::Colour (0xf2061a24));
        g.fillRoundedRectangle (b, 10.0f);
        g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.55f));
        g.drawRoundedRectangle (b.reduced (0.5f), 10.0f, 1.0f);

        // 标题
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 13.0f, juce::Font::bold)));
        g.drawText (MediaPlayAudioProcessor::zh ("歌词偏移"),
                    juce::Rectangle<float> (0, 6, w, 18), juce::Justification::centred);

        const auto tr = trackRect();
        const float cx = tr.getCentreX();

        // 上/下方向提示
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.5f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff8bc34a));
        g.drawText (MediaPlayAudioProcessor::zh ("提前"),
                    juce::Rectangle<float> (cx + 12.0f, tr.getY() - 2.0f, w - cx - 12.0f, 14.0f),
                    juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xffffd54f));
        g.drawText (MediaPlayAudioProcessor::zh ("延后"),
                    juce::Rectangle<float> (cx + 12.0f, tr.getBottom() - 12.0f, w - cx - 12.0f, 14.0f),
                    juce::Justification::centredLeft);

        // 轨道
        g.setColour (juce::Colour (0xff2a3b46));
        g.fillRoundedRectangle (tr, 3.0f);

        // 刻度：0 / ±2 / ±4 / ±6 / ±8 / ±10
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 9.0f, juce::Font::plain)));
        for (int s = -10; s <= 10; s += 2)
        {
            const float fy = yForSeconds ((double) s);
            const bool zero = (s == 0);
            g.setColour (zero ? juce::Colour (0xff29b6f6) : juce::Colour (0x40ffffff));
            g.fillRect (cx - (zero ? 18.0f : 12.0f), fy - 0.5f, zero ? 36.0f : 24.0f, 1.0f);
            if (zero)
            {
                g.setColour (juce::Colour (0xff29b6f6));
                g.drawText ("0", juce::Rectangle<float> (cx + 12.0f, fy - 6.0f, 26.0f, 12.0f),
                            juce::Justification::centredLeft);
            }
        }

        // 已应用区段高亮（中点 → 把手）
        const double off = currentOffset();
        const float hy = yForSeconds (off);
        g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.55f));
        if (off >= 0.0)
            g.fillRect (tr.getX() + 1.0f, yForSeconds (0.0), tr.getWidth() - 2.0f, hy - yForSeconds (0.0));
        else
            g.fillRect (tr.getX() + 1.0f, hy, tr.getWidth() - 2.0f, yForSeconds (0.0) - hy);

        // 把手（白圆 + accent 描边）
        g.setColour (juce::Colours::white);
        g.fillEllipse (cx - 8.0f, hy - 8.0f, 16.0f, 16.0f);
        g.setColour (juce::Colour (0xff29b6f6));
        g.drawEllipse (cx - 8.0f, hy - 8.0f, 16.0f, 16.0f, 2.0f);

        // 底部数值
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::bold)));
        g.setColour (std::abs (off) < 0.05 ? juce::Colour (0xffcfe3ee) : juce::Colour (0xff29b6f6));
        g.drawText (offText (off),
                    juce::Rectangle<float> (0, h - 30.0f, w, 20.0f), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = true;
        applyFromY (e.position.y);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging) applyFromY (e.position.y);
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    // 点击轨道区外（面板空白处）关闭
    void mouseDownOutside() {}

private:
    juce::Rectangle<float> trackRect() const
    {
        const auto b = getLocalBounds().toFloat();
        const float cx = b.getWidth() * 0.30f;
        return { cx - 3.0f, 32.0f, 6.0f, b.getHeight() - 32.0f - 42.0f };
    }

    float yForSeconds (double s) const
    {
        const auto tr = trackRect();
        return (float) (tr.getY() + (juce::jlimit (-10.0, 10.0, s) / 10.0 * 0.5 + 0.5) * tr.getHeight());
    }

    void applyFromY (float y)
    {
        const auto tr = trackRect();
        const double f = juce::jlimit (0.0, 1.0, (double) (y - tr.getY()) / tr.getHeight());
        double v = (f - 0.5) * 20.0;                 // 上=-10(提前) 下=+10(延后)
        v = juce::roundToInt (v * 10.0) / 10.0;      // 0.1s 步进
        if (onOffsetChanged) onOffsetChanged (v);
        repaint();
    }

    double currentOffset() const { return proc.getLyricOffsetOfCurrent(); }

    static juce::String offText (double off)
    {
        if (std::abs (off) < 0.05)
            return MediaPlayAudioProcessor::zh ("默认时间");
        // 注意：不能用 String::formatted —— 中文字面量会被按 ASCII 解析成乱码，须走 zh() + 拼接
        const auto dir = off < 0 ? MediaPlayAudioProcessor::zh ("提前")
                                 : MediaPlayAudioProcessor::zh ("延后");
        return (off < 0 ? "-" : "+") + juce::String (std::abs (off), 1)
             + MediaPlayAudioProcessor::zh (" 秒 (") + dir + MediaPlayAudioProcessor::zh (")");
    }

    MediaPlayAudioProcessor& proc;
    bool dragging = false;
};

//==============================================================================
class MediaPlayAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MediaPlayAudioProcessorEditor (MediaPlayAudioProcessor&);
    ~MediaPlayAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    static juce::String formatTime (double seconds);

    MediaPlayAudioProcessor& processorRef;

    VisualizerComponent visualizer;
    LyricsOverlay lyricsOverlay;
    PlaylistComponent playlistBox;

    PillButton openBtn   { MediaPlayAudioProcessor::zh ("导入文件") };
    PillButton bypassBtn { MediaPlayAudioProcessor::zh ("旁通"), true };

    IconButton prevBtn { IconButton::Prev, true };
    IconButton playBtn { IconButton::Play, true };
    IconButton nextBtn { IconButton::Next, true };
    PillButton spectrumBtn { MediaPlayAudioProcessor::zh ("频谱"), true };
    PillButton lyricBtn { MediaPlayAudioProcessor::zh ("歌词"), true };
    PillButton offsetBtn { MediaPlayAudioProcessor::zh ("偏移") };
    LyricOffsetPopup offsetPopup;
    PillButton modeBtn { MediaPlayAudioProcessor::zh ("列表循环") };

    juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label volumeValue   { {}, "80%" };

    juce::Label timeLabel;
    juce::Slider seekSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    juce::Label appTitle { {}, MediaPlayAudioProcessor::zh ("媒体播放") };

    PlayerLookAndFeel lookAndFeel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool seekDragging = false;
    int lastPlaylistSize = -1;
    int lastCurTrack = -1;

    // 诊断自动加载（沙箱测试用）
    bool autoLoaded = false;
    bool seekTested = false;
    double bootMs = juce::Time::getMillisecondCounterHiRes();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaPlayAudioProcessorEditor)
};
