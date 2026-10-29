/*
 ==============================================================================
 
 This file is part of SPARTA; a suite of spatial audio plug-ins.
 Copyright (c) 2018 - Leo McCormack.
 
 SPARTA is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 SPARTA is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with SPARTA.  If not, see <http://www.gnu.org/licenses/>.
 
 ==============================================================================
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#if AMBIBIN_MULTIBUS
#include "pluginterfaces/vst/ivsteditcontroller.h"
#endif

namespace {
enum FeedIndex {
    kFeedLeft = 0,
    kFeedRight,
    kFeedFront,
    kFeedBack
};

struct EarFeedGains {
    std::array<float, 4> left{};
    std::array<float, 4> right{};
};

using RotationMatrix = std::array<std::array<float, 3>, 3>;

RotationMatrix multiplyRotationMatrices(const RotationMatrix& left,
                                        const RotationMatrix& right)
{
    RotationMatrix result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            for (int inner = 0; inner < 3; ++inner)
                result[(size_t)row][(size_t)column] += left[(size_t)row][(size_t)inner]
                                                     * right[(size_t)inner][(size_t)column];
    return result;
}

RotationMatrix getXRotation(float angle)
{
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return {{
        {{ 1.0f, 0.0f, 0.0f }},
        {{ 0.0f, cosine, sine }},
        {{ 0.0f, -sine, cosine }}
    }};
}

RotationMatrix getYRotation(float angle)
{
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return {{
        {{ cosine, 0.0f, -sine }},
        {{ 0.0f, 1.0f, 0.0f }},
        {{ sine, 0.0f, cosine }}
    }};
}

RotationMatrix getZRotation(float angle)
{
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return {{
        {{ cosine, sine, 0.0f }},
        {{ -sine, cosine, 0.0f }},
        {{ 0.0f, 0.0f, 1.0f }}
    }};
}

RotationMatrix getAnatomicalHeadRotation(float yaw,
                                         float pitch,
                                         float roll,
                                         bool useRollPitchYaw)
{
    const auto yawRotation = getZRotation(yaw);
    const auto pitchRotation = getYRotation(pitch);
    const auto rollRotation = getXRotation(roll);

    /* This matrix maps head-local axes into the fixed microphone-rig frame.
       For YPR, roll acts about the already-yawed and pitched head-forward
       axis; the alternative setting applies the corresponding RPY order.
       This is deliberately separate from SAF's sound-field rotation matrix,
       which maps in the opposite conceptual direction for decoding. */
    if (useRollPitchYaw)
        return multiplyRotationMatrices(
            multiplyRotationMatrices(rollRotation, pitchRotation),
            yawRotation);

    return multiplyRotationMatrices(
        multiplyRotationMatrices(yawRotation, pitchRotation),
        rollRotation);
}

void normaliseEarWeights(std::array<float, 4>& weights)
{
    float powerSum = 0.0f;
    for (const float weight : weights)
        powerSum += weight * weight;

    if (powerSum > 0.0f) {
        const float normalisation = 1.0f / std::sqrt(powerSum);
        for (float& weight : weights)
            weight *= normalisation;
    }
}

void focusAndNormaliseEarWeights(std::array<float, 4>& weights, float focus)
{
    const float exponent = std::max(1.0f, focus);

    for (float& weight : weights)
        weight = std::pow(weight, exponent);

    normaliseEarWeights(weights);
}

std::array<float, 4> getPlanarFeedWeights(float mixerX,
                                          float mixerY,
                                          float blendFocus)
{
    std::array<float, 4> weights{};
    const float length = std::sqrt(mixerX * mixerX + mixerY * mixerY);
    if (length <= 1.0e-6f)
        return weights;

    mixerX /= length;
    mixerY /= length;

    /* Diamond-array positions in mixer coordinates: x points right and y
       points forward. */
    weights[(size_t)kFeedLeft] = std::max(0.0f, -mixerX);
    weights[(size_t)kFeedRight] = std::max(0.0f, mixerX);
    weights[(size_t)kFeedFront] = std::max(0.0f, mixerY);
    weights[(size_t)kFeedBack] = std::max(0.0f, -mixerY);
    focusAndNormaliseEarWeights(weights, blendFocus);
    return weights;
}

std::array<float, 3> rotateVector(const RotationMatrix& rotation,
                                  const std::array<float, 3>& vector)
{
    std::array<float, 3> result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            result[(size_t)row] += rotation[row][column] * vector[(size_t)column];
    return result;
}

std::array<float, 2> safToMixerPlane(const std::array<float, 3>& safVector)
{
    /* SAF uses +x forward and +y left. The mixer geometry uses +x right and
       +y forward, matching the established yaw-only feed mapping. */
    return { -safVector[1], safVector[0] };
}

EarFeedGains getEarFeedGains(void* hAmbi, float blendFocus, int numFeeds)
{
    EarFeedGains gains;
    RotationMatrix rotation {{
        {{ 1.0f, 0.0f, 0.0f }},
        {{ 0.0f, 1.0f, 0.0f }},
        {{ 0.0f, 0.0f, 1.0f }}
    }};

    if (ambi_bin_getEnableRotation(hAmbi) != 0) {
        /* The getters return the displayed/tracker angles independent of the
           SAF decoder's axis-flip switches. Feed selection represents the
           physical head/receiver geometry, so it must follow these unflipped
           angles just as the original yaw-only mixer did. Reapplying the flip
           flags here reverses the front/back ear assignment when Flip Yaw is
           enabled. */
        const float yaw = ambi_bin_getYaw(hAmbi) * float_Pi / 180.0f;
        const float pitch = ambi_bin_getPitch(hAmbi) * float_Pi / 180.0f;
        const float roll = ambi_bin_getRoll(hAmbi) * float_Pi / 180.0f;

        rotation = getAnatomicalHeadRotation(yaw,
                                             pitch,
                                             roll,
                                             ambi_bin_getRPYflag(hAmbi) != 0);
    }

    /* SAF coordinates: +x is forward, +y is left and +z is up. */
    const auto leftEar = rotateVector(rotation, { 0.0f, 1.0f, 0.0f });
    const auto rightEar = rotateVector(rotation, { 0.0f, -1.0f, 0.0f });
    const auto forward = rotateVector(rotation, { 1.0f, 0.0f, 0.0f });

    if (numFeeds == 2) {
        /* Interpolate each ear along the physical Left/Right baseline. Unlike
           positive projection, this gives two nonzero weights at sideways
           orientations instead of an undefined selection followed by a swap.
           Keep the full 3D ear coordinate: vertical/fore-aft displacement
           reduces the lateral separation instead of being normalised away. */
        const float lateral = std::clamp(leftEar[1], -1.0f, 1.0f);
        gains.left[kFeedLeft] = 0.5f * (1.0f + lateral);
        gains.left[kFeedRight] = 0.5f * (1.0f - lateral);
        gains.right[kFeedLeft] = gains.left[kFeedRight];
        gains.right[kFeedRight] = gains.left[kFeedLeft];
        focusAndNormaliseEarWeights(gains.left, blendFocus);
        focusAndNormaliseEarWeights(gains.right, blendFocus);
        return gains;
    }

    const auto leftPlanar = safToMixerPlane(leftEar);
    const auto rightPlanar = safToMixerPlane(rightEar);
    const auto forwardPlanar = safToMixerPlane(forward);

    auto distributedLeft = getPlanarFeedWeights(leftPlanar[0], leftPlanar[1], blendFocus);
    auto distributedRight = getPlanarFeedWeights(rightPlanar[0], rightPlanar[1], blendFocus);
    auto reference = getPlanarFeedWeights(forwardPlanar[0], forwardPlanar[1], blendFocus);

    /* The four receiver centres occupy only the horizontal plane. As the ear
       axis tilts out of that plane, fade toward a common receiver perspective
       for both ears instead of inventing a full-strength horizontal baseline.
       The common perspective follows the projected head-forward direction. */
    const float planarConfidence = std::clamp(
        std::sqrt(leftEar[0] * leftEar[0] + leftEar[1] * leftEar[1]),
        0.0f,
        1.0f);

    if (std::all_of(reference.begin(), reference.end(), [](float value) { return value == 0.0f; }))
        reference[(size_t)kFeedFront] = 1.0f;

    for (int feed = 0; feed < 4; ++feed) {
        gains.left[(size_t)feed] = planarConfidence * distributedLeft[(size_t)feed]
                                + (1.0f - planarConfidence) * reference[(size_t)feed];
        gains.right[(size_t)feed] = planarConfidence * distributedRight[(size_t)feed]
                                 + (1.0f - planarConfidence) * reference[(size_t)feed];
    }

    normaliseEarWeights(gains.left);
    normaliseEarWeights(gains.right);

    return gains;
}
}

