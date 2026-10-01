#include "../Source/AssistantController.h"
#include <cmath>
#include <iostream>

namespace
{
bool check(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAILED AssistantController: " << message << std::endl;
    return condition;
}
juce::var call(AssistantController& controller, const char* action, const juce::String& arguments = "{}")
{
    return controller.handleRequest(juce::JSON::parse("{\"action\":\"" + juce::String(action) + "\",\"args\":" + arguments + "}"));
}
bool good(const juce::var& result) { return !result.hasProperty("error"); }
bool close(float left, float right) { return std::abs(left - right) < 1.0e-5f; }
TrackData fixture(const char* id, const char* clipId)
{
    TrackData track;
    track.id = id;
    track.name = id;
    track.volume = 0.5f;
    if (juce::String(id) == "music")
    {
        track.simpleMix.brightness = 0.7f;
        track.simpleMix.ambience = 0.4f;
        track.noiseReduction.amount = 0.6f;
    }
    AudioClip clip;
    clip.id = clipId;
    clip.name = clipId;
    clip.sampleRate = 44100;
    clip.duration = 2.0;
    clip.buffer = std::make_shared<juce::AudioBuffer<float>>(1, 88200);
    for (int sample = 0; sample < clip.buffer->getNumSamples(); ++sample)
        clip.buffer->setSample(0, sample, 0.15f * std::sin(static_cast<float>(sample) * 0.063f));
    track.clips.push_back(std::move(clip));
    return track;
}
}

