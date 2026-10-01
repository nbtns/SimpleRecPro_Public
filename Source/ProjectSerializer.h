#pragma once
#include <JuceHeader.h>
#include "TrackData.h"

#include <vector>

class AudioEngine;

class ProjectSerializer
{
public:
    static constexpr int currentSchemaVersion = 8;

    struct ProjectSnapshot
    {
        int bpm = 120;
        int timeSignatureNumerator = 4;
        int timeSignatureDenominator = 4;
        double zoomLevel = 50.0;
        float masterVolume = 0.8f;
        bool metronomeEnabled = false;
        float metronomeVolume = 0.5f;
        bool countInEnabled = false;
        ProjectDawSettings dawSettings;
        std::vector<TrackData> tracks;
    };

    using LoadedProjectData = ProjectSnapshot;

    /** Captures a cheap immutable project snapshot for a background save. */
    static ProjectSnapshot captureProjectSnapshot(AudioEngine& audioEngine);

    /** Saves a previously captured snapshot without touching AudioEngine. */
    static bool saveProject(const juce::File& file, const ProjectSnapshot& snapshot);

    /** Loads and validates a project without touching AudioEngine. */
    static bool loadProject(const juce::File& file,
                            LoadedProjectData& loadedProject,
                            juce::String* errorMessage = nullptr);

    /** Applies data returned by the background-safe loadProject overload. */
    static void applyLoadedProject(LoadedProjectData loadedProject, AudioEngine& audioEngine);

    /**
     * 現在のプロジェクト状態をファイルに保存する
     * @param file 保存先のファイル (.srec)
     * @param audioEngine 状態を取得するAudioEngine
     * @return 成功した場合はtrue
     */
    static bool saveProject(const juce::File& file, AudioEngine& audioEngine);

    /**
     * プロジェクトファイルから状態を読み込む
     * @param file 読み込むファイル (.srec)
     * @param audioEngine 状態を設定するAudioEngine
     * @return 成功した場合はtrue
     */
    static bool loadProject(const juce::File& file, AudioEngine& audioEngine);
    
private:
    static juce::String colourToHex(juce::Colour c);
    static juce::Colour hexToColour(const juce::String& hex);
};
