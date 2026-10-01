#include "AssistantController.h"
#include "AutoMixAnalyzer.h"
#include "ReferenceMixAnalyzer.h"
#include "SimpleMixProcessor.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>

namespace
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* result = new juce::DynamicObject();
    for (const auto& property : properties)
        result->setProperty(property.first, property.second);
    return juce::var(result);
}
juce::String tr(const char* text) { return juce::String::fromUTF8(text); }
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
double number(const juce::var& args, const char* name, double fallback,
              double minimum, double maximum)
{
    const auto value = args.getProperty(name, juce::var());
    if (value.isVoid()) return fallback;
    require(value.isInt() || value.isInt64() || value.isDouble(), u8"数値で指定してください。");
    const auto result = static_cast<double>(value);
    require(std::isfinite(result) && result >= minimum && result <= maximum,
            u8"値が指定できる範囲を超えています。");
    return result;
}
juce::String stringArg(const juce::var& args, const char* name,
                       const juce::String& fallback = {})
{
    const auto value = args.getProperty(name, juce::var());
    if (value.isVoid()) return fallback;
    require(value.isString(), u8"文字列で指定してください。");
    return value.toString();
}
bool has(const juce::var& value, const char* name) { return value.hasProperty(name); }
juce::var mixJson(const TrackData& track)
{
    return object({ { "volume", track.volume }, { "gain_db", juce::Decibels::gainToDecibels(track.volume, -100.0f) },
                    { "pan", track.pan }, { "enabled", track.simpleMix.enabled },
                    { "brightness", track.simpleMix.brightness }, { "ambience", track.simpleMix.ambience },
                    { "stability", track.simpleMix.stability }, { "noise_reduction", track.noiseReduction.amount },
                    { "de_ess", track.noiseReduction.deEssAmount } });
}
juce::var analysisJson(const AutoMixAnalysis& value)
{
    return object({ { "valid", value.valid }, { "analysed_seconds", value.analysedSeconds },
        { "rms_db", value.rmsDb }, { "peak_db", value.peakDb },
        { "brightness", value.brightness }, { "sibilance", value.sibilance },
        { "noise_floor_db", value.noiseFloorDb },
        { "measurement", "source_audio_before_effects" } });
}
int indexForId(const std::vector<TrackData>& tracks, const juce::String& id)
{
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == id) return static_cast<int>(i);
    return -1;
}
float lerp(float before, float after, double factor)
{
    return static_cast<float>(before + (after - before) * factor);
}
}

struct AssistantController::Impl : private juce::Timer
{
    struct Edit
    {
        juce::String label;
        std::vector<TrackData> before, after, originalAfter;
    };
    struct Job
    {
        juce::String id, kind, trackId;
        uint64_t revision = 0;
        double strength = 1.0;
        std::mutex mutex;
        bool ready = false, settled = false;
        juce::var result;
        ReferenceMixResult reference;
        float beforeGain = 1.0f, afterGain = 1.0f;
    };
    struct Comparison
    {
        int phase = 0; // 1/2: exports, 3: analysis, 4: ready
        juce::File beforeFile, afterFile;
        juce::String requestedMode;
        juce::String selectionKey;
        std::vector<TrackData> before, after;
        uint64_t revision = 0;
        double previousTime = 0.0;
        double rangeStart = 0.0, rangeEnd = 0.0;
        std::shared_ptr<Job> job;
    };

    AudioEngine& engine;
    std::function<int()> selectedTrack;
    std::function<void(int)> selectTrack;
    std::function<bool()> externalBusy;
    juce::File preferencesFile;
    juce::var preferences;
    std::vector<Edit> history;
    size_t historyPosition = 0;
    uint64_t expectedRevision = 0;
    std::map<juce::String, std::vector<TrackData>> variants;
    uint64_t variantRevision = 0;
    bool variantApplied = false;
    std::map<juce::String, std::shared_ptr<Job>> jobs;
    std::shared_ptr<Job> latestJob;
    Comparison comparison;
    juce::CriticalSection previewLock;
    juce::AudioTransportSource preview;
    juce::TimeSliceThread previewReadAhead { "Assistant preview audio" };
    std::unique_ptr<juce::AudioFormatReaderSource> previewSource;
    std::atomic<bool> previewActive { false };
    juce::String summary = tr(u8"トラックと範囲を選んで、音の調整を依頼できます。");
    juce::ThreadPool workers { 1 };

    Impl(AudioEngine& e, std::function<int()> getter, std::function<void(int)> setter,
         juce::File file)
        : engine(e), selectedTrack(std::move(getter)), selectTrack(std::move(setter)),
          preferencesFile(std::move(file)), expectedRevision(e.getContentRevision())
    {
        preferences = object({});
        if (preferencesFile.existsAsFile() && preferencesFile.getSize() <= 1024 * 1024)
        {
            const auto parsed = juce::JSON::parse(preferencesFile);
            if (parsed.isObject()) preferences = parsed;
        }
        previewReadAhead.startThread(juce::Thread::Priority::background);
        startTimer(50);
    }
    ~Impl() override
    {
        stopTimer();
        stopPreview();
        if (comparison.phase == 1 || comparison.phase == 2) engine.cancelExport();
        workers.removeAllJobs(true, 10000);
        preview.setSource(nullptr);
        previewReadAhead.stopThread(5000);
    }

