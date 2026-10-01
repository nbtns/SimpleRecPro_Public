#include "TransportBarComponent.h"
#include "UiTheme.h"

/** 時間をMM:SS形式にフォーマット（Web版のformatTimeに相当） */
static juce::String formatTime(double seconds)
{
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    return juce::String(mins).paddedLeft('0', 2) + ":" + juce::String(secs).paddedLeft('0', 2);
}

namespace {
    juce::Path createArrowPath(float cx, float cy, float w, float h) {
        juce::Path p;
        p.startNewSubPath(cx - w*0.3f, cy - h*0.4f);
        p.lineTo(cx + w*0.4f, cy + h*0.1f);
        p.lineTo(cx + w*0.1f, cy + h*0.1f);
        p.lineTo(cx + w*0.3f, cy + h*0.5f);
        p.lineTo(cx + w*0.1f, cy + h*0.6f);
        p.lineTo(cx - w*0.1f, cy + h*0.2f);
        p.lineTo(cx - w*0.4f, cy + h*0.4f);
        p.closeSubPath();
        return p;
    }
    
    class TransportButtonLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        juce::Font getTextButtonFont(juce::TextButton&, int) override
        {
            return juce::Font(juce::FontOptions(16.0f));
        }

        void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                  const juce::Colour& backgroundColour,
                                  bool highlighted, bool down) override
        {
            auto colour = backgroundColour;
            if (!button.isEnabled()) colour = UiTheme::controlSurface.withAlpha(0.45f);
            else if (down) colour = colour.darker(0.18f);
            else if (highlighted) colour = colour.brighter(0.10f);
            const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
            g.setColour(colour);
            g.fillRoundedRectangle(bounds, 7.0f);
            g.setColour(UiTheme::border);
            g.drawRoundedRectangle(bounds, 7.0f, 1.0f);
        }
    };
    TransportButtonLookAndFeel transportLF;
}

ToolButton::ToolButton(const juce::String& name, Type t) : juce::Button(name), type(t)
{
    setClickingTogglesState(true);
    setRadioGroupId(1);
}

void ToolButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(shouldDrawButtonAsDown);
    auto bounds = getLocalBounds().toFloat();
    
    auto fill = getToggleState() ? UiTheme::accentSoft
                                 : (shouldDrawButtonAsHighlighted
                                        ? UiTheme::controlHover
                                        : UiTheme::controlSurface);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds.reduced(0.5f), 8.0f);
    g.setColour(getToggleState() ? UiTheme::accent : UiTheme::border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
        
    g.setColour(getToggleState() ? UiTheme::accentHover : UiTheme::textPrimary);
    
    auto iconBounds = bounds.withWidth(32.0f).reduced(5.0f, 10.0f);
    float cx = iconBounds.getCentreX();
    float cy = bounds.getCentreY();
    float w = iconBounds.getWidth();
    float h = iconBounds.getHeight();
    
    switch (type)
    {
        case Type::Arrow:
            g.fillPath(createArrowPath(cx, cy, w, h));
            break;
            
        case Type::Hand:
        {
            // 小さいボタンでも判別しやすい、塗りの手のひらシルエット。
            const float fingerWidth = w * 0.13f;
            const float fingerRadius = fingerWidth * 0.48f;

            g.fillRoundedRectangle(
                cx - w * 0.30f, cy - h * 0.05f,
                w * 0.62f, h * 0.48f, w * 0.12f);

            g.fillRoundedRectangle(
                cx - w * 0.30f, cy - h * 0.34f,
                fingerWidth, h * 0.40f, fingerRadius);
            g.fillRoundedRectangle(
                cx - w * 0.14f, cy - h * 0.49f,
                fingerWidth, h * 0.55f, fingerRadius);
            g.fillRoundedRectangle(
                cx + w * 0.02f, cy - h * 0.56f,
                fingerWidth, h * 0.62f, fingerRadius);
            g.fillRoundedRectangle(
                cx + w * 0.18f, cy - h * 0.46f,
                fingerWidth, h * 0.52f, fingerRadius);

            juce::Path thumb;
            thumb.addRoundedRectangle(
                cx - w * 0.47f, cy - h * 0.02f,
                w * 0.34f, h * 0.15f, w * 0.07f);
            thumb.applyTransform(juce::AffineTransform::rotation(
                -0.52f, cx - w * 0.16f, cy + h * 0.05f));
            g.fillPath(thumb);
            break;
        }
            
        case Type::Split:
            g.drawEllipse(cx - w*0.4f, cy + h*0.1f, w*0.3f, w*0.3f, 1.5f);
            g.drawEllipse(cx + w*0.1f, cy + h*0.1f, w*0.3f, w*0.3f, 1.5f);
            g.drawLine(cx - w*0.2f, cy + h*0.1f, cx + w*0.3f, cy - h*0.5f, 1.5f);
            g.drawLine(cx + w*0.2f, cy + h*0.1f, cx - w*0.3f, cy - h*0.5f, 1.5f);
            g.fillEllipse(cx - 1.5f, cy - 1.5f, 3.0f, 3.0f);
            break;
    }
    g.setFont(juce::Font(juce::FontOptions(16.0f)));
    const auto text = type == Type::Arrow ? juce::CharPointer_UTF8(u8"選択")
                    : type == Type::Hand ? juce::CharPointer_UTF8(u8"移動")
                                         : juce::CharPointer_UTF8(u8"分割");
    g.drawText(text, getLocalBounds().withTrimmedLeft(32).reduced(2, 0),
               juce::Justification::centredLeft);
}

