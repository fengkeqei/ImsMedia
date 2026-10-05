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

#ifndef IMSMEDIA_AEC_H_INCLUDED
#define IMSMEDIA_AEC_H_INCLUDED

#include <cstdint>
#include <mutex>
#include <vector>

#include <api/scoped_refptr.h>

namespace webrtc
{
class AudioProcessing;
}

/**
 * Round-72 72-2n: in-process acoustic echo canceller for the userspace IMS
 * voice path.
 *
 * AOSP no longer feeds a far-end (reverse) stream to the pre-processing AEC
 * effect (processReverse() has no callers in the A15 effect framework), so the
 * software preprocessor attached by AudioPolicyEffects runs without a reference
 * and cannot suppress loudspeaker echo. Both directions of the IMS call live in
 * this process, so run WebRTC AudioProcessing here: the decoded downlink is fed
 * as the far-end reference (FeedFarEnd, right before rendering) and the captured
 * uplink is filtered in place through ProcessNear before encoding.
 *
 * All methods may be called from any thread; the instance serializes access
 * internally. Audio is accumulated internally into the ~10 ms frames
 * AudioProcessing requires.
 */
class ImsMediaAec
{
public:
    static ImsMediaAec* GetInstance();

    /** Feed decoded downlink PCM (host order int16, mono) as the far-end echo
     * reference. Call right before the frames are handed to the audio sink. */
    void FeedFarEnd(const int16_t* data, int32_t numFrames, int32_t sampleRate);

    /** Filter captured uplink PCM in place before encoding. */
    void ProcessNear(int16_t* data, int32_t numFrames, int32_t sampleRate);

private:
    ImsMediaAec() = default;
    void EnsureInit_l(int32_t sampleRate);

    std::mutex mMutex;
    rtc::scoped_refptr<webrtc::AudioProcessing> mApm;
    bool mEnabled = false;
    int32_t mCaptureRate = 0;
    int32_t mRenderRate = 0;
    std::vector<int16_t> mFarAcc;
    std::vector<int16_t> mNearAcc;
    std::vector<int16_t> mOutAcc;
};

#endif  // IMSMEDIA_AEC_H_INCLUDED