    void stopPreview()
    {
        previewActive.store(false);
        const juce::ScopedLock lock(previewLock);
        preview.stop();
    }
    bool busy() const { return comparison.phase > 0 && comparison.phase < 4; }
    void ensureNotRecording() const
    {
        require(engine.getPlaybackState() != PlaybackState::Recording
                && engine.getPlaybackState() != PlaybackState::CountIn,
                u8"録音・カウントイン中は音を変更できません。停止してから実行してください。");
    }
    void ensureEditable()
    {
        ensureNotRecording();
        require(!externalBusy || !externalBusy(), u8"保存・読込またはほかの解析処理中です。完了してから実行してください。");
        require(!busy(), u8"比較用音声を準備中です。完了か停止を待ってください。");
        require(!engine.getExportStatus().active, u8"書き出し中は変更できません。");
        stopPreview();
    }
    bool fresh() const { return expectedRevision == engine.getContentRevision(); }
    juce::String selectionKey() const
    {
        const auto daw = engine.getProjectDawSettings();
        return juce::String(selectedTrack ? selectedTrack() : -1) + ":" + engine.getSelectedClipId()
            + ":" + juce::String(daw.hasLoopStart ? 1 : 0) + ":" + juce::String(daw.hasLoopEnd ? 1 : 0)
            + ":" + juce::String(daw.loopStartSeconds, 12) + ":" + juce::String(daw.loopEndSeconds, 12);
    }
    juce::var comparisonState() const
    {
        const bool preparing = busy();
        const bool current = fresh();
        const bool hasHistory = historyPosition > 0 && historyPosition <= history.size();
        bool hasAudio = false;
        // Inspect immutable metadata only; never copy the project or scan samples.
        if (current && hasHistory && engine.getDuration() > 0.0)
            for (const auto& track : history[historyPosition - 1].after)
            {
                for (const auto& clip : track.clips)
                    if (clip.buffer != nullptr && clip.buffer->getNumSamples() > 0
                        && clip.buffer->getNumChannels() > 0 && clip.duration > 0.0)
                    {
                        hasAudio = true;
                        break;
                    }
                if (hasAudio) break;
            }
        const auto playback = engine.getPlaybackState();
        const bool recording = playback == PlaybackState::Recording || playback == PlaybackState::CountIn;
        const bool otherBusy = externalBusy && externalBusy();
        const bool otherExport = engine.getExportStatus().active && !preparing;
        const auto daw = engine.getProjectDawSettings();
        const bool incompleteRange = daw.hasLoopStart != daw.hasLoopEnd;
        const bool invalidRange = daw.hasLoopStart && daw.hasLoopEnd
            && (!std::isfinite(daw.loopStartSeconds) || !std::isfinite(daw.loopEndSeconds)
                || daw.loopStartSeconds < 0.0 || daw.loopEndSeconds <= daw.loopStartSeconds
                || daw.loopEndSeconds > engine.getDuration() + 0.001);
        const bool available = current && hasHistory && hasAudio && !recording && !otherBusy
            && !otherExport && !incompleteRange && !invalidRange;
        const bool playing = previewActive.load() && comparison.phase == 4
            && comparison.revision == engine.getContentRevision()
            && comparison.selectionKey == selectionKey() && playback == PlaybackState::Stopped;
        const auto mode = preparing || playing ? comparison.requestedMode : juce::String("stop");
        juce::String message;
        if (preparing) message = tr(u8"比較する音声を準備しています。停止ボタンで中止できます。");
        else if (playing) message = comparison.requestedMode == "before"
            ? tr(u8"変更前を音量を合わせて試聴しています。") : tr(u8"変更後を音量を合わせて試聴しています。");
        else if (recording) message = tr(u8"録音が終わると比較できます。");
        else if (otherBusy || otherExport) message = tr(u8"保存・読込・書き出し・解析の完了を待ってください。");
        else if (!hasHistory) message = tr(u8"チャットで音を調整すると、変更前と変更後を比較できます。");
        else if (!current) message = tr(u8"手動編集後は、チャットで新しく調整すると比較できます。");
        else if (!hasAudio) message = tr(u8"比較できる音声がありません。");
        else if (incompleteRange || invalidRange) message = tr(u8"A–B範囲の開始と終了を音声内に設定してください。");
        else message = tr(u8"変更前と変更後を、音量を合わせて比較できます。");
        return object({ { "available", available }, { "busy", preparing }, { "playing", playing },
            { "mode", mode }, { "message", message } });
    }
    void requireHistory()
    {
        require(fresh(), u8"手動編集またはプロジェクト変更がありました。古い変更を戻さず、現在の音から新しく調整してください。");
    }
    int target(const juce::var& args, const std::vector<TrackData>& tracks) const
    {
        const auto id = stringArg(args, "track_id");
        const auto index = id.isNotEmpty() ? indexForId(tracks, id)
                                          : (selectedTrack ? selectedTrack() : -1);
        require(juce::isPositiveAndBelow(index, static_cast<int>(tracks.size())),
                u8"対象トラックを選択するか、存在するtrack_idを指定してください。");
        return index;
    }
    std::pair<double, double> range(const juce::var& args, const TrackData& track) const
    {
        double start = 0.0, end = 0.0;
        const bool explicitRange = has(args, "start_seconds") || has(args, "end_seconds");
        if (explicitRange)
        {
            require(has(args, "start_seconds") && has(args, "end_seconds"),
                    u8"開始と終了の両方を指定してください。");
            start = number(args, "start_seconds", 0.0, 0.0, 86400.0);
            end = number(args, "end_seconds", 0.0, 0.0, 86400.0);
        }
        else
        {
            const auto settings = engine.getProjectDawSettings();
            if (settings.hasLoopStart && settings.hasLoopEnd)
            {
                start = settings.loopStartSeconds;
                end = settings.loopEndSeconds;
            }
            else
            {
                const auto id = engine.getSelectedClipId();
                const auto clip = std::find_if(track.clips.begin(), track.clips.end(),
                    [&id](const AudioClip& c) { return c.id == id; });
                require(clip != track.clips.end(), u8"対象トラックのクリップかA–B範囲を選択してください。");
                start = clip->startTime / track.playbackSpeed;
                end = (clip->startTime + clip->duration) / track.playbackSpeed;
            }
        }
        require(std::isfinite(start) && std::isfinite(end) && start >= 0.0
                && end > start + 0.001 && end <= track.getDuration() + 0.001,
                u8"選択範囲が空か、対象トラックの長さを超えています。");
        return { start, end };
    }
    void apply(std::vector<TrackData> after, const juce::String& label)
    {
        ensureEditable();
        if (!fresh()) { history.clear(); historyPosition = 0; variants.clear(); }
        const auto before = engine.getTracksSnapshot();
        engine.beginEdit();
        engine.restoreTracksState(after);
        engine.endEdit();
        history.resize(historyPosition);
        history.push_back({ label, before, after, after });
        if (history.size() > 24) history.erase(history.begin());
        historyPosition = history.size();
        expectedRevision = engine.getContentRevision();
        variants.clear();
        comparison.phase = 0;
        summary = label;
    }
    juce::var success() const
    {
        return object({ { "ok", true }, { "message", summary },
                        { "revision", static_cast<juce::int64>(engine.getContentRevision()) } });
    }
    juce::var jobJson(const std::shared_ptr<Job>& job) const
    {
        if (job == nullptr) return {};
        const std::lock_guard<std::mutex> lock(job->mutex);
        return object({ { "job_id", job->id }, { "action", job->kind },
                        { "status", job->settled ? (job->result.hasProperty("error") ? "failed" : "completed") : "running" },
                        { "result", job->settled ? job->result : juce::var() } });
    }
    std::shared_ptr<Job> newJob(const juce::String& kind)
    {
        // Bound retained reports; running jobs are never evicted.
        if (jobs.size() >= 32)
            for (auto iterator = jobs.begin(); iterator != jobs.end();)
                if (iterator->second->settled && iterator->second != latestJob) iterator = jobs.erase(iterator);
                else ++iterator;
        require(jobs.size() < 32, u8"実行中の処理が多すぎます。完了を待ってください。");
        auto job = std::make_shared<Job>();
        job->id = juce::Uuid().toString();
        job->kind = kind;
        job->revision = engine.getContentRevision();
        jobs.emplace(job->id, job);
        latestJob = job;
        return job;
    }
    juce::var context() const
    {
        juce::Array<juce::var> allTracks;
        const auto tracks = engine.getTracksSnapshot();
        for (const auto& track : tracks)
        {
            juce::Array<juce::var> clips, regions, effects;
            for (const auto& clip : track.clips)
                clips.add(object({ { "clip_id", clip.id }, { "name", clip.name },
                    { "start_seconds", clip.startTime / track.playbackSpeed },
                    { "end_seconds", (clip.startTime + clip.duration) / track.playbackSpeed } }));
            for (const auto& region : track.automationRegions)
                regions.add(object({ { "region_id", region.id }, { "start_seconds", region.startSeconds },
                    { "end_seconds", region.endSeconds }, { "gain_db", region.gainDb },
                    { "brightness", region.brightness }, { "ambience", region.ambience } }));
            const juce::StringArray effectNames { "none", "muffled", "radio", "noise", "distortion", "bitcrush" };
            for (const auto& effect : track.specialFxRegions)
                effects.add(object({ { "region_id", effect.id },
                    { "type", effectNames[juce::jlimit(0, 5, static_cast<int>(effect.type))] },
                    { "start_seconds", effect.startSeconds }, { "end_seconds", effect.endSeconds },
                    { "amount", effect.amount }, { "wet", effect.wet },
                    { "fade_seconds", effect.fadeSeconds }, { "enabled", effect.enabled } }));
            allTracks.add(object({ { "track_id", track.id }, { "id", track.id }, { "name", track.name },
                { "duration_seconds", track.getDuration() }, { "settings", mixJson(track) },
                { "clips", clips }, { "automation_regions", regions }, { "special_fx_regions", effects },
                { "muted", track.isMuted }, { "solo", track.isSolo } }));
        }
        juce::Array<juce::var> edits;
        if (fresh()) for (size_t i = 0; i < history.size(); ++i)
            edits.add(object({ { "label", history[i].label }, { "applied", i < historyPosition } }));
        const auto selected = selectedTrack ? selectedTrack() : -1;
        const auto daw = engine.getProjectDawSettings();
        auto selection = object({ { "has_start", daw.hasLoopStart }, { "has_end", daw.hasLoopEnd },
            { "start_seconds", daw.loopStartSeconds }, { "end_seconds", daw.loopEndSeconds },
            { "source", daw.hasLoopStart && daw.hasLoopEnd ? "ab" : "none" } });
        if (!(daw.hasLoopStart && daw.hasLoopEnd)
            && juce::isPositiveAndBelow(selected, static_cast<int>(tracks.size())))
        {
            const auto& track = tracks[static_cast<size_t>(selected)];
            for (const auto& clip : track.clips)
                if (clip.id == engine.getSelectedClipId())
                    selection = object({ { "has_start", true }, { "has_end", true },
                        { "start_seconds", clip.startTime / track.playbackSpeed },
                        { "end_seconds", (clip.startTime + clip.duration) / track.playbackSpeed }, { "source", "clip" } });
        }
        return object({ { "tracks", allTracks },
            { "selected_track_id", juce::isPositiveAndBelow(selected, static_cast<int>(tracks.size())) ? tracks[static_cast<size_t>(selected)].id : juce::String() },
            { "selected_clip_id", engine.getSelectedClipId() },
            { "selection", selection },
            { "position_seconds", engine.getCurrentTime() },
            { "playback_state", static_cast<int>(engine.getPlaybackState()) },
            { "revision", static_cast<juce::int64>(engine.getContentRevision()) },
            { "history", edits }, { "history_stale", !fresh() }, { "latest_job", jobJson(latestJob) },
            { "preview_busy", busy() }, { "preview_playing", previewActive.load() },
            { "comparison", comparisonState() },
            { "selection_controls", juce::StringArray { "gain_db", "brightness", "ambience" } } });
    }
    void adjustTrack(TrackData& track, const juce::var& args, bool selection)
    {
        const auto gain = number(args, "gain_db", 0.0, -24.0, 12.0);
        const auto brightness = number(args, "brightness", track.simpleMix.brightness, -1.0, 1.0);
        const auto ambience = number(args, "ambience", track.simpleMix.ambience, 0.0, 1.0);
        const auto stability = number(args, "stability", track.simpleMix.stability, 0.0, 1.0);
        const auto noise = number(args, "noise_reduction", track.noiseReduction.amount, 0.0, 1.0);
        const auto deEss = number(args, "de_ess", track.noiseReduction.deEssAmount, 0.0, 1.0);
        require(has(args, "gain_db") || has(args, "brightness") || has(args, "ambience")
                || has(args, "stability") || has(args, "noise_reduction") || has(args, "de_ess"),
                u8"変更する音の設定を指定してください。");
        if (selection)
        {
            require(!has(args, "stability") && !has(args, "noise_reduction") && !has(args, "de_ess"),
                    u8"範囲指定では音量・明るさ・残響を変更できます。安定感・ノイズ・歯擦音はトラック全体で指定してください。");
            const auto selectedRange = range(args, track);
            auto existing = track.automationRegions.end();
            for (auto candidate = track.automationRegions.begin(); candidate != track.automationRegions.end(); ++candidate)
            {
                const bool same = std::abs(candidate->startSeconds - selectedRange.first) < 1.0e-6
                    && std::abs(candidate->endSeconds - selectedRange.second) < 1.0e-6;
                if (same)
                {
                    require(existing == track.automationRegions.end(), u8"同じ範囲の調整が複数あります。範囲を整理してから実行してください。");
                    existing = candidate;
                }
                else if (candidate->enabled)
                    require(candidate->endSeconds <= selectedRange.first || candidate->startSeconds >= selectedRange.second,
                        u8"既存の範囲調整と一部が重なります。既存と同じ範囲か、重ならない範囲を指定してください。");
            }
            if (existing != track.automationRegions.end())
            {
                const auto combinedGain = existing->gainDb + gain;
                require(combinedGain >= -24.0 && combinedGain <= 12.0, u8"範囲の合計音量調整が上限を超えます。");
                if (has(args, "gain_db")) existing->gainDb = static_cast<float>(combinedGain);
                if (has(args, "brightness")) existing->brightness = static_cast<float>(brightness);
                if (has(args, "ambience")) existing->ambience = static_cast<float>(ambience);
                existing->enabled = true;
                return;
            }
            require(track.automationRegions.size() < 256, u8"範囲調整が上限に達しました。");
            AudioAutomationRegion region;
            region.id = juce::Uuid().toString();
            region.startSeconds = selectedRange.first;
            region.endSeconds = selectedRange.second;
            region.gainDb = static_cast<float>(gain);
            region.brightness = static_cast<float>(has(args, "brightness") ? brightness : 0.0);
            region.ambience = static_cast<float>(has(args, "ambience") ? ambience : 0.0);
            region.fadeSeconds = std::min(0.03, (region.endSeconds - region.startSeconds) / 2.0);
            region.enabled = true;
            track.automationRegions.push_back(region);
            return;
        }
        if (has(args, "gain_db"))
        {
            const auto volume = track.volume * juce::Decibels::decibelsToGain(static_cast<float>(gain));
            require(volume >= 0.0f && volume <= 1.0f, u8"音量がトラックの上限を超えます。小さい変更量を指定してください。");
            track.volume = volume;
        }
        if (has(args, "brightness") || has(args, "ambience") || has(args, "stability"))
        {
            track.simpleMix.enabled = true;
            track.simpleMix.preset = MixPreset::flat;
            track.simpleMix.brightness = static_cast<float>(brightness);
            track.simpleMix.ambience = static_cast<float>(ambience);
            track.simpleMix.stability = static_cast<float>(stability);
        }
        if (has(args, "noise_reduction") || has(args, "de_ess"))
        {
            track.noiseReduction.enabled = true;
            track.noiseReduction.amount = static_cast<float>(noise);
            track.noiseReduction.deEssAmount = static_cast<float>(deEss);
        }
    }
    bool selectionScope(const juce::var& args) const
    {
        const auto scope = stringArg(args, "scope", "track");
        require(scope == "track" || scope == "selection", u8"scopeはtrackかselectionを指定してください。");
        return scope == "selection";
    }

