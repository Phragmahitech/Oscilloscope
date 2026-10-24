cmake_minimum_required(VERSION 3.22)

# --- Réglages ----------------------------------------------------------------
set(OSC_COMPANY_NAME "Phragma")                  # nom affiché comme éditeur dans le DAW
# -----------------------------------------------------------------------------
# Trois plugins : Oscilloscope (l'afficheur), Oscilloscope Sync (la sonde à mettre en tête de piste)
# et Oscilloscope Sync Instrument (la sonde pour une piste d'instrument : charge l'instrument dedans).
# Ne changer ni PLUGIN_MANUFACTURER_CODE ni PLUGIN_CODE : ce sont eux (et pas le nom)
# qui permettent aux DAW de retrouver les plugins dans les projets.

# macOS : binaire universel (Apple Silicon + Intel), compatible macOS 10.13+
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Version minimale de macOS")
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures macOS")
endif()

# Windows : runtime C++ lié en statique -> aucun "Visual C++ Redistributable" à installer
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Oscilloscope VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(OSC_BUILD_TESTS "Compiler les tests" OFF)
set(OSC_FONT_DIR "" CACHE PATH "Dossier contenant déjà les polices (sinon elles sont téléchargées)")

# --- JUCE (téléchargé automatiquement) ---------------------------------------
# Pour utiliser une copie locale : -DFETCHCONTENT_SOURCE_DIR_JUCE=/chemin/vers/JUCE
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# --- Polices (licence OFL, voir Assets/) -------------------------------------
# Michroma et IBM Plex Mono, prises dans le dépôt google/fonts à un commit fixe et vérifiées par SHA-256.
set(OSC_FONTS_COMMIT 66928797f271fed5ee428544e7229685e2d90127)
set(OSC_FONTS
    "ofl/michroma/Michroma-Regular.ttf|b62301163788bc5b7f8fcac0b74b184e34e1827e577b499ecb724da065098f87"
    "ofl/ibmplexmono/IBMPlexMono-Regular.ttf|6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46"
    "ofl/ibmplexmono/IBMPlexMono-Medium.ttf|a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755")

set(OSC_FONT_FILES "")
foreach(entry IN LISTS OSC_FONTS)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 font_path)
    list(GET parts 1 font_hash)
    get_filename_component(font_name "${font_path}" NAME)

    if(OSC_FONT_DIR)
        set(font_file "${OSC_FONT_DIR}/${font_name}")
        file(SHA256 "${font_file}" actual_hash)
        if(NOT actual_hash STREQUAL font_hash)
            message(FATAL_ERROR "Police inattendue : ${font_file}")
        endif()
    else()
        set(font_file "${CMAKE_BINARY_DIR}/fonts/${font_name}")
        if(NOT EXISTS "${font_file}")
            file(DOWNLOAD
                "https://raw.githubusercontent.com/google/fonts/${OSC_FONTS_COMMIT}/${font_path}"
                "${font_file}"
                EXPECTED_HASH SHA256=${font_hash}
                TLS_VERIFY ON
                STATUS download_status)
            list(GET download_status 0 download_code)
            if(NOT download_code EQUAL 0)
                message(FATAL_ERROR "Téléchargement impossible : ${font_path} (${download_status})")
            endif()
        endif()
    endif()

    list(APPEND OSC_FONT_FILES "${font_file}")
endforeach()

