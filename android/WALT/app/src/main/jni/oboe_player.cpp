#include "oboe_player.h"

#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <oboe/Oboe.h>

#include "sync_clock.h"

#define APPNAME "WALT"
#define MAXIMUM_AMPLITUDE_VALUE 32767
#define BUFFERS_TO_PLAY 10

namespace {

class WaltOboeCallback : public oboe::AudioStreamDataCallback {
  public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audio_stream,
                                          void *audio_data,
                                          int32_t num_frames) override {
        (void) audio_stream;
        auto *out = static_cast<int16_t *>(audio_data);

        if (tone_frames_remaining_.load(std::memory_order_acquire) > 0 &&
            first_tone_frame_pending_.exchange(false, std::memory_order_acq_rel)) {
            te_play_.store(uptimeMicros(), std::memory_order_release);
        }

        for (int32_t i = 0; i < num_frames; ++i) {
            int remaining = tone_frames_remaining_.load(std::memory_order_acquire);
            if (remaining > 0) {
                uint32_t wave_index = wave_index_.fetch_add(1, std::memory_order_acq_rel);
                out[i] = ((wave_index & 0x2) ? MAXIMUM_AMPLITUDE_VALUE : -MAXIMUM_AMPLITUDE_VALUE);
                tone_frames_remaining_.store(remaining - 1, std::memory_order_release);
            } else {
                out[i] = 0;
            }
        }

        if (!warmed_up_.load(std::memory_order_acquire) &&
            tone_frames_remaining_.load(std::memory_order_acquire) == 0) {
            return oboe::DataCallbackResult::Stop;
        }

        return oboe::DataCallbackResult::Continue;
    }

    void setFramesPerBurst(int32_t frames_per_burst) {
        frames_per_burst_.store(frames_per_burst, std::memory_order_release);
    }

    void setWarmedUp(bool warmed_up) {
        warmed_up_.store(warmed_up, std::memory_order_release);
    }

    void primeTone() {
        tone_frames_remaining_.store(BUFFERS_TO_PLAY * frames_per_burst_.load(std::memory_order_acquire),
                                     std::memory_order_release);
        first_tone_frame_pending_.store(true, std::memory_order_release);
        wave_index_.store(0, std::memory_order_release);
    }

    int64_t getPlayTimestamp() const {
        return te_play_.load(std::memory_order_acquire);
    }

    void setPlayTimestamp(int64_t timestamp_us) {
        te_play_.store(timestamp_us, std::memory_order_release);
    }

  private:
    std::atomic<int64_t> te_play_{0};
    std::atomic<int32_t> frames_per_burst_{192};
    std::atomic<int32_t> tone_frames_remaining_{0};
    std::atomic<bool> warmed_up_{false};
    std::atomic<bool> first_tone_frame_pending_{false};
    std::atomic<uint32_t> wave_index_{0};
};

std::shared_ptr<oboe::AudioStream> g_stream;
WaltOboeCallback g_callback;
std::mutex g_stream_mutex;

void closeStreamLocked() {
    if (g_stream) {
        g_stream->close();
        g_stream.reset();
    }
}

void openStreamLocked(int32_t frame_rate, int32_t frames_per_burst) {
    closeStreamLocked();

    g_callback.setFramesPerBurst(frames_per_burst);
    g_callback.setWarmedUp(false);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setUsage(oboe::Usage::Game)
            ->setContentType(oboe::ContentType::Sonification)
            ->setFormat(oboe::AudioFormat::I16)
            ->setChannelCount(oboe::ChannelCount::Mono)
            ->setSampleRate(frame_rate)
            ->setFramesPerDataCallback(frames_per_burst)
            ->setDataCallback(&g_callback);

    oboe::Result result = builder.openStream(g_stream);
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME,
                            "Failed to open exclusive stream (%s), retrying shared",
                            oboe::convertToText(result));
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(g_stream);
    }

    if (result != oboe::Result::OK || !g_stream) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME, "Failed to open Oboe stream: %s",
                            oboe::convertToText(result));
        g_stream.reset();
        return;
    }

    __android_log_print(ANDROID_LOG_VERBOSE, APPNAME,
                        "Created Oboe output stream. rate=%d burst=%d",
                        g_stream->getSampleRate(), g_stream->getFramesPerBurst());
}

}  // namespace

extern "C" void oboe_create_player(int32_t frame_rate, int32_t frames_per_burst) {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    openStreamLocked(frame_rate, frames_per_burst);
}

extern "C" void oboe_destroy_player(void) {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    closeStreamLocked();
}

extern "C" int64_t oboe_play_tone(void) {
    int64_t t_start = uptimeMicros();
    g_callback.setPlayTimestamp(0);
    g_callback.primeTone();

    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (!g_stream) return t_start;

    if (!g_stream->isXRunCountSupported()) {
        __android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "Oboe stream xrun count not supported");
    }

    if (!g_callback.getPlayTimestamp()) {
        g_callback.setPlayTimestamp(t_start);
    }

    oboe::Result result = g_stream->requestStart();
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME, "Failed to start Oboe stream: %s",
                            oboe::convertToText(result));
    }
    return t_start;
}

extern "C" void oboe_start_warm_test(void) {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (!g_stream) return;
    g_callback.setWarmedUp(true);
    oboe::Result result = g_stream->requestStart();
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, APPNAME, "Failed to warm Oboe stream: %s",
                            oboe::convertToText(result));
    }
}

extern "C" void oboe_stop_tests(void) {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    g_callback.setWarmedUp(false);
    if (!g_stream) return;
    oboe::Result result = g_stream->requestStop();
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_WARN, APPNAME, "Failed to stop Oboe stream: %s",
                            oboe::convertToText(result));
    }
}

extern "C" int64_t oboe_get_te_play(void) {
    return g_callback.getPlayTimestamp();
}
