#include "RegionAutomation.h"
#include <algorithm>
#include <cmath>

bool RegionAutomation::isValid(const AudioAutomationRegion& r) noexcept
{
    return std::isfinite(r.startSeconds) && std::isfinite(r.endSeconds)
        && std::isfinite(r.gainDb) && std::isfinite(r.brightness)
        && std::isfinite(r.ambience) && std::isfinite(r.fadeSeconds)
        && r.startSeconds >= 0.0 && r.endSeconds <= 86400.0
        && r.endSeconds > r.startSeconds
        && r.gainDb >= -24.0f && r.gainDb <= 12.0f
        && r.brightness >= -1.0f && r.brightness <= 1.0f
        && r.ambience >= 0.0f && r.ambience <= 1.0f
        && r.fadeSeconds >= 0.0
        && r.fadeSeconds <= std::min(10.0, (r.endSeconds - r.startSeconds) * 0.5);
}

juce::var RegionAutomation::toVar(const AudioAutomationRegion& r)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("id", r.id);
    o->setProperty("startSeconds", r.startSeconds);
    o->setProperty("endSeconds", r.endSeconds);
    o->setProperty("gainDb", r.gainDb);
    o->setProperty("brightness", r.brightness);
    o->setProperty("ambience", r.ambience);
    o->setProperty("fadeSeconds", r.fadeSeconds);
    o->setProperty("enabled", r.enabled);
    return juce::var(o);
}

bool RegionAutomation::fromVar(const juce::var& v, AudioAutomationRegion& r)
{
    if (!v.isObject()) return false;
    for (const auto* name : { "startSeconds", "endSeconds", "gainDb",
                               "brightness", "ambience", "fadeSeconds" })
    {
        const auto n = v.getProperty(name, juce::var());
        if (!n.isVoid() && !(n.isInt() || n.isInt64() || n.isDouble())) return false;
    }
    r.id = v.getProperty("id", "automation-" + juce::Uuid().toString()).toString();
    r.startSeconds = static_cast<double>(v.getProperty("startSeconds", 0.0));
    r.endSeconds = static_cast<double>(v.getProperty("endSeconds", 0.0));
    r.gainDb = static_cast<float>(v.getProperty("gainDb", 0.0));
    r.brightness = static_cast<float>(v.getProperty("brightness", 0.0));
    r.ambience = static_cast<float>(v.getProperty("ambience", 0.0));
    r.fadeSeconds = static_cast<double>(v.getProperty("fadeSeconds", 0.03));
    r.enabled = static_cast<bool>(v.getProperty("enabled", true));
    return isValid(r);
}

bool RegionAutomationProcessor::prepare(double sampleRate, int channelCount,
                                         const AudioAutomationRegion& region)
{
    if (!RegionAutomation::isValid(region) || !std::isfinite(sampleRate)
        || sampleRate < 1.0 || sampleRate > 384000.0
        || channelCount < 1 || channelCount > 2) return false;
    settings = region;
    rate = sampleRate;
    channels = channelCount;
    start = static_cast<std::int64_t>(std::llround(settings.startSeconds * rate));
    end = static_cast<std::int64_t>(std::llround(settings.endSeconds * rate));
    fade = static_cast<std::int64_t>(std::llround(settings.fadeSeconds * rate));
    lowCoefficient = static_cast<float>(1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi
                                                    * std::min(1500.0, rate * 0.25) / rate));
    delayLength = { std::max(1, juce::roundToInt(rate * 0.091)),
                    std::max(1, juce::roundToInt(rate * 0.137)) };
    delay.setSize(channels, std::max(delayLength[0], delayLength[1]) + 1);
    reset();
    return true;
}

void RegionAutomationProcessor::reset() noexcept
{
    lowState.fill(0.0f);
    delay.clear();
    writePosition = 0;
    expected = std::numeric_limits<std::int64_t>::min();
}

void RegionAutomationProcessor::process(juce::AudioBuffer<float>& buffer,
                                         int offset, int count,
                                         std::int64_t timeline) noexcept
{
    if (rate <= 0.0 || !settings.enabled || offset < 0 || offset >= buffer.getNumSamples()) return;
    count = std::clamp(count, 0, buffer.getNumSamples() - offset);
    if (timeline != expected) reset();
    expected = timeline + count;
    const auto usedChannels = std::min(channels, buffer.getNumChannels());
    for (int i = 0; i < count; ++i)
    {
        const auto t = timeline + i;
        if (t < start || t >= end) continue;
        const auto edge = std::min(t - start, end - 1 - t);
        const float blend = fade <= 0 ? 1.0f : static_cast<float>(std::clamp(
            static_cast<double>(edge) / static_cast<double>(fade), 0.0, 1.0));
        const float gain = juce::Decibels::decibelsToGain(settings.gainDb * blend);
        for (int ch = 0; ch < usedChannels; ++ch)
        {
            float dry = buffer.getSample(ch, offset + i);
            if (!std::isfinite(dry)) dry = 0.0f;
            auto& low = lowState[static_cast<size_t>(ch)];
            low += lowCoefficient * (dry - low);
            const float bright = settings.brightness >= 0.0f
                ? dry + settings.brightness * (dry - low)
                : dry + settings.brightness * (dry - low) * 0.85f;
            int read = writePosition - delayLength[static_cast<size_t>(ch)];
            if (read < 0) read += delay.getNumSamples();
            const float echo = delay.getSample(ch, read);
            delay.setSample(ch, writePosition, bright + echo * 0.3f);
            const float wet = bright + echo * settings.ambience * 0.45f;
            buffer.setSample(ch, offset + i, (dry + (wet - dry) * blend) * gain);
        }
        if (++writePosition >= delay.getNumSamples()) writePosition = 0;
    }
}
