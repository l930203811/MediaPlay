#pragma once

#include <JuceHeader.h>
#include <vector>

// LRC 歌词解析：支持普通逐句格式 [mm:ss.xx]歌词，
// 以及增强逐字格式 [mm:ss.xx]<mm:ss.xx>字<mm:ss.xx>字...
struct LyricWord
{
    double t0 = 0.0, t1 = 0.0;
    juce::String text;
};

struct LyricLine
{
    double t0 = 0.0, t1 = 0.0;             // 行起止（t1 = 下一行起点，供逐句 KTV 渐染用）
    juce::String text;
    std::vector<LyricWord> words;          // 非空 = 该行支持逐字显示
};

class Lyrics
{
public:
    // 在音频同目录找同名 .lrc；失败时保持空状态
    bool loadForFile (const juce::File& audio)
    {
        clear();
        if (! audio.existsAsFile())
            return false;
        auto lrc = audio.getParentDirectory().getChildFile (
                       audio.getFileNameWithoutExtension() + ".lrc");
        if (! lrc.existsAsFile())
            return false;
        return parse (lrc.loadFileAsString());
    }

    void clear()
    {
        lines.clear();
        loaded = false;
        wordLevelAny = false;
    }

    // 全部时间轴平移（秒）：正 = 歌词推迟显示，负 = 提前
    void applyTimeOffset (double s)
    {
        if (s == 0.0 || ! loaded) return;
        for (auto& L : lines)
        {
            L.t0 += s;
            L.t1 += s;
            for (auto& w : L.words)
            {
                w.t0 += s;
                w.t1 += s;
            }
        }
    }

    bool isLoaded() const        { return loaded && ! lines.empty(); }
    bool hasWordTiming() const   { return wordLevelAny; }   // 整体是否存在逐字歌词
    int getLineCount() const     { return (int) lines.size(); }
    const LyricLine* line (int i) const
    {
        return (i >= 0 && i < (int) lines.size()) ? &lines[(size_t) i] : nullptr;
    }

    // 当前时间所在行（最后一行 t0 <= t）；尚未进入第一行返回 -1
    int lineIndexAt (double t) const
    {
        int idx = -1;
        for (int i = 0; i < (int) lines.size(); ++i)
        {
            if (lines[(size_t) i].t0 <= t) idx = i;
            else break;
        }
        return idx;
    }

private:
    static double parseTs (const juce::String& s)
    {
        // 兼容 mm:ss / mm:ss.xx / mm:ss.xxx / 纯秒
        auto parts = juce::StringArray::fromTokens (s, ":", "");
        if (parts.size() >= 2)
            return parts[0].getDoubleValue() * 60.0 + parts[1].getDoubleValue();
        if (parts.size() == 1)
            return parts[0].getDoubleValue();
        return -1.0;
    }

    bool parse (const juce::String& content)
    {
        double offsetSec = 0.0;
        for (const auto& raw : juce::StringArray::fromLines (content))
        {
            auto rest = raw.trim();
            if (rest.isEmpty()) continue;

            // 提取行首连续的 [..] 标签
            juce::Array<double> times;
            while (rest.startsWith ("["))
            {
                const int close = rest.indexOf ("]");
                if (close < 0) break;
                const auto tag = rest.substring (1, close);
                if (! tag.isEmpty() && juce::CharacterFunctions::isDigit (tag[0]))
                {
                    const double t = parseTs (tag);
                    if (t >= 0.0) times.add (t);
                }
                else if (tag.startsWithIgnoreCase ("offset:"))
                {
                    offsetSec = tag.fromFirstOccurrenceOf (":", false, false)
                                    .getDoubleValue() / 1000.0;
                }
                rest = rest.substring (close + 1);
            }
            rest = rest.trim();
            if (times.isEmpty()) continue;      // [ti:]/[ar:] 等元数据行忽略

            // 逐字标签分段：<mm:ss.xx>字...<mm:ss.xx>字...
            std::vector<std::pair<double, juce::String>> segs;
            juce::String acc;
            double pendingT = -1.0;
            int i = 0;
            while (i < rest.length())
            {
                if (rest[i] == '<')
                {
                    const int close = rest.indexOf (i, ">");
                    if (close > i)
                    {
                        const auto inner = rest.substring (i + 1, close);
                        if (! inner.isEmpty() && juce::CharacterFunctions::isDigit (inner[0]))
                        {
                            if (pendingT >= 0.0)
                                segs.push_back ({ pendingT, acc });
                            pendingT = parseTs (inner);
                            acc = juce::String();
                            i = close + 1;
                            continue;
                        }
                    }
                }
                acc += rest[i];
                ++i;
            }
            if (pendingT >= 0.0)
                segs.push_back ({ pendingT, acc });

            juce::String text;
            std::vector<LyricWord> words;
            if (! segs.empty())
            {
                for (size_t k = 0; k < segs.size(); ++k)
                {
                    LyricWord w;
                    w.t0 = segs[k].first;
                    w.t1 = (k + 1 < segs.size()) ? segs[k + 1].first
                                                 : segs[k].first + 4.0;
                    w.text = segs[k].second;
                    words.push_back (w);
                    text += segs[k].second;
                }
                text = text.trimEnd();
                wordLevelAny = true;
            }
            else
            {
                text = rest;
            }

            // 同一行文本可带多个时间戳（重复演唱段）
            for (const auto t : times)
            {
                LyricLine L;
                L.t0 = t + offsetSec;
                L.text = text;
                L.words = words;
                lines.push_back (L);
            }
        }

        if (lines.empty()) return false;
        std::sort (lines.begin(), lines.end(),
                   [] (const LyricLine& a, const LyricLine& b) { return a.t0 < b.t0; });

        // 行结束时间 = 下一行起点；末行给 6s 展示尾
        for (size_t k = 0; k + 1 < lines.size(); ++k)
        {
            lines[k].t1 = juce::jmax (lines[k].t0 + 0.4, lines[k + 1].t0);
            for (auto& w : lines[k].words)
                w.t1 = juce::jmin (juce::jmax (w.t0 + 0.05, w.t1), lines[k].t1);
        }
        lines.back().t1 = lines.back().t0 + 6.0;

        loaded = true;
        return true;
    }

    std::vector<LyricLine> lines;
    bool loaded = false;
    bool wordLevelAny = false;
};
