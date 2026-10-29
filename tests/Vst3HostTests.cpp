#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace juce;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void setParameter(AudioPluginInstance& plugin, const String& name, float value)
{
    for (auto* parameter : plugin.getParameters())
        if (parameter->getName(100) == name) {
            parameter->setValueNotifyingHost(value);
            return;
        }
    throw std::runtime_error(("Missing parameter: " + name).toStdString());
}

String getOrderText(AudioPluginInstance& plugin)
{
    for (auto* parameter : plugin.getParameters())
        if (parameter->getName(100) == "order")
            return parameter->getCurrentValueAsText();
    return {};
}

void checkFreshStateRestore(VST3PluginFormat& format, const PluginDescription& description)
{
    String error;
    auto source = format.createInstanceFromDescription(description, 48000, 128, error);
    check(source != nullptr, "Could not create save-state source instance");
    auto savedLayout = source->getBusesLayout();
    for (int feed = 0; feed < 4; ++feed)
        savedLayout.inputBuses.set(feed, AudioChannelSet::ambisonic(4));
    check(source->setBusesLayout(savedLayout), "Could not configure fourth-order source state");
    source->prepareToPlay(48000, 128);
    check(getOrderText(*source) == "4", "Source decoder was not fourth order");
    MemoryBlock state;
    source->getStateInformation(state);
    source->releaseResources();
    source.reset();
    for (bool layoutFirst : {false, true}) {
        auto fresh = format.createInstanceFromDescription(description, 48000, 128, error);
        check(fresh != nullptr, "Could not recreate VST3 for reload test");
        if (layoutFirst)
            check(fresh->setBusesLayout(savedLayout), "Could not restore host layout before state");
        fresh->setStateInformation(state.getData(), int(state.getSize()));
        std::cout << "Fresh VST3 restore (layout " << (layoutFirst ? "before" : "after")
                  << " state): immediate decoder order=" << getOrderText(*fresh) << "\n";
        check(getOrderText(*fresh) == "4", "VST3 state returned before restoring the saved decoder order");
        if (!layoutFirst)
            check(fresh->setBusesLayout(savedLayout), "Could not restore host layout after state");
        fresh->prepareToPlay(48000, 128);
        MessageManager::getInstance()->runDispatchLoopUntil(150);
        check(fresh->getTotalNumInputChannels() == 100 && getOrderText(*fresh) == "4",
              "Saved fourth-order bus and decoder diverged after restart");
        AudioBuffer<float> audio(100, 128);
        MidiBuffer midi;
        double energy[2]{};
        for (int block = 0; block < 64; ++block) {
            audio.clear();
            for (int sample = 0; sample < 128; ++sample)
                audio.setSample(25, sample, 0.05f*std::sin(float(block*128+sample)*0.043f));
            fresh->processBlock(audio, midi);
            if (block >= 32)
                for (int ear = 0; ear < 2; ++ear)
                    for (int sample = 0; sample < 128; ++sample)
                        energy[ear] += std::pow(double(audio.getSample(ear, sample)), 2);
        }
        check(energy[1] > 0.001 && energy[0] < 0.000001,
              "Restored decoder did not process fourth-order microphone offsets");
        fresh->releaseResources();
        MemoryBlock resaved;
        fresh->getStateInformation(resaved);
        std::cout << "PASS fresh-instance fourth-order save/reload: 100/2, order 4 and decoded right-feed audio\n";
    }
}