PluginProcessor::PluginProcessor() :
    AudioProcessor(BusesProperties()
#if AMBIBIN_MULTIBUS
        .withInput("Left microphone", AudioChannelSet::ambisonic(2), true)
        .withInput("Right microphone", AudioChannelSet::ambisonic(2), true)
        .withInput("Front microphone", AudioChannelSet::ambisonic(2), true)
        .withInput("Back microphone", AudioChannelSet::ambisonic(2), true)
        .withOutput("Binaural", AudioChannelSet::stereo(), true))
#else
        .withInput("Input", AudioChannelSet::discreteChannels(MAX_NUM_CHANNELS), true)
        .withOutput("Output", AudioChannelSet::discreteChannels(2), true))
#endif
{
    for (void*& hAmbi : hAmbis) {
        ambi_bin_create(&hAmbi);
#if AMBIBIN_MULTIBUS
        ambi_bin_setInputOrderPreset(hAmbi, SH_ORDER_SECOND);
#endif
    }
  
    /* specify here on which UDP port number to receive incoming OSC messages */
    osc_port_ID = DEFAULT_OSC_PORT;
    osc.connect(osc_port_ID);
    /* tell the component to listen for OSC messages */
    osc.addListener(this);
    
    startTimer(TIMER_PROCESSING_RELATED, 40); 
}

PluginProcessor::~PluginProcessor()
{
#if AMBIBIN_MULTIBUS
    cancelPendingUpdate();
    if (vst3ComponentHandler != nullptr)
        vst3ComponentHandler->release();
#endif
    osc.disconnect();
    osc.removeListener(this);
    
    for (void*& hAmbi : hAmbis)
        ambi_bin_destroy(&hAmbi);
}

void PluginProcessor::oscMessageReceived(const OSCMessage& message)
{
    /* if rotation angles are sent as an array \ypr[3] */
    if (message.size() == 3 && message.getAddressPattern().toString().compare("/ypr")==0) {
        if (message[0].isFloat32())
            for (void* hAmbi : hAmbis)
                ambi_bin_setYaw(hAmbi, message[0].getFloat32());
        if (message[1].isFloat32())
            for (void* hAmbi : hAmbis)
                ambi_bin_setPitch(hAmbi, message[1].getFloat32());
        if (message[2].isFloat32())
            for (void* hAmbi : hAmbis)
                ambi_bin_setRoll(hAmbi, message[2].getFloat32());
        return;
    }
    
    /* if rotation angles are sent individually: */
    if (message.getAddressPattern().toString().compare("/yaw") == 0) {
        for (void* hAmbi : hAmbis)
            ambi_bin_setYaw(hAmbi, message[0].getFloat32());
    }
    else if (message.getAddressPattern().toString().compare("/pitch") == 0) {
        for (void* hAmbi : hAmbis)
            ambi_bin_setPitch(hAmbi, message[0].getFloat32());
    }
    else if (message.getAddressPattern().toString().compare("/roll") == 0) {
        for (void* hAmbi : hAmbis)
            ambi_bin_setRoll(hAmbi, message[0].getFloat32());
    }
}

void PluginProcessor::setParameter(int index, float newValue)
{
    switch (index) {
    case k_inputOrder:
#if AMBIBIN_MULTIBUS
        requestBusOrder((int)(newValue * float(MAX_SH_ORDER-1) + 1.5f));
#else
        for (void* hAmbi : hAmbis)
            ambi_bin_setInputOrderPreset(hAmbi, (SH_ORDERS)(int)(newValue * (float)(MAX_SH_ORDER - 1) + 1.5f));
#endif
        break;
    case k_channelOrder:
        for (void* hAmbi : hAmbis)
            ambi_bin_setChOrder(hAmbi, (int)(newValue * (float)(NUM_CH_ORDERINGS - 1) + 1.5f));
        break;
    case k_normType:
        for (void* hAmbi : hAmbis)
            ambi_bin_setNormType(hAmbi, (int)(newValue * (float)(NUM_NORM_TYPES - 1) + 1.5f));
        break;
    case k_decMethod:
        for (void* hAmbi : hAmbis)
            ambi_bin_setDecodingMethod(hAmbi, (AMBI_BIN_DECODING_METHODS)(int)(newValue * (float)(AMBI_BIN_NUM_DECODING_METHODS - 1) + 1.5f));
        break;
    case k_enableDiffuseMatching:
        for (void* hAmbi : hAmbis)
            ambi_bin_setEnableDiffuseMatching(hAmbi, (int)(newValue + 0.5f));
        break;
    case k_enableMaxRE:
        for (void* hAmbi : hAmbis)
            ambi_bin_setEnableMaxRE(hAmbi, (int)(newValue + 0.5f));
        break;
    case k_enableRotation:
        for (void* hAmbi : hAmbis)
            ambi_bin_setEnableRotation(hAmbi, (int)(newValue + 0.5f));
        break;
    case k_useRollPitchYaw:
        for (void* hAmbi : hAmbis)
            ambi_bin_setRPYflag(hAmbi, (int)(newValue + 0.5f));
        break;
    case k_yaw:
        for (void* hAmbi : hAmbis)
            ambi_bin_setYaw(hAmbi, (newValue - 0.5f) * 360.0f);
        break;
    case k_pitch:
        for (void* hAmbi : hAmbis)
            ambi_bin_setPitch(hAmbi, (newValue - 0.5f) * 180.0f);
        break;
    case k_roll:
        for (void* hAmbi : hAmbis)
            ambi_bin_setRoll(hAmbi, (newValue - 0.5f) * 180.0f);
        break;
    case k_flipYaw:
        for (void* hAmbi : hAmbis)
            ambi_bin_setFlipYaw(hAmbi, (int)(newValue + 0.5f));
        break;
    case k_flipPitch:
        for (void* hAmbi : hAmbis)
            ambi_bin_setFlipPitch(hAmbi, (int)(newValue + 0.5f));
        break;
    case k_flipRoll:
        for (void* hAmbi : hAmbis)
            ambi_bin_setFlipRoll(hAmbi, (int)(newValue + 0.5f));
        break;
    case k_feedCount:
        setAmbisonicFeedCount(newValue < 0.5f ? 2 : 4);
        break;
    default:
        break;
    }
}

