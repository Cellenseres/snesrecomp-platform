/* Field-wise conversion; pointer fields, C padding and host output ring are
 * never part of the canonical S-DSP state. */
static int sr_bridge_export_state(const Dsp *dsp, uint16_t *words) {
    SrSnesDspCore core;
    memset(&core, 0, sizeof(core));
    for (unsigned i = 0; i < 128; ++i) core.ram[i] = dsp->ram[i];
    for (unsigned ch = 0; ch < 8; ++ch) {
        core.channel[ch].pitch = dsp->channel[ch].pitch;
        core.channel[ch].pitchCounter = dsp->channel[ch].pitchCounter;
        core.channel[ch].pitchModulation = !!dsp->channel[ch].pitchModulation;
        core.channel[ch].srcn = dsp->channel[ch].srcn;
        core.channel[ch].decodeOffset = dsp->channel[ch].decodeOffset;
        core.channel[ch].previousFlags = dsp->channel[ch].previousFlags;
        core.channel[ch].old = dsp->channel[ch].old;
        core.channel[ch].older = dsp->channel[ch].older;
        core.channel[ch].useNoise = !!dsp->channel[ch].useNoise;
        core.channel[ch].rateCounter = dsp->channel[ch].rateCounter;
        core.channel[ch].adsrState = dsp->channel[ch].adsrState;
        core.channel[ch].sustainLevel = dsp->channel[ch].sustainLevel;
        core.channel[ch].useGain = !!dsp->channel[ch].useGain;
        core.channel[ch].gainMode = dsp->channel[ch].gainMode;
        core.channel[ch].directGain = !!dsp->channel[ch].directGain;
        core.channel[ch].gainValue = dsp->channel[ch].gainValue;
        core.channel[ch].gain = dsp->channel[ch].gain;
        core.channel[ch].keyOn = !!dsp->channel[ch].keyOn;
        core.channel[ch].keyOff = !!dsp->channel[ch].keyOff;
        core.channel[ch].sampleOut = dsp->channel[ch].sampleOut;
        core.channel[ch].volumeL = dsp->channel[ch].volumeL;
        core.channel[ch].volumeR = dsp->channel[ch].volumeR;
        core.channel[ch].echoEnable = !!dsp->channel[ch].echoEnable;
        for (unsigned i = 0; i < 19; ++i)
            core.channel[ch].decodeBuffer[i] = dsp->channel[ch].decodeBuffer[i];
        for (unsigned i = 0; i < 4; ++i)
            core.channel[ch].adsrRates[i] = dsp->channel[ch].adsrRates[i];
    }
    core.dirPage = dsp->dirPage;
    core.evenCycle = !!dsp->evenCycle;
    core.mute = !!dsp->mute;
    core.reset = !!dsp->reset;
    core.masterVolumeL = dsp->masterVolumeL;
    core.masterVolumeR = dsp->masterVolumeR;
    core.noiseSample = dsp->noiseSample;
    core.noiseRate = dsp->noiseRate;
    core.noiseCounter = dsp->noiseCounter;
    core.echoWrites = !!dsp->echoWrites;
    core.echoVolumeL = dsp->echoVolumeL;
    core.echoVolumeR = dsp->echoVolumeR;
    core.feedbackVolume = dsp->feedbackVolume;
    core.echoBufferAdr = dsp->echoBufferAdr;
    core.echoDelay = dsp->echoDelay;
    core.echoRemain = dsp->echoRemain;
    core.echoBufferIndex = dsp->echoBufferIndex;
    core.firBufferIndex = dsp->firBufferIndex;
    for (unsigned i = 0; i < 8; ++i) {
        core.firValues[i] = dsp->firValues[i];
        core.firBufferL[i] = dsp->firBufferL[i];
        core.firBufferR[i] = dsp->firBufferR[i];
    }
    return sr_snes_dsp_core_save(&core, words, SR_SNES_DSP_CORE_STATE_WORDS);
}

static int sr_bridge_import_state(Dsp *dsp, const uint16_t *words) {
    SrSnesDspCore core;
    memset(&core, 0, sizeof(core));
    if (!sr_snes_dsp_core_load(&core, words, SR_SNES_DSP_CORE_STATE_WORDS))
        return 0;
    for (unsigned i = 0; i < 128; ++i) dsp->ram[i] = (uint8_t)core.ram[i];
    for (unsigned ch = 0; ch < 8; ++ch) {
        dsp->channel[ch].pitch = core.channel[ch].pitch;
        dsp->channel[ch].pitchCounter = core.channel[ch].pitchCounter;
        dsp->channel[ch].pitchModulation = core.channel[ch].pitchModulation;
        dsp->channel[ch].srcn = core.channel[ch].srcn;
        dsp->channel[ch].decodeOffset = core.channel[ch].decodeOffset;
        dsp->channel[ch].previousFlags = core.channel[ch].previousFlags;
        dsp->channel[ch].old = core.channel[ch].old;
        dsp->channel[ch].older = core.channel[ch].older;
        dsp->channel[ch].useNoise = core.channel[ch].useNoise;
        dsp->channel[ch].rateCounter = core.channel[ch].rateCounter;
        dsp->channel[ch].adsrState = core.channel[ch].adsrState;
        dsp->channel[ch].sustainLevel = core.channel[ch].sustainLevel;
        dsp->channel[ch].useGain = core.channel[ch].useGain;
        dsp->channel[ch].gainMode = core.channel[ch].gainMode;
        dsp->channel[ch].directGain = core.channel[ch].directGain;
        dsp->channel[ch].gainValue = core.channel[ch].gainValue;
        dsp->channel[ch].gain = core.channel[ch].gain;
        dsp->channel[ch].keyOn = core.channel[ch].keyOn;
        dsp->channel[ch].keyOff = core.channel[ch].keyOff;
        dsp->channel[ch].sampleOut = core.channel[ch].sampleOut;
        dsp->channel[ch].volumeL = core.channel[ch].volumeL;
        dsp->channel[ch].volumeR = core.channel[ch].volumeR;
        dsp->channel[ch].echoEnable = core.channel[ch].echoEnable;
        for (unsigned i = 0; i < 19; ++i)
            dsp->channel[ch].decodeBuffer[i] = core.channel[ch].decodeBuffer[i];
        for (unsigned i = 0; i < 4; ++i)
            dsp->channel[ch].adsrRates[i] = core.channel[ch].adsrRates[i];
    }
    dsp->dirPage = core.dirPage;
    dsp->evenCycle = core.evenCycle;
    dsp->mute = core.mute;
    dsp->reset = core.reset;
    dsp->masterVolumeL = core.masterVolumeL;
    dsp->masterVolumeR = core.masterVolumeR;
    dsp->noiseSample = core.noiseSample;
    dsp->noiseRate = core.noiseRate;
    dsp->noiseCounter = core.noiseCounter;
    dsp->echoWrites = core.echoWrites;
    dsp->echoVolumeL = core.echoVolumeL;
    dsp->echoVolumeR = core.echoVolumeR;
    dsp->feedbackVolume = core.feedbackVolume;
    dsp->echoBufferAdr = core.echoBufferAdr;
    dsp->echoDelay = core.echoDelay;
    dsp->echoRemain = core.echoRemain;
    dsp->echoBufferIndex = core.echoBufferIndex;
    dsp->firBufferIndex = core.firBufferIndex;
    for (unsigned i = 0; i < 8; ++i) {
        dsp->firValues[i] = core.firValues[i];
        dsp->firBufferL[i] = core.firBufferL[i];
        dsp->firBufferR[i] = core.firBufferR[i];
    }
    return 1;
}
