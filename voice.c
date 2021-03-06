// "frequency" or "freq" refers to a Hz value, and "pitch" refers to
// a numeric musical note with 0 for C0, 12 for C1, etc..
#include <stdio.h>
#include <float.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "voice.h"

#define fr(i, bound) for (int i = 0; i < (bound); i++)

static void sdlec(int line, const char *file) {
	const char *error = SDL_GetError();
	if (!error || !error[0]) return;
	printf("SDL error at line %i in %s :\n%s\n", line, file, error);
	SDL_ClearError();
}
#define _sdlec sdlec(__LINE__, __FILE__);

//#define LOG_AUDIO_HISTORY


#define atomic_t SDL_atomic_t
#define atomic_max INT32_MAX // not sure if this is right...

const double tau = 6.28318530717958647692528676655900576839433879875;
const double semitoneRatio = 1.059463094359295264562; // the 12th root of 2
const double A4freq  = 440.0;
const double A4pitch =  57.0;

double freqFromPitch(double pitch) {
	return pow(semitoneRatio, pitch-A4pitch)*A4freq;
}

uint32_t   sampleRate = 48000; // may be changed by initVoices(), but not after
uint32_t   floatStreamSize = 1024; // must be a power of 2
double     globalVolume = 1.0;
SDL_mutex *gvMutex;

void setGlobalVolume(double v) {
	SDL_LockMutex(gvMutex);
	globalVolume = v;
	SDL_UnlockMutex(gvMutex);
}
double getGlobalVolume(void) {
	SDL_LockMutex(gvMutex);
	double v = globalVolume;
	SDL_UnlockMutex(gvMutex);
	return v;
}

typedef struct {float *data; long count;} floatArray;
int         shapeCount;
floatArray *shapes = NULL;
floatArray *shapesIn = NULL;
SDL_mutex **shapeMutexes = NULL;
atomic_t    shapesHaveChanged = {0};

void shapeFromMem(int shapeIndex, int sampleCount, const float *mem) {
	SDL_LockMutex(shapeMutexes[shapeIndex]);
	shapesIn[shapeIndex].data = realloc(shapesIn[shapeIndex].data, sizeof(float)*sampleCount);
	shapesIn[shapeIndex].count = -sampleCount; // negative count indicates change
	fr (s, sampleCount) {
		shapesIn[shapeIndex].data[s] = mem[s];
		//printf("shapesIn[%i].data[%i]: %f\n", shapeIndex, s, shapesIn[shapeIndex].data[s]);
	}
	SDL_UnlockMutex(shapeMutexes[shapeIndex]);
}
void shapeFromSine(int shapeIndex, int sampleCount) {
	SDL_LockMutex(shapeMutexes[shapeIndex]);
	shapesIn[shapeIndex].data = realloc(shapesIn[shapeIndex].data, sizeof(float)*sampleCount);
	shapesIn[shapeIndex].count = -sampleCount;
	fr (s, sampleCount) {
		shapesIn[shapeIndex].data[s] = sin(s*(tau/sampleCount));
	}
	SDL_UnlockMutex(shapeMutexes[shapeIndex]);
}
void shapeFromSaw(int shapeIndex, int sampleCount) {
	SDL_LockMutex(shapeMutexes[shapeIndex]);
	shapesIn[shapeIndex].data = realloc(shapesIn[shapeIndex].data, sizeof(float)*sampleCount);
	shapesIn[shapeIndex].count = -sampleCount;
	fr (s, sampleCount) {
		shapesIn[shapeIndex].data[s] = 1.0 - ((double)s/sampleCount)*2.0;
	}
	SDL_UnlockMutex(shapeMutexes[shapeIndex]);
}
void shapeFromTri(int shapeIndex, int sampleCount) {
	SDL_LockMutex(shapeMutexes[shapeIndex]);
	shapesIn[shapeIndex].data = realloc(shapesIn[shapeIndex].data, sizeof(float)*sampleCount);
	shapesIn[shapeIndex].count = -sampleCount;
	int s = 0;
	for (double t = 0; s < sampleCount/2; s++, t++) shapesIn[shapeIndex].data[s] = -1.0 + (t/sampleCount)*4.0;
	for (double t = 0; s < sampleCount;   s++, t++) shapesIn[shapeIndex].data[s] =  1.0 - (t/sampleCount)*4.0;
	SDL_UnlockMutex(shapeMutexes[shapeIndex]);
}
void shapeFromPulse(int shapeIndex, int sampleCount, double pulseWidth) {
	const double pw = pulseWidth > 1 ? 1.0 : (pulseWidth < 0 ? 0.0 : pulseWidth);
	SDL_LockMutex(shapeMutexes[shapeIndex]);
	shapesIn[shapeIndex].data = realloc(shapesIn[shapeIndex].data, sizeof(float)*sampleCount);
	shapesIn[shapeIndex].count = -sampleCount;
	int s = 0;
	for (; s < sampleCount*pw; s++) shapesIn[shapeIndex].data[s] =  1.0;
	for (; s < sampleCount;    s++) shapesIn[shapeIndex].data[s] = -1.0;
	SDL_UnlockMutex(shapeMutexes[shapeIndex]);
}