void PluginProcessor::setCurrentProgram (int /*index*/)
{
}

float PluginProcessor::getParameter (int index)
{
    switch (index) {
        case k_inputOrder:            return (float)(ambi_bin_getInputOrderPreset(hAmbis[0])-1)/(float)(MAX_SH_ORDER-1);
        case k_channelOrder:          return (float)(ambi_bin_getChOrder(hAmbis[0])-1)/(float)(NUM_NORM_TYPES-1);
        case k_normType:              return (float)(ambi_bin_getNormType(hAmbis[0])-1)/(float)(NUM_NORM_TYPES-1);
        case k_decMethod:             return (float)(ambi_bin_getDecodingMethod(hAmbis[0])-1)/(float)(AMBI_BIN_NUM_DECODING_METHODS-1);
        case k_enableDiffuseMatching: return (float)ambi_bin_getEnableDiffuseMatching(hAmbis[0]);
        case k_enableMaxRE:           return (float)ambi_bin_getEnableMaxRE(hAmbis[0]);
        case k_enableRotation:        return (float)ambi_bin_getEnableRotation(hAmbis[0]);
        case k_useRollPitchYaw:       return (float)ambi_bin_getRPYflag(hAmbis[0]);
        case k_yaw:                   return (ambi_bin_getYaw(hAmbis[0])/360.0f) + 0.5f;
        case k_pitch:                 return (ambi_bin_getPitch(hAmbis[0])/180.0f) + 0.5f;
        case k_roll:                  return (ambi_bin_getRoll(hAmbis[0])/180.0f) + 0.5f;
        case k_flipYaw:               return (float)ambi_bin_getFlipYaw(hAmbis[0]);
        case k_flipPitch:             return (float)ambi_bin_getFlipPitch(hAmbis[0]);
        case k_flipRoll:              return (float)ambi_bin_getFlipRoll(hAmbis[0]);
        case k_feedCount:             return getAmbisonicFeedCount() == 2 ? 0.0f : 1.0f;
        
        default: return 0.0f;
    }
}

int PluginProcessor::getNumParameters()
{
	return k_NumOfParameters;
}

const String PluginProcessor::getName() const
{
    return JucePlugin_Name;
}

const String PluginProcessor::getParameterName (int index)
{
    switch (index) {
        case k_inputOrder:            return "order";
        case k_channelOrder:          return "channel_order";
        case k_normType:              return "norm_type";
        case k_decMethod:             return "decode_method";
        case k_enableDiffuseMatching: return "apply_diff_match";
        case k_enableMaxRE:           return "apply_maxre_weights";
        case k_enableRotation:        return "enable_rotation";
        case k_useRollPitchYaw:       return "use_rpy";
        case k_yaw:                   return "yaw";
        case k_pitch:                 return "pitch";
        case k_roll:                  return "roll";
        case k_flipYaw:               return "flip_yaw";
        case k_flipPitch:             return "flip_pitch";
        case k_flipRoll:              return "flip_roll";
        case k_feedCount:             return "microphone_feeds";
       
        default: return "NULL";
    }
}

const String PluginProcessor::getParameterText(int index)
{
    switch (index) {
        case k_inputOrder: return String(ambi_bin_getInputOrderPreset(hAmbis[0]));
        case k_channelOrder:
            switch(ambi_bin_getChOrder(hAmbis[0])){
                case CH_ACN:  return "ACN";
                case CH_FUMA: return "FuMa";
                default: return "NULL";
            }
        case k_normType:
            switch(ambi_bin_getNormType(hAmbis[0])){
                case NORM_N3D:  return "N3D";
                case NORM_SN3D: return "SN3D";
                case NORM_FUMA: return "FuMa";
                default: return "NULL";
            }
        case k_decMethod:
            switch(ambi_bin_getDecodingMethod(hAmbis[0])){
                case DECODING_METHOD_LS:       return "LS";
                case DECODING_METHOD_LSDIFFEQ: return "LS-DiffEQ";
                case DECODING_METHOD_SPR:      return "SPR";
                case DECODING_METHOD_TA:       return "TA";
                case DECODING_METHOD_MAGLS:    return "Mag-LS";
                default: return "NULL";
            }
        case k_enableDiffuseMatching: return !ambi_bin_getEnableDiffuseMatching(hAmbis[0]) ? "Off" : "On";
        case k_enableMaxRE:           return !ambi_bin_getEnableMaxRE(hAmbis[0]) ? "Off" : "On";
        case k_enableRotation:        return !ambi_bin_getEnableRotation(hAmbis[0]) ? "Off" : "On";
        case k_useRollPitchYaw:       return !ambi_bin_getRPYflag(hAmbis[0]) ? "YPR" : "RPY";
        case k_yaw:                   return String(ambi_bin_getYaw(hAmbis[0]));
        case k_pitch:                 return String(ambi_bin_getPitch(hAmbis[0]));
        case k_roll:                  return String(ambi_bin_getRoll(hAmbis[0]));
        case k_flipYaw:               return !ambi_bin_getFlipYaw(hAmbis[0]) ? "No-Flip" : "Flip";
        case k_flipPitch:             return !ambi_bin_getFlipPitch(hAmbis[0]) ? "No-Flip" : "Flip";
        case k_flipRoll:              return !ambi_bin_getFlipRoll(hAmbis[0]) ? "No-Flip" : "Flip";
        case k_feedCount:             return getAmbisonicFeedCount() == 2 ? "2 (Left/Right)" : "4 (Left/Right/Front/Back)";
            
        default: return "NULL";
    }
}

const String PluginProcessor::getInputChannelName (int channelIndex) const
{
    return String (channelIndex + 1);
}

const String PluginProcessor::getOutputChannelName (int channelIndex) const
{
    return String (channelIndex + 1);
}

double PluginProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PluginProcessor::getNumPrograms()
{
    return 0;
}

int PluginProcessor::getCurrentProgram()
{
    return 0;
}

const String PluginProcessor::getProgramName (int /*index*/)
{
    return String();
}


bool PluginProcessor::isInputChannelStereoPair (int /*index*/) const
{
    return true;
}

bool PluginProcessor::isOutputChannelStereoPair (int /*index*/) const
{
    return true;
}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if AMBIBIN_MULTIBUS
    if (layouts.inputBuses.size() != 4 || layouts.outputBuses.size() != 1 ||
        layouts.outputBuses[0] != AudioChannelSet::stereo())
        return false;
    const int order = layouts.inputBuses[0].getAmbisonicOrder();
    if (order < 1 || order > kMaxBusOrder)
        return false;
    for (int feed = 0; feed < 4; ++feed) {
        const auto& input = layouts.inputBuses[feed];
        if (feed >= 2 && input.isDisabled())
            continue;
        if (input != AudioChannelSet::ambisonic(order))
            return false;
    }
#else
    ignoreUnused(layouts);
#endif
    return true;
}

#if AMBIBIN_MULTIBUS
void PluginProcessor::setIComponentHandler(Steinberg::FUnknown* unknown)
{
    Steinberg::Vst::IComponentHandler* handler = nullptr;
    if (unknown != nullptr)
        unknown->queryInterface(Steinberg::Vst::IComponentHandler_iid,
                                reinterpret_cast<void**>(&handler));
    if (vst3ComponentHandler != nullptr)
        vst3ComponentHandler->release();
    vst3ComponentHandler = handler; // queryInterface supplied an owning reference.
}