TransportBarComponent::TransportBarComponent()
{
    editingToolbar.onResized = [this] { layoutEditingToolbar(); };
    // Metronome
    addAndMakeVisible(bpmTitleLabel);
    bpmTitleLabel.setFont(juce::Font(juce::FontOptions(14.0f)));
    bpmTitleLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    bpmTitleLabel.setJustificationType(juce::Justification::centredLeft);
    
    addAndMakeVisible(bpmValueLabel);
    bpmValueLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    bpmValueLabel.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    bpmValueLabel.setJustificationType(juce::Justification::centredLeft);
    // BPM数値ラベルのみを編集可能にする
    bpmValueLabel.setEditable(true, true);
    bpmValueLabel.setTooltip(juce::CharPointer_UTF8(u8"クリックしてBPMを入力"));
    bpmValueLabel.setColour(juce::Label::backgroundColourId, UiTheme::controlSurface);
    bpmValueLabel.onTextChange = [this]() {
        if (audioEngine == nullptr) return;
        int newBpm = bpmValueLabel.getText().retainCharacters("0123456789").getIntValue();
        if (newBpm >= 20 && newBpm <= 300)
        {
            const double time = audioEngine->getCurrentTime();
            const auto point = audioEngine->getTempoPointAt(time);
            audioEngine->setTempoPoint(time, newBpm,
                                       point.numerator,
                                       point.denominator);
            bpmValueLabel.setText(juce::String(newBpm), juce::dontSendNotification);
        }
    };
    
    tapButton.setButtonText("TAP");
    tapButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    tapButton.onClick = [this]() {
        if (!audioEngine) return;
        
        juce::uint32 now = juce::Time::getMillisecondCounter();
        
        // 2秒以上空いたらリセット
        if (!tapTimes.empty() && now - tapTimes.back() > 2000)
            tapTimes.clear();
            
        tapTimes.push_back(now);
        
        // 過去4回分まで保持
        if (tapTimes.size() > 5)
            tapTimes.erase(tapTimes.begin());
            
        if (tapTimes.size() >= 2)
        {
            double totalInterval = 0.0;
            for (size_t i = 1; i < tapTimes.size(); ++i)
                totalInterval += static_cast<double>(tapTimes[i] - tapTimes[i - 1]);
                
            double averageIntervalMs = totalInterval / static_cast<double>(tapTimes.size() - 1);
            int newBpm = static_cast<int>(std::round(60000.0 / averageIntervalMs));
            
            const double time = audioEngine->getCurrentTime();
            const auto point = audioEngine->getTempoPointAt(time);
            audioEngine->setTempoPoint(time, newBpm,
                                       point.numerator,
                                       point.denominator);
            bpmValueLabel.setText(juce::String(newBpm), juce::dontSendNotification);
            
            // UI全体の再描画（グリッド線の更新）
            if (getParentComponent())
                getParentComponent()->repaint();
        }
    };
    addAndMakeVisible(tempoDetailsButton);
    tempoDetailsButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    tempoDetailsButton.setTooltip(juce::CharPointer_UTF8(u8"テンポの詳細：TAP・拍子"));
    tempoDetailsButton.onClick = [this]() {
        auto panel = std::make_unique<juce::Component>();
        panel->setSize(224, 76);
        panel->addAndMakeVisible(tapButton);
        panel->addAndMakeVisible(timeSigButton);
        tapButton.setBounds(12, 14, 86, 48);
        timeSigButton.setBounds(108, 14, 104, 48);
        juce::CallOutBox::launchAsynchronously(
            std::move(panel), tempoDetailsButton.getScreenBounds(), nullptr);
    };

    timeSigButton.setButtonText(juce::CharPointer_UTF8(u8"4/4 \u25BC")); // ▼をつける
    timeSigButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    timeSigButton.onClick = [this]() {
        juce::PopupMenu m;
        m.addItem(1, "4/4");
        m.addItem(2, "3/4");
        m.addItem(3, "6/8");
        m.addItem(4, "2/4");
        m.addItem(5, "5/4");
        m.addItem(6, "7/8");

        juce::PopupMenu::Options options = juce::PopupMenu::Options()
            .withTargetComponent(&timeSigButton)
            .withStandardItemHeight(36);
            
        const auto safeThis = juce::Component::SafePointer<TransportBarComponent>(this);
        m.showMenuAsync(options, [safeThis](int result) {
            if (safeThis == nullptr || result == 0) return;
            
            juce::String text = "4/4";
            if (result == 2) text = "3/4";
            if (result == 3) text = "6/8";
            if (result == 4) text = "2/4";
            if (result == 5) text = "5/4";
            if (result == 6) text = "7/8";
            
            safeThis->timeSigButton.setButtonText(
                text + juce::String(juce::CharPointer_UTF8(u8" \u25BC")));
            
            if (safeThis->audioEngine)
            {
                auto parts = juce::StringArray::fromTokens(text, "/", "");
                if (parts.size() == 2)
                {
                    const double time = safeThis->audioEngine->getCurrentTime();
                    const auto point = safeThis->audioEngine->getTempoPointAt(time);
                    safeThis->audioEngine->setTempoPoint(
                        time,
                        point.bpm,
                        parts[0].getIntValue(),
                        parts[1].getIntValue());
                }
            }
        });
    };
    
    clickButton.setButtonText(juce::CharPointer_UTF8(u8"クリック"));
    clickButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    addAndMakeVisible(clickButton);
    clickButton.onClick = [this]() {
        if (audioEngine == nullptr) return;
        auto& met = audioEngine->getMetronome();
        met.setEnabled(!met.getEnabled());
        audioEngine->markDirty();
        // ボタンの色でON/OFFを表示
        clickButton.setColour(juce::TextButton::buttonColourId,
            met.getEnabled() ? UiTheme::success : UiTheme::controlSurface);
    };
    
    countInButton.setButtonText(juce::CharPointer_UTF8(u8"カウントイン"));
    countInButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    addAndMakeVisible(countInButton);
    countInButton.onClick = [this]() {
        if (audioEngine == nullptr) return;
        bool isEnabled = !audioEngine->isCountInEnabled();
        audioEngine->setCountInEnabled(isEnabled);
        audioEngine->markDirty();
        countInButton.setColour(juce::TextButton::buttonColourId,
            isEnabled ? UiTheme::success : UiTheme::controlSurface);
    };

    addAndMakeVisible(clickVolSlider);
    addAndMakeVisible(clickVolLabel);
    clickVolLabel.setFont(
        juce::Font(juce::FontOptions(14.0f)));
    clickVolLabel.setText(juce::CharPointer_UTF8(u8"クリック音量"), juce::dontSendNotification);
    clickVolLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    clickVolLabel.setJustificationType(juce::Justification::centredLeft);
    clickVolSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    clickVolSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    clickVolSlider.setRange(0.0, 1.0, 0.01);
    clickVolSlider.setValue(0.5);
    clickVolSlider.setTooltip(
        juce::CharPointer_UTF8(u8"メトロノームの音量"));
    clickVolSlider.onValueChange = [this]() {
        if (audioEngine == nullptr) return;
        audioEngine->getMetronome().setVolume(static_cast<float>(clickVolSlider.getValue()));
        audioEngine->markDirty();
    };

    // Transport - ボタンにコールバックを接続
    recordButton.setButtonText(juce::CharPointer_UTF8(u8"● 録音"));
    playButton.setButtonText(juce::CharPointer_UTF8(u8"▶ 再生"));
    stopButton.setButtonText(juce::CharPointer_UTF8(u8"■ 停止"));

    addAndMakeVisible(recordButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    
    recordButton.setLookAndFeel(&transportLF);
    playButton.setLookAndFeel(&transportLF);
    stopButton.setLookAndFeel(&transportLF);

    recordButton.setColour(juce::TextButton::buttonColourId, UiTheme::danger);
    playButton.setColour(juce::TextButton::buttonColourId, UiTheme::success);
    stopButton.setColour(juce::TextButton::buttonColourId, UiTheme::controlSurface);
    
    // 録音ボタン: ARM済みトラックに録音開始
    recordButton.onClick = [this]() {
        if (audioEngine == nullptr) return;
        
        const auto state = audioEngine->getPlaybackState();
        if (state == PlaybackState::Recording || state == PlaybackState::CountIn)
        {
            // 録音中またはカウントイン中なら停止
            audioEngine->stop();
            return;
        }
        
        // ARMされたトラックを探す
        const auto tracks = audioEngine->getTracksSnapshot();
        int armedIndex = -1;
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            if (tracks[static_cast<size_t>(i)].isArmed)
            {
                armedIndex = i;
                break;
            }
        }
        
        if (armedIndex < 0)
        {
            // ARMされたトラックがない場合、新しいトラックを作成してARM
            armedIndex = audioEngine->addTrack();
            audioEngine->toggleArm(armedIndex);
        }
        
        audioEngine->record(armedIndex);
    };
    
    // 再生ボタン
    playButton.onClick = [this]() {
        if (audioEngine == nullptr) return;
        
        if (audioEngine->getPlaybackState() == PlaybackState::Playing)
        {
            // 再生中なら一時停止（stop）
            audioEngine->stop();
        }
        else if (audioEngine->getPlaybackState() == PlaybackState::Stopped)
        {
            audioEngine->play();
        }
    };
    
    // 停止ボタン: 先頭に戻す
    stopButton.onClick = [this]() {
        if (audioEngine == nullptr) return;
        
        // まず停止し、その後もう一度呼んで先頭に戻す
        if (audioEngine->getPlaybackState() != PlaybackState::Stopped)
        {
            audioEngine->stop();
        }
        // 先頭に戻す
        audioEngine->stop();
    };
    
    // Time
    addAndMakeVisible(timeLabel);
    timeLabel.setText("00:00.000", juce::dontSendNotification);
    timeLabel.setFont(juce::Font(juce::FontOptions(23.0f)));
    timeLabel.setColour(juce::Label::textColourId, UiTheme::textPrimary);
    timeLabel.setJustificationType(juce::Justification::centredLeft);
    for (auto* label : { &playbackLabel, &durationLabel, &masterVolLabel, &masterVolValue })
    {
        addAndMakeVisible(*label);
        label->setFont(juce::Font(juce::FontOptions(14.0f)));
        label->setColour(juce::Label::textColourId, UiTheme::textSecondary);
    }
    masterVolValue.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(masterVolSlider);
    masterVolSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    masterVolSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    masterVolSlider.setRange(0.0, 1.0, 0.01);
    masterVolSlider.setValue(0.8);
    masterVolSlider.setTooltip(juce::CharPointer_UTF8(u8"全体の再生音量"));
    masterVolSlider.onValueChange = [this]() {
        masterVolValue.setText(juce::String(juce::roundToInt(masterVolSlider.getValue() * 100.0)) + " %",
                               juce::dontSendNotification);
        if (audioEngine != nullptr)
        {
            audioEngine->setMasterVolume(static_cast<float>(masterVolSlider.getValue()));
            audioEngine->markDirty();
        }
    };

    // 通知専用の行に表示し、録音・再生ボタンを覆わない。
    editingToolbar.addAndMakeVisible(statusLabel);
    statusLabel.setVisible(false);
    statusLabel.setFont(juce::Font(14.0f, juce::Font::bold));
    statusLabel.setJustificationType(juce::Justification::centred);
    statusLabel.setInterceptsMouseClicks(false, false);

    // Tools & Export
    editingToolbar.addAndMakeVisible(arrowButton);
    editingToolbar.addAndMakeVisible(handButton);
    editingToolbar.addAndMakeVisible(splitButton);
    editingToolbar.addAndMakeVisible(loopStartButton);
    editingToolbar.addAndMakeVisible(loopEndButton);
    editingToolbar.addAndMakeVisible(loopToggleButton);
    editingToolbar.addAndMakeVisible(loopClearButton);
    handButton.setTooltip(
        juce::CharPointer_UTF8(u8"手のひらツール（ドラッグ移動）"));
    
    arrowButton.setToggleState(true, juce::dontSendNotification);
    
    arrowButton.onClick = [this]() {
        if (arrowButton.getToggleState() && audioEngine) 
            audioEngine->setCursorMode(CursorMode::Arrow);
    };
    handButton.onClick = [this]() {
        if (handButton.getToggleState() && audioEngine) 
            audioEngine->setCursorMode(CursorMode::Hand);
    };
    splitButton.onClick = [this]() {
        if (splitButton.getToggleState() && audioEngine) 
            audioEngine->setCursorMode(CursorMode::Split);
    };

    for (auto* button : { &loopStartButton, &loopEndButton,
                          &loopToggleButton, &loopClearButton })
    {
        button->setColour(juce::TextButton::buttonColourId,
                          UiTheme::controlSurface);
        button->setColour(juce::TextButton::textColourOffId,
                          UiTheme::textSecondary);
    }
    loopStartButton.setTooltip(juce::CharPointer_UTF8(u8"現在位置をループ開始Aに設定"));
    loopEndButton.setTooltip(juce::CharPointer_UTF8(u8"現在位置をループ終了Bに設定"));
    loopToggleButton.setTooltip(juce::CharPointer_UTF8(u8"A/B範囲の繰り返し再生"));
    loopClearButton.setTooltip(juce::CharPointer_UTF8(u8"A/B範囲を消去"));
    loopStartButton.onClick = [this]() {
        if (audioEngine != nullptr) audioEngine->setLoopStartAtCurrentTime();
        updateLoopControls();
    };
    loopEndButton.onClick = [this]() {
        if (audioEngine != nullptr) audioEngine->setLoopEndAtCurrentTime();
        updateLoopControls();
    };
    loopToggleButton.onClick = [this]() {
        if (audioEngine == nullptr) return;
        const auto settings = audioEngine->getProjectDawSettings();
        audioEngine->setLoopEnabled(!settings.loopEnabled);
        updateLoopControls();
    };
    loopClearButton.onClick = [this]() {
        if (audioEngine != nullptr) audioEngine->clearLoopRange();
        updateLoopControls();
    };
    
    editingToolbar.addAndMakeVisible(zoomSlider);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.setRange(10.0, 500.0, 1.0); // AudioEngineの保存可能範囲と揃える
    zoomSlider.setValue(50.0);
    zoomSlider.setTooltip(
        juce::CharPointer_UTF8(u8"タイムラインの横方向の拡大率"));
    zoomSlider.onValueChange = [this]() {
        if (audioEngine)
        {
            double value = zoomSlider.getValue();
            audioEngine->setZoomLevel(value);
            updateZoomLabelText();
        }
    };
    
    editingToolbar.addAndMakeVisible(zoomLabel);
    updateZoomLabelText();
    zoomLabel.setColour(juce::Label::textColourId, UiTheme::textSecondary);
    zoomLabel.setJustificationType(juce::Justification::centredRight);
    
    exportButton.setButtonText(juce::CharPointer_UTF8(u8"書き出す"));
    editingToolbar.addAndMakeVisible(exportButton);
    exportButton.setColour(juce::TextButton::buttonColourId, UiTheme::accent);
    exportButton.onClick = [this]() {
        if (audioEngine == nullptr)
        {
            showStatusMessage(juce::CharPointer_UTF8(u8"オーディオ機能を利用できません"), true);
            return;
        }
        if (audioEngine->getPlaybackState() != PlaybackState::Stopped)
        {
            showStatusMessage(juce::CharPointer_UTF8(u8"停止してから書き出してください"), true);
            return;
        }
        if (audioEngine->getTracksSnapshot().empty() || audioEngine->getDuration() <= 0.0)
        {
            showStatusMessage(juce::CharPointer_UTF8(u8"書き出せる音声がありません"), true);
            return;
        }

        if (onExportClicked)
        {
            onExportClicked();
            return;
        }
        
        juce::PopupMenu m;
        m.addItem(1, juce::CharPointer_UTF8(u8"\u5168\u4F53\u3092\u30DF\u30C3\u30AF\u30B9\u30C0\u30A6\u30F3")); // 全体をミックスダウン
        m.addSeparator();
        
        const auto tracks = audioEngine->getTracksSnapshot();
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            m.addItem(static_cast<int>(i + 2), juce::String(juce::CharPointer_UTF8(u8"\u30C8\u30E9\u30C3\u30AF: ")) + tracks[i].name);
        }
        
        juce::PopupMenu::Options options = juce::PopupMenu::Options()
            .withTargetComponent(&exportButton);
            
        const auto safeThis = juce::Component::SafePointer<TransportBarComponent>(this);
        m.showMenuAsync(options, [safeThis](int result) {
            if (safeThis == nullptr || result == 0) return; // キャンセル
            
            if (safeThis->audioEngine)
            {
                if (result == 1)
                {
                    safeThis->showStatusMessage(
                        juce::CharPointer_UTF8(u8"ミックスダウンの保存先を選択してください"));
                    safeThis->audioEngine->exportMixdownToWav();
                }
                else
                {
                    int trackIndex = result - 2;
                    const auto tracks = safeThis->audioEngine->getTracksSnapshot();
                    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())
                        || tracks[static_cast<size_t>(trackIndex)].clips.empty())
                    {
                        safeThis->showStatusMessage(
                            juce::CharPointer_UTF8(u8"このトラックには書き出せる音声がありません"), true);
                        return;
                    }

                    safeThis->showStatusMessage(
                        juce::CharPointer_UTF8(u8"トラックの保存先を選択してください"));
                    safeThis->audioEngine->exportTrackToWav(trackIndex);
                }
            }
        });
    };
    for (auto* button : { &tapButton, &tempoDetailsButton, &timeSigButton, &clickButton,
                          &countInButton, &loopStartButton, &loopEndButton,
                          &loopToggleButton, &loopClearButton, &exportButton })
        button->setLookAndFeel(&transportLF);
    zoomLabel.setFont(juce::Font(juce::FontOptions(14.0f)));
    arrowButton.setTooltip(juce::CharPointer_UTF8(u8"選択ツール"));
    splitButton.setTooltip(juce::CharPointer_UTF8(u8"分割ツール"));
}

