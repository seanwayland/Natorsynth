#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace daisysp;

//==============================================================================
// Constants ported directly from the Waylinator Daisy Pod firmware's
// pod/Midi/Midi.cpp, scoped to the SQUARE_SAW_LFO patch (mode 49).
namespace
{
    constexpr float kVelocityCutoffBoost = 1200.0f;
    constexpr float kVelocityPwModAmount = 0.05f;
    // Firmware hardcodes left/right base pulse width as 0.82/0.86 - a fixed
    // +0.04 offset between sides. We expose ONE Pulse Width knob (matching
    // the user's request) and preserve that offset so both sides still move
    // together, sounding identical to hardware at the default 0.82 value.
    constexpr float kBasePwRightOffset = 0.04f;
    constexpr float kPwKeyTrackingAmount = 0.0009f;
    constexpr float kNoteFilterTrackingAmount = 10.0f;
    constexpr float kFadeInTime = 0.005f; // 5ms fade-in, avoids chord-attack clicks
    // SQUARE_SAW_LFO uses a 32-sample control-rate divider (vs the usual 16).
    constexpr int kControlRateDivider = 32;
    constexpr float kResonance = 0.1f;
    constexpr float kFilterEnvAmount = 2000.0f;

    constexpr float kChorusDepthScale = 3.0f;
    constexpr float kChorusSpeedScale = 6.0f;

    // Fixed delay taps: 300ms (center), 470ms (left), 800ms (right), matching
    // the 14400/22560/38400 sample values hardcoded for 48kHz in the firmware.
    constexpr float kDelayCenterMs = 300.0f;
    constexpr float kDelayLeftMs = 470.0f;
    constexpr float kDelayRightMs = 800.0f;
    constexpr float kDelayCenterFeedback = 0.37f;
    constexpr float kDelayLeftFeedback = 0.31f;
    constexpr float kDelayRightFeedback = 0.26f;

    constexpr float kOutputScale = 3.0f; // firmware's fixed output headroom multiplier

    float noteToFreq (float note)
    {
        return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f);
    }
}

//==============================================================================
void NatorsynthAudioProcessor::Voice::init (float sampleRate, float initialBaseCutoff)
{
    osc.Init (sampleRate);
    osc.SetWaveform (Oscillator::WAVE_POLYBLEP_SQUARE);
    osc.SetAmp (1.0f);
    osc.SetPw (0.5f);

    baseCutoff = initialBaseCutoff;
    resonance = kResonance;
    filt.Init (sampleRate);
    filt.SetFreq (baseCutoff);
    filt.SetRes (resonance);

    dcBlocker.init (0.995f);

    // Real firmware defaults (Bank A): amp A=0.035 D=0.9 S=0.9 R=0.032 (0.11
    // is the device-wide setting default, applied by the processor below);
    // filter A=0.035 D=0.0195 S=0.01 R=0.032/0.11. All fully re-appliable at
    // runtime via setEnvelopeTimes(), unlike the hardware which only exposes
    // attack/release.
    env.Init (sampleRate);
    env.SetTime (ADSR_SEG_ATTACK, 0.035f);
    env.SetTime (ADSR_SEG_DECAY, 0.9f);
    env.SetSustainLevel (0.9f);
    env.SetTime (ADSR_SEG_RELEASE, 0.11f);

    filterEnv.Init (sampleRate);
    filterEnv.SetTime (ADSR_SEG_ATTACK, 0.035f);
    filterEnv.SetTime (ADSR_SEG_DECAY, 0.0195f);
    filterEnv.SetSustainLevel (0.01f);
    filterEnv.SetTime (ADSR_SEG_RELEASE, 0.11f);

    fadeInInc = 1.0f / (kFadeInTime * sampleRate);
}

void NatorsynthAudioProcessor::Voice::setShape (int shape)
{
    switch (shape)
    {
        case 0: osc.SetWaveform (Oscillator::WAVE_SIN); break;
        case 2: osc.SetWaveform (Oscillator::WAVE_POLYBLEP_SAW); break;
        case 1:
        default: osc.SetWaveform (Oscillator::WAVE_POLYBLEP_SQUARE); break;
    }
}

