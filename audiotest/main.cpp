// re3 WASM audio diagnostic (Phase 6, see docs/AUDIO_WASM.md).
//
// This is deliberately NOT part of re3/GTA III -- it links against nothing from
// src/ or vendor/librw, only Emscripten's own OpenAL implementation (the same
// backend src/audio/sampman_oal.cpp already targets via AUDIO_OAL -- see
// src/CMakeLists.txt). All test tones are generated procedurally so this tool
// needs zero GTA III assets, consistent with rendertest/main.cpp's approach in
// Phase 4.
//
// Unlike rendertest, module instantiation here does nothing audio-related --
// main() just registers the exported functions below and returns immediately,
// keeping the runtime alive. The actual OpenAL device/context is only opened
// when audiotest_init() is called, which web/audiotest.html only ever does from
// inside a real button-click handler ("Click to Start Audio Test"). That click
// is also what satisfies the browser's user-gesture requirement for
// AudioContext -- see the comment above audiotest_init() for how this composes
// with Emscripten's own built-in autoResumeAudioContext() safety net.
//
// Every action exposes its result through *objective, queryable OpenAL state*
// (alGetSourcei(..., AL_SOURCE_STATE, ...), alGetError()) rather than relying on
// anyone actually listening to the output -- the same "don't just check for no
// crash, check the actual state" philosophy rendertest/main.cpp used for pixels.

#include <AL/al.h>
#include <AL/alc.h>
#include <emscripten.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int kSampleRate = 44100;

ALCdevice *device = nullptr;
ALCcontext *context = nullptr;

ALuint sfxBuffer = 0, musicBuffer = 0, panBuffer = 0;
ALuint sfxSource = 0, musicSource = 0, panSource = 0;

bool initialised = false;

// Generates a mono 16-bit PCM tone with a short linear fade in/out (avoids the
// click a hard-edged buffer would produce) and hands it to OpenAL as a static
// buffer. This mirrors the shape of a real one-shot SFX or looping music stem
// without needing any actual GTA III .WAV/.OPUS asset.
ALuint makeTone(float freqHz, float durationSec, float amplitude) {
	int sampleCount = static_cast<int>(durationSec * kSampleRate);
	std::vector<int16_t> samples(sampleCount);
	int fadeSamples = std::min(sampleCount / 8, kSampleRate / 50); // ~20ms max
	for (int i = 0; i < sampleCount; i++) {
		float t = static_cast<float>(i) / kSampleRate;
		float env = 1.0f;
		if (i < fadeSamples) env = static_cast<float>(i) / fadeSamples;
		else if (i >= sampleCount - fadeSamples) env = static_cast<float>(sampleCount - i) / fadeSamples;
		float s = std::sin(2.0f * 3.14159265f * freqHz * t) * amplitude * env;
		samples[i] = static_cast<int16_t>(s * 32767.0f);
	}

	ALuint buffer;
	alGenBuffers(1, &buffer);
	alBufferData(buffer, AL_FORMAT_MONO16, samples.data(),
	             static_cast<ALsizei>(samples.size() * sizeof(int16_t)), kSampleRate);
	return buffer;
}

// A two-tone chord loop (root + fifth), long enough (2s) that pause/resume can
// be exercised mid-playback and looping is visibly distinct from a one-shot.
ALuint makeMusicLoop() {
	float durationSec = 2.0f;
	int sampleCount = static_cast<int>(durationSec * kSampleRate);
	std::vector<int16_t> samples(sampleCount);
	int fadeSamples = kSampleRate / 20; // 50ms, needed so the loop seam doesn't click
	for (int i = 0; i < sampleCount; i++) {
		float t = static_cast<float>(i) / kSampleRate;
		float env = 1.0f;
		if (i < fadeSamples) env = static_cast<float>(i) / fadeSamples;
		else if (i >= sampleCount - fadeSamples) env = static_cast<float>(sampleCount - i) / fadeSamples;
		float s = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * t)
		        + 0.3f * std::sin(2.0f * 3.14159265f * 330.0f * t);
		samples[i] = static_cast<int16_t>(s * env * 32767.0f * 0.6f);
	}
	ALuint buffer;
	alGenBuffers(1, &buffer);
	alBufferData(buffer, AL_FORMAT_MONO16, samples.data(),
	             static_cast<ALsizei>(samples.size() * sizeof(int16_t)), kSampleRate);
	return buffer;
}

} // namespace

