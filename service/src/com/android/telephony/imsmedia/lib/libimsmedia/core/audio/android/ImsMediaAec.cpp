/**
 * Copyright (C) 2026 The Android Open Source Project
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

#include <ImsMediaAec.h>

#include <android/log.h>
#include <modules/audio_processing/include/audio_processing.h>

#include <algorithm>
#include <cmath>

#define AEC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ims-aec", __VA_ARGS__)
#define AEC_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ims-aec", __VA_ARGS__)

// Near-end voice activity threshold on the raw capture RMS (int16 scale,
// 1200 ≈ -28dBFS): above it the local user is speaking and the processed
// uplink passes untouched.
static constexpr float kNearSpeechRmsThresh = 1200.0f;
// Gain applied to the processed uplink while the near end is silent: enough to
// break the speakerphone howl loop, small enough to stay invisible for normal
// conversation (residual echo only). -34dB: even a strong residual cannot
// sustain the loop.
static constexpr float kSilenceGain = 0.02f;
static constexpr float kNearSpeechGain = 1.0f;

ImsMediaAec* ImsMediaAec::GetInstance()
{
    static ImsMediaAec sInstance;
    return &sInstance;
}

void ImsMediaAec::EnsureInit_l(int32_t sampleRate)
{
    if (mApm != nullptr)
    {
        return;
    }

    mApm = webrtc::AudioProcessingBuilder().Create();
    if (mApm == nullptr)
    {
        AEC_LOGW("[EnsureInit] AudioProcessingBuilder failed");
        return;
    }

    // AEC3 gives far better suppression on mobile loudspeakers than AECM and
    // estimates the render/capture delay itself (we always declare 0 ms).
    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = true;
    config.echo_canceller.mobile_mode = false;
    config.noise_suppression.enabled = true;
    config.high_pass_filter.enabled = true;
    mApm->ApplyConfig(config);

    mEnabled = true;
    AEC_LOGI("[EnsureInit] created APM, AEC3+NS+HPF, rate[%d]", sampleRate);
}

void ImsMediaAec::FeedFarEnd(
        const int16_t* data, int32_t numFrames, int32_t sampleRate, int32_t delayMs)
{
    if (data == nullptr || numFrames <= 0 || sampleRate <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    EnsureInit_l(sampleRate);
    if (mApm == nullptr || !mEnabled)
    {
        return;
    }

    mRenderRate = sampleRate;
    mDelayMs = delayMs;
    // Track far-end loudness (smoothed RMS over processed chunks) for the
    // residual suppressor: FeedFarEnd is called for silence frames too, so this
    // decays naturally when the far end goes quiet.
    const int32_t frame = sampleRate / 100;  // 10 ms of mono samples
    if (frame <= 0)
    {
        return;
    }
    mFarAcc.insert(mFarAcc.end(), data, data + numFrames);
    std::vector<int16_t> scratch(frame);
    webrtc::StreamConfig cfg(sampleRate, 1);
    while ((int32_t)mFarAcc.size() >= frame)
    {
        int ret = mApm->ProcessReverseStream(mFarAcc.data(), cfg, cfg, scratch.data());
        if (ret != webrtc::AudioProcessing::kNoError)
        {
            AEC_LOGW("[FeedFarEnd] ProcessReverseStream failed[%d]", ret);
            mFarAcc.clear();
            return;
        }
        double acc = 0;
        for (int32_t i = 0; i < frame; i++)
        {
            acc += (double)scratch[i] * scratch[i];
        }
        const float rms = (float)sqrt(acc / frame);
        mFarRms = 0.8f * mFarRms + 0.2f * rms;
        mFarAcc.erase(mFarAcc.begin(), mFarAcc.begin() + frame);
    }
}

void ImsMediaAec::ProcessNear(int16_t* data, int32_t numFrames, int32_t sampleRate)
{
    if (data == nullptr || numFrames <= 0 || sampleRate <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    EnsureInit_l(sampleRate);
    if (mApm == nullptr || !mEnabled)
    {
        return;
    }

    mCaptureRate = sampleRate;
    const int32_t frame = sampleRate / 100;  // 10 ms of mono samples
    if (frame <= 0)
    {
        return;
    }

    mNearAcc.insert(mNearAcc.end(), data, data + numFrames);
    std::vector<int16_t> scratch(frame);
    webrtc::StreamConfig cfg(sampleRate, 1);
    static uint32_t sTuneLogCounter = 0;
    while ((int32_t)mNearAcc.size() >= frame)
    {
        // Near-end voice activity on the raw capture: while the local user is
        // talking the processed audio passes untouched; while they are silent
        // any significant remaining energy is echo residual, and attenuating it
        // breaks the speakerphone howl loop (mute stops the squeal the same
        // way, this only ducks the residual instead of cutting the mic).
        double acc = 0;
        for (int32_t i = 0; i < frame; i++)
        {
            const int16_t v = mNearAcc[i];
            acc += (double)v * v;
        }
        const float rawRms = (float)sqrt(acc / frame);
        const bool nearSpeaking = rawRms > kNearSpeechRmsThresh;

        mApm->set_stream_delay_ms(mDelayMs);
        int ret = mApm->ProcessStream(mNearAcc.data(), cfg, cfg, scratch.data());
        if (ret != webrtc::AudioProcessing::kNoError)
        {
            AEC_LOGW("[ProcessNear] ProcessStream failed[%d]", ret);
            mNearAcc.clear();
            mOutAcc.clear();
            return;
        }

        if (nearSpeaking)
        {
            mGain = kNearSpeechGain;
        }
        else
        {
            mGain += (kSilenceGain - mGain) * 0.7f;  // ~2-3 chunks to full duck
        }
        if (mGain < 0.999f)
        {
            for (int32_t i = 0; i < frame; i++)
            {
                scratch[i] = (int16_t)std::max(-32768,
                        std::min(32767, (int)(scratch[i] * mGain)));
            }
        }
        mOutAcc.insert(mOutAcc.end(), scratch.begin(), scratch.end());
        mNearAcc.erase(mNearAcc.begin(), mNearAcc.begin() + frame);

        if (++sTuneLogCounter >= 500)
        {
            sTuneLogCounter = 0;
            AEC_LOGI("[ProcessNear] rawRms[%.0f] farRms[%.0f] gain[%.2f] delayMs[%d]",
                    rawRms, mFarRms, mGain, mDelayMs);
        }
    }

    // Hand back exactly the burst that was passed in; keep any surplus
    // processed audio queued for the next burst.
    const int32_t out = (int32_t)mOutAcc.size() < numFrames ? (int32_t)mOutAcc.size() : numFrames;
    memcpy(data, mOutAcc.data(), out * sizeof(int16_t));
    mOutAcc.erase(mOutAcc.begin(), mOutAcc.begin() + out);
    if (out < numFrames)
    {
        // Input lagged (first burst shorter than 10 ms): leave the raw tail.
        AEC_LOGW("[ProcessNear] short output out[%d] of[%d]", out, numFrames);
    }
}