void PluginProcessor::requestBusOrder(int order)
{
    const int requested = jlimit(1, kMaxBusOrder, order);
    if (requested != requestedBusOrder.load())
        restoredOrderNeedsNotification.store(false);
    requestedBusOrder.store(requested);
    triggerAsyncUpdate();
    // VST3 changes are always deferred: even offline audio may run on the
    // message thread, so a parameter queue must not resize buses mid-process.
    // Unwrapped processor tests can complete synchronously on the UI thread.
    if (wrapperType != wrapperType_VST3)
        if (auto* manager = MessageManager::getInstanceWithoutCreating())
            if (manager->isThisTheMessageThread())
                handleUpdateNowIfNeeded();
}

bool PluginProcessor::applyBusLayouts(const BusesLayout& layouts)
{
    if (!isBusesLayoutSupported(layouts) || !AudioProcessor::applyBusLayouts(layouts))
        return false;
    const int order = layouts.inputBuses[0].getAmbisonicOrder();
    synchroniseDecoderOrder(order);
    requestedBusOrder.store(order);
    feedGainsInitialised = false;
    sendParamChangeMessageToListeners(k_inputOrder, float(order-1)/float(MAX_SH_ORDER-1));
    return true;
}

void PluginProcessor::synchroniseDecoderOrder(int order)
{
    for (void* handle : hAmbis)
        if (handle != nullptr) {
            // SAF compares order requests against its last initialized order.
            // If a pending change is being cancelled back to that order, settle
            // it first so the official setter does not leave the pending value.
            while (ambi_bin_getCodecStatus(handle) == CODEC_STATUS_INITIALISING)
                Thread::sleep(1);
            if (ambi_bin_getInputOrderPreset(handle) != int(order) &&
                ambi_bin_getNSHrequired(handle) == (int(order)+1)*(int(order)+1))
                ambi_bin_initCodec(handle);
            ambi_bin_setInputOrderPreset(handle, static_cast<SH_ORDERS>(order));
        }
}

void PluginProcessor::restoreBusOrder(int savedOrder)
{
    // VST3 reads parameter values immediately after setStateInformation returns.
    // Loading a project must not leave the constructor's order in the controller
    // cache while waiting for a message-thread callback.
    cancelPendingUpdate();
    const ScopedLock lock(getCallbackLock());
    restoredOrderNeedsNotification.store(false);
    const int order = jlimit(1, kMaxBusOrder, savedOrder);
    auto layout = getBusesLayout();
    const auto previousLayout = layout;
    for (int feed = 0; feed < 4; ++feed)
        layout.inputBuses.set(feed, AudioChannelSet::ambisonic(order));
    if (setBusesLayoutWithoutEnabling(layout)) {
        // setBusesLayout may be a no-op when the host already restored the
        // requested width. Synchronize the decoders in that case too.
        synchroniseDecoderOrder(order);
        requestedBusOrder.store(order);
        feedGainsInitialised = false;
        if (getBusesLayout() != previousLayout) {
            layoutBeforeStateRestore = previousLayout;
            restoredOrderNeedsNotification.store(true);
            triggerAsyncUpdate();
        }
    }
    // Notify only after this transaction returns, never re-enter host I/O
    // negotiation while the host is still reading the restored parameters.
}

void PluginProcessor::handleAsyncUpdate()
{
    const int order = requestedBusOrder.load();
    const auto oldLayout = getBusesLayout();
    const bool restored = restoredOrderNeedsNotification.exchange(false);
    const auto rollbackLayout = restored ? layoutBeforeStateRestore : oldLayout;
    auto layout = oldLayout;
    bool changed = false;
    for (int feed = 0; feed < 4; ++feed) {
        const auto desired = AudioChannelSet::ambisonic(order);
        changed |= getBus(true, feed)->getLastEnabledLayout() != desired;
        layout.inputBuses.set(feed, desired);
    }
    if (!changed) {
        const ScopedLock lock(getCallbackLock());
        synchroniseDecoderOrder(order);
        if (!restored)
            return;
    }
    if (changed) {
        const ScopedLock lock(getCallbackLock());
        if (!setBusesLayoutWithoutEnabling(layout)) {
            requestedBusOrder.store(oldLayout.inputBuses[0].getAmbisonicOrder());
            return;
        }
    }
    // JUCE's generic updateHostDisplay does not send kIoChanged. Use its public
    // VST3 extension hook instead of patching the official JUCE submodule.
    if (vst3ComponentHandler != nullptr) {
        auto* handler = static_cast<Steinberg::Vst::IComponentHandler*>(vst3ComponentHandler);
        const auto result = handler->restartComponent(Steinberg::Vst::kIoChanged
                                                       | Steinberg::Vst::kParamValuesChanged);
        if (result != Steinberg::kResultOk) {
            const ScopedLock lock(getCallbackLock());
            auto rollback = rollbackLayout;
            for (int feed = 0; feed < 4; ++feed)
                rollback.inputBuses.set(feed, AudioChannelSet::ambisonic(rollbackLayout.inputBuses[0].getAmbisonicOrder()));
            setBusesLayoutWithoutEnabling(rollback);
        }
    }
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}
#endif

bool PluginProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::silenceInProducesSilenceOut() const
{
    return false;
}

void PluginProcessor::changeProgramName (int /*index*/, const String& /*newName*/)
{
}

void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
#if AMBIBIN_MULTIBUS
    // Reconcile after either host call order: layout-then-state or state-then-
    // layout. Codec creation below must use the final negotiated bus order.
    synchroniseDecoderOrder(getChannelLayoutOfBus(true, 0).getAmbisonicOrder());
#endif
    nHostBlockSize = samplesPerBlock;
    nNumInputs =  jmin(getTotalNumInputChannels(), 256);
    nNumOutputs = jmin(getTotalNumOutputChannels(), 256);
    nSampleRate = (int)(sampleRate + 0.5);
    feedGainsInitialised = false;
    inactiveFlushFrames.fill(0);
#if AMBIBIN_MULTIBUS
    multibusInput.setSize(4*ambi_bin_getNSHrequired(hAmbis[0]), samplesPerBlock);
#endif

    for (void* hAmbi : hAmbis) {
        ambi_bin_init(hAmbi, nSampleRate);
        ambi_bin_initCodec(hAmbi);
    }

    AudioProcessor::setLatencySamples(ambi_bin_getProcessingDelay());
}

void PluginProcessor::releaseResources()
{
}

