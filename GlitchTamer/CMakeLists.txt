# Built from the repository root, which fetches JUCE.

juce_add_plugin(GlitchTamer
    COMPANY_NAME "GlitchTamer"
    PLUGIN_MANUFACTURER_CODE Gtmr
    PLUGIN_CODE Gtm1
    FORMATS VST3 Standalone
    PRODUCT_NAME "GlitchTamer"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Restoration)

target_sources(GlitchTamer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/OfflineRender.cpp)

target_compile_definitions(GlitchTamer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(GlitchTamer
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
# No juce_recommended_lto_flags: MSVC 14.44 hits intermittent C1001 during LTCG.

# Command-line batch tool / tuning harness (no JUCE dependency, WAV in/out).
add_executable(glitchtamer-cli Tools/glitchtamer_cli.cpp)
target_include_directories(glitchtamer-cli PRIVATE Source)