void NatorsynthAudioProcessor::Voice::setFilter (float cutoffHz, float res)
{
    baseCutoff = cutoffHz;
    resonance = res;
    updateFilter (0.0f);
}

void NatorsynthAudioProcessor::Voice::noteOn (int n, float velocity, bool left, uint32_t tick, float basePw, float currentPitchBend)
{
    if (active && gate)
        gate = false;

    note = n;
    active = true;
    gate = true;
    isLeft = left;
    birthTick = tick;
    pitchBendSemitones = currentPitchBend;

    velocityAmp = velocity / 127.0f;
    velocityCutoffBoost = velocityAmp * kVelocityCutoffBoost;

    const float sidePw = left ? basePw : (basePw + kBasePwRightOffset);
    const float keyTracking = (127.0f - (float) n) * kPwKeyTrackingAmount;
    const float velMod = (1.0f - velocityAmp) * kVelocityPwModAmount;
    const float pw = juce::jlimit (0.01f, 0.99f, sidePw + keyTracking + velMod);
    osc.SetPw (pw);
    osc.Reset();
    currentFreq = noteToFreq ((float) n + pitchBendSemitones);
    osc.SetFreq (currentFreq);

    fadeInPhase = 0.0f;
    isFadingIn = true;

    updateFilter (0.0f);
}

void NatorsynthAudioProcessor::Voice::noteOff (int n)
{
    if (note == n)
        gate = false;
}

void NatorsynthAudioProcessor::Voice::updateFilter (float globalFilterEnvAmount)
{
    const float filterEnvVal = filterEnv.Process (gate);
    const float noteTracking = (127.0f - (float) note) * kNoteFilterTrackingAmount;
    const float envModulation = filterEnvVal * globalFilterEnvAmount * kFilterEnvAmount;
    const float modulatedCutoff = baseCutoff + velocityCutoffBoost - noteTracking + 0.035f * envModulation;
    filt.SetFreq (juce::jlimit (20.0f, 20000.0f, modulatedCutoff));
    filt.SetRes (resonance);
}

float NatorsynthAudioProcessor::Voice::process (float globalFilterEnvAmount, float lfoValue, float lfoDepthSemitones)
{
    if (! active)
        return 0.0f;

    const float rawAmp = env.Process (gate);
    if (rawAmp <= 0.0001f && ! gate)
    {
        active = false;
        isFadingIn = false;
        return 0.0f;
    }

    // envCurveEnabled is true by default for SQUARE_SAW_LFO: pure quartic curve.
    const float amp = rawAmp * rawAmp * rawAmp * rawAmp;

    if (++controlCounter >= kControlRateDivider)
    {
        controlCounter = 0;
        updateFilter (globalFilterEnvAmount);

        // Matches the firmware's UpdateFrequency(): re-derive frequency from
        // note + pitch bend + shared per-sample LFO value each control-rate tick.
        const float modulatedFreq = noteToFreq ((float) note + pitchBendSemitones + lfoValue * lfoDepthSemitones);
        currentFreq = modulatedFreq;
        osc.SetFreq (currentFreq);
    }

    float sig = osc.Process() * amp * velocityAmp;

    if (isFadingIn)
    {
        sig *= fadeInPhase;
        fadeInPhase += fadeInInc;
        if (fadeInPhase >= 1.0f)
        {
            fadeInPhase = 1.0f;
            isFadingIn = false;
        }
    }

    filt.Process (sig);
    return dcBlocker.process (filt.Low());
}

