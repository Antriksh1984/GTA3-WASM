/*
 * Minimal AL_EXT_EFX (Effects Extension) header.
 *
 * Emscripten's bundled OpenAL implementation (src/lib/libopenal.js) does not
 * implement the EFX extension or alGetProcAddress, and does not ship this
 * header. re3's OpenAL backend only touches these symbols behind runtime
 * checks of alcIsExtensionPresent(..., ALC_EXT_EFX_NAME), which is always
 * false under Emscripten, so this header only needs to satisfy the compiler
 * and linker with the standard EFX interface (types, tokens, and function
 * pointer typedefs) -- none of it is exercised at runtime in the WASM build.
 */
#ifndef AL_EFX_H
#define AL_EFX_H

#include <AL/al.h>
#include <AL/alc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALC_EXT_EFX_NAME "ALC_EXT_EFX"

#define ALC_EFX_MAJOR_VERSION                   0x20001
#define ALC_EFX_MINOR_VERSION                   0x20002
#define ALC_MAX_AUXILIARY_SENDS                 0x20003

/* Listener properties. */
#define AL_METERS_PER_UNIT                      0x20004

/* Source properties. */
#define AL_DIRECT_FILTER                        0x20005
#define AL_AUXILIARY_SEND_FILTER                0x20006
#define AL_AIR_ABSORPTION_FACTOR                0x20007
#define AL_ROOM_ROLLOFF_FACTOR                  0x20008
#define AL_CONE_OUTER_GAINHF                    0x20009
#define AL_DIRECT_FILTER_GAINHF_AUTO            0x2000A
#define AL_AUXILIARY_SEND_FILTER_GAIN_AUTO      0x2000B
#define AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO    0x2000C

/* Effect slot properties. */
#define AL_EFFECTSLOT_EFFECT                    0x0001
#define AL_EFFECTSLOT_GAIN                      0x0002
#define AL_EFFECTSLOT_AUXILIARY_SEND_AUTO       0x0003
#define AL_EFFECTSLOT_NULL                      0x0000

/* Effect object properties. */
#define AL_EFFECT_TYPE                          0x8001

#define AL_EFFECT_NULL                          0x0000
#define AL_EFFECT_REVERB                        0x0001
#define AL_EFFECT_CHORUS                        0x0002
#define AL_EFFECT_DISTORTION                    0x0003
#define AL_EFFECT_ECHO                          0x0004
#define AL_EFFECT_FLANGER                       0x0005
#define AL_EFFECT_FREQUENCY_SHIFTER             0x0006
#define AL_EFFECT_VOCAL_MORPHER                 0x0007
#define AL_EFFECT_PITCH_SHIFTER                 0x0008
#define AL_EFFECT_RING_MODULATOR                0x0009
#define AL_EFFECT_AUTOWAH                       0x000A
#define AL_EFFECT_COMPRESSOR                    0x000B
#define AL_EFFECT_EQUALIZER                     0x000C
#define AL_EFFECT_EAXREVERB                     0x8000

/* Reverb (standard, non-EAX) effect parameters. */
#define AL_REVERB_DENSITY                       0x0001
#define AL_REVERB_DIFFUSION                     0x0002
#define AL_REVERB_GAIN                          0x0003
#define AL_REVERB_GAINHF                        0x0004
#define AL_REVERB_DECAY_TIME                    0x0005
#define AL_REVERB_DECAY_HFRATIO                 0x0006
#define AL_REVERB_REFLECTIONS_GAIN              0x0007
#define AL_REVERB_REFLECTIONS_DELAY             0x0008
#define AL_REVERB_LATE_REVERB_GAIN              0x0009
#define AL_REVERB_LATE_REVERB_DELAY             0x000A
#define AL_REVERB_AIR_ABSORPTION_GAINHF         0x000B
#define AL_REVERB_ROOM_ROLLOFF_FACTOR           0x000C
#define AL_REVERB_DECAY_HFLIMIT                 0x000D

/* EAX Reverb effect parameters. */
#define AL_EAXREVERB_DENSITY                    0x0001
#define AL_EAXREVERB_DIFFUSION                  0x0002
#define AL_EAXREVERB_GAIN                       0x0003
#define AL_EAXREVERB_GAINHF                     0x0004
#define AL_EAXREVERB_GAINLF                     0x0005
#define AL_EAXREVERB_DECAY_TIME                 0x0006
#define AL_EAXREVERB_DECAY_HFRATIO              0x0007
#define AL_EAXREVERB_DECAY_LFRATIO               0x0008
#define AL_EAXREVERB_REFLECTIONS_GAIN           0x0009
#define AL_EAXREVERB_REFLECTIONS_DELAY          0x000A
#define AL_EAXREVERB_REFLECTIONS_PAN            0x000B
#define AL_EAXREVERB_LATE_REVERB_GAIN           0x000C
#define AL_EAXREVERB_LATE_REVERB_DELAY          0x000D
#define AL_EAXREVERB_LATE_REVERB_PAN            0x000E
#define AL_EAXREVERB_ECHO_TIME                  0x000F
#define AL_EAXREVERB_ECHO_DEPTH                 0x0010
#define AL_EAXREVERB_MODULATION_TIME            0x0011
#define AL_EAXREVERB_MODULATION_DEPTH           0x0012
#define AL_EAXREVERB_AIR_ABSORPTION_GAINHF      0x0013
#define AL_EAXREVERB_HFREFERENCE                0x0014
#define AL_EAXREVERB_LFREFERENCE                0x0015
#define AL_EAXREVERB_ROOM_ROLLOFF_FACTOR         0x0016
#define AL_EAXREVERB_DECAY_HFLIMIT               0x0017

