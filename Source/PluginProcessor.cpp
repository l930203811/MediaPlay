#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MFAudioFormat.h"

#include <cstdlib>

// MF 格式诊断回调 → mp_debug.log
std::function<void (const juce::String&)> mfDebugLog = nullptr;

//==============================================================================
// 临时诊断日志（默认写 D:\Xiangmu\VstMediaPlayer\mp_debug.log，MEDIAPLAY_LOG 可覆盖）
void MediaPlayAudioProcessor::debugLog (const juce::String& s)
{
    juce::String path = "D:\\Xiangmu\\VstMediaPlayer\\mp_debug.log";
    if (const char* p = std::getenv ("MEDIAPLAY_LOG"))
        path = juce::String::fromUTF8 (p);
    juce::File (path).appendText (
        juce::Time::getCurrentTime().toString (false, true) + "  " + s + "\n");
}
MediaPlayAudioProcessor::MediaPlayAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // MF 必须最先注册：registerBasicFormats 自带的 WindowsMediaAudioFormat 内部也用 MF，
    // 但没显式声明输出采样率，遇到 HE-AAC 会解出 12000Hz 静音垃圾；且 JUCE 的 MP3 格式
    // 靠扫描帧同步头识别文件，会把某些 MP4 容器误认成 MP3。MF 按容器结构识别，全兜底。
    formatManager.registerFormat (new MFAudioFormat(), false);  // MP4/M4A/AAC（含伪装成 .mp3 的 MP4）/WMA/MP3
    formatManager.registerBasicFormats();                       // WAV / AIFF / FLAC / OGG / WMF
    formatManager.registerFormat (new juce::MP3AudioFormat(), false);
    mfDebugLog = [] (const juce::String& s) { debugLog (s); };

    for (auto& b : bars)
        b.store (0.0f, std::memory_order_relaxed);

    // 曲尾切歌由处理器自身的定时器驱动：编辑器关闭（Timer 销毁）后仍继续工作。
    // Timer 回调经 MessageManager 投递到消息线程，宿主消息循环常驻，故与 UI 是否打开无关。
    startTimer (50);
}

MediaPlayAudioProcessor::~MediaPlayAudioProcessor()
{
    stopTimer();

    // 宿主销毁插件时音频回调已停止，可安全清理
    if (auto* r = pendingReader.exchange (nullptr))
        delete r;
    delete activeReader;
    activeReader = nullptr;
}

//==============================================================================
void MediaPlayAudioProcessor::timerCallback()
{
    if (trackEndedFlag.exchange (0) == 1)
        handleTrackEnd();
}

//==============================================================================
// 宿主状态持久化：重启机架 / 重开工程后恢复播放列表、曲目、位置、模式、音量
//==============================================================================
void MediaPlayAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("MediaPlayState");
    xml.setAttribute ("playMode", (int) playMode);
    xml.setAttribute ("volume", (double) gain.load (std::memory_order_relaxed));
    xml.setAttribute ("inputThru", inputThru.load (std::memory_order_relaxed));
    xml.setAttribute ("currentTrack", currentTrack);
    xml.setAttribute ("position", currentSeconds.load (std::memory_order_relaxed));
    xml.setAttribute ("lyricsOn", lyricsOn.load (std::memory_order_relaxed));
    xml.setAttribute ("spectrumOn", spectrumOn.load (std::memory_order_relaxed));

    auto* list = xml.createNewChildElement ("Playlist");
    for (const auto& f : playlist)
        list->createNewChildElement ("Track")->setAttribute ("path", f.getFullPathName());

    auto* offs = xml.createNewChildElement ("LyricOffsets");
    for (const auto& kv : lyricOffsets)
    {
        auto* item = offs->createNewChildElement ("Item");
        item->setAttribute ("path", kv.first);
        item->setAttribute ("sec", kv.second);
    }

    copyXmlToBinary (xml, dest);
}

void MediaPlayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName ("MediaPlayState"))
        return;

    // 停止播放，重建列表（保留全部条目以维持索引对齐；缺文件的条目切到时自动跳过加载）
    playing.store (false, std::memory_order_relaxed);
    playlist.clear();
    if (auto* list = xml->getChildByName ("Playlist"))
        for (auto* t : list->getChildWithTagNameIterator ("Track"))
            playlist.push_back (juce::File (t->getStringAttribute ("path")));

    playMode = (PlayMode) juce::jlimit (0, 3, xml->getIntAttribute ("playMode", (int) LoopAll));
    gain.store ((float) xml->getDoubleAttribute ("volume", 0.8), std::memory_order_relaxed);
    inputThru.store (xml->getBoolAttribute ("inputThru", true), std::memory_order_relaxed);
    lyricsOn.store (xml->getBoolAttribute ("lyricsOn", true), std::memory_order_relaxed);
    spectrumOn.store (xml->getBoolAttribute ("spectrumOn", true), std::memory_order_relaxed);

    // 歌词偏移表先于恢复曲目解析，loadTrack 时才能应用
    lyricOffsets.clear();
    if (auto* offs = xml->getChildByName ("LyricOffsets"))
        for (auto* it : offs->getChildWithTagNameIterator ("Item"))
            lyricOffsets[it->getStringAttribute ("path")] = it->getDoubleAttribute ("sec", 0.0);

    const int cur = xml->getIntAttribute ("currentTrack", -1);
    currentTrack = -1;
    currentSeconds.store (0.0, std::memory_order_relaxed);
    lengthSeconds.store (0.0, std::memory_order_relaxed);

    // 恢复上次曲目（不自动播放），文件仍存在则预载 reader，可从上次位置继续
    if (cur >= 0 && cur < (int) playlist.size() && playlist[(size_t) cur].existsAsFile())
    {
        loadTrack (cur, false);
        const double pos = xml->getDoubleAttribute ("position", 0.0);
        if (pos > 0.5)
        {
            seekRequestSeconds.store (pos, std::memory_order_relaxed);
            currentSeconds.store (pos, std::memory_order_relaxed);
        }
    }

    debugLog (juce::String::formatted ("state restored: %d tracks, cur=%d, mode=%d",
                                       (int) playlist.size(), cur, (int) playMode));
}

//==============================================================================
void MediaPlayAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    smoothedGain = gain.load (std::memory_order_relaxed);
    fifoIndex = 0;
    fifo.fill (0.0f);
    debugLog (juce::String::formatted ("prepareToPlay sr=%.1f block=%d", sampleRate, samplesPerBlock));
}

void MediaPlayAudioProcessor::releaseResources() {}

bool MediaPlayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 效果器型插件：接受「输入 <= 输出」的任意布局，避免宿主因布局协商失败而不拉取音频
    const auto out = layouts.getMainOutputChannelSet();
    const auto in  = layouts.getMainInputChannelSet();
    return out.size() > 0 && in.size() <= out.size();
}