void checkOrderNegotiation(AudioPluginInstance& plugin)
{
    // Test host-driven layout changes on the real wrapper. Host response to a
    // plug-in initiated I/O restart is tested separately, not assumed here.
    MidiBuffer midi;
    setParameter(plugin, "microphone_feeds", 1);
    setParameter(plugin, "enable_rotation", 1);
    setParameter(plugin, "yaw", 0.5f);
    for (int order : {1, 2, 3, 4, 2}) {
        plugin.releaseResources();
        auto layout = plugin.getBusesLayout();
        for (int feed = 0; feed < 4; ++feed)
            layout.inputBuses.set(feed, AudioChannelSet::ambisonic(order));
        check(plugin.setBusesLayout(layout), "VST3 rejected a supported microphone order");
        const int perFeed = (order+1)*(order+1);
        check(plugin.getTotalNumInputChannels() == 4*perFeed, "Host negotiated an incorrect total width");
        plugin.prepareToPlay(48000, 128);
        AudioBuffer<float> buffer(4*perFeed, 128);
        for (int block = 0; block < 96; ++block) {
            buffer.clear();
            plugin.processBlock(buffer, midi);
        }
        double energy[2]{};
        for (int block = 0; block < 64; ++block) {
            buffer.clear();
            for (int sample = 0; sample < 128; ++sample)
                buffer.setSample(0, sample, 0.05f*std::sin(float(block*128+sample)*0.043f));
            plugin.processBlock(buffer, midi);
            if (block >= 32)
                for (int ear = 0; ear < 2; ++ear)
                    for (int sample = 0; sample < 128; ++sample)
                        energy[ear] += std::pow(double(buffer.getSample(ear, sample)), 2);
        }
        check(energy[0] > 0.001 && energy[1] < 0.000001,
              "Order change desynchronized microphone layout and decoding");
        std::cout << "PASS actual VST3 order " << order << ": " << 4*perFeed << "/2, decoded W routing\n";
    }
    plugin.releaseResources();
    // Probe the parameter/GUI-initiated route independently of host-driven
    // requests. Some hosts may reassert their old layout after kIoChanged.
    plugin.prepareToPlay(48000, 128);
    setParameter(plugin, "order", 0);
    AudioBuffer<float> transition(plugin.getTotalNumInputChannels(), 128);
    transition.clear();
    plugin.processBlock(transition, midi);
    MessageManager::getInstance()->runDispatchLoopUntil(300);
    std::cout << "Plugin-initiated first-order request: host now reports "
              << plugin.getTotalNumInputChannels() << "/2\n";
    for (auto* parameter : plugin.getParameters())
        if (parameter->getName(100) == "order")
            std::cout << "Decoder order after host restart: " << parameter->getCurrentValueAsText() << "\n";
    check(plugin.getTotalNumInputChannels() == 16 || plugin.getTotalNumInputChannels() == 36,
          "Host left an inconsistent layout after the I/O restart");
    plugin.releaseResources();
    auto restored = plugin.getBusesLayout();
    for (int feed = 0; feed < 4; ++feed)
        restored.inputBuses.set(feed, AudioChannelSet::ambisonic(2));
    check(plugin.setBusesLayout(restored), "Could not restore second order after restart probe");
}