//==============================================================================
NatorsynthAudioProcessor::NatorsynthAudioProcessor()
     : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    auto waveChoices = juce::StringArray { "Sine", "Square", "Saw" };
    addParameter (leftWaveformParam = new juce::AudioParameterChoice ("leftWaveform", "Left Oscillator Waveform", waveChoices, 1));  // Square
    addParameter (rightWaveformParam = new juce::AudioParameterChoice ("rightWaveform", "Right Oscillator Waveform", waveChoices, 2)); // Saw

    addParameter (chorusAmountParam = new juce::AudioParameterFloat (
        "chorusAmount", "Chorus Amount", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
    addParameter (delayAmountParam = new juce::AudioParameterFloat (
        "delayAmount", "Delay Amount", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
    addParameter (pulseWidthParam = new juce::AudioParameterFloat (
        "pulseWidth", "Pulse Width", juce::NormalisableRange<float> (0.01f, 0.95f), 0.82f));

    addParameter (ampAttackParam = new juce::AudioParameterFloat (
        "ampAttack", "Amp Attack", juce::NormalisableRange<float> (0.001f, 2.0f, 0.0f, 0.4f), 0.035f));
    addParameter (ampDecayParam = new juce::AudioParameterFloat (
        "ampDecay", "Amp Decay", juce::NormalisableRange<float> (0.001f, 2.0f, 0.0f, 0.4f), 0.9f));
    addParameter (ampSustainParam = new juce::AudioParameterFloat (
        "ampSustain", "Amp Sustain", juce::NormalisableRange<float> (0.0f, 1.0f), 0.9f));
    addParameter (ampReleaseParam = new juce::AudioParameterFloat (
        "ampRelease", "Amp Release", juce::NormalisableRange<float> (0.001f, 2.0f, 0.0f, 0.4f), 0.11f));

    addParameter (filterAttackParam = new juce::AudioParameterFloat (
        "filterAttack", "Filter Attack", juce::NormalisableRange<float> (0.001f, 2.0f, 0.0f, 0.4f), 0.035f));
    addParameter (filterDecayParam = new juce::AudioParameterFloat (
        "filterDecay", "Filter Decay", juce::NormalisableRange<float> (0.001f, 2.0f, 0.0f, 0.4f), 0.0195f));
    addParameter (filterSustainParam = new juce::AudioParameterFloat (
        "filterSustain", "Filter Sustain", juce::NormalisableRange<float> (0.0f, 1.0f), 0.01f));
    addParameter (filterReleaseParam = new juce::AudioParameterFloat (
        "filterRelease", "Filter Release", juce::NormalisableRange<float> (0.001f, 2.0f, 0.0f, 0.4f), 0.11f));

    addParameter (filterEnvAmountParam = new juce::AudioParameterFloat (
        "filterEnvAmount", "Filter Env Amount", juce::NormalisableRange<float> (0.0f, 2.0f), 0.0f));

    // Hardware defaults: Left 15 cents / 1.5 Hz, Right 6 cents / 0.06 Hz
    // (from defaults_generated.h + editor.html's DEFAULT_PARAMS).
    addParameter (lfoDepthLeftParam = new juce::AudioParameterFloat (
        "lfoDepthLeft", "LFO Depth Left", juce::NormalisableRange<float> (0.0f, 20.0f), 15.0f));
    addParameter (lfoDepthRightParam = new juce::AudioParameterFloat (
        "lfoDepthRight", "LFO Depth Right", juce::NormalisableRange<float> (0.0f, 20.0f), 6.0f));
    addParameter (lfoRateLeftParam = new juce::AudioParameterFloat (
        "lfoRateLeft", "LFO Rate Left", juce::NormalisableRange<float> (0.01f, 2.0f), 1.5f));
    addParameter (lfoRateRightParam = new juce::AudioParameterFloat (
        "lfoRateRight", "LFO Rate Right", juce::NormalisableRange<float> (0.01f, 2.0f), 0.06f));

    addParameter (outputGainParam = new juce::AudioParameterFloat (
        "outputGain", "Output Gain", juce::NormalisableRange<float> (0.0f, 2.0f), 1.0f));

    addParameter (pitchBendUpParam = new juce::AudioParameterFloat (
        "pitchBendUp", "Pitch Bend Up", juce::NormalisableRange<float> (0.0f, 24.0f), 7.0f));
    addParameter (pitchBendDownParam = new juce::AudioParameterFloat (
        "pitchBendDown", "Pitch Bend Down", juce::NormalisableRange<float> (0.0f, 24.0f), 7.0f));

    delayCenterL = new DelayLine<float, 48000 * 2>();
    delayLeftL = new DelayLine<float, 48000 * 2>();
    delayRightL = new DelayLine<float, 48000 * 2>();
    delayCenterR = new DelayLine<float, 48000 * 2>();
    delayLeftR = new DelayLine<float, 48000 * 2>();
    delayRightR = new DelayLine<float, 48000 * 2>();
}

NatorsynthAudioProcessor::~NatorsynthAudioProcessor()
{
    delete delayCenterL;
    delete delayLeftL;
    delete delayRightL;
    delete delayCenterR;
    delete delayLeftR;
    delete delayRightR;
}

//==============================================================================
const juce::String NatorsynthAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NatorsynthAudioProcessor::acceptsMidi() const { return true; }
bool NatorsynthAudioProcessor::producesMidi() const { return false; }
bool NatorsynthAudioProcessor::isMidiEffect() const { return false; }
double NatorsynthAudioProcessor::getTailLengthSeconds() const { return 2.5; }

int NatorsynthAudioProcessor::getNumPrograms() { return 1; }
int NatorsynthAudioProcessor::getCurrentProgram() { return 0; }
void NatorsynthAudioProcessor::setCurrentProgram (int) {}
const juce::String NatorsynthAudioProcessor::getProgramName (int) { return {}; }
void NatorsynthAudioProcessor::changeProgramName (int, const juce::String&) {}

//==============================================================================
void NatorsynthAudioProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;

    for (auto& v : voicesLeft)
        v.init ((float) sampleRate, globalCutoffLeft);
    for (auto& v : voicesRight)
        v.init ((float) sampleRate, globalCutoffRight);

    for (auto& v : voicesLeft)
        v.setShape (leftWaveformParam->getIndex());
    for (auto& v : voicesRight)
        v.setShape (rightWaveformParam->getIndex());

    lfoLeft.Init ((float) sampleRate);
    lfoLeft.SetWaveform (Oscillator::WAVE_SIN);
    lfoLeft.SetAmp (1.0f);
    lfoRight.Init ((float) sampleRate);
    lfoRight.SetWaveform (Oscillator::WAVE_SIN);
    lfoRight.SetAmp (1.0f);

    chorusL.Init ((float) sampleRate);
    chorusR.Init ((float) sampleRate);

    delayCenterL->Init();
    delayLeftL->Init();
    delayRightL->Init();
    delayCenterR->Init();
    delayLeftR->Init();
    delayRightR->Init();

    delayCenterL->SetDelay ((float) (kDelayCenterMs * 0.001 * sampleRate));
    delayLeftL->SetDelay ((float) (kDelayLeftMs * 0.001 * sampleRate));
    delayRightL->SetDelay ((float) (kDelayRightMs * 0.001 * sampleRate));
    delayCenterR->SetDelay ((float) (kDelayCenterMs * 0.001 * sampleRate));
    delayLeftR->SetDelay ((float) (kDelayLeftMs * 0.001 * sampleRate));
    delayRightR->SetDelay ((float) (kDelayRightMs * 0.001 * sampleRate));
}

void NatorsynthAudioProcessor::releaseResources() {}

bool NatorsynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void NatorsynthAudioProcessor::handleNoteOn (int note, float velocity)
{
    ++voiceTick;

    const float basePw = pulseWidthParam->get();
    const float bend = currentPitchBendSemitones;

    // Voice stealing: prefer an inactive voice, else steal the oldest (lowest birthTick).
    auto trigger = [this, basePw, bend] (std::array<Voice, kVoicesPerSide>& voices, int n, float vel, bool left)
    {
        Voice* target = nullptr;
        for (auto& v : voices)
        {
            if (! v.active)
            {
                target = &v;
                break;
            }
        }
        if (target == nullptr)
        {
            for (auto& v : voices)
                if (target == nullptr || v.birthTick < target->birthTick)
                    target = &v;
        }
        target->noteOn (n, vel, left, voiceTick, basePw, bend);
    };

    trigger (voicesLeft, note, velocity, true);
    trigger (voicesRight, note, velocity, false);
}

void NatorsynthAudioProcessor::handleNoteOff (int note)
{
    auto release = [] (std::array<Voice, kVoicesPerSide>& voices, int n)
    {
        for (auto& v : voices)
            if (v.active && v.gate && v.note == n)
            {
                v.noteOff (n);
                return;
            }
        for (auto& v : voices)
            if (v.active && v.note == n)
            {
                v.noteOff (n);
                return;
            }
    };

    release (voicesLeft, note);
    release (voicesRight, note);
}

void NatorsynthAudioProcessor::applyPitchBend()
{
    for (auto& v : voicesLeft)
        v.pitchBendSemitones = currentPitchBendSemitones;
    for (auto& v : voicesRight)
        v.pitchBendSemitones = currentPitchBendSemitones;
}

void NatorsynthAudioProcessor::applyModWheelCutoff()
{
    // Ported from the firmware's UpdateFilters(): sets EVERY voice's base
    // cutoff immediately, independent of the (much smaller) envelope-based
    // modulation. This is the dominant, obviously-audible mod wheel effect
    // on real hardware.
    for (auto& v : voicesLeft)
        v.setFilter (globalCutoffLeft, kResonance);
    for (auto& v : voicesRight)
        v.setFilter (globalCutoffRight, kResonance);
}

void NatorsynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    keyboardState.processNextMidiBuffer (midiMessages, 0, buffer.getNumSamples(), true);

    // Oscillator waveform choice can change live (host automation or the
    // combo box); cheap enough to just re-apply every block.
    for (auto& v : voicesLeft)
        v.setShape (leftWaveformParam->getIndex());
    for (auto& v : voicesRight)
        v.setShape (rightWaveformParam->getIndex());

    // Envelope times are fully editable here (unlike the hardware, which
    // only exposes attack/release) - re-applied every block so knob moves
    // are heard promptly. DaisySP's SetTime()/SetSustainLevel() just update
    // internal coefficients, so this is cheap.
    const float ampA = ampAttackParam->get();
    const float ampD = ampDecayParam->get();
    const float ampS = ampSustainParam->get();
    const float ampR = ampReleaseParam->get();
    const float filtA = filterAttackParam->get();
    const float filtD = filterDecayParam->get();
    const float filtS = filterSustainParam->get();
    const float filtR = filterReleaseParam->get();
    auto applyEnvelopes = [&] (Voice& v)
    {
        v.env.SetTime (ADSR_SEG_ATTACK, ampA);
        v.env.SetTime (ADSR_SEG_DECAY, ampD);
        v.env.SetSustainLevel (ampS);
        v.env.SetTime (ADSR_SEG_RELEASE, ampR);
        v.filterEnv.SetTime (ADSR_SEG_ATTACK, filtA);
        v.filterEnv.SetTime (ADSR_SEG_DECAY, filtD);
        v.filterEnv.SetSustainLevel (filtS);
        v.filterEnv.SetTime (ADSR_SEG_RELEASE, filtR);
    };
    for (auto& v : voicesLeft) applyEnvelopes (v);
    for (auto& v : voicesRight) applyEnvelopes (v);

    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
            handleNoteOn (msg.getNoteNumber(), msg.getFloatVelocity() * 127.0f);
        else if (msg.isNoteOff())
            handleNoteOff (msg.getNoteNumber());
        else if (msg.isControllerOfType (1)) // CC1: mod wheel
        {
            // Two independent effects, both ported from the firmware's CC1
            // handler: the envelope-amount knob (subtle, attack-transient
            // only - see the filter ADSR's low sustain), and a direct,
            // immediate cutoff sweep on every active voice (the dominant,
            // clearly-audible effect on real hardware).
            filterEnvAmountParam->setValueNotifyingHost (msg.getControllerValue() / 127.0f);

            const float ccValue = (float) msg.getControllerValue();
            globalCutoffLeft = noteToFreq (ccValue) + 20.0f;
            globalCutoffRight = noteToFreq (ccValue);
            applyModWheelCutoff();
        }
        else if (msg.isPitchWheel())
        {
            // Ported from the firmware's PitchBend handler's generic branch
            // (semitones = norm * kPitchBendRange, symmetric +/-7). Here the
            // up/down range is independently adjustable via two parameters,
            // both defaulting to 7 so the out-of-the-box range matches.
            const float norm = ((float) msg.getPitchWheelValue() - 8192.0f) / 8192.0f; // -1..~1
            const float bendUp = pitchBendUpParam->get();
            const float bendDown = pitchBendDownParam->get();
            currentPitchBendSemitones = norm >= 0.0f ? norm * bendUp : norm * bendDown;
            applyPitchBend();
        }
    }

    // Ported from ApplySharedChorusDelayControls(): knob 1 drives both the
    // chorus dry/wet and its LFO depth/rate; knob 2 drives the fixed-delay wet.
    const float k1 = chorusAmountParam->get();
    const float k2 = delayAmountParam->get();
    const float drywet = k1;
    chorusL.SetLfoDepth ((4.0f + k1 * 1.1f) * kChorusDepthScale);
    chorusR.SetLfoDepth ((5.0f + k1 * 1.2f) * kChorusDepthScale);
    chorusL.SetLfoFreq (k1 * 0.6f * kChorusSpeedScale);
    chorusR.SetLfoFreq (k1 * 0.7f * kChorusSpeedScale);
    const float chorusDelayWet = k2;
    const float outputGain = outputGainParam->get();
    const float globalFilterEnvAmount = filterEnvAmountParam->get();

    lfoLeft.SetFreq (lfoRateLeftParam->get());
    lfoRight.SetFreq (lfoRateRightParam->get());
    const float lfoDepthLeftSemitones = lfoDepthLeftParam->get() / 100.0f;  // cents -> semitones
    const float lfoDepthRightSemitones = lfoDepthRightParam->get() / 100.0f;

    auto* outL = buffer.getWritePointer (0);
    auto* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : outL;

    for (int n = 0; n < buffer.getNumSamples(); ++n)
    {
        // Shared per-sample LFOs, matching the firmware's AudioCallback
        // (processed once, not per-voice).
        const float lfoLeftValue = lfoLeft.Process();
        const float lfoRightValue = lfoRight.Process();

        float synthLeft = 0.0f, synthRight = 0.0f;
        for (auto& v : voicesLeft)
            synthLeft += v.process (globalFilterEnvAmount, lfoLeftValue, lfoDepthLeftSemitones);
        for (auto& v : voicesRight)
            synthRight += v.process (globalFilterEnvAmount, lfoRightValue, lfoDepthRightSemitones);
        synthLeft /= (float) kVoicesPerSide;
        synthRight /= (float) kVoicesPerSide;

        // --- GetChorusSample() ---
        chorusL.Process (synthLeft);
        chorusR.Process (synthRight);
        float outl = chorusL.GetLeft() * drywet * 2.5f + synthLeft * (0.5f - drywet);
        float outr = chorusR.GetRight() * drywet * 2.5f + synthRight * (0.5f - drywet);

        // --- 3-tap fixed delay, ported from GetChorusDelayPannedSample() ---
        const float delayCenterLVal = delayCenterL->Read();
        const float delayLeftLVal = delayLeftL->Read();
        const float delayRightLVal = delayRightL->Read();
        const float delayCenterRVal = delayCenterR->Read();
        const float delayLeftRVal = delayLeftR->Read();
        const float delayRightRVal = delayRightR->Read();

        delayCenterL->Write ((kDelayCenterFeedback * delayCenterLVal) + synthLeft);
        delayLeftL->Write ((kDelayLeftFeedback * delayLeftLVal) + synthLeft);
        delayRightL->Write ((kDelayRightFeedback * delayRightLVal) + synthLeft);
        delayCenterR->Write ((kDelayCenterFeedback * delayCenterRVal) + synthRight);
        delayLeftR->Write ((kDelayLeftFeedback * delayLeftRVal) + synthRight);
        delayRightR->Write ((kDelayRightFeedback * delayRightRVal) + synthRight);

        const float fixedDelayMixL = delayCenterLVal + delayLeftLVal + 0.5f * delayRightLVal;
        const float fixedDelayMixR = delayCenterRVal + 0.5f * delayLeftRVal + delayRightRVal;

        outl += fixedDelayMixL * chorusDelayWet * 0.3f;
        outr += fixedDelayMixR * chorusDelayWet * 0.3f;

        outL[n] = outl * kOutputScale * outputGain;
        outR[n] = outr * kOutputScale * outputGain;
    }
}

