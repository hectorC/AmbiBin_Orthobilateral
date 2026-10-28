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

option(AMBIBIN_BUILD_VST2 "Build the existing VST2 plug-in" ON)
option(AMBIBIN_BUILD_VST3 "Build an additional VST3 plug-in (not the reference format)" OFF)
option(AMBIBIN_BUILD_TESTS "Build this plug-in's regression tests" ON)
option(AMBIBIN_INSTALL_AFTER_BUILD "Copy plug-ins to the system plug-in directory" OFF)
set(AMBIBIN_VST2_SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/VST2_SDK" CACHE PATH "Locally supplied VST2 SDK")

include(cmake/Dependencies.cmake)

set(ambibin_formats "")
if(AMBIBIN_BUILD_VST2)
    if(NOT EXISTS "${AMBIBIN_VST2_SDK}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR "VST2 SDK missing. Supply third_party/VST2_SDK or set AMBIBIN_VST2_SDK.")
    endif()
    juce_set_vst2_sdk_path("${AMBIBIN_VST2_SDK}")
    list(APPEND ambibin_formats VST)
endif()
if(AMBIBIN_BUILD_VST3)
    list(APPEND ambibin_formats VST3)
endif()
if(NOT ambibin_formats)
    message(FATAL_ERROR "Select at least one plug-in format.")
endif()

# Retain legacy identifiers/state while giving this project its own product name.
set(ambibin_target AmbiBINOrthobilateral)
juce_add_plugin(${ambibin_target}
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
    FORMATS ${ambibin_formats}
    PRODUCT_NAME "AmbiBIN Orthobilateral"
    LV2URI "http://AALTO.sparta_ambiBIN_orthoLat.${PROJECT_VERSION}"
)
juce_generate_juce_header(${ambibin_target})
target_sources(${ambibin_target} PRIVATE
    src/PluginProcessor.cpp src/PluginProcessor.h
    src/PluginEditor.cpp src/PluginEditor.h
    resources/SPARTALookAndFeel.h)
target_include_directories(${ambibin_target} PRIVATE resources)
target_compile_features(${ambibin_target} PUBLIC cxx_std_17)
target_compile_definitions(${ambibin_target} PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(${ambibin_target} PRIVATE
    saf_example_ambi_bin
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_audio_plugin_client juce::juce_audio_utils juce::juce_osc)
if(UNIX AND NOT APPLE)
    target_compile_definitions(${ambibin_target} PUBLIC PLUGIN_EDITOR_DISABLE_OPENGL=1)
else()
    target_link_libraries(${ambibin_target} PRIVATE juce::juce_opengl)
endif()

function(ambibin_copy_runtime target)
    if(WIN32 AND AMBIBIN_MKL_RUNTIME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AMBIBIN_MKL_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
foreach(format IN LISTS ambibin_formats)
    ambibin_copy_runtime(${ambibin_target}_${format})
endforeach()

if(AMBIBIN_BUILD_TESTS)
    enable_testing()
    add_executable(ambibin_tests tests/PluginTests.cpp)
    target_compile_features(ambibin_tests PRIVATE cxx_std_17)
    target_include_directories(ambibin_tests PRIVATE src resources
        $<TARGET_PROPERTY:${ambibin_target},INCLUDE_DIRECTORIES>)
    target_compile_definitions(ambibin_tests PRIVATE
        $<TARGET_PROPERTY:${ambibin_target},COMPILE_DEFINITIONS>)
    target_link_libraries(ambibin_tests PRIVATE ${ambibin_target})
    set_target_properties(ambibin_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    ambibin_copy_runtime(ambibin_tests)
    add_test(NAME ambibin_regression COMMAND ambibin_tests test-artifacts)
    set_tests_properties(ambibin_regression PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" TIMEOUT 180)
endif()

message(STATUS "Independent project: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Formats: ${ambibin_formats}; automatic installation: ${AMBIBIN_INSTALL_AFTER_BUILD}")
