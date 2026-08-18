#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "daisysp.h"

#include <array>

//==============================================================================
/** Natorsynth: a JUCE port of the SQUARE_SAW_LFO patch from the
    Waylinator Daisy Pod hardware synth firmware (pod/Midi/Midi.cpp). Left
    voices are a square wave, right voices are a saw wave, and each side has
    its own sine LFO doing subtle pitch vibrato (a few cents, slow) -
    processed once per audio sample and shared across every voice on that
    side, exactly like the firmware. Reuses the exact same DaisySP
    objects (Oscillator/Svf/Adsr/Chorus/DelayLine) as the firmware, plus the
    same custom DCBlocker/fade-in/pulse-width-key-tracking/filter-tracking
    math, so it should sound identical at the default settings.

    Unlike the hardware (which only exposes attack/release, not
    decay/sustain, and has no per-side waveform choice or LFO knobs at all),
    every one of these is a real editable parameter here - they just default
    to the exact values the firmware hardcodes, so out of the box it matches
    the Pod exactly.
 */
class NatorsynthAudioProcessor : public juce::AudioProcessor
{
public:
    //==============================================================================
    NatorsynthAudioProcessor();
    ~NatorsynthAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // 0=Sine 1=Square 2=Saw, matches the firmware's Voice::SetShape(). Default
    // Left=Square, Right=Saw is what SQUARE_SAW_LFO hardcodes on real hardware.
    juce::AudioParameterChoice* leftWaveformParam = nullptr;
    juce::AudioParameterChoice* rightWaveformParam = nullptr;

    juce::AudioParameterFloat* chorusAmountParam = nullptr; // hardware knob 1
    juce::AudioParameterFloat* delayAmountParam = nullptr;  // hardware knob 2
    juce::AudioParameterFloat* pulseWidthParam = nullptr;   // base PW; key/velocity tracking still applies on top

    juce::AudioParameterFloat* ampAttackParam = nullptr;
    juce::AudioParameterFloat* ampDecayParam = nullptr;
    juce::AudioParameterFloat* ampSustainParam = nullptr;
    juce::AudioParameterFloat* ampReleaseParam = nullptr;

    juce::AudioParameterFloat* filterAttackParam = nullptr;
    juce::AudioParameterFloat* filterDecayParam = nullptr;
    juce::AudioParameterFloat* filterSustainParam = nullptr;
    juce::AudioParameterFloat* filterReleaseParam = nullptr;

    // Also driven by MIDI CC1 (mod wheel), matching the firmware's
    // globalFilterEnvAmount - sweeping the mod wheel moves this same value.
    juce::AudioParameterFloat* filterEnvAmountParam = nullptr;

    juce::AudioParameterFloat* lfoDepthLeftParam = nullptr;  // cents
    juce::AudioParameterFloat* lfoDepthRightParam = nullptr; // cents
    juce::AudioParameterFloat* lfoRateLeftParam = nullptr;   // Hz
    juce::AudioParameterFloat* lfoRateRightParam = nullptr;  // Hz

    juce::AudioParameterFloat* outputGainParam = nullptr;

    // Pitch bend range, in semitones, up and down independently. The
    // firmware hardcodes a symmetric +/-7 semitones for this patch
    // (kPitchBendRange) - both default to 7 here so the default sound
    // matches, but unlike hardware they're independently adjustable.
    juce::AudioParameterFloat* pitchBendUpParam = nullptr;
    juce::AudioParameterFloat* pitchBendDownParam = nullptr;

