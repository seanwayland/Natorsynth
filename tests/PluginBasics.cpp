#include "helpers/test_helpers.h"
#include <PluginProcessor.h>
#include <memory>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

TEST_CASE ("one is equal to one", "[dummy]")
{
    REQUIRE (1 == 1);
}

TEST_CASE ("Plugin instance", "[instance]")
{
    auto testPlugin = std::make_unique<NatorsynthAudioProcessor>();

    SECTION ("name")
    {
        CHECK_THAT (testPlugin->getName().toStdString(),
            Catch::Matchers::Equals ("Natorsynth"));
    }
}


TEST_CASE ("SQUARE_SAW_LFO renders audible, finite audio on note-on", "[dsp]")
{
    auto testPlugin = std::make_unique<NatorsynthAudioProcessor>();
    testPlugin->prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

    testPlugin->processBlock (buffer, midi);
    midi.clear();

    double sumSquares = 0.0;
    bool allFinite = true;

    // Render a few more blocks (past the 5ms fade-in) and accumulate energy.
    for (int block = 0; block < 20; ++block)
    {
        testPlugin->processBlock (buffer, midi);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getReadPointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                if (! std::isfinite (data[i]))
                    allFinite = false;
                sumSquares += (double) data[i] * (double) data[i];
            }
        }
    }

    CHECK (allFinite);
    CHECK (sumSquares > 0.0);
}

TEST_CASE ("CC1 (mod wheel) audibly changes the note-on attack transient", "[dsp]")
{
    // filter_env's sustain level is 0.01 (near-silent) on the real hardware,
    // so the mod wheel's effect is only really audible right at note-on,
    // as a brightness change in the attack transient - not as a continuous
    // sweep while a note is held. This test isolates just that transient.
    auto renderAttackHfEnergy = [] (int ccValue) -> double
    {
        auto plugin = std::make_unique<NatorsynthAudioProcessor>();
        plugin->prepareToPlay (48000.0, 64);

        juce::AudioBuffer<float> buffer (2, 64);
        juce::MidiBuffer midi;
        if (ccValue >= 0)
            midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, ccValue), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        plugin->processBlock (buffer, midi);
        midi.clear();

        // A high-frequency-content proxy: sum of |x[n] - x[n-1]| over the
        // first block (the attack transient), which rises when the filter
        // cutoff is briefly pushed higher by CC1.
        double hf = 0.0;
        auto* data = buffer.getReadPointer (0);
        for (int i = 1; i < buffer.getNumSamples(); ++i)
            hf += std::abs (data[i] - data[i - 1]);
        return hf;
    };

    const double hfNoModWheel = renderAttackHfEnergy (0);
    const double hfFullModWheel = renderAttackHfEnergy (127);

    CHECK (hfFullModWheel > hfNoModWheel);
}

TEST_CASE ("CC1 (mod wheel) directly raises the base filter cutoff, not just the envelope amount", "[dsp]")
{
    // Ported from the firmware's UpdateFilters(): mod wheel directly sets
    // every active voice's base cutoff (mtof(ccValue)), independent of the
    // (much smaller) envelope-based modulation tested above. This is the
    // dominant effect a player actually hears. Compare two fresh instances
    // at the same block index (rather than the same instance over time) so
    // the amp envelope's own decay can't confound the measurement.
    auto renderHfEnergyAtBlock10 = [] (int modWheelValueBeforeNoteOn) -> double
    {
        auto plugin = std::make_unique<NatorsynthAudioProcessor>();
        plugin->prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        if (modWheelValueBeforeNoteOn >= 0)
            midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, modWheelValueBeforeNoteOn), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
        plugin->processBlock (buffer, midi);
        midi.clear();

        for (int i = 0; i < 9; ++i)
            plugin->processBlock (buffer, midi);

        plugin->processBlock (buffer, midi);
        double hf = 0.0;
        auto* data = buffer.getReadPointer (0);
        for (int i = 1; i < buffer.getNumSamples(); ++i)
            hf += std::abs (data[i] - data[i - 1]);
        return hf;
    };

    const double hfNoModWheel = renderHfEnergyAtBlock10 (0);
    const double hfFullModWheel = renderHfEnergyAtBlock10 (127);

    CHECK (hfFullModWheel > hfNoModWheel * 1.5); // should be a large, obvious jump
}

TEST_CASE ("Pitch bend raises/lowers pitch by the configured up/down range", "[dsp]")
{
    // Bending up an octave (12 semitones) should roughly double the zero-
    // crossing rate; bending down an octave should roughly halve it. Using
    // zero crossings as a cheap pitch proxy avoids needing an FFT here.
    auto countZeroCrossings = [] (float pitchBendUpSemitones, float pitchBendDownSemitones, int pitchWheelValue) -> int
    {
        auto plugin = std::make_unique<NatorsynthAudioProcessor>();
        plugin->prepareToPlay (48000.0, 2048);
        *plugin->pitchBendUpParam = pitchBendUpSemitones;
        *plugin->pitchBendDownParam = pitchBendDownSemitones;
        // Chorus/delay default to 50% wet, which mixes in their own comb/
        // modulation artifacts and swamps a simple zero-crossing count as a
        // pitch proxy - isolate the raw oscillator+filter signal instead.
        *plugin->chorusAmountParam = 0.0f;
        *plugin->delayAmountParam = 0.0f;

        juce::AudioBuffer<float> buffer (2, 2048);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::pitchWheel (1, pitchWheelValue), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
        plugin->processBlock (buffer, midi);
        midi.clear();
        for (int i = 0; i < 5; ++i)
            plugin->processBlock (buffer, midi); // let the fade-in/control-rate settle

        int crossings = 0;
        auto* data = buffer.getReadPointer (0);
        for (int i = 1; i < buffer.getNumSamples(); ++i)
            if ((data[i - 1] < 0.0f) != (data[i] < 0.0f))
                ++crossings;
        return crossings;
    };

    const int crossingsNoBend = countZeroCrossings (12.0f, 12.0f, 8192);   // center: no bend applied
    const int crossingsBendUp = countZeroCrossings (12.0f, 12.0f, 16383);  // full up: +12 semitones
    const int crossingsBendDown = countZeroCrossings (12.0f, 12.0f, 0);    // full down: -12 semitones

    CHECK (crossingsBendUp > crossingsNoBend);
    CHECK (crossingsBendDown < crossingsNoBend);
}

#ifdef PAMPLEJUCE_IPP
    #include <ipp.h>

TEST_CASE ("IPP version", "[ipp]")
{
    CHECK_THAT (ippsGetLibVersion()->Version, Catch::Matchers::Equals ("2022.0.0 (r0x131e93b0)"));
}
#endif