//==============================================================================
// 核心渲染：直接在音频线程流式解码（线性插值重采样到宿主采样率）
void MediaPlayAudioProcessor::renderAudio (juce::AudioBuffer<float>& buffer)
{
    const int numOut = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    const double devRate = getSampleRate();
    const double fileRate = activeReader->sampleRate;
    const juce::int64 fileLen = activeReader->lengthInSamples;
    const double ratio = fileRate / devRate;                    // 每 1 个输出样本需要的文件样本数

    // 音量平滑（只作用于播放器输出，不影响透传的通道输入）
    const float gTarget = gain.load (std::memory_order_relaxed);
    const float g0 = smoothedGain;
    const float g1 = g0 + (gTarget - g0) * 0.25f;
    smoothedGain = g1;

    // 解码窗口大小（含插值余量）
    const int need = (int) std::ceil ((double) n * ratio) + 3;
    if (scratch.getNumChannels() < (int) activeReader->numChannels || scratch.getNumSamples() < need)
        scratch.setSize ((int) activeReader->numChannels, need, false, false, true);

    auto startPos = (juce::int64) readerPos;
    const bool unknownLen = fileLen <= 0;   // 少数容器拿不到时长：顺序解码直到 EOF
    auto readable = unknownLen ? (juce::int64) need
                               : juce::jlimit<juce::int64> (0, (juce::int64) need, fileLen - startPos);
    scratch.clear();
    if (readable > 0)
        activeReader->read (scratch.getArrayOfWritePointers(), scratch.getNumChannels(),
                            startPos, (int) readable);

    const float frac0 = (float) (readerPos - (double) startPos);

    for (int ch = 0; ch < numOut; ++ch)
    {
        // 输出通道映射：单声道文件复制到所有输出，多声道取 min(ch, 源声道-1)
        const int src = (activeReader->numChannels == 1)
                            ? 0 : juce::jmin (ch, (int) activeReader->numChannels - 1);
        const float* s = scratch.getReadPointer (src);
        float* dst = buffer.getWritePointer (ch);

        for (int i = 0; i < n; ++i)
        {
            const double p = (double) frac0 + (double) i * ratio;
            const int idx = (int) p;
            const float fr = (float) (p - (double) idx);
            const float g = g0 + (g1 - g0) * ((float) i / (float) juce::jmax (1, n - 1));
            dst[i] += (s[idx] + (s[idx + 1] - s[idx]) * fr) * g;   // 叠加在透传输入之上
        }
    }

    readerPos += (double) n * ratio;

    // 进度 / 结束检测（EOF 标记边沿触发：只置位一次，避免编辑器重复切歌）
    if (! unknownLen)
    {
        currentSeconds.store (juce::jmin (readerPos / fileRate, (double) fileLen / fileRate),
                              std::memory_order_relaxed);
        if (readerPos >= (double) fileLen - 1.0)
        {
            if (! eofFlagFired)
            {
                eofFlagFired = true;
                trackEndedFlag.store (1, std::memory_order_relaxed);
            }
        }
        else
            eofFlagFired = false;
    }
    else
    {
        currentSeconds.store (readerPos / fileRate, std::memory_order_relaxed);

        // 未知时长：用 MF reader 的真实 EOF + 尾部静音持续 ~1.5s 判定曲尾切歌
        auto* mf = dynamic_cast<MFAudioFormatReader*> (activeReader);
        if (mf != nullptr && mf->isEof())
        {
            float tailPeak = 0.0f;
            for (int i = 0; i < need; i += 4)
                tailPeak = juce::jmax (tailPeak, std::abs (scratch.getSample (0, i)));
            if (tailPeak < 0.001f)
            {
                if (++silentEofBlocks > (int) (1.5 * devRate / (double) juce::jmax (1, n))
                    && ! eofFlagFired)
                {
                    eofFlagFired = true;
                    trackEndedFlag.store (1, std::memory_order_relaxed);
                }
            }
            else
                silentEofBlocks = 0;
        }
    }
}

void MediaPlayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn  = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOut = buffer.getNumChannels();
    const int n = buffer.getNumSamples();

    // 1) 输入处理：透传开关关闭时屏蔽输入（只输出播放器，消除麦克风→扬声器反馈啸叫）；
    //    开启时即为效果器语义——in==out 时 JUCE 的 buffer 本身就承载通道输入，不清除即为透传，
    //    仅当输出通道多于输入时，把最后的输入通道补拷到其余输出。
    if (numIn == 0 || ! inputThru.load (std::memory_order_relaxed))
        buffer.clear();
    else
        for (int ch = numIn; ch < numOut; ++ch)
            buffer.copyFrom (ch, 0, buffer.getReadPointer (numIn - 1), n);

    // 旁通 = 仅透传输入，不渲染播放器
    if (bypassed.load (std::memory_order_relaxed))
        return;

    // 1) 接管消息线程送来的新曲目（含释放旧 reader）
    if (auto* incoming = pendingReader.exchange (nullptr))
    {
        delete activeReader;
        activeReader = incoming;
        readerPos = 0.0;
        silentEofBlocks = 0;
        eofFlagFired = false;
        const double fr = activeReader->sampleRate;
        lengthSeconds.store (fr > 0.0 ? (double) activeReader->lengthInSamples / fr : 0.0,
                             std::memory_order_relaxed);
        currentSeconds.store (0.0, std::memory_order_relaxed);
        renderBlockCount = 0;
        debugLog (juce::String::formatted ("[audio] took reader sr=%.0f ch=%u len=%lld playing=%d devSR=%.1f numIn=%d numOut=%d",
                                           activeReader->sampleRate, activeReader->numChannels,
                                           (long long) activeReader->lengthInSamples,
                                           (int) playing.load(), getSampleRate(), numIn, numOut));
    }

    // 2) 应用 seek 请求
    if (const double s = seekRequestSeconds.exchange (-1.0); s >= 0.0 && activeReader != nullptr)
    {
        readerPos = s * activeReader->sampleRate;
        currentSeconds.store (s, std::memory_order_relaxed);
        eofFlagFired = false;   // 手动 seek 后重新武装曲尾检测
    }

    // 3) 解码渲染
    if (playing.load (std::memory_order_relaxed) && activeReader != nullptr
        && activeReader->sampleRate > 0.0)
    {
        renderAudio (buffer);
        if (++renderBlockCount % 86 == 0)   // 约 1 秒一次
        {
            double pk = 0.0;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                pk = juce::jmax (pk, (double) std::abs (buffer.getSample (0, i)));
            debugLog (juce::String::formatted ("[audio] render pos=%.1fs vol=%.2f bypass=%d chPeak=%.3f",
                                               getCurrentPosition(), (double) gain.load(),
                                               (int) bypassed.load(), pk));
        }
    }
    else if (playing.load (std::memory_order_relaxed))
    {
        static int waitLogged = 0;   // 进程级，仅诊断用
        if (waitLogged++ % 86 == 0)
            debugLog (juce::String::formatted ("[audio] PLAYING but NO RENDER (activeReader=%p devSR=%.1f bypass=%d)",
                                               (void*) activeReader, getSampleRate(),
                                               (int) bypassed.load()));
    }

    // 4) 音量已在 renderAudio 内平滑应用（只作用于播放器，不碰透传输入）

    // 5) 频谱分析（单声道混合，取自最终输出）
    const auto* chL = buffer.getReadPointer (0);
    for (int i = 0; i < n; ++i)
    {
        float s = chL[i];
        if (buffer.getNumChannels() > 1)
            s = 0.5f * (s + buffer.getSample (1, i));
        pushSampleIntoFifo (s);
    }
}