SDL_AudioSpec audioSpec;
void logSpec(const SDL_AudioSpec as) {
  printf(
    " freq______%5d\n"
    " format____%5d\n"
    " channels__%5d\n"
    " silence___%5d\n"
    " samples___%5d\n"
    " size______%5d\n\n",
    as.freq,
    as.format,
    as.channels,
    as.silence,
    as.samples,
    as.size
  );
}
void shapesFromWavFile(int firstShapeIndex, uint32_t shapeCount, const char *path) {
	float *samples;
	int sampleCount;
	{
		SDL_AudioSpec wavSpec;
		uint8_t      *wavBuf;
		uint32_t      wavBufSize;
		if (!SDL_LoadWAV(path, &wavSpec, &wavBuf, &wavBufSize)) {
			printf("\ncould not load file: \"%s\"\n", path);
			return;
		}
		//logSpec(wavSpec);
		SDL_AudioCVT cvt;
		// if channelSelect < 0 or past the available channels,
		// then let SDL mix it, else we will extract the desired channel later
		if (shapeCount > wavSpec.channels) shapeCount = wavSpec.channels;
		SDL_BuildAudioCVT(
			&cvt,             // SDL_AudioCVT   *cvt
			wavSpec.format,   // SDL_AudioFormat src_format
			wavSpec.channels, // Uint8           src_channels
			wavSpec.freq,     // int             src_rate
			audioSpec.format, // SDL_AudioFormat dst_format
			shapeCount,       // Uint8           dst_channels
			audioSpec.freq    // int             dst_rate
		);_sdlec;
		SDL_assert(cvt.needed);
		cvt.len = wavBufSize;
		cvt.len_cvt = wavBufSize*cvt.len_mult;
		cvt.buf = malloc(cvt.len_cvt*sizeof(uint8_t));
		fr (i, wavBufSize) cvt.buf[i] = wavBuf[i];
		SDL_ConvertAudio(&cvt);_sdlec;
		samples = (float*)cvt.buf;
		sampleCount = cvt.len_cvt/sizeof(float);
		SDL_FreeWAV(wavBuf);_sdlec;
	}
	switch (shapeCount) {
	 	case 1: shapeFromMem(firstShapeIndex, sampleCount, samples); break;
		case 2:
			{
				const int sampleCountPerCh = sampleCount/2;
				float *deinterlaced = malloc(sampleCount*sizeof(float));
				int di = 0;
				for (int si = 0; si < sampleCountPerCh; si++, di++) {
					deinterlaced[di] = samples[si*2];
				}
				for (int si = 0; si < sampleCountPerCh; si++, di++) {
					deinterlaced[di] = samples[si*2 + 1];
				}
				shapeFromMem(firstShapeIndex,   sampleCountPerCh, deinterlaced);
				shapeFromMem(firstShapeIndex+1, sampleCountPerCh, &deinterlaced[sampleCountPerCh]);
				free(deinterlaced);
			}
			break;
		default: printf("unsupported channel count: %i from %s\n", shapeCount, path);
	}
	free(samples);
}
long getShapeLength(int shapeIndex) {
	SDL_LockMutex(shapeMutexes[shapeIndex]);
	long l = shapesIn[shapeIndex].count;
	SDL_UnlockMutex(shapeMutexes[shapeIndex]);
	return l;
}
double incFromFreq(long shapeLength, double freq) {
	return (freq/((double)sampleRate/shapeLength))/shapeLength;
}
double incFromPeriod(double period) {
	return (1.0/sampleRate)/period;
}
double incFromSpeed(long shapeLength, double speed) {
	return speed/shapeLength;
}