// Decode two (Left/Right) or four (Left/Right/Front/Back) Ambisonic feeds.
void PluginProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer& /*midiMessages*/)
{
#if AMBIBIN_MULTIBUS
    // Preserve physical feed slots even if an auxiliary bus is disabled.
    // Stage every input before writing the output, which may alias the Left bus.
    const int channelsPerFeed = ambi_bin_getNSHrequired(hAmbis[0]);
    // During a host I/O restart, an old-width buffer must not be interpreted
    // using the new order. Wait silently for matching bus buffers instead.
    if (buffer.getNumChannels() != jmax(getTotalNumInputChannels(), getTotalNumOutputChannels())) {
        buffer.clear();
        return;
    }
    multibusInput.setSize(4*channelsPerFeed, buffer.getNumSamples(), false, false, true);
    multibusInput.clear();
    for (int feed = 0; feed < kNumAmbiFeeds; ++feed) {
        auto* bus = getBus(true, feed);
        if (bus == nullptr || !bus->isEnabled()) {
            if (feed < getAmbisonicFeedCount()) {
                buffer.clear();
                return;
            }
            continue;
        }
        auto input = getBusBuffer(buffer, true, feed);
        if (input.getNumChannels() != channelsPerFeed) {
            buffer.clear();
            return;
        }
        for (int channel = 0; channel < channelsPerFeed; ++channel)
            multibusInput.copyFrom(feed*channelsPerFeed + channel, 0, input, channel, 0, buffer.getNumSamples());
    }
    processAmbisonicBlock(multibusInput);
    auto output = getBusBuffer(buffer, false, 0);
    for (int ear = 0; ear < 2; ++ear)
        output.copyFrom(ear, 0, multibusInput, ear, 0, buffer.getNumSamples());
#else
    processAmbisonicBlock(buffer);
#endif
}

void PluginProcessor::processAmbisonicBlock(AudioSampleBuffer& buffer)
{
    int nCurrentBlockSize = nHostBlockSize = buffer.getNumSamples();
    nNumInputs = jmin(getTotalNumInputChannels(), buffer.getNumChannels(), 256);
    nNumOutputs = jmin(getTotalNumOutputChannels(), buffer.getNumChannels(), 256);

    int frameSize = ambi_bin_getFrameSize();
    int nChPerFeed = ambi_bin_getNSHrequired(hAmbis[0]);
    const int numFeeds = getAmbisonicFeedCount(); /* One mode snapshot per block. */
    const int nRequiredInputs = numFeeds * nChPerFeed;
    const int outputMode = diagnosticOutputMode > numFeeds && diagnosticOutputMode <= kNumAmbiFeeds
                           ? 0 : diagnosticOutputMode;

    float* const* bufferData = buffer.getArrayOfWritePointers();

    /* Drain inactive filterbanks even while a direct/raw diagnostic is
       selected, so returning to four feeds cannot replay frozen audio. */
    if (nCurrentBlockSize % frameSize == 0) {
        for (int feed = numFeeds; feed < kNumAmbiFeeds; ++feed) {
            for (int frame = 0; frame < nCurrentBlockSize / frameSize && inactiveFlushFrames[(size_t)feed] > 0; ++frame) {
                ambi_bin_process(hAmbis[(size_t)feed], nullptr, nullptr, 0, 0, frameSize);
                --inactiveFlushFrames[(size_t)feed];
            }
        }
    }

    if (outputMode == 5 || outputMode == 6) {
        const int rawLeftChannel = outputMode == 5 ? 0 : 1;
        const int rawRightChannel = nChPerFeed + rawLeftChannel;

        if (getTotalNumOutputChannels() >= 2 && rawRightChannel < nNumInputs) {
            FloatVectorOperations::copy(buffer.getWritePointer(0),
                                        bufferData[rawLeftChannel],
                                        nCurrentBlockSize);
            FloatVectorOperations::copy(buffer.getWritePointer(1),
                                        bufferData[rawRightChannel],
                                        nCurrentBlockSize);

            for (int ch = 2; ch < jmin(getTotalNumOutputChannels(), buffer.getNumChannels(), 256); ++ch)
                buffer.clear(ch, 0, nCurrentBlockSize);
            return;
        }
    }

    if (outputMode > 0 && outputMode <= numFeeds) {
        const int feed = outputMode - 1;
        const int chOffset = feed * nChPerFeed;
        const int nRequiredForFeed = chOffset + nChPerFeed;

        if (nNumInputs < nRequiredForFeed || getTotalNumOutputChannels() < 2) {
            buffer.clear();
            return;
        }

        if (nCurrentBlockSize % frameSize != 0) {
            buffer.clear();
            return;
        }

        AudioSampleBuffer tempOut(2, nCurrentBlockSize);
        std::vector<float*> frameData((size_t)nChPerFeed);
        std::array<float*, 2> frameOutputs{};
        const std::array<float, kNumAmbiFeeds> linearGains{
            std::pow(10.0f, gainCompLeft / 20.0f),
            std::pow(10.0f, gainCompRight / 20.0f),
            std::pow(10.0f, gainCompFront / 20.0f),
            std::pow(10.0f, gainCompBack / 20.0f)
        };

        const int nFrames = nCurrentBlockSize / frameSize;
        for (int frame = 0; frame < nFrames; ++frame) {
            for (int ch = 0; ch < nChPerFeed; ++ch) {
                float* channelData = &bufferData[chOffset + ch][frame * frameSize];

                if (linearGains[(size_t)feed] != 1.0f) {
                    for (int i = 0; i < frameSize; ++i)
                        channelData[i] *= linearGains[(size_t)feed];
                }

                frameData[(size_t)ch] = channelData;
            }

            frameOutputs[0] = tempOut.getWritePointer(0) + frame * frameSize;
            frameOutputs[1] = tempOut.getWritePointer(1) + frame * frameSize;

            ambi_bin_process(hAmbis[(size_t)feed],
                             frameData.data(),
                             frameOutputs.data(),
                             nChPerFeed,
                             2,
                             frameSize);
            inactiveFlushFrames[(size_t)feed] = (2 * ambi_bin_getProcessingDelay() + frameSize - 1) / frameSize + 2;
        }

        FloatVectorOperations::copy(buffer.getWritePointer(0), tempOut.getReadPointer(0), nCurrentBlockSize);
        FloatVectorOperations::copy(buffer.getWritePointer(1), tempOut.getReadPointer(1), nCurrentBlockSize);

        for (int ch = 2; ch < jmin(getTotalNumOutputChannels(), buffer.getNumChannels(), 256); ++ch)
            buffer.clear(ch, 0, nCurrentBlockSize);
        return;
    }

    if (nNumInputs < nRequiredInputs || getTotalNumOutputChannels() < 2) {
        buffer.clear();
        return;
    }

    std::array<AudioSampleBuffer, kNumAmbiFeeds> tempOuts;
    std::array<std::vector<float*>, kNumAmbiFeeds> frameData;
    std::array<std::array<float*, 2>, kNumAmbiFeeds> frameOutputs{};
    const std::array<float, kNumAmbiFeeds> linearGains{
        std::pow(10.0f, gainCompLeft / 20.0f),
        std::pow(10.0f, gainCompRight / 20.0f),
        std::pow(10.0f, gainCompFront / 20.0f),
        std::pow(10.0f, gainCompBack / 20.0f)
    };

    for (int feed = 0; feed < numFeeds; ++feed) {
        tempOuts[(size_t)feed].setSize(2, nCurrentBlockSize, false, false, true);
        frameData[(size_t)feed].resize((size_t)nChPerFeed);
    }

    if (nCurrentBlockSize % frameSize == 0) {
        int nFrames = nCurrentBlockSize / frameSize;

        for (int frame = 0; frame < nFrames; ++frame) {
            for (int feed = 0; feed < numFeeds; ++feed) {
                const int chOffset = feed * nChPerFeed;
                for (int ch = 0; ch < nChPerFeed; ++ch) {
                    float* channelData = &bufferData[chOffset + ch][frame * frameSize];

                    if (linearGains[(size_t)feed] != 1.0f) {
                        for (int i = 0; i < frameSize; ++i)
                            channelData[i] *= linearGains[(size_t)feed];
                    }

                    frameData[(size_t)feed][(size_t)ch] = channelData;
                }

                for (int ear = 0; ear < 2; ++ear)
                    frameOutputs[(size_t)feed][(size_t)ear] = tempOuts[(size_t)feed].getWritePointer(ear) + frame * frameSize;

                ambi_bin_process(hAmbis[(size_t)feed],
                                 frameData[(size_t)feed].data(),
                                 frameOutputs[(size_t)feed].data(),
                                 nChPerFeed,
                                 2,
                                 frameSize);
                /* The filterbank's response extends on both sides of its
                   reported latency; drain its full support, not just delay. */
                inactiveFlushFrames[(size_t)feed] = (2 * ambi_bin_getProcessingDelay() + frameSize - 1) / frameSize + 2;
            }

        }

        const EarFeedGains earGains = getEarFeedGains(hAmbis[0], blendFocus, numFeeds);

        if (!feedGainsInitialised) {
            previousLeftFeedGains = earGains.left;
            previousRightFeedGains = earGains.right;
            feedGainsInitialised = true;
        }

        float* outL = buffer.getWritePointer(0);
        float* outR = buffer.getWritePointer(1);
        std::array<const float*, kNumAmbiFeeds> decodedLeft{};
        std::array<const float*, kNumAmbiFeeds> decodedRight{};

        for (int feed = 0; feed < numFeeds; ++feed) {
            decodedLeft[(size_t)feed] = tempOuts[(size_t)feed].getReadPointer(0);
            decodedRight[(size_t)feed] = tempOuts[(size_t)feed].getReadPointer(1);
        }

        for (int i = 0; i < nCurrentBlockSize; ++i) {
            const float interpolation = (float)(i + 1) / (float)nCurrentBlockSize;
            float leftOut = 0.0f;
            float rightOut = 0.0f;

            for (int feed = 0; feed < numFeeds; ++feed) {
                const float leftGain = previousLeftFeedGains[(size_t)feed]
                                     + interpolation * (earGains.left[(size_t)feed]
                                                      - previousLeftFeedGains[(size_t)feed]);
                const float rightGain = previousRightFeedGains[(size_t)feed]
                                      + interpolation * (earGains.right[(size_t)feed]
                                                       - previousRightFeedGains[(size_t)feed]);
                leftOut += leftGain * decodedLeft[(size_t)feed][i];
                rightOut += rightGain * decodedRight[(size_t)feed][i];
            }

            outL[i] = leftOut;
            outR[i] = rightOut;
        }

        previousLeftFeedGains = earGains.left;
        previousRightFeedGains = earGains.right;

        for (int ch = 2; ch < jmin(getTotalNumOutputChannels(), buffer.getNumChannels(), 256); ++ch)
            buffer.clear(ch, 0, nCurrentBlockSize);
    }
    else {
        buffer.clear();
    }
}

