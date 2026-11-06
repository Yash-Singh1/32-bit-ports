#include <AudioToolbox/AudioToolbox.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The deprecated Component Manager typedefs are absent from modern i386 SDK
   headers; their handles are the same four-byte opaque values used by TFU/Source. */
extern void *FindNextComponent(void *, const AudioComponentDescription *);
extern int OpenAComponent(void *, AudioUnit *);
extern int CloseComponent(AudioUnit);
extern void *alcCaptureOpenDevice(const char *, unsigned, int, int);
extern char alcCaptureCloseDevice(void *);
extern int alcGetError(void *);
static volatile unsigned calls;
static volatile int callback_error;
static OSStatus input(void *refcon, AudioUnitRenderActionFlags *flags,
    const AudioTimeStamp *time, UInt32 bus, UInt32 frames, AudioBufferList *data)
{
    if (refcon != (void *)0x12345678 || !flags || !time || bus || frames != 128 ||
        !data || data->mNumberBuffers != 1 || data->mBuffers[0].mDataByteSize < frames * 4)
        callback_error = 1;
    if (data && data->mNumberBuffers)
        memset(data->mBuffers[0].mData, 0, data->mBuffers[0].mDataByteSize);
    ++calls;
    return noErr;
}

/* Enable a HAL input without starting capture. On Mel/Rosetta the bridge
   must reject initialization before native microphone buffers are allocated. */
int check_mel_microphone_guard(void)
{
    AudioComponentDescription desc = {kAudioUnitType_Output,
        kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    void *component = FindNextComponent(NULL, &desc);
    AudioUnit unit = NULL;
    if (!component || OpenAComponent(component, &unit)) return -290;
    UInt32 enabled = 1, disabled = 0;
    OSStatus status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Output, 0, &disabled, sizeof(disabled));
    if (!status) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Input, 1, &enabled, sizeof(enabled));
    if (status) { CloseComponent(unit); return -291; }
    status = AudioUnitInitialize(unit);
    if (!status) AudioUnitUninitialize(unit);
    OSStatus closed = CloseComponent(unit);
    return status == kAudioUnitErr_FailedInitialization && !closed ? 0 : -292;
}

int check_audio_unit(void)
{
    if (alcCaptureOpenDevice(NULL, 0, 0, 0) || alcGetError(NULL) != 0xa004 ||
        alcCaptureCloseDevice(NULL) || alcGetError(NULL) != 0xa001 ||
        alcCaptureCloseDevice((void *)0xdeadbeef) || alcGetError(NULL) != 0xa001) return -260;
    AudioComponentDescription desc = {kAudioUnitType_FormatConverter,
        kAudioUnitSubType_AUConverter, kAudioUnitManufacturer_Apple, 0, 0};
    void *component = FindNextComponent(NULL, &desc);
    struct { AudioUnit unit; uint32_t guard; } out = {0, 0x87654321};
    OSStatus open_status = component ? OpenAComponent(component, &out.unit) : -1;
    if (!component || open_status || !out.unit || out.guard != 0x87654321) {
        printf("audio-unit fixture: component=%p open_status=%d unit=%p guard=0x%08x\n",
                component, (int)open_status, out.unit, (unsigned)out.guard);
        fflush(NULL);
        return -261;
    }
    AudioStreamBasicDescription format = {44100, kAudioFormatLinearPCM,
        kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, 4, 1, 4, 2, 16, 0};
    if (AudioUnitSetProperty(out.unit, kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input, 0, &format, sizeof(format)) ||
        AudioUnitSetProperty(out.unit, kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Output, 0, &format, sizeof(format))) return -262;
    struct { AURenderCallbackStruct callback; uint32_t guard; } cb = {{input, (void *)0x12345678}, 0xaabbccdd};
    if (sizeof(cb.callback) != 8) return -263;
    for (int i = 0; i < 300; ++i) {
        if (AudioUnitSetProperty(out.unit, kAudioUnitProperty_SetRenderCallback,
                kAudioUnitScope_Input, 0, &cb.callback, sizeof(cb.callback))) return -264;
    }
    struct { UInt32 size; uint32_t guard; } size = {0, 0x11223344};
    struct { Boolean value; unsigned char guard; } writable = {0, 0x56};
    if (AudioUnitGetPropertyInfo(out.unit, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &size.size, &writable.value) || size.size != 8 ||
        size.guard != 0x11223344 || !writable.value || writable.guard != 0x56) return -265;
    struct { AURenderCallbackStruct callback; uint32_t guard; } read = {{0}, 0xabcdef01};
    OSStatus read_status = AudioUnitGetProperty(out.unit, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &read.callback, &size.size);
    /* Some Apple units expose this as write-only despite GetPropertyInfo
       succeeding. Preserve that native error, and guard outputs either way. */
    if ((read_status && read_status != kAudioUnitErr_InvalidProperty) ||
        (!read_status && (read.callback.inputProc != input ||
        read.callback.inputProcRefCon != cb.callback.inputProcRefCon)) ||
        read.guard != 0xabcdef01 || size.size != 8 || size.guard != 0x11223344 ||
        cb.guard != 0xaabbccdd) return -266;
    if (!AudioUnitSetProperty(out.unit, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &cb.callback, 4)) return -267;
    if (AudioUnitInitialize(out.unit)) return -268;
    struct { int16_t samples[256]; uint32_t guard; } samples = {{0}, 0xabcdef01};
    struct { AudioBufferList list; uint32_t guard; } buffers = {{1, {{2, sizeof(samples.samples), samples.samples}}}, 0x12345678};
    AudioTimeStamp time = {0}; time.mFlags = kAudioTimeStampSampleTimeValid;
    struct { AudioUnitRenderActionFlags value; uint32_t guard; } flags = {0, 0xaabbccdd};
    calls = 0; callback_error = 0;
    if (AudioUnitRender(out.unit, &flags.value, &time, 0, 128, &buffers.list) ||
        calls != 1 || callback_error || samples.guard != 0xabcdef01 ||
        buffers.guard != 0x12345678 || flags.guard != 0xaabbccdd) return -269;
    if (AudioUnitUninitialize(out.unit)) return -270;
    cb.callback = (AURenderCallbackStruct){0};
    if (AudioUnitSetProperty(out.unit, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &cb.callback, sizeof(cb.callback)) || CloseComponent(out.unit)) return -271;

    /* Exercise the exact voice-input property too, without initializing or
       starting the HAL unit: no microphone access is requested by this test. */
    desc = (AudioComponentDescription){kAudioUnitType_Output,
        kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    component = FindNextComponent(NULL, &desc);
    if (!component || OpenAComponent(component, &out.unit)) return -272;
    cb.callback = (AURenderCallbackStruct){input, (void *)0x12345678};
    if (AudioUnitSetProperty(out.unit, kAudioOutputUnitProperty_SetInputCallback,
        kAudioUnitScope_Global, 0, &cb.callback, sizeof(cb.callback))) return -273;
    size.size = 8;
    read_status = AudioUnitGetProperty(out.unit, kAudioOutputUnitProperty_SetInputCallback,
        kAudioUnitScope_Global, 0, &read.callback, &size.size);
    if ((read_status && read_status != kAudioUnitErr_InvalidProperty) ||
        (!read_status && (read.callback.inputProc != input ||
        read.callback.inputProcRefCon != cb.callback.inputProcRefCon)) ||
        read.guard != 0xabcdef01 || size.size != 8 || size.guard != 0x11223344) return -274;
    if (CloseComponent(out.unit)) return -275;
    return 0;
}
