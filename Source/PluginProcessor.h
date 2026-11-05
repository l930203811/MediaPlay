#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <map>
#include "Lyrics.h"

class MediaPlayAudioProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    MediaPlayAudioProcessor();
    ~MediaPlayAudioProcessor() override;

    // JUCE 的 String(const char*) 按 ASCII 逐字节解析，中文必须显式按 UTF-8 构造
    static juce::String zh (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

    // 临时诊断日志（设置 MEDIAPLAY_LOG 环境变量后启用，定位无声问题用）
    static void debugLog (const juce::String& s);

    //===== 播放模式 =====
    enum PlayMode { Sequential = 0, LoopAll, LoopOne, Shuffle };
    static juce::String playModeName (PlayMode m)
    {
        switch (m)
        {
            case Sequential: return zh ("顺序播放");
            case LoopAll:    return zh ("列表循环");
            case LoopOne:    return zh ("单曲循环");
            case Shuffle:    return zh ("随机播放");
        }
        return {};
    }

    //===== 频谱参数 =====
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder;   // 2048
    static constexpr int numBars  = 48;              // 频谱柱数量

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "MediaPlay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // 播放列表 / 播放控制（在消息线程调用）
    void addFilesToPlaylist (const juce::Array<juce::File>& files, bool playFirstNew);
    void clearPlaylist();
    void removeTrack (int index);       // 删除指定条目（右键菜单用）
    void loadTrack (int index, bool andPlay);
    void play();
    void pause();
    void stop();
    void nextTrack (bool userTriggered);
    void prevTrack();
    void handleTrackEnd();          // 自然播放结束后按模式切歌
    void setPlayMode (PlayMode m)   { playMode = m; }
    PlayMode getPlayMode() const    { return playMode; }

    bool isPlaying() const            { return playing.load (std::memory_order_relaxed); }
    double getCurrentPosition() const { return currentSeconds.load (std::memory_order_relaxed); }
    double getLengthSeconds() const   { return lengthSeconds.load (std::memory_order_relaxed); }
    void setPositionSeconds (double t)
    {
        seekRequestSeconds.store (t, std::memory_order_relaxed);
        currentSeconds.store (t, std::memory_order_relaxed);   // UI 立即响应
    }

    juce::String getCurrentTrackTitle() const;
    int getCurrentTrackIndex() const { return currentTrack; }
    int getPlaylistSize() const { return (int) playlist.size(); }
    juce::String getPlaylistItem (int i) const { return playlist[(size_t) i].getFileName(); }

    // 歌词（loadTrack 时自动加载同名 .lrc；不支持/不支持逐字时逐句显示）
    Lyrics& getLyrics()            { return lyrics; }
    bool getLyricsEnabled() const  { return lyricsOn.load (std::memory_order_relaxed); }
    void setLyricsEnabled (bool on){ lyricsOn.store (on, std::memory_order_relaxed); }

    // 可视化频谱开关（隐藏/显示整个可视化区域）
    bool getSpectrumEnabled() const  { return spectrumOn.load (std::memory_order_relaxed); }
    void setSpectrumEnabled (bool on){ spectrumOn.store (on, std::memory_order_relaxed); }

    // 歌词偏移（按歌曲路径记忆，持久化到宿主状态）：正 = 推迟，负 = 提前
    double getLyricOffsetOfCurrent() const
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return 0.0;
        auto it = lyricOffsets.find (playlist[(size_t) currentTrack].getFullPathName());
        return it != lyricOffsets.end() ? it->second : 0.0;
    }
    void adjustLyricOffset (double delta)
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return;
        auto& off = lyricOffsets[playlist[(size_t) currentTrack].getFullPathName()];
        off = juce::jlimit (-10.0, 10.0, off + delta);
        lyrics.applyTimeOffset (delta);
    }
    // 参考线拖动用：直接设定当前歌曲的歌词偏移（秒，正=推迟，±10s 封顶）
    void setLyricOffsetAbsolute (double v)
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return;
        const auto key = playlist[(size_t) currentTrack].getFullPathName();
        double cur = 0.0;
        if (auto it = lyricOffsets.find (key); it != lyricOffsets.end())
            cur = it->second;
        const double target = juce::jlimit (-10.0, 10.0, v);
        const double delta = target - cur;
        if (std::abs (delta) < 1e-9) return;
        lyricOffsets[key] = target;
        lyrics.applyTimeOffset (delta);
    }
    void resetLyricOffset()
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return;
        auto it = lyricOffsets.find (playlist[(size_t) currentTrack].getFullPathName());
        if (it != lyricOffsets.end())
        {
            lyrics.applyTimeOffset (-it->second);
            lyricOffsets.erase (it);
        }
    }

    // 音量 / 旁通
    void setVolume (float v)
    {
        debugLog (juce::String::formatted ("UI: setVolume %.2f", (double) v));
        gain.store (v, std::memory_order_relaxed);
    }
    float getVolume() const   { return gain.load(); }
    void setBypassed (bool b)
    {
        debugLog (juce::String ("UI: setBypassed ") + (b ? "ON" : "OFF"));
        bypassed = b;
    }
    bool getBypassed() const  { return bypassed.load(); }

    // 输入透传：开 = 通道输入信号直达输出（机架唱歌用）；关 = 屏蔽输入，只输出播放器（消除啸叫用）
    void setInputThru (bool on)
    {
        debugLog (juce::String ("UI: setInputThru ") + (on ? "ON" : "OFF"));
        inputThru.store (on, std::memory_order_relaxed);
    }
    bool getInputThru() const { return inputThru.load (std::memory_order_relaxed); }

    // 频谱数据（音频线程写，编辑器读）
    float getBar (int i) const     { return bars[(size_t) juce::jlimit (0, numBars - 1, i)].load (std::memory_order_relaxed); }
    float getLowEnergy() const     { return lowEnergy.load (std::memory_order_relaxed); }
    float getMidEnergy() const     { return midEnergy.load (std::memory_order_relaxed); }
    float getHighEnergy() const    { return highEnergy.load (std::memory_order_relaxed); }
    bool  isSpectrumActive() const { return spectrumActive.load (std::memory_order_relaxed); }

    std::atomic<int> trackEndedFlag { 0 };  // 音频线程置位，Processor Timer 处理（界面关闭也生效）

