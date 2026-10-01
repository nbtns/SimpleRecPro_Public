#include "MainComponent.h"
#include "LatencyCalibration.h"
#include "ReferenceMixAnalyzer.h"
#include "TempoMap.h"
#include "UiTheme.h"
#include "UiLookAndFeel.h"
#include "WorkspaceLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

struct MainComponent::LatencyCalibrationSession
{
    enum class State
    {
        idle,
        capturing,
        cancelling,
        captureReady,
        analysing,
        resultReady,
        succeeded,
        failed
    };

    std::atomic<State> state { State::idle };
    std::atomic<int> capturePosition { 0 };
    std::atomic<int> activeAudioCallbacks { 0 };
    std::atomic<std::uint64_t> generation { 1 };

    double sampleRate = 0.0;
    int maximumLatencySamples = 0;
    juce::String deviceFingerprint;
    std::vector<float> probe;
    std::vector<float> playbackSignal;
    std::vector<std::vector<float>> capturedChannels;
    std::vector<int> emissionStartSamples;

    mutable juce::CriticalSection resultLock;
    juce::String statusText { juce::CharPointer_UTF8(u8"未測定（機器情報から推定して補正中）") };
    bool pendingSuccess = false;
    int pendingLatencySamples = 0;
    float pendingConfidence = 0.0f;
    juce::String pendingMessage;
    juce::String pendingFingerprint;
};

namespace
{
    static ModernLookAndFeel modernLookAndFeel;

    class SettingsLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        SettingsLookAndFeel()
        {
            setColour(juce::ComboBox::backgroundColourId, UiTheme::controlSurface);
            setColour(juce::ComboBox::outlineColourId, UiTheme::border);
            setColour(juce::ComboBox::textColourId, UiTheme::textPrimary);
            setColour(juce::ComboBox::arrowColourId, UiTheme::textSecondary);
            
            setColour(juce::PopupMenu::backgroundColourId, UiTheme::raisedSurface);
            setColour(juce::PopupMenu::textColourId, UiTheme::textPrimary);
            setColour(juce::PopupMenu::highlightedBackgroundColourId, UiTheme::accentSoft);
            setColour(juce::PopupMenu::highlightedTextColourId, UiTheme::textPrimary);
            
            setColour(juce::TextButton::buttonColourId, UiTheme::accent);
            setColour(juce::TextButton::textColourOnId, UiTheme::textPrimary);
            setColour(juce::TextButton::textColourOffId, UiTheme::textPrimary);
            
            setColour(juce::ResizableWindow::backgroundColourId, UiTheme::panelBackground);
            setColour(juce::Label::textColourId, UiTheme::textSecondary);
        }
        
        void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
        {
            auto bounds = button.getLocalBounds().toFloat();
            auto cornerSize = 8.0f;
            
            // "Cancel" button or others might want a different color, but we'll use baseColour for all
            juce::Colour baseColour = backgroundColour;
            if (button.getName() == "キャンセル" || button.getButtonText() == "Close")
                baseColour = UiTheme::controlSurface;

            if (shouldDrawButtonAsDown)             baseColour = baseColour.darker(0.2f);
            else if (shouldDrawButtonAsHighlighted) baseColour = baseColour.brighter(0.1f);
            
            g.setColour(baseColour);
            g.fillRoundedRectangle(bounds, cornerSize);
        }
        
        void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                           int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override
        {
            auto cornerSize = 8.0f;
            juce::Rectangle<int> boxBounds(0, 0, width, height);
            
            g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
            g.fillRoundedRectangle(boxBounds.toFloat(), cornerSize);
            
            g.setColour(box.findColour(juce::ComboBox::outlineColourId));
            g.drawRoundedRectangle(boxBounds.toFloat().reduced(0.5f), cornerSize, 1.0f);
            
            juce::Rectangle<int> arrowZone(width - 30, 0, 20, height);
            juce::Path path;
            path.addTriangle(arrowZone.getX() + 5.0f, arrowZone.getCentreY() - 2.0f,
                             arrowZone.getRight() - 5.0f, arrowZone.getCentreY() - 2.0f,
                             arrowZone.getCentreX(), arrowZone.getCentreY() + 3.0f);
            
            g.setColour(box.findColour(juce::ComboBox::arrowColourId).withAlpha(0.6f));
            g.fillPath(path);
        }
    };
    
    class SettingsModalComponent : public juce::Component,
                                   private juce::ChangeListener,
                                   private juce::Timer
    {
    public:
        SettingsModalComponent(juce::AudioDeviceManager& deviceManagerToUse,
                               AudioEngine& audioEngineToUse,
                               std::function<void(double)> manualOffsetChanged,
                               std::function<void()> startCalibration,
                               std::function<void()> cancelCalibration,
                               std::function<juce::String()> calibrationStatus,
                               std::function<bool()> calibrationRunning)
            : deviceManager(deviceManagerToUse),
              audioEngine(audioEngineToUse),
              onManualOffsetChanged(std::move(manualOffsetChanged)),
              onStartCalibration(std::move(startCalibration)),
              onCancelCalibration(std::move(cancelCalibration)),
              getCalibrationStatus(std::move(calibrationStatus)),
              getCalibrationRunning(std::move(calibrationRunning))
        {
            deviceManager.addChangeListener(this);
            
            auto addRow = [this](juce::Label& lbl, juce::ComboBox& cb, const char* text) {
                addAndMakeVisible(lbl);
                lbl.setText(juce::CharPointer_UTF8(text), juce::dontSendNotification);
                lbl.setJustificationType(juce::Justification::centredRight); // プルダウンにくっつけるため右揃え
                addAndMakeVisible(cb);
            };
            
            addRow(deviceTypeLabel, deviceTypeCombo, u8"デバイスの種類:");
            addRow(outputDeviceLabel, outputDeviceCombo, u8"出力デバイス:");
            addRow(inputDeviceLabel, inputDeviceCombo, u8"入力デバイス:");
            addRow(purposeLabel, purposeCombo, u8"録音用途（かんたん設定）:");
            addRow(sampleRateLabel, sampleRateCombo, u8"サンプルレート:");
            addRow(bufferSizeLabel, bufferSizeCombo, u8"バッファサイズ:");

            addAndMakeVisible(manualOffsetLabel);
            manualOffsetLabel.setText(juce::CharPointer_UTF8(u8"手動録音補正:"), juce::dontSendNotification);
            manualOffsetLabel.setJustificationType(juce::Justification::centredRight);
            manualOffsetLabel.setTooltip(juce::CharPointer_UTF8(u8"録音が後ろへずれる場合はプラス方向へ調整します"));
            addAndMakeVisible(manualOffsetSlider);
            manualOffsetSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            manualOffsetSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 76, 24);
            manualOffsetSlider.setRange(-200.0, 200.0, 0.1);
            manualOffsetSlider.setTextValueSuffix(" ms");
            manualOffsetSlider.setTooltip(juce::CharPointer_UTF8(u8"自動補正に追加する録音位置の微調整"));
            const double sampleRate = std::max(1.0, audioEngine.getSampleRate());
            manualOffsetSlider.setValue(audioEngine.getManualRecordingOffsetSamples() * 1000.0 / sampleRate,
                                        juce::dontSendNotification);
            manualOffsetSlider.onValueChange = [this]() {
                if (onManualOffsetChanged)
                    onManualOffsetChanged(manualOffsetSlider.getValue());
                updateLatencyText();
            };

            addAndMakeVisible(latencyInfoLabel);
            latencyInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa1a1aa));
            latencyInfoLabel.setJustificationType(juce::Justification::centred);
            latencyInfoLabel.setFont(12.0f);

            addAndMakeVisible(calibrationStatusLabel);
            calibrationStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xfffbbf24));
            calibrationStatusLabel.setJustificationType(juce::Justification::centred);
            calibrationStatusLabel.setFont(11.5f);

            addAndMakeVisible(calibrationButton);
            calibrationButton.setButtonText(juce::CharPointer_UTF8(u8"自動音ズレ測定を開始"));
            calibrationButton.setTooltip(juce::CharPointer_UTF8(
                u8"テスト音を実際に録音し、録音位置の補正値を自動測定します"));
            calibrationButton.onClick = [this]() {
                if (getCalibrationRunning && getCalibrationRunning())
                {
                    if (onCancelCalibration)
                        onCancelCalibration();
                    return;
                }

                const auto options = juce::MessageBoxOptions::makeOptionsOkCancel(
                    juce::MessageBoxIconType::WarningIcon,
                    juce::CharPointer_UTF8(u8"自動音ズレ測定の準備"),
                    juce::CharPointer_UTF8(
                        u8"約4秒のテスト音を出して、入力へ戻るまでの時間を測ります。\n\n"
                        u8"・普段ヘッドホンなら、片側を耳から外してマイクへ近づけてください\n"
                        u8"・スピーカーでも測れます（対応を確認できる場合のみライン接続も可）\n"
                        u8"・音量を小さめにしてください\n"
                        u8"・測定中だけAG03のダイレクトモニターをOFFにしてください\n\n"
                        u8"測定に成功した値だけ、自動補正へ保存します。"),
                    juce::CharPointer_UTF8(u8"測定を始める"),
                    juce::CharPointer_UTF8(u8"キャンセル"),
                    this);

                const auto safeSettings = juce::Component::SafePointer<SettingsModalComponent>(this);
                calibrationPrompt = juce::AlertWindow::showScopedAsync(
                    options,
                    [safeSettings](int result) {
                        if (safeSettings != nullptr && result == 1 && safeSettings->onStartCalibration)
                            safeSettings->onStartCalibration();
                    });
            };
            
            purposeCombo.addItem(juce::CharPointer_UTF8(u8"マイク（ボーカル・実況など） -> 入力1"), 1);
            purposeCombo.addItem(juce::CharPointer_UTF8(u8"ギター / ベース（ライン入力） -> 入力2"), 2);
            purposeCombo.addItem(juce::CharPointer_UTF8(u8"キーボード / ステレオ機器 -> 入力1+2"), 3);
            purposeCombo.addItem(juce::CharPointer_UTF8(u8"カスタム（変更なし）"), 4);
            
            addAndMakeVisible(asioPanelButton);
            asioPanelButton.setButtonText(juce::CharPointer_UTF8(u8"コントロールパネルを開く"));
            asioPanelButton.onClick = [this]() {
                if (auto* device = deviceManager.getCurrentAudioDevice())
                {
                    if (device->hasControlPanel())
                        device->showControlPanel();
                }
            };
            
            deviceTypeCombo.onChange = [this]() {
                int index = deviceTypeCombo.getSelectedId() - 1;
                if (index >= 0 && index < deviceManager.getAvailableDeviceTypes().size()) {
                    auto* type = deviceManager.getAvailableDeviceTypes()[index];
                    deviceManager.setCurrentAudioDeviceType(type->getTypeName(), true);
                }
            };
            
            outputDeviceCombo.onChange = [this]() {
                if (outputDeviceCombo.getSelectedId() > 0) {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.outputDeviceName = outputDeviceCombo.getText();
                    deviceManager.setAudioDeviceSetup(setup, true);
                }
            };
            
            inputDeviceCombo.onChange = [this]() {
                if (inputDeviceCombo.getSelectedId() > 0) {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.inputDeviceName = inputDeviceCombo.getText();
                    deviceManager.setAudioDeviceSetup(setup, true);
                }
            };
            
            purposeCombo.onChange = [this]() {
                int id = purposeCombo.getSelectedId();
                if (id == 4) return;
                
                juce::AudioDeviceManager::AudioDeviceSetup setup = deviceManager.getAudioDeviceSetup();
                setup.useDefaultInputChannels = false;
                
                if (id == 1) setup.inputChannels = juce::BigInteger(1);
                else if (id == 2) setup.inputChannels = juce::BigInteger(2);
                else if (id == 3) setup.inputChannels = juce::BigInteger(3);
                
                deviceManager.setAudioDeviceSetup(setup, true);
            };
            
            sampleRateCombo.onChange = [this]() {
                if (sampleRateCombo.getSelectedId() > 0) {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.sampleRate = sampleRateCombo.getSelectedId();
                    deviceManager.setAudioDeviceSetup(setup, true);
                }
            };
            
            bufferSizeCombo.onChange = [this]() {
                if (bufferSizeCombo.getSelectedId() > 0) {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.bufferSize = bufferSizeCombo.getSelectedId();
                    deviceManager.setAudioDeviceSetup(setup, true);
                }
            };
            
            populateDeviceTypes();
            updateAllControls();
            updateCalibrationControls();
            startTimerHz(10);
        }
        
        ~SettingsModalComponent() override
        {
            stopTimer();
            if (getCalibrationRunning && getCalibrationRunning() && onCancelCalibration)
                onCancelCalibration();
            deviceManager.removeChangeListener(this);
        }
        
        void resized() override
        {
            auto bounds = getLocalBounds();
            bounds.removeFromTop(40); // 上部余白40px
            
            auto rowHeight = 32;
            auto spacing = 16;
            
            auto layoutRow = [&](juce::Label& lbl, juce::ComboBox& cb, bool visible) {
                if (!visible) {
                    lbl.setVisible(false);
                    cb.setVisible(false);
                    return;
                }
                lbl.setVisible(true);
                cb.setVisible(true);
                
                auto row = bounds.removeFromTop(rowHeight);
                row.removeFromLeft(10); // 左側の余白
                lbl.setBounds(row.removeFromLeft(160)); // ラベル幅を調整
                row.removeFromLeft(10); // 余白
                cb.setBounds(row.removeFromLeft(280)); // プルダウン幅を広げる (全体で460px。右余白90px。視覚的左余白90pxで完璧な中央)
                
                bounds.removeFromTop(spacing);
            };
            
            layoutRow(deviceTypeLabel, deviceTypeCombo, true);
            layoutRow(outputDeviceLabel, outputDeviceCombo, true);
            
            juce::AudioIODeviceType* type = nullptr;
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (t->getTypeName() == deviceManager.getCurrentAudioDeviceType())
                    type = t;
            
            bool hasSeparate = type != nullptr && type->hasSeparateInputsAndOutputs();
            layoutRow(inputDeviceLabel, inputDeviceCombo, hasSeparate);
            
            layoutRow(purposeLabel, purposeCombo, true);
            layoutRow(sampleRateLabel, sampleRateCombo, true);
            layoutRow(bufferSizeLabel, bufferSizeCombo, true);

            {
                auto row = bounds.removeFromTop(rowHeight);
                row.removeFromLeft(10);
                manualOffsetLabel.setBounds(row.removeFromLeft(160));
                row.removeFromLeft(10);
                manualOffsetSlider.setBounds(row.removeFromLeft(280));
                bounds.removeFromTop(6);
            }

            latencyInfoLabel.setBounds(bounds.removeFromTop(30).reduced(20, 0));
            calibrationStatusLabel.setBounds(bounds.removeFromTop(42).reduced(18, 0));
            bounds.removeFromTop(4);
            calibrationButton.setBounds(bounds.removeFromTop(34).withSizeKeepingCentre(240, 34));
            
            if (asioPanelButton.isVisible())
            {
                bounds.removeFromTop(10);
                auto row = bounds.removeFromTop(rowHeight);
                asioPanelButton.setBounds(row.withSizeKeepingCentre(220, rowHeight));
            }
        }
        
        void changeListenerCallback(juce::ChangeBroadcaster*) override
        {
            updateAllControls();
        }

        void timerCallback() override
        {
            updateLatencyText();
            updateCalibrationControls();
        }
        
    private:
        void populateDeviceTypes()
        {
            deviceTypeCombo.clear(juce::dontSendNotification);
            const auto& types = deviceManager.getAvailableDeviceTypes();
            int selectedId = 1;
            juce::AudioIODeviceType* currentType = nullptr;
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (t->getTypeName() == deviceManager.getCurrentAudioDeviceType())
                    currentType = t;
            
            for (int i = 0; i < types.size(); ++i)
            {
                deviceTypeCombo.addItem(types[i]->getTypeName(), i + 1);
                if (currentType != nullptr && types[i]->getTypeName() == currentType->getTypeName())
                    selectedId = i + 1;
            }
            deviceTypeCombo.setSelectedId(selectedId, juce::dontSendNotification);
        }
        
        void updateAllControls()
        {
            juce::AudioIODeviceType* currentType = nullptr;
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (t->getTypeName() == deviceManager.getCurrentAudioDeviceType())
                    currentType = t;
            
            if (currentType == nullptr) return;
            
            currentType->scanForDevices();
            
            auto setup = deviceManager.getAudioDeviceSetup();
            auto* currentDevice = deviceManager.getCurrentAudioDevice();
            
            // Output Devices
            outputDeviceCombo.clear(juce::dontSendNotification);
            auto outNames = currentType->getDeviceNames(false); // false = Outputs
            int outId = -1;
            for (int i = 0; i < outNames.size(); ++i)
            {
                outputDeviceCombo.addItem(outNames[i], i + 1);
                if (outNames[i] == setup.outputDeviceName) outId = i + 1;
            }
            outputDeviceCombo.setSelectedId(outId, juce::dontSendNotification);
            
            // Input Devices
            if (currentType->hasSeparateInputsAndOutputs())
            {
                inputDeviceCombo.clear(juce::dontSendNotification);
                auto inNames = currentType->getDeviceNames(true); // true = Inputs
                int inId = -1;
                for (int i = 0; i < inNames.size(); ++i)
                {
                    inputDeviceCombo.addItem(inNames[i], i + 1);
                    if (inNames[i] == setup.inputDeviceName) inId = i + 1;
                }
                inputDeviceCombo.setSelectedId(inId, juce::dontSendNotification);
            }
            
            // Purpose Combo
            int activeBit1 = setup.inputChannels.findNextSetBit(0);
            int activeBit2 = setup.inputChannels.findNextSetBit(1);
            if (activeBit1 == 0 && activeBit2 == 1 && setup.inputChannels.countNumberOfSetBits() == 2) 
                purposeCombo.setSelectedId(3, juce::dontSendNotification);
            else if (activeBit1 == 0 && setup.inputChannels.countNumberOfSetBits() == 1) 
                purposeCombo.setSelectedId(1, juce::dontSendNotification);
            else if (activeBit1 == 1 && setup.inputChannels.countNumberOfSetBits() == 1) 
                purposeCombo.setSelectedId(2, juce::dontSendNotification);
            else 
                purposeCombo.setSelectedId(4, juce::dontSendNotification);
                
            // Sample Rate
            sampleRateCombo.clear(juce::dontSendNotification);
            if (currentDevice != nullptr)
            {
                auto rates = currentDevice->getAvailableSampleRates();
                int currentRate = juce::roundToInt(currentDevice->getCurrentSampleRate());
                
                for (auto rate : rates)
                {
                    int r = juce::roundToInt(rate);
                    sampleRateCombo.addItem(juce::String(r) + " Hz", r);
                }
                sampleRateCombo.setSelectedId(currentRate, juce::dontSendNotification);
            }
            
            // Buffer Size
            bufferSizeCombo.clear(juce::dontSendNotification);
            if (currentDevice != nullptr)
            {
                auto sizes = currentDevice->getAvailableBufferSizes();
                int currentSize = currentDevice->getCurrentBufferSizeSamples();
                double currentRate = currentDevice->getCurrentSampleRate();
                if (currentRate <= 0.0) currentRate = 48000.0;
                
                for (auto size : sizes)
                {
                    double ms = size * 1000.0 / currentRate;
                    juce::String text = juce::String(size) + " samples (" + juce::String(ms, 1) + " ms)";
                    bufferSizeCombo.addItem(text, size);
                }
                bufferSizeCombo.setSelectedId(currentSize, juce::dontSendNotification);
            }
            
            asioPanelButton.setVisible(currentDevice != nullptr && currentDevice->hasControlPanel());
            updateLatencyText();
            
            resized();
        }

        void updateLatencyText()
        {
            const double rate = std::max(1.0, audioEngine.getSampleRate());
            const int automatic = audioEngine.getAutomaticRecordingLatencySamples();
            const int manual = audioEngine.getManualRecordingOffsetSamples();
            const double autoMs = automatic * 1000.0 / rate;
            const double totalMs = (automatic + manual) * 1000.0 / rate;
            auto text = juce::String(audioEngine.hasCalibratedRecordingLatency()
                                         ? juce::CharPointer_UTF8(u8"実測した自動補正 ")
                                         : juce::CharPointer_UTF8(u8"機器情報からの推定補正 "))
                + juce::String(autoMs, 1)
                + " ms  /  " + juce::String(juce::CharPointer_UTF8(u8"合計補正 "))
                + juce::String(totalMs, 1) + " ms";
            const auto setup = deviceManager.getAudioDeviceSetup();
            if (setup.inputDeviceName.isNotEmpty() && setup.outputDeviceName.isNotEmpty()
                && setup.inputDeviceName != setup.outputDeviceName)
                text += juce::String(juce::CharPointer_UTF8(u8"  ⚠ 別々の入出力機器は長時間録音でずれる場合があります"));
            latencyInfoLabel.setText(text, juce::dontSendNotification);
            latencyInfoLabel.setTooltip(text);
        }

        void updateCalibrationControls()
        {
            const bool running = getCalibrationRunning && getCalibrationRunning();
            deviceTypeCombo.setEnabled(!running);
            outputDeviceCombo.setEnabled(!running);
            inputDeviceCombo.setEnabled(!running);
            purposeCombo.setEnabled(!running);
            sampleRateCombo.setEnabled(!running);
            bufferSizeCombo.setEnabled(!running);
            manualOffsetSlider.setEnabled(!running);
            asioPanelButton.setEnabled(!running);
            calibrationButton.setButtonText(running
                ? juce::String(juce::CharPointer_UTF8(u8"測定をキャンセル"))
                : juce::String(juce::CharPointer_UTF8(u8"自動音ズレ測定を開始")));

            auto status = getCalibrationStatus
                ? getCalibrationStatus()
                : juce::String(juce::CharPointer_UTF8(u8"測定状態を取得できません"));
            if (running)
            {
                const float inputDb = juce::Decibels::gainToDecibels(
                    audioEngine.getCurrentInputLevel(), -100.0f);
                status += juce::String(juce::CharPointer_UTF8(u8"\nマイク入力 "))
                          + juce::String(inputDb, 0)
                          + juce::String(juce::CharPointer_UTF8(u8" dB / ダイレクトモニターOFF"));
            }
            else
            {
                status += juce::String(juce::CharPointer_UTF8(
                    u8"\n通常録音ではAG03のダイレクトモニターをON"));
            }
            calibrationStatusLabel.setText(status, juce::dontSendNotification);
            calibrationStatusLabel.setTooltip(status);
        }
        
        juce::AudioDeviceManager& deviceManager;
        AudioEngine& audioEngine;
        std::function<void(double)> onManualOffsetChanged;
        std::function<void()> onStartCalibration;
        std::function<void()> onCancelCalibration;
        std::function<juce::String()> getCalibrationStatus;
        std::function<bool()> getCalibrationRunning;
        
        juce::Label deviceTypeLabel, outputDeviceLabel, inputDeviceLabel, purposeLabel, sampleRateLabel, bufferSizeLabel;
        juce::ComboBox deviceTypeCombo, outputDeviceCombo, inputDeviceCombo, purposeCombo, sampleRateCombo, bufferSizeCombo;
        juce::Label manualOffsetLabel, latencyInfoLabel, calibrationStatusLabel;
        juce::Slider manualOffsetSlider;
        juce::TextButton calibrationButton, asioPanelButton;
        juce::ScopedMessageBox calibrationPrompt;
        
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsModalComponent)
    };

    class BpmCandidateDialogComponent final : public juce::Component
    {
    public:
        explicit BpmCandidateDialogComponent(
            std::vector<TempoCandidate> candidatesToUse)
            : candidates(std::move(candidatesToUse))
        {
            setSize(460, 260);
            title.setText(juce::String::fromUTF8(u8"BPM候補を聴き比べる"),
                          juce::dontSendNotification);
            title.setFont(juce::Font(juce::FontOptions(20.0f,
                                                       juce::Font::bold)));
            title.setColour(juce::Label::textColourId, UiTheme::textPrimary);
            explanation.setText(
                juce::String::fromUTF8(
                    u8"「試聴」で伴奏とメトロノームを同時に再生し、\n合う候補を選んでから決定してください。"),
                juce::dontSendNotification);
            explanation.setFont(juce::Font(juce::FontOptions(12.0f)));
            explanation.setColour(juce::Label::textColourId,
                                  UiTheme::textSecondary);
            explanation.setJustificationType(juce::Justification::topLeft);

            for (size_t index = 0; index < candidates.size(); ++index)
            {
                const auto confidence = juce::roundToInt(
                    candidates[index].confidence * 100.0);
                candidateBox.addItem(
                    juce::String(candidates[index].bpm, 1) + " BPM  ("
                        + juce::String(confidence)
                        + juce::String::fromUTF8(u8"% 信頼度)"),
                    static_cast<int>(index) + 1);
            }
            candidateBox.setSelectedId(candidates.empty() ? 0 : 1,
                                       juce::dontSendNotification);
            candidateBox.setColour(juce::ComboBox::backgroundColourId,
                                   UiTheme::controlSurface);
            candidateBox.setColour(juce::ComboBox::outlineColourId,
                                   UiTheme::border);
            candidateBox.setColour(juce::ComboBox::textColourId,
                                   UiTheme::textPrimary);

            previewButton.setButtonText(
                juce::String::fromUTF8(u8"このBPMで試聴"));
            acceptButton.setButtonText(
                juce::String::fromUTF8(u8"このBPMに決定"));
            closeButton.setButtonText(juce::String::fromUTF8(u8"閉じる"));
            previewButton.setColour(juce::TextButton::buttonColourId,
                                    UiTheme::controlSurface);
            acceptButton.setColour(juce::TextButton::buttonColourId,
                                   UiTheme::accent);
            closeButton.setColour(juce::TextButton::buttonColourId,
                                  UiTheme::controlSurface);

            const std::array<juce::Component*, 6> components {
                &title, &explanation, &candidateBox,
                &previewButton, &acceptButton, &closeButton
            };
            for (auto* component : components)
                addAndMakeVisible(*component);

            previewButton.onClick = [this]
            {
                if (onPreview && getSelectedBpm() > 0.0)
                    onPreview(getSelectedBpm());
            };
            acceptButton.onClick = [this]
            {
                if (onAccept && getSelectedBpm() > 0.0)
                    onAccept(getSelectedBpm());
            };
            closeButton.onClick = [this]
            {
                if (onClose)
                    onClose();
            };
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(UiTheme::windowBackground);
            UiTheme::fillCard(g, getLocalBounds().toFloat().reduced(14.0f),
                              12.0f);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(30, 24);
            title.setBounds(area.removeFromTop(30));
            explanation.setBounds(area.removeFromTop(58));
            area.removeFromTop(8);
            candidateBox.setBounds(area.removeFromTop(38));
            area.removeFromTop(18);
            auto buttons = area.removeFromTop(42);
            previewButton.setBounds(buttons.removeFromLeft(130));
            buttons.removeFromLeft(10);
            acceptButton.setBounds(buttons.removeFromLeft(140));
            buttons.removeFromLeft(10);
            closeButton.setBounds(buttons.removeFromLeft(90));
        }

        std::function<void(double)> onPreview;
        std::function<void(double)> onAccept;
        std::function<void()> onClose;

    private:
        double getSelectedBpm() const
        {
            const auto index = candidateBox.getSelectedId() - 1;
            return juce::isPositiveAndBelow(index,
                                             static_cast<int>(candidates.size()))
                ? candidates[static_cast<size_t>(index)].bpm : 0.0;
        }

        std::vector<TempoCandidate> candidates;
        juce::Label title;
        juce::Label explanation;
        juce::ComboBox candidateBox;
        juce::TextButton previewButton;
        juce::TextButton acceptButton;
        juce::TextButton closeButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
            BpmCandidateDialogComponent)
    };
    
    static SettingsLookAndFeel settingsLookAndFeel;
}

