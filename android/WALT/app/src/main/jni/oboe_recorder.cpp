#include "oboe_recorder.h"

#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <oboe/Oboe.h>

#include "sync_clock.h"

#define APPNAME "WALT"

namespace {

class WaltOboeRecorderCallback : public oboe::AudioStreamDataCallback {
  public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audio_stream,
                                          void *audio_data,
                                          int32_t num_frames) override {
        (void) audio_stream;

        if (!recording_active_.load(std::memory_order_acquire)) {
            return oboe::DataCallbackResult::Continue;
        }

        auto *input = static_cast<int16_t *>(audio_data);
        if (input == nullptr) {
            return oboe::DataCallbackResult::Continue;
        }

        const int32_t target_frames = target_frames_.load(std::memory_order_acquire);
        const int32_t captured_frames = frames_captured_.load(std::memory_order_acquire);
        if (target_frames <= 0 || captured_frames >= target_frames) {
            tc_rec_.store(uptimeMicros(), std::memory_order_release);
            recording_active_.store(false, std::memory_order_release);
            recording_busy_.store(false, std::memory_order_release);
            return oboe::DataCallbackResult::Stop;
        }

        const int32_t remaining_frames = target_frames - captured_frames;
        const int32_t frames_to_copy = std::min(num_frames, remaining_frames);
        if (frames_to_copy > 0) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            memcpy(recorded_wave_.data() + captured_frames,
                   input,
                   static_cast<size_t>(frames_to_copy) * sizeof(int16_t));
        }

        const int32_t next_captured_frames = captured_frames + frames_to_copy;
        frames_captured_.store(next_captured_frames, std::memory_order_release);

        if (next_captured_frames >= target_frames) {
            tc_rec_.store(uptimeMicros(), std::memory_order_release);
            recording_active_.store(false, std::memory_order_release);
            recording_busy_.store(false, std::memory_order_release);
            return oboe::DataCallbackResult::Stop;
        }

        return oboe::DataCallbackResult::Continue;
    }

    void configureTargetFrames(int32_t frames_to_record) {
        if (frames_to_record < 0) {
            frames_to_record = 0;
        }

        std::lock_guard<std::mutex> lock(buffer_mutex_);
        recorded_wave_.assign(static_cast<size_t>(frames_to_record), 0);
        target_frames_.store(frames_to_record, std::memory_order_release);
        frames_captured_.store(0, std::memory_order_release);
        te_rec_.store(0, std::memory_order_release);
        tc_rec_.store(0, std::memory_order_release);
        recording_active_.store(false, std::memory_order_release);
        recording_busy_.store(false, std::memory_order_release);
    }

    bool prepareRecordingStart() {
        if (recording_busy_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }

        frames_captured_.store(0, std::memory_order_release);
        tc_rec_.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            std::fill(recorded_wave_.begin(), recorded_wave_.end(), 0);
        }
        te_rec_.store(uptimeMicros(), std::memory_order_release);
        recording_active_.store(true, std::memory_order_release);
        return true;
    }

    void stopRecording() {
        recording_active_.store(false, std::memory_order_release);
        recording_busy_.store(false, std::memory_order_release);
    }

    int32_t getRecordedFrameCount() const {
        return target_frames_.load(std::memory_order_acquire);
    }

    int32_t copyRecordedWave(int16_t *destination, int32_t max_frames) const {
        if (destination == nullptr || max_frames <= 0) {
            return 0;
        }

        const int32_t available_frames = target_frames_.load(std::memory_order_acquire);
        const int32_t frames_to_copy = std::min(max_frames, available_frames);
        if (frames_to_copy <= 0) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (recorded_wave_.empty()) {
            return 0;
        }

        memcpy(destination, recorded_wave_.data(), static_cast<size_t>(frames_to_copy) * sizeof(int16_t));
        return frames_to_copy;
    }

    int64_t getTeRec() const {
        return te_rec_.load(std::memory_order_acquire);
    }

    int64_t getTcRec() const {
        return tc_rec_.load(std::memory_order_acquire);
    }

  private:
    mutable std::mutex buffer_mutex_;
    std::vector<int16_t> recorded_wave_;
    std::atomic<int32_t> target_frames_{0};
    std::atomic<int32_t> frames_captured_{0};
    std::atomic<int64_t> te_rec_{0};
    std::atomic<int64_t> tc_rec_{0};
    std::atomic<bool> recording_active_{false};
    std::atomic<bool> recording_busy_{false};
};

std::shared_ptr<oboe::AudioStream> g_recorder_stream;
WaltOboeRecorderCallback g_recorder_callback;
std::mutex g_recorder_mutex;

void closeRecorderLocked() {
    g_recorder_callback.stopRecording();
    if (!g_recorder_stream) {
        return;
    }

    oboe::Result stop_result = g_recorder_stream->requestStop();
    if (stop_result != oboe::Result::OK &&
        stop_result != oboe::Result::ErrorInvalidState) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME, "Failed to stop Oboe recorder: %s",
                            oboe::convertToText(stop_result));
    }

    g_recorder_stream->close();
    g_recorder_stream.reset();
}

void openRecorderLocked(int32_t frame_rate, int32_t frames_to_record) {
    closeRecorderLocked();
    g_recorder_callback.configureTargetFrames(frames_to_record);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setInputPreset(oboe::InputPreset::VoiceRecognition)
            ->setFormat(oboe::AudioFormat::I16)
            ->setChannelCount(oboe::ChannelCount::Mono)
            ->setSampleRate(frame_rate)
            ->setDataCallback(&g_recorder_callback);

    oboe::Result result = builder.openStream(g_recorder_stream);
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to open exclusive Oboe recorder (%s), retrying shared",
                            oboe::convertToText(result));
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(g_recorder_stream);
    }

    if (result != oboe::Result::OK || !g_recorder_stream) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME, "Failed to open Oboe recorder stream: %s",
                            oboe::convertToText(result));
        g_recorder_stream.reset();
        return;
    }

    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME,
                        "Created Oboe input stream. rate=%d burst=%d",
                        g_recorder_stream->getSampleRate(), g_recorder_stream->getFramesPerBurst());
}

}  // namespace

extern "C" void oboe_create_recorder(int32_t frame_rate, int32_t frames_to_record) {
    std::lock_guard<std::mutex> lock(g_recorder_mutex);
    openRecorderLocked(frame_rate, frames_to_record);
}

extern "C" void oboe_destroy_recorder(void) {
    std::lock_guard<std::mutex> lock(g_recorder_mutex);
    closeRecorderLocked();
}

extern "C" void oboe_start_recording(void) {
    std::lock_guard<std::mutex> lock(g_recorder_mutex);
    if (!g_recorder_stream) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME, "Recorder stream is not initialized");
        return;
    }

    if (!g_recorder_callback.prepareRecordingStart()) {
        return;
    }

    oboe::Result result = g_recorder_stream->requestStart();
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME, "Failed to start Oboe recorder stream: %s",
                            oboe::convertToText(result));
        g_recorder_callback.stopRecording();
    }
}

extern "C" int32_t oboe_get_recorded_frame_count(void) {
    return g_recorder_callback.getRecordedFrameCount();
}

extern "C" int32_t oboe_copy_recorded_wave(int16_t *destination, int32_t max_frames) {
    return g_recorder_callback.copyRecordedWave(destination, max_frames);
}

extern "C" int64_t oboe_get_te_rec(void) {
    return g_recorder_callback.getTeRec();
}

extern "C" int64_t oboe_get_tc_rec(void) {
    return g_recorder_callback.getTcRec();
}
