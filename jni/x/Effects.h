//
// FPlayAndroid is distributed under the FreeBSD License
//
// Copyright (c) 2013-2014, Carlos Rafael Gimenes das Neves
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// The views and conclusions contained in the software and documentation are those
// of the authors and should not be interpreted as representing official policies,
// either expressed or implied, of the FreeBSD Project.
//
// https://github.com/carlosrafaelgn/FPlayAndroid
//

#include "EffectsImplMacros.h"

#define DB_RANGE 1500 //+-15dB (in millibels)
#define BAND_COUNT 10
#define COEF_SET_COUNT 8

#define EQUALIZER_ENABLED 1
#define BASSBOOST_ENABLED 2
#define VIRTUALIZER_ENABLED 4

#define BASSBOOST_BAND_COUNT 3 //(31.25 Hz, 62.5 Hz and 125 Hz)

//https://en.wikipedia.org/wiki/Dynamic_range_compression
//https://en.wikipedia.org/wiki/Dynamic_range_compression#Limiting
//as the article states, brick-wall limiting are harsh and unpleasant.. also... reducing the gain abruptly causes audible clicks!
#define GAIN_REDUCTION_PER_SECOND_DB -40.0 //-40.0dB/s
#define GAIN_RECOVERY_PER_SECOND_DB 0.5 //+0.5dB/s

static uint32_t bassBoostStrength, virtualizerStrength;
static float equalizerGainInDB[BAND_COUNT];
static EFFECTPROC effectProc;

uint32_t effectsEnabled, equalizerMaxBandCount, effectsGainEnabled;
int32_t effectsMustReduceGain, effectsFramesBeforeRecoveringGain, effectsTemp[4] __attribute__((aligned(16)));
float effectsGainRecoveryOne[4] __attribute__((aligned(16))) = { 1.0f, 1.0f, 0.0f, 0.0f },
effectsGainReductionPerFrame[4] __attribute__((aligned(16))),
effectsGainRecoveryPerFrame[4] __attribute__((aligned(16))),
effectsGainClip[4] __attribute__((aligned(16))),
equalizerCoefs[2 * 4 * BAND_COUNT] __attribute__((aligned(16))),
//order for equalizerCoefs:
//0 band0 b0 L
//1 band0 b0 R
//2 band0 b1 L (which is also a1 in our case)
//3 band0 b1 R (which is also a1 in our case)
//4 band0 b2 L
//5 band0 b2 R
//6 band0 -a2 L
//7 band0 -a2 R
//8 band1 b0
//...
equalizerSamples[2 * 4 * BAND_COUNT] __attribute__((aligned(16)));

#ifdef FPLAY_X86
	#include <pmmintrin.h>
	//https://software.intel.com/sites/landingpage/IntrinsicsGuide/
#else
	extern void processEqualizerNeon(int16_t* srcBuffer, uint32_t sizeInFrames, int16_t* dstBuffer);
	extern void processVirtualizerNeon(int16_t* srcBuffer, uint32_t sizeInFrames, int16_t* dstBuffer);
	extern void processEffectsNeon(int16_t* srcBuffer, uint32_t sizeInFrames, int16_t* dstBuffer);
#endif

#include "Filter.h"

void updateEffectProc();

int32_t JNICALL getCurrentAutomaticEffectsGainInMB(JNIEnv* env, jclass clazz) {
	return ((effectsGainEnabled && effectsEnabled) ? (int32_t)(2000.0 * log10(effectsGainClip[0])) : 0);
}

void JNICALL enableAutomaticEffectsGain(JNIEnv* env, jclass clazz, uint32_t enabled) {
	::effectsGainEnabled = enabled;

	if (!enabled) {
		effectsGainClip[0] = 1.0f;
		effectsGainClip[1] = 1.0f;
		effectsGainClip[2] = 0.0f;
		effectsGainClip[3] = 0.0f;
		effectsMustReduceGain = 0;
		effectsFramesBeforeRecoveringGain = 0x7FFFFFFF;
	}
}	

uint32_t JNICALL isAutomaticEffectsGainEnabled(JNIEnv* env, jclass clazz) {
	return effectsGainEnabled;
}