// Debug version that works with Bidule
//void PluginProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer&)
//{
//    ScopedNoDenormals noDenormals;
//
//    int nCurrentBlockSize = nHostBlockSize = buffer.getNumSamples();
//    nNumInputs = jmin(getTotalNumInputChannels(), buffer.getNumChannels(), 256);
//    nNumOutputs = jmin(getTotalNumOutputChannels(), buffer.getNumChannels(), 256);
//    float* const* bufferData = buffer.getArrayOfWritePointers();
//    float* pFrameData1[256] = { nullptr };
//    float* pFrameData2[256] = { nullptr };
//    int frameSize = ambi_bin_getFrameSize();
//
//    // Get ambisonic order and derive channels per feed
//    int order = ambi_bin_getInputOrderPreset(hAmbi);
//    int nChPerFeed = (order + 1) * (order + 1);
//
//    // Initialize codecs if needed
//    if (ambi_bin_getCodecStatus(hAmbi) == CODEC_STATUS_NOT_INITIALISED)
//        ambi_bin_initCodec(hAmbi);
//    if (ambi_bin_getCodecStatus(hAmbi2) == CODEC_STATUS_NOT_INITIALISED)
//        ambi_bin_initCodec(hAmbi2);
//
//    if ((nCurrentBlockSize % frameSize) == 0 && buffer.getNumChannels() >= 2 * nChPerFeed) {
//        int nFrames = nCurrentBlockSize / frameSize;
//
//        for (int frame = 0; frame < nFrames; ++frame) {
//            for (int ch = 0; ch < nChPerFeed; ++ch) {
//                pFrameData1[ch] = &buffer.getWritePointer(ch)[frame * frameSize];
//                pFrameData2[ch] = &buffer.getWritePointer(ch + nChPerFeed)[frame * frameSize];
//            }
//
//            ambi_bin_process(hAmbi, pFrameData1, pFrameData1, nChPerFeed, nNumOutputs, frameSize);
//            ambi_bin_process(hAmbi2, pFrameData2, pFrameData2, nChPerFeed, nNumOutputs, frameSize);
//        }
//    }
//    else {
//        buffer.clear();
//    }
//}

// Works with Bidule
//void PluginProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer&)
//{
//    ScopedNoDenormals noDenormals;
//
//    int nCurrentBlockSize = nHostBlockSize = buffer.getNumSamples();
//
//    float* const* bufferData = buffer.getArrayOfWritePointers();
//    float* pFrameData1[64] = { nullptr };
//    float* pFrameData2[64] = { nullptr };
//    float* out1[2] = { nullptr, nullptr };
//    float* out2[2] = { nullptr, nullptr };
//    int frameSize = ambi_bin_getFrameSize();
//
//    int order = ambi_bin_getInputOrderPreset(hAmbi);
//    int nChPerFeed = (order + 1) * (order + 1);
//    //debugfloat = ambi_bin_getEnableRotation(hAmbi2);
//    if ((nCurrentBlockSize % frameSize) != 0) {
//        buffer.clear();
//        return;
//    }
//
//    AudioSampleBuffer tempOut1(2, nCurrentBlockSize);
//    AudioSampleBuffer tempOut2(2, nCurrentBlockSize);
//
//    float gainL = std::pow(10.0f, gainCompLeft / 20.0f);
//    float gainR = std::pow(10.0f, gainCompRight / 20.0f);
//
//    int nFrames = nCurrentBlockSize / frameSize;
//
//    for (int frame = 0; frame < nFrames; ++frame) {
//        for (int ch = 0; ch < nChPerFeed; ++ch) {
//            float* ptr1 = &buffer.getWritePointer(ch)[frame * frameSize];
//            float* ptr2 = &buffer.getWritePointer(ch + nChPerFeed)[frame * frameSize];
//
//            for (int i = 0; i < frameSize; ++i) {
//                ptr1[i] *= gainL;
//                ptr2[i] *= gainR;
//            }
//
//            pFrameData1[ch] = ptr1;
//            pFrameData2[ch] = ptr2;
//        }
//
//        for (int ch = 0; ch < 2; ++ch) {
//            out1[ch] = tempOut1.getWritePointer(ch) + frame * frameSize;
//            out2[ch] = tempOut2.getWritePointer(ch) + frame * frameSize;
//        }
//
//        ambi_bin_process(hAmbi, pFrameData1, out1, nChPerFeed, 2, frameSize);
//        ambi_bin_process(hAmbi2, pFrameData2, out2, nChPerFeed, 2, frameSize);
//    }
//
//    float yaw = ambi_bin_getYaw(hAmbi);
//    float fadeFactor = 0.0f;
//    const float fadeStartDeg = 75.0f;
//    const float fadeEndDeg = 105.0f;
//    float absYaw = std::abs(yaw);
//
//    if (absYaw > fadeStartDeg && ambi_bin_getEnableRotation(hAmbi)) {
//        fadeFactor = (absYaw >= fadeEndDeg) ? 1.0f
//            : (absYaw - fadeStartDeg) / (fadeEndDeg - fadeStartDeg);
//        fadeFactor = 0.5f * (1.0f - std::cos(fadeFactor * float_Pi));
//    }
//
//    const float* left1 = tempOut1.getReadPointer(0);
//    const float* right1 = tempOut1.getReadPointer(1);
//    const float* left2 = tempOut2.getReadPointer(0);
//    const float* right2 = tempOut2.getReadPointer(1);
//
//    float* outL = buffer.getWritePointer(0);
//    float* outR = buffer.getWritePointer(1);
//
//    for (int i = 0; i < nCurrentBlockSize; ++i) {
//        outL[i] = (1.0f - fadeFactor) * left1[i] + fadeFactor * left2[i];
//        outR[i] = (1.0f - fadeFactor) * right2[i] + fadeFactor * right1[i];
//    }
//
//    // Actively zero the remaining output channels Bidule expects
//    //for (int ch = 2; ch < nNumOutputs; ++ch) {
//    //    buffer.clear(ch, 0, nCurrentBlockSize);
//    //}
//}