//==============================================================================
MainComponent::MainComponent()
{
    latencyCalibration = std::make_unique<LatencyCalibrationSession>();

    // 日本語化
    juce::String translations = juce::CharPointer_UTF8(u8R"(
"Audio device type:" = "デバイスの種類:"
"Output:" = "出力デバイス:"
"Input:" = "入力デバイス:"
"Sample rate:" = "サンプルレート:"
"Audio buffer size:" = "バッファサイズ:"
"Active output channels:" = "出力チャンネル:"
"Active input channels:" = "入力チャンネル:"
"Test" = "テスト再生"
"Show control panel" = "コントロールパネルを開く"
)");
    juce::LocalisedStrings::setCurrentMappings(new juce::LocalisedStrings(translations, false));

    setLookAndFeel(&modernLookAndFeel);
    setSize (1440, 900);

    initialiseAppSettings();
    assistantController = std::make_unique<AssistantController>(
        audioEngine,
        [this] { return trackArea.getSelectedTrackIndex(); },
        [this](int index) { trackArea.setSelectedTrackIndex(index); },
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("SimpleRecPro").getChildFile("sound-preferences.json"));
    assistantController->setExternalBusyCheck([this]
    {
        return projectIoInProgress.load() || isLatencyCalibrationRunning()
            || autoMixAnalysisInProgress || referenceMixAnalysisInProgress;
    });

    auto initialiseAudio = [this](int numInputs) {
        std::unique_ptr<juce::XmlElement> savedAudioState;
        if (appSettings != nullptr)
            savedAudioState = juce::parseXML(appSettings->getValue("audioDeviceState"));
        setAudioChannels(numInputs, 2, savedAudioState.get());
    };

    if (juce::RuntimePermissions::isRequired (juce::RuntimePermissions::recordAudio)
        && ! juce::RuntimePermissions::isGranted (juce::RuntimePermissions::recordAudio))
    {
        juce::RuntimePermissions::request (juce::RuntimePermissions::recordAudio,
                                           [initialiseAudio] (bool granted) { initialiseAudio(granted ? 1 : 0); });
    }
    else
    {
        initialiseAudio(1);
    }

    // AudioEngineをUI部品に渡す
    header.setAudioEngine(&audioEngine);
    transportBar.setAudioEngine(&audioEngine);
    trackArea.setAudioEngine(&audioEngine);
    transportBar.onExportClicked = [this]() { showExportDialog(); };
    header.onExportClicked = [this]() { showExportDialog(); };
    trackArea.onSelectedTrackChanged = [this](int trackIndex) {
        refreshStudioPanel(trackIndex);
    };
    trackArea.onStudioClicked = [this](int trackIndex) {
        showStudioPanel(trackIndex);
    };
    trackArea.onVst3BrowserClicked = [this](int trackIndex) {
        showVst3Browser(trackIndex);
    };

    addAndMakeVisible (header);
    addAndMakeVisible (trackArea);
    addAndMakeVisible (transportBar);
    addAndMakeVisible (transportBar.getEditingToolbar());
    addAndMakeVisible(comparisonBar);
    comparisonBar.onCompare = [this](const juce::String& mode)
    {
        auto* args = new juce::DynamicObject();
        args->setProperty("mode", mode);
        auto* request = new juce::DynamicObject();
        request->setProperty("action", "compare");
        request->setProperty("args", juce::var(args));
        const auto response = handleAssistantRequest(juce::var(request));
        if (response.hasProperty("error")) showStatus(response["error"].toString(), true);
        comparisonBar.setState(assistantController->getComparisonState());
    };
    
    // 初期トラックを1つ追加（ARM状態で録音準備完了）
    int idx = audioEngine.addTrack("Track 1");
    audioEngine.toggleArm(idx);
    audioEngine.markClean();
    trackArea.syncFromEngine();
    trackArea.setSelectedTrackIndex(idx);
    showStudioPanel(idx);
    
    // 設定ボタンのコールバック
    header.onSettingsClicked = [this]() {
        if (settingsWindow != nullptr)
        {
            settingsWindow->toFront(true);
            return;
        }
        const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
        auto* settingsModal = new SettingsModalComponent(
            deviceManager,
            audioEngine,
            [safeThis](double milliseconds) {
                if (safeThis != nullptr)
                    safeThis->setManualRecordingOffsetMs(milliseconds);
            },
            [safeThis]() {
                if (safeThis != nullptr)
                    safeThis->startLatencyCalibration();
            },
            [safeThis]() {
                if (safeThis != nullptr)
                    safeThis->cancelLatencyCalibration();
            },
            [safeThis]() {
                return safeThis != nullptr
                    ? safeThis->getLatencyCalibrationStatusText()
                    : juce::String();
            },
            [safeThis]() {
                return safeThis != nullptr && safeThis->isLatencyCalibrationRunning();
            });
        settingsModal->setSize(550, 630);
        settingsModal->setLookAndFeel(&settingsLookAndFeel);
        
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(settingsModal);
        options.componentToCentreAround = this;
        options.dialogTitle = juce::CharPointer_UTF8(u8"⚙ オーディオ設定");
        options.dialogBackgroundColour = UiTheme::panelBackground;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = false;
        options.resizable = false;
        
        settingsWindow = options.launchAsync();
        if (settingsWindow != nullptr)
            settingsWindow->setLookAndFeel(&settingsLookAndFeel);
    };

    configureProjectActions();
    
    // AudioEngineの状態変更コールバック（UIスレッドでの更新）
    audioEngine.onStateChanged = [safeThis = juce::Component::SafePointer<MainComponent>(this)]() {
        juce::MessageManager::callAsync([safeThis]() {
            if (safeThis == nullptr) return;

            safeThis->transportBar.updatePlaybackState(safeThis->audioEngine.getPlaybackState());
            safeThis->transportBar.updateTimeDisplay(safeThis->audioEngine.getCurrentTime(),
                                                     safeThis->audioEngine.getDuration());
            safeThis->transportBar.syncFromEngine();
            safeThis->header.syncFromEngine();
            safeThis->refreshProjectDisplay();
            safeThis->trackArea.syncFromEngine();
            safeThis->trackArea.updateScrollbar();
            safeThis->trackArea.repaint();
            safeThis->trackArea.resized(); // トラック追加時にボタン位置も更新
            if (safeThis->studioPanel != nullptr)
                safeThis->refreshStudioPanel(
                    safeThis->trackArea.getSelectedTrackIndex());
            if (safeThis->vst3Browser != nullptr)
                safeThis->refreshVst3Browser(
                    safeThis->vst3Browser->getSelectedTrackIndex());
        });
    };

    audioEngine.onStatusMessage = [safeThis = juce::Component::SafePointer<MainComponent>(this)]
                                  (const juce::String& message, bool isError) {
        juce::MessageManager::callAsync([safeThis, message, isError]() {
            if (safeThis != nullptr)
                safeThis->showStatus(message, isError);
        });
    };
    audioEngine.onPluginScanFinished =
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        (const juce::File& pluginFile, bool succeeded,
         const juce::String& reason)
    {
        juce::MessageManager::callAsync(
            [safeThis, pluginFile, succeeded, reason]
        {
            if (safeThis == nullptr
                || safeThis->pendingVst3ScanFile == juce::File())
                return;
#if JUCE_WINDOWS
            const bool matches = safeThis->pendingVst3ScanFile
                .getFullPathName().equalsIgnoreCase(pluginFile.getFullPathName());
#else
            const bool matches = safeThis->pendingVst3ScanFile
                .getFullPathName() == pluginFile.getFullPathName();
#endif
            if (!matches)
                return;

            safeThis->vst3ScanMarkerFile.deleteFile();
            safeThis->pendingVst3ScanFile = {};
            if (succeeded)
            {
                safeThis->vst3Catalog.clearQuarantine(pluginFile);
                safeThis->vst3Catalog.markRecentlyUsed(pluginFile);
            }
            else
            {
                safeThis->vst3Catalog.quarantine(
                    pluginFile,
                    reason.isNotEmpty()
                        ? reason
                        : juce::String::fromUTF8(
                            u8"プラグインの読み込みを完了できませんでした。"));
            }
            safeThis->vst3Catalog.saveState(
                safeThis->vst3CatalogStateFile);
            if (safeThis->vst3Browser != nullptr)
            {
                safeThis->vst3Browser->refreshCatalogView();
                safeThis->vst3Browser->setStatusMessage(
                    succeeded
                        ? juce::String::fromUTF8(
                            u8"VST3を安全に追加しました")
                        : juce::String::fromUTF8(
                            u8"読み込みに失敗したVST3を隔離しました"));
            }
        });
    };
    audioEngine.onDirtyChanged = [safeThis = juce::Component::SafePointer<MainComponent>(this)](bool) {
        juce::MessageManager::callAsync([safeThis]() {
            if (safeThis != nullptr)
                safeThis->refreshProjectDisplay();
        });
    };

    refreshProjectDisplay();
    transportBar.syncFromEngine();
    assistantBridge = std::make_unique<AssistantBridge>(
        [this](const juce::var& request) { return handleAssistantRequest(request); });
    assistantBridge->start();
    
    // タイマーを開始（30fps程度でUI更新）
    startTimerHz(30);
    
    // キーボードイベントを確実に受け取るための設定
    setWantsKeyboardFocus(true);
    grabKeyboardFocus();

    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]() {
        if (safeThis != nullptr)
            safeThis->offerRecoveryIfAvailable();
    });
}