extern "C" {

// Opens the OpenAL device/context and builds the test buffers/sources. Must
// only be called from a real user-gesture call stack (a click handler in
// web/audiotest.html) -- not because this code enforces it, but because the
// underlying browser AudioContext will otherwise be created in a 'suspended'
// state per browser autoplay policy.
//
// Belt-and-suspenders note: even if this WERE somehow called without a prior
// gesture, Emscripten's alcCreateContext() (emsdk/upstream/emscripten/src/lib/
// libopenal.js) already calls autoResumeAudioContext(ac) itself, which attaches
// one-time keydown/mousedown/touchstart listeners on document and #canvas to
// resume() the context -- so audio would still recover on the next interaction
// rather than staying silent forever. This tool's explicit "Click to Start"
// button exists for UX clarity (the user should not have to guess that a
// random click will unstick things), not because the engine needs it to
// eventually work.
EMSCRIPTEN_KEEPALIVE
int audiotest_init(void) {
	if (initialised) return 1;

	device = alcOpenDevice(nullptr);
	if (!device) {
		printf("[audiotest] FAIL: alcOpenDevice returned null\n");
		return 0;
	}
	context = alcCreateContext(device, nullptr);
	if (!context || !alcMakeContextCurrent(context)) {
		printf("[audiotest] FAIL: alcCreateContext/alcMakeCurrent failed\n");
		return 0;
	}
	printf("[audiotest] PASS: OpenAL device + context created\n");

	alListenerf(AL_GAIN, 1.0f);
	alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);

	sfxBuffer = makeTone(880.0f, 0.15f, 0.8f);
	musicBuffer = makeMusicLoop();
	panBuffer = makeTone(440.0f, 0.6f, 0.8f);
	printf("[audiotest] PASS: procedural SFX/music/pan buffers generated\n");

	alGenSources(1, &sfxSource);
	alSourcei(sfxSource, AL_BUFFER, sfxBuffer);
	alSourcef(sfxSource, AL_GAIN, 1.0f);

	alGenSources(1, &musicSource);
	alSourcei(musicSource, AL_BUFFER, musicBuffer);
	alSourcei(musicSource, AL_LOOPING, AL_TRUE);
	alSourcef(musicSource, AL_GAIN, 1.0f);

	// Positional source: rolloff/reference distance tuned so only *direction*
	// (left/right pan), not distance attenuation, is being demonstrated.
	alGenSources(1, &panSource);
	alSourcei(panSource, AL_BUFFER, panBuffer);
	alSourcef(panSource, AL_GAIN, 1.0f);
	alSourcef(panSource, AL_ROLLOFF_FACTOR, 0.0f);
	alSourcef(panSource, AL_REFERENCE_DISTANCE, 1000.0f);
	alSource3f(panSource, AL_POSITION, 0.0f, 0.0f, -1.0f);

	ALenum err = alGetError();
	if (err != AL_NO_ERROR) {
		printf("[audiotest] FAIL: alGetError after setup = 0x%x\n", err);
		return 0;
	}
	printf("[audiotest] PASS: sources ready (sfx=%u music=%u pan=%u)\n", sfxSource, musicSource, panSource);

	initialised = true;
	return 1;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_play_sfx(void) {
	if (!initialised) return 0;
	alSourceRewind(sfxSource);
	alSourcePlay(sfxSource);
	printf("[audiotest] play SFX (one-shot 880Hz beep)\n");
	return alGetError() == AL_NO_ERROR;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_play_music(void) {
	if (!initialised) return 0;
	alSourcePlay(musicSource);
	printf("[audiotest] play music (looping chord)\n");
	return alGetError() == AL_NO_ERROR;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_pause_music(void) {
	if (!initialised) return 0;
	alSourcePause(musicSource);
	printf("[audiotest] pause music\n");
	return alGetError() == AL_NO_ERROR;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_resume_music(void) {
	if (!initialised) return 0;
	// Per OpenAL spec, alSourcePlay on an AL_PAUSED source resumes from the
	// paused offset; on an AL_STOPPED/AL_INITIAL source it (re)starts from zero.
	alSourcePlay(musicSource);
	printf("[audiotest] resume music\n");
	return alGetError() == AL_NO_ERROR;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_stop_music(void) {
	if (!initialised) return 0;
	alSourceStop(musicSource);
	printf("[audiotest] stop music\n");
	return alGetError() == AL_NO_ERROR;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_set_music_volume(float gain) {
	if (!initialised) return 0;
	if (gain < 0.0f) gain = 0.0f;
	if (gain > 1.0f) gain = 1.0f;
	alSourcef(musicSource, AL_GAIN, gain);
	printf("[audiotest] music volume -> %.2f\n", gain);
	return alGetError() == AL_NO_ERROR;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_set_sfx_volume(float gain) {
	if (!initialised) return 0;
	if (gain < 0.0f) gain = 0.0f;
	if (gain > 1.0f) gain = 1.0f;
	alSourcef(sfxSource, AL_GAIN, gain);
	printf("[audiotest] sfx volume -> %.2f\n", gain);
	return alGetError() == AL_NO_ERROR;
}

// pan in [-1, 1]: -1 = full left, 0 = centre, 1 = full right.
EMSCRIPTEN_KEEPALIVE
int audiotest_play_pan_test(float pan) {
	if (!initialised) return 0;
	if (pan < -1.0f) pan = -1.0f;
	if (pan > 1.0f) pan = 1.0f;
	alSource3f(panSource, AL_POSITION, pan * 5.0f, 0.0f, -1.0f);
	alSourceRewind(panSource);
	alSourcePlay(panSource);
	printf("[audiotest] play stereo pan test at x=%.2f\n", pan);
	return alGetError() == AL_NO_ERROR;
}

// which: 0 = sfx, 1 = music, 2 = pan. Returns the raw AL_SOURCE_STATE enum
// (AL_INITIAL=0x1011, AL_PLAYING=0x1012, AL_PAUSED=0x1013, AL_STOPPED=0x1014)
// so the JS side can assert on real OpenAL state rather than guessing from
// timers -- the same "query, don't assume" approach as rendertest's pixel
// readback.
EMSCRIPTEN_KEEPALIVE
int audiotest_get_state(int which) {
	if (!initialised) return -1;
	ALuint src = which == 0 ? sfxSource : which == 1 ? musicSource : panSource;
	ALint state = AL_INITIAL;
	alGetSourcei(src, AL_SOURCE_STATE, &state);
	return state;
}

EMSCRIPTEN_KEEPALIVE
int audiotest_get_error(void) {
	return alGetError();
}

} // extern "C"

int main() {
	// Deliberately does nothing audio-related -- see file header. Module load
	// (this function running) and audio device initialisation (audiotest_init())
	// are intentionally decoupled so the "Click to Start Audio Test" gesture in
	// web/audiotest.html is unambiguously what triggers the first OpenAL call.
	printf("[audiotest] module ready -- waiting for audiotest_init() from a user gesture\n");
	return 0;
}
