#include "PluginEditor.h"
#include "BinaryData.h"

//==============================================================================
NatorsynthAudioProcessorEditor::NatorsynthAudioProcessorEditor (NatorsynthAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      keyboardComponent (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lookAndFeel);
    logoImage = juce::ImageCache::getFromMemory (BinaryData::natorsynth_png, BinaryData::natorsynth_pngSize);
    wayloLogoImage = juce::ImageCache::getFromMemory (BinaryData::waylologo_png, BinaryData::waylologo_pngSize);

    setupWaveformCombo (leftWaveformCombo, leftWaveformLabel, "Left Waveform", *audioProcessor.leftWaveformParam);
    setupWaveformCombo (rightWaveformCombo, rightWaveformLabel, "Right Waveform", *audioProcessor.rightWaveformParam);

    int i = 0;
    setupKnob (i++, *audioProcessor.outputGainParam, "Volume");
    setupKnob (i++, *audioProcessor.chorusAmountParam, "Chorus");
    setupKnob (i++, *audioProcessor.delayAmountParam, "Delay");
    setupKnob (i++, *audioProcessor.pulseWidthParam, "Pulse Width");
    setupKnob (i++, *audioProcessor.ampAttackParam, "Amp Attack");
    setupKnob (i++, *audioProcessor.ampDecayParam, "Amp Decay");
    setupKnob (i++, *audioProcessor.ampSustainParam, "Amp Sustain");
    setupKnob (i++, *audioProcessor.ampReleaseParam, "Amp Release");
    setupKnob (i++, *audioProcessor.filterAttackParam, "Filt Attack");
    setupKnob (i++, *audioProcessor.filterDecayParam, "Filt Decay");
    setupKnob (i++, *audioProcessor.filterSustainParam, "Filt Sustain");
    setupKnob (i++, *audioProcessor.filterReleaseParam, "Filt Release");
    setupKnob (i++, *audioProcessor.filterEnvAmountParam, "Filt Env Amt");
    setupKnob (i++, *audioProcessor.lfoDepthLeftParam, "LFO Depth L");
    setupKnob (i++, *audioProcessor.lfoDepthRightParam, "LFO Depth R");
    setupKnob (i++, *audioProcessor.lfoRateLeftParam, "LFO Rate L");
    setupKnob (i++, *audioProcessor.lfoRateRightParam, "LFO Rate R");
    setupKnob (i++, *audioProcessor.pitchBendUpParam, "Bend Up");
    setupKnob (i++, *audioProcessor.pitchBendDownParam, "Bend Down");
    jassert (i == kNumKnobs);

    addAndMakeVisible (keyboardComponent);
    keyboardComponent.setAvailableRange (24, 96);

    setSize (975, 750);
}

NatorsynthAudioProcessorEditor::~NatorsynthAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void NatorsynthAudioProcessorEditor::setupWaveformCombo (juce::ComboBox& combo, juce::Label& label,
    const juce::String& labelText, juce::AudioParameterChoice& param)
{
    for (int idx = 0; idx < param.choices.size(); ++idx)
        combo.addItem (param.choices[idx], idx + 1);
    combo.setSelectedItemIndex (param.getIndex(), juce::dontSendNotification);
    combo.onChange = [&param, &combo] { param = combo.getSelectedItemIndex(); };
    addAndMakeVisible (combo);

    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.attachToComponent (&combo, false);
    addAndMakeVisible (label);
}

void NatorsynthAudioProcessorEditor::setupKnob (int index, juce::AudioParameterFloat& param, const juce::String& labelText)
{
    knobParams[index] = &param;
    knobNames[index] = labelText;

    auto& slider = knobs[index];
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 68, 17);
    slider.setRange (param.range.start, param.range.end, 0.0);
    slider.setValue (param.get(), juce::dontSendNotification);
    slider.onValueChange = [&param, &slider] { param.setValueNotifyingHost (param.convertTo0to1 ((float) slider.getValue())); };
    slider.onDragStart = [&param] { param.beginChangeGesture(); };
    slider.onDragEnd = [&param] { param.endChangeGesture(); };
    addAndMakeVisible (slider);

    auto& label = knobLabels[index];
    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::Font (12.0f));
    addAndMakeVisible (label);
}

void NatorsynthAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    if (logoImage.isValid())
    {
        auto area = getLocalBounds().reduced (10);
        auto logoArea = area.removeFromTop (logoHeight);
        const float aspect = (float) logoImage.getWidth() / (float) logoImage.getHeight();
        const int drawWidth = juce::jmin (logoArea.getWidth(), juce::roundToInt ((float) logoHeight * aspect));
        const int x = logoArea.getCentreX() - drawWidth / 2;
        g.drawImage (logoImage, x, logoArea.getY(), drawWidth, logoHeight, 0, 0, logoImage.getWidth(), logoImage.getHeight());
    }

    if (wayloLogoImage.isValid())
    {
        constexpr int badgeHeight = 60;
        const float aspect = (float) wayloLogoImage.getWidth() / (float) wayloLogoImage.getHeight();
        const int badgeWidth = juce::roundToInt ((float) badgeHeight * aspect);
        const int x = getWidth() - badgeWidth - 11;
        const int y = 11;
        g.drawImage (wayloLogoImage, x, y, badgeWidth, badgeHeight, 0, 0, wayloLogoImage.getWidth(), wayloLogoImage.getHeight());
    }
}

void NatorsynthAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (11);
    area.removeFromTop (logoHeight);

    area.removeFromTop (20); // room for waveform combo labels
    auto waveRow = area.removeFromTop (24);
    leftWaveformCombo.setBounds (waveRow.removeFromLeft (waveRow.getWidth() / 2).reduced (5, 0));
    rightWaveformCombo.setBounds (waveRow.reduced (5, 0));

    area.removeFromTop (11);

    constexpr int kCols = 5;
    constexpr int kRows = (kNumKnobs + kCols - 1) / kCols;
    constexpr int kCellHeight = 120;
    const int knobAreaHeight = kCellHeight * kRows;
    auto knobArea = area.removeFromTop (knobAreaHeight);
    const int colWidth = knobArea.getWidth() / kCols;

    for (int i = 0; i < kNumKnobs; ++i)
    {
        const int row = i / kCols;
        const int col = i % kCols;
        juce::Rectangle<int> cell (knobArea.getX() + col * colWidth, knobArea.getY() + row * kCellHeight, colWidth, kCellHeight);
        knobLabels[i].setBounds (cell.removeFromTop (16));
        knobs[i].setBounds (cell.reduced (8));
    }

    area.removeFromTop (11);
    keyboardComponent.setBounds (area.removeFromTop (105));
}
