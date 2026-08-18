#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "WayloLookAndFeel.h"

//==============================================================================
class NatorsynthAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit NatorsynthAudioProcessorEditor (NatorsynthAudioProcessor&);
    ~NatorsynthAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    NatorsynthAudioProcessor& audioProcessor;

    WayloLookAndFeel lookAndFeel;
    juce::Image logoImage;
    juce::Image wayloLogoImage;
    static constexpr int logoHeight = 90;

    juce::ComboBox leftWaveformCombo, rightWaveformCombo;
    juce::Label leftWaveformLabel, rightWaveformLabel;

    // All the float-parameter knobs, laid out as a uniform grid.
    static constexpr int kNumKnobs = 19;
    juce::Slider knobs[kNumKnobs];
    juce::Label knobLabels[kNumKnobs];
    juce::AudioParameterFloat* knobParams[kNumKnobs] {};
    juce::String knobNames[kNumKnobs];

    juce::MidiKeyboardComponent keyboardComponent;

    void setupKnob (int index, juce::AudioParameterFloat& param, const juce::String& labelText);
    void setupWaveformCombo (juce::ComboBox& combo, juce::Label& label, const juce::String& labelText, juce::AudioParameterChoice& param);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NatorsynthAudioProcessorEditor)
};
