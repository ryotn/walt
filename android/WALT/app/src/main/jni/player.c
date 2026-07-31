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
#include <stdint.h>

#include "oboe_player.h"
#include "oboe_recorder.h"

#define APPNAME "WALT"

void Java_org_chromium_latency_walt_AudioTest_createEngine(JNIEnv* env, jclass clazz) {
    (void) env;
    (void) clazz;
    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "Initializing native audio");
}

jlong Java_org_chromium_latency_walt_AudioTest_playTone(JNIEnv* env, jclass clazz){
    (void) env;
    (void) clazz;
    return (jlong) oboe_play_tone();
}

void Java_org_chromium_latency_walt_AudioTest_destroyEngine(JNIEnv *env, jclass clazz) {
    (void) env;
    (void) clazz;
    oboe_destroy_recorder();
    oboe_destroy_player();
}

void Java_org_chromium_latency_walt_AudioTest_createBufferQueueAudioPlayer(JNIEnv* env,
        jclass clazz, jint optimalFrameRate, jint optimalFramesPerBuffer) {
    (void) env;
    (void) clazz;
    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME,
                        "Creating Oboe audio player with frame rate %d and frames per buffer %d",
                        optimalFrameRate, optimalFramesPerBuffer);
    oboe_create_player((int32_t) optimalFrameRate, (int32_t) optimalFramesPerBuffer);
}

void Java_org_chromium_latency_walt_AudioTest_startWarmTest(JNIEnv* env, jclass clazz) {
    (void) env;
    (void) clazz;
    oboe_start_warm_test();
}

void Java_org_chromium_latency_walt_AudioTest_stopTests(JNIEnv *env, jclass clazz) {
    (void) env;
    (void) clazz;
    oboe_stop_tests();
}

void Java_org_chromium_latency_walt_AudioTest_createAudioRecorder(JNIEnv* env,
        jclass clazz, jint optimalFrameRate, jint framesToRecord) {
    (void) env;
    (void) clazz;
    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME,
                        "Creating Oboe audio recorder with frame rate %d and frames to record %d",
                        optimalFrameRate, framesToRecord);
    oboe_create_recorder((int32_t) optimalFrameRate, (int32_t) framesToRecord);
}

void Java_org_chromium_latency_walt_AudioTest_startRecording(JNIEnv* env, jclass clazz) {
    (void) env;
    (void) clazz;
    oboe_start_recording();
}

jshortArray Java_org_chromium_latency_walt_AudioTest_getRecordedWave(JNIEnv *env, jclass cls) {
    (void) cls;
    int32_t recorder_frames = oboe_get_recorded_frame_count();
    if (recorder_frames < 0) {
        recorder_frames = 0;
    }

    jshortArray result = (*env)->NewShortArray(env, (jsize) recorder_frames);
    if (result == NULL || recorder_frames == 0) {
        return result;
    }

    jshort *wave = (*env)->GetShortArrayElements(env, result, NULL);
    if (wave == NULL) {
        return result;
    }
    oboe_copy_recorded_wave((int16_t *) wave, recorder_frames);
    (*env)->ReleaseShortArrayElements(env, result, wave, 0);

    return result;
}

jlong Java_org_chromium_latency_walt_AudioTest_getTcRec(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    return (jlong) oboe_get_tc_rec();
}

jlong Java_org_chromium_latency_walt_AudioTest_getTeRec(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    return (jlong) oboe_get_te_rec();
}

jlong Java_org_chromium_latency_walt_AudioTest_getTePlay(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    return (jlong) oboe_get_te_play();
}