    void settleJobs()
    {
        for (const auto& item : jobs)
        {
            auto& job = *item.second;
            const std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.ready || job.settled) continue;
            try
            {
                if (job.kind == "match_reference" && !job.result.hasProperty("error"))
                {
                    require(job.revision == engine.getContentRevision(),
                            u8"参考曲の解析中に編集されました。現在の音で再実行してください。");
                    if (job.strength == 0.0)
                    {
                        job.result = object({ { "ok", true }, { "message", tr(u8"強さが0のため音は変更していません。") } });
                        job.settled = true;
                        continue;
                    }
                    auto tracks = engine.getTracksSnapshot();
                    const auto index = indexForId(tracks, job.trackId);
                    require(index >= 0, u8"解析対象のトラックがなくなりました。");
                    const auto found = std::find_if(job.reference.tracks.begin(), job.reference.tracks.end(),
                        [&job](const ReferenceMixTrackSuggestion& value) { return value.trackId == job.trackId; });
                    require(found != job.reference.tracks.end() && found->analysisValid,
                            u8"対象の音声を解析できませんでした。");
                    auto& track = tracks[static_cast<size_t>(index)];
                    track.simpleMix.enabled = true;
                    track.simpleMix.brightness = juce::jlimit(-1.0f, 1.0f, lerp(track.simpleMix.brightness, found->simpleMix.brightness, job.strength));
                    track.simpleMix.ambience = juce::jlimit(0.0f, 1.0f, lerp(track.simpleMix.ambience, found->simpleMix.ambience, job.strength));
                    track.simpleMix.stability = juce::jlimit(0.0f, 1.0f, lerp(track.simpleMix.stability, found->simpleMix.stability, job.strength));
                    track.noiseReduction.enabled = true;
                    track.noiseReduction.amount = juce::jlimit(0.0f, 1.0f, lerp(track.noiseReduction.amount, found->noiseReduction.amount, job.strength));
                    track.noiseReduction.deEssAmount = juce::jlimit(0.0f, 1.0f, lerp(track.noiseReduction.deEssAmount, found->noiseReduction.deEssAmount, job.strength));
                    track.volume = juce::jlimit(0.0f, 1.0f, lerp(track.volume, found->volume, job.strength));
                    apply(std::move(tracks), tr(u8"参考曲の音色・音量傾向に近づけました。"));
                    job.result = object({ { "ok", true }, { "message", summary },
                        { "confidence", job.reference.overallConfidence }, { "limitation", job.reference.limitation } });
                }
                if (job.kind == "compare" && !job.result.hasProperty("error"))
                {
                    require(comparison.job.get() == &job && comparison.phase == 3
                            && comparison.revision == engine.getContentRevision()
                            && comparison.selectionKey == selectionKey(),
                            u8"比較の準備中に編集されました。比較をやり直してください。");
                    comparison.phase = 4;
                    playComparison(comparison.requestedMode);
                }
            }
            catch (const std::exception& error) { job.result = object({ { "error", tr(error.what()) } }); }
            job.settled = true;
            if (job.result.hasProperty("error"))
            {
                summary = job.result["error"].toString();
                if (job.kind == "compare") comparison.phase = 0;
            }
        }
    }
    void playComparison(const juce::String& mode)
    {
        require(comparison.phase == 4 && comparison.job != nullptr, u8"比較用音声はまだ準備中です。");
        require(comparison.revision == engine.getContentRevision(), u8"音が変更されたため比較を作り直してください。");
        require(comparison.selectionKey == selectionKey(), u8"選択範囲が変更されたため比較を作り直してください。");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(mode == "before" ? comparison.beforeFile : comparison.afterFile));
        require(reader != nullptr, u8"比較用音声を開けませんでした。");
        const auto sampleRate = reader->sampleRate;
        auto source = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
        source->setLooping(true);
        stopPreview();
        engine.stop();
        const juce::ScopedLock lock(previewLock);
        preview.setSource(nullptr);
        previewSource = std::move(source);
        preview.setSource(previewSource.get(), 65536, &previewReadAhead, sampleRate);
        preview.setGain(mode == "before" ? comparison.job->beforeGain : comparison.job->afterGain);
        preview.setPosition(0.0);
        preview.start();
        previewActive.store(true);
        comparison.requestedMode = mode;
        summary = mode == "before" ? tr(u8"変更前を音量補正して試聴中です。") : tr(u8"変更後を音量補正して試聴中です。");
    }
    bool exportComparison(bool before)
    {
        const auto current = engine.getTracksSnapshot();
        const auto wasDirty = engine.isDirty();
        engine.restoreTracksState(before ? comparison.before : comparison.after);
        ExportSettings settings;
        settings.wavBits = 24;
        settings.sampleRate = 44100;
        if (comparison.rangeEnd > comparison.rangeStart)
        {
            settings.target = ExportTarget::abRange;
            settings.rangeStartSeconds = comparison.rangeStart;
            settings.rangeEndSeconds = comparison.rangeEnd;
        }
        const bool started = engine.startExport(before ? comparison.beforeFile : comparison.afterFile, settings);
        engine.restoreTracksState(current);
        if (!wasDirty) engine.markClean();
        expectedRevision = comparison.revision = engine.getContentRevision();
        return started;
    }
    void failComparison(const juce::String& message)
    {
        if (comparison.job != nullptr)
        {
            const std::lock_guard<std::mutex> lock(comparison.job->mutex);
            comparison.job->result = object({ { "error", message } });
            comparison.job->settled = comparison.job->ready = true;
        }
        comparison.phase = 0;
        summary = message;
    }
    void timerCallback() override
    {
        if (engine.getPlaybackState() != PlaybackState::Stopped && preview.isPlaying()) stopPreview();
        if (comparison.phase == 4 && (comparison.revision != engine.getContentRevision()
            || comparison.selectionKey != selectionKey()))
        {
            stopPreview();
            comparison.phase = 0;
            summary = tr(u8"編集または選択範囲の変更に合わせて比較試聴を停止しました。");
        }
        if (busy() && engine.getPlaybackState() != PlaybackState::Stopped)
        {
            if (comparison.phase == 1 || comparison.phase == 2) engine.cancelExport();
            failComparison(tr(u8"通常の再生・録音が始まったため、比較の準備を停止しました。"));
        }
        if (busy() && comparison.selectionKey != selectionKey())
        {
            if (comparison.phase == 1 || comparison.phase == 2) engine.cancelExport();
            failComparison(tr(u8"選択範囲が変わったため、比較の準備を停止しました。"));
        }
        settleJobs();
        if (comparison.phase != 1 && comparison.phase != 2) return;
        if (engine.getContentRevision() != comparison.revision || comparison.selectionKey != selectionKey())
        {
            engine.cancelExport();
            failComparison(tr(u8"比較準備中に編集されたため、準備を中止しました。"));
            return;
        }
        const auto status = engine.getExportStatus();
        if (status.active) return;
        if (!status.completed || !status.succeeded)
        {
            failComparison(status.message.isNotEmpty() ? status.message : tr(u8"比較用音声を書き出せませんでした。"));
            return;
        }
        if (comparison.phase == 1)
        {
            comparison.phase = 2;
            if (!exportComparison(false)) failComparison(tr(u8"変更後の比較音声を書き出せませんでした。"));
            return;
        }
        comparison.phase = 3;
        const auto beforeFile = comparison.beforeFile;
        const auto afterFile = comparison.afterFile;
        const auto job = comparison.job;
        workers.addJob([job, beforeFile, afterFile]
        {
            juce::var result;
            float beforeGain = 1.0f, afterGain = 1.0f;
            try
            {
                const auto measure = [](const juce::File& file)
                {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
                    require(reader != nullptr && reader->lengthInSamples > 0, u8"比較音声を測定できませんでした。");
                    juce::AudioBuffer<float> block(2, 8192);
                    double energy = 0.0, peak = 0.0, count = 0.0;
                    for (juce::int64 position = 0; position < reader->lengthInSamples; position += block.getNumSamples())
                    {
                        const int size = static_cast<int>(std::min<juce::int64>(block.getNumSamples(), reader->lengthInSamples - position));
                        require(reader->read(&block, 0, size, position, true, true), u8"比較音声の読込に失敗しました。");
                        for (int channel = 0; channel < 2; ++channel)
                            for (int i = 0; i < size; ++i)
                            {
                                const double sample = block.getSample(channel, i);
                                require(std::isfinite(sample), u8"比較音声に不正なサンプルがあります。");
                                energy += sample * sample;
                                peak = std::max(peak, std::abs(sample));
                                count += 1.0;
                            }
                    }
                    return std::make_pair(std::sqrt(energy / std::max(1.0, count)), peak);
                };
                const auto before = measure(beforeFile), after = measure(afterFile);
                require(before.first > 1.0e-7 && after.first > 1.0e-7, u8"無音に近いため音量を合わせた比較ができません。");
                const auto targetRms = std::min({ before.first, after.first,
                    before.first * (0.891 / std::max(0.891, before.second)),
                    after.first * (0.891 / std::max(0.891, after.second)) });
                beforeGain = static_cast<float>(targetRms / before.first);
                afterGain = static_cast<float>(targetRms / after.first);
                result = object({ { "ok", true },
                    { "message", tr(u8"処理後の音声のRMSを測定し、音量を合わせて比較試聴しています。") },
                    { "measurement", "processed_export_rms" },
                    { "before_rms_db", juce::Decibels::gainToDecibels(before.first) },
                    { "after_rms_db", juce::Decibels::gainToDecibels(after.first) },
                    { "before_gain", beforeGain }, { "after_gain", afterGain },
                    { "before_file", beforeFile.getFullPathName() }, { "after_file", afterFile.getFullPathName() } });
            }
            catch (const std::exception& error) { result = object({ { "error", tr(error.what()) } }); }
            const std::lock_guard<std::mutex> lock(job->mutex);
            job->result = result;
            job->beforeGain = beforeGain;
            job->afterGain = afterGain;
            job->ready = true;
        });
    }

    juce::var handle(const juce::var& request)
    {
        require(request.isObject(), u8"リクエストはJSONオブジェクトで指定してください。");
        const auto action = stringArg(request, "action");
        const auto args = request.getProperty("args", object({}));
        require(args.isObject(), u8"argsはJSONオブジェクトで指定してください。");
        settleJobs();
        if (action == "get_context") return context();
        if (action == "get_job")
        {
            const auto found = jobs.find(stringArg(args, "job_id"));
            require(found != jobs.end(), u8"指定した処理が見つかりません。");
            return jobJson(found->second);
        }
        if (action == "list_preferences")
        {
            juce::Array<juce::var> entries;
            for (const auto& property : preferences.getDynamicObject()->getProperties())
                entries.add(object({ { "name", property.name.toString() }, { "settings", property.value } }));
            return object({ { "preferences", entries } });
        }
        if (action == "transport")
        {
            const auto command = stringArg(args, "command");
            ensureNotRecording();
            require(!busy(), u8"比較用音声の準備中です。");
            stopPreview();
            if (command == "stop" || command == "pause")
            {
                const auto position = engine.getCurrentTime();
                engine.stop();
                engine.setCurrentTime(command == "pause" ? position : 0.0);
            }
            else if (command == "play") engine.play();
            else if (command == "seek") engine.setCurrentTime(number(args, "position_seconds", 0.0, 0.0, std::max(0.0, engine.getDuration())));
            else if (command == "loop_selection")
            {
                const auto tracks = engine.getTracksSnapshot();
                const auto selectedRange = range(args, tracks[static_cast<size_t>(target(args, tracks))]);
                auto daw = engine.getProjectDawSettings();
                daw.hasLoopStart = daw.hasLoopEnd = daw.loopEnabled = true;
                daw.loopStartSeconds = selectedRange.first;
                daw.loopEndSeconds = selectedRange.second;
                const auto wasFresh = fresh();
                engine.setProjectDawSettings(daw);
                if (wasFresh) expectedRevision = engine.getContentRevision();
                engine.setCurrentTime(selectedRange.first);
                engine.play();
            }
            else require(false, u8"未対応の再生操作です。");
            return object({ { "ok", true } });
        }
        if (action == "compare")
        {
            const auto mode = stringArg(args, "mode", "after");
            require(mode == "before" || mode == "after" || mode == "stop", u8"比較はbefore/after/stopを指定してください。");
            if (mode == "stop")
            {
                const bool wasActive = busy() || previewActive.load();
                stopPreview();
                if (busy())
                {
                    if (comparison.phase == 1 || comparison.phase == 2) engine.cancelExport();
                    failComparison(tr(u8"比較の準備を停止しました。"));
                }
                // Stopping an inactive comparison must not move the normal cursor.
                // A recording or manual edit that started meanwhile owns its transport.
                if (wasActive && engine.getPlaybackState() == PlaybackState::Stopped
                    && comparison.revision == engine.getContentRevision())
                    engine.setCurrentTime(comparison.previousTime);
                return object({ { "ok", true } });
            }
            ensureNotRecording();
            if (busy()) { comparison.requestedMode = mode; return jobJson(comparison.job); }
            require(!externalBusy || !externalBusy(), u8"保存・読込またはほかの解析処理中です。");
            requireHistory();
            require(historyPosition > 0, u8"比較する変更がありません。");
            const auto daw = engine.getProjectDawSettings();
            require(daw.hasLoopStart == daw.hasLoopEnd, u8"A–B範囲の開始と終了を両方設定してください。");
            require(!daw.hasLoopStart || (std::isfinite(daw.loopStartSeconds) && std::isfinite(daw.loopEndSeconds)
                && daw.loopStartSeconds >= 0.0 && daw.loopEndSeconds > daw.loopStartSeconds
                && daw.loopEndSeconds <= engine.getDuration() + 0.001), u8"A–B範囲が音声の外にあります。");
            if (comparison.phase == 4 && comparison.revision == engine.getContentRevision()
                && comparison.selectionKey == selectionKey())
            {
                playComparison(mode);
                return success();
            }
            require(!engine.getExportStatus().active, u8"ほかの書き出しが終わってから比較してください。");
            stopPreview();
            comparison.previousTime = engine.getCurrentTime();
            engine.stop();
            comparison.before = history[historyPosition - 1].before;
            comparison.after = engine.getTracksSnapshot();
            comparison.selectionKey = selectionKey();
            comparison.rangeStart = comparison.rangeEnd = 0.0;
            const auto currentSelection = context()["selection"];
            if (static_cast<bool>(currentSelection["has_start"]) && static_cast<bool>(currentSelection["has_end"]))
            {
                comparison.rangeStart = static_cast<double>(currentSelection["start_seconds"]);
                comparison.rangeEnd = static_cast<double>(currentSelection["end_seconds"]);
            }
            comparison.requestedMode = mode;
            const auto directory = preferencesFile.getParentDirectory().getChildFile("assistant-previews");
            require(directory.createDirectory().wasOk(), u8"比較用音声の保存場所を作れません。");
            comparison.job = newJob("compare");
            comparison.beforeFile = directory.getChildFile(comparison.job->id + "-before.wav");
            comparison.afterFile = directory.getChildFile(comparison.job->id + "-after.wav");
            comparison.phase = 1;
            if (!exportComparison(true))
            {
                failComparison(tr(u8"比較用音声の書き出しを開始できません。"));
                return jobJson(comparison.job);
            }
            summary = tr(u8"処理後の音声を書き出して、比較する音量を測定しています。");
            return jobJson(comparison.job);
        }
        if (action == "select_target")
        {
            ensureNotRecording();
            require(!busy(), u8"比較用音声の準備中です。");
            const auto tracks = engine.getTracksSnapshot();
            auto index = -1;
            const auto clipId = stringArg(args, "clip_id");
            if (clipId.isNotEmpty())
                for (size_t i = 0; i < tracks.size(); ++i)
                    for (const auto& clip : tracks[i].clips)
                        if (clip.id == clipId) index = static_cast<int>(i);
            if (clipId.isNotEmpty()) require(index >= 0, u8"指定されたクリップがありません。");
            if (has(args, "track_id") || index < 0)
            {
                const auto requestedIndex = target(args, tracks);
                require(index < 0 || index == requestedIndex, u8"クリップが指定トラックに属していません。");
                index = requestedIndex;
            }
            auto daw = engine.getProjectDawSettings();
            if (has(args, "start_seconds") || has(args, "end_seconds"))
            {
                const auto selectedRange = range(args, tracks[static_cast<size_t>(index)]);
                daw.hasLoopStart = daw.hasLoopEnd = true;
                daw.loopStartSeconds = selectedRange.first;
                daw.loopEndSeconds = selectedRange.second;
            }
            else if (clipId.isNotEmpty())
            {
                // A newly selected clip must not silently keep an unrelated A-B target.
                daw.hasLoopStart = daw.hasLoopEnd = daw.loopEnabled = false;
            }
            const auto wasFresh = fresh();
            engine.setProjectDawSettings(daw);
            if (wasFresh) expectedRevision = engine.getContentRevision();
            if (selectTrack) selectTrack(index);
            engine.setSelectedClipId(clipId);
            stopPreview();
            comparison.phase = 0;
            return context();
        }
        if (action == "undo_edit" || action == "redo_edit")
        {
            ensureEditable();
            requireHistory();
            if (action == "undo_edit")
            {
                require(historyPosition > 0, u8"戻せる変更がありません。");
                engine.undo();
                --historyPosition;
                summary = tr(u8"直前の依頼を元に戻しました。");
            }
            else
            {
                require(historyPosition < history.size(), u8"やり直せる変更がありません。");
                engine.redo();
                ++historyPosition;
                summary = tr(u8"依頼の変更をやり直しました。");
            }
            expectedRevision = engine.getContentRevision();
            variants.clear();
            comparison.phase = 0;
            return success();
        }
        if (action == "scale_last_edit")
        {
            ensureEditable();
            requireHistory();
            require(historyPosition > 0, u8"強さを変える変更がありません。");
            const auto factor = number(args, "factor", 0.5, 0.0, 2.0);
            auto& edit = history[historyPosition - 1];
            auto scaled = edit.originalAfter;
            for (size_t i = 0; i < scaled.size(); ++i)
            {
                auto& next = scaled[i];
                const auto& before = edit.before[i];
                const auto& after = edit.originalAfter[i];
                next.volume = juce::jlimit(0.0f, 1.0f, before.volume > 0.0f && after.volume > 0.0f
                    ? before.volume * std::pow(after.volume / before.volume, static_cast<float>(factor))
                    : lerp(before.volume, after.volume, factor));
                if (!(before.simpleMix == after.simpleMix))
                {
                    if (before.simpleMix.brightness != after.simpleMix.brightness || before.simpleMix.enabled != after.simpleMix.enabled)
                        next.simpleMix.brightness = juce::jlimit(-1.0f, 1.0f, lerp(before.simpleMix.enabled ? before.simpleMix.brightness : 0.0f, after.simpleMix.brightness, factor));
                    if (before.simpleMix.ambience != after.simpleMix.ambience || before.simpleMix.enabled != after.simpleMix.enabled)
                        next.simpleMix.ambience = juce::jlimit(0.0f, 1.0f, lerp(before.simpleMix.enabled ? before.simpleMix.ambience : 0.0f, after.simpleMix.ambience, factor));
                    if (before.simpleMix.stability != after.simpleMix.stability || before.simpleMix.enabled != after.simpleMix.enabled)
                        next.simpleMix.stability = juce::jlimit(0.0f, 1.0f, lerp(before.simpleMix.enabled ? before.simpleMix.stability : 0.0f, after.simpleMix.stability, factor));
                }
                if (!(before.noiseReduction == after.noiseReduction))
                {
                    if (before.noiseReduction.amount != after.noiseReduction.amount || before.noiseReduction.enabled != after.noiseReduction.enabled)
                        next.noiseReduction.amount = juce::jlimit(0.0f, 1.0f, lerp(before.noiseReduction.enabled ? before.noiseReduction.amount : 0.0f, after.noiseReduction.amount, factor));
                    if (before.noiseReduction.deEssAmount != after.noiseReduction.deEssAmount || before.noiseReduction.enabled != after.noiseReduction.enabled)
                        next.noiseReduction.deEssAmount = juce::jlimit(0.0f, 1.0f, lerp(before.noiseReduction.enabled ? before.noiseReduction.deEssAmount : 0.0f, after.noiseReduction.deEssAmount, factor));
                }
                for (auto& region : next.automationRegions)
                {
                    const auto original = std::find_if(before.automationRegions.begin(), before.automationRegions.end(),
                        [&region](const AudioAutomationRegion& existing) { return existing.id == region.id; });
                    if (original != before.automationRegions.end() && *original == region) continue;
                    const bool wasEnabled = original != before.automationRegions.end() && original->enabled;
                    region.gainDb = juce::jlimit(-24.0f, 12.0f, lerp(wasEnabled ? original->gainDb : 0.0f, region.gainDb, factor));
                    region.brightness = juce::jlimit(-1.0f, 1.0f, lerp(wasEnabled ? original->brightness : 0.0f, region.brightness, factor));
                    region.ambience = juce::jlimit(0.0f, 1.0f, lerp(wasEnabled ? original->ambience : 0.0f, region.ambience, factor));
                }
                for (auto& effect : next.specialFxRegions)
                    if (std::none_of(before.specialFxRegions.begin(), before.specialFxRegions.end(), [&effect](const SpecialFxRegion& existing) { return existing.id == effect.id; }))
                        effect.wet = juce::jlimit(0.0f, 1.0f, static_cast<float>(effect.wet * factor));
                if (factor == 0.0) next = before;
            }
            // Replace the same native undo transaction, so keyboard undo/redo and
            // assistant history cannot disagree about the current edit.
            engine.undo();
            engine.beginEdit();
            engine.restoreTracksState(scaled);
            engine.endEdit();
            edit.after = std::move(scaled);
            history.resize(historyPosition);
            expectedRevision = engine.getContentRevision();
            variants.clear();
            comparison.phase = 0;
            summary = tr(u8"直前の変更の強さを調整しました。");
            return success();
        }

        ensureEditable();
        auto tracks = engine.getTracksSnapshot();
        const auto index = target(args, tracks);
        auto& track = tracks[static_cast<size_t>(index)];
        if (action == "adjust_sound")
        {
            adjustTrack(track, args, selectionScope(args));
            apply(std::move(tracks), stringArg(args, "label", tr(u8"音の設定を調整しました。")));
            return success();
        }
        if (action == "add_effect")
        {
            const auto type = stringArg(args, "type");
            const juce::StringArray types { "muffled", "radio", "noise", "distortion", "bitcrush" };
            const auto typeIndex = types.indexOf(type);
            require(typeIndex >= 0, u8"対応するエフェクト名を指定してください。");
            const auto selectedRange = range(args, track);
            require(track.specialFxRegions.size() < 128, u8"エフェクトが上限に達しました。");
            SpecialFxRegion effect;
            effect.id = juce::Uuid().toString();
            effect.type = static_cast<SpecialFxType>(typeIndex + 1);
            effect.startSeconds = selectedRange.first;
            effect.endSeconds = selectedRange.second;
            effect.amount = static_cast<float>(number(args, "amount", 0.5, 0.0, 1.0));
            effect.wet = static_cast<float>(number(args, "wet", 1.0, 0.0, 1.0));
            effect.fadeSeconds = number(args, "fade_seconds", std::min(0.03, (effect.endSeconds - effect.startSeconds) / 2.0), 0.0, std::min(10.0, (effect.endSeconds - effect.startSeconds) / 2.0));
            track.specialFxRegions.push_back(effect);
            apply(std::move(tracks), tr(u8"選択範囲にエフェクトを追加しました。"));
            return success();
        }
        if (action == "analyse_audio")
        {
            const auto selection = selectionScope(args);
            const auto trackId = track.id;
            std::pair<double, double> selectedRange { 0.0, track.getDuration() };
            if (selection)
            {
                selectedRange = range(args, track);
                for (auto& snapshotTrack : tracks)
                {
                    std::vector<AudioClip> cropped;
                    const auto start = selectedRange.first * snapshotTrack.playbackSpeed;
                    const auto end = selectedRange.second * snapshotTrack.playbackSpeed;
                    for (auto clip : snapshotTrack.clips)
                    {
                        const auto clippedStart = std::max(start, clip.startTime);
                        const auto clippedEnd = std::min(end, clip.startTime + clip.duration);
                        if (clippedEnd <= clippedStart) continue;
                        clip.offset += clippedStart - clip.startTime;
                        clip.startTime = clippedStart;
                        clip.duration = clippedEnd - clippedStart;
                        cropped.push_back(std::move(clip));
                    }
                    snapshotTrack.clips = std::move(cropped);
                }
            }
            auto job = newJob(action);
            workers.addJob([job, tracks, trackId, selection, selectedRange]
            {
                const auto analysed = AutoMixAnalyzer::analyseAndSuggest(tracks);
                const auto found = std::find_if(analysed.tracks.begin(), analysed.tracks.end(),
                    [&trackId](const AutoMixTrackSuggestion& suggestion) { return suggestion.trackId == trackId; });
                juce::var result = object({ { "error", tr(u8"解析できる音声がありません。") } });
                if (found != analysed.tracks.end() && found->analysisValid)
                {
                    result = analysisJson(found->analysis);
                    juce::Array<juce::var> issues, recommendations;
                    const auto add = [&issues, &recommendations](const char* issue, const char* recommendation)
                    {
                        issues.add(tr(issue));
                        recommendations.add(tr(recommendation));
                    };
                    if (found->analysis.peakDb > -0.5f)
                        add(u8"元音声のピークが上限に近いです。", u8"音量を少し下げ、書き出した音でも歪みを確認してください。");
                    if (found->analysis.sibilance > 0.35f)
                        add(u8"歯擦音が目立つ可能性があります。", u8"歯擦音の軽減を少し加えて比較してください。");
                    if (found->analysis.noiseFloorDb > -42.0f)
                        add(u8"静かな部分にノイズが残る可能性があります。", u8"ノイズ軽減を弱くかけて試聴してください。");
                    if (found->maskingScore > 0.25f)
                        add(u8"ほかのトラックと音域が重なる可能性があります。", u8"伴奏を少し下げ、声の明るさを調整して比較してください。");
                    auto* obj = result.getDynamicObject();
                    obj->setProperty("masking_score", found->maskingScore);
                    obj->setProperty("issues", issues);
                    obj->setProperty("recommendations", recommendations);
                    obj->setProperty("scope", selection ? "selection" : "track");
                    obj->setProperty("start_seconds", selectedRange.first);
                    obj->setProperty("end_seconds", selectedRange.second);
                    obj->setProperty("message", issues.isEmpty()
                        ? tr(u8"元音声の解析が完了しました。大きな問題の兆候は検出されませんでした。効果を含む仕上がりは比較試聴で確認してください。")
                        : tr(u8"元音声の解析が完了しました: ") + issues[0].toString());
                }
                const std::lock_guard<std::mutex> lock(job->mutex);
                job->result = result;
                job->ready = true;
            });
            return jobJson(job);
        }
        if (action == "create_variants")
        {
            const auto selection = selectionScope(args);
            variants.clear();
            variantApplied = false;
            juce::Array<juce::var> descriptions;
            const juce::StringArray ids { "natural", "clear", "wide" };
            for (int variant = 0; variant < 3; ++variant)
            {
                auto snapshot = tracks;
                const auto settings = SimpleMixProcessor::makePresetSettings(static_cast<MixPreset>(variant + 1));
                auto parameters = object({ { "brightness", settings.brightness }, { "ambience", settings.ambience } });
                if (!selection) parameters.getDynamicObject()->setProperty("stability", settings.stability);
                adjustTrack(snapshot[static_cast<size_t>(index)], parameters, selection);
                variants.emplace(ids[variant], std::move(snapshot));
                descriptions.add(object({ { "variant_id", ids[variant] }, { "settings", parameters } }));
            }
            variantRevision = engine.getContentRevision();
            summary = tr(u8"自然・くっきり・広がりの3案を用意しました。選択すると適用します。");
            return object({ { "variants", descriptions }, { "message", summary } });
        }
        if (action == "choose_variant")
        {
            require(variantRevision == engine.getContentRevision(), u8"3案の作成後に編集されました。現在の音から3案を作り直してください。");
            const auto variant = stringArg(args, "variant_id");
            const auto found = variants.find(variant);
            require(found != variants.end(), u8"3案を作成してからnatural/clear/wideを選んでください。");
            const auto snapshot = found->second;
            const auto label = tr(u8"音の案を適用しました: ") + variant;
            if (variantApplied)
            {
                requireHistory();
                require(historyPosition > 0, u8"3案の元の変更が見つかりません。");
                engine.undo();
                engine.beginEdit();
                engine.restoreTracksState(snapshot);
                engine.endEdit();
                auto& edit = history[historyPosition - 1];
                edit.after = edit.originalAfter = snapshot;
                edit.label = label;
                history.resize(historyPosition);
                expectedRevision = engine.getContentRevision();
                comparison.phase = 0;
                summary = label;
            }
            else
            {
                auto retained = variants;
                apply(snapshot, label);
                variants = std::move(retained);
                variantApplied = true;
            }
            variantRevision = engine.getContentRevision();
            return success();
        }
        if (action == "match_reference")
        {
            const auto path = stringArg(args, "file_path");
            require(juce::File::isAbsolutePath(path), u8"参考曲は絶対パスで指定してください。");
            const juce::File file(path);
            require(file.existsAsFile() && file.getSize() <= 512 * 1024 * 1024, u8"参考曲が見つからないか、大きすぎます。");
            auto job = newJob(action);
            job->trackId = track.id;
            job->strength = number(args, "strength", 1.0, 0.0, 1.0);
            const std::vector<TrackData> targetTracks { track };
            workers.addJob([job, file, targetTracks]
            {
                juce::var result;
                ReferenceMixResult suggestion;
                try
                {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
                    require(reader != nullptr && reader->sampleRate > 0.0 && reader->sampleRate <= 192000.0,
                            u8"参考曲の形式を読み込めません。");
                    const auto samples = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples,
                        static_cast<juce::int64>(reader->sampleRate * 60.0)));
                    require(samples > 0, u8"参考曲に音声がありません。");
                    juce::AudioBuffer<float> buffer(2, samples);
                    require(reader->read(&buffer, 0, samples, 0, true, true), u8"参考曲の読込に失敗しました。");
                    suggestion = ReferenceMixAnalyzer::analyseAndSuggest(buffer, reader->sampleRate, targetTracks);
                    require(suggestion.success, u8"参考曲と対象の音を比較できませんでした。");
                    result = object({ { "ok", true } });
                }
                catch (const std::exception& error) { result = object({ { "error", tr(error.what()) } }); }
                const std::lock_guard<std::mutex> lock(job->mutex);
                job->reference = std::move(suggestion);
                job->result = result;
                job->ready = true;
            });
            return jobJson(job);
        }
        if (action == "save_preference")
        {
            const auto name = stringArg(args, "name").trim();
            require(name.isNotEmpty() && name.length() <= 80, u8"好みの名前は1～80文字で指定してください。");
            require(preferences.getDynamicObject()->getProperties().size() < 100 || preferences.hasProperty(name),
                    u8"保存できる好みは100件までです。");
            auto saved = mixJson(track);
            if (!track.simpleMix.enabled)
            {
                saved.getDynamicObject()->setProperty("brightness", 0.0);
                saved.getDynamicObject()->setProperty("ambience", 0.0);
                saved.getDynamicObject()->setProperty("stability", 0.0);
            }
            if (!track.noiseReduction.enabled)
            {
                saved.getDynamicObject()->setProperty("noise_reduction", 0.0);
                saved.getDynamicObject()->setProperty("de_ess", 0.0);
            }
            // Preference gain is neutral by default: retain tone, not recording-specific level.
            saved.getDynamicObject()->removeProperty("volume");
            saved.getDynamicObject()->removeProperty("gain_db");
            saved.getDynamicObject()->removeProperty("pan");
            saved.getDynamicObject()->removeProperty("enabled");
            auto next = preferences.clone();
            next.getDynamicObject()->setProperty(name, saved);
            require(preferencesFile.getParentDirectory().createDirectory().wasOk(), u8"好みの保存先を作れません。");
            juce::TemporaryFile temporary(preferencesFile);
            require(temporary.getFile().replaceWithText(juce::JSON::toString(next))
                    && temporary.overwriteTargetFileWithTemporary(), u8"好みを保存できませんでした。");
            preferences = next;
            summary = tr(u8"音の好みを保存しました: ") + name;
            return success();
        }
        if (action == "apply_preference")
        {
            const auto name = stringArg(args, "name").trim();
            const juce::Identifier preferenceKey(name);
            require(preferences.hasProperty(preferenceKey) && preferences[preferenceKey].isObject(), u8"指定した好みが保存されていません。");
            auto parameters = preferences[preferenceKey].clone();
            const auto selection = selectionScope(args);
            if (selection)
            {
                parameters.getDynamicObject()->removeProperty("stability");
                parameters.getDynamicObject()->removeProperty("noise_reduction");
                parameters.getDynamicObject()->removeProperty("de_ess");
            }
            adjustTrack(track, parameters, selection);
            apply(std::move(tracks), tr(u8"音の好みを適用しました: ") + name);
            return success();
        }
        require(false, u8"未対応の操作です。");
        return {};
    }
};