void syncShapes(void) {
	fr (s, shapeCount) {
		if (!SDL_TryLockMutex(shapeMutexes[s])) {
			if (shapesIn[s].count < 0) { // negative count indicates change
				shapesIn[s].count *= -1;
				const long newCount = shapesIn[s].count;
				if (shapes[s].count < newCount) {
					shapes[s].data = realloc(shapes[s].data, sizeof(float)*newCount);
				}
				fr (f, newCount) shapes[s].data[f] = shapesIn[s].data[f];
				shapes[s].count = newCount;
			}
			SDL_UnlockMutex(shapeMutexes[s]);
		}
	}
}


int         voiceCount = 0;
voice      *voices = NULL;
float      *voicesPan = NULL;
SDL_mutex **voiceMutexes = NULL;

void setOscShape(int voiceIndex, int voicePart, int shapeIndex) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart].shape = shapeIndex;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setOscShift(int voiceIndex, int voicePart, double shift) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart].shift = shift;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setOscAmp(int voiceIndex, int voicePart, double amp) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart].amp = amp;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setOscPos(int voiceIndex, int voicePart, double pos) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart].pos = pos;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setOscInc(int voiceIndex, int voicePart, double inc) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart].inc = inc;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void mulOscInc(int voiceIndex, int voicePart, double n) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart].inc *= n;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}

void setOscIncFromFreq(int voiceIndex, int voicePart, double freq) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	long shapeLength = getShapeLength(voices[voiceIndex][voicePart].shape);
	voices[voiceIndex][voicePart].inc = incFromFreq(shapeLength, freq);
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setOscIncFromFreqAndRestart(int voiceIndex, int voicePart, double freq) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	long shapeLength = getShapeLength(voices[voiceIndex][voicePart].shape);
	voices[voiceIndex][voicePart].inc = incFromFreq(shapeLength, freq);
	fr (o, vo_oscPerVoice) voices[voiceIndex][o].pos = 0;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setOscIncFromPeriod(int voiceIndex, int voicePart, double period) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart].inc = incFromPeriod(period);
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setOscIncFromSpeed(int voiceIndex, int voicePart, double speed) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	long shapeLength = getShapeLength(voices[voiceIndex][voicePart].shape);
	voices[voiceIndex][voicePart].inc = incFromSpeed(shapeLength, speed);
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}

void setOsc(int voiceIndex, int voicePart, const osc o) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][voicePart] = o;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void setVoice(int voiceIndex, const voice v) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	fr (o, vo_oscPerVoice) voices[voiceIndex][o] = v[o];
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void restartVoice(int voiceIndex) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	fr (o, vo_oscPerVoice) voices[voiceIndex][o].pos = 0;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void enableVoice(int voiceIndex) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][vo_wave].shape = abs(voices[voiceIndex][vo_wave].shape);
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}
void disableVoice(int voiceIndex) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voices[voiceIndex][vo_wave].shape = -1*abs(voices[voiceIndex][vo_wave].shape);
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}

void getVoice(int voiceIndex, voice v) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	fr (o, vo_oscPerVoice) v[o] = voices[voiceIndex][o];
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}

