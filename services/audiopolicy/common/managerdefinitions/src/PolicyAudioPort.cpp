/*
 * Copyright (C) 2015 The Android Open Source Project
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

#define LOG_TAG "APM::PolicyAudioPort"
//#define LOG_NDEBUG 0
#include "TypeConverter.h"
#include "PolicyAudioPort.h"
#include "HwModule.h"
#include <audio_utils/audio_channel_mask_utils.h>
#include <audio_utils/audio_pcm_format_utils.h>
#include <policy.h>
#include <system/audio.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

namespace android {

// --- PolicyAudioPort class implementation
void PolicyAudioPort::attach(const sp<HwModule>& module)
{
    mModule = module;
    ALOGV("%s: attaching module %s to port %s",
            __FUNCTION__, getModuleName(), asAudioPort()->getName().c_str());
}

void PolicyAudioPort::detach()
{
    mModule = nullptr;
}

// Note that is a different namespace than AudioFlinger unique IDs
audio_port_handle_t PolicyAudioPort::getNextUniqueId()
{
    return getNextHandle();
}

audio_module_handle_t PolicyAudioPort::getModuleHandle() const
{
    return mModule != 0 ? mModule->getHandle() : AUDIO_MODULE_HANDLE_NONE;
}

uint32_t PolicyAudioPort::getModuleVersionMajor() const
{
    return mModule != 0 ? mModule->getHalVersionMajor() : 0;
}

const char *PolicyAudioPort::getModuleName() const
{
    return mModule != 0 ? mModule->getName() : "invalid module";
}

status_t PolicyAudioPort::checkExactAudioProfile(const struct audio_port_config *config) const
{
    return checkAudioProfile(config, checkExactProfile);
}

status_t PolicyAudioPort::checkIdenticalAudioProfile(const struct audio_port_config *config) const {
    return checkAudioProfile(config, checkIdenticalProfile);
}

void PolicyAudioPort::pickSamplingRate(uint32_t &pickedRate,
                                       const SampleRateSet &samplingRates) const
{
    pickedRate = 0;
    // For direct outputs, pick minimum sampling rate: this helps ensuring that the
    // channel count / sampling rate combination chosen will be supported by the connected
    // sink
    if (asAudioPort()->isDirectOutput()) {
        uint32_t samplingRate = UINT_MAX;
        for (const auto rate : samplingRates) {
            if ((rate < samplingRate) && (rate > 0)) {
                samplingRate = rate;
            }
        }
        pickedRate = (samplingRate == UINT_MAX) ? 0 : samplingRate;
    } else {
        uint32_t maxRate = SAMPLE_RATE_HZ_MAX;

        // For mixed output and inputs, use max mixer sampling rates. Do not
        // limit sampling rate otherwise
        // For inputs, also see checkCompatibleSamplingRate().
        if (asAudioPort()->getType() == AUDIO_PORT_TYPE_MIX) {
            maxRate = UINT_MAX;
        }
        // TODO: should mSamplingRates[] be ordered in terms of our preference
        // and we return the first (and hence most preferred) match?  This is of concern if
        // we want to choose 96kHz over 192kHz for USB driver stability or resource constraints.
        for (const auto rate : samplingRates) {
            if ((rate > pickedRate) && (rate <= maxRate)) {
                pickedRate = rate;
            }
        }
    }
}

void PolicyAudioPort::pickChannelMask(audio_channel_mask_t &pickedChannelMask,
                                      const ChannelMaskSet &channelMasks) const
{
    pickedChannelMask = AUDIO_CHANNEL_NONE;
    // For direct outputs, pick minimum channel count: this helps ensuring that the
    // channel count / sampling rate combination chosen will be supported by the connected
    // sink
    if (asAudioPort()->isDirectOutput()) {
        uint32_t channelCount = UINT_MAX;
        for (const auto channelMask : channelMasks) {
            uint32_t cnlCount;
            if (asAudioPort()->useInputChannelMask()) {
                cnlCount = audio_channel_count_from_in_mask(channelMask);
            } else {
                cnlCount = audio_channel_count_from_out_mask(channelMask);
            }
            if ((cnlCount < channelCount) && (cnlCount > 0)) {
                pickedChannelMask = channelMask;
                channelCount = cnlCount;
            }
        }
    } else {
        uint32_t channelCount = 0;
        uint32_t maxCount = MAX_MIXER_CHANNEL_COUNT;

        // For mixed output and inputs, use max mixer channel count. Do not
        // limit channel count otherwise
        if (asAudioPort()->getType() != AUDIO_PORT_TYPE_MIX) {
            maxCount = UINT_MAX;
        }
        // Only output mixer ports actually go through AudioFlinger's mixer, so
        // only for those does mixer compatibility matter when breaking ties
        // between candidates of the same channel count.
        const bool isMixerOutputPort = !asAudioPort()->useInputChannelMask()
                && asAudioPort()->getType() == AUDIO_PORT_TYPE_MIX;
        for (const auto channelMask : channelMasks) {
            uint32_t cnlCount;
            if (asAudioPort()->useInputChannelMask()) {
                cnlCount = audio_channel_count_from_in_mask(channelMask);
            } else {
                cnlCount = audio_channel_count_from_out_mask(channelMask);
            }
            if (cnlCount > maxCount) {
                continue;
            }
            if (cnlCount > channelCount) {
                pickedChannelMask = channelMask;
                channelCount = cnlCount;
            } else if (cnlCount == channelCount && isMixerOutputPort
                    && !audio_utils::isMixerCompatibleChannelMask(
                            pickedChannelMask, FCC_2, maxCount)
                    && audio_utils::isMixerCompatibleChannelMask(
                            channelMask, FCC_2, maxCount)) {
                // Prefer the candidate the mixer will actually accept, e.g. an
                // INDEX mask over a non-canonical POSITION mask advertising the
                // same channel count (seen on some USB devices).
                pickedChannelMask = channelMask;
            }
        }
    }
}

int PolicyAudioPort::compareFormats(audio_format_t format1, audio_format_t format2)
{
    // NOTE: AUDIO_FORMAT_INVALID is also considered not PCM and will be compared equal to any
    // compressed format and better than any PCM format. This is by design of pickFormat()
    if (!audio_is_linear_pcm(format1)) {
        if (!audio_is_linear_pcm(format2)) {
            return 0;
        }
        return 1;
    }
    if (!audio_is_linear_pcm(format2)) {
        return -1;
    }

    // format not found => index < 0 => treated as worse than any found format
    const int index1 = audio_utils::pcmFormatRankIndex(format1);
    const int index2 = audio_utils::pcmFormatRankIndex(format2);
    return index1 - index2;
}

uint32_t PolicyAudioPort::formatDistance(audio_format_t format1, audio_format_t format2)
{
    if (format1 == format2) {
        return 0;
    }
    if (format1 == AUDIO_FORMAT_INVALID || format2 == AUDIO_FORMAT_INVALID) {
        return kFormatDistanceMax;
    }
    int diffBytes = (int)audio_bytes_per_sample(format1) -
            audio_bytes_per_sample(format2);

    return abs(diffBytes);
}

bool PolicyAudioPort::isBetterFormatMatch(audio_format_t newFormat,
                                          audio_format_t currentFormat,
                                          audio_format_t targetFormat)
{
    return formatDistance(newFormat, targetFormat) < formatDistance(currentFormat, targetFormat);
}

void PolicyAudioPort::pickAudioProfile(uint32_t &samplingRate,
                                       audio_channel_mask_t &channelMask,
                                       audio_format_t &format) const
{
    format = AUDIO_FORMAT_DEFAULT;
    samplingRate = 0;
    channelMask = AUDIO_CHANNEL_NONE;

    // special case for uninitialized dynamic profile
    if (!asAudioPort()->hasValidAudioProfile()) {
        return;
    }
    audio_format_t bestFormat = audio_utils::pcmBestSinkFormat();

    // For mixed output and inputs, use best mixer output format.
    // Do not limit format otherwise
    if ((asAudioPort()->getType() != AUDIO_PORT_TYPE_MIX) || asAudioPort()->isDirectOutput()) {
        bestFormat = AUDIO_FORMAT_INVALID;
    }

    const bool directOutput = asAudioPort()->isDirectOutput();
    const bool inputChannelMask = asAudioPort()->useInputChannelMask();
    uint32_t channelCount = directOutput ? UINT_MAX : 0;

    const AudioProfileVector& audioProfiles = asAudioPort()->getAudioProfiles();
    for (size_t i = 0; i < audioProfiles.size(); i ++) {
        if (!audioProfiles[i]->isValid()) {
            continue;
        }
        audio_format_t formatToCompare = audioProfiles[i]->getFormat();
        if (formatToCompare == AUDIO_FORMAT_DEFAULT
                || compareFormats(formatToCompare, bestFormat) > 0) {
            continue;
        }

        uint32_t pickedSamplingRate = 0;
        audio_channel_mask_t pickedChannelMask = AUDIO_CHANNEL_NONE;
        pickChannelMask(pickedChannelMask, audioProfiles[i]->getChannels());
        pickSamplingRate(pickedSamplingRate, audioProfiles[i]->getSampleRates());

        if (pickedChannelMask == AUDIO_CHANNEL_NONE || pickedSamplingRate == 0) {
            continue;
        }

        // Precedence: Format > Channel count > Sampling rate
        const uint32_t pickedChannelCount =
                inputChannelMask ? audio_channel_count_from_in_mask(pickedChannelMask)
                                 : audio_channel_count_from_out_mask(pickedChannelMask);
        const int formatComparison = compareFormats(formatToCompare, format);
        bool betterProfile = false;
        const char* reason = "";
        if (formatComparison > 0) {
            betterProfile = true;
            reason = "format";
        } else if (formatComparison == 0) {
            if (directOutput ? pickedChannelCount < channelCount
                             : pickedChannelCount > channelCount) {
                betterProfile = true;
                reason = "channelCount";
            } else if (pickedChannelCount == channelCount) {
                if (directOutput ? pickedSamplingRate < samplingRate
                                 : pickedSamplingRate > samplingRate) {
                    betterProfile = true;
                    reason = "samplingRate";
                }
            }
        }

        if (betterProfile) {
            ALOGV("%s Port[nm:%s] select profile[%zu] reason=%s rate=%u format=0x%x "
                  "mask=0x%x ch=%u previous rate=%u format=0x%x mask=0x%x ch=%u",
                  __func__, asAudioPort()->getName().c_str(), i, reason, pickedSamplingRate,
                  formatToCompare, pickedChannelMask, pickedChannelCount, samplingRate, format,
                  channelMask, channelCount == UINT_MAX ? 0 : channelCount);
            format = formatToCompare;
            channelMask = pickedChannelMask;
            samplingRate = pickedSamplingRate;
            channelCount = pickedChannelCount;
        }
    }
    ALOGV("%s Port[nm:%s] profile rate=%u, format=0x%x, mask=0x%x", __func__,
          asAudioPort()->getName().c_str(), samplingRate, format, channelMask);
}

status_t PolicyAudioPort::checkAudioProfile(
        const struct audio_port_config *config,
        std::function<status_t(const AudioProfileVector &,
                               const uint32_t,
                               audio_channel_mask_t,
                               audio_format_t)> checkProfile) const {
    status_t status = NO_ERROR;
    auto config_mask = config->config_mask;
    if (config_mask & AUDIO_PORT_CONFIG_GAIN) {
        config_mask &= ~AUDIO_PORT_CONFIG_GAIN;
        status = asAudioPort()->checkGain(&config->gain, config->gain.index);
        if (status != NO_ERROR) {
            return status;
        }
    }
    if (config_mask != 0) {
        // TODO should we check sample_rate / channel_mask / format separately?
        status = checkProfile(asAudioPort()->getAudioProfiles(), config->sample_rate,
                   config->channel_mask, config->format);
    }
    return status;

}

} // namespace android
