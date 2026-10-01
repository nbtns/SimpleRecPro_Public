#pragma once
#include <JuceHeader.h>
#include "TempoMap.h"
#include <atomic>

/**
 * MetronomeEngine - メトロノーム機能
 * Web版のMetronomeEngine.tsに相当
 * 
 * OscillatorNodeの代わりにJUCEのオーディオバッファで直接クリック音を生成。
 * AudioEngineのprocessBlock()から呼ばれ、リアルタイムスレッドで動作する。
 */
class MetronomeEngine
{
public:
    MetronomeEngine();
    ~MetronomeEngine();
    
    /** 初期化 */
    void prepare(double sampleRate);
    
    /** メトロノームのクリック音をバッファに書き込む */
    void processBlock(juce::AudioBuffer<float>& buffer, int numSamples,
                      int64_t playbackPositionInSamples,
                      bool forceEnabled = false);

    /** Adds clicks using the same cumulative beat grid as the project UI. */
    void processBlock(juce::AudioBuffer<float>& buffer, int numSamples,
                      int64_t playbackPositionInSamples,
                      const TempoMap& tempoMap,
                      bool forceEnabled = false);
    
    /** BPMを設定 */
    void setBpm(int newBpm) { bpm.store(juce::jlimit(20, 300, newBpm)); }
    int getBpm() const { return bpm.load(); }
    
    /** 拍子を設定 */
    void setTimeSignature(int numerator, int denominator);
    int getNumerator() const { return timeSignatureNumerator.load(); }
    int getDenominator() const { return timeSignatureDenominator.load(); }
    
    /** クリック有効/無効 */
    void setEnabled(bool enabled) { isEnabled.store(enabled); }
    bool getEnabled() const { return isEnabled.load(); }
    
    /** クリック音量 */
    void setVolume(float vol) { volume.store(juce::jlimit(0.0f, 1.0f, vol)); }
    float getVolume() const { return volume.load(); }

private:
    /** クリック音の1サンプルを生成 */
    float generateClickSample(int sampleIndex, bool isAccent) const;
    
    double currentSampleRate = 44100.0;
    std::atomic<int> bpm { 120 };
    std::atomic<int> timeSignatureNumerator { 4 };
    std::atomic<int> timeSignatureDenominator { 4 };
    std::atomic<bool> isEnabled { false };
    std::atomic<float> volume { 0.5f };
    
    // クリック音のパラメータ
    int clickDurationSamples = 0; // クリック音の長さ（サンプル数）
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetronomeEngine)
};