int runMultibus(AudioPluginInstance& plugin, const String& referencePath)
{
    checkOrderNegotiation(plugin);
    check(plugin.getBusCount(true) == 4 && plugin.getBusCount(false) == 1,
          "Expected four microphone buses and one output");
    auto layout = plugin.getBusesLayout();
    for (int feed = 0; feed < 4; ++feed) {
        layout.inputBuses.set(feed, AudioChannelSet::ambisonic(2));
        std::cout << "Input bus " << feed << ": " << plugin.getBus(true, feed)->getName()
                  << ", channels=" << plugin.getChannelCountOfBus(true, feed) << "\n";
    }
    layout.outputBuses.set(0, AudioChannelSet::stereo());
    check(plugin.setBusesLayout(layout), "VST3 rejected four second-order Ambisonic buses / stereo");
    check(plugin.getTotalNumInputChannels() == 36 && plugin.getTotalNumOutputChannels() == 2,
          "Negotiated channel count is incorrect");
    auto invalid = layout;
    invalid.inputBuses.set(1, AudioChannelSet::ambisonic(1));
    check(!plugin.setBusesLayout(invalid), "Mixed microphone orders should be rejected");
    std::cout << "PASS actual VST3 negotiation: four nine-channel Ambisonic buses / stereo\n";
    setParameter(plugin, "microphone_feeds", 1);
    setParameter(plugin, "enable_rotation", 1);
    setParameter(plugin, "yaw", 0.5f);
    setParameter(plugin, "pitch", 0.5f);
    setParameter(plugin, "roll", 0.5f);
    plugin.setRateAndBufferSizeDetails(48000, 128);
    plugin.prepareToPlay(48000, 128);
    auto reference = File(referencePath).createInputStream();
    check(reference != nullptr && reference->getTotalLength() == 2*4*9*2*64*128*4,
          "Reference render missing or wrong size");
    AudioBuffer<float> buffer(36, 128), result(2, 64*128);
    MidiBuffer midi;
    float worst = 0;
    int comparisons = 0;
    for (int yaw : {0, 90}) {
        setParameter(plugin, "yaw", 0.5f + float(yaw)/360.0f);
        for (int feed = 0; feed < 4; ++feed)
            for (int sh = 0; sh < 9; ++sh) {
                for (int block = 0; block < 96; ++block) {
                    buffer.clear();
                    plugin.processBlock(buffer, midi);
                }
                for (int block = 0; block < 64; ++block) {
                    buffer.clear();
                    for (int sample = 0; sample < 128; ++sample)
                        buffer.setSample(feed*9+sh, sample,
                            0.05f*std::sin(float(block*128+sample)*0.043f)
                            + ((block*128+sample)%113 == 0 ? 0.1f : 0.0f));
                    plugin.processBlock(buffer, midi);
                    for (int ear = 0; ear < 2; ++ear)
                        result.copyFrom(ear, block*128, buffer, ear, 0, 128);
                }
                for (int ear = 0; ear < 2; ++ear)
                    for (int sample = 0; sample < result.getNumSamples(); ++sample) {
                        const float actual = result.getSample(ear, sample);
                        const float expected = reference->readFloat();
                        worst = std::max(worst, std::abs(actual-expected));
                        if (!std::isfinite(actual) || std::abs(actual-expected) >= 0.00002f)
                            std::cerr << "Mismatch yaw=" << yaw << " feed=" << feed << " SH=" << sh
                                      << " ear=" << ear << " sample=" << sample << " actual=" << actual
                                      << " reference=" << expected << "\n";
                        check(std::isfinite(actual) && std::abs(actual-expected) < 0.00002f,
                              "Actual VST3 output differs from processor reference");
                        ++comparisons;
                    }
            }
        std::cout << "PASS all 36 VST3 input lanes at yaw " << yaw << "\n";
    }
    plugin.releaseResources();
    setParameter(plugin, "microphone_feeds", 0);
    layout.inputBuses.set(2, AudioChannelSet::disabled());
    layout.inputBuses.set(3, AudioChannelSet::disabled());
    check(plugin.setBusesLayout(layout), "VST3 rejected disabled Front/Back buses");
    check(plugin.getTotalNumInputChannels() == 18, "Bilateral negotiated input count is not 18");
    std::cout << "PASS bilateral negotiation: two active nine-channel buses / stereo\n";
    setParameter(plugin, "yaw", 0.5f);
    plugin.prepareToPlay(48000, 128);
    AudioBuffer<float> bilateral(18, 128);
    for (int block = 0; block < 96; ++block) {
        bilateral.clear();
        plugin.processBlock(bilateral, midi);
    }
    double leftEnergy = 0, rightEnergy = 0;
    for (int block = 0; block < 64; ++block) {
        bilateral.clear();
        for (int sample = 0; sample < 128; ++sample)
            bilateral.setSample(0, sample, 0.05f*std::sin(float(block*128+sample)*0.043f));
        plugin.processBlock(bilateral, midi);
        if (block >= 32)
            for (int sample = 0; sample < 128; ++sample) {
                leftEnergy += std::pow(double(bilateral.getSample(0, sample)), 2);
                rightEnergy += std::pow(double(bilateral.getSample(1, sample)), 2);
            }
    }
    check(leftEnergy > 0.001 && rightEnergy < 0.000001, "Bilateral audio mapping failed with auxiliary buses disabled");
    plugin.releaseResources();
    std::cout << "PASS bilateral W audio with auxiliary buses disabled\n";
    std::cout << "PASS actual VST3 audio: " << comparisons << " comparisons; max error=" << worst << "\n";
    return 0;
}

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    std::cout << std::unitbuf;
    try {
        check(argc >= 2, "Expected VST3 bundle path");
        VST3PluginFormat format;
        OwnedArray<PluginDescription> descriptions;
        format.findAllTypesForFile(descriptions, argv[1]);
        check(descriptions.size() == 1, "Expected exactly one VST3 audio component");
        String error;
        auto plugin = format.createInstanceFromDescription(*descriptions[0], 48000, 128, error);
        if (!plugin) throw std::runtime_error(error.toStdString());
        std::cout << "Loaded: " << plugin->getName() << " buses=" << plugin->getBusCount(true)
                  << "/" << plugin->getBusCount(false) << " default I/O="
                  << plugin->getTotalNumInputChannels() << "/" << plugin->getTotalNumOutputChannels() << "\n";
        if (plugin->getBusCount(true) == 4) {
            check(argc >= 3, "Expected processor-reference render path");
            checkFreshStateRestore(format, *descriptions[0]);
            return runMultibus(*plugin, argv[2]);
        }
        bool accepts36 = false;
        for (int inputs : {8, 16, 18, 32, 36, 64}) {
            auto layout = plugin->getBusesLayout();
            layout.inputBuses.set(0, AudioChannelSet::discreteChannels(inputs));
            layout.outputBuses.set(0, AudioChannelSet::stereo());
            const bool accepted = plugin->setBusesLayout(layout);
            std::cout << "Requested " << inputs << " discrete inputs / stereo: "
                      << (accepted ? "accepted" : "REJECTED") << "; actual "
                      << plugin->getTotalNumInputChannels() << "/" << plugin->getTotalNumOutputChannels() << "\n";
            if (inputs == 36) accepts36 = accepted && plugin->getTotalNumInputChannels() == 36;
        }
        std::cout << "Discrete 36-channel negotiation: " << (accepts36 ? "PASS" : "FAIL") << "\n";
        // Diagnostic comparison only: a fifth-order layout has 36 channels,
        // but four second-order feeds are NOT a fifth-order sound field.
        // This tests whether representable VST3 channel labels are the barrier.
        auto layout = plugin->getBusesLayout();
        layout.inputBuses.set(0, AudioChannelSet::ambisonic(5));
        layout.outputBuses.set(0, AudioChannelSet::stereo());
        check(plugin->setBusesLayout(layout), "Diagnostic 36-channel typed layout also rejected");
        std::cout << "Diagnostic typed 36-channel arrangement: accepted; actual "
                  << plugin->getTotalNumInputChannels() << "/" << plugin->getTotalNumOutputChannels() << "\n";
        // The pinned SAF example supports orders 1..10: order 2 is 1/9 normalized.
        setParameter(*plugin, "order", 1.0f/9.0f);
        setParameter(*plugin, "microphone_feeds", 1.0f);
        setParameter(*plugin, "enable_rotation", 1.0f);
        setParameter(*plugin, "pitch", 0.5f);
        setParameter(*plugin, "roll", 0.5f);
        setParameter(*plugin, "yaw", 0.5f);
        for (auto* parameter : plugin->getParameters())
            std::cout << "Parameter " << parameter->getName(100) << "=" << parameter->getCurrentValueAsText() << "\n";
        plugin->setRateAndBufferSizeDetails(48000, 128);
        plugin->prepareToPlay(48000, 128);
        AudioBuffer<float> buffer(36, 128);
        MidiBuffer midi;
        // Deliver VST3 parameter queues before preparing the new decoder order.
        buffer.clear();
        plugin->processBlock(buffer, midi);
        plugin->releaseResources();
        plugin->prepareToPlay(48000, 128);
        // Probe only W channels. At yaw 0 the L/R feeds supply L/R respectively;
        // at yaw +90 the existing mixer selects Front for L and Back for R.
        for (int yaw : {0, 90}) {
            setParameter(*plugin, "yaw", 0.5f + float(yaw)/360.0f);
            buffer.clear();
            plugin->processBlock(buffer, midi);
            plugin->releaseResources();
            plugin->prepareToPlay(48000, 128);
            for (int feed = 0; feed < 4; ++feed) {
                for (int block = 0; block < 96; ++block) {
                    buffer.clear();
                    plugin->processBlock(buffer, midi);
                }
                double energy[2]{};
                for (int block = 0; block < 64; ++block) {
                    buffer.clear();
                    for (int i = 0; i < 128; ++i)
                        buffer.setSample(feed*9, i, 0.1f * std::sin(float(block*128+i)*0.043f));
                    plugin->processBlock(buffer, midi);
                    if (block >= 32)
                        for (int ear = 0; ear < 2; ++ear)
                            for (int i = 0; i < 128; ++i) {
                                const float value = buffer.getSample(ear, i);
                                check(std::isfinite(value), "Non-finite VST3 output");
                                energy[ear] += double(value)*value;
                            }
                }
                const int activeEar = yaw == 0 ? (feed < 2 ? feed : -1) : (feed >= 2 ? feed-2 : -1);
                std::cout << "W probe yaw=" << yaw << " feed=" << feed << " energies="
                          << energy[0] << "," << energy[1] << "\n";
                for (int ear = 0; ear < 2; ++ear)
                    check(ear == activeEar ? energy[ear] > 0.0001 : energy[ear] < 0.000001,
                          "Unexpected microphone-to-ear routing through VST3");
            }
        }
        plugin->releaseResources();
        std::cout << "PASS typed-layout W-channel routing; discrete layout acceptance reported separately\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
