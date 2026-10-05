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

#define AEC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ims-aec", __VA_ARGS__)
#define AEC_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ims-aec", __VA_ARGS__)

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
    static uint32_t sDelayLogCounter = 0;
    if (++sDelayLogCounter >= 100)
    {
        sDelayLogCounter = 0;
        AEC_LOGI("[FeedFarEnd] delayMs[%d]", delayMs);
    }
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
    while ((int32_t)mNearAcc.size() >= frame)
    {
        // Render/capture alignment hint: measured AAudio/HAL playback buffering
        // of the far-end reference. AEC3 refines the residual offset itself.
        mApm->set_stream_delay_ms(mDelayMs);
        int ret = mApm->ProcessStream(mNearAcc.data(), cfg, cfg, scratch.data());
        if (ret != webrtc::AudioProcessing::kNoError)
        {
            AEC_LOGW("[ProcessNear] ProcessStream failed[%d]", ret);
            mNearAcc.clear();
            mOutAcc.clear();
            return;
        }
        mOutAcc.insert(mOutAcc.end(), scratch.begin(), scratch.end());
        mNearAcc.erase(mNearAcc.begin(), mNearAcc.begin() + frame);
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