//==============================================================================
void MediaPlayAudioProcessor::pushSampleIntoFifo (float sample)
{
    fifo[(size_t) fifoIndex] = sample;
    if (++fifoIndex == fftSize)
    {
        fifoIndex = 0;
        computeSpectrum();
    }
}

void MediaPlayAudioProcessor::computeSpectrum()
{
    const auto sr = (float) getSampleRate();

    std::copy (fifo.begin(), fifo.end(), windowBuf.begin());
    window.multiplyWithWindowingTable (windowBuf.data(), (size_t) fftSize);

    for (int i = 0; i < fftSize; ++i)
    {
        fftData[(size_t) (2 * i)]     = windowBuf[(size_t) i];
        fftData[(size_t) (2 * i + 1)] = 0.0f;
    }
    fft.performRealOnlyForwardTransform (fftData.data(), true);

    // bin 幅度（1024 个），映射到 48 根对数频率柱
    const int numBins = fftSize / 2;
    std::array<float, numBars> newBars {};
    const float minFreq = 30.0f, maxFreq = juce::jmin (16000.0f, sr * 0.5f);

    for (int b = 0; b < numBars; ++b)
    {
        float f0 = minFreq * std::pow (maxFreq / minFreq, (float) b / numBars);
        float f1 = minFreq * std::pow (maxFreq / minFreq, (float) (b + 1) / numBars);
        int bin0 = juce::jlimit (0, numBins - 1, (int) std::floor (f0 / (sr * 0.5f) * numBins));
        int bin1 = juce::jlimit (bin0 + 1, numBins, (int) std::ceil (f1 / (sr * 0.5f) * numBins));

        float sum = 0.0f;
        for (int k = bin0; k < bin1; ++k)
        {
            float re = fftData[(size_t) (2 * k)];
            float im = fftData[(size_t) (2 * k + 1)];
            sum += std::sqrt (re * re + im * im);
        }
        float mag = sum / (float) (bin1 - bin0);

        float db = juce::Decibels::gainToDecibels (mag / (float) numBins * 4.0f, -120.0f);
        newBars[(size_t) b] = juce::jlimit (0.0f, 1.0f, (db + 72.0f) / 60.0f);
    }
    for (int b = 0; b < numBars; ++b)
        bars[(size_t) b].store (newBars[(size_t) b], std::memory_order_relaxed);

    // 低 / 中 / 高 频段能量（用于节奏跳动：太阳=低频，山峦=中频，星星=高频）
    auto bandEnergy = [&] (float f0, float f1)
    {
        int bin0 = juce::jlimit (0, numBins - 1, (int) (f0 / (sr * 0.5f) * numBins));
        int bin1 = juce::jlimit (bin0 + 1, numBins, (int) (f1 / (sr * 0.5f) * numBins));
        float sum = 0.0f;
        for (int k = bin0; k < bin1; ++k)
        {
            float re = fftData[(size_t) (2 * k)];
            float im = fftData[(size_t) (2 * k + 1)];
            sum += std::sqrt (re * re + im * im);
        }
        float mag = (sum / (float) (bin1 - bin0)) / (float) numBins * 4.0f;
        float db  = juce::Decibels::gainToDecibels (mag, -120.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 48.0f);
    };

    lowEnergy.store  (bandEnergy (20.0f, 250.0f),    std::memory_order_relaxed);
    midEnergy.store  (bandEnergy (250.0f, 2000.0f),  std::memory_order_relaxed);
    highEnergy.store (bandEnergy (2000.0f, 12000.0f), std::memory_order_relaxed);

    spectrumActive.store (true, std::memory_order_relaxed);
}