MainComponent::~MainComponent()
{
    stopTimer();
    assistantBridge.reset();
    cancelLatencyCalibration();
    latencyAnalysisThread.removeAllJobs(true, -1);
    projectIoThread.removeAllJobs(true, -1);
    recoveryGeneration.fetch_add(1);
    recoverySaveThread.removeAllJobs(true, -1);
    studioWorkThread.removeAllJobs(true, -1);
    exportFileChooser.reset();
    referenceMixFileChooser.reset();

    auto closeDialog = [](juce::Component::SafePointer<juce::DialogWindow>& window)
    {
        if (window == nullptr)
            return;
        window->clearContentComponent();
        if (window != nullptr)
            window->exitModalState(0);
        window = nullptr;
    };
    closeDialog(exportWindow);
    studioDock.reset();
    closeDialog(vst3Window);
    closeDialog(bpmCandidateWindow);
    closeDialog(settingsWindow);

    if (vst3CatalogStateFile != juce::File())
        vst3Catalog.saveState(vst3CatalogStateFile);
    if (appSettings != nullptr)
    {
        if (auto audioState = deviceManager.createStateXml())
            appSettings->setValue("audioDeviceState", audioState->toString());
        appSettings->saveIfNeeded();
    }
    setLookAndFeel(nullptr);
    shutdownAudio();
}

//==============================================================================
void MainComponent::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    if (latencyCalibration != nullptr)
    {
        latencyCalibration->generation.fetch_add(1);
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
    }

    audioEngine.prepare(samplesPerBlockExpected, sampleRate);
    if (assistantController != nullptr)
        assistantController->preparePreview(samplesPerBlockExpected, sampleRate);

    auto* device = deviceManager.getCurrentAudioDevice();
    currentNumInputChannels = (device != nullptr) ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
    audioEngine.setDeviceLatencySamples(device != nullptr ? device->getInputLatencyInSamples() : 0,
                                        device != nullptr ? device->getOutputLatencyInSamples() : 0);
    audioEngine.clearCalibratedRecordingLatency();
    audioEngine.setManualRecordingOffsetSamples(
        juce::roundToInt(manualRecordingOffsetMs * sampleRate / 1000.0));
    latencyDeviceRefreshRequested.store(true, std::memory_order_release);

    // オーディオコールバック内でのメモリアロケーションを避けるため、事前にバッファを確保
    tempInputBuffer.setSize(std::max(1, currentNumInputChannels), std::max(samplesPerBlockExpected, 8192));
    tempInputBuffer.clear();
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    // JUCEのAudioAppComponentでは、bufferToFillのbufferに入出力が共有される場合がある
    // 録音前にデータを読み取る必要があるため、事前に確保した一時バッファにコピーする
    const float* channels[2] = { nullptr, nullptr };
    int availableInputChannels = 0;

    if (currentNumInputChannels > 0 && tempInputBuffer.getNumSamples() >= bufferToFill.numSamples)
    {
        availableInputChannels = std::min(
            { currentNumInputChannels, bufferToFill.buffer->getNumChannels(), 2 });
        for (int ch = 0; ch < availableInputChannels; ++ch)
        {
            tempInputBuffer.copyFrom(ch, 0, *bufferToFill.buffer, ch, bufferToFill.startSample, bufferToFill.numSamples);
        }

        for (int ch = 0; ch < availableInputChannels; ++ch)
            channels[ch] = tempInputBuffer.getReadPointer(ch);
    }

    if (processLatencyCalibration(bufferToFill, channels, availableInputChannels))
        return;
    if (assistantController != nullptr && assistantController->renderPreview(bufferToFill))
        return;

    audioEngine.processBlock(bufferToFill,
                             availableInputChannels > 0 ? channels : nullptr,
                             availableInputChannels);
}

void MainComponent::releaseResources()
{
    if (latencyCalibration != nullptr)
    {
        latencyCalibration->generation.fetch_add(1);
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
    }
    audioEngine.release();
    if (assistantController != nullptr) assistantController->releasePreview();
}

juce::var MainComponent::handleAssistantRequest(const juce::var& request)
{
    const auto action = request["action"].toString();
    if ((projectIoInProgress.load() || isLatencyCalibrationRunning()
         || autoMixAnalysisInProgress || referenceMixAnalysisInProgress)
        && action != "get_context" && action != "get_job" && action != "list_preferences"
        && !(action == "compare" && request["args"]["mode"].toString() == "stop"))
    {
        auto* error = new juce::DynamicObject();
        error->setProperty("error", juce::String::fromUTF8(u8"読み込み・保存・音声解析が完了してから操作してください。"));
        return juce::var(error);
    }
    auto response = assistantController->handleRequest(request);
    if (action == "get_context" && response.isObject())
        response.getDynamicObject()->setProperty("summary", assistantController->getSummaryText());
    return response;
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll(UiTheme::windowBackground);
}

void MainComponent::resized()
{
    const auto layout = WorkspaceLayout::calculate(getLocalBounds(),
        transportBar.getPreferredHeight(getWidth()),
        transportBar.getEditingToolbarHeight(getWidth()),
        studioPanel != nullptr && studioPanel->isVisible());
    header.setBounds(layout.header);
    transportBar.setBounds(layout.transport);
    comparisonBar.setBounds(layout.comparison);
    transportBar.getEditingToolbar().setBounds(layout.editingToolbar);
    trackArea.setBounds(layout.tracks);
    if (studioPanel != nullptr)
        studioPanel->setBounds(layout.studio);
}

void MainComponent::timerCallback()
{
    serviceLatencyCalibration();
    if (assistantController != nullptr) comparisonBar.setState(assistantController->getComparisonState());
    if (!projectIoInProgress.load() && assistantController != nullptr)
    {
        const auto enabled = !assistantController->isPreviewBusy();
        header.setEnabled(enabled);
        trackArea.setEnabled(enabled);
        transportBar.setEnabled(enabled);
        transportBar.getEditingToolbar().setEnabled(enabled);
        if (studioPanel != nullptr) studioPanel->setEnabled(enabled);
    }

    const auto playbackState = audioEngine.getPlaybackState();
    // 再生中・録音中にUI（時間表示と波形）をリアルタイム更新
    if (playbackState != PlaybackState::Stopped)
    {
        transportBar.updateTimeDisplay(audioEngine.getCurrentTime(), audioEngine.getDuration());
        trackArea.updateScrollbar();
        trackArea.autoScrollToPlayhead();
        trackArea.repaint();
    }
    else
    {
        // 停止中は波形まで毎秒30回描き直さず、動いている入力メーターだけ更新する。
        trackArea.repaintInputMeters();
    }
    maybeSaveRecovery();

    if (exportDialog != nullptr)
    {
        const auto status = audioEngine.getExportStatus();
        if (!exportDestinationPending)
        {
            exportDialog->setProgress(status.progress, status.message);
            exportDialog->setExportRunning(status.active);
        }
    }

    if (studioPanel != nullptr)
    {
        studioPanel->setMasteringMeters(
            audioEngine.getMasteringLoudnessLufs(),
            audioEngine.getMasteringOutputPeak(),
            audioEngine.getMasteringGainReductionDb());
    }
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown())
    {
        int kc = key.getKeyCode();
        if (kc == 's' || kc == 'S')
        {
            if (key.getModifiers().isShiftDown())
                header.triggerSaveAs();
            else
                header.triggerSave();
            return true;
        }
        if (kc == 'o' || kc == 'O')
        {
            header.triggerOpen();
            return true;
        }
        if (kc == 'z' || kc == 'Z')
        {
            if (key.getModifiers().isShiftDown())
                header.triggerRedo();
            else
                header.triggerUndo();
            return true;
        }
        if (kc == 'y' || kc == 'Y')
        {
            header.triggerRedo();
            return true;
        }
    }

    if (key.getKeyCode() == juce::KeyPress::spaceKey)
    {
        if (audioEngine.getPlaybackState() == PlaybackState::Stopped)
            audioEngine.play();
        else
            audioEngine.stop();
        return true;
    }
    return false;
}