TransportBarComponent::~TransportBarComponent()
{
    stopTimer();
    recordButton.setLookAndFeel(nullptr);
    playButton.setLookAndFeel(nullptr);
    stopButton.setLookAndFeel(nullptr);
    for (auto* button : { &tapButton, &tempoDetailsButton, &timeSigButton, &clickButton,
                          &countInButton, &loopStartButton, &loopEndButton,
                          &loopToggleButton, &loopClearButton, &exportButton })
        button->setLookAndFeel(nullptr);
}

void TransportBarComponent::updateZoomLabelText()
{
    const int percent = juce::roundToInt(
        (zoomSlider.getValue() / 50.0) * 100.0);
    zoomLabel.setText(
        editingToolbar.getWidth() < 1000
            ? juce::String(percent) + "%"
            : juce::String(juce::CharPointer_UTF8(u8"表示 "))
                  + juce::String(percent) + "%",
        juce::dontSendNotification);
}

void TransportBarComponent::updateLoopControls()
{
    if (audioEngine == nullptr)
        return;
    const auto settings = audioEngine->getProjectDawSettings();
    loopStartButton.setButtonText(
        settings.hasLoopStart
            ? "A " + formatTime(settings.loopStartSeconds)
            : "A");
    loopEndButton.setButtonText(
        settings.hasLoopEnd
            ? "B " + formatTime(settings.loopEndSeconds)
            : "B");
    loopToggleButton.setColour(
        juce::TextButton::buttonColourId,
        settings.loopEnabled ? UiTheme::success : UiTheme::controlSurface);
    loopToggleButton.setButtonText(
        settings.loopEnabled ? juce::CharPointer_UTF8(u8"ループ ON")
                             : juce::CharPointer_UTF8(u8"ループ"));
}

