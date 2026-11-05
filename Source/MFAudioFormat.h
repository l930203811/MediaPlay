#pragma once

// 基于 Windows Media Foundation 的通用音频格式（兜底 JUCE 内置不支持的容器）
// 支持：MP4/M4A/AAC（含伪装成 .mp3 的 MP4 文件）、WMA、以及 MP3/WAV 等 MF 能解的格式
// 仅 Windows；reader 支持随机访问 read（内部顺序解码 + Seek）

#include <juce_audio_formats/juce_audio_formats.h>
#include <cstring>

// 诊断回调（由插件设置，输出到 mp_debug.log）
#include <functional>
extern std::function<void (const juce::String&)> mfDebugLog;

#if JUCE_WINDOWS
#define MFLOG(msg) do { if (mfDebugLog) mfDebugLog (msg); } while (false)
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>

class MFAudioFormatReader : public juce::AudioFormatReader
{
public:
    static MFAudioFormatReader* create (const juce::File& file)
    {
        // MF 完整模式启动（LITE 模式不加载部分解码器管线，HE-AAC 的 SBR 展开会缺失）
        HRESULT hr = MFStartup (MF_VERSION, 0);
        if (FAILED (hr))
        {
            // COM 未初始化的场景：先初始化 MTA 再重试
            CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            hr = MFStartup (MF_VERSION, 0);
            if (FAILED (hr))
            {
                MFLOG ("MF: MFStartup failed hr=0x" + juce::String::toHexString ((int) hr));
                return nullptr;
            }
        }

        IMFSourceReader* r = nullptr;
        hr = MFCreateSourceReaderFromURL (file.getFullPathName().toWideCharPointer(), nullptr, &r);
        if (FAILED (hr) || r == nullptr)
        {
            MFLOG ("MF: CreateSourceReader failed hr=0x" + juce::String::toHexString ((int) hr)
                   + " file=" + file.getFullPathName());
            MFShutdown();
            return nullptr;
        }

        auto* reader = new MFAudioFormatReader (r, true, file.getFullPathName());
        if (! reader->initOutput (file))
        {
            MFLOG ("MF: initOutput failed for " + file.getFullPathName());
            delete reader;
            MFShutdown();
            return nullptr;
        }
        MFLOG (juce::String::formatted ("MF: reader ok sr=%.0f ch=%u len=%lld",
                                        reader->sampleRate, reader->numChannels,
                                        (long long) reader->lengthInSamples));
        return reader;
    }

    ~MFAudioFormatReader() override
    {
        if (sourceReader != nullptr)
            sourceReader->Release();
        if (mfStarted)
            MFShutdown();
    }

    // 供播放引擎判断未知时长文件的真实 EOF（音频线程调用）
    bool isEof() const { return eof; }

private:
    // 元数据缺失或被判定损坏时的兜底：二分探测真实时长。
    // 反复 SetCurrentPosition(t) 再读一帧看是否 EOS；音频流关键帧密集，
    // 误差一般 <1~2s，对进度条显示与拖动 seek 完全够用。
    void probeDuration()
    {
        auto probeEos = [this] (double sec) -> bool
        {
            seekTo ((juce::int64) (sec * sampleRate));
            // 部分 MF 源在 seek 到 EOF 附近时，头 1~2 帧仍是末尾音频数据，
            // 需要继续读到 EOS 才能判定；设上限防止 seek 失效时死循环。
            for (int k = 0; k < 8 && ! eof; ++k)
                decodeChunk();
            return eof;
        };

        double lo = 0.0, hi = 45.0;
        while (hi <= 7200.0)
        {
            if (probeEos (hi)) break;
            lo = hi;
            hi *= 2.0;
        }

        if (hi > 7200.0)
        {
            // seek 探测失效（部分 DASH/分片 MP4 的 SetCurrentPosition 不生效）
            // → 顺序解码计数到 EOF：慢一点但结果精确（MF 解码远快于实时）
            MFLOG ("MF: seek probe failed, counting by sequential decode");
            seekTo (0);

            const size_t srcCh = numChannels;
            juce::int64 total = 0;
            const juce::int64 cap = (juce::int64) (1800.0 * sampleRate);   // 最多数 30 分钟
            while (! eof && total < cap)
            {
                decodeChunk();
                // 计入本次新增帧并消费掉（避免重复累计）
                const size_t avail = interleaved.size() / srcCh - framePos;
                total += (juce::int64) avail;
                framePos += avail;
                if (framePos > 0)
                {
                    interleaved.erase (interleaved.begin(),
                                       interleaved.begin() + (std::ptrdiff_t) (framePos * srcCh));
                    framePos = 0;
                }
            }

            if (eof && total > 0)
            {
                lengthInSamples = total;
                MFLOG (juce::String::formatted ("MF: counted duration %.2fs -> %lld samples",
                                                total / sampleRate, (long long) total));
            }
            else
            {
                MFLOG ("MF: sequential count also failed, keep len=0");
            }
            seekTo (0);
            return;
        }

        for (int i = 0; i < 9; ++i)   // 分辨率约 hi/512，3 分钟歌曲约 0.1s
        {
            const double mid = 0.5 * (lo + hi);
            if (probeEos (mid)) hi = mid;
            else                lo = mid;
        }

        lengthInSamples = (juce::int64) (lo * sampleRate);
        MFLOG (juce::String::formatted ("MF: probed duration ~%.2fs -> %lld samples",
                                        lo, (long long) lengthInSamples));
        seekTo (0);
    }
    MFAudioFormatReader (IMFSourceReader* r, bool started, const juce::String& url)
        : juce::AudioFormatReader (nullptr, "MediaFoundation"),
          sourceReader (r), mfStarted (started), sourceUrl (url)
    {
        usesFloatingPointData = false;   // readSamples 输出左对齐 int32，基类 float read() 自动转换
    }