void MainComponent::showExportDialog()
{
    if (audioEngine.getPlaybackState() != PlaybackState::Stopped)
    {
        showStatus(juce::String::fromUTF8(u8"停止してから書き出してください。"), true);
        return;
    }
    if (audioEngine.getDuration() <= 0.0)
    {
        showStatus(juce::String::fromUTF8(u8"書き出す音声がありません。"), true);
        return;
    }
    if (exportWindow != nullptr)
    {
        exportWindow->toFront(true);
        return;
    }

    const auto tracks = audioEngine.getTracksSnapshot();
    juce::StringArray trackNames;
    for (const auto& track : tracks) trackNames.add(track.name);

    auto initial = audioEngine.getProjectDawSettings().exportDefaults;
    const auto project = audioEngine.getProjectDawSettings();
    initial.rangeStartSeconds = project.hasLoopStart
        ? project.loopStartSeconds : 0.0;
    initial.rangeEndSeconds = project.hasLoopEnd
        ? project.loopEndSeconds : 0.0;

    auto* content = new ExportDialogComponent(trackNames, initial);
    content->setSize(540, 610);
    exportDialog = content;
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    content->onStartExport = [safeThis](const ExportSettings& settings)
    {
        if (safeThis == nullptr || safeThis->exportDestinationPending
            || safeThis->exportFileChooser != nullptr)
            return;
        safeThis->exportDestinationPending = true;
        if (safeThis->exportDialog != nullptr)
        {
            safeThis->exportDialog->setProgress(
                0.0, juce::String::fromUTF8(u8"保存先を選んでください…"));
            safeThis->exportDialog->setExportRunning(true);
        }
        auto projectSettings = safeThis->audioEngine.getProjectDawSettings();
        projectSettings.exportDefaults = settings;
        safeThis->audioEngine.setProjectDawSettings(projectSettings);

        const juce::String extension = settings.format == ExportFormat::mp3
            ? ".mp3" : ".wav";
        juce::String suggestedName = settings.target == ExportTarget::selectedTrack
            ? "track" : "mixdown";
        const auto currentTracks = safeThis->audioEngine.getTracksSnapshot();
        if (settings.target == ExportTarget::selectedTrack
            && juce::isPositiveAndBelow(settings.trackIndex,
                                        static_cast<int>(currentTracks.size())))
            suggestedName = currentTracks[static_cast<size_t>(settings.trackIndex)].name;

        safeThis->exportFileChooser = std::make_unique<juce::FileChooser>(
            juce::CharPointer_UTF8(u8"書き出し先を選択"),
            juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                .getChildFile(suggestedName + extension),
            settings.format == ExportFormat::mp3 ? "*.mp3" : "*.wav");
        safeThis->exportFileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis, settings, extension](const juce::FileChooser& chooser)
            {
                if (safeThis == nullptr)
                    return;
                auto file = chooser.getResult();
                safeThis->exportFileChooser.reset();
                safeThis->exportDestinationPending = false;
                if (file == juce::File())
                {
                    if (safeThis->exportDialog != nullptr)
                    {
                        safeThis->exportDialog->setExportRunning(false);
                        safeThis->exportDialog->setProgress(
                            0.0, juce::String::fromUTF8(u8"書き出しをキャンセルしました"));
                    }
                    return;
                }
                file = file.withFileExtension(extension.substring(1));
                const bool started = safeThis->audioEngine.startExport(file, settings);
                if (safeThis->exportDialog != nullptr)
                {
                    safeThis->exportDialog->setExportRunning(started);
                }
            });
    };
    content->onCancelExport = [safeThis]()
    {
        if (safeThis != nullptr) safeThis->audioEngine.cancelExport();
    };
    content->onClose = [safeThis]()
    {
        if (safeThis == nullptr) return;
        if (safeThis->exportDestinationPending)
            return;
        if (safeThis->audioEngine.getExportStatus().active)
            safeThis->audioEngine.cancelExport();
        if (safeThis->exportWindow != nullptr)
            safeThis->exportWindow->exitModalState(0);
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.dialogTitle = juce::CharPointer_UTF8(u8"音声を書き出す");
    options.dialogBackgroundColour = UiTheme::panelBackground;
    options.escapeKeyTriggersCloseButton = false;
    options.useNativeTitleBar = false;
    options.resizable = false;
    exportWindow = options.launchAsync();
    if (exportWindow != nullptr)
        exportWindow->setTitleBarButtonsRequired(0, false);
}

void MainComponent::refreshStudioPanel(int selectedTrackIndex)
{
    if (studioPanel == nullptr)
        return;
    const auto tracks = audioEngine.getTracksSnapshot();
    StudioPanelState state;
    for (const auto& track : tracks) state.trackNames.add(track.name);
    state.selectedTrackIndex = tracks.empty()
        ? -1 : juce::jlimit(0, static_cast<int>(tracks.size()) - 1,
                           selectedTrackIndex);
    state.pitchNetInstalled = audioEngine.isPitchNetInstalled();
    const auto project = audioEngine.getProjectDawSettings();
    state.mastering = project.mastering;
    state.hasSpecialFxRange = project.hasLoopStart && project.hasLoopEnd
        && project.loopEndSeconds > project.loopStartSeconds + 1.0e-6;
    state.specialFxRangeStartSeconds = project.loopStartSeconds;
    state.specialFxRangeEndSeconds = project.loopEndSeconds;
    const double currentTime = audioEngine.getCurrentTime();
    state.tempoPoint = audioEngine.getTempoPointAt(currentTime);
    // The controls inherit BPM/meter from the active point, but the Add
    // button always targets the playhead rather than editing that older point.
    state.tempoPoint.timeSeconds = currentTime;
    if (juce::isPositiveAndBelow(state.selectedTrackIndex,
                                 static_cast<int>(tracks.size())))
    {
        const auto& track = tracks[static_cast<size_t>(state.selectedTrackIndex)];
        state.trackVolume = track.volume;
        state.trackPan = track.pan;
        state.trackPitchSemitones = track.pitchSemitones;
        state.trackPlaybackSpeed = track.playbackSpeed;
        state.pitchCorrection = track.pitchCorrection;
        state.pitchCorrectionApplied =
            audioEngine.isPitchCorrectionActive(state.selectedTrackIndex);
        if (!state.pitchCorrectionApplied)
        {
            state.pitchCorrection.enabled = false;
            state.pitchCorrection.auditionCorrected = false;
            state.pitchCorrection.key = project.musicalKey;
            state.pitchCorrection.scale = project.musicalScale;
            state.pitchCorrection.strength = project.pitchCorrectionStrength;
        }
        state.selectedTrackRole = track.role;
        state.simpleMix = track.simpleMix;
        state.noiseReduction = track.noiseReduction;
        state.specialFxRegionCount = static_cast<int>(track.specialFxRegions.size());
    }
    studioPanel->setState(state);
}

void MainComponent::showStudioPanel(int selectedTrackIndex)
{
    if (studioPanel != nullptr)
    {
        trackArea.setSelectedTrackIndex(selectedTrackIndex);
        refreshStudioPanel(selectedTrackIndex);
        studioPanel->setVisible(true);
        resized();
        return;
    }

    studioDock = std::make_unique<StudioPanelComponent>();
    auto* content = studioDock.get();
    content->setDockedMode(true);
    addAndMakeVisible(*content);
    studioPanel = content;
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    content->onSelectedTrackChanged = [safeThis](int trackIndex)
    {
        if (safeThis != nullptr)
        {
            safeThis->trackArea.setSelectedTrackIndex(trackIndex);
            safeThis->refreshStudioPanel(trackIndex);
        }
    };
    content->onTrackVolumeChanged = [safeThis](int trackIndex, float volume) {
        if (safeThis != nullptr) safeThis->audioEngine.setTrackVolume(trackIndex, volume);
    };
    content->onTrackPanChanged = [safeThis](int trackIndex, float pan) {
        if (safeThis != nullptr) safeThis->audioEngine.setTrackPan(trackIndex, pan);
    };
    content->onTrackPitchSemitonesChanged = [safeThis](int trackIndex, double semitones) {
        if (safeThis != nullptr) safeThis->audioEngine.setTrackPitchSemitones(trackIndex, semitones);
    };
    content->onTrackPlaybackSpeedChanged = [safeThis](int trackIndex, double speed) {
        if (safeThis != nullptr) safeThis->audioEngine.setTrackPlaybackSpeed(trackIndex, speed);
    };
    content->onResetTrackTransform = [safeThis](int trackIndex) {
        if (safeThis != nullptr) safeThis->audioEngine.resetTrackTransform(trackIndex);
    };
    content->onBeginTrackEdit = [safeThis]() {
        if (safeThis != nullptr) safeThis->audioEngine.beginEdit();
    };
    content->onEndTrackEdit = [safeThis]() {
        if (safeThis != nullptr) safeThis->audioEngine.endEdit();
    };
    content->onTrackRoleChanged = [safeThis](int trackIndex, TrackRole role)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.setTrackRole(trackIndex, role);
    };
    content->onNaturalPitchCorrection =
        [safeThis](int trackIndex, const PitchCorrectionSettings& settings)
    {
        if (safeThis == nullptr) return;
        auto project = safeThis->audioEngine.getProjectDawSettings();
        project.musicalKey = settings.key;
        project.musicalScale = settings.scale;
        project.pitchCorrectionStrength = settings.strength;
        safeThis->audioEngine.setProjectDawSettings(project);
        safeThis->audioEngine.applyNaturalPitchCorrection(trackIndex);
    };
    content->onOpenPitchDetails = [safeThis](int trackIndex)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.showPitchCorrectionDetails(trackIndex);
    };
    content->onPitchComparisonChanged = [safeThis](int trackIndex, bool corrected)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.setTrackPitchCorrectionAudition(trackIndex,
                                                                  corrected);
    };
    content->onMixSettingsChanged =
        [safeThis](int trackIndex, const SimpleMixSettings& settings)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.setTrackSimpleMixSettings(trackIndex, settings);
    };
    content->onNoiseReductionChanged =
        [safeThis](int trackIndex, const NoiseReductionSettings& settings)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.setTrackNoiseReductionSettings(trackIndex,
                                                                  settings);
    };
    content->onRunAutoMix = [safeThis]()
    {
        if (safeThis != nullptr)
            safeThis->runAutoMix();
    };
    content->onApplySpecialFx =
        [safeThis](int trackIndex, SpecialFxType type, float amount,
                   float wet, double fadeSeconds)
    {
        if (safeThis == nullptr) return;
        const auto project = safeThis->audioEngine.getProjectDawSettings();
        if (!project.hasLoopStart || !project.hasLoopEnd
            || project.loopEndSeconds <= project.loopStartSeconds + 1.0e-6)
        {
            safeThis->showStatus(
                juce::String::fromUTF8(u8"先にタイムラインでAとBを設定してください"),
                true);
            safeThis->refreshStudioPanel(trackIndex);
            return;
        }
        const bool added = safeThis->audioEngine.addTrackSpecialFxRegion(
            trackIndex, type, project.loopStartSeconds, project.loopEndSeconds,
            amount, wet, fadeSeconds);
        safeThis->showStatus(
            added ? juce::String::fromUTF8(u8"A-B範囲に特殊FXを追加しました")
                  : juce::String::fromUTF8(u8"特殊FXを追加できませんでした"),
            !added);
        safeThis->refreshStudioPanel(trackIndex);
    };
    content->onClearSpecialFx = [safeThis](int trackIndex)
    {
        if (safeThis == nullptr) return;
        safeThis->audioEngine.clearTrackSpecialFxRegions(trackIndex);
        safeThis->showStatus(
            juce::String::fromUTF8(u8"このトラックの特殊FXを解除しました"));
        safeThis->refreshStudioPanel(trackIndex);
    };
    content->onRunReferenceMix = [safeThis](int trackIndex)
    {
        if (safeThis != nullptr)
            safeThis->chooseAndRunReferenceMix(trackIndex);
    };
    content->onOpenVst3Browser = [safeThis](int trackIndex)
    {
        if (safeThis != nullptr) safeThis->showVst3Browser(trackIndex);
    };
    content->onRhythmCorrection = [safeThis](int trackIndex, float strength)
    {
        if (safeThis == nullptr) return;
        const auto tracks = safeThis->audioEngine.getTracksSnapshot();
        if (!juce::isPositiveAndBelow(trackIndex,
                                      static_cast<int>(tracks.size()))
            || tracks[static_cast<size_t>(trackIndex)].clips.empty())
            return;
        auto clipId = safeThis->audioEngine.getSelectedClipId();
        const auto& track = tracks[static_cast<size_t>(trackIndex)];
        if (std::none_of(track.clips.begin(), track.clips.end(),
                         [&clipId](const AudioClip& clip) { return clip.id == clipId; }))
            clipId = track.clips.front().id;
        const int markerCount = safeThis->audioEngine.quantizeClipRhythm(clipId,
                                                                         strength);
        safeThis->showStatus(
            markerCount > 0
                ? juce::String(markerCount)
                    + juce::String::fromUTF8(
                        u8"個のノートを非破壊で整えました。波形上の黄色い帯は中央で移動、左右端で長さを調整できます")
                : juce::String::fromUTF8(u8"整列できるタイミングが見つかりませんでした"),
            markerCount <= 0);
    };
    content->onBpmEstimate = [safeThis](int trackIndex)
    {
        if (safeThis != nullptr) safeThis->estimateTrackBpm(trackIndex);
    };
    content->onAddTempoPoint = [safeThis](const TempoPoint& point)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.setTempoPoint(
                                                safeThis->audioEngine.getCurrentTime(),
                                                point.bpm,
                                                point.numerator,
                                                point.denominator);
    };
    content->onCreateHarmony = [safeThis](int trackIndex, int semitones)
    {
        if (safeThis == nullptr) return;
        safeThis->audioEngine.duplicateTrackAsHarmony(
            trackIndex, semitones, 0.72f, semitones >= 0 ? 0.28f : -0.28f);
    };
    content->onMasteringSettingsChanged =
        [safeThis](const MasteringSettings& settings)
    {
        if (safeThis == nullptr) return;
        auto project = safeThis->audioEngine.getProjectDawSettings();
        project.mastering = settings;
        safeThis->audioEngine.setProjectDawSettings(project);
    };
    content->onClose = [safeThis]()
    {
        if (safeThis != nullptr && safeThis->studioPanel != nullptr)
        {
            safeThis->studioPanel->setVisible(false);
            safeThis->resized();
        }
    };

    refreshStudioPanel(selectedTrackIndex);
    resized();
}

void MainComponent::refreshVst3Browser(int selectedTrackIndex)
{
    if (vst3Browser == nullptr)
        return;
    const auto tracks = audioEngine.getTracksSnapshot();
    juce::StringArray names;
    for (const auto& track : tracks) names.add(track.name);
    const int selected = tracks.empty()
        ? -1 : juce::jlimit(0, static_cast<int>(tracks.size()) - 1,
                           selectedTrackIndex);
    vst3Browser->setTracks(names, selected);
    vst3Browser->setEffectSlots(
        juce::isPositiveAndBelow(selected, static_cast<int>(tracks.size()))
            ? tracks[static_cast<size_t>(selected)].effectSlots
            : std::vector<EffectSlotData>{});
    vst3Browser->refreshCatalogView();
}

void MainComponent::showVst3Browser(int selectedTrackIndex)
{
    if (vst3Window != nullptr)
    {
        refreshVst3Browser(selectedTrackIndex);
        vst3Window->toFront(true);
        return;
    }

    auto* content = new Vst3BrowserComponent(vst3Catalog);
    content->setSize(850, 620);
    vst3Browser = content;
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    content->onSelectedTrackChanged = [safeThis](int trackIndex)
    {
        if (safeThis != nullptr) safeThis->refreshVst3Browser(trackIndex);
    };
    content->onRequestCatalogRebuild = [safeThis]()
    {
        if (safeThis != nullptr) safeThis->rebuildVst3Catalog();
    };
    content->onAddPlugin = [safeThis](int trackIndex, const juce::File& file)
    {
        if (safeThis == nullptr) return;
        if (safeThis->pendingVst3ScanFile != juce::File())
        {
            safeThis->showStatus(
                juce::String::fromUTF8(
                    u8"前のVST3検査が終わるまでお待ちください。"),
                true);
            return;
        }
        if (!safeThis->vst3ScanMarkerFile.replaceWithText(
                file.getFullPathName()))
        {
            safeThis->showStatus(
                juce::String::fromUTF8(
                    u8"VST3の安全確認情報を保存できないため、追加を中止しました。"),
                true);
            return;
        }
        safeThis->pendingVst3ScanFile = file;
        safeThis->vst3Catalog.clearQuarantine(file);
        safeThis->vst3Catalog.saveState(safeThis->vst3CatalogStateFile);
        if (safeThis->vst3Browser != nullptr)
            safeThis->vst3Browser->setStatusMessage(
                juce::String::fromUTF8(u8"VST3を安全に確認しています…"));
        safeThis->audioEngine.addEffectPluginForTrack(trackIndex, file);
    };
    content->onRemoveSlot = [safeThis](int trackIndex, int slotIndex)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.removeEffectSlot(trackIndex, slotIndex);
    };
    content->onMoveSlot = [safeThis](int trackIndex, int fromIndex, int toIndex)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.moveEffectSlot(trackIndex, fromIndex, toIndex);
    };
    content->onSetSlotBypassed =
        [safeThis](int trackIndex, int slotIndex, bool bypassed)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.setEffectSlotBypassed(trackIndex, slotIndex,
                                                        bypassed);
    };
    content->onOpenSlotEditor = [safeThis](int trackIndex, int slotIndex)
    {
        if (safeThis != nullptr)
            safeThis->audioEngine.showEffectSlotEditor(trackIndex, slotIndex);
    };
    content->onFavouriteChanged =
        [safeThis](const juce::File& file, bool favourite)
    {
        if (safeThis == nullptr) return;
        safeThis->vst3Catalog.setFavourite(file, favourite);
        safeThis->vst3Catalog.saveState(safeThis->vst3CatalogStateFile);
        if (safeThis->vst3Browser != nullptr)
            safeThis->vst3Browser->refreshCatalogView();
    };
    content->onClose = [safeThis]()
    {
        if (safeThis != nullptr && safeThis->vst3Window != nullptr)
            safeThis->vst3Window->exitModalState(0);
    };

    refreshVst3Browser(selectedTrackIndex);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.dialogTitle = juce::CharPointer_UTF8(u8"VST3エフェクト");
    options.dialogBackgroundColour = UiTheme::panelBackground;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = true;
    vst3Window = options.launchAsync();
    if (vst3Window != nullptr)
        vst3Window->setResizeLimits(760, 520, 1600, 1000);
    if (vst3Catalog.getEntries().empty())
        rebuildVst3Catalog();
}

