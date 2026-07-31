/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android/log.h>
#include <jni.h>
#include <malloc.h>
#include <math.h>
#include <sys/types.h>

// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include "oboe_player.h"
#include "sync_clock.h"

// logging
#define APPNAME "WALT"

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine = NULL;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;

// recorder interfaces
static SLObjectItf recorderObject = NULL;
static SLRecordItf recorderRecord;
static SLAndroidSimpleBufferQueueItf recorderBufferQueue;
static volatile int bqPlayerRecorderBusy = 0;

static unsigned int recorder_frames;
static short* recorderBuffer;
static unsigned recorderSize = 0;

#define CHANNELS 1  // 1 for mono, 2 for stereo

// Timestamps
// te - enqueue time
// tc - callback time
int64_t te_rec = 0, tc_rec = 0;

static void destroyRecorder(void) {
    bqPlayerRecorderBusy = 0;
    recorderRecord = NULL;
    recorderBufferQueue = NULL;
    if (recorderObject != NULL) {
        (*recorderObject)->Destroy(recorderObject);
        recorderObject = NULL;
    }
    if (recorderBuffer != NULL) {
        free(recorderBuffer);
        recorderBuffer = NULL;
    }
    recorder_frames = 0;
    recorderSize = 0;
}


// create the engine and output mix objects
void Java_org_chromium_latency_walt_AudioTest_createEngine(JNIEnv* env, jclass clazz)
{
    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "Creating audio engine");

    if (engineObject != NULL && engineEngine != NULL) {
        return;
    }

    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    if (SL_RESULT_SUCCESS != result || engineObject == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to create audio engine, result=%d", result);
        engineObject = NULL;
        return;
    }

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to realize audio engine, result=%d", result);
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        return;
    }

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    if (SL_RESULT_SUCCESS != result || engineEngine == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to get engine interface, result=%d", result);
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
        return;
    }

    // create output mix
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, NULL, NULL);
    if (SL_RESULT_SUCCESS != result || outputMixObject == NULL) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to create output mix, result=%d", result);
        return;
    }

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to realize output mix, result=%d", result);
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }
}

jlong Java_org_chromium_latency_walt_AudioTest_playTone(JNIEnv* env, jclass clazz){
    return (jlong) oboe_play_tone();
}

void Java_org_chromium_latency_walt_AudioTest_destroyEngine(JNIEnv *env, jclass clazz)
{
    destroyRecorder();
    oboe_destroy_player();

    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }

    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
    }
    engineEngine = NULL;
}

// create buffer queue audio player
void Java_org_chromium_latency_walt_AudioTest_createBufferQueueAudioPlayer(JNIEnv* env,
        jclass clazz, jint optimalFrameRate, jint optimalFramesPerBuffer)
{
    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME,
                        "Creating Oboe audio player with frame rate %d and frames per buffer %d",
                        optimalFrameRate, optimalFramesPerBuffer);
    oboe_create_player((int32_t) optimalFrameRate, (int32_t) optimalFramesPerBuffer);
}

void Java_org_chromium_latency_walt_AudioTest_startWarmTest(JNIEnv* env, jclass clazz) {
    oboe_start_warm_test();
}

void Java_org_chromium_latency_walt_AudioTest_stopTests(JNIEnv *env, jclass clazz) {
    oboe_stop_tests();
}

// this callback handler is called every time a buffer finishes recording
void bqRecorderCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    tc_rec = uptimeMicros();
    if (bq != recorderBufferQueue || context != NULL || recorderRecord == NULL) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME, "Invalid recorder callback state");
        bqPlayerRecorderBusy = 0;
        return;
    }

    // for streaming recording, here we would call Enqueue to give recorder the next buffer to fill
    // but instead, this is a one-time buffer so we stop recording
    SLresult result;
    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    if (SL_RESULT_SUCCESS == result) {
        recorderSize = recorder_frames * sizeof(short);
    }
    bqPlayerRecorderBusy = 0;

    //// TODO: Use small buffers and re-enqueue each time
    // result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, recorderBuffer,
    //         recorder_frames * sizeof(short));
    // assert(SL_RESULT_SUCCESS == result);
}

// create audio recorder
void Java_org_chromium_latency_walt_AudioTest_createAudioRecorder(JNIEnv* env,
    jclass clazz, jint optimalFrameRate, jint framesToRecord)
{
    SLresult result;

    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "Creating audio recorder with frame rate %d and frames to record %d",
                        optimalFrameRate, framesToRecord);
if (engineEngine == NULL) {
    __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                        "Audio engine is not initialized; recorder unavailable");
    return;
}

destroyRecorder();

// Allocate buffer
recorder_frames = framesToRecord;
recorderBuffer = malloc(sizeof(*recorderBuffer) * recorder_frames);
if (recorderBuffer == NULL) {
    __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                        "Failed to allocate recorder buffer for %u frames", recorder_frames);
    recorder_frames = 0;
    return;
}

