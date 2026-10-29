#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "pluginterfaces/vst/ivsteditcontroller.h"

namespace {
int checks = 0;
constexpr int frame = 128;
struct TestHostHandler : Steinberg::Vst::IComponentHandler {
    Steinberg::uint32 refs = 1;
    int restartFlags = 0;
    bool reject = false;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID id, void** object) override
    {
        if (std::memcmp(id, Steinberg::Vst::IComponentHandler_iid, 16) == 0) {
            *object = this; addRef(); return Steinberg::kResultOk;
        }
        *object = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++refs; }
    Steinberg::uint32 PLUGIN_API release() override { return --refs; }
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override
    {
        restartFlags = flags;
        return reject ? Steinberg::kResultFalse : Steinberg::kResultOk;
    }
};
void require(bool ok, const char* message)
{
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

AudioSampleBuffer render(PluginProcessor& processor, int feed, int sh, bool direct = true)
{
    processor.diagnosticOutputMode = direct ? feed + 1 : 0;
    const int perFeed = ambi_bin_getNSHrequired(processor.getFXHandle(0));
    AudioSampleBuffer buffer(4*perFeed, frame), result(2, 64*frame);
    MidiBuffer midi;
    for (int block = 0; block < 96; ++block) {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }
    for (int block = 0; block < 64; ++block) {
        buffer.clear();
        for (int sample = 0; sample < frame; ++sample)
            buffer.setSample(feed*perFeed+sh, sample,
                             0.05f * std::sin(float(block*frame+sample)*0.043f)
                             + ((block*frame+sample)%113 == 0 ? 0.1f : 0.0f));
        processor.processBlock(buffer, midi);
        for (int ear = 0; ear < 2; ++ear)
            result.copyFrom(ear, block*frame, buffer, ear, 0, frame);
    }
    return result;
}
}

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    try {
        {
            TestHostHandler host;
            PluginProcessor probe;
            probe.stopTimer(TIMER_PROCESSING_RELATED);
            probe.setIComponentHandler(&host);
            probe.requestBusOrder(3);
            std::cout << "Order request probe: wrapper=" << int(probe.wrapperType)
                      << " inputs=" << probe.getTotalNumInputChannels() << " flags=" << host.restartFlags << "\n";
            require((host.restartFlags & Steinberg::Vst::kIoChanged) != 0 &&
                    probe.getTotalNumInputChannels() == 64, "Order change did not request a VST3 I/O restart");
            host.reject = true;
            probe.requestBusOrder(4);
            require(probe.getTotalNumInputChannels() == 64 &&
                    ambi_bin_getInputOrderPreset(probe.getFXHandle(0)) == SH_ORDER_THIRD,
                    "Rejected host restart did not restore the previous decoder and bus widths");
            probe.setIComponentHandler(nullptr);
            std::thread automation([&] { probe.setParameter(k_inputOrder, 0); });
            automation.join();
            require(probe.getTotalNumInputChannels() == 64,
                    "Audio-thread automation changed the bus layout synchronously");
            probe.requestBusOrder(1);
            require(probe.getTotalNumInputChannels() == 16, "Queued order could not be applied on the message thread");
        }
        PluginProcessor processor;
        processor.stopTimer(TIMER_PROCESSING_RELATED);
        require(processor.getBusCount(true) == 4 && processor.getTotalNumInputChannels() == 36,
                "Prototype must expose four nine-channel inputs");
        require(processor.getChannelLayoutOfBus(false, 0) == AudioChannelSet::stereo(),
                "Output must be explicitly stereo");
        auto layout = processor.getBusesLayout();
        layout.inputBuses.set(1, AudioChannelSet::stereo());
        require(!processor.checkBusesLayoutSupported(layout), "Unequal/incorrect microphone widths must be rejected");
        processor.setParameter(k_inputOrder, 0);
        require(ambi_bin_getInputOrderPreset(processor.getFXHandle(0)) == SH_ORDER_FIRST &&
                processor.getTotalNumInputChannels() == 16,
                "First-order request did not resize all microphone buses");
        processor.requestBusOrder(2);
        processor.setRateAndBufferSizeDetails(48000, frame);
        processor.prepareToPlay(48000, frame);
        AudioSampleBuffer buffer(36, frame);
        MidiBuffer midi;
        for (int mode : {5, 6}) {
            processor.diagnosticOutputMode = mode;
            for (int channel = 0; channel < 36; ++channel)
                FloatVectorOperations::fill(buffer.getWritePointer(channel), float(channel+1), frame);
            processor.processBlock(buffer, midi);
            require(buffer.getSample(0, 0) == (mode == 5 ? 1 : 2), "Left raw input mapping changed");
            require(buffer.getSample(1, 0) == (mode == 5 ? 10 : 11), "Right raw input mapping changed");
        }
        for (int order : {1, 2, 3, 4}) {
          processor.requestBusOrder(order);
          processor.prepareToPlay(48000, frame);
          const int perFeed = (order+1)*(order+1);
          require(processor.getTotalNumInputChannels() == 4*perFeed,
                  "Requested order and actual bus widths disagree");
          AudioSampleBuffer raw(4*perFeed, frame);
          for (int mode : {5, 6}) {
            processor.diagnosticOutputMode = mode;
            for (int channel = 0; channel < 4*perFeed; ++channel)
                FloatVectorOperations::fill(raw.getWritePointer(channel), float(channel+1), frame);
            processor.processBlock(raw, midi);
            require(raw.getSample(0, 0) == (mode == 5 ? 1 : 2) &&
                    raw.getSample(1, 0) == perFeed + (mode == 5 ? 1 : 2),
                    "Raw microphone offsets did not follow the selected order");
          }
          for (int sh = 0; sh < perFeed; ++sh) {
            const auto reference = render(processor, 0, sh);
            for (int feed = 1; feed < 4; ++feed) {
                const auto actual = render(processor, feed, sh);
                for (int ear = 0; ear < 2; ++ear)
                    for (int sample = 0; sample < actual.getNumSamples(); ++sample)
                        require(std::isfinite(actual.getSample(ear, sample)) &&
                                std::abs(reference.getSample(ear, sample)-actual.getSample(ear, sample)) < 0.00002f,
                                "Microphone bus/SH channel differs from reference feed decode");
            }
          }
          std::cout << "PASS all " << 4*perFeed << " feed/SH input lanes at order " << order << "\n";
        }
        processor.requestBusOrder(2);
        processor.prepareToPlay(48000, frame);
        processor.setAmbisonicFeedCount(2);
        layout = processor.getBusesLayout();
        layout.inputBuses.set(2, AudioChannelSet::disabled());
        layout.inputBuses.set(3, AudioChannelSet::disabled());
        require(processor.setBusesLayout(layout), "Bilateral mode must permit disabled Front/Back buses");
        AudioSampleBuffer twoInputs(18, frame);
        for (int channel = 0; channel < 18; ++channel)
            FloatVectorOperations::fill(twoInputs.getWritePointer(channel), float(channel+1), frame);
        processor.diagnosticOutputMode = 5;
        processor.processBlock(twoInputs, midi);
        require(twoInputs.getSample(0, 0) == 1 && twoInputs.getSample(1, 0) == 10,
                "Bilateral input slots moved when auxiliary buses were disabled");
        processor.requestBusOrder(3);
        require(processor.getTotalNumInputChannels() == 32 &&
                processor.getBus(true, 2)->getLastEnabledLayout() == AudioChannelSet::ambisonic(3),
                "Disabled auxiliary buses did not retain the new order for reactivation");
        processor.requestBusOrder(2);
        processor.prepareToPlay(48000, frame);
        processor.setAmbisonicFeedCount(4);
        processor.diagnosticOutputMode = 0;
        twoInputs.clear();
        twoInputs.setSample(0, 0, 1);
        processor.processBlock(twoInputs, midi);
        require(twoInputs.getMagnitude(0, 0, frame) == 0 && twoInputs.getMagnitude(1, 0, frame) == 0,
                "Missing required microphone buses must not be substituted or partially played");
        // A disabled Front bus must not relabel the Back bus as Front.
        layout.inputBuses.set(3, AudioChannelSet::ambisonic(2));
        require(processor.setBusesLayout(layout), "Could not test a missing Front bus");
        AudioSampleBuffer missingFront(27, frame);
        missingFront.clear();
        missingFront.setSample(18, 0, 1);
        processor.processBlock(missingFront, midi);
        require(missingFront.getMagnitude(0, 0, frame) == 0 && missingFront.getMagnitude(1, 0, frame) == 0,
                "Back input was incorrectly substituted for disabled Front input");
        layout.inputBuses.set(2, AudioChannelSet::ambisonic(2));
        require(processor.setBusesLayout(layout), "Could not restore four microphone buses");
        processor.setAmbisonicFeedCount(2);
        MemoryBlock state;
        processor.getStateInformation(state);
        processor.setAmbisonicFeedCount(4);
        processor.setStateInformation(state.getData(), int(state.getSize()));
        require(processor.getAmbisonicFeedCount() == 2, "Bilateral state did not restore");
        require(ambi_bin_getInputOrderPreset(processor.getFXHandle(0)) == SH_ORDER_SECOND,
                "State restore changed the saved prototype order");
        processor.requestBusOrder(3);
        processor.getStateInformation(state);
        processor.requestBusOrder(1);
        processor.setStateInformation(state.getData(), int(state.getSize()));
        require(processor.getTotalNumInputChannels() == 64 &&
                processor.getChannelCountOfBus(true, 0) == 16 &&
                ambi_bin_getInputOrderPreset(processor.getFXHandle(0)) == SH_ORDER_THIRD,
                "State restore did not synchronize saved third-order buses and decoder");
        processor.requestBusOrder(2);
        if (argc > 1) {
            processor.setAmbisonicFeedCount(4);
            std::unique_ptr<AudioProcessorEditor> editor(processor.createEditor());
            File directory = File::getCurrentWorkingDirectory().getChildFile(argv[1]);
            directory.createDirectory();
            auto stream = directory.getChildFile("four-bus-editor.png").createOutputStream();
            require(stream != nullptr, "Could not write editor snapshot");
            stream->setPosition(0);
            stream->truncate();
            require(PNGImageFormat().writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()), *stream),
                    "Could not encode editor snapshot");
            auto reference = directory.getChildFile("vst3-routing-reference.bin").createOutputStream();
            require(reference != nullptr, "Could not write wrapper reference renders");
            reference->setPosition(0);
            reference->truncate();
            processor.setParameter(k_enableRotation, 1);
            // Restoring state marks SAF codecs for reinitialization; this test
            // has deliberately stopped the asynchronous codec timer.
            processor.prepareToPlay(48000, frame);
            double referenceEnergy = 0;
            for (int yaw : {0, 90}) {
                processor.setParameter(k_yaw, 0.5f + float(yaw)/360.0f);
                for (int feed = 0; feed < 4; ++feed)
                    for (int sh = 0; sh < 9; ++sh) {
                        const auto audio = render(processor, feed, sh, false);
                        for (int ear = 0; ear < 2; ++ear)
                            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                            {
                                const float value = audio.getSample(ear, sample);
                                reference->writeFloat(value);
                                referenceEnergy += double(value)*value;
                            }
                    }
            }
            require(referenceEnergy > 0.01, "Wrapper reference renders must not be silent");
            std::cout << "Saved normal-mixer reference renders for all 36 lanes at yaw 0 and +90\n";
        }
        std::cout << "PASS bus validation, bilateral mode, missing buses and state; " << checks << " assertions\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