void MainComponent::rebuildVst3Catalog()
{
    if (vst3Browser != nullptr)
        vst3Browser->setStatusMessage(
            juce::CharPointer_UTF8(u8"VST3の保存場所を確認しています…"));
    const auto stateFile = vst3CatalogStateFile;
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    studioWorkThread.addJob([safeThis, stateFile]()
    {
        auto rebuilt = std::make_shared<Vst3PluginCatalog>();
        rebuilt->loadState(stateFile);
        rebuilt->rebuild();
        juce::MessageManager::callAsync([safeThis, rebuilt]()
        {
            if (safeThis == nullptr) return;
            safeThis->vst3Catalog = *rebuilt;
            safeThis->vst3Catalog.saveState(safeThis->vst3CatalogStateFile);
            if (safeThis->vst3Browser != nullptr)
            {
                safeThis->vst3Browser->refreshCatalogView();
                safeThis->vst3Browser->setStatusMessage(
                    juce::String(rebuilt->getEntries().size())
                        + juce::String::fromUTF8(u8"個のVST3を見つけました"));
            }
        });
    });
}

void MainComponent::estimateTrackBpm(int trackIndex)
{
    if (bpmCandidateWindow != nullptr)
    {
        bpmCandidateWindow->toFront(true);
        return;
    }
    if (bpmAnalysisInProgress)
    {
        showStatus(juce::String::fromUTF8(u8"BPM候補を解析中です…"));
        return;
    }

    const auto tracks = audioEngine.getTracksSnapshot();
    if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(tracks.size()))
        || tracks[static_cast<size_t>(trackIndex)].clips.empty())
    {
        showStatus(juce::CharPointer_UTF8(u8"BPMを調べる音声がありません"), true);
        return;
    }
    const auto& clips = tracks[static_cast<size_t>(trackIndex)].clips;
    const auto longest = std::max_element(
        clips.begin(), clips.end(),
        [](const AudioClip& left, const AudioClip& right)
        { return left.duration < right.duration; });
    if (longest == clips.end() || longest->buffer == nullptr)
        return;
    const auto buffer = longest->buffer;
    const double sampleRate = std::max(1, longest->sampleRate);
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    bpmAnalysisInProgress = true;
    showStatus(juce::CharPointer_UTF8(u8"伴奏からBPM候補を解析しています…"));
    studioWorkThread.addJob([safeThis, buffer, sampleRate]()
    {
        const auto candidates = TempoEstimator::estimate(*buffer, sampleRate, 3);
        juce::MessageManager::callAsync([safeThis, candidates]()
        {
            if (safeThis == nullptr) return;
            safeThis->bpmAnalysisInProgress = false;
            if (candidates.empty())
            {
                safeThis->showStatus(
                    juce::CharPointer_UTF8(u8"安定したBPM候補を見つけられませんでした"),
                    true);
                return;
            }
            const auto targetTime = safeThis->audioEngine.getCurrentTime();
            auto& metronome = safeThis->audioEngine.getMetronome();
            const int originalBpm = metronome.getBpm();
            const int originalNumerator = metronome.getNumerator();
            const int originalDenominator = metronome.getDenominator();
            const bool originalEnabled = metronome.getEnabled();
            auto playbackStartedByDialog = std::make_shared<bool>(false);
            auto originalProject = std::make_shared<ProjectDawSettings>(
                safeThis->audioEngine.getProjectDawSettings());

            auto* content = new BpmCandidateDialogComponent(candidates);
            content->onPreview = [safeThis, playbackStartedByDialog,
                                  originalProject, targetTime](double bpm)
            {
                if (safeThis == nullptr) return;
                auto previewProject = *originalProject;
                const TempoMap originalTempo(previewProject.tempoMap);
                const auto meter = originalTempo.getPointAt(targetTime);
                previewProject.tempoMap.erase(
                    std::remove_if(previewProject.tempoMap.begin(),
                                   previewProject.tempoMap.end(),
                        [targetTime](const TempoPoint& point)
                        {
                            return std::abs(point.timeSeconds - targetTime)
                                <= 0.001;
                        }),
                    previewProject.tempoMap.end());
                previewProject.tempoMap.push_back(
                    { targetTime, bpm, meter.numerator, meter.denominator });
                safeThis->audioEngine.setProjectDawSettings(previewProject,
                                                            false);
                auto& previewMetronome = safeThis->audioEngine.getMetronome();
                previewMetronome.setBpm(juce::roundToInt(bpm));
                previewMetronome.setTimeSignature(meter.numerator,
                                                  meter.denominator);
                previewMetronome.setEnabled(true);
                if (safeThis->audioEngine.getPlaybackState()
                    == PlaybackState::Stopped)
                {
                    safeThis->audioEngine.setCurrentTime(targetTime);
                    safeThis->audioEngine.play();
                    *playbackStartedByDialog =
                        safeThis->audioEngine.getPlaybackState()
                            != PlaybackState::Stopped;
                }
                safeThis->showStatus(
                    juce::String(bpm, 1)
                        + juce::String::fromUTF8(
                            u8" BPMで試聴中です。伴奏とクリックを聴き比べてください。"));
            };
            content->onAccept = [safeThis, playbackStartedByDialog,
                                 originalProject, targetTime,
                                 originalEnabled](double bpm)
            {
                if (safeThis == nullptr) return;
                if (*playbackStartedByDialog)
                {
                    safeThis->audioEngine.stop();
                    safeThis->audioEngine.setCurrentTime(targetTime);
                }
                const TempoMap originalTempo(originalProject->tempoMap);
                const auto meter = originalTempo.getPointAt(targetTime);
                safeThis->audioEngine.setProjectDawSettings(*originalProject,
                                                            false);
                safeThis->audioEngine.setTempoPoint(targetTime, bpm,
                                                    meter.numerator,
                                                    meter.denominator);
                safeThis->audioEngine.getMetronome().setEnabled(originalEnabled);
                safeThis->showStatus(
                    juce::String(bpm, 1)
                        + juce::String::fromUTF8(
                            u8" BPMをテンポマップへ追加しました"));
                if (safeThis->bpmCandidateWindow != nullptr)
                    safeThis->bpmCandidateWindow->exitModalState(0);
            };
            content->onClose = [safeThis, playbackStartedByDialog,
                                originalProject, targetTime, originalBpm,
                                originalNumerator, originalDenominator,
                                originalEnabled]()
            {
                if (safeThis == nullptr) return;
                if (*playbackStartedByDialog)
                {
                    safeThis->audioEngine.stop();
                    safeThis->audioEngine.setCurrentTime(targetTime);
                }
                safeThis->audioEngine.setProjectDawSettings(*originalProject,
                                                            false);
                auto& restoredMetronome = safeThis->audioEngine.getMetronome();
                restoredMetronome.setBpm(originalBpm);
                restoredMetronome.setTimeSignature(originalNumerator,
                                                   originalDenominator);
                restoredMetronome.setEnabled(originalEnabled);
                if (safeThis->bpmCandidateWindow != nullptr)
                    safeThis->bpmCandidateWindow->exitModalState(0);
            };

            juce::DialogWindow::LaunchOptions options;
            options.content.setOwned(content);
            options.componentToCentreAround = safeThis.getComponent();
            options.dialogTitle = juce::String::fromUTF8(u8"BPM候補");
            options.dialogBackgroundColour = UiTheme::panelBackground;
            options.escapeKeyTriggersCloseButton = false;
            options.useNativeTitleBar = false;
            options.resizable = false;
            safeThis->bpmCandidateWindow = options.launchAsync();
            if (safeThis->bpmCandidateWindow != nullptr)
                safeThis->bpmCandidateWindow->setTitleBarButtonsRequired(0,
                                                                         false);
        });
    });
}

void MainComponent::runAutoMix()
{
    if (autoMixAnalysisInProgress)
    {
        if (studioPanel != nullptr)
            studioPanel->setAutoMixStatus(
                juce::String::fromUTF8(u8"全トラックを解析しています…"), true);
        return;
    }

    const auto snapshot = audioEngine.getTracksSnapshot();
    const bool hasAudio = std::any_of(
        snapshot.begin(), snapshot.end(), [](const TrackData& track)
        {
            return std::any_of(track.clips.begin(), track.clips.end(),
                [](const AudioClip& clip)
                {
                    return clip.buffer != nullptr
                        && clip.buffer->getNumSamples() > 0
                        && clip.sampleRate > 0;
                });
        });
    if (!hasAudio)
    {
        const auto message = juce::String::fromUTF8(
            u8"お任せMIXを行う音声がありません");
        if (studioPanel != nullptr)
            studioPanel->setAutoMixStatus(message, false);
        showStatus(message, true);
        return;
    }

    const auto expectedRevision = audioEngine.getContentRevision();
    autoMixAnalysisInProgress = true;
    if (studioPanel != nullptr)
        studioPanel->setAutoMixStatus(
            juce::String::fromUTF8(
                u8"役割と音の特徴、伴奏との重なりを解析しています…"), true);

    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    studioWorkThread.addJob(
        [safeThis, snapshot, expectedRevision]() mutable
        {
            auto result = AutoMixAnalyzer::analyseAndSuggest(snapshot);
            juce::MessageManager::callAsync(
                [safeThis, result = std::move(result), expectedRevision]() mutable
                {
                    if (safeThis == nullptr)
                        return;
                    safeThis->autoMixAnalysisInProgress = false;

                    if (safeThis->audioEngine.getContentRevision()
                        != expectedRevision)
                    {
                        const auto message = juce::String::fromUTF8(
                            u8"解析中にトラックが変更されたため、古いMIX結果は反映しませんでした");
                        if (safeThis->studioPanel != nullptr)
                            safeThis->studioPanel->setAutoMixStatus(message, false);
                        safeThis->showStatus(message, true);
                        return;
                    }

                    if (!result.success || result.tracks.empty())
                    {
                        const auto message = result.message.isNotEmpty()
                            ? result.message
                            : juce::String::fromUTF8(
                                u8"解析できるトラックが見つかりませんでした");
                        if (safeThis->studioPanel != nullptr)
                            safeThis->studioPanel->setAutoMixStatus(message, false);
                        safeThis->showStatus(message, true);
                        return;
                    }

                    const auto current = safeThis->audioEngine.getTracksSnapshot();
                    std::vector<std::pair<int, const AutoMixTrackSuggestion*>>
                        applicable;
                    applicable.reserve(result.tracks.size());
                    for (const auto& suggestion : result.tracks)
                    {
                        if (!suggestion.analysisValid)
                            continue;
                        const auto match = std::find_if(
                            current.begin(), current.end(),
                            [&suggestion](const TrackData& track)
                            {
                                return track.id == suggestion.trackId;
                            });
                        if (match != current.end())
                            applicable.emplace_back(
                                static_cast<int>(std::distance(current.begin(), match)),
                                &suggestion);
                    }

                    if (applicable.empty())
                    {
                        const auto message = juce::String::fromUTF8(
                            u8"音の特徴を判定できるトラックがありませんでした");
                        if (safeThis->studioPanel != nullptr)
                            safeThis->studioPanel->setAutoMixStatus(message, false);
                        safeThis->showStatus(message, true);
                        return;
                    }

                    int unlabelled = 0;
                    safeThis->audioEngine.beginEdit();
                    for (const auto& [trackIndex, suggestion] : applicable)
                    {
                        safeThis->audioEngine.setTrackVolume(
                            trackIndex, suggestion->volume);
                        safeThis->audioEngine.setTrackPan(
                            trackIndex, suggestion->pan);
                        safeThis->audioEngine.setTrackSimpleMixSettings(
                            trackIndex, suggestion->simpleMix);
                        safeThis->audioEngine.setTrackNoiseReductionSettings(
                            trackIndex, suggestion->noiseReduction);
                        if (current[static_cast<size_t>(trackIndex)].role
                            == TrackRole::unknown)
                            ++unlabelled;
                    }
                    safeThis->audioEngine.endEdit();

                    auto message = juce::String(applicable.size())
                        + juce::String::fromUTF8(
                            u8"トラックを役割と音の特徴に合わせて整えました");
                    if (unlabelled > 0)
                        message += juce::String::fromUTF8(u8"（未設定 ")
                            + juce::String(unlabelled)
                            + juce::String::fromUTF8(
                                u8"件は安全な標準設定で処理）");
                    if (safeThis->studioPanel != nullptr)
                    {
                        safeThis->refreshStudioPanel(
                            safeThis->studioPanel->getState().selectedTrackIndex);
                        safeThis->studioPanel->setAutoMixStatus(message, false);
                    }
                    safeThis->showStatus(message);
                });
        });
}