void TransportBarComponent::setAudioEngine(AudioEngine* engine)
{
    audioEngine = engine;
    syncFromEngine();

    if (audioEngine != nullptr)
        updatePlaybackState(audioEngine->getPlaybackState());
}

void TransportBarComponent::syncFromEngine()
{
    // プロジェクト読込が別スレッドから完了した場合でも、画面部品は必ず
    // メッセージスレッド上で更新する。
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager != nullptr && !messageManager->isThisTheMessageThread())
    {
        juce::Component::SafePointer<TransportBarComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis]() mutable {
            if (safeThis != nullptr)
                safeThis->syncFromEngine();
        });
        return;
    }

    if (audioEngine == nullptr)
        return;

    auto& metronome = audioEngine->getMetronome();
    bpmValueLabel.setText(juce::String(metronome.getBpm()), juce::dontSendNotification);
    timeSigButton.setButtonText(juce::String(metronome.getNumerator()) + "/"
                                + juce::String(metronome.getDenominator())
                                + juce::String(juce::CharPointer_UTF8(u8" \u25BC")));

    clickButton.setColour(juce::TextButton::buttonColourId,
        metronome.getEnabled() ? UiTheme::success : UiTheme::controlSurface);
    clickVolSlider.setValue(metronome.getVolume(), juce::dontSendNotification);
    masterVolSlider.setValue(audioEngine->getMasterVolume(), juce::dontSendNotification);
    masterVolValue.setText(juce::String(juce::roundToInt(masterVolSlider.getValue() * 100.0)) + " %",
                           juce::dontSendNotification);

    countInButton.setColour(juce::TextButton::buttonColourId,
        audioEngine->isCountInEnabled() ? UiTheme::success : UiTheme::controlSurface);

    const double zoom = juce::jlimit(zoomSlider.getMinimum(), zoomSlider.getMaximum(),
                                      audioEngine->getZoomLevel());
    zoomSlider.setValue(zoom, juce::dontSendNotification);
    updateZoomLabelText();

    const auto cursorMode = audioEngine->getCursorMode();
    arrowButton.setToggleState(cursorMode == CursorMode::Arrow, juce::dontSendNotification);
    handButton.setToggleState(cursorMode == CursorMode::Hand, juce::dontSendNotification);
    splitButton.setToggleState(cursorMode == CursorMode::Split, juce::dontSendNotification);
    updateLoopControls();
    repaint();
}

