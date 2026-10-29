cmake_minimum_required(VERSION 3.22)
project(AmbiBINOrthobilateral VERSION 0.0.4 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    add_compile_options(/MP4)
endif()

option(AMBIBIN_BUILD_VST2 "Build VST2 when the locally supplied SDK is available" ON)
option(AMBIBIN_BUILD_VST3 "Build the multi-microphone-bus VST3 plug-in" ON)
option(AMBIBIN_BUILD_TESTS "Build the selected formats' regression tests" ON)
option(AMBIBIN_INSTALL_AFTER_BUILD "Copy plug-ins to the system plug-in directory" OFF)
set(AMBIBIN_VST2_SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/VST2_SDK" CACHE PATH "Locally supplied VST2 SDK")

set(ambibin_vst2_available FALSE)
if(AMBIBIN_BUILD_VST2)
    if(EXISTS "${AMBIBIN_VST2_SDK}/pluginterfaces/vst2.x/aeffect.h" AND
       EXISTS "${AMBIBIN_VST2_SDK}/pluginterfaces/vst2.x/aeffectx.h")
        set(ambibin_vst2_available TRUE)
    else()
        message(STATUS "VST2 SDK headers not found at ${AMBIBIN_VST2_SDK}; skipping VST2.")
    endif()
endif()
if(NOT ambibin_vst2_available AND NOT AMBIBIN_BUILD_VST3)
    message(FATAL_ERROR "No available plug-in format. Enable AMBIBIN_BUILD_VST3 or supply the VST2 SDK.")
endif()

include(cmake/Dependencies.cmake)
if(ambibin_vst2_available)
    juce_set_vst2_sdk_path("${AMBIBIN_VST2_SDK}")
endif()
if(AMBIBIN_BUILD_TESTS)
    enable_testing()
endif()

function(ambibin_copy_runtime target)
    if(WIN32 AND AMBIBIN_MKL_RUNTIME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AMBIBIN_MKL_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

function(ambibin_add_format target format multibus)
    # Separate shared-code libraries preserve the established VST2 input layout.
    juce_add_plugin(${target}
        VERSION "${PROJECT_VERSION}"
        COMPANY_NAME "AALTO"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD ${AMBIBIN_INSTALL_AFTER_BUILD}
        PLUGIN_MANUFACTURER_CODE LEOM
        PLUGIN_CODE ABBI
        FORMATS ${format}
        PRODUCT_NAME "AmbiBIN Orthobilateral"
        LV2URI "http://AALTO.sparta_ambiBIN_orthoLat.${PROJECT_VERSION}")
    juce_generate_juce_header(${target})
    target_sources(${target} PRIVATE
        src/PluginProcessor.cpp src/PluginProcessor.h
        src/PluginEditor.cpp src/PluginEditor.h resources/SPARTALookAndFeel.h)
    target_include_directories(${target} PRIVATE resources)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_definitions(${target} PUBLIC
        AMBIBIN_MULTIBUS=${multibus}
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    if(multibus)
        target_include_directories(${target} PRIVATE
            "${AMBIBIN_JUCE_SOURCE}/modules/juce_audio_processors/format_types/VST3_SDK")
    endif()
    target_link_libraries(${target} PRIVATE saf_example_ambi_bin
        juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
        juce::juce_audio_plugin_client juce::juce_audio_utils juce::juce_osc)
    if(UNIX AND NOT APPLE)
        target_compile_definitions(${target} PUBLIC PLUGIN_EDITOR_DISABLE_OPENGL=1)
    else()
        target_link_libraries(${target} PRIVATE juce::juce_opengl)
    endif()
    ambibin_copy_runtime(${target}_${format})

    if(AMBIBIN_BUILD_TESTS)
        if(multibus)
            set(test_target ambibin_vst3_tests)
            set(test_source tests/MultiBusTests.cpp)
            set(test_name ambibin_vst3_regression)
            set(test_artifacts test-artifacts-vst3)
            set(test_bin "${CMAKE_BINARY_DIR}/bin/vst3")
        else()
            set(test_target ambibin_tests)
            set(test_source tests/PluginTests.cpp)
            set(test_name ambibin_regression)
            set(test_artifacts test-artifacts)
            set(test_bin "${CMAKE_BINARY_DIR}/bin")
        endif()
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_source}")
            message(STATUS "Optional local test source ${test_source} is absent; skipping ${test_target}.")
            return()
        endif()
        add_executable(${test_target} ${test_source})
        target_compile_features(${test_target} PRIVATE cxx_std_17)
        target_include_directories(${test_target} PRIVATE src resources
            $<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>)
        target_compile_definitions(${test_target} PRIVATE
            $<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>)
        target_link_libraries(${test_target} PRIVATE ${target})
        set_target_properties(${test_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${test_bin}")
        ambibin_copy_runtime(${test_target})
        add_test(NAME ${test_name} COMMAND ${test_target} ${test_artifacts})
        set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" TIMEOUT 180)
    endif()
endfunction()

if(ambibin_vst2_available)
    ambibin_add_format(AmbiBINOrthobilateral VST 0)
endif()
if(AMBIBIN_BUILD_VST3)
    ambibin_add_format(AmbiBINOrthobilateralVST3 VST3 1)
endif()
message(STATUS "AmbiBIN Orthobilateral: VST2=${ambibin_vst2_available}; VST3=${AMBIBIN_BUILD_VST3}; automatic installation=${AMBIBIN_INSTALL_AFTER_BUILD}")