#define forVoiceRange(i) for (int i = firstVoiceIndex; i <= lastVoiceIndex; i++)
void setOscPoss(int firstVoiceIndex, int lastVoiceIndex, int voicePart, double pos) {
	forVoiceRange(i) SDL_LockMutex(voiceMutexes[i]);
	forVoiceRange(i) voices[i][voicePart].pos = pos;
	forVoiceRange(i) SDL_UnlockMutex(voiceMutexes[i]);
}
void setOscIncs(int firstVoiceIndex, int lastVoiceIndex, int voicePart, double inc) {
	forVoiceRange(i) SDL_LockMutex(voiceMutexes[i]);
	forVoiceRange(i) voices[i][voicePart].inc = inc;
	forVoiceRange(i) SDL_UnlockMutex(voiceMutexes[i]);
}
void mulOscIncs(int firstVoiceIndex, int lastVoiceIndex, int voicePart, double n) {
	forVoiceRange(i) SDL_LockMutex(voiceMutexes[i]);
	forVoiceRange(i) voices[i][voicePart].inc *= n;
	forVoiceRange(i) SDL_UnlockMutex(voiceMutexes[i]);
}
void restartVoices(int firstVoiceIndex, int lastVoiceIndex) {
	forVoiceRange(i) SDL_LockMutex(voiceMutexes[i]);
	forVoiceRange(i) {fr (o, vo_oscPerVoice) voices[i][o].pos = 0;}
	forVoiceRange(i) SDL_UnlockMutex(voiceMutexes[i]);
}
void enableVoices(int firstVoiceIndex, int lastVoiceIndex) {
	forVoiceRange(i) SDL_LockMutex(voiceMutexes[i]);
	forVoiceRange(i) voices[i][vo_wave].shape = abs(voices[i][vo_wave].shape);
	forVoiceRange(i) SDL_UnlockMutex(voiceMutexes[i]);
}
void disableVoices(int firstVoiceIndex, int lastVoiceIndex) {
	forVoiceRange(i) SDL_LockMutex(voiceMutexes[i]);
	forVoiceRange(i) voices[i][vo_wave].shape = -1*abs(voices[i][vo_wave].shape);
	forVoiceRange(i) SDL_UnlockMutex(voiceMutexes[i]);
}

void setVoicePan(int voiceIndex, double pan) {
	SDL_LockMutex(voiceMutexes[voiceIndex]);
	voicesPan[voiceIndex] = pan;
	SDL_UnlockMutex(voiceMutexes[voiceIndex]);
}


// the following functions expect the containing voice to be locked already
void loopOsc(osc *o) {
	const double p = o->pos;
	if      (p > 1) o->pos -= (long)p;
	else if (p < 0) o->pos -= (long)p-1;
}
void clampOsc(osc *o) {
	const double p = o->pos;
	if      (p > 1) o->pos = 1;
	else if (p < 0) o->pos = 0;
}

#define scootch 0.0000001 // to read up to "count" without going out of bounds
float readOsc(const osc o) {
	// this is only expected to be called for enabled voices, so abs is not called on shape index
	return shapes[o.shape].data[(long)(o.pos * (shapes[o.shape].count-scootch))] * o.amp + o.shift;
}


#ifdef LOG_AUDIO_HISTORY
#define audioHistoryLength 2048
float   audioHistory[audioHistoryLength];
int     audioHistoryPos = 0;
#endif

void audioCallback(void *_unused, uint8_t *byteStream, int byteStreamLength) {
	syncShapes();
	float *floatStream = (float*)byteStream;
	int enabledVoiceCount = 0;
	fr (s, floatStreamSize) floatStream[s] = 0;
	fr (v, voiceCount) {
		SDL_LockMutex(voiceMutexes[v]);_sdlec;
		if (voices[v][vo_wave].shape < 0) {
			SDL_UnlockMutex(voiceMutexes[v]);_sdlec;
			continue;
		}
		enabledVoiceCount++;
		const double rightFactor = fabs(sin(((voicesPan[v]+1.0)*M_PI)/4));
		const double leftFactor  = fabs(sin(((voicesPan[v]-1.0)*M_PI)/4));
		for (int s = 0; s < floatStreamSize; s += 2) {
			voices[v][vo_incEnv].pos += voices[v][vo_incEnv].inc;
			clampOsc(&voices[v][vo_incEnv]);
			voices[v][vo_incMod].pos += voices[v][vo_incMod].inc;
			loopOsc(&voices[v][vo_incMod]);
			voices[v][vo_wave].pos += (voices[v][vo_wave].inc * readOsc(voices[v][vo_incEnv]) * readOsc(voices[v][vo_incMod]));
			loopOsc(&voices[v][vo_wave]);
			voices[v][vo_ampEnv].pos += voices[v][vo_ampEnv].inc;
			clampOsc(&voices[v][vo_ampEnv]);
			voices[v][vo_ampMod].pos += voices[v][vo_ampMod].inc;
			loopOsc(&voices[v][vo_ampMod]);
			const double sample = readOsc(voices[v][vo_wave]) * readOsc(voices[v][vo_ampMod]) * readOsc(voices[v][vo_ampEnv]);
			#ifdef LOG_AUDIO_HISTORY
			if (audioHistoryPos < audioHistoryLength) audioHistory[audioHistoryPos++] = sample; // TEMP
			#endif
			floatStream[s  ] += sample * leftFactor;
			floatStream[s+1] += sample * rightFactor;
		}
		SDL_UnlockMutex(voiceMutexes[v]);_sdlec;
	}
	if (enabledVoiceCount < 1) return;
	SDL_LockMutex(gvMutex);
	const double gv = globalVolume;
	SDL_UnlockMutex(gvMutex);
	if (enabledVoiceCount > 1) {
		const double amp = gv / enabledVoiceCount;
		fr (s, floatStreamSize) floatStream[s] *= amp;
	}
	else fr (s, floatStreamSize) floatStream[s] *= gv;
}