void MainComponent::chooseAndRunReferenceMix(int selectedTrackIndex)
{
    if (referenceMixAnalysisInProgress || referenceMixFileChooser != nullptr)
    {
        if (studioPanel != nullptr)
            studioPanel->setReferenceMixStatus(
                juce::String::fromUTF8(u8"参考曲を解析しています…"), true);
        return;
    }

    referenceMixFileChooser = std::make_unique<juce::FileChooser>(
        juce::String::fromUTF8(u8"雰囲気を参考にする曲を選択"),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.wav;*.wave;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    referenceMixFileChooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, selectedTrackIndex](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr) return;
            const auto referenceFile = chooser.getResult();
            safeThis->referenceMixFileChooser.reset();
            if (!referenceFile.existsAsFile())
            {
                if (safeThis->studioPanel != nullptr)
                    safeThis->studioPanel->setReferenceMixStatus(
                        juce::String::fromUTF8(u8"参考曲の選択をキャンセルしました"),
                        false);
                return;
            }

            const auto snapshot = safeThis->audioEngine.getTracksSnapshot();
            if (snapshot.empty())
            {
                if (safeThis->studioPanel != nullptr)
                    safeThis->studioPanel->setReferenceMixStatus(
                        juce::String::fromUTF8(u8"MIXするトラックがありません"),
                        false);
                return;
            }
            const auto expectedRevision = safeThis->audioEngine.getContentRevision();
            safeThis->referenceMixAnalysisInProgress = true;
            if (safeThis->studioPanel != nullptr)
                safeThis->studioPanel->setReferenceMixStatus(
                    juce::String::fromUTF8(u8"参考曲の音色・広がり・音量差を解析しています…"),
                    true);

            safeThis->studioWorkThread.addJob(
                [safeThis, referenceFile, snapshot, expectedRevision,
                 selectedTrackIndex]() mutable
                {
                    ReferenceMixResult result;
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    auto reader = std::unique_ptr<juce::AudioFormatReader>(
                        formats.createReaderFor(referenceFile));
                    if (reader == nullptr || reader->sampleRate < 1000.0
                        || reader->numChannels <= 0
                        || reader->lengthInSamples <= 0)
                    {
                        result.message = juce::String::fromUTF8(
                            u8"参考曲を音声として読み込めませんでした");
                    }
                    else
                    {
                        constexpr double maximumReferenceSeconds = 300.0;
                        const auto sampleCount64 = std::min<juce::int64>(
                            reader->lengthInSamples,
                            static_cast<juce::int64>(std::llround(
                                reader->sampleRate * maximumReferenceSeconds)));
                        const int sampleCount = static_cast<int>(std::min<juce::int64>(
                            sampleCount64, std::numeric_limits<int>::max()));
                        juce::AudioBuffer<float> referenceBuffer(
                            std::min(2, static_cast<int>(reader->numChannels)),
                            sampleCount);
                        referenceBuffer.clear();
                        if (!reader->read(&referenceBuffer, 0, sampleCount, 0,
                                          true, true))
                        {
                            result.message = juce::String::fromUTF8(
                                u8"参考曲の音声を読み取れませんでした");
                        }
                        else
                        {
                            result = ReferenceMixAnalyzer::analyseAndSuggest(
                                referenceBuffer, reader->sampleRate, snapshot);
                        }
                    }

                    juce::MessageManager::callAsync(
                        [safeThis, result = std::move(result), expectedRevision,
                         selectedTrackIndex]() mutable
                        {
                            if (safeThis == nullptr) return;
                            safeThis->referenceMixAnalysisInProgress = false;
                            if (safeThis->audioEngine.getContentRevision()
                                != expectedRevision)
                            {
                                const auto message = juce::String::fromUTF8(
                                    u8"解析中に曲が変更されたため、参考曲MIXは反映しませんでした");
                                if (safeThis->studioPanel != nullptr)
                                    safeThis->studioPanel->setReferenceMixStatus(
                                        message, false);
                                safeThis->showStatus(message, true);
                                return;
                            }
                            if (!result.success)
                            {
                                if (safeThis->studioPanel != nullptr)
                                    safeThis->studioPanel->setReferenceMixStatus(
                                        result.message, false);
                                safeThis->showStatus(result.message, true);
                                return;
                            }

                            const auto tracks = safeThis->audioEngine.getTracksSnapshot();
                            safeThis->audioEngine.beginEdit();
                            int appliedTracks = 0;
                            for (const auto& suggestion : result.tracks)
                            {
                                if (!suggestion.analysisValid)
                                    continue;
                                const auto found = std::find_if(
                                    tracks.begin(), tracks.end(),
                                    [&suggestion](const TrackData& track)
                                    {
                                        return track.id == suggestion.trackId;
                                    });
                                if (found == tracks.end())
                                    continue;
                                const int trackIndex = static_cast<int>(
                                    std::distance(tracks.begin(), found));
                                safeThis->audioEngine.setTrackSimpleMixSettings(
                                    trackIndex, suggestion.simpleMix);
                                safeThis->audioEngine.setTrackNoiseReductionSettings(
                                    trackIndex, suggestion.noiseReduction);
                                safeThis->audioEngine.setTrackVolume(
                                    trackIndex, suggestion.volume);
                                safeThis->audioEngine.setTrackPan(
                                    trackIndex, suggestion.pan);
                                ++appliedTracks;
                            }

                            bool appliedSpecialFx = false;
                            const auto project =
                                safeThis->audioEngine.getProjectDawSettings();
                            if (!result.specialFx.empty()
                                && juce::isPositiveAndBelow(
                                    selectedTrackIndex,
                                    static_cast<int>(tracks.size()))
                                && project.hasLoopStart && project.hasLoopEnd
                                && project.loopEndSeconds
                                    > project.loopStartSeconds + 1.0e-6)
                            {
                                const auto& fx = result.specialFx.front();
                                appliedSpecialFx =
                                    safeThis->audioEngine.addTrackSpecialFxRegion(
                                        selectedTrackIndex, fx.type,
                                        project.loopStartSeconds,
                                        project.loopEndSeconds,
                                        fx.amount, 0.85f, 0.03);
                            }
                            safeThis->audioEngine.endEdit();

                            auto message = juce::String(appliedTracks)
                                + juce::String::fromUTF8(
                                    u8"トラックを参考曲の雰囲気へ調整しました");
                            if (!result.specialFx.empty())
                            {
                                message += appliedSpecialFx
                                    ? juce::String::fromUTF8(
                                        u8"。A-B範囲には質感FXも追加しました")
                                    : juce::String::fromUTF8(
                                        u8"。質感FX候補はA-B設定後に適用できます");
                            }
                            if (safeThis->studioPanel != nullptr)
                            {
                                safeThis->refreshStudioPanel(
                                    safeThis->trackArea.getSelectedTrackIndex());
                                safeThis->studioPanel->setReferenceMixStatus(
                                    message, false);
                            }
                            safeThis->showStatus(message, appliedTracks <= 0);
                        });
                });
        });
}

//==============================================================================
// プロジェクト管理・復元
//==============================================================================

void MainComponent::initialiseAppSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "SimpleRecPro";
    options.filenameSuffix = "settings";
    options.folderName = "SimpleRecPro";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = 1000;
    appSettings = std::make_unique<juce::PropertiesFile>(options);

    manualRecordingOffsetMs = appSettings->getDoubleValue("manualRecordingOffsetMs", 0.0);

    recentProjectPaths = juce::StringArray::fromTokens(
        appSettings->getValue("recentProjects"), "|", "");
    for (int i = recentProjectPaths.size(); --i >= 0;)
    {
        if (!juce::File(recentProjectPaths[i]).existsAsFile())
            recentProjectPaths.remove(i);
    }

    const auto recoveryDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                       .getChildFile("SimpleRecPro");
    recoveryDirectory.createDirectory();
    recoveryFile = recoveryDirectory.getChildFile("AutoRecovery.srec");
    vst3CatalogStateFile = recoveryDirectory.getChildFile("vst3-catalog.json");
    vst3ScanMarkerFile = recoveryDirectory.getChildFile(
        "vst3-scan-in-progress.txt");
    vst3Catalog.loadState(vst3CatalogStateFile);
    if (vst3ScanMarkerFile.existsAsFile())
    {
        const auto interruptedPath = vst3ScanMarkerFile.loadFileAsString().trim();
        if (interruptedPath.isNotEmpty())
        {
            const juce::File interruptedPlugin(interruptedPath);
            vst3Catalog.quarantine(
                interruptedPlugin,
                juce::String::fromUTF8(
                    u8"前回、このVST3を確認中にアプリが終了したため隔離しました。"));
            vst3Catalog.saveState(vst3CatalogStateFile);
        }
        vst3ScanMarkerFile.deleteFile();
    }
}

void MainComponent::configureProjectActions()
{
    header.onOpenClicked = [this]() { requestOpenProject(); };
    header.onSaveClicked = [this]() { saveProject(false); };
    header.onSaveAsClicked = [this]() { saveProject(true); };
    header.onRecentProjectSelected = [this](const juce::File& file) { requestOpenRecentProject(file); };
    header.setRecentProjects(recentProjectPaths);
}

void MainComponent::requestOpenProject()
{
    if (projectIoInProgress.load())
    {
        showStatus(juce::CharPointer_UTF8(u8"プロジェクトの処理が完了するまでお待ちください"));
        return;
    }

    if (audioEngine.getPlaybackState() != PlaybackState::Stopped)
        audioEngine.stop();

    confirmBeforeDestructiveAction(juce::CharPointer_UTF8(u8"別のプロジェクトを開く"),
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]() {
            if (safeThis == nullptr) return;

            const auto initial = safeThis->currentProjectFile.existsAsFile()
                                   ? safeThis->currentProjectFile.getParentDirectory()
                                   : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
            safeThis->projectFileChooser = std::make_unique<juce::FileChooser>(
                juce::CharPointer_UTF8(u8"プロジェクトを開く..."), initial, "*.srec");
            safeThis->projectFileChooser->launchAsync(
                juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safeThis](const juce::FileChooser& chooser) {
                    if (safeThis != nullptr && chooser.getResult().existsAsFile())
                        safeThis->loadProjectFile(chooser.getResult());
                });
        });
}

void MainComponent::requestOpenRecentProject(const juce::File& file)
{
    if (projectIoInProgress.load())
    {
        showStatus(juce::CharPointer_UTF8(u8"プロジェクトの処理が完了するまでお待ちください"));
        return;
    }

    if (!file.existsAsFile())
    {
        recentProjectPaths.removeString(file.getFullPathName());
        header.setRecentProjects(recentProjectPaths);
        showStatus(juce::CharPointer_UTF8(u8"プロジェクトファイルが見つかりません"), true);
        return;
    }

    if (audioEngine.getPlaybackState() != PlaybackState::Stopped)
        audioEngine.stop();

    confirmBeforeDestructiveAction(juce::CharPointer_UTF8(u8"最近使ったプロジェクトを開く"),
        [safeThis = juce::Component::SafePointer<MainComponent>(this), file]() {
            if (safeThis != nullptr)
                safeThis->loadProjectFile(file);
        });
}

void MainComponent::saveProject(bool forceSaveAs, std::function<void(bool)> completion)
{
    if (projectIoInProgress.load())
    {
        showStatus(juce::CharPointer_UTF8(u8"プロジェクトを処理中です"));
        if (completion) completion(false);
        return;
    }

    if (audioEngine.getPlaybackState() != PlaybackState::Stopped)
        audioEngine.stop();

    if (!forceSaveAs && currentProjectFile.getFullPathName().isNotEmpty())
    {
        saveProjectToFile(currentProjectFile, std::move(completion));
        return;
    }

    const auto initial = currentProjectFile.getFullPathName().isNotEmpty()
                           ? currentProjectFile
                           : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                 .getChildFile(projectDisplayName + ".srec");
    projectFileChooser = std::make_unique<juce::FileChooser>(
        juce::CharPointer_UTF8(u8"プロジェクトを保存..."), initial, "*.srec");
    projectFileChooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis = juce::Component::SafePointer<MainComponent>(this), completion]
        (const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
            {
                if (completion) completion(false);
                return;
            }

            auto file = chooser.getResult();
            if (file.getFileName().isEmpty())
            {
                if (completion) completion(false);
                return;
            }
            if (!file.hasFileExtension(".srec"))
                file = file.withFileExtension("srec");

            safeThis->saveProjectToFile(file, completion);
        });
}

void MainComponent::saveProjectToFile(const juce::File& file,
                                      std::function<void(bool)> completion)
{
    if (projectIoInProgress.exchange(true))
    {
        showStatus(juce::CharPointer_UTF8(u8"プロジェクトを処理中です"));
        if (completion) completion(false);
        return;
    }

    // Use the revision from before plug-in state capture. A plug-in may report
    // another edit while getStateInformation() is running; that newer edit is
    // deliberately left dirty so the user is asked to save it again.
    const auto savedRevision = audioEngine.getContentRevision();
    if (!audioEngine.capturePluginStates())
    {
        projectIoInProgress.store(false);
        showStatus(juce::CharPointer_UTF8(
            u8"プラグインの編集内容を保存できませんでした。もう一度お試しください。"),
            true);
        if (completion) completion(false);
        return;
    }

    auto snapshot = std::make_shared<ProjectSerializer::ProjectSnapshot>(
        ProjectSerializer::captureProjectSnapshot(audioEngine));
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    showStatus(juce::CharPointer_UTF8(u8"プロジェクトを保存しています…"));

    projectIoThread.addJob([safeThis, file, snapshot, savedRevision, completion]() mutable
    {
        bool saved = false;
        try
        {
            saved = ProjectSerializer::saveProject(file, *snapshot);
        }
        catch (...)
        {
            saved = false;
        }

        juce::MessageManager::callAsync(
            [safeThis, file, savedRevision, saved, completion]() mutable
            {
                if (safeThis == nullptr)
                    return;

                safeThis->projectIoInProgress.store(false);
                if (!saved)
                {
                    safeThis->showStatus(
                        juce::CharPointer_UTF8(u8"プロジェクトを保存できませんでした"), true);
                    if (completion) completion(false);
                    return;
                }

                safeThis->currentProjectFile = file;
                safeThis->projectDisplayName = file.getFileNameWithoutExtension();
                const bool unchanged = safeThis->audioEngine.getContentRevision()
                                       == savedRevision;
                if (unchanged)
                {
                    safeThis->audioEngine.markClean();
                    safeThis->clearRecoveryFile();
                }
                safeThis->rememberRecentProject(file);
                safeThis->refreshProjectDisplay();
                safeThis->showStatus(
                    unchanged
                        ? juce::String(juce::CharPointer_UTF8(u8"プロジェクトを保存しました"))
                        : juce::String(juce::CharPointer_UTF8(
                            u8"保存中に変更があったため、もう一度保存してください")),
                    !unchanged);
                if (completion) completion(unchanged);
            });
    });
}

void MainComponent::loadProjectFile(const juce::File& file, bool fromRecovery)
{
    if (projectIoInProgress.exchange(true))
    {
        showStatus(juce::CharPointer_UTF8(u8"プロジェクトを処理中です"));
        return;
    }

    if (audioEngine.getPlaybackState() != PlaybackState::Stopped)
        audioEngine.stop();

    auto loadedProject = std::make_shared<ProjectSerializer::LoadedProjectData>();
    auto loadError = std::make_shared<juce::String>();
    const double targetSampleRate = audioEngine.getSampleRate();
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    setEnabled(false);
    showStatus(juce::CharPointer_UTF8(u8"プロジェクトを読み込んでいます…"));

    projectIoThread.addJob(
        [safeThis, file, fromRecovery, targetSampleRate, loadedProject,
         loadError]() mutable
        {
            bool loaded = false;
            try
            {
                loaded = ProjectSerializer::loadProject(file, *loadedProject,
                                                        loadError.get());
                if (loaded)
                    AudioEngine::prepareTracksForSampleRate(loadedProject->tracks,
                                                            targetSampleRate);
            }
            catch (...)
            {
                loaded = false;
                if (loadError->isEmpty())
                    *loadError = juce::String::fromUTF8(
                        u8"プロジェクトの読み込み中に予期しないエラーが発生しました。");
            }

            juce::MessageManager::callAsync(
                [safeThis, file, fromRecovery, loadedProject, loadError,
                 loaded]() mutable
                {
                    if (safeThis == nullptr)
                        return;

                    if (!loaded)
                    {
                        safeThis->projectIoInProgress.store(false);
                        safeThis->setEnabled(true);
                        safeThis->showStatus(
                            loadError->isNotEmpty()
                                ? *loadError
                                : juce::String(juce::CharPointer_UTF8(
                                    u8"プロジェクトを開けませんでした。ファイルを確認してください")),
                            true);
                        return;
                    }

                    ProjectSerializer::applyLoadedProject(std::move(*loadedProject),
                                                          safeThis->audioEngine);
                    if (fromRecovery)
                    {
                        safeThis->currentProjectFile = {};
                        safeThis->projectDisplayName = juce::CharPointer_UTF8(
                            u8"復元プロジェクト");
                        safeThis->audioEngine.markDirty();
                        safeThis->showStatus(juce::CharPointer_UTF8(
                            u8"自動保存からプロジェクトを復元しました"));
                    }
                    else
                    {
                        safeThis->currentProjectFile = file;
                        safeThis->projectDisplayName = file.getFileNameWithoutExtension();
                        safeThis->audioEngine.markClean();
                        safeThis->rememberRecentProject(file);
                        safeThis->clearRecoveryFile();
                        safeThis->showStatus(juce::CharPointer_UTF8(
                            u8"プロジェクトを開きました"));
                    }

                    safeThis->transportBar.syncFromEngine();
                    safeThis->header.syncFromEngine();
                    safeThis->trackArea.syncFromEngine();
                    safeThis->trackArea.updateScrollbar();
                    safeThis->trackArea.repaint();
                    safeThis->refreshProjectDisplay();
                    safeThis->projectIoInProgress.store(false);
                    safeThis->setEnabled(true);
                });
        });
}