// SINE gen DEBUG for Bidule
//void PluginProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer&)
//{
//    ScopedNoDenormals noDenormals;
//    int nSamples = buffer.getNumSamples();
//    int sampleRate = getSampleRate(); // or hardcode 48000.0 for test
//
//    float freq = 440.0f;
//    float phase = 0.0f;
//    float phaseDelta = 2.0f * float_Pi * freq / sampleRate;
//
//    for (int i = 0; i < nSamples; ++i) {
//        float sample = std::sin(phase);
//        phase += phaseDelta;
//        if (phase >= 2.0f * float_Pi)
//            phase -= 2.0f * float_Pi;
//
//        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
//            buffer.getWritePointer(ch)[i] = sample;
//    }
//}

// Bilateral decode without rotation swap
//void PluginProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer& /*midiMessages*/)
//{
//    int nCurrentBlockSize = nHostBlockSize = buffer.getNumSamples();
//    int frameSize = ambi_bin_getFrameSize();
//
//    // Get current order and number of channels per feed
//    int order = ambi_bin_getInputOrderPreset(hAmbi);
//    int nChPerFeed = (order + 1) * (order + 1);
//
//    // Check for enough input channels
//    if (buffer.getNumChannels() < 2 * nChPerFeed || buffer.getNumChannels() < 2) {
//        buffer.clear();
//        return;
//    }
//
//    float* const* bufferData = buffer.getArrayOfWritePointers();
//
//    // Temporary output buffers for each decode
//    AudioSampleBuffer tempOut1(2, nCurrentBlockSize);
//    AudioSampleBuffer tempOut2(2, nCurrentBlockSize);
//
//    float* pFrameData1[64] = { nullptr };
//    float* pFrameData2[64] = { nullptr };
//    float* out1[2] = { nullptr, nullptr };
//    float* out2[2] = { nullptr, nullptr };
//
//    if (nCurrentBlockSize % frameSize == 0) { // divisible by frame size
//        int nFrames = nCurrentBlockSize / frameSize;
//        for (int frame = 0; frame < nFrames; ++frame) {
//            // Prepare input pointers for each feed
//            for (int ch = 0; ch < nChPerFeed; ++ch) {
//                pFrameData1[ch] = &bufferData[ch][frame * frameSize];
//                pFrameData2[ch] = &bufferData[ch + nChPerFeed][frame * frameSize];
//            }
//            // Prepare output pointers for each temp buffer
//            for (int ch = 0; ch < 2; ++ch) {
//                out1[ch] = tempOut1.getWritePointer(ch) + frame * frameSize;
//                out2[ch] = tempOut2.getWritePointer(ch) + frame * frameSize;
//            }
//            // Decode first feed
//            ambi_bin_process(hAmbi, pFrameData1, out1, nChPerFeed, 2, frameSize);
//            // Decode second feed
//            ambi_bin_process(hAmbi2, pFrameData2, out2, nChPerFeed, 2, frameSize);
//        }
//
//        // Output: left = left of first, right = right of second
//        float* outL = buffer.getWritePointer(0);
//        float* outR = buffer.getWritePointer(1);
//        memcpy(outL, tempOut1.getReadPointer(0), sizeof(float) * nCurrentBlockSize);
//        memcpy(outR, tempOut2.getReadPointer(1), sizeof(float) * nCurrentBlockSize);
//
//        // Clear any extra output channels
//        for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
//            buffer.clear(ch, 0, nCurrentBlockSize);
//    }
//    else {
//        buffer.clear();
//    }
//
//}

// original AmbiBIN processing
//void PluginProcessor::processBlock (AudioSampleBuffer& buffer, MidiBuffer& /*midiMessages*/)
//{
//    int nCurrentBlockSize = nHostBlockSize = buffer.getNumSamples();
//    nNumInputs = jmin(getTotalNumInputChannels(), buffer.getNumChannels(), 256);
//    nNumOutputs = jmin(getTotalNumOutputChannels(), buffer.getNumChannels(), 256);
//    float* const* bufferData = buffer.getArrayOfWritePointers();
//    float* pFrameData[256];
//    int frameSize = ambi_bin_getFrameSize();
//
//    if((nCurrentBlockSize % frameSize == 0)){ /* divisible by frame size */
//        for (int frame = 0; frame < nCurrentBlockSize/frameSize; frame++) {
//            for (int ch = 0; ch < jmin(buffer.getNumChannels(), 256); ch++)
//                pFrameData[ch] = &bufferData[ch][frame*frameSize];
//
//            /* perform processing */
//            ambi_bin_process(hAmbi, pFrameData, pFrameData, nNumInputs, nNumOutputs, frameSize);
//        }
//    }
//    else
//        buffer.clear();
//}

bool PluginProcessor::hasEditor() const
{
    return true; 
}

AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor (this);
}