//==============================================================================
// 播放列表 / 播放控制（消息线程）
//==============================================================================
void MediaPlayAudioProcessor::addFilesToPlaylist (const juce::Array<juce::File>& files, bool playFirstNew)
{
    int firstNew = (int) playlist.size();
    for (const auto& f : files)
        playlist.push_back (f);

    if (playFirstNew && ! playlist.empty())
        loadTrack (firstNew, true);
}

void MediaPlayAudioProcessor::clearPlaylist()
{
    playing.store (false, std::memory_order_relaxed);
    // 丢弃尚未被音频线程接管的 reader，避免泄漏
    if (auto* r = pendingReader.exchange (nullptr))
        delete r;

    playlist.clear();          // 之前只重置了状态，列表条目其实没被清空
    currentTrack = -1;
    seekRequestSeconds.store (-1.0, std::memory_order_relaxed);
    currentSeconds.store (0.0, std::memory_order_relaxed);
    lengthSeconds.store (0.0, std::memory_order_relaxed);
    trackEndedFlag.store (0, std::memory_order_relaxed);
    lyrics.clear();
}

void MediaPlayAudioProcessor::removeTrack (int index)
{
    if (index < 0 || index >= (int) playlist.size()) return;

    if (index == currentTrack)
    {
        const bool wasPlaying = isPlaying();
        playlist.erase (playlist.begin() + index);
        playing.store (false, std::memory_order_relaxed);

        if (playlist.empty())
        {
            clearPlaylist();
            return;
        }

        // 删掉正在播放的曲目：自动接上后一首（删末尾则回到新的最后一首），
        // 并保持原来的播放/暂停状态；loadTrack 失败时保持静音，不会继续放旧文件。
        currentTrack = -1;
        lyrics.clear();
        currentSeconds.store (0.0, std::memory_order_relaxed);
        lengthSeconds.store (0.0, std::memory_order_relaxed);
        trackEndedFlag.store (0, std::memory_order_relaxed);
        loadTrack (juce::jmin (index, (int) playlist.size() - 1), wasPlaying);
    }
    else
    {
        // 删除当前曲目之前的条目：索引左移，正在播放的文件不变
        if (index < currentTrack)
            --currentTrack;
        playlist.erase (playlist.begin() + index);
    }
}

void MediaPlayAudioProcessor::loadTrack (int index, bool andPlay)
{
    lyrics.clear();   // 无论成败，旧歌词先清掉，避免曲词错位

    if (index < 0 || index >= (int) playlist.size())
        return;

    // 先确保 reader 打开成功再切曲 —— 失败则保持现状，曲名与音频始终一致
    auto file = playlist[(size_t) index];
    auto* reader = formatManager.createReaderFor (file);
    if (reader == nullptr)
    {
        debugLog ("loadTrack FAILED (reader==null): " + file.getFullPathName());
        return;
    }

    lyrics.loadForFile (file);
    auto savedOff = lyricOffsets.find (file.getFullPathName());
    if (savedOff != lyricOffsets.end() && savedOff->second != 0.0)
        lyrics.applyTimeOffset (savedOff->second);   // 应用该曲记忆的偏移
    debugLog (juce::String::formatted ("lyrics: %s (%s), lines=%d",
                                       lyrics.isLoaded() ? "loaded" : "none",
                                       lyrics.hasWordTiming() ? "word" : "line",
                                       lyrics.getLineCount()));

    debugLog (juce::String::formatted ("loadTrack ok idx=%d sr=%.0f ch=%u len=%lld (%.1fs)",
                                       index, reader->sampleRate, reader->numChannels,
                                       (long long) reader->lengthInSamples,
                                       reader->sampleRate > 0 ? reader->lengthInSamples / reader->sampleRate : 0.0));

    currentTrack = index;
    // 覆盖前先释放尚未被音频线程接管的旧 reader（快速切歌/删歌时可能出现）
    if (auto* stale = pendingReader.exchange (reader, std::memory_order_relaxed))
        delete stale;
    seekRequestSeconds.store (-1.0, std::memory_order_relaxed);
    currentSeconds.store (0.0, std::memory_order_relaxed);

    if (andPlay)
        playing.store (true, std::memory_order_relaxed);
}