void TransportBarComponent::showStatusMessage(const juce::String& message, bool isError)
{
    // オーディオ設定の失敗など、呼び出し元がメッセージスレッド以外でも安全に表示する。
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager != nullptr && !messageManager->isThisTheMessageThread())
    {
        juce::Component::SafePointer<TransportBarComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, message, isError]() mutable {
            if (safeThis != nullptr)
                safeThis->showStatusMessage(message, isError);
        });
        return;
    }

    stopTimer();

    if (message.isEmpty())
    {
        statusLabel.setVisible(false);
        return;
    }

    const auto background = isError ? juce::Colour(0xff4a1c24) : juce::Colour(0xff123c35);
    const auto outline = isError ? UiTheme::danger : UiTheme::success;

    statusLabel.setText(message, juce::dontSendNotification);
    statusLabel.setColour(juce::Label::backgroundColourId, background);
    statusLabel.setColour(juce::Label::outlineColourId, outline);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    statusLabel.setVisible(true);
    statusLabel.toFront(false);

    startTimer(isError ? 6000 : 4000);
}

void TransportBarComponent::timerCallback()
{
    stopTimer();
    statusLabel.setVisible(false);
}

void TransportBarComponent::updateTimeDisplay(double currentTime, double duration)
{
    const int milliseconds = static_cast<int>(std::floor(juce::jmax(0.0, currentTime) * 1000.0)) % 1000;
    timeLabel.setText(formatTime(currentTime) + "." + juce::String(milliseconds).paddedLeft('0', 3),
                      juce::dontSendNotification);
    durationLabel.setText("/ " + formatTime(duration), juce::dontSendNotification);
}