    juce::MidiKeyboardState keyboardState;

private:
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NatorsynthAudioProcessor)

    // Port of the original firmware's custom DC-blocking one-pole highpass
    // (not part of DaisySP - Midi.cpp defines its own DCBlocker class).
    struct DCBlocker
    {
        void init (float r = 0.995f) { R = r; x1 = 0.0f; y1 = 0.0f; }
        float process (float input)
        {
            float output = input - x1 + R * y1;
            x1 = input;
            y1 = output;
            return output;
        }
        float x1 = 0.0f, y1 = 0.0f, R = 0.995f;
    };

    // Everything a voice needs to reproduce SQUARE_SAW_LFO's per-voice
    // behaviour: oscillator + filter + two ADSRs (amp, filter) + DC blocker,
    // plus pulse-width key/velocity tracking and per-sample LFO pitch mod.
    struct Voice
    {
        daisysp::Oscillator osc;
        daisysp::Svf filt;
        daisysp::Adsr env;
        daisysp::Adsr filterEnv;
        DCBlocker dcBlocker;

        bool active = false;
        bool gate = false;
        int note = -1;
        bool isLeft = false;
        uint32_t birthTick = 0;

        float velocityAmp = 1.0f;
        float velocityCutoffBoost = 0.0f;
        float currentFreq = 0.0f;
        float pitchBendSemitones = 0.0f;

        // Base filter cutoff/resonance - NOT a fixed constant on real hardware:
        // the mod wheel calls Voice::SetFilter() directly (globalCutoffLeft/
        // Right = mtof(ccValue)+/-, applied immediately to every active
        // voice), which is a far bigger and more audible effect than the
        // envelope-based modulation below. Defaults match the firmware's
        // boot values (1020 Hz left / 1000 Hz right).
        float baseCutoff = 1000.0f;
        float resonance = 0.1f;

        float fadeInPhase = 0.0f;
        float fadeInInc = 0.0f;
        bool isFadingIn = false;

        int controlCounter = 0;

        void init (float sampleRate, float initialBaseCutoff);
        void setShape (int shape);
        void setFilter (float cutoffHz, float res); // ported from Voice::SetFilter(), driven by mod wheel
        void noteOn (int n, float velocity, bool left, uint32_t tick, float basePw, float currentPitchBend);
        void noteOff (int n);
        void updateFilter (float globalFilterEnvAmount);
        // lfoValue/lfoDepth: this side's shared-per-sample LFO output and
        // depth (semitones), applied to pitch every control-rate tick,
        // matching the firmware's UpdateFrequency().
        float process (float globalFilterEnvAmount, float lfoValue, float lfoDepthSemitones);
    };

    static constexpr size_t kVoicesPerSide = 6; // matches kMaxVoices=12 on hardware
    std::array<Voice, kVoicesPerSide> voicesLeft;
    std::array<Voice, kVoicesPerSide> voicesRight;
    uint32_t voiceTick = 0;

    // Shared LFOs: processed once per sample (not per voice), matching the
    // firmware's AudioCallback - every voice on a side reads the same value.
    daisysp::Oscillator lfoLeft, lfoRight;

    void handleNoteOn (int note, float velocity);
    void handleNoteOff (int note);

    // Ported from UpdatePitchBend()'s generic (non-EWI/chord-mode) branch:
    // applies to every currently-active voice immediately, and newly
    // triggered voices pick up the current value too (a small, deliberate
    // improvement over the firmware, which only updates whichever voice
    // slots happen to be alive at the moment a PitchBend message arrives).
    float currentPitchBendSemitones = 0.0f;
    void applyPitchBend();

    // Mod wheel (CC1) also directly sets every active voice's base filter
    // cutoff (mtof(ccValue), +20Hz on the left side) - ported from the
    // firmware's UpdateFilters(). Applied once per received CC1 message.
    float globalCutoffLeft = 1020.0f;
    float globalCutoffRight = 1000.0f;
    void applyModWheelCutoff();

    // Effects chain, ported from GetChorusDelayPannedSample()/GetChorusSample()
    daisysp::Chorus chorusL, chorusR;
    daisysp::DelayLine<float, 48000 * 2>* delayCenterL = nullptr;
    daisysp::DelayLine<float, 48000 * 2>* delayLeftL = nullptr;
    daisysp::DelayLine<float, 48000 * 2>* delayRightL = nullptr;
    daisysp::DelayLine<float, 48000 * 2>* delayCenterR = nullptr;
    daisysp::DelayLine<float, 48000 * 2>* delayLeftR = nullptr;
    daisysp::DelayLine<float, 48000 * 2>* delayRightR = nullptr;

    double currentSampleRate = 44100.0;
};