AssistantController::AssistantController(AudioEngine& engine, std::function<int()> selectedTrack,
                                       std::function<void(int)> selectTrack, juce::File preferencesFile)
    : impl(std::make_unique<Impl>(engine, std::move(selectedTrack), std::move(selectTrack), std::move(preferencesFile))) {}
AssistantController::~AssistantController() = default;
juce::var AssistantController::handleRequest(const juce::var& request)
{
    try { return impl->handle(request); }
    catch (const std::exception& error)
    {
        impl->summary = tr(error.what());
        return object({ { "error", impl->summary } });
    }
    catch (...) { return object({ { "error", tr(u8"操作を実行できませんでした。") } }); }
}
juce::String AssistantController::getSummaryText() const { return impl->summary; }
juce::var AssistantController::getComparisonState() const { return impl->comparisonState(); }
void AssistantController::preparePreview(int maximumBlockSize, double sampleRate)
{
    const juce::ScopedLock lock(impl->previewLock);
    impl->preview.prepareToPlay(maximumBlockSize, sampleRate);
}
void AssistantController::releasePreview()
{
    impl->stopPreview();
    const juce::ScopedLock lock(impl->previewLock);
    impl->preview.releaseResources();
}
bool AssistantController::renderPreview(const juce::AudioSourceChannelInfo& block)
{
    if (!impl->previewActive.load()) return false;
    if (impl->engine.getPlaybackState() != PlaybackState::Stopped)
    {
        impl->previewActive.store(false);
        return false;
    }
    const juce::ScopedTryLock lock(impl->previewLock);
    if (!lock.isLocked()) { block.clearActiveBufferRegion(); return true; }
    impl->preview.getNextAudioBlock(block);
    return true;
}
bool AssistantController::isPreviewBusy() const { return impl->busy(); }
bool AssistantController::isComparing() const { return impl->busy() || impl->previewActive.load(); }
void AssistantController::setExternalBusyCheck(std::function<bool()> check)
{
    impl->externalBusy = std::move(check);
}