//==============================================================================
bool NatorsynthAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* NatorsynthAudioProcessor::createEditor()
{
    return new NatorsynthAudioProcessorEditor (*this);
}

//==============================================================================
void NatorsynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("NatorsynthState");
    xml.setAttribute ("leftWaveform", leftWaveformParam->getIndex());
    xml.setAttribute ("rightWaveform", rightWaveformParam->getIndex());
    xml.setAttribute ("chorusAmount", (double) chorusAmountParam->get());
    xml.setAttribute ("delayAmount", (double) delayAmountParam->get());
    xml.setAttribute ("pulseWidth", (double) pulseWidthParam->get());
    xml.setAttribute ("ampAttack", (double) ampAttackParam->get());
    xml.setAttribute ("ampDecay", (double) ampDecayParam->get());
    xml.setAttribute ("ampSustain", (double) ampSustainParam->get());
    xml.setAttribute ("ampRelease", (double) ampReleaseParam->get());
    xml.setAttribute ("filterAttack", (double) filterAttackParam->get());
    xml.setAttribute ("filterDecay", (double) filterDecayParam->get());
    xml.setAttribute ("filterSustain", (double) filterSustainParam->get());
    xml.setAttribute ("filterRelease", (double) filterReleaseParam->get());
    xml.setAttribute ("filterEnvAmount", (double) filterEnvAmountParam->get());
    xml.setAttribute ("lfoDepthLeft", (double) lfoDepthLeftParam->get());
    xml.setAttribute ("lfoDepthRight", (double) lfoDepthRightParam->get());
    xml.setAttribute ("lfoRateLeft", (double) lfoRateLeftParam->get());
    xml.setAttribute ("lfoRateRight", (double) lfoRateRightParam->get());
    xml.setAttribute ("outputGain", (double) outputGainParam->get());
    xml.setAttribute ("pitchBendUp", (double) pitchBendUpParam->get());
    xml.setAttribute ("pitchBendDown", (double) pitchBendDownParam->get());
    copyXmlToBinary (xml, destData);
}

void NatorsynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml.get() != nullptr && xml->hasTagName ("NatorsynthState"))
    {
        *leftWaveformParam = xml->getIntAttribute ("leftWaveform", leftWaveformParam->getIndex());
        *rightWaveformParam = xml->getIntAttribute ("rightWaveform", rightWaveformParam->getIndex());
        *chorusAmountParam = (float) xml->getDoubleAttribute ("chorusAmount", chorusAmountParam->get());
        *delayAmountParam = (float) xml->getDoubleAttribute ("delayAmount", delayAmountParam->get());
        *pulseWidthParam = (float) xml->getDoubleAttribute ("pulseWidth", pulseWidthParam->get());
        *ampAttackParam = (float) xml->getDoubleAttribute ("ampAttack", ampAttackParam->get());
        *ampDecayParam = (float) xml->getDoubleAttribute ("ampDecay", ampDecayParam->get());
        *ampSustainParam = (float) xml->getDoubleAttribute ("ampSustain", ampSustainParam->get());
        *ampReleaseParam = (float) xml->getDoubleAttribute ("ampRelease", ampReleaseParam->get());
        *filterAttackParam = (float) xml->getDoubleAttribute ("filterAttack", filterAttackParam->get());
        *filterDecayParam = (float) xml->getDoubleAttribute ("filterDecay", filterDecayParam->get());
        *filterSustainParam = (float) xml->getDoubleAttribute ("filterSustain", filterSustainParam->get());
        *filterReleaseParam = (float) xml->getDoubleAttribute ("filterRelease", filterReleaseParam->get());
        *filterEnvAmountParam = (float) xml->getDoubleAttribute ("filterEnvAmount", filterEnvAmountParam->get());
        *lfoDepthLeftParam = (float) xml->getDoubleAttribute ("lfoDepthLeft", lfoDepthLeftParam->get());
        *lfoDepthRightParam = (float) xml->getDoubleAttribute ("lfoDepthRight", lfoDepthRightParam->get());
        *lfoRateLeftParam = (float) xml->getDoubleAttribute ("lfoRateLeft", lfoRateLeftParam->get());
        *lfoRateRightParam = (float) xml->getDoubleAttribute ("lfoRateRight", lfoRateRightParam->get());
        *outputGainParam = (float) xml->getDoubleAttribute ("outputGain", outputGainParam->get());
        *pitchBendUpParam = (float) xml->getDoubleAttribute ("pitchBendUp", pitchBendUpParam->get());
        *pitchBendDownParam = (float) xml->getDoubleAttribute ("pitchBendDown", pitchBendDownParam->get());
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NatorsynthAudioProcessor();
}