juce_add_binary_data(OscilloscopeAssets SOURCES ${OSC_FONT_FILES})
set_target_properties(OscilloscopeAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

set(OSC_COMMON_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    OSC_VERSION="${PROJECT_VERSION}")

# --- Oscilloscope ------------------------------------------------------------
juce_add_plugin(Oscilloscope
    PRODUCT_NAME                "Oscilloscope"
    COMPANY_NAME                "${OSC_COMPANY_NAME}"
    BUNDLE_ID                   "com.phragma.oscilloscope"
    PLUGIN_MANUFACTURER_CODE    Phrg
    PLUGIN_CODE                 Oscp
    FORMATS                     VST3
    VST3_CATEGORIES             Fx Analyzer
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources(Oscilloscope PRIVATE
    Source/HubPlatform.cpp
    Source/Look.cpp
    Source/ScopeProcessor.cpp
    Source/ScopeEditor.cpp)
target_compile_definitions(Oscilloscope PUBLIC ${OSC_COMMON_DEFINITIONS})
target_link_libraries(Oscilloscope
    PRIVATE
        juce::juce_audio_processors
        OscilloscopeAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# --- Oscilloscope Sync -------------------------------------------------------
juce_add_plugin(OscilloscopeSync
    PRODUCT_NAME                "Oscilloscope Sync"
    COMPANY_NAME                "${OSC_COMPANY_NAME}"
    BUNDLE_ID                   "com.phragma.oscilloscopesync"
    PLUGIN_MANUFACTURER_CODE    Phrg
    PLUGIN_CODE                 Oscs
    FORMATS                     VST3
    VST3_CATEGORIES             Fx Analyzer
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources(OscilloscopeSync PRIVATE
    Source/HubPlatform.cpp
    Source/Look.cpp
    Source/SyncProcessor.cpp)
target_compile_definitions(OscilloscopeSync PUBLIC ${OSC_COMMON_DEFINITIONS})
target_link_libraries(OscilloscopeSync
    PRIVATE
        juce::juce_audio_processors
        OscilloscopeAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# --- Oscilloscope Sync Instrument --------------------------------------------
# Instrument qui en charge un autre (VST3) pour connaître et déclarer sa latence.
juce_add_plugin(OscilloscopeSyncInstrument
    PRODUCT_NAME                "Oscilloscope Sync Instrument"
    COMPANY_NAME                "${OSC_COMPANY_NAME}"
    BUNDLE_ID                   "com.phragma.oscilloscopesyncinstrument"
    PLUGIN_MANUFACTURER_CODE    Phrg
    PLUGIN_CODE                 Osci
    FORMATS                     VST3
    VST3_CATEGORIES             Instrument
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources(OscilloscopeSyncInstrument PRIVATE
    Source/HubPlatform.cpp
    Source/Look.cpp
    Source/HostProcessor.cpp)
target_compile_definitions(OscilloscopeSyncInstrument PUBLIC ${OSC_COMMON_DEFINITIONS} JUCE_PLUGINHOST_VST3=1)
target_link_libraries(OscilloscopeSyncInstrument
    PRIVATE
        juce::juce_audio_processors
        OscilloscopeAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# --- Tests (optionnels) ------------------------------------------------------
if(OSC_BUILD_TESTS)
    enable_testing()

    # Moteur seul, sans JUCE
    add_executable(OscilloscopeEngineTests Tests/EngineTests.cpp Source/HubPlatform.cpp)
    target_include_directories(OscilloscopeEngineTests PRIVATE Source)
    add_test(NAME engine COMMAND OscilloscopeEngineTests)

    # Les deux plugins dans le même programme (paramètres, état, mesure, interface)
    juce_add_console_app(OscilloscopePluginTests PRODUCT_NAME "OscilloscopePluginTests")
    target_sources(OscilloscopePluginTests PRIVATE
        Tests/PluginTests.cpp
        Source/HubPlatform.cpp
        Source/Look.cpp
        Source/ScopeProcessor.cpp
        Source/ScopeEditor.cpp
        Source/SyncProcessor.cpp
        Source/HostProcessor.cpp)
    target_compile_definitions(OscilloscopePluginTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 OSC_TESTS=1
        JUCE_PLUGINHOST_VST3=1 OSC_TEST_SYNTH_DIR="${CMAKE_BINARY_DIR}/OscTestSynth_artefacts/$<CONFIG>/VST3"
        OSC_VERSION="${PROJECT_VERSION}")
    target_link_libraries(OscilloscopePluginTests PRIVATE
        juce::juce_audio_processors
        OscilloscopeAssets
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    add_test(NAME plugin COMMAND OscilloscopePluginTests)

    # Instrument de test (latence 110) chargé par les tests dans Oscilloscope Sync Instrument
    juce_add_plugin(OscTestSynth
        PRODUCT_NAME                "OscTestSynth"
        COMPANY_NAME                "PhragmaTests"
        PLUGIN_MANUFACTURER_CODE    PhrT
        PLUGIN_CODE                 OsTs
        FORMATS                     VST3
        IS_SYNTH                    TRUE
        NEEDS_MIDI_INPUT            TRUE
        COPY_PLUGIN_AFTER_BUILD     FALSE)
    target_sources(OscTestSynth PRIVATE Tests/TestSynth.cpp)
    target_compile_definitions(OscTestSynth PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(OscTestSynth PRIVATE juce::juce_audio_processors PUBLIC juce::juce_recommended_config_flags)
    add_dependencies(OscilloscopePluginTests OscTestSynth_VST3)
endif()