private:
    // 独立于编辑器的定时器：接管曲尾切歌，不依赖 UI 是否打开
    void timerCallback() override;

    //===== 播放列表 =====
    juce::AudioFormatManager formatManager;
    std::vector<juce::File> playlist;
    int currentTrack = -1;
    PlayMode playMode = LoopAll;
    Lyrics lyrics;                                     // 当前曲目歌词（消息线程访问）
    std::atomic<bool> lyricsOn { true };               // 歌词显示开关
    std::atomic<bool> spectrumOn { true };             // 可视化频谱显示开关
    std::map<juce::String, double> lyricOffsets;       // 按歌曲路径记忆的歌词偏移（秒）

    //===== 直读播放引擎（无 AudioTransportSource，宿主无关）=====
    // 消息线程只负责打开 reader 丢进 pendingReader；音频线程接管/释放/解码。
    std::atomic<juce::AudioFormatReader*> pendingReader { nullptr };
    juce::AudioFormatReader* activeReader = nullptr;   // 仅音频线程触碰
    juce::AudioBuffer<float> scratch;                  // 解码暂存窗（仅音频线程）
    double readerPos = 0.0;                            // 文件内采样点位置（音频线程）
    int silentEofBlocks = 0;                           // 未知时长文件的 EOF 静音计数（音频线程）
    bool eofFlagFired = false;                         // EOF 已置位标记（边沿触发，防止重复切歌）
    std::atomic<bool>   playing { false };
    std::atomic<double> seekRequestSeconds { -1.0 };
    std::atomic<double> currentSeconds { 0.0 };
    std::atomic<double> lengthSeconds  { 0.0 };

    std::atomic<float> gain { 0.8f };                  // 目标音量
    float smoothedGain = 0.8f;                         // 音频线程内平滑
    std::atomic<bool>  bypassed { false };
    std::atomic<bool>  inputThru { true };             // 输入透传开关（默认开）

    //===== FFT 频谱分析 =====
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize,
        juce::dsp::WindowingFunction<float>::hann };
    std::array<float, fftSize> fifo {};
    std::array<float, fftSize> windowBuf {};
    std::array<float, fftSize * 2> fftData {};
    int fifoIndex = 0;
    std::atomic<bool> spectrumActive { false };

    std::array<std::atomic<float>, numBars> bars {};
    std::atomic<float> lowEnergy { 0.0f };
    std::atomic<float> midEnergy { 0.0f };
    std::atomic<float> highEnergy { 0.0f };

    void pushSampleIntoFifo (float sample);
    void computeSpectrum();
    void renderAudio (juce::AudioBuffer<float>& buffer);
    int renderBlockCount = 0;   // 诊断用

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaPlayAudioProcessor)
};