void resetEqualizer() {
	effectsGainClip[0] = 1.0f;
	effectsGainClip[1] = 1.0f;
	effectsGainClip[2] = 0.0f;
	effectsGainClip[3] = 0.0f;
	effectsMustReduceGain = 0;
	effectsFramesBeforeRecoveringGain = 0x7FFFFFFF;

	memset(effectsTemp, 0, 4 * sizeof(int32_t));
	memset(equalizerSamples, 0, 2 * 4 * BAND_COUNT * sizeof(float));
}

void destroyVirtualizer() {
}

void resetVirtualizer() {
}

void equalizerConfigChanged() {
	//this only happens in two moments: upon initialization and when the sample rate changes

	if (dstSampleRate > (2 * 16000))
		equalizerMaxBandCount = 10;
	else if (dstSampleRate > (2 * 8000))
		equalizerMaxBandCount = 9;
	else if (dstSampleRate > (2 * 4000))
		equalizerMaxBandCount = 8;
	else if (dstSampleRate > (2 * 2000))
		equalizerMaxBandCount = 7;
	else
		equalizerMaxBandCount = 6; //Android's minimum allowed sample rate is 4000 Hz

	effectsGainReductionPerFrame[0] = (float)pow(10.0, GAIN_REDUCTION_PER_SECOND_DB / (double)(dstSampleRate * 20));
	effectsGainReductionPerFrame[1] = effectsGainReductionPerFrame[0];
	effectsGainRecoveryPerFrame[0] = (float)pow(10.0, GAIN_RECOVERY_PER_SECOND_DB / (double)(dstSampleRate * 20));
	effectsGainRecoveryPerFrame[1] = effectsGainRecoveryPerFrame[0];

	for (int32_t i = 0; i < BAND_COUNT; i++)
		computeFilter(i);

	resetEqualizer();
}

void recomputeVirtualizer() {
	//recompute the filter
}

void virtualizerConfigChanged() {
	//this only happens in two moments: upon initialization and when the sample rate changes

	if (!(effectsEnabled & VIRTUALIZER_ENABLED))
		return;

	destroyVirtualizer();

	//recreate all internal structures

	//recompute the filter
	recomputeVirtualizer();

	resetVirtualizer();
}

void initializeEffects() {
	effectsEnabled = 0;
	bassBoostStrength = 0;
	virtualizerStrength = 0;
	equalizerMaxBandCount = 0;
	effectsGainEnabled = 1;
	effectsGainReductionPerFrame[0] = 1.0f;
	effectsGainReductionPerFrame[1] = 1.0f;
	effectsGainReductionPerFrame[2] = 0.0f;
	effectsGainReductionPerFrame[3] = 0.0f;
	effectsGainRecoveryPerFrame[0] = 1.0f;
	effectsGainRecoveryPerFrame[1] = 1.0f;
	effectsGainRecoveryPerFrame[2] = 0.0f;
	effectsGainRecoveryPerFrame[3] = 0.0f;

	memset(equalizerGainInDB, 0, BAND_COUNT * sizeof(float));

	resetEqualizer();
	resetVirtualizer();

	updateEffectProc();
}

void processNull(int16_t* srcBuffer, uint32_t sizeInFrames, int16_t* dstBuffer) {
	//nothing to be done but copying from source to destination (if they are different buffers)
	if (srcBuffer != dstBuffer)
		memcpy(dstBuffer, srcBuffer, sizeInFrames << 2);
}

void processEqualizer(int16_t* srcBuffer, uint32_t sizeInFrames, int16_t* dstBuffer) {
	effectsFramesBeforeRecoveringGain -= sizeInFrames;

#ifdef FPLAY_X86
	__m128 gainClip = _mm_load_ps(effectsGainClip);
	__m128 maxAbsSample, tmp2;
	maxAbsSample = _mm_xor_ps(maxAbsSample, maxAbsSample);
	tmp2 = _mm_xor_ps(tmp2, tmp2);

	while ((sizeInFrames--)) {
		float *samples = equalizerSamples;

		effectsTemp[0] = (int32_t)srcBuffer[0];
		effectsTemp[1] = (int32_t)srcBuffer[1];
		//inLR = { L, R, xxx, xxx }
		__m128 inLR;
		inLR = _mm_cvtpi32_ps(inLR, *((__m64*)effectsTemp));

		equalizerX86();

		floatToShortX86();
	}

	footerX86();
#else
	float gainClip = effectsGainClip[0];
	float maxAbsSample = 0.0f;

	//no neon support... :(
	while ((sizeInFrames--)) {
		float *samples = equalizerSamples;

		float inL = (float)srcBuffer[0], inR = (float)srcBuffer[1];

		equalizerPlain();

		floatToShortPlain();
	}

	footerPlain();
#endif
}

