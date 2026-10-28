#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
constexpr int sampleRate = 48000;
constexpr int blocks = 32;
int checks = 0;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
    ++checks;
}

// Expose JUCE's state conversion helpers to construct a genuine legacy chunk.
struct StateHelpers : PluginProcessor {
    using AudioProcessor::copyXmlToBinary;
    using AudioProcessor::getXmlFromBinary;
};

void configure(PluginProcessor& processor, int feeds, float yaw, int order = 1)
{
    processor.stopTimer(TIMER_PROCESSING_RELATED);
    processor.setAmbisonicFeedCount(feeds);
    processor.setPlayConfigDetails(feeds * (order+1)*(order+1), 2, sampleRate, ambi_bin_getFrameSize());
    processor.blendFocus = 1.0f;
    for (int feed = 0; feed < PluginProcessor::kNumAmbiFeeds; ++feed) {
        auto handle = processor.getFXHandle(feed);
        ambi_bin_setInputOrderPreset(handle, static_cast<SH_ORDERS>(order));
        ambi_bin_setDecodingMethod(handle, DECODING_METHOD_TA);
        ambi_bin_setEnableRotation(handle, 1);
        ambi_bin_setYaw(handle, yaw);
    }
    processor.prepareToPlay(sampleRate, ambi_bin_getFrameSize());
}

// Distinct broadband signals for each microphone and SH channel. They are
// generated afresh before processing because the plugin may apply gain trims.
void fillInput(AudioSampleBuffer& buffer, int block, int channelsPerFeed)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            const int sample = block * buffer.getNumSamples() + i;
            const int feed = ch / channelsPerFeed;
            buffer.setSample(ch, i, 0.04f * std::sin(0.013f * (ch+1) * sample)
                                   + ((sample % (97+feed*12)) == 0 ? 0.2f : 0.0f));
        }
}

AudioSampleBuffer render(int feeds, float yaw, int diagnostic, int order = 1,
                         float focus = 1.0f, int extraChannels = 0)
{
    PluginProcessor processor;
    configure(processor, feeds, yaw, order);
    processor.diagnosticOutputMode = diagnostic;
    processor.blendFocus = focus;
    const int frameSize = ambi_bin_getFrameSize();
    const int channelsPerFeed = (order+1)*(order+1);
    AudioSampleBuffer buffer(feeds * channelsPerFeed + extraChannels, frameSize);
    AudioSampleBuffer result(2, blocks * frameSize);
    MidiBuffer midi;
    for (int block = 0; block < blocks; ++block) {
        fillInput(buffer, block, channelsPerFeed);
        processor.processBlock(buffer, midi);
        for (int ear = 0; ear < 2; ++ear)
            result.copyFrom(ear, block * frameSize, buffer, ear, 0, frameSize);
    }
    return result;
}

void compareMix(const AudioSampleBuffer& actual,
                const AudioSampleBuffer& first, const AudioSampleBuffer& second,
                float leftA, float leftB, float rightA, float rightB)
{
    float worst = 0.0f, energy = 0.0f;
    for (int ear = 0; ear < 2; ++ear)
        for (int i = 0; i < actual.getNumSamples(); ++i) {
            const float expected = (ear == 0 ? leftA : rightA) * first.getSample(ear, i)
                                 + (ear == 0 ? leftB : rightB) * second.getSample(ear, i);
            require(std::isfinite(actual.getSample(ear, i)), "Non-finite decoded audio");
            worst = std::max(worst, std::abs(actual.getSample(ear, i) - expected));
            energy += actual.getSample(ear, i) * actual.getSample(ear, i);
        }
    require(energy > 0.001f, "Routing test rendered silence");
    require(worst < 0.00002f, "Mixer output differs from independently decoded feed references");
}

void testRouting()
{
    const float equal = std::sqrt(0.5f);
    for (float yaw : {0.0f, 90.0f, -90.0f, 180.0f}) {
        const auto left = render(2, yaw, 1);
        const auto right = render(2, yaw, 2);
        const auto mixed = render(2, yaw, 0);
        if (yaw == 0.0f)
            compareMix(mixed, left, right, 1, 0, 0, 1);
        else if (yaw == 180.0f)
            compareMix(mixed, left, right, 0, 1, 1, 0);
        else
            compareMix(mixed, left, right, equal, equal, equal, equal);
    }
    const auto left = render(2, 45, 1);
    const auto right = render(2, 45, 2);
    // Analytic unfocused baseline interpolation at 45 degrees.
    const float a = 0.5f * (1.0f + std::sqrt(0.5f));
    const float b = 1.0f - a;
    const float norm = std::sqrt(a*a+b*b);
    compareMix(render(2, 45, 0), left, right, a/norm, b/norm, b/norm, a/norm);
    compareMix(render(2, 90, 0, 1, 8), render(2, 90, 1), render(2, 90, 2),
               equal, equal, equal, equal);
    // Extra host channels must not be mistaken for active Front/Back inputs.
    compareMix(render(2, 45, 0, 1, 1, 8), left, right, a/norm, b/norm, b/norm, a/norm);
    // Existing four-feed cardinal assignments must remain unchanged.
    compareMix(render(4, 0, 0), render(4, 0, 1), render(4, 0, 2), 1, 0, 0, 1);
    compareMix(render(4, 90, 0), render(4, 90, 3), render(4, 90, 4), 1, 0, 0, 1);
    // Near sideways yaw, both microphone contributions remain significant.
    for (float yaw : {89.9f, 90.1f}) {
        const float q = std::cos(yaw * juce::MathConstants<float>::pi / 180.0f);
        const float wa = (1+q)/2, wb = (1-q)/2;
        const float scale = std::sqrt(wa*wa+wb*wb);
        compareMix(render(2, yaw, 0), render(2, yaw, 1), render(2, yaw, 2),
                   wa/scale, wb/scale, wb/scale, wa/scale);
    }
    std::cout << "PASS audio routing, four-feed regression, focus, extra channels and sideways continuity\n";
}