    bool initOutput (const juce::File& file)
    {
        // 显式声明输出格式（48kHz 立体声 float）：强制 MF 组装完整解码链，
        // 包含 HE-AAC 的 SBR 展开 + 声道/采样率转换器。
        // 上一版只声明 major+subtype，遇到 HE-AAC 文件时 MF 不插 SBR 解码器，
        // 输出退化为 12kHz 的静音数据（诊断日志 sr=12000 实锤）。
        HRESULT hr = setOutputType (48000, 2);
        if (FAILED (hr))
        {
            MFLOG (juce::String::formatted ("MF: explicit 48k/stereo rejected hr=0x%x, fallback to auto",
                                            (unsigned) hr));
            hr = setOutputType (0, 0);   // 退回只声明 major+subtype
        }
        if (FAILED (hr))
        {
            MFLOG ("MF: SetCurrentMediaType failed hr=0x" + juce::String::toHexString ((int) hr));
            return false;
        }

        IMFMediaType* actual = nullptr;
        if (FAILED (sourceReader->GetCurrentMediaType (MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)))
        {
            MFLOG ("MF: GetCurrentMediaType failed");
            return false;
        }
        UINT32 channels = 1, rate = 44100;
        actual->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &channels);
        actual->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        actual->Release();

        // 音频流实际采样率异常低（<8000）视为解码链未就绪
        if (rate < 8000)
        {
            MFLOG (juce::String::formatted ("MF: implausible sample rate %u", rate));
            return false;
        }

        numChannels = channels;
        sampleRate = (double) rate;
        bytesPerFrame = (size_t) channels * sizeof (float);

        // 时长 → 总样本数：只用 MF 自己报告的 duration，且做合理性校验。
        // 实测这些 DASH/伪 MP3 文件的 mvhd=0、mdhd=0.457s、码率估算偏 10 倍——
        // 全不可信，宁可信其无（lengthInSamples=0 → 顺序解码到真实 EOF）。
        PROPVARIANT dur;
        PropVariantInit (&dur);
        HRESULT hrDur = sourceReader->GetPresentationAttribute (MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur);
        if (FAILED (hrDur) || dur.vt != VT_I8 || dur.hVal.QuadPart <= 0)
        {
            PropVariantClear (&dur);
            hrDur = sourceReader->GetPresentationAttribute (MF_SOURCE_READER_FIRST_AUDIO_STREAM, MF_PD_DURATION, &dur);
        }
        if (SUCCEEDED (hrDur) && dur.vt == VT_I8 && dur.hVal.QuadPart > 0)
        {
            const double sec = (double) dur.hVal.QuadPart / 1e7;
            // 合理性校验：时长对应的最低码率不得低于 8KB/s（64kbps），否则视为容器元数据损坏
            if (sec * 8000.0 <= (double) file.getSize())
            {
                lengthInSamples = (juce::int64) (sec * sampleRate);
            }
            else
            {
                MFLOG (juce::String::formatted ("MF: duration %.1fs implausible for %lld bytes, probing",
                                                sec, (long long) file.getSize()));
                probeDuration();
            }
        }
        else
        {
            MFLOG ("MF: no duration metadata, probing");
            probeDuration();
        }
        PropVariantClear (&dur);