// configure audio source
    SLDataLocator_IODevice loc_dev = {
            SL_DATALOCATOR_IODEVICE,
            SL_IODEVICE_AUDIOINPUT,
            SL_DEFAULTDEVICEID_AUDIOINPUT,
            NULL
        };
    SLDataSource audioSrc = {&loc_dev, NULL};

    // configure audio sink
    SLDataLocator_AndroidSimpleBufferQueue loc_bq;
    loc_bq.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
    loc_bq.numBuffers = 2;


    // source format
    SLDataFormat_PCM format_pcm;
    format_pcm.formatType = SL_DATAFORMAT_PCM;
    format_pcm.numChannels = CHANNELS;
    // Note: this shouldn't be called samplesPerSec it should be called *framesPerSec*
    // because when channels = 2 then there are 2 samples per frame.
    format_pcm.samplesPerSec = (SLuint32) optimalFrameRate * 1000;
    format_pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
    format_pcm.containerSize = 16;
    format_pcm.channelMask = SL_SPEAKER_FRONT_CENTER;
    format_pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;


    SLDataSink audioSnk = {&loc_bq, &format_pcm};

    // create audio recorder
    // (requires the RECORD_AUDIO permission)
    const SLInterfaceID id[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                 SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    result = (*engineEngine)->CreateAudioRecorder(engineEngine,
                                              &recorderObject,
                                              &audioSrc,
                                              &audioSnk,
                                              sizeof(id)/sizeof(id[0]),
                                              id, req);
    if (SL_RESULT_SUCCESS != result || recorderObject == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to create audio recorder, result=%d", result);
        destroyRecorder();
        return;
    }

    // Configure the voice recognition preset which has no
    // signal processing for lower latency.
    SLAndroidConfigurationItf inputConfig;
    result = (*recorderObject)->GetInterface(recorderObject,
                                            SL_IID_ANDROIDCONFIGURATION,
                                            &inputConfig);
    if (SL_RESULT_SUCCESS == result) {
        SLuint32 presetValue = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
        (*inputConfig)->SetConfiguration(inputConfig,
                                         SL_ANDROID_KEY_RECORDING_PRESET,
                                         &presetValue,
                                         sizeof(SLuint32));
    }

    // realize the audio recorder
    result = (*recorderObject)->Realize(recorderObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to realize audio recorder, result=%d", result);
        destroyRecorder();
        return;
    }

    // get the record interface
    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_RECORD, &recorderRecord);
    if (SL_RESULT_SUCCESS != result || recorderRecord == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to get recorder interface, result=%d", result);
        destroyRecorder();
        return;
    }

    // get the buffer queue interface
    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
            &recorderBufferQueue);
    if (SL_RESULT_SUCCESS != result || recorderBufferQueue == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to get recorder queue interface, result=%d", result);
        destroyRecorder();
        return;
    }

    // register callback on the buffer queue
    result = (*recorderBufferQueue)->RegisterCallback(recorderBufferQueue, bqRecorderCallback,
            NULL);
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME,
                            "Failed to register recorder callback, result=%d", result);
        destroyRecorder();
        return;
    }

    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "Audio recorder created, buffer size: %d frames",
                        recorder_frames);
}


// set the recording state for the audio recorder
void Java_org_chromium_latency_walt_AudioTest_startRecording(JNIEnv* env, jclass clazz)
{
    SLresult result;

    if (recorderRecord == NULL || recorderBufferQueue == NULL || recorderBuffer == NULL ||
        recorder_frames == 0) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Recorder is not ready; skipping recording");
        return;
    }

    if( bqPlayerRecorderBusy) {
        return;
    }
    // in case already recording, stop recording and clear buffer queue
    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to stop recorder before start, result=%d", result);
        return;
    }
    result = (*recorderBufferQueue)->Clear(recorderBufferQueue);
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to clear recorder queue, result=%d", result);
        return;
    }

    // the buffer is not valid for playback yet
    recorderSize = 0;

    // enqueue an empty buffer to be filled by the recorder
    // (for streaming recording, we would enqueue at least 2 empty buffers to start things off)
    te_rec = uptimeMicros();  // TODO: investigate if it's better to time after SetRecordState
    tc_rec = 0;
    result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, recorderBuffer,
            recorder_frames * sizeof(short));
    // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
    // which for this code example would indicate a programming error
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to enqueue recorder buffer, result=%d", result);
        return;
    }

    // start recording
    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_RECORDING);
    if (SL_RESULT_SUCCESS != result) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to start recording, result=%d", result);
        return;
    }
    bqPlayerRecorderBusy = 1;
}

jshortArray Java_org_chromium_latency_walt_AudioTest_getRecordedWave(JNIEnv *env, jclass cls)
{
    jshortArray result;
    jsize frames = recorder_frames;
    if (recorderBuffer == NULL || frames < 0) {
        frames = 0;
    }
    result = (*env)->NewShortArray(env, frames);
    if (result == NULL) {
        return NULL; /* out of memory error thrown */
    }
    if (frames > 0) {
        (*env)->SetShortArrayRegion(env, result, 0, frames, recorderBuffer);
    }
    return result;
}

jlong Java_org_chromium_latency_walt_AudioTest_getTcRec(JNIEnv *env, jclass cls) {
    return (jlong) tc_rec;
}

jlong Java_org_chromium_latency_walt_AudioTest_getTeRec(JNIEnv *env, jclass cls) {
    return (jlong) te_rec;
}

jlong Java_org_chromium_latency_walt_AudioTest_getTePlay(JNIEnv *env, jclass cls) {
    return (jlong) oboe_get_te_play();
}
