#include "WindowsMp3Encoder.h"

#include <algorithm>

namespace
{
WindowsMp3Encoder::Result cancelledResult()
{
    WindowsMp3Encoder::Result result;
    result.cancelled = true;
    return result;
}

juce::File findBundledFfmpeg()
{
#if JUCE_WINDOWS
    const auto executable = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
    const auto bundled = executable.getSiblingFile("ffmpeg.exe");
    if (bundled.existsAsFile())
        return bundled;
#endif
    return {};
}
}

WindowsMp3Encoder::Result WindowsMp3Encoder::encode(
    const juce::AudioBuffer<float>& audio,
    int sampleRate,
    int bitRateKbps,
    const juce::File& targetFile,
    const std::function<bool()>& shouldCancel)
{
    const auto isCancelled = [&shouldCancel]
    {
        return shouldCancel && shouldCancel();
    };

    if (isCancelled())
        return cancelledResult();
    if (targetFile == juce::File() || targetFile.isDirectory())
        return { false, false, "MP3の保存先が正しくありません。" };
    if (audio.getNumSamples() <= 0)
        return { false, false, "MP3へ書き出す音声がありません。" };
    if (audio.getNumChannels() != 1 && audio.getNumChannels() != 2)
        return { false, false, "MP3はモノラルまたはステレオで書き出してください。" };
    if (sampleRate != 44100 && sampleRate != 48000)
        return { false, false, "MP3のサンプルレートは44.1 kHzまたは48 kHzにしてください。" };
    if (bitRateKbps != 192)
        return { false, false, "MP3のビットレートは192 kbpsにしてください。" };

    const auto ffmpeg = findBundledFfmpeg();
    if (!ffmpeg.existsAsFile())
        return { false, false,
                 "MP3変換用のffmpeg.exeがアプリと同じフォルダーにありません。" };

    juce::TemporaryFile wavTemporary(".wav");
    {
        auto stream = wavTemporary.getFile().createOutputStream();
        if (stream == nullptr || !stream->openedOk())
            return { false, false, "MP3変換用の一時音声を作成できませんでした。" };
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(stream.release(), sampleRate,
                                audio.getNumChannels(), 24, {}, 0));
        if (writer == nullptr)
            return { false, false, "MP3変換用の一時音声を準備できませんでした。" };

        constexpr int chunkSamples = 65536;
        for (int position = 0; position < audio.getNumSamples();
             position += chunkSamples)
        {
            if (isCancelled())
                return cancelledResult();
            const int count = std::min(chunkSamples,
                                       audio.getNumSamples() - position);
            if (!writer->writeFromAudioSampleBuffer(audio, position, count))
                return { false, false, "MP3変換用の一時音声を書き込めませんでした。" };
        }
    }

    juce::TemporaryFile mp3Temporary(targetFile);
    mp3Temporary.getFile().deleteFile();
    juce::StringArray arguments {
        ffmpeg.getFullPathName(),
        "-y", "-hide_banner", "-loglevel", "error",
        "-i", wavTemporary.getFile().getFullPathName(),
        "-vn", "-codec:a", "libmp3lame",
        "-b:a", juce::String(bitRateKbps) + "k",
        "-ar", juce::String(sampleRate),
        "-ac", juce::String(audio.getNumChannels()),
        "-f", "mp3", mp3Temporary.getFile().getFullPathName()
    };

    juce::ChildProcess process;
    if (!process.start(arguments,
                       juce::ChildProcess::wantStdOut
                           | juce::ChildProcess::wantStdErr))
        return { false, false, "MP3変換プログラムを開始できませんでした。" };

    while (process.isRunning())
    {
        if (isCancelled())
        {
            process.kill();
            return cancelledResult();
        }
        juce::Thread::sleep(20);
    }

    const auto converterOutput = process.readAllProcessOutput().trim();
    if (process.getExitCode() != 0
        || !mp3Temporary.getFile().existsAsFile()
        || mp3Temporary.getFile().getSize() <= 0)
    {
        return { false, false,
                 "MP3変換に失敗しました。 "
                     + converterOutput.substring(0, 240) };
    }
    if (isCancelled())
        return cancelledResult();
    if (!mp3Temporary.overwriteTargetFileWithTemporary())
        return { false, false, "完成したMP3を保存先へ置き換えられませんでした。" };
    return { true, false, {} };
}