void testState()
{
    PluginProcessor processor;
    processor.stopTimer(TIMER_PROCESSING_RELATED);
    require(processor.getAmbisonicFeedCount() == 4, "New instance must default to four feeds");
    configure(processor, 4, 0, 2);
    require(k_feedCount == 14, "Existing host parameter indices changed");
    require(processor.getParameterDefaultValue(k_feedCount) == 1.0f &&
            processor.getParameterNumSteps(k_feedCount) == 2 &&
            processor.isParameterDiscrete(k_feedCount), "Host mode parameter must be discrete and default to four feeds");
    processor.setParameter(k_feedCount, 0.0f);
    require(processor.getAmbisonicFeedCount() == 2, "Host parameter did not select two feeds");
    require(processor.getRequiredInputChannels() == 18, "Second-order two-feed input count is wrong");
    MemoryBlock chunk;
    processor.getStateInformation(chunk);
    processor.setAmbisonicFeedCount(4);
    processor.setStateInformation(chunk.getData(), static_cast<int>(chunk.getSize()));
    require(processor.getAmbisonicFeedCount() == 2, "Two-feed state did not restore");
    auto xml = StateHelpers::getXmlFromBinary(chunk.getData(), static_cast<int>(chunk.getSize()));
    xml->removeAttribute("ambisonicFeedCount");
    StateHelpers::copyXmlToBinary(*xml, chunk);
    processor.setStateInformation(chunk.getData(), static_cast<int>(chunk.getSize()));
    require(processor.getAmbisonicFeedCount() == 4, "Legacy state must restore four feeds even into a two-feed instance");
    require(processor.getRequiredInputChannels() == 36, "Four-feed input count is wrong");
    processor.setAmbisonicFeedCount(3);
    require(processor.getAmbisonicFeedCount() == 4, "Invalid feed count must fall back to four");
    std::cout << "PASS defaults, automation, state round-trip and legacy state compatibility\n";
}

void testDiagnosticsAndMissingInputs()
{
    for (int order : {1, 2, 3}) {
        PluginProcessor processor;
        configure(processor, 2, 0, order);
        const int perFeed = (order+1)*(order+1);
        AudioSampleBuffer buffer(2*perFeed, ambi_bin_getFrameSize());
        MidiBuffer midi;
        for (int mode : {5, 6}) {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                FloatVectorOperations::fill(buffer.getWritePointer(ch), float(ch+1), buffer.getNumSamples());
            processor.diagnosticOutputMode = mode;
            processor.processBlock(buffer, midi);
            require(buffer.getSample(0, 0) == (mode == 5 ? 1 : 2), "Raw Left channel mapping failed");
            require(buffer.getSample(1, 0) == perFeed + (mode == 5 ? 1 : 2), "Raw Right channel offset is wrong for the selected order");
        }
        // Output buffers can be larger than the declared host input layout.
        processor.diagnosticOutputMode = 0;
        processor.setPlayConfigDetails(perFeed, 2, sampleRate, ambi_bin_getFrameSize());
        fillInput(buffer, 0, perFeed);
        processor.processBlock(buffer, midi);
        require(buffer.getMagnitude(0, buffer.getNumSamples()) == 0, "Insufficient host inputs must produce silence");
    }
    std::cout << "PASS raw diagnostics at orders 1-3 and missing-input handling\n";
}