void MainComponent::confirmBeforeDestructiveAction(const juce::String& actionText,
                                                   std::function<void()> continuation)
{
    if (!audioEngine.isDirty())
    {
        continuation();
        return;
    }

    const auto options = juce::MessageBoxOptions::makeOptionsYesNoCancel(
        juce::MessageBoxIconType::QuestionIcon,
        juce::CharPointer_UTF8(u8"未保存の変更があります"),
        actionText + juce::String(juce::CharPointer_UTF8(u8"前に、現在の変更を保存しますか？")),
        juce::CharPointer_UTF8(u8"保存"),
        juce::CharPointer_UTF8(u8"保存しない"),
        juce::CharPointer_UTF8(u8"キャンセル"));

    activeMessageBox = juce::AlertWindow::showScopedAsync(
        options,
        [safeThis = juce::Component::SafePointer<MainComponent>(this),
         continuation = std::move(continuation)](int result) mutable {
            if (safeThis == nullptr) return;
            if (result == 1)
            {
                safeThis->saveProject(false, [continuation = std::move(continuation)](bool ok) mutable {
                    if (ok && continuation) continuation();
                });
            }
            else if (result == 2 && continuation)
            {
                continuation();
            }
        });
}

void MainComponent::rememberRecentProject(const juce::File& file)
{
    const auto path = file.getFullPathName();
    recentProjectPaths.removeString(path);
    recentProjectPaths.insert(0, path);
    while (recentProjectPaths.size() > 8)
        recentProjectPaths.remove(recentProjectPaths.size() - 1);

    if (appSettings != nullptr)
    {
        appSettings->setValue("recentProjects", recentProjectPaths.joinIntoString("|"));
        appSettings->saveIfNeeded();
    }
    header.setRecentProjects(recentProjectPaths);
}

void MainComponent::refreshProjectDisplay()
{
    const bool dirty = audioEngine.isDirty();
    header.setProjectDisplayName(projectDisplayName, dirty);
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName("SimpleRec Pro - " + projectDisplayName + (dirty ? " *" : ""));
}

void MainComponent::maybeSaveRecovery()
{
    if (recoveryDecisionDeferred
        || !audioEngine.isDirty()
        || audioEngine.getPlaybackState() != PlaybackState::Stopped
        || projectIoInProgress.load()
        || recoverySaveInProgress.load())
        return;

    const auto contentRevision = audioEngine.getContentRevision();
    if (lastRecoveryContentRevision.load() == contentRevision)
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    // Large multitrack projects should not continuously saturate the disk.
    // One minute still limits crash loss while avoiding a full-project write
    // every few seconds during active plug-in editing.
    if (now - static_cast<double>(lastRecoverySaveMs) < 60000.0)
        return;
    lastRecoverySaveMs = static_cast<juce::uint64>(now);

    if (recoverySaveInProgress.exchange(true))
        return;

    // VST3の現在のつまみ位置を、バックグラウンド保存を始める前に安全に取得する。
    // 取得中に新しい変更通知が来た場合は、この古いrevisionで復旧データを記録し、
    // 次回の自動保存でもう一度取り込めるようにする。
    const auto snapshotRevision = audioEngine.getContentRevision();
    if (!audioEngine.capturePluginStates())
    {
        recoverySaveInProgress.store(false);
        return;
    }

    auto snapshot = std::make_shared<ProjectSerializer::ProjectSnapshot>(
        ProjectSerializer::captureProjectSnapshot(audioEngine));

    const auto generation = recoveryGeneration.load();
    const auto targetFile = recoveryFile;
    const auto pendingFile = targetFile.getSiblingFile(
        "AutoRecovery-" + juce::Uuid().toString() + ".pending.srec");
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);

    recoverySaveThread.addJob([this, safeThis, generation, snapshotRevision,
                               targetFile, pendingFile, snapshot]() mutable
    {
        bool saved = false;
        try
        {
            saved = ProjectSerializer::saveProject(pendingFile, *snapshot);
        }
        catch (...)
        {
            saved = false;
        }

        bool published = false;
        {
            const juce::ScopedLock lock(recoveryFileLock);
            if (saved && recoveryGeneration.load() == generation)
                published = pendingFile.replaceFileIn(targetFile);
        }

        if (published)
            lastRecoveryContentRevision.store(snapshotRevision);

        if (pendingFile.existsAsFile())
            pendingFile.deleteFile();

        recoverySaveInProgress.store(false);

        if ((!saved || !published) && recoveryGeneration.load() == generation)
        {
            juce::MessageManager::callAsync([safeThis]() mutable
            {
                if (safeThis != nullptr)
                    safeThis->showStatus(
                        juce::CharPointer_UTF8(u8"自動復旧データを保存できませんでした"), true);
            });
        }
    });
}

void MainComponent::offerRecoveryIfAvailable()
{
    if (recoveryPromptShown || !recoveryFile.existsAsFile()) return;
    recoveryPromptShown = true;

    const auto options = juce::MessageBoxOptions::makeOptionsYesNoCancel(
        juce::MessageBoxIconType::QuestionIcon,
        juce::CharPointer_UTF8(u8"未保存プロジェクトの復元"),
        juce::CharPointer_UTF8(u8"前回終了時の自動保存データが見つかりました。復元しますか？"),
        juce::CharPointer_UTF8(u8"復元"),
        juce::CharPointer_UTF8(u8"破棄"),
        juce::CharPointer_UTF8(u8"あとで"));
    activeMessageBox = juce::AlertWindow::showScopedAsync(
        options,
        [safeThis = juce::Component::SafePointer<MainComponent>(this)](int result) {
            if (safeThis == nullptr) return;
            if (result == 1)
            {
                safeThis->recoveryDecisionDeferred = false;
                safeThis->loadProjectFile(safeThis->recoveryFile, true);
            }
            else if (result == 2)
            {
                safeThis->recoveryDecisionDeferred = false;
                safeThis->clearRecoveryFile(true);
            }
            else
            {
                // Preserve the previous session's recovery verbatim. Automatic
                // recovery is paused for this run so "Later" cannot overwrite it.
                safeThis->recoveryDecisionDeferred = true;
                safeThis->recoveryGeneration.fetch_add(1);
                safeThis->showStatus(
                    juce::CharPointer_UTF8(u8"復旧データを保護しています。この起動中の自動復旧保存は一時停止します"));
            }
        });
}

void MainComponent::clearRecoveryFile(bool force)
{
    if (recoveryDecisionDeferred && !force)
        return;

    recoveryGeneration.fetch_add(1);
    const juce::ScopedLock lock(recoveryFileLock);

    if (recoveryFile.existsAsFile())
        recoveryFile.deleteFile();

    const auto temp = recoveryFile.getSiblingFile("AutoRecovery.tmp");
    if (temp.existsAsFile())
        temp.deleteFile();

    for (const auto& pending : recoveryFile.getParentDirectory().findChildFiles(
             juce::File::findFiles, false, "AutoRecovery-*.pending.srec"))
        pending.deleteFile();
}

void MainComponent::showStatus(const juce::String& message, bool isError)
{
    transportBar.showStatusMessage(message, isError);
}

void MainComponent::setManualRecordingOffsetMs(double milliseconds)
{
    manualRecordingOffsetMs = juce::jlimit(-200.0, 200.0, milliseconds);
    audioEngine.setManualRecordingOffsetSamples(
        juce::roundToInt(manualRecordingOffsetMs * audioEngine.getSampleRate() / 1000.0));
    if (appSettings != nullptr)
    {
        appSettings->setValue("manualRecordingOffsetMs", manualRecordingOffsetMs);
        appSettings->saveIfNeeded();
    }
}

void MainComponent::startLatencyCalibration()
{
    if (latencyCalibration == nullptr || isLatencyCalibrationRunning())
        return;

    if (latencyCalibration->activeAudioCallbacks.load(std::memory_order_acquire) != 0)
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        latencyCalibration->statusText = juce::CharPointer_UTF8(
            u8"キャンセル処理中です。少し待ってからもう一度開始してください");
        return;
    }

    if (audioEngine.getPlaybackState() != PlaybackState::Stopped)
    {
        const auto message = juce::String(juce::CharPointer_UTF8(
            u8"再生または録音を停止してから、自動測定を始めてください"));
        {
            const juce::ScopedLock lock(latencyCalibration->resultLock);
            latencyCalibration->statusText = message;
        }
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
        showStatus(message, true);
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    const int inputChannels = device != nullptr
        ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
    const int outputChannels = device != nullptr
        ? device->getActiveOutputChannels().countNumberOfSetBits() : 0;
    const double sampleRate = device != nullptr ? device->getCurrentSampleRate() : 0.0;
    if (device == nullptr || inputChannels <= 0 || outputChannels <= 0 || sampleRate < 8000.0)
    {
        const auto message = juce::String(juce::CharPointer_UTF8(
            u8"入力と出力の両方を有効にしてから、自動測定を始めてください"));
        {
            const juce::ScopedLock lock(latencyCalibration->resultLock);
            latencyCalibration->statusText = message;
        }
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
        showStatus(message, true);
        return;
    }

    const auto deviceSetup = deviceManager.getAudioDeviceSetup();
    if (deviceSetup.inputDeviceName.isNotEmpty()
        && deviceSetup.outputDeviceName.isNotEmpty()
        && deviceSetup.inputDeviceName != deviceSetup.outputDeviceName)
    {
        const auto message = juce::String(juce::CharPointer_UTF8(
            u8"入力と出力が別々の機器です。時間とともに広がるズレは固定補正では直せないため、"
            u8"同じAG03を入出力に選んでください"));
        {
            const juce::ScopedLock lock(latencyCalibration->resultLock);
            latencyCalibration->statusText = message;
        }
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
        showStatus(message, true);
        return;
    }

    LatencyCalibration::ProbeOptions probeOptions;
    probeOptions.amplitude = 0.42f;
    auto probe = LatencyCalibration::createProbeSignal(sampleRate, probeOptions);
    if (probe.empty())
    {
        const auto message = juce::String(juce::CharPointer_UTF8(
            u8"現在のサンプルレートではテスト音を作れませんでした"));
        {
            const juce::ScopedLock lock(latencyCalibration->resultLock);
            latencyCalibration->statusText = message;
        }
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
        showStatus(message, true);
        return;
    }

    constexpr int measurementCount = 4;
    const int firstEmission = juce::roundToInt(sampleRate * 0.65);
    const int emissionSpacing = juce::roundToInt(sampleRate * 0.80);
    const int tailSamples = juce::roundToInt(sampleRate * 0.55);
    const int maximumLatencySamples = juce::roundToInt(sampleRate * 0.50);
    const int lastEmission = firstEmission + (measurementCount - 1) * emissionSpacing;
    const int totalSamples = lastEmission + static_cast<int>(probe.size()) + tailSamples;

    try
    {
        latencyCalibration->probe = std::move(probe);
        latencyCalibration->playbackSignal.assign(static_cast<size_t>(totalSamples), 0.0f);
        latencyCalibration->emissionStartSamples.clear();
        latencyCalibration->emissionStartSamples.reserve(measurementCount);

        for (int measurement = 0; measurement < measurementCount; ++measurement)
        {
            const int emission = firstEmission + measurement * emissionSpacing;
            latencyCalibration->emissionStartSamples.push_back(emission);
            for (size_t sample = 0; sample < latencyCalibration->probe.size(); ++sample)
                latencyCalibration->playbackSignal[static_cast<size_t>(emission) + sample]
                    = latencyCalibration->probe[sample];
        }

        const int channelsToCapture = std::min(inputChannels, 2);
        latencyCalibration->capturedChannels.assign(
            static_cast<size_t>(channelsToCapture),
            std::vector<float>(static_cast<size_t>(totalSamples), 0.0f));
    }
    catch (...)
    {
        const auto message = juce::String(juce::CharPointer_UTF8(
            u8"自動測定用のメモリを確保できませんでした"));
        {
            const juce::ScopedLock lock(latencyCalibration->resultLock);
            latencyCalibration->statusText = message;
        }
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
        showStatus(message, true);
        return;
    }

    latencyCalibration->sampleRate = sampleRate;
    latencyCalibration->maximumLatencySamples = maximumLatencySamples;
    latencyCalibration->deviceFingerprint = createLatencyCalibrationFingerprint();
    latencyCalibration->generation.fetch_add(1);
    latencyCalibration->capturePosition.store(0, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        latencyCalibration->statusText = juce::CharPointer_UTF8(
            u8"測定中…テスト音が入力へ戻るようにしてください（約4秒）");
        latencyCalibration->pendingSuccess = false;
        latencyCalibration->pendingMessage.clear();
    }
    latencyCalibration->state.store(LatencyCalibrationSession::State::capturing,
                                    std::memory_order_release);
    showStatus(juce::CharPointer_UTF8(u8"自動音ズレ測定を開始しました"), false);
}

void MainComponent::cancelLatencyCalibration()
{
    if (latencyCalibration == nullptr)
        return;

    auto state = latencyCalibration->state.load(std::memory_order_acquire);
    for (;;)
    {
        if (state == LatencyCalibrationSession::State::cancelling)
            return;

        const auto targetState = state == LatencyCalibrationSession::State::capturing
            ? LatencyCalibrationSession::State::cancelling
            : LatencyCalibrationSession::State::failed;
        const bool isRunningState = state == LatencyCalibrationSession::State::capturing
            || state == LatencyCalibrationSession::State::captureReady
            || state == LatencyCalibrationSession::State::analysing
            || state == LatencyCalibrationSession::State::resultReady;
        if (!isRunningState)
            return;

        if (latencyCalibration->state.compare_exchange_weak(
                state, targetState, std::memory_order_acq_rel))
            break;
    }

    latencyCalibration->generation.fetch_add(1);
    const auto message = juce::String(juce::CharPointer_UTF8(
        u8"測定をキャンセルしました。以前の補正値は変更していません。"
        u8"AG03のダイレクトモニターをONに戻してください"));
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        latencyCalibration->statusText = message;
    }
    showStatus(message, false);
}

