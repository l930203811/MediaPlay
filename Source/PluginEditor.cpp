#include "PluginEditor.h"

//==============================================================================
MediaPlayAudioProcessorEditor::MediaPlayAudioProcessorEditor (MediaPlayAudioProcessor& p)
    : AudioProcessorEditor (p), processorRef (p), visualizer (p), lyricsOverlay (p), playlistBox (p),
      offsetPopup (p)
{
    setOpaque (true);
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (visualizer);
    visualizer.setVisible (processorRef.getSpectrumEnabled());
    addAndMakeVisible (lyricsOverlay);
    lyricsOverlay.setVisible (processorRef.getLyricsEnabled());
    lyricsOverlay.setInterceptsMouseClicks (false, false);   // 不挡可视化区域交互

    // 频谱开关（默认开，状态随宿主持久化）
    addAndMakeVisible (spectrumBtn);
    spectrumBtn.setToggleState (processorRef.getSpectrumEnabled(), juce::dontSendNotification);
    spectrumBtn.onStateChange = [this]
    {
        processorRef.setSpectrumEnabled (spectrumBtn.getToggleState());
        visualizer.setVisible (spectrumBtn.getToggleState());
        spectrumBtn.repaint();
    };

    // 歌词开关（默认开，状态随宿主持久化）
    addAndMakeVisible (lyricBtn);
    lyricBtn.setToggleState (processorRef.getLyricsEnabled(), juce::dontSendNotification);
    lyricBtn.onStateChange = [this]
    {
        processorRef.setLyricsEnabled (lyricBtn.getToggleState());
        lyricsOverlay.setVisible (lyricBtn.getToggleState());
        lyricBtn.repaint();
    };

    // 歌词偏移：点击弹出参考线（中点=默认时间，上拉提前/下拉延后，±10s，按歌曲持久化）
    addAndMakeVisible (offsetBtn);
    offsetBtn.onClick = [this]
    {
        offsetPopup.setVisible (! offsetPopup.isVisible());
        if (offsetPopup.isVisible())
            offsetPopup.syncFromProcessor();
        offsetBtn.repaint();
    };

    addAndMakeVisible (offsetPopup);
    offsetPopup.setVisible (false);
    offsetPopup.onOffsetChanged = [this] (double v)
    {
        processorRef.setLyricOffsetAbsolute (v);
    };

    addAndMakeVisible (openBtn);
    openBtn.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            MediaPlayAudioProcessor::zh ("选择音频文件（可多选）"), juce::File(),
            "*.mp3;*.wav;*.flac;*.ogg;*.aif;*.aiff;*.m4a;*.aac;*.mp4;*.wma");

        fileChooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems,
            [this] (const juce::FileChooser& fc)
            {
                auto results = fc.getResults();
                if (! results.isEmpty())
                    processorRef.addFilesToPlaylist (results, true);
                fileChooser = nullptr;
            });
    };

    addAndMakeVisible (bypassBtn);
    bypassBtn.onStateChange = [this]
    {
        processorRef.setBypassed (bypassBtn.getToggleState());
        bypassBtn.repaint();
    };

    // 曲目列表（点击切歌）
    addAndMakeVisible (playlistBox);
    playlistBox.onTrackChosen = [this] (int row) { processorRef.loadTrack (row, true); };

    addAndMakeVisible (playBtn);
    playBtn.onClick = [this]
    {
        if (processorRef.isPlaying())
            processorRef.pause();
        else
            processorRef.play();
    };

    addAndMakeVisible (prevBtn);
    prevBtn.onClick = [this] { processorRef.prevTrack(); };

    addAndMakeVisible (nextBtn);
    nextBtn.onClick = [this] { processorRef.nextTrack (true); };

    // 播放模式按钮（文字胶囊，直接显示当前模式名，点击循环切换）
    addAndMakeVisible (modeBtn);
    modeBtn.setButtonText (MediaPlayAudioProcessor::playModeName (processorRef.getPlayMode()));
    modeBtn.onClick = [this]
    {
        using PM = MediaPlayAudioProcessor::PlayMode;
        const int next = ((int) processorRef.getPlayMode() + 1) % 4;
        processorRef.setPlayMode ((PM) next);
        modeBtn.setButtonText (MediaPlayAudioProcessor::playModeName ((PM) next));
    };

    // 音量（传输行右侧横向滑条）
    volumeSlider.setRange (0.0, 1.0, 0.01);
    volumeSlider.setValue (processorRef.getVolume(), juce::dontSendNotification);
    volumeSlider.onValueChange = [this]
    {
        float v = (float) volumeSlider.getValue();
        processorRef.setVolume (v);
        volumeValue.setText (juce::String ((int) (v * 100.0f)) + "%", juce::dontSendNotification);
    };
    addAndMakeVisible (volumeSlider);

    volumeValue.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.0f, juce::Font::plain)));
    volumeValue.setColour (juce::Label::textColourId, juce::Colour (0xffcfe3ee));
    volumeValue.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (volumeValue);

    // 时间 / 进度
    timeLabel.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::plain)));
    timeLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcfe3ee));
    timeLabel.setText ("00:00 / 00:00", juce::dontSendNotification);
    addAndMakeVisible (timeLabel);

    seekSlider.setRange (0.0, 1.0, 0.001);
    seekSlider.onDragStart = [this] { seekDragging = true; };
    seekSlider.onDragEnd   = [this]
    {
        double len = processorRef.getLengthSeconds();
        if (len > 0.0)
        {
            // 留 0.5s 余量，避免拖到最右立刻触发切歌
            double target = juce::jmin ((double) seekSlider.getValue() * len, len - 0.5);
            processorRef.setPositionSeconds (juce::jmax (0.0, target));
        }
        seekDragging = false;
    };
    addAndMakeVisible (seekSlider);

    // 标题
    appTitle.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 17.0f, juce::Font::bold)));
    appTitle.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (appTitle);

    setSize (880, 560);
    startTimer (33);   // ~30fps

    MediaPlayAudioProcessor::debugLog ("=== editor opened (build 18:30) ===");
}