bool runAssistantControllerTests()
{
    bool ok = true;
    AudioEngine engine;
    engine.prepare(512, 44100.0);
    engine.replaceTracks({ fixture("vocal", "voice-clip"), fixture("music", "music-clip") });
    int selected = -1;
    const auto preferencesFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SimpleRecPro-AssistantTests-" + juce::Uuid().toString()).getChildFile("preferences.json");
    {
        AssistantController controller(engine, [&selected] { return selected; },
            [&selected](int value) { selected = value; }, preferencesFile);
        const auto initialComparison = controller.getComparisonState();
        ok = check(!static_cast<bool>(initialComparison["available"])
            && !static_cast<bool>(initialComparison["busy"])
            && !static_cast<bool>(initialComparison["playing"])
            && initialComparison["mode"].toString() == "stop"
            && initialComparison["message"].toString().isNotEmpty(), "inline comparison initially unavailable with explanatory message") && ok;
        engine.setCurrentTime(0.7);
        ok = check(good(call(controller, "compare", "{\"mode\":\"stop\"}"))
            && std::abs(engine.getCurrentTime() - 0.7) < 1.0e-6, "stopping inactive comparison leaves normal cursor unchanged") && ok;
        ok = check(!good(call(controller, "adjust_sound", "{\"gain_db\":-3}")), "unselected edit rejected") && ok;
        ok = check(!good(call(controller, "select_target", "{\"track_id\":\"missing\"}")), "missing ID rejected") && ok;
        ok = check(!good(call(controller, "select_target", "{\"track_id\":\"music\",\"clip_id\":\"voice-clip\"}")), "mismatched IDs rejected") && ok;
        ok = check(good(call(controller, "select_target", "{\"clip_id\":\"voice-clip\"}")) && selected == 0, "clip resolves stable track ID") && ok;
        const auto before = engine.getTracksSnapshot();
        ok = check(good(call(controller, "adjust_sound", "{\"gain_db\":-6,\"brightness\":0.4}")), "track adjustment") && ok;
        ok = check(static_cast<bool>(controller.getComparisonState()["available"]), "audio edit enables inline comparison") && ok;
        ok = check(juce::JSON::toString(call(controller, "get_context")["comparison"])
            == juce::JSON::toString(controller.getComparisonState()), "context and lightweight comparison API agree") && ok;
        auto tracks = engine.getTracksSnapshot();
        ok = check(close(tracks[0].volume, before[0].volume * juce::Decibels::decibelsToGain(-6.0f))
            && close(tracks[0].simpleMix.brightness, 0.4f) && close(tracks[1].volume, before[1].volume), "only intended track changed") && ok;
        ok = check(good(call(controller, "scale_last_edit", "{\"factor\":0.5}")), "weaken last request") && ok;
        tracks = engine.getTracksSnapshot();
        ok = check(close(tracks[0].volume, before[0].volume * juce::Decibels::decibelsToGain(-3.0f))
            && close(tracks[0].simpleMix.brightness, 0.2f), "weaken interpolates dB and tone from original baseline") && ok;
        ok = check(tracks[1].simpleMix == before[1].simpleMix
            && tracks[1].noiseReduction == before[1].noiseReduction, "weaken leaves unchanged disabled settings untouched") && ok;
        ok = check(good(call(controller, "undo_edit")), "undo request") && ok;
        ok = check(close(engine.getTracksSnapshot()[0].volume, before[0].volume), "undo restored original volume") && ok;
        ok = check(good(call(controller, "redo_edit")), "redo request") && ok;
        ok = check(close(engine.getTracksSnapshot()[0].simpleMix.brightness, 0.2f), "redo restored weakened request") && ok;

        ok = check(good(call(controller, "select_target", "{\"track_id\":\"vocal\",\"start_seconds\":0.4,\"end_seconds\":1.2}")), "explicit range") && ok;
        const auto trackVolume = engine.getTracksSnapshot()[0].volume;
        ok = check(good(call(controller, "adjust_sound", "{\"scope\":\"selection\",\"gain_db\":-4,\"brightness\":0.3}")), "selection automation") && ok;
        tracks = engine.getTracksSnapshot();
        ok = check(tracks[0].automationRegions.size() == 1 && tracks[1].automationRegions.empty()
            && close(tracks[0].volume, trackVolume), "range adjustment leaves whole-track volume and other track untouched") && ok;
        if (!tracks[0].automationRegions.empty())
            ok = check(std::abs(tracks[0].automationRegions[0].startSeconds - 0.4) < 1.0e-9
                && std::abs(tracks[0].automationRegions[0].endSeconds - 1.2) < 1.0e-9, "exact region boundaries") && ok;
        ok = check(good(call(controller, "adjust_sound", "{\"scope\":\"selection\",\"gain_db\":1,\"ambience\":0.4}")), "update existing range") && ok;
        tracks = engine.getTracksSnapshot();
        ok = check(tracks[0].automationRegions.size() == 1
            && close(tracks[0].automationRegions[0].gainDb, -3.0f)
            && close(tracks[0].automationRegions[0].brightness, 0.3f)
            && close(tracks[0].automationRegions[0].ambience, 0.4f), "regional update keeps unspecified tone and adds relative gain") && ok;
        ok = check(good(call(controller, "adjust_sound", "{\"scope\":\"selection\",\"ambience\":0.1}")), "reduce existing regional ambience") && ok;
        ok = check(engine.getTracksSnapshot()[0].automationRegions.size() == 1
            && close(engine.getTracksSnapshot()[0].automationRegions[0].ambience, 0.1f), "less ambience does not add a second effect layer") && ok;
        ok = check(good(call(controller, "scale_last_edit", "{\"factor\":0.5}"))
            && close(engine.getTracksSnapshot()[0].automationRegions[0].ambience, 0.25f), "scale existing region from previous value") && ok;
        const auto revision = engine.getContentRevision();
        ok = check(!good(call(controller, "adjust_sound", "{\"scope\":\"selection\",\"stability\":0.5}")), "unsupported regional processing explicitly rejected") && ok;
        ok = check(!good(call(controller, "adjust_sound", "{\"gain_db\":\"loud\"}")), "wrong numeric type rejected") && ok;
        ok = check(!good(call(controller, "adjust_sound", "{\"brightness\":2}")), "out of bounds rejected") && ok;
        ok = check(!good(call(controller, "adjust_sound", "{\"gain_db\":-1,\"scope\":\"unknown\"}")), "invalid scope rejected") && ok;
        ok = check(!good(call(controller, "select_target", "{\"start_seconds\":1.5,\"end_seconds\":8}")), "out of audio range rejected") && ok;
        ok = check(!good(call(controller, "add_effect", "{\"type\":\"unknown\"}")), "unknown effect rejected") && ok;
        ok = check(!good(call(controller, "adjust_sound", "{\"scope\":\"selection\",\"gain_db\":-1,\"start_seconds\":0.7,\"end_seconds\":1.5}")), "ambiguous overlapping region rejected") && ok;
        ok = check(engine.getContentRevision() == revision, "invalid requests do not modify project") && ok;

        const auto analysis = call(controller, "analyse_audio", "{\"scope\":\"selection\"}");
        ok = check(analysis["job_id"].toString().isNotEmpty(), "analysis starts an asynchronous job") && ok;
        juce::var analysisStatus;
        for (int attempt = 0; attempt < 300; ++attempt)
        {
            analysisStatus = call(controller, "get_job", "{\"job_id\":\"" + analysis["job_id"].toString() + "\"}");
            if (analysisStatus["status"].toString() != "running") break;
            juce::Thread::sleep(10);
        }
        ok = check(analysisStatus["status"].toString() == "completed", "analysis finishes") && ok;
        const auto analysisResult = analysisStatus["result"];
        ok = check(analysisResult["scope"].toString() == "selection"
            && static_cast<double>(analysisResult["analysed_seconds"]) <= 0.801
            && analysisResult["measurement"].toString() == "source_audio_before_effects",
            "analysis measures selected source segment with explicit measurement label") && ok;
        ok = check(analysisResult.hasProperty("masking_score") && analysisResult.hasProperty("issues")
            && analysisResult["message"].toString().isNotEmpty(), "analysis provides actionable report") && ok;

        ok = check(good(call(controller, "add_effect", "{\"type\":\"radio\",\"amount\":0.65,\"wet\":0.7}")), "regional effect added") && ok;
        ok = check(engine.getTracksSnapshot()[0].specialFxRegions.size() == 1, "effect persisted in state") && ok;
        ok = check(good(call(controller, "scale_last_edit", "{\"factor\":0}"))
            && engine.getTracksSnapshot()[0].specialFxRegions.empty(), "zero strength returns exact before state") && ok;

        const auto variants = call(controller, "create_variants");
        ok = check(good(variants) && variants["variants"].size() == 3, "three variants available") && ok;
        ok = check(good(call(controller, "choose_variant", "{\"variant_id\":\"clear\"}")), "choose clear variant") && ok;
        ok = check(good(call(controller, "choose_variant", "{\"variant_id\":\"wide\"}")), "switch from clear to wide without rebuilding") && ok;
        ok = check(good(call(controller, "choose_variant", "{\"variant_id\":\"clear\"}")), "switch back to clear") && ok;
        ok = check(engine.getTracksSnapshot()[0].simpleMix.enabled, "chosen variant audibly enabled") && ok;
        ok = check(good(call(controller, "save_preference", "{\"name\":\"My voice\"}")), "save preference") && ok;
        ok = check(preferencesFile.existsAsFile(), "preference persisted") && ok;
        ok = check(good(call(controller, "adjust_sound", "{\"brightness\":-0.8}")), "change tone after save") && ok;
        ok = check(good(call(controller, "apply_preference", "{\"name\":\"My voice\"}")), "apply saved preference") && ok;
        const auto savedTone = engine.getTracksSnapshot()[0].simpleMix.brightness;
        ok = check(savedTone > 0.0f, "saved tone restored") && ok;

        engine.setTrackPan(0, -0.3f);
        ok = check(!static_cast<bool>(controller.getComparisonState()["available"]), "manual edit disables stale inline comparison immediately") && ok;
        const auto manualState = engine.getTracksSnapshot();
        ok = check(!good(call(controller, "undo_edit")), "manual edit invalidates stale assistant undo") && ok;
        ok = check(close(engine.getTracksSnapshot()[0].pan, manualState[0].pan), "manual edit retained") && ok;
        ok = check(good(call(controller, "adjust_sound", "{\"gain_db\":-1}")), "new edit starts at latest manual state") && ok;
        ok = check(good(call(controller, "undo_edit")) && close(engine.getTracksSnapshot()[0].pan, -0.3f), "new undo preserves prior manual edit") && ok;
        ok = check(good(call(controller, "create_variants")), "recreate variants") && ok;
        engine.setTrackVolume(0, 0.4f);
        ok = check(!good(call(controller, "choose_variant", "{\"variant_id\":\"natural\"}")), "stale variants rejected") && ok;
        engine.replaceTracks({ fixture("new-project", "new-clip") });
        ok = check(!good(call(controller, "undo_edit")), "project replacement invalidates history") && ok;
        ok = check(engine.getTracksSnapshot()[0].id == "new-project", "old project not restored") && ok;
        ok = check(!good(call(controller, "adjust_sound", "{\"gain_db\":12}")), "gain above serialisable track ceiling rejected") && ok;
        bool ioBusy = true;
        controller.setExternalBusyCheck([&ioBusy] { return ioBusy; });
        ok = check(!static_cast<bool>(controller.getComparisonState()["available"]), "external I/O disables inline comparison") && ok;
        ok = check(!good(call(controller, "adjust_sound", "{\"gain_db\":-1}")), "external save/load busy rejects mutation") && ok;
        ioBusy = false;
        ok = check(good(call(controller, "adjust_sound", "{\"gain_db\":-1}")), "mutation resumes after I/O") && ok;
        ok = check(static_cast<bool>(controller.getComparisonState()["available"]), "inline comparison resumes with valid current edit") && ok;
        const auto changedVolume = engine.getTracksSnapshot()[0].volume;
        ok = check(good(call(controller, "undo_edit")), "assistant undo before native redo") && ok;
        engine.redo();
        ok = check(close(engine.getTracksSnapshot()[0].volume, changedVolume), "native redo agrees with assistant undo") && ok;
        ok = check(!good(call(controller, "redo_edit")), "native redo invalidates stale assistant pointer") && ok;
        ok = check(good(call(controller, "adjust_sound", "{\"brightness\":0.6}")), "new edit after native redo") && ok;
        ok = check(good(call(controller, "scale_last_edit", "{\"factor\":0.5}")), "scale replaces native transaction") && ok;
        const auto scaledTone = engine.getTracksSnapshot()[0].simpleMix.brightness;
        engine.undo();
        ok = check(!close(engine.getTracksSnapshot()[0].simpleMix.brightness, scaledTone), "native undo returns before scaled transaction") && ok;
        engine.redo();
        ok = check(close(engine.getTracksSnapshot()[0].simpleMix.brightness, scaledTone), "native redo returns scaled result") && ok;
    }
    {
        selected = 0;
        AssistantController restored(engine, [&selected] { return selected; },
            [&selected](int value) { selected = value; }, preferencesFile);
        const auto listed = call(restored, "list_preferences");
        ok = check(listed["preferences"].size() == 1 && listed["preferences"][0]["name"].toString() == "My voice", "preferences survive controller restart") && ok;
        ok = check(good(call(restored, "apply_preference", "{\"name\":\"My voice\"}")), "persisted preference reusable on new project") && ok;
        ok = check(!good(restored.handleRequest("invalid")), "nonobject rejected") && ok;
        engine.clearLoopRange();
        ok = check(good(call(restored, "adjust_sound", "{\"gain_db\":-1}")), "fresh edit for comparison state test") && ok;
        engine.setCurrentTime(0.35);
        const auto preparingComparison = call(restored, "compare", "{\"mode\":\"before\"}");
        const auto preparingState = restored.getComparisonState();
        ok = check(preparingComparison["job_id"].toString().isNotEmpty()
            && static_cast<bool>(preparingState["busy"])
            && !static_cast<bool>(preparingState["playing"])
            && preparingState["mode"].toString() == "before", "inline state exposes cancellable preparation and requested mode") && ok;
        ok = check(good(call(restored, "compare", "{\"mode\":\"stop\"}"))
            && !static_cast<bool>(restored.getComparisonState()["busy"])
            && restored.getComparisonState()["mode"].toString() == "stop"
            && std::abs(engine.getCurrentTime() - 0.35) < 1.0e-6, "stop cancels preparation and restores its cursor") && ok;
        for (int attempt = 0; attempt < 300 && engine.getExportStatus().active; ++attempt)
            juce::Thread::sleep(10);
        ok = check(!engine.getExportStatus().active, "cancelled comparison export finishes") && ok;
        engine.record(0);
        const auto recordingState = engine.getPlaybackState();
        ok = check(!static_cast<bool>(restored.getComparisonState()["available"])
            && good(call(restored, "compare", "{\"mode\":\"stop\"}"))
            && engine.getPlaybackState() == recordingState, "comparison stop remains safe during recording") && ok;
        engine.stop();
        TrackData empty;
        empty.id = "empty";
        empty.volume = 0.5f;
        engine.replaceTracks({ empty });
        engine.clearLoopRange();
        ok = check(good(call(restored, "adjust_sound", "{\"gain_db\":-1}")), "metadata edit on empty track") && ok;
        ok = check(!static_cast<bool>(restored.getComparisonState()["available"]), "empty audio does not enable inline comparison") && ok;
        const auto failedComparison = call(restored, "compare", "{\"mode\":\"after\"}");
        ok = check(failedComparison["status"].toString() == "failed" && !restored.isPreviewBusy(),
            "comparison export failure settles the job and unlocks editing") && ok;
    }
    engine.release();
    return ok;
}
