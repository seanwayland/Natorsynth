#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MODULE_AVAILABLE_juce_audio_utils
 #include <juce_audio_utils/juce_audio_utils.h>
#endif

// Shared visual identity across the Wayland Audio plugin line: dark
// background, green accent knobs/labels - the same palette WayloUD-Delay's
// original UI used (dark background, lightgreen/yellow labels), applied
// consistently everywhere via one LookAndFeel instead of per-component
// colour calls scattered through each editor.
class WayloLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static const juce::Colour accentGreen;
    static const juce::Colour background;
    static const juce::Colour panel;
    static const juce::Colour outline;

    WayloLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, background);

        setColour (juce::Slider::rotarySliderFillColourId, accentGreen);
        setColour (juce::Slider::rotarySliderOutlineColourId, outline);
        setColour (juce::Slider::thumbColourId, accentGreen);
        setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        setColour (juce::Slider::textBoxOutlineColourId, outline);
        setColour (juce::Slider::textBoxBackgroundColourId, panel);
        setColour (juce::Slider::trackColourId, accentGreen);
        setColour (juce::Slider::backgroundColourId, outline);

        setColour (juce::Label::textColourId, accentGreen);

        setColour (juce::ComboBox::backgroundColourId, panel);
        setColour (juce::ComboBox::textColourId, juce::Colours::white);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::arrowColourId, accentGreen);
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::PopupMenu::textColourId, juce::Colours::white);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accentGreen);

        setColour (juce::TextButton::buttonColourId, panel);
        setColour (juce::TextButton::textColourOffId, accentGreen);
        setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        setColour (juce::TextButton::buttonOnColourId, accentGreen);

        setColour (juce::TextEditor::backgroundColourId, panel);
        setColour (juce::TextEditor::textColourId, juce::Colours::white);
        setColour (juce::TextEditor::outlineColourId, outline);
        setColour (juce::TextEditor::focusedOutlineColourId, accentGreen);

       #if JUCE_MODULE_AVAILABLE_juce_audio_utils
        setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, outline);
        setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accentGreen.withAlpha (0.4f));
        setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, accentGreen.withAlpha (0.7f));
       #endif
    }
};

inline const juce::Colour WayloLookAndFeel::accentGreen { 0xff4caf50 };
inline const juce::Colour WayloLookAndFeel::background { 0xff1a1a1a };
inline const juce::Colour WayloLookAndFeel::panel { 0xff2a2a2a };
inline const juce::Colour WayloLookAndFeel::outline { 0xff3a3a3a };