MediaPlayAudioProcessorEditor::~MediaPlayAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

//==============================================================================
void MediaPlayAudioProcessorEditor::paint (juce::Graphics& g)
{
    // 侧边栏背景
    g.fillAll (juce::Colour (0xff0a222d));
    g.setColour (juce::Colour (0xff05121a));
    g.fillRect (0, 0, 4, getHeight());

    // 顶部彩色圆点 logo（参考截图）
    const juce::Colour dotColours[] = { juce::Colour (0xff4fc3f7), juce::Colour (0xffffd54f), juce::Colour (0xffef5350) };
    for (int i = 0; i < 3; ++i)
    {
        g.setColour (dotColours[i]);
        g.fillEllipse (14.0f + i * 13.0f, 18.0f, 9.0f, 9.0f);
    }
}

void MediaPlayAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // 可视化一直延伸到传输区上沿，频谱柱紧贴进度条上方
    visualizer.setBounds (200, 0, area.getWidth() - 200, 484);
    lyricsOverlay.setBounds (216, 214, area.getWidth() - 248, 176);   // 月亮下方，两行上下拉开

    // 侧边栏：标题 / 顶部按钮 / 曲目列表（占满剩余空间）
    appTitle.setBounds (44, 12, 150, 24);
    openBtn.setBounds   (12, 44, 85, 30);
    bypassBtn.setBounds (103, 44, 85, 30);
    playlistBox.setBounds (12, 82, 176, area.getHeight() - 82 - 12);

    // 底部传输区：时间 / 进度条 / 传输按钮（对齐进度条起点）+ 频谱 + 歌词 + 偏移 + 模式 + 音量
    timeLabel.setBounds  (216, 486, 220, 14);
    seekSlider.setBounds (216, 502, 648, 16);
    prevBtn.setBounds (216, 520, 34, 34);
    playBtn.setBounds (266, 516, 42, 42);
    nextBtn.setBounds (324, 520, 34, 34);
    spectrumBtn.setBounds (516, 522, 52, 28);
    lyricBtn.setBounds (572, 522, 52, 28);
    offsetBtn.setBounds (628, 522, 52, 28);
    // 偏移参考线弹窗：悬浮在偏移按钮正上方（120×240，居中对齐按钮）
    offsetPopup.setBounds (628 + 26 - 60, 522 - 240 - 8, 120, 240);
    modeBtn.setBounds (684, 522, 84, 30);
    volumeSlider.setBounds (772, 526, 60, 22);
    volumeValue.setBounds (834, 524, 28, 26);
}