        MFLOG (juce::String::formatted ("MF: output sr=%.0f ch=%u len=%lld",
                                        sampleRate, (unsigned) numChannels,
                                        (long long) lengthInSamples));
        return sampleRate > 0.0 && numChannels > 0;
    }

    HRESULT setOutputType (UINT32 rate, UINT32 channels)
    {
        IMFMediaType* type = nullptr;
        if (FAILED (MFCreateMediaType (&type)))
            return E_FAIL;
        type->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (rate != 0 && channels != 0)
        {
            type->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            type->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, channels);
            type->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            type->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 4);
            type->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        }
        HRESULT hr = sourceReader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
        type->Release();
        return hr;
    }

    //==========================================================================
    void seekTo (juce::int64 samplePos)
    {
        PROPVARIANT pos;
        PropVariantInit (&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = (juce::int64) ((double) samplePos / sampleRate * 1e7);
        sourceReader->SetCurrentPosition (GUID_NULL, pos);
        interleaved.clear();
        framePos = 0;
        currentPos = samplePos;
        tail.clear();
        tailStart = samplePos;
        eof = false;
    }

    void decodeChunk()
    {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = sourceReader->ReadSample (MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                               nullptr, &flags, nullptr, &sample);
        if (FAILED (hr))
        {
            eof = true;
            return;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || sample == nullptr)
        {
            if (sample != nullptr) sample->Release();
            eof = true;
            return;
        }
        appendSample (sample);
        sample->Release();
    }

    void appendSample (IMFSample* sample)
    {
        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED (sample->ConvertToContiguousBuffer (&buf)))
        {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED (buf->Lock (&data, nullptr, &len)))
            {
                const size_t frames = (size_t) len / bytesPerFrame;
                const float* src = reinterpret_cast<const float*> (data);
                interleaved.insert (interleaved.end(), src, src + frames * numChannels);
                buf->Unlock();
            }
            buf->Release();
        }
    }

    //==========================================================================
    // SetCurrentPosition 对部分容器（DASH/分片 MP4）会静默失效——返回 S_OK 但
    // 解码位置纹丝不动。这里 seek 后读一帧核对时间戳，未生效则重开文件快进。
    bool reliableSeek (juce::int64 samplePos)
    {
        PROPVARIANT pos;
        PropVariantInit (&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = (juce::int64) ((double) samplePos / sampleRate * 1e7);
        HRESULT hr = sourceReader->SetCurrentPosition (GUID_NULL, pos);
        PropVariantClear (&pos);
        if (FAILED (hr))
        {
            MFLOG (juce::String::formatted ("MF: SetCurrentPosition hr=0x%x, reopen",
                                            (unsigned) hr));
            return reopenAndFastForward (samplePos);
        }

        // 读一帧核对时间戳（seek 到 EOF 附近时可能直接 EOS，也算成功）
        DWORD flags = 0;
        LONGLONG ts = -1;
        IMFSample* smp = nullptr;
        hr = sourceReader->ReadSample (MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                       nullptr, &flags, &ts, &smp);
        if (SUCCEEDED (hr) && smp != nullptr)
        {
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
            {
                smp->Release();
                interleaved.clear();
                framePos = 0;
                currentPos = samplePos;
                tail.clear();
                tailStart = samplePos;
                eof = true;
                return true;
            }

            const double tsSec = (double) ts / 1e7;
            const double want  = (double) samplePos / sampleRate;
            if (tsSec >= want - 1.0 && tsSec <= want + 2.0)
            {
                interleaved.clear();
                framePos = 0;
                appendSample (smp);
                smp->Release();
                currentPos = (juce::int64) (tsSec * sampleRate);
                tail.clear();
                tailStart = currentPos;
                eof = false;
                return true;
            }

            MFLOG (juce::String::formatted ("MF: seek verify want=%.1fs got=%.1fs, reopen",
                                            want, tsSec));
            smp->Release();
            return reopenAndFastForward (samplePos);
        }
        if (smp != nullptr) smp->Release();
        return reopenAndFastForward (samplePos);
    }

    // 重开文件并顺序解码丢弃到目标位置（实测 277s 歌曲全程解码约 1s，可接受）
    bool reopenAndFastForward (juce::int64 samplePos)
    {
        IMFSourceReader* r = nullptr;
        HRESULT hr = MFCreateSourceReaderFromURL (sourceUrl.toWideCharPointer(), nullptr, &r);
        if (FAILED (hr) || r == nullptr)
        {
            MFLOG ("MF: reopen failed hr=0x" + juce::String::toHexString ((int) hr));
            return false;
        }
        sourceReader->Release();
        sourceReader = r;

        if (FAILED (setOutputType (48000, 2)))
            setOutputType (0, 0);

        interleaved.clear();
        framePos = 0;
        tail.clear();
        tailStart = samplePos;
        eof = false;

        const juce::int64 toSkip = samplePos;
        juce::int64 skipped = 0;
        int emptyRuns = 0;
        while (! eof && skipped < toSkip)
        {
            if (framePos > 0)
            {
                interleaved.erase (interleaved.begin(),
                                   interleaved.begin() + (std::ptrdiff_t) (framePos * numChannels));
                framePos = 0;
            }
            if (interleaved.empty())
            {
                const size_t before = interleaved.size();
                decodeChunk();
                emptyRuns = (interleaved.size() == before) ? emptyRuns + 1 : 0;
                if (emptyRuns > 64)   // 解码器连续产出空帧：放弃快进
                    break;
                continue;
            }
            const size_t avail = interleaved.size() / numChannels;
            const juce::int64 remain = toSkip - skipped;
            if ((juce::int64) avail <= (juce::int64) remain)
            {
                skipped += (juce::int64) avail;
                interleaved.clear();
            }
            else
            {
                framePos = (size_t) remain;   // 留下的帧正好从目标位置开始
                skipped = toSkip;
            }
        }

        currentPos = samplePos;
        MFLOG (juce::String::formatted ("MF: reopen+ffwd done skipped %.1fs eof=%d",
                                        skipped / sampleRate, (int) eof));
        return true;
    }

    void ensureFrames (int need)
    {
        while (((size_t) interleaved.size() / numChannels - framePos) < (size_t) need && ! eof)
            decodeChunk();

        // 已消费过半就整理缓冲，防止无限增长
        if (framePos > 65536)
        {
            interleaved.erase (interleaved.begin(),
                               interleaved.begin() + (std::ptrdiff_t) (framePos * numChannels));
            framePos = 0;
        }
    }

    bool readSamples (int* const* destChannels, int numDestChannels,
                      int startOffsetInDestBuffer, juce::int64 startSampleInFile,
                      int numSamples) override
    {
        const size_t srcCh = numChannels;
        int written = 0;
        juce::int64 filePos = startSampleInFile;

        // 1) 微小回退（调用方线性插值需要 +1 样本回看，导致每块起点回退 2~3 样本）：
        //    用 tail 缓存服务，绝不触发 MF 的 SetCurrentPosition —— 它是近似的（跳到关键帧），
        //    每块都 seek 会让解码永远从同一帧开始，输出变成循环碎片（实测实锤）。
        if (filePos < currentPos && tailStart <= filePos
            && filePos - tailStart < (juce::int64) (tail.size() / srcCh))
        {
            const size_t skip = (size_t) (filePos - tailStart);
            const size_t avail = tail.size() / srcCh - skip;
            const int take = (int) std::min<juce::int64> ((juce::int64) (numSamples - written),
                                                          (juce::int64) avail);
            for (int ch = 0; ch < numDestChannels; ++ch)
            {
                const size_t s = (srcCh == 1) ? 0 : juce::jmin ((size_t) ch, srcCh - 1);
                int* dst = destChannels[ch] + startOffsetInDestBuffer + written;
                const float* base = tail.data() + skip * srcCh;
                for (int i = 0; i < take; ++i)
                    dst[i] = (int) (juce::jlimit (-1.0f, 1.0f,
                        base[(size_t) i * srcCh + s]) * 2147483647.0f);
            }
            written += take;
            filePos += take;
        }

        // 2) 其余重定位（大幅回退或前跳）
        if (written < numSamples)
        {
            if (filePos < currentPos || filePos - currentPos > 96000)
            {
                if (! reliableSeek (filePos))
                {
                    // seek 彻底失败：按 EOF 处理，填静音
                    for (int ch = 0; ch < numDestChannels; ++ch)
                    {
                        int* dst = destChannels[ch] + startOffsetInDestBuffer;
                        for (int i = written; i < numSamples; ++i)
                            dst[i] = 0;
                    }
                    return false;
                }
            }
            else if (filePos != currentPos)
                currentPos = filePos;   // 小幅前跳：按帧消费自然对齐

            while (written < numSamples)
            {
                ensureFrames (numSamples - written);
                const size_t avail = (size_t) interleaved.size() / srcCh - framePos;
                if (avail == 0)
                    break;   // EOF

                const int take = (int) std::min<juce::int64> ((juce::int64) (numSamples - written),
                                                              (juce::int64) avail);

                for (int ch = 0; ch < numDestChannels; ++ch)
                {
                    const size_t s = (srcCh == 1) ? 0 : juce::jmin ((size_t) ch, srcCh - 1);
                    int* dst = destChannels[ch] + startOffsetInDestBuffer + written;
                    for (int i = 0; i < take; ++i)
                    {
                        const float v = juce::jlimit (-1.0f, 1.0f,
                            interleaved[(size_t) (framePos + i) * srcCh + s]);
                        dst[i] = (int) (v * 2147483647.0f);
                    }
                }

                rememberTail (&interleaved[(size_t) framePos * srcCh], (size_t) take, currentPos);
                framePos += (size_t) take;
                currentPos += take;
                written += take;
                filePos += take;
            }
        }

        if (written < numSamples)   // EOF：剩余填 0
        {
            for (int ch = 0; ch < numDestChannels; ++ch)
            {
                int* dst = destChannels[ch] + startOffsetInDestBuffer;
                for (int i = written; i < numSamples; ++i)
                    dst[i] = 0;
            }
        }

        return written == numSamples;
    }

    // 记录最近解码的帧（连续时追加，供下一次读取的插值回看）
    void rememberTail (const float* frames, size_t count, juce::int64 posOfFirst)
    {
        const size_t srcCh = numChannels;
        tail.insert (tail.end(), frames, frames + count * srcCh);
        const size_t maxFrames = 64;
        if (tail.size() / srcCh > maxFrames)
        {
            const size_t drop = (tail.size() / srcCh - maxFrames) * srcCh;
            tail.erase (tail.begin(), tail.begin() + (std::ptrdiff_t) drop);
        }
        tailStart = posOfFirst + (juce::int64) count - (juce::int64) (tail.size() / srcCh);
    }

    //==========================================================================
    IMFSourceReader* sourceReader = nullptr;
    juce::String sourceUrl;            // seek 失效时重开文件用
    size_t bytesPerFrame = 4;
    std::vector<float> interleaved;    // 交错 float 帧
    size_t framePos = 0;               // 已消费帧偏移
    juce::int64 currentPos = 0;        // 已解码/对齐到的文件样本位置
    std::vector<float> tail;           // 最近解码帧缓存（插值回看，避免微小回退触发 MF seek）
    juce::int64 tailStart = 0;         // tail 第一帧的文件位置
    bool eof = false;
    bool mfStarted = false;
};