void processVirtualizer(int16_t* srcBuffer, uint32_t sizeInFrames, int16_t* dstBuffer) {
	effectsFramesBeforeRecoveringGain -= sizeInFrames;

#ifdef FPLAY_X86
	__m128 gainClip = _mm_load_ps(effectsGainClip);
	__m128 maxAbsSample, tmp2;
	maxAbsSample = _mm_xor_ps(maxAbsSample, maxAbsSample);
	tmp2 = _mm_xor_ps(tmp2, tmp2);

	while ((sizeInFrames--)) {
		float *samples = equalizerSamples;

		effectsTemp[0] = (int32_t)srcBuffer[0];
		effectsTemp[1] = (int32_t)srcBuffer[1];
		//inLR = { L, R, xxx, xxx }
		__m128 inLR;
		inLR = _mm_cvtpi32_ps(inLR, *((__m64*)effectsTemp));

		virtualizerX86();

		floatToShortX86();
	}

	footerX86();
#else
	float gainClip = effectsGainClip[0];
	float maxAbsSample = 0.0f;

	//no neon support... :(
	while ((sizeInFrames--)) {
		float *samples = equalizerSamples;

		float inL = (float)srcBuffer[0], inR = (float)srcBuffer[1];

		virtualizerPlain();

		floatToShortPlain();
	}

	footerPlain();
#endif
}

void processEffects(int16_t* srcBuffer, uint32_t sizeInFrames, int16_t* dstBuffer) {
	effectsFramesBeforeRecoveringGain -= sizeInFrames;

#ifdef FPLAY_X86
	__m128 gainClip = _mm_load_ps(effectsGainClip);
	__m128 maxAbsSample, tmp2;
	maxAbsSample = _mm_xor_ps(maxAbsSample, maxAbsSample);
	tmp2 = _mm_xor_ps(tmp2, tmp2);

	while ((sizeInFrames--)) {
		float *samples = equalizerSamples;

		effectsTemp[0] = (int32_t)srcBuffer[0];
		effectsTemp[1] = (int32_t)srcBuffer[1];
		//inLR = { L, R, xxx, xxx }
		__m128 inLR;
		inLR = _mm_cvtpi32_ps(inLR, *((__m64*)effectsTemp));

		equalizerX86();

		virtualizerX86();

		floatToShortX86();
	}

	footerX86();
#else
	float gainClip = effectsGainClip[0];
	float maxAbsSample = 0.0f;

	//no neon support... :(
	while ((sizeInFrames--)) {
		float *samples = equalizerSamples;

		float inL = (float)srcBuffer[0], inR = (float)srcBuffer[1];

		equalizerPlain();

		virtualizerPlain();

		floatToShortPlain();
	}

	footerPlain();
#endif
}

void JNICALL enableEqualizer(JNIEnv* env, jclass clazz, uint32_t enabled) {
	if (enabled)
		effectsEnabled |= EQUALIZER_ENABLED;
	else
		effectsEnabled &= ~EQUALIZER_ENABLED;

	//recompute the filter if the bass boost is enabled
	if ((effectsEnabled & BASSBOOST_ENABLED)) {
		for (int32_t i = 0; i < BAND_COUNT; i++)
			computeFilter(i);
	}

	updateEffectProc();
}

uint32_t JNICALL isEqualizerEnabled(JNIEnv* env, jclass clazz) {
	return (effectsEnabled & EQUALIZER_ENABLED);
}

void JNICALL setEqualizerBandLevel(JNIEnv* env, jclass clazz, uint32_t band, int32_t level) {
	if (band >= BAND_COUNT)
		return;

	equalizerGainInDB[band] = (float)((level <= -DB_RANGE) ? -DB_RANGE : ((level >= DB_RANGE) ? DB_RANGE : level)) / 100.0f; //level is given in millibels

	//both the previous and the next bands depend on this one (if they exist)
	if (band > 0)
		computeFilter(band - 1);
	computeFilter(band);
	if (band < (equalizerMaxBandCount - 1))
		computeFilter(band + 1);
}