void PluginProcessor::getStateInformation (MemoryBlock& destData)
{
    XmlElement xml("AMBIBINPLUGINSETTINGS");
    
#if AMBIBIN_MULTIBUS
    xml.setAttribute("order", getChannelLayoutOfBus(true, 0).getAmbisonicOrder());
#else
    xml.setAttribute("order", ambi_bin_getInputOrderPreset(hAmbis[0]));
#endif
  
    xml.setAttribute("UseDefaultHRIRset", ambi_bin_getUseDefaultHRIRsflag(hAmbis[0]));
    xml.setAttribute("Norm", ambi_bin_getNormType(hAmbis[0]));
    xml.setAttribute("ChOrder", ambi_bin_getChOrder(hAmbis[0]));
    xml.setAttribute("maxrE", ambi_bin_getEnableMaxRE(hAmbis[0]));
    xml.setAttribute("diffMatch", ambi_bin_getEnableDiffuseMatching(hAmbis[0]));
    xml.setAttribute("truncationEQ", ambi_bin_getEnableTruncationEQ(hAmbis[0]));
    xml.setAttribute("method", ambi_bin_getDecodingMethod(hAmbis[0]));
    xml.setAttribute("preproc", ambi_bin_getHRIRsPreProc(hAmbis[0]));
    
    xml.setAttribute("ENABLEROT", ambi_bin_getEnableRotation(hAmbis[0]));
    xml.setAttribute("YAW", ambi_bin_getYaw(hAmbis[0]));
    xml.setAttribute("PITCH", ambi_bin_getPitch(hAmbis[0]));
    xml.setAttribute("ROLL", ambi_bin_getRoll(hAmbis[0]));
    xml.setAttribute("FLIP_YAW", ambi_bin_getFlipYaw(hAmbis[0]));
    xml.setAttribute("FLIP_PITCH", ambi_bin_getFlipPitch(hAmbis[0]));
    xml.setAttribute("FLIP_ROLL", ambi_bin_getFlipRoll(hAmbis[0]));
    xml.setAttribute("RPY_FLAG", ambi_bin_getRPYflag(hAmbis[0]));

	xml.setAttribute("gainCompLeft", gainCompLeft);
	xml.setAttribute("gainCompRight", gainCompRight);
    xml.setAttribute("gainCompFront", gainCompFront);
    xml.setAttribute("gainCompBack", gainCompBack);
    xml.setAttribute("blendFocus", blendFocus);
    xml.setAttribute("ambisonicFeedCount", getAmbisonicFeedCount());
    
    xml.setAttribute("OSC_PORT", osc_port_ID);
    
    if(!ambi_bin_getUseDefaultHRIRsflag(hAmbis[0]))
        xml.setAttribute("SofaFilePath", String(ambi_bin_getSofaFilePath(hAmbis[0])));

    copyXmlToBinary(xml, destData);
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr) {
        if (xmlState->hasTagName("AMBIBINPLUGINSETTINGS")) {
            /* Old sessions have no feed-count attribute and retain four feeds. */
            setAmbisonicFeedCount(xmlState->getIntAttribute("ambisonicFeedCount", 4));

            if (xmlState->hasAttribute("order")) {
#if AMBIBIN_MULTIBUS
                restoreBusOrder(xmlState->getIntAttribute("order", 2));
#else
                for (void* hAmbi : hAmbis)
                    ambi_bin_setInputOrderPreset(hAmbi, (SH_ORDERS)xmlState->getIntAttribute("order", 2));
#endif
            }
            if (xmlState->hasAttribute("UseDefaultHRIRset")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setUseDefaultHRIRsflag(hAmbi, xmlState->getIntAttribute("UseDefaultHRIRset", 1));
            }
            if (xmlState->hasAttribute("Norm")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setNormType(hAmbi, xmlState->getIntAttribute("Norm", 1));
            }
            if (xmlState->hasAttribute("ChOrder")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setChOrder(hAmbi, xmlState->getIntAttribute("ChOrder", 1));
            }
            if (xmlState->hasAttribute("maxrE")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setEnableMaxRE(hAmbi, xmlState->getIntAttribute("maxrE", 1));
            }
            if (xmlState->hasAttribute("diffMatch")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setEnableDiffuseMatching(hAmbi, xmlState->getIntAttribute("diffMatch", 1));
            }
            if (xmlState->hasAttribute("truncationEQ")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setEnableTruncationEQ(hAmbi, xmlState->getIntAttribute("truncationEQ", 1));
            }
            if (xmlState->hasAttribute("method")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setDecodingMethod(hAmbi, (AMBI_BIN_DECODING_METHODS)xmlState->getIntAttribute("method", 1));
            }
            if (xmlState->hasAttribute("preproc")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setHRIRsPreProc(hAmbi, (AMBI_BIN_PREPROC)xmlState->getIntAttribute("preproc", 1));
            }

            if (xmlState->hasAttribute("ENABLEROT")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setEnableRotation(hAmbi, xmlState->getIntAttribute("ENABLEROT", 0));
            }
            if (xmlState->hasAttribute("YAW")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setYaw(hAmbi, (float)xmlState->getDoubleAttribute("YAW", 0.0f));
            }
            if (xmlState->hasAttribute("PITCH")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setPitch(hAmbi, (float)xmlState->getDoubleAttribute("PITCH", 0.0f));
            }
            if (xmlState->hasAttribute("ROLL")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setRoll(hAmbi, (float)xmlState->getDoubleAttribute("ROLL", 0.0f));
            }
            if (xmlState->hasAttribute("FLIP_YAW")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setFlipYaw(hAmbi, xmlState->getIntAttribute("FLIP_YAW", 0));
            }
            if (xmlState->hasAttribute("FLIP_PITCH")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setFlipPitch(hAmbi, xmlState->getIntAttribute("FLIP_PITCH", 0));
            }
            if (xmlState->hasAttribute("FLIP_ROLL")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setFlipRoll(hAmbi, xmlState->getIntAttribute("FLIP_ROLL", 0));
            }
            if (xmlState->hasAttribute("RPY_FLAG")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setRPYflag(hAmbi, xmlState->getIntAttribute("RPY_FLAG", 0));
            }

            if (xmlState->hasAttribute("OSC_PORT")) {
                osc_port_ID = xmlState->getIntAttribute("OSC_PORT", DEFAULT_OSC_PORT);
                osc.connect(osc_port_ID);
            }

            if (xmlState->hasAttribute("SofaFilePath")) {
                String directory = xmlState->getStringAttribute("SofaFilePath", "no_file");
                const char* new_cstring = (const char*)directory.toUTF8();
                for (void* hAmbi : hAmbis)
                    ambi_bin_setSofaFilePath(hAmbi, new_cstring);
            }

			if (xmlState->hasAttribute("gainCompLeft")) {
				gainCompLeft = (float)xmlState->getDoubleAttribute("gainCompLeft", 0.0);
			}

            if (xmlState->hasAttribute("gainCompRight")) {
                gainCompRight = (float)xmlState->getDoubleAttribute("gainCompRight", 0.0);
            }

            if (xmlState->hasAttribute("gainCompFront")) {
                gainCompFront = (float)xmlState->getDoubleAttribute("gainCompFront", 0.0);
            }

            if (xmlState->hasAttribute("gainCompBack")) {
                gainCompBack = (float)xmlState->getDoubleAttribute("gainCompBack", 0.0);
            }

            if (xmlState->hasAttribute("blendFocus")) {
                blendFocus = (float)xmlState->getDoubleAttribute("blendFocus", 3.0);
            }

            if (AudioProcessorEditor* ed = getActiveEditor()) {
                if (PluginEditor* pluginEditor = dynamic_cast<PluginEditor*>(ed)) {
                    pluginEditor->s_gainLeft->setValue(gainCompLeft, dontSendNotification);
                    pluginEditor->s_gainRight->setValue(gainCompRight, dontSendNotification);
                    pluginEditor->s_gainFront->setValue(gainCompFront, dontSendNotification);
                    pluginEditor->s_gainBack->setValue(gainCompBack, dontSendNotification);
                    pluginEditor->s_blendFocus->setValue(blendFocus, dontSendNotification);
                }
            }

            if (!xmlState->hasAttribute("UseDefaultHRIRset")) {
                for (void* hAmbi : hAmbis)
                    ambi_bin_setUseDefaultHRIRsflag(hAmbi, 1);
            }

            for (void* hAmbi : hAmbis)
                ambi_bin_refreshParams(hAmbi);
        }
    }
}


// This creates new instances of the plugin..
AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}