#endif // JUCE_WINDOWS

//==============================================================================
class MFAudioFormat : public juce::AudioFormat
{
public:
    MFAudioFormat()
        : juce::AudioFormat ("MediaFoundation",
                             juce::StringArray (".mp4", ".m4a", ".aac", ".wma", ".mp3", ".m4b")) {}

    ~MFAudioFormat() override = default;

    juce::Array<int> getPossibleSampleRates() override
    {
        return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    }
    juce::Array<int> getPossibleBitDepths() override { return { 16, 24, 32 }; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream, bool deleteStreamIfOpeningFailure) override
    {
        std::unique_ptr<juce::InputStream> holder (deleteStreamIfOpeningFailure ? sourceStream : nullptr);

#if JUCE_WINDOWS
        // MF 需要文件路径：优先直接取 FileInputStream 的文件
        juce::File file;
        if (auto* fis = dynamic_cast<juce::FileInputStream*> (sourceStream))
        {
            file = fis->getFile();
        }
        else
        {
            // 非文件流：落到临时文件再解（兜底，极少走到）
            file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getNonexistentChildFile ("mf_", ".audio");
            juce::FileOutputStream out (file);
            out.writeFromInputStream (*sourceStream, -1);
            out.flush();
        }

        if (auto* r = MFAudioFormatReader::create (file))
            return r;
#endif

        if (deleteStreamIfOpeningFailure)
            holder = nullptr;   // 已在析构中删除
        return nullptr;
    }

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&,
                                                              const juce::AudioFormatWriterOptions&) override
    {
        return nullptr;   // 只读
    }
};