void JNICALL setEqualizerBandLevels(JNIEnv* env, jclass clazz, jshortArray jlevels) {
	int16_t* const levels = (int16_t*)env->GetPrimitiveArrayCritical(jlevels, 0);
	if (!levels)
		return;

	for (int32_t i = 0; i < BAND_COUNT; i++)
		equalizerGainInDB[i] = (float)((levels[i] <= -DB_RANGE) ? -DB_RANGE : ((levels[i] >= DB_RANGE) ? DB_RANGE : levels[i])) / 100.0f; //level is given in millibels

	env->ReleasePrimitiveArrayCritical(jlevels, levels, JNI_ABORT);

	for (int32_t i = 0; i < BAND_COUNT; i++)
		computeFilter(i);
}

void JNICALL enableBassBoost(JNIEnv* env, jclass clazz, uint32_t enabled) {
	if (enabled)
		effectsEnabled |= BASSBOOST_ENABLED;
	else
		effectsEnabled &= ~BASSBOOST_ENABLED;

	//recompute the entire filter (whether the bass boost is enabled or not)
	for (int32_t i = 0; i < BAND_COUNT; i++)
		computeFilter(i);

	updateEffectProc();
}

uint32_t JNICALL isBassBoostEnabled(JNIEnv* env, jclass clazz) {
	return ((effectsEnabled & BASSBOOST_ENABLED) >> 1);
}

void JNICALL setBassBoostStrength(JNIEnv* env, jclass clazz, int32_t strength) {
	bassBoostStrength = ((strength <= 0) ? 0 : ((strength >= 1000) ? 1000 : strength));

	//recompute the filter if the bass boost is enabled
	if ((effectsEnabled & BASSBOOST_ENABLED)) {
		for (int32_t i = 0; i < BAND_COUNT; i++)
			computeFilter(i);
	}
}

int32_t JNICALL getBassBoostRoundedStrength(JNIEnv* env, jclass clazz) {
	return bassBoostStrength;
}

void JNICALL enableVirtualizer(JNIEnv* env, jclass clazz, int32_t enabled) {
	if (enabled)
		effectsEnabled |= VIRTUALIZER_ENABLED;
	else
		effectsEnabled &= ~VIRTUALIZER_ENABLED;

	//recreate the filter if the virtualizer is enabled
	if ((effectsEnabled & VIRTUALIZER_ENABLED))
		virtualizerConfigChanged();
	else
		destroyVirtualizer();

	updateEffectProc();
}

uint32_t JNICALL isVirtualizerEnabled(JNIEnv* env, jclass clazz) {
	return ((effectsEnabled & VIRTUALIZER_ENABLED) >> 2);
}

void JNICALL setVirtualizerStrength(JNIEnv* env, jclass clazz, int32_t strength) {
	virtualizerStrength = ((strength <= 0) ? 0 : ((strength >= 1000) ? 1000 : strength));

	//recompute the filter if the virtualizer is enabled
	if ((effectsEnabled & VIRTUALIZER_ENABLED))
		recomputeVirtualizer();
}

int32_t JNICALL getVirtualizerRoundedStrength(JNIEnv* env, jclass clazz) {
	return virtualizerStrength;
}

void updateEffectProc() {
#ifdef FPLAY_X86
	if ((effectsEnabled & VIRTUALIZER_ENABLED))
		effectProc = ((effectsEnabled & (EQUALIZER_ENABLED | BASSBOOST_ENABLED)) ? processEffects : processVirtualizer);
	else if ((effectsEnabled & (EQUALIZER_ENABLED | BASSBOOST_ENABLED)))
		effectProc = processEqualizer;
	else
		effectProc = processNull;
#else
	if (neonMode) {
		if ((effectsEnabled & VIRTUALIZER_ENABLED))
			effectProc = ((effectsEnabled & (EQUALIZER_ENABLED | BASSBOOST_ENABLED)) ? processEffectsNeon : processVirtualizerNeon);
		else if ((effectsEnabled & (EQUALIZER_ENABLED | BASSBOOST_ENABLED)))
			effectProc = processEqualizerNeon;
		else
			effectProc = processNull;
	} else {
		if ((effectsEnabled & VIRTUALIZER_ENABLED))
			effectProc = ((effectsEnabled & (EQUALIZER_ENABLED | BASSBOOST_ENABLED)) ? processEffects : processVirtualizer);
		else if ((effectsEnabled & (EQUALIZER_ENABLED | BASSBOOST_ENABLED)))
			effectProc = processEqualizer;
		else
			effectProc = processNull;
	}
#endif
}