void MediaPlayAudioProcessor::play()
{
    debugLog ("UI: play() pressed");
    if (currentTrack < 0)
    {
        if (! playlist.empty())
            loadTrack (0, true);
        return;
    }

    // 播放到尾部后再按播放 → 从头开始
    const double len = lengthSeconds.load (std::memory_order_relaxed);
    const double pos = currentSeconds.load (std::memory_order_relaxed);
    if (len > 0.0 && pos >= len - 0.05)
        seekRequestSeconds.store (0.0, std::memory_order_relaxed);

    playing.store (true, std::memory_order_relaxed);
}

void MediaPlayAudioProcessor::pause()
{
    debugLog ("UI: pause()");
    playing.store (false, std::memory_order_relaxed);
}

void MediaPlayAudioProcessor::stop()
{
    debugLog ("UI: stop()");
    playing.store (false, std::memory_order_relaxed);
    seekRequestSeconds.store (0.0, std::memory_order_relaxed);
    currentSeconds.store (0.0, std::memory_order_relaxed);
}

int pickShuffle (int current, int size)
{
    if (size <= 1) return current;
    int n = current;
    juce::Random r = juce::Random::getSystemRandom();
    while (n == current)
        n = r.nextInt (size);
    return n;
}

void MediaPlayAudioProcessor::nextTrack (bool userTriggered)
{
    if (playlist.empty()) return;

    switch (playMode)
    {
        case LoopOne:
            if (userTriggered)
            {
                loadTrack ((currentTrack + 1) % (int) playlist.size(), true);
            }
            else
            {
                seekRequestSeconds.store (0.0, std::memory_order_relaxed);
                playing.store (true, std::memory_order_relaxed);
            }
            break;

        case LoopAll:
            loadTrack ((currentTrack + 1) % (int) playlist.size(), true);
            break;

        case Shuffle:
            loadTrack (pickShuffle (currentTrack, (int) playlist.size()), true);
            break;

        case Sequential:
        default:
            if (currentTrack + 1 < (int) playlist.size())
                loadTrack (currentTrack + 1, true);
            else
                stop();   // 播完列表即停
            break;
    }
}

void MediaPlayAudioProcessor::prevTrack()
{
    if (playlist.empty()) return;

    if (playMode == Shuffle)
    {
        loadTrack (pickShuffle (currentTrack, (int) playlist.size()), true);
        return;
    }

    const bool wasPlaying = isPlaying();

    // 播放超过 3 秒 → 先回到本曲开头
    if (getCurrentPosition() > 3.0)
    {
        setPositionSeconds (0.0);
        if (wasPlaying) playing.store (true, std::memory_order_relaxed);
        return;
    }

    const int n = (currentTrack <= 0 ? (int) playlist.size() - 1 : currentTrack - 1);
    loadTrack (n, wasPlaying);
}

void MediaPlayAudioProcessor::handleTrackEnd()
{
    nextTrack (false);
}

juce::String MediaPlayAudioProcessor::getCurrentTrackTitle() const
{
    if (currentTrack < 0 || currentTrack >= (int) playlist.size())
        return zh ("未加载曲目");
    return playlist[(size_t) currentTrack].getFileName();
}

juce::AudioProcessorEditor* MediaPlayAudioProcessor::createEditor()
{
    return new MediaPlayAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MediaPlayAudioProcessor();
}