bool MainComponent::processLatencyCalibration(
    const juce::AudioSourceChannelInfo& bufferToFill,
    const float* const* inputChannelData,
    int numInputChannels) noexcept
{
    if (latencyCalibration == nullptr || bufferToFill.buffer == nullptr)
        return false;

    auto state = latencyCalibration->state.load(std::memory_order_acquire);
    if (state != LatencyCalibrationSession::State::capturing
        && state != LatencyCalibrationSession::State::cancelling)
    {
        return false;
    }

    latencyCalibration->activeAudioCallbacks.fetch_add(1, std::memory_order_acq_rel);
    state = latencyCalibration->state.load(std::memory_order_acquire);
    if (state != LatencyCalibrationSession::State::capturing
        && state != LatencyCalibrationSession::State::cancelling)
    {
        latencyCalibration->activeAudioCallbacks.fetch_sub(1, std::memory_order_release);
        return false;
    }

    audioEngine.updateInputMeter(inputChannelData,
                                 numInputChannels,
                                 bufferToFill.numSamples);

    bufferToFill.buffer->clear(bufferToFill.startSample, bufferToFill.numSamples);

    const int position = latencyCalibration->capturePosition.load(std::memory_order_relaxed);
    const int totalSamples = static_cast<int>(latencyCalibration->playbackSignal.size());
    const int samplesToProcess = std::max(
        0, std::min(bufferToFill.numSamples, totalSamples - position));
    if (samplesToProcess <= 0)
    {
        auto expected = LatencyCalibrationSession::State::capturing;
        if (!latencyCalibration->state.compare_exchange_strong(
                expected, LatencyCalibrationSession::State::captureReady,
                std::memory_order_acq_rel))
        {
            latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                            std::memory_order_release);
        }
        latencyCalibration->activeAudioCallbacks.fetch_sub(1, std::memory_order_release);
        return true;
    }

    const int channelsToCapture = static_cast<int>(latencyCalibration->capturedChannels.size());
    for (int channel = 0; channel < channelsToCapture; ++channel)
    {
        float* destination = latencyCalibration->capturedChannels[static_cast<size_t>(channel)].data()
                             + position;
        if (inputChannelData != nullptr && channel < numInputChannels
            && inputChannelData[channel] != nullptr)
        {
            juce::FloatVectorOperations::copy(destination,
                                              inputChannelData[channel],
                                              samplesToProcess);
        }
        else
        {
            juce::FloatVectorOperations::clear(destination, samplesToProcess);
        }
    }

    const float* source = latencyCalibration->playbackSignal.data() + position;
    for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
    {
        juce::FloatVectorOperations::copy(
            bufferToFill.buffer->getWritePointer(channel, bufferToFill.startSample),
            source,
            samplesToProcess);
    }

    const auto fadeOutAndStop = [&]() noexcept {
        for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
        {
            bufferToFill.buffer->applyGainRamp(channel,
                                               bufferToFill.startSample,
                                               samplesToProcess,
                                               1.0f,
                                               0.0f);
        }
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
    };

    if (latencyCalibration->state.load(std::memory_order_acquire)
        == LatencyCalibrationSession::State::cancelling)
    {
        fadeOutAndStop();
        latencyCalibration->activeAudioCallbacks.fetch_sub(1, std::memory_order_release);
        return true;
    }

    const int nextPosition = position + samplesToProcess;
    latencyCalibration->capturePosition.store(nextPosition, std::memory_order_relaxed);
    if (nextPosition >= totalSamples)
    {
        auto expected = LatencyCalibrationSession::State::capturing;
        if (!latencyCalibration->state.compare_exchange_strong(
                expected, LatencyCalibrationSession::State::captureReady,
                std::memory_order_acq_rel)
            && expected == LatencyCalibrationSession::State::cancelling)
        {
            fadeOutAndStop();
        }
    }
    latencyCalibration->activeAudioCallbacks.fetch_sub(1, std::memory_order_release);
    return true;
}

void MainComponent::serviceLatencyCalibration()
{
    if (latencyCalibration == nullptr)
        return;

    if (latencyDeviceRefreshRequested.exchange(false, std::memory_order_acq_rel))
        refreshLatencyCalibrationForCurrentDevice();

    const auto state = latencyCalibration->state.load(std::memory_order_acquire);
    if (state == LatencyCalibrationSession::State::captureReady)
        beginLatencyCalibrationAnalysis();
    else if (state == LatencyCalibrationSession::State::resultReady)
        applyLatencyCalibrationResult();
}

void MainComponent::beginLatencyCalibrationAnalysis()
{
    auto expected = LatencyCalibrationSession::State::captureReady;
    if (latencyCalibration == nullptr
        || !latencyCalibration->state.compare_exchange_strong(
            expected, LatencyCalibrationSession::State::analysing,
            std::memory_order_acq_rel))
    {
        return;
    }

    std::vector<std::vector<float>> capturedChannels;
    std::vector<float> probe;
    std::vector<int> emissionStarts;
    try
    {
        capturedChannels = latencyCalibration->capturedChannels;
        probe = latencyCalibration->probe;
        emissionStarts = latencyCalibration->emissionStartSamples;
    }
    catch (...)
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        latencyCalibration->statusText = juce::CharPointer_UTF8(
            u8"測定結果を解析するためのメモリを確保できませんでした");
        latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                        std::memory_order_release);
        return;
    }

    const double sampleRate = latencyCalibration->sampleRate;
    const int maximumLatencySamples = latencyCalibration->maximumLatencySamples;
    const auto fingerprint = latencyCalibration->deviceFingerprint;
    const auto generation = latencyCalibration->generation.load();
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        latencyCalibration->statusText = juce::CharPointer_UTF8(u8"測定結果を解析中…");
    }

    latencyAnalysisThread.addJob(
        [this, capturedChannels = std::move(capturedChannels), probe = std::move(probe),
         emissionStarts = std::move(emissionStarts), sampleRate, maximumLatencySamples,
         fingerprint, generation]() mutable
        {
            try
            {
            LatencyCalibration::AggregateResult bestResult;
            bool foundResult = false;
            bool sawQuietInput = false;
            bool sawClipping = false;
            bool sawUnreliableInput = false;

            for (const auto& captured : capturedChannels)
            {
                std::vector<LatencyCalibration::Measurement> measurements;
                measurements.reserve(emissionStarts.size());

                for (const int emissionStart : emissionStarts)
                {
                    const int windowLength = maximumLatencySamples
                                             + static_cast<int>(probe.size());
                    const int available = static_cast<int>(captured.size()) - emissionStart;
                    if (emissionStart < 0 || available < windowLength)
                        continue;

                    std::vector<float> window(
                        captured.begin() + emissionStart,
                        captured.begin() + emissionStart + windowLength);
                    LatencyCalibration::AnalysisOptions options;
                    options.maximumLatencySamples = maximumLatencySamples;
                    // Microphone-through-headphone measurements are often much
                    // quieter than a cable loopback. Correlation still guards
                    // against accepting unrelated room noise.
                    options.minimumInputRms = 0.0007f;
                    const auto measurement = LatencyCalibration::analyse(
                        probe, window, sampleRate, options);
                    sawQuietInput = sawQuietInput
                        || measurement.status == LatencyCalibration::MeasurementStatus::inputTooQuiet;
                    sawClipping = sawClipping
                        || measurement.status == LatencyCalibration::MeasurementStatus::inputClipped;
                    sawUnreliableInput = sawUnreliableInput
                        || measurement.status == LatencyCalibration::MeasurementStatus::unreliableMatch;
                    measurements.push_back(measurement);
                }

                LatencyCalibration::AggregateOptions aggregateOptions;
                aggregateOptions.minimumValidMeasurements = 3;
                aggregateOptions.minimumMeasurementConfidence = 0.55f;
                aggregateOptions.maximumSpreadMilliseconds = 2.0;
                const auto result = LatencyCalibration::aggregate(
                    measurements, sampleRate, aggregateOptions);
                if (result.isValid()
                    && (!foundResult || result.confidence > bestResult.confidence))
                {
                    bestResult = result;
                    foundResult = true;
                }
            }

            if (latencyCalibration == nullptr
                || latencyCalibration->generation.load() != generation)
            {
                return;
            }

            juce::String message;
            if (foundResult)
            {
                message = juce::String(juce::CharPointer_UTF8(u8"測定成功: 自動補正 "))
                          + juce::String(bestResult.latencyMilliseconds, 1) + " ms  ("
                          + juce::String(juce::roundToInt(bestResult.confidence * 100.0f))
                          + juce::String(juce::CharPointer_UTF8(u8"% 信頼度)"));
            }
            else if (sawClipping)
            {
                message = juce::CharPointer_UTF8(
                    u8"測定失敗: 入力音が大きすぎます。音量を下げて再測定してください");
            }
            else if (sawQuietInput && !sawUnreliableInput)
            {
                message = juce::CharPointer_UTF8(
                    u8"測定失敗: テスト音が入力へ届いていません。接続またはマイク位置を確認してください");
            }
            else
            {
                message = juce::CharPointer_UTF8(
                    u8"測定失敗: 値が安定しませんでした。周囲を静かにして再測定してください");
            }

            {
                const juce::ScopedLock lock(latencyCalibration->resultLock);
                latencyCalibration->pendingSuccess = foundResult;
                latencyCalibration->pendingLatencySamples = foundResult
                    ? bestResult.latencySamples : 0;
                latencyCalibration->pendingConfidence = foundResult
                    ? bestResult.confidence : 0.0f;
                latencyCalibration->pendingMessage = message;
                latencyCalibration->pendingFingerprint = fingerprint;
            }

            if (latencyCalibration->generation.load() == generation)
                latencyCalibration->state.store(LatencyCalibrationSession::State::resultReady,
                                                std::memory_order_release);
            }
            catch (...)
            {
                if (latencyCalibration == nullptr
                    || latencyCalibration->generation.load() != generation)
                {
                    return;
                }

                {
                    const juce::ScopedLock lock(latencyCalibration->resultLock);
                    latencyCalibration->pendingSuccess = false;
                    latencyCalibration->pendingLatencySamples = 0;
                    latencyCalibration->pendingConfidence = 0.0f;
                    latencyCalibration->pendingMessage = juce::CharPointer_UTF8(
                        u8"測定失敗: 結果の解析中にエラーが発生しました");
                    latencyCalibration->pendingFingerprint = fingerprint;
                }
                latencyCalibration->state.store(LatencyCalibrationSession::State::resultReady,
                                                std::memory_order_release);
            }
        });
}

void MainComponent::applyLatencyCalibrationResult()
{
    if (latencyCalibration == nullptr
        || latencyCalibration->state.load(std::memory_order_acquire)
               != LatencyCalibrationSession::State::resultReady)
    {
        return;
    }

    bool success = false;
    int latencySamples = 0;
    float confidence = 0.0f;
    juce::String message;
    juce::String measuredFingerprint;
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        success = latencyCalibration->pendingSuccess;
        latencySamples = latencyCalibration->pendingLatencySamples;
        confidence = latencyCalibration->pendingConfidence;
        message = latencyCalibration->pendingMessage;
        measuredFingerprint = latencyCalibration->pendingFingerprint;
    }

    if (success && measuredFingerprint != createLatencyCalibrationFingerprint())
    {
        success = false;
        message = juce::CharPointer_UTF8(
            u8"測定中にオーディオ設定が変わったため、結果を保存しませんでした");
    }

    if (success)
    {
        message += juce::String(juce::CharPointer_UTF8(
            u8"。AG03のダイレクトモニターをONに戻してください"));
        audioEngine.setCalibratedRecordingLatencySamples(latencySamples);
        if (appSettings != nullptr)
        {
            const auto key = juce::String("latencyCalibration.")
                + juce::String::toHexString(
                    static_cast<juce::int64>(measuredFingerprint.hashCode64()));
            appSettings->setValue(key + ".fingerprint", measuredFingerprint);
            appSettings->setValue(key + ".samples", latencySamples);
            appSettings->setValue(key + ".sampleRate", latencyCalibration->sampleRate);
            appSettings->setValue(key + ".confidence", static_cast<double>(confidence));
            appSettings->saveIfNeeded();
        }

        {
            const juce::ScopedLock lock(latencyCalibration->resultLock);
            latencyCalibration->statusText = message;
        }
        latencyCalibration->state.store(LatencyCalibrationSession::State::succeeded,
                                        std::memory_order_release);
        showStatus(message, false);
        return;
    }

    message += juce::String(juce::CharPointer_UTF8(
        u8"。以前の補正値は変更していません。"
        u8"AG03のダイレクトモニターをONに戻してください"));
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        latencyCalibration->statusText = message;
    }
    latencyCalibration->state.store(LatencyCalibrationSession::State::failed,
                                    std::memory_order_release);
    showStatus(message, true);
}

void MainComponent::refreshLatencyCalibrationForCurrentDevice()
{
    if (latencyCalibration == nullptr)
        return;

    const auto fingerprint = createLatencyCalibrationFingerprint();
    const double sampleRate = std::max(1.0, audioEngine.getSampleRate());
    const auto key = juce::String("latencyCalibration.")
        + juce::String::toHexString(static_cast<juce::int64>(fingerprint.hashCode64()));
    const bool fingerprintMatches = appSettings != nullptr
        && appSettings->getValue(key + ".fingerprint") == fingerprint;
    const int savedSamples = fingerprintMatches
        ? appSettings->getIntValue(key + ".samples", -1) : -1;
    const bool savedValueIsValid = savedSamples >= 0
        && savedSamples <= juce::roundToInt(sampleRate * 2.0);

    juce::String status;
    if (savedValueIsValid)
    {
        audioEngine.setCalibratedRecordingLatencySamples(savedSamples);
        status = juce::String(juce::CharPointer_UTF8(u8"保存済みの実測補正 "))
                 + juce::String(savedSamples * 1000.0 / sampleRate, 1)
                 + juce::String(juce::CharPointer_UTF8(u8" msを使用中"));
        latencyCalibration->state.store(LatencyCalibrationSession::State::succeeded,
                                        std::memory_order_release);
    }
    else
    {
        audioEngine.clearCalibratedRecordingLatency();
        status = juce::CharPointer_UTF8(u8"未測定（機器情報から推定して補正中）");
        latencyCalibration->state.store(LatencyCalibrationSession::State::idle,
                                        std::memory_order_release);
    }

    latencyCalibration->deviceFingerprint = fingerprint;
    {
        const juce::ScopedLock lock(latencyCalibration->resultLock);
        latencyCalibration->statusText = status;
    }
}

juce::String MainComponent::createLatencyCalibrationFingerprint() const
{
    const auto setup = deviceManager.getAudioDeviceSetup();
    auto* device = deviceManager.getCurrentAudioDevice();
    const double sampleRate = device != nullptr
        ? device->getCurrentSampleRate() : setup.sampleRate;
    const int bufferSize = device != nullptr
        ? device->getCurrentBufferSizeSamples() : setup.bufferSize;
    const int inputLatency = device != nullptr ? device->getInputLatencyInSamples() : 0;
    const int outputLatency = device != nullptr ? device->getOutputLatencyInSamples() : 0;

    juce::String fingerprint;
    fingerprint << "v1|" << deviceManager.getCurrentAudioDeviceType()
                << "|in=" << setup.inputDeviceName
                << "|out=" << setup.outputDeviceName
                << "|rate=" << juce::roundToInt(sampleRate)
                << "|buffer=" << bufferSize
                << "|inChannels=" << setup.inputChannels.toString(16)
                << "|outChannels=" << setup.outputChannels.toString(16)
                << "|reported=" << inputLatency << "+" << outputLatency;
    return fingerprint;
}

juce::String MainComponent::getLatencyCalibrationStatusText() const
{
    if (latencyCalibration == nullptr)
        return {};
    const juce::ScopedLock lock(latencyCalibration->resultLock);
    return latencyCalibration->statusText;
}

bool MainComponent::isLatencyCalibrationRunning() const
{
    if (latencyCalibration == nullptr)
        return false;

    const auto state = latencyCalibration->state.load(std::memory_order_acquire);
    return state == LatencyCalibrationSession::State::capturing
        || state == LatencyCalibrationSession::State::cancelling
        || state == LatencyCalibrationSession::State::captureReady
        || state == LatencyCalibrationSession::State::analysing
        || state == LatencyCalibrationSession::State::resultReady;
}

void MainComponent::requestQuit()
{
    if (projectIoInProgress.load())
    {
        showStatus(juce::CharPointer_UTF8(u8"プロジェクトの処理が完了するまでお待ちください"));
        return;
    }

    cancelLatencyCalibration();

    if (audioEngine.getPlaybackState() != PlaybackState::Stopped)
        audioEngine.stop();

    confirmBeforeDestructiveAction(juce::CharPointer_UTF8(u8"アプリを終了する"),
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]() {
            if (safeThis == nullptr) return;
            safeThis->clearRecoveryFile();
            juce::JUCEApplication::getInstance()->quit();
        });
}