SDL_AudioDeviceID audioDevice;

int initVoices(int initVoiceCount, int initShapeCount) {
	SDL_Init(SDL_INIT_AUDIO);_sdlec;
	SDL_AudioSpec want = {0};
	want.freq     = sampleRate;
	want.format   = AUDIO_F32;
	want.channels = 2; // stereo
	want.samples  = 1024; // must be a power of 2
	want.callback = audioCallback;
	audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &audioSpec, 0);_sdlec;
	sampleRate = audioSpec.freq;
	floatStreamSize = audioSpec.size/sizeof(float);
	// shape data
	shapeCount = initShapeCount;
	shapes   = malloc(sizeof(floatArray)*shapeCount);
	shapesIn = malloc(sizeof(floatArray)*shapeCount);
	fr (s, shapeCount) {
		shapes  [s].data = malloc(sizeof(float));
		shapesIn[s].data = malloc(sizeof(float));
		shapes  [s].count = 1;
		shapesIn[s].count = 1;
		shapes  [s].data[0] = 1.0;
		shapesIn[s].data[0] = 1.0;
	}
	shapeMutexes = malloc(sizeof(SDL_mutex*)*shapeCount);
	fr (s, shapeCount) {shapeMutexes[s] = SDL_CreateMutex();_sdlec;}
	// voice data
	voiceCount = initVoiceCount;
	voices = calloc(voiceCount, sizeof(voice));
	voicesPan = calloc(voiceCount, sizeof(float));
	voiceMutexes = malloc(sizeof(SDL_mutex*)*voiceCount);
	gvMutex = SDL_CreateMutex();_sdlec;
	fr (v, voiceCount) {voiceMutexes[v] = SDL_CreateMutex();_sdlec;}
	fr (v, voiceCount) voices[v][vo_wave].shape = -1; // disables all voices
	return 0;
}

int closeVoices(void) {
	SDL_CloseAudioDevice(audioDevice);_sdlec;
	
	#ifdef LOG_AUDIO_HISTORY
	puts("\n________audioHistory________");
	fr (s, audioHistoryLength) {
		printf("%4i: %7.6f", s, audioHistory[s]);
		if (audioHistory[s] > 1 || audioHistory[s] < -1) puts("CLIPPING!");
		else puts("");
	}
	puts("");
	#endif
	
	fr (s, shapeCount) {
		SDL_LockMutex(shapeMutexes[s]);
		free(shapes[s].data);
		free(shapesIn[s].data);
		SDL_UnlockMutex(shapeMutexes[s]);
		SDL_DestroyMutex(shapeMutexes[s]);_sdlec;
	}
	free(shapes);
	free(shapesIn);
	free(shapeMutexes);
	free(voices);
	free(voicesPan);
	fr (v, voiceCount) {SDL_DestroyMutex(voiceMutexes[v]);_sdlec;}
	free(voiceMutexes);
	return 0;
}

void unpauseAudio(void) {
	syncShapes();
	SDL_PauseAudioDevice(audioDevice, 0);_sdlec;
}
void pauseAudio(void) {SDL_PauseAudioDevice(audioDevice, 1);_sdlec;}