void TransportBarComponent::updatePlaybackState(PlaybackState state)
{
    playbackLabel.setText(
        state == PlaybackState::Playing ? juce::CharPointer_UTF8(u8"再生中")
        : state == PlaybackState::Recording ? juce::CharPointer_UTF8(u8"録音中")
        : state == PlaybackState::CountIn ? juce::CharPointer_UTF8(u8"カウントイン")
        : juce::CharPointer_UTF8(u8"停止中"), juce::dontSendNotification);
    playbackLabel.setColour(juce::Label::textColourId,
        state == PlaybackState::Recording ? UiTheme::danger
        : state == PlaybackState::CountIn ? UiTheme::warning : UiTheme::textSecondary);
    switch (state)
    {
        case PlaybackState::Playing:
            playButton.setButtonText(juce::CharPointer_UTF8(u8"Ⅱ 一時停止")); // 一時停止アイコン
            recordButton.setEnabled(false);
            break;
            
        case PlaybackState::Recording:
            recordButton.setButtonText(juce::CharPointer_UTF8(u8"■ 停止")); // 停止アイコン
            playButton.setEnabled(false);
            break;
            
        case PlaybackState::CountIn:
            recordButton.setButtonText(juce::CharPointer_UTF8(u8"■ 停止")); // 停止アイコン
            playButton.setEnabled(false);
            break;
            
        case PlaybackState::Stopped:
        default:
            playButton.setButtonText(juce::CharPointer_UTF8(u8"▶ 再生")); // 再生アイコン
            recordButton.setButtonText(juce::CharPointer_UTF8(u8"● 録音")); // 録音アイコン
            playButton.setEnabled(true);
            recordButton.setEnabled(true);
            break;
    }
    
    repaint();
}