#define AL_EAXREVERB_MIN_REFLECTIONS_GAIN         0.0f
#define AL_EAXREVERB_MAX_REFLECTIONS_GAIN         3.16f
#define AL_EAXREVERB_MIN_LATE_REVERB_GAIN         0.0f
#define AL_EAXREVERB_MAX_LATE_REVERB_GAIN         10.0f
#define AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF    0.892f
#define AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF    1.0f

/* Filter object properties. */
#define AL_FILTER_TYPE                          0x8001

#define AL_FILTER_NULL                          0x0000
#define AL_FILTER_LOWPASS                       0x0001
#define AL_FILTER_HIGHPASS                      0x0002
#define AL_FILTER_BANDPASS                      0x0003

#define AL_LOWPASS_GAIN                         0x0001
#define AL_LOWPASS_GAINHF                       0x0002

/* Function types, loaded dynamically via alGetProcAddress(). */
typedef void (*LPALGENEFFECTS)(ALsizei, ALuint*);
typedef void (*LPALDELETEEFFECTS)(ALsizei, const ALuint*);
typedef ALboolean (*LPALISEFFECT)(ALuint);
typedef void (*LPALEFFECTI)(ALuint, ALenum, ALint);
typedef void (*LPALEFFECTIV)(ALuint, ALenum, const ALint*);
typedef void (*LPALEFFECTF)(ALuint, ALenum, ALfloat);
typedef void (*LPALEFFECTFV)(ALuint, ALenum, const ALfloat*);
typedef void (*LPALGETEFFECTI)(ALuint, ALenum, ALint*);
typedef void (*LPALGETEFFECTIV)(ALuint, ALenum, ALint*);
typedef void (*LPALGETEFFECTF)(ALuint, ALenum, ALfloat*);
typedef void (*LPALGETEFFECTFV)(ALuint, ALenum, ALfloat*);

typedef void (*LPALGENFILTERS)(ALsizei, ALuint*);
typedef void (*LPALDELETEFILTERS)(ALsizei, const ALuint*);
typedef ALboolean (*LPALISFILTER)(ALuint);
typedef void (*LPALFILTERI)(ALuint, ALenum, ALint);
typedef void (*LPALFILTERIV)(ALuint, ALenum, const ALint*);
typedef void (*LPALFILTERF)(ALuint, ALenum, ALfloat);
typedef void (*LPALFILTERFV)(ALuint, ALenum, const ALfloat*);
typedef void (*LPALGETFILTERI)(ALuint, ALenum, ALint*);
typedef void (*LPALGETFILTERIV)(ALuint, ALenum, ALint*);
typedef void (*LPALGETFILTERF)(ALuint, ALenum, ALfloat*);
typedef void (*LPALGETFILTERFV)(ALuint, ALenum, ALfloat*);

typedef void (*LPALGENAUXILIARYEFFECTSLOTS)(ALsizei, ALuint*);
typedef void (*LPALDELETEAUXILIARYEFFECTSLOTS)(ALsizei, const ALuint*);
typedef ALboolean (*LPALISAUXILIARYEFFECTSLOT)(ALuint);
typedef void (*LPALAUXILIARYEFFECTSLOTI)(ALuint, ALenum, ALint);
typedef void (*LPALAUXILIARYEFFECTSLOTIV)(ALuint, ALenum, const ALint*);
typedef void (*LPALAUXILIARYEFFECTSLOTF)(ALuint, ALenum, ALfloat);
typedef void (*LPALAUXILIARYEFFECTSLOTFV)(ALuint, ALenum, const ALfloat*);
typedef void (*LPALGETAUXILIARYEFFECTSLOTI)(ALuint, ALenum, ALint*);
typedef void (*LPALGETAUXILIARYEFFECTSLOTIV)(ALuint, ALenum, ALint*);
typedef void (*LPALGETAUXILIARYEFFECTSLOTF)(ALuint, ALenum, ALfloat*);
typedef void (*LPALGETAUXILIARYEFFECTSLOTFV)(ALuint, ALenum, ALfloat*);

#ifdef __cplusplus
}
#endif

#endif /* AL_EFX_H */
