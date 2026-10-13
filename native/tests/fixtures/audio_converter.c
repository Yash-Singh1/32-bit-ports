#include <AudioToolbox/AudioToolbox.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(AudioStreamBasicDescription) == 40, "i386 ASBD ABI");
_Static_assert(sizeof(AudioBufferList) == 16, "i386 AudioBufferList ABI");
_Static_assert(sizeof(AudioConverterRef) == 4, "i386 converter handle ABI");

struct input_state {
    AudioConverterRef converter;
    const float *samples;
    uint32_t frames, position, calls;
    int error, callback_error, dispose_inside;
};

static OSStatus input(AudioConverterRef converter, UInt32 *packets, AudioBufferList *data,
                      AudioStreamPacketDescription **descriptions, void *opaque)
{
    struct input_state *state = opaque;
    if (!state || converter != state->converter || !packets || !data ||
        data->mNumberBuffers != 1 || data->mBuffers[0].mNumberChannels != 1 ||
        (descriptions && *descriptions)) {
        if (state) state->callback_error = 1;
        return -12346;
    }
    ++state->calls;
    if (state->dispose_inside && AudioConverterDispose(converter) != -50)
        state->callback_error = 1;
    uint32_t available = state->frames - state->position;
    uint32_t count = *packets < available ? *packets : available;
    if (state->error) count = 0;
    /* This matches Source's callback: native code supplies count/channels;
       guest code updates the PCM size and pointer without rebuilding a list. */
    data->mBuffers[0].mDataByteSize = count * sizeof(float);
    data->mBuffers[0].mData = count ? (void *)(state->samples + state->position) : NULL;
    *packets = count;
    state->position += count;
    return state->error ? state->error : count ? noErr : -1;
}

static AudioStreamBasicDescription pcm(double rate, int floating)
{
    uint32_t bytes = floating ? sizeof(float) : sizeof(int16_t);
    return (AudioStreamBasicDescription){rate, kAudioFormatLinearPCM,
        kAudioFormatFlagIsPacked |
        (floating ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger),
        bytes, 1, bytes, 1, bytes * 8, 0};
}