//==============================================================================
void MediaPlayAudioProcessorEditor::timerCallback()
{
    // 诊断自动加载：设置 MEDIAPLAY_AUTOLOAD 环境变量后 2 秒自动导入并播放
    if (! autoLoaded)
    {
        if (const char* al = std::getenv ("MEDIAPLAY_AUTOLOAD"))
        {
            if (juce::Time::getMillisecondCounterHiRes() - bootMs > 2000.0)
            {
                autoLoaded = true;
                juce::Array<juce::File> files;
                files.add (juce::File (juce::CharPointer_UTF8 (al)));
                MediaPlayAudioProcessor::debugLog ("[editor] auto load: " + juce::String (juce::CharPointer_UTF8 (al)));
                processorRef.addFilesToPlaylist (files, true);
            }
        }
        else
        {
            autoLoaded = true;
        }
    }

    // 诊断 seek 测试：MEDIAPLAY_SEEK=秒数，加载 5 秒后自动跳转（沙箱验证拖动 seek 用）
    if (autoLoaded && ! seekTested)
    {
        if (const char* sk = std::getenv ("MEDIAPLAY_SEEK"))
        {
            if (juce::Time::getMillisecondCounterHiRes() - bootMs > 5000.0)
            {
                seekTested = true;
                const double t = juce::String (sk).getDoubleValue();
                MediaPlayAudioProcessor::debugLog (
                    juce::String::formatted ("[editor] seek test -> %.1fs", t));
                processorRef.setPositionSeconds (t);
            }
        }
        else
        {
            seekTested = true;
        }
    }

    // 曲尾切歌由 Processor 自身的 Timer 处理，界面关闭后也能自动切歌
    // 播放/暂停图标状态
    playBtn.setIcon (processorRef.isPlaying() ? IconButton::Pause : IconButton::Play);

    // 播放模式文字同步
    modeBtn.setButtonText (MediaPlayAudioProcessor::playModeName (processorRef.getPlayMode()));

    // 时间 / 进度
    double pos = processorRef.getCurrentPosition();
    double len = processorRef.getLengthSeconds();
    timeLabel.setText (formatTime (pos) + " / " + formatTime (len), juce::dontSendNotification);
    if (! seekDragging && len > 0.0)
        seekSlider.setValue (pos / len, juce::dontSendNotification);

    // 播放列表同步（条目数 / 当前曲目变化时才刷新）
    const int ps = processorRef.getPlaylistSize();
    if (ps != lastPlaylistSize)
    {
        lastPlaylistSize = ps;
        playlistBox.updateContent();
    }
    const int ct = processorRef.getCurrentTrackIndex();
    if (ct != lastCurTrack)
    {
        lastCurTrack = ct;
        playlistBox.repaint();
    }

    visualizer.timerTicked();
    lyricsOverlay.repaint();   // ~30fps 驱动 KTV 渐染

    // 偏移参考线打开时同步当前歌曲偏移（切歌后把手位置跟随）
    if (offsetPopup.isVisible())
        offsetPopup.syncFromProcessor();
}

juce::String MediaPlayAudioProcessorEditor::formatTime (double seconds)
{
    if (seconds < 0.0 || std::isnan (seconds) || std::isinf (seconds))
        seconds = 0.0;
    const int total = (int) seconds;
    if (total >= 3600)
        return juce::String::formatted ("%d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60);
    return juce::String::formatted ("%02d:%02d", total / 60, total % 60);
}