int TransportBarComponent::getPreferredHeight(int width) const
{
    return width >= 1340 ? 86 : 116;
}

int TransportBarComponent::getEditingToolbarHeight(int width) const
{
    return width >= 1000 ? 84 : 136;
}

void TransportBarComponent::EditingToolbar::paint(juce::Graphics& g)
{
    g.fillAll(UiTheme::panelBackground);
    g.setColour(UiTheme::borderSoft);
    g.fillRect(0, getHeight() - 1, getWidth(), 1);
}

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(UiTheme::panelBackground);
    g.setColour(UiTheme::borderSoft);
    g.fillRect(0, 0, getWidth(), 1);
}

void TransportBarComponent::resized()
{
    auto content = getLocalBounds().reduced(14, 10);
    constexpr int centreWidth = 446;
    constexpr int sectionGap = 12;
    auto centre = content.withSizeKeepingCentre(centreWidth, 64);
    auto left = content.withRight(centre.getX() - sectionGap);
    auto right = content.withLeft(centre.getRight() + sectionGap);

    // 左右の内容量にかかわらず、時間と再生操作のまとまりは画面中央に固定する。
    auto clock = centre.removeFromLeft(132);
    playbackLabel.setBounds(clock.removeFromTop(18));
    timeLabel.setBounds(clock.removeFromTop(28));
    durationLabel.setBounds(clock);
    centre.removeFromLeft(12);
    recordButton.setBounds(centre.removeFromLeft(88).withSizeKeepingCentre(88, 48));
    centre.removeFromLeft(8);
    playButton.setBounds(centre.removeFromLeft(110).withSizeKeepingCentre(110, 48));
    centre.removeFromLeft(8);
    stopButton.setBounds(centre.removeFromLeft(88).withSizeKeepingCentre(88, 48));

    auto tempo = left.withHeight(48).withY(left.getCentreY() - 24);
    const bool twoRows = getWidth() < 1340;
    if (twoRows)
        tempo.setY(left.getY());
    auto bpm = tempo.removeFromLeft(50);
    bpmTitleLabel.setBounds(bpm.removeFromTop(18));
    bpmValueLabel.setBounds(bpm);
    tempo.removeFromLeft(6);
    tempoDetailsButton.setBounds(tempo.removeFromLeft(28));
    tempo.removeFromLeft(8);
    clickButton.setBounds(tempo.removeFromLeft(82));
    tempo.removeFromLeft(8);
    countInButton.setBounds(tempo.removeFromLeft(112));
    tempo.removeFromLeft(8);
    if (twoRows)
    {
        auto volume = left.withHeight(36).withY(left.getY() + 54);
        clickVolLabel.setBounds(volume.removeFromLeft(108));
        clickVolSlider.setBounds(volume);
    }
    else
    {
        clickVolLabel.setBounds(tempo.removeFromTop(20));
        clickVolSlider.setBounds(tempo);
    }

    auto master = right.withSizeKeepingCentre(juce::jmin(200, right.getWidth()), 54);
    master.setRight(right.getRight());
    auto title = master.removeFromTop(22);
    masterVolValue.setBounds(title.removeFromRight(64));
    masterVolLabel.setBounds(title);
    masterVolSlider.setBounds(master);
}