int check_audio_converter(void)
{
    AudioStreamBasicDescription source = pcm(48000, 1), target = pcm(48000, 0);
    struct { AudioConverterRef value; uint32_t guard; } converter = {NULL, 0x87654321};
    if (AudioConverterNew(&source, &target, &converter.value) || !converter.value ||
        converter.guard != 0x87654321) return -280;
    uint32_t prime = kConverterPrimeMethod_None;
    if (AudioConverterSetProperty(converter.value, kAudioConverterPrimeMethod, 8, &prime) !=
        kAudioConverterErr_BadPropertySizeError) return -281;
    if (AudioConverterSetProperty(converter.value, kAudioConverterDecompressionMagicCookie,
        sizeof(prime), &prime) != kAudioConverterErr_PropertyNotSupported) return -282;

    struct { float data[8]; uint32_t guard; } samples = {{-1, -.75f, -.5f, -.25f, 0, .25f, .5f, .75f}, 0x11223344};
    struct { uint32_t before; int16_t data[8]; uint32_t after; } output = {0x12345678, {0}, 0xaabbccdd};
    struct { UInt32 value, guard; } packets = {8, 0x56789abc};
    struct { AudioBufferList value; uint32_t guard; } buffers = {{1, {{1, sizeof(output.data), output.data}}}, 0xabcdef01};
    struct input_state state = {converter.value, samples.data, 8, 0, 0, 0, 0, 1};
    struct { AudioStreamPacketDescription value; uint32_t guard; } descriptions = {{0}, 0x98765432};
    memset(&descriptions.value, 0xa5, sizeof(descriptions.value));
    AudioStreamPacketDescription saved_description = descriptions.value;
    AudioBufferList saved_buffers = buffers.value;
    buffers.value.mBuffers[0].mData = (void *)0xfffffff0u;
    buffers.value.mBuffers[0].mDataByteSize = 32;
    AudioBufferList bad_range = buffers.value;
    if (AudioConverterFillComplexBuffer(converter.value, input, &state, &packets.value,
        &buffers.value, NULL) != -50 || state.calls || packets.value != 8 ||
        packets.guard != 0x56789abc || buffers.guard != 0xabcdef01 ||
        memcmp(&buffers.value, &bad_range, sizeof(bad_range))) return -296;
    buffers.value = saved_buffers;
    if (AudioConverterFillComplexBuffer(converter.value, input, &state, &packets.value,
        &buffers.value, &descriptions.value) != kAudioConverterErr_OperationNotSupported ||
        packets.value != 8 || packets.guard != 0x56789abc || state.calls ||
        memcmp(&saved_description, &descriptions.value, sizeof(saved_description)) ||
        descriptions.guard != 0x98765432) return -283;
    if (AudioConverterFillComplexBuffer(converter.value, input, &state, &packets.value,
        &buffers.value, NULL) || !state.calls || state.callback_error || packets.value != 8 ||
        packets.guard != 0x56789abc || buffers.guard != 0xabcdef01 ||
        buffers.value.mNumberBuffers != 1 || buffers.value.mBuffers[0].mNumberChannels != 1 ||
        buffers.value.mBuffers[0].mDataByteSize != sizeof(output.data) ||
        buffers.value.mBuffers[0].mData != output.data || output.before != 0x12345678 ||
        output.after != 0xaabbccdd || samples.guard != 0x11223344) return -284;
    for (int i = 0; i < 8; ++i) if (output.data[i] != (i - 4) * 8192) return -285;
    packets.value = 8; buffers.value.mBuffers[0].mDataByteSize = sizeof(output.data);
    if (AudioConverterFillComplexBuffer(converter.value, input, &state, &packets.value,
        &buffers.value, NULL) != -1 || packets.value || packets.guard != 0x56789abc ||
        buffers.guard != 0xabcdef01 || output.before != 0x12345678 ||
        output.after != 0xaabbccdd) return -286;
    AudioConverterRef stale = converter.value;
    if (AudioConverterDispose(stale) || AudioConverterDispose(stale) != -50) return -287;
    converter.value = NULL;
    if (AudioConverterNew(&source, &target, &converter.value) || !converter.value ||
        converter.value == stale || converter.guard != 0x87654321) return -288;
    if (AudioConverterSetProperty(stale, kAudioConverterPrimeMethod, sizeof(prime), &prime) != -50)
        return -289;
    state = (struct input_state){converter.value, samples.data, 8, 0, 0, -12345, 0, 0};
    packets.value = 8; buffers.value.mBuffers[0].mDataByteSize = sizeof(output.data);
    if (AudioConverterFillComplexBuffer(converter.value, input, &state, &packets.value,
        &buffers.value, NULL) != -12345 || !state.calls || state.callback_error ||
        packets.guard != 0x56789abc || buffers.guard != 0xabcdef01 ||
        output.before != 0x12345678 || output.after != 0xaabbccdd ||
        AudioConverterDispose(converter.value)) return -290;

    /* Exercise the actual voice sample rates with a native PCM resampler;
       this uses only synthetic samples and never accesses the microphone. */
    target = pcm(11025, 0);
    if (AudioConverterNew(&source, &target, &converter.value) ||
        AudioConverterSetProperty(converter.value, kAudioConverterPrimeMethod, sizeof(prime), &prime))
        return -291;
    uint32_t quality = 64;
    if (AudioConverterSetProperty(converter.value, kAudioConverterSampleRateConverterQuality,
        sizeof(quality), &quality)) return -292;
    float resample_input[4096];
    for (unsigned i = 0; i < 4096; ++i) resample_input[i] = (i % 128 < 64) ? .5f : -.5f;
    struct { int16_t data[512]; uint32_t guard; } resample_output = {{0}, 0x13572468};
    buffers.value = (AudioBufferList){1, {{1, sizeof(resample_output.data), resample_output.data}}};
    packets.value = 512;
    state = (struct input_state){converter.value, resample_input, 4096, 0, 0, 0, 0, 0};
    if (AudioConverterFillComplexBuffer(converter.value, input, &state, &packets.value,
        &buffers.value, NULL) || packets.value != 512 || !state.calls || state.callback_error ||
        packets.guard != 0x56789abc || buffers.guard != 0xabcdef01 ||
        resample_output.guard != 0x13572468 ||
        buffers.value.mBuffers[0].mData != resample_output.data ||
        buffers.value.mBuffers[0].mDataByteSize != sizeof(resample_output.data)) return -293;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 512; ++i) if (resample_output.data[i]) ++nonzero;
    if (nonzero < 480 || AudioConverterDispose(converter.value)) return -294;
    source.mFormatID = kAudioFormatMPEGLayer3;
    converter.value = (void *)0x12345678;
    if (AudioConverterNew(&source, &target, &converter.value) != kAudioConverterErr_FormatNotSupported ||
        converter.value || converter.guard != 0x87654321) return -295;
    return 0;
}
