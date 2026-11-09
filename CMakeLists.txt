cmake_minimum_required(VERSION 3.22)

# runtime statico su Windows: il .vst3 funziona anche senza i "Visual C++ Redistributable"
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(CueOSC VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(CueOSC
    COMPANY_NAME "Loiudice"
    PLUGIN_MANUFACTURER_CODE Flou
    PLUGIN_CODE Cuos
    FORMATS VST3
    PRODUCT_NAME "CueOSC"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Tools)

target_sources(CueOSC PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(CueOSC PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(CueOSC
    PRIVATE
        juce::juce_audio_utils
        juce::juce_osc
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(WIN32)
    target_link_libraries(CueOSC PRIVATE winmm)
endif()

# --- test senza REAPER (OSC su loopback) ---
option(CUEOSC_TESTS "Compila i test" ON)
if(CUEOSC_TESTS)
    add_executable(CueOSCTests Tests/main.cpp)
    target_link_libraries(CueOSCTests PRIVATE CueOSC)
    target_include_directories(CueOSCTests PRIVATE $<TARGET_PROPERTY:CueOSC,INCLUDE_DIRECTORIES>)
    target_compile_definitions(CueOSCTests PRIVATE $<TARGET_PROPERTY:CueOSC,COMPILE_DEFINITIONS>)
endif()

if(CUEOSC_TESTS)
    enable_testing()
    add_test(NAME CueOSCTests COMMAND CueOSCTests)
endif()