void testModeChangesAndSmoothing()
{
    const int frame = ambi_bin_getFrameSize();
    MidiBuffer midi;
    PluginProcessor processor;
    configure(processor, 4, 90);
    AudioSampleBuffer buffer(16, frame);
    for (int block = 0; block < blocks; ++block) {
        fillInput(buffer, block, 4);
        processor.processBlock(buffer, midi);
    }
    processor.setAmbisonicFeedCount(2);
    for (int block = 0; block < blocks; ++block) {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }
    processor.setAmbisonicFeedCount(4);
    for (int block = 0; block < blocks; ++block) {
        buffer.clear();
        processor.processBlock(buffer, midi);
        require(std::max(buffer.getMagnitude(0, 0, frame), buffer.getMagnitude(1, 0, frame)) < 0.000001f,
                "A frozen Front/Back tail reappeared after switching modes");
    }

    processor.diagnosticOutputMode = 3;
    for (int block = 0; block < blocks; ++block) {
        fillInput(buffer, block, 4);
        processor.processBlock(buffer, midi);
    }
    processor.setAmbisonicFeedCount(2);
    processor.diagnosticOutputMode = 5;
    for (int block = 0; block < blocks; ++block) {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }
    processor.setAmbisonicFeedCount(4);
    processor.diagnosticOutputMode = 0;
    for (int block = 0; block < blocks; ++block) {
        buffer.clear();
        processor.processBlock(buffer, midi);
        require(std::max(buffer.getMagnitude(0, 0, frame), buffer.getMagnitude(1, 0, frame)) < 0.000001f,
                "Diagnostic playback left a frozen tail across mode changes");
    }

    PluginProcessor mixed, left, right;
    configure(mixed, 2, 0);
    configure(left, 2, 0);
    configure(right, 2, 0);
    left.diagnosticOutputMode = 1;
    right.diagnosticOutputMode = 2;
    AudioSampleBuffer leftBuffer(8, frame), rightBuffer(8, frame), mixedBuffer(8, frame);
    for (int block = 0; block <= blocks; ++block) {
        if (block == blocks)
            for (auto* current : {&mixed, &left, &right})
                for (int feed = 0; feed < 4; ++feed)
                    ambi_bin_setYaw(current->getFXHandle(feed), 90);
        fillInput(leftBuffer, block, 4);
        fillInput(rightBuffer, block, 4);
        fillInput(mixedBuffer, block, 4);
        left.processBlock(leftBuffer, midi);
        right.processBlock(rightBuffer, midi);
        mixed.processBlock(mixedBuffer, midi);
    }
    const float equal = std::sqrt(0.5f);
    for (int i = 0; i < frame; ++i) {
        const float position = float(i+1)/frame;
        const float expectedLeft = (1+position*(equal-1))*leftBuffer.getSample(0, i)
                                 + position*equal*rightBuffer.getSample(0, i);
        const float expectedRight = position*equal*leftBuffer.getSample(1, i)
                                  + (1+position*(equal-1))*rightBuffer.getSample(1, i);
        require(std::abs(mixedBuffer.getSample(0, i)-expectedLeft) < 0.00002f &&
                std::abs(mixedBuffer.getSample(1, i)-expectedRight) < 0.00002f,
                "Two-feed rotation did not retain the per-sample gain ramp");
    }
    std::cout << "PASS mode changes without frozen tails and per-sample rotation smoothing\n";
}

void testEditor(const File& outputDirectory)
{
    PluginProcessor processor;
    configure(processor, 4, 0);
    std::unique_ptr<PluginEditor> editor(static_cast<PluginEditor*>(processor.createEditor()));
    auto* selector = dynamic_cast<ComboBox*>(editor->findChildWithID("microphone-feed-count"));
    require(selector != nullptr, "Microphone selector is missing");
    selector->setSelectedId(2, sendNotificationSync);
    require(processor.getAmbisonicFeedCount() == 2, "Editor selector did not change processor mode");
    require(!editor->s_gainFront->isEnabled() && !editor->s_gainBack->isEnabled(), "Unavailable feed trims should be disabled");
    outputDirectory.createDirectory();
    PNGImageFormat png;
    {
        auto stream = outputDirectory.getChildFile("two-mics.png").createOutputStream();
        if (stream != nullptr) {
            stream->setPosition(0);
            stream->truncate();
        }
        require(stream != nullptr && png.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()), *stream), "Could not capture two-mic editor");
    }
    selector->setSelectedId(4, sendNotificationSync);
    require(editor->s_gainFront->isEnabled() && editor->s_gainBack->isEnabled(), "Four-feed trims were not restored");
    {
        auto stream = outputDirectory.getChildFile("four-mics.png").createOutputStream();
        if (stream != nullptr) {
            stream->setPosition(0);
            stream->truncate();
        }
        require(stream != nullptr && png.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()), *stream), "Could not capture four-mic editor");
    }
    std::cout << "PASS editor selector and control availability; snapshots saved\n";
}
}

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juce;
    try {
        if (argc > 2 && String(argv[1]) == "--editor-only") {
            testEditor(File::getCurrentWorkingDirectory().getChildFile(argv[2]));
            return 0;
        }
        testState();
        testDiagnosticsAndMissingInputs();
        testRouting();
        testModeChangesAndSmoothing();
        if (argc > 1)
            testEditor(File::getCurrentWorkingDirectory().getChildFile(argv[1]));
        std::cout << "All plugin checks passed (" << checks << " assertions).\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