void TransportBarComponent::layoutEditingToolbar()
{
    auto content = editingToolbar.getLocalBounds().reduced(14, 8);
    statusLabel.setBounds(content.removeFromBottom(20));
    content.removeFromBottom(4);
    auto row = content.removeFromTop(48);
    constexpr int gap = 6;
    for (auto* button : { &arrowButton, &handButton, &splitButton })
    {
        button->setBounds(row.removeFromLeft(78));
        row.removeFromLeft(gap);
    }
    row.removeFromLeft(8);
    for (auto* button : { &loopStartButton, &loopEndButton })
    {
        button->setBounds(row.removeFromLeft(90));
        row.removeFromLeft(gap);
    }
    loopToggleButton.setBounds(row.removeFromLeft(96));
    row.removeFromLeft(gap);
    loopClearButton.setBounds(row.removeFromLeft(60));
    row.removeFromLeft(12);

    if (editingToolbar.getWidth() < 1000)
    {
        content.removeFromTop(4);
        row = content.removeFromTop(48);
    }
    exportButton.setBounds(row.removeFromRight(108));
    row.removeFromRight(12);
    zoomLabel.setBounds(row.removeFromRight(86));
    zoomSlider.setBounds(row.withSizeKeepingCentre(juce::jmin(200, row.getWidth()), 40));
    updateZoomLabelText();
}
