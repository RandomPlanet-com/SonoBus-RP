# To get started on a new plugin, copy this entire folder (containing this file and C++ sources) to
# a convenient location, and then start making modifications.

# The first line of any CMake project should be a call to `cmake_minimum_required`, which checks
# that the installed CMake will be able to understand the following CMakeLists, and ensures that
# CMake's behaviour is compatible with the named version. This is a standard CMake command, so more
# information can be found in the CMake docs.

cmake_minimum_required(VERSION 3.15)


if (WIN32)
    #set (CMAKE_GENERATOR_TOOLSET ClangCL)
    #static linking in Windows
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# If we are compiling for Mac OS we want to target OS versions down to 10.9
option(UniversalBinary "Build universal binary for mac" ON)

if (APPLE)
    set (CMAKE_OSX_DEPLOYMENT_TARGET "10.10" CACHE INTERNAL "")
    if (UniversalBinary)
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE INTERNAL "")
        #set(CMAKE_OSX_ARCHITECTURES arm64 x86_64)        
    endif()
endif()







# The top-level CMakeLists.txt file for a project must contain a literal, direct call to the
# `project()` command. `project()` sets up some helpful variables that describe source/binary
# directories, and the current project version. This is a standard CMake command.

project(SonoBus VERSION 1.7.2)

include(CTest)
enable_testing()

set(BETA_VERSION_SUFFIX "")   # the base IS Jesse's 1.7.2 release; "b02" was our own March label and the About window mis-reported it as the upstream version
set(DISPLAY_VERSION "${PROJECT_VERSION}${BETA_VERSION_SUFFIX}")
# the hub server is OURS — its own version line, independent of upstream SonoBus 1.7.2.
# Shown on the web page / About as "Hub 1.0.0 (build <commit>, <date>)". Bump here on hub releases.
# Apple-style versioning — marketing version (CFBundleShortVersionString) + date build id
# (CFBundleVersion, stamped by the release scripts as YYYY.MM.DD.HHMM). The updater keeps comparing the DATE build.
set(SONOBUS_PLUS_VERSION "1.2.7")
set(SONOBUS_HUB_VERSION "1.2.7")
# the client's date build id + source commit as compile-time defines for platforms
# without an Info.plist (Windows/Linux). Mac bundles still read CFBundleVersion/SBPlusCommit stamped at release.
#   -DSONOBUS_PLUS_BUILD=YYYY.MM.DD.HHMM[-dev]   (a build script passes the configure time)
#   -DSONOBUS_PLUS_COMMIT=<short hash>           (a build script passes it; a local tree reads git)
set(SONOBUS_PLUS_BUILD "" CACHE STRING "date build id for the client on Windows/Linux (-DSONOBUS_PLUS_BUILD=YYYY.MM.DD.HHMM-dev)")
set(SONOBUS_PLUS_COMMIT "" CACHE STRING "short source commit for the client on Windows/Linux")
if (SONOBUS_PLUS_COMMIT STREQUAL "")
    if (EXISTS "${CMAKE_SOURCE_DIR}/.sbplus-commit")
        file(READ "${CMAKE_SOURCE_DIR}/.sbplus-commit" SONOBUS_PLUS_COMMIT)
        string(STRIP "${SONOBUS_PLUS_COMMIT}" SONOBUS_PLUS_COMMIT)
    else()
        execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
                        OUTPUT_VARIABLE SONOBUS_PLUS_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
endif()
set(SONOBUS_HUB_BUILD "" CACHE STRING "date build id for a Linux hub (-DSONOBUS_HUB_BUILD=YYYY.MM.DD.HHMM-dev); Mac bundles read CFBundleVersion")
# Build id = source commit. Build workers get an rsync'd tree WITHOUT .git, so the build scripts pass
# -DSONOBUS_HUB_COMMIT=<short hash>; a local tree reads git itself.
if (NOT DEFINED SONOBUS_HUB_COMMIT OR SONOBUS_HUB_COMMIT STREQUAL "")
    execute_process(COMMAND git rev-parse --short=8 HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE SONOBUS_HUB_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if (NOT SONOBUS_HUB_COMMIT)
    set(SONOBUS_HUB_COMMIT "unknown")
endif()
set(BUILDVERSION 82)


# If you've installed JUCE somehow (via a package manager, or directly using the CMake install
# target), you'll need to tell this project that it depends on the installed copy of JUCE. If you've
# included JUCE directly in your source tree (perhaps as a submodule), you'll need to tell CMake to
# include that subdirectory as part of the build.

# find_package(JUCE CONFIG REQUIRED)        # If you've installed JUCE to your system
# or


# By default we don't want Xcode schemes to be made for modules, etc
set(CMAKE_XCODE_GENERATE_SCHEME OFF)

# No ZERO_CHECK target (it helps bust cache for cmake)
set(CMAKE_SUPPRESS_REGENERATION true) 

# prevent install all
#set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY true)

# Adds all the module sources so they appear correctly in the IDE
# Must be set before JUCE is added as a sub-dir (or any targets are made)
# https://github.com/juce-framework/JUCE/commit/6b1b4cf7f6b1008db44411f2c8887d71a3348889
set_property(GLOBAL PROPERTY USE_FOLDERS YES)

# This is a failed attempt to bury ALL_BUILD in Targets/
# This should be called before any target is made
# Bug in Xcode? https://gitlab.kitware.com/cmake/cmake/-/issues/21383
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Targets")

# Create a /Modules directory in the IDE with the JUCE Module code
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Show all module sources in IDE projects" ON)


# include JUCE

add_subdirectory(deps/juce EXCLUDE_FROM_ALL )

if (WIN32)
    # always emit a PDB, even for Release, so WER crash dumps and cdb
    # stacks resolve to source lines (no effect on optimisation; /OPT:REF,ICF keep the exe small).
    # Directory scope on purpose: inside sono_add_custom_plugin_target() these would be function-local.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Zi")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif()


juce_add_modules(deps/ff_meters)



set (FormatsToBuild VST3 Standalone)

# On Mac, a AU version will be built too
if (APPLE)
    list (APPEND FormatsToBuild AU)
endif()

# On Linux, LV2 will be built too
if (UNIX)
    list (APPEND FormatsToBuild LV2)
endif()

# If you are building a VST2 or AAX plugin, CMake needs to be told where to find these SDKs on your
# system. This setup should be done before calling `juce_add_plugin`.

#juce_set_vst2_sdk_path("../VST2_SDK")
#juce_set_aax_sdk_path("../AAX_SDK_2p3p2")


if (AAX_SDK_PATH)
    juce_set_aax_sdk_path (${AAX_SDK_PATH})
    
    if (APPLE OR (NOT ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")))
      list (APPEND FormatsToBuild AAX)
    endif()
endif()

if (VST2_SDK_PATH)
    juce_set_vst2_sdk_path (${VST2_SDK_PATH})
    list (APPEND FormatsToBuild VST)
endif()


set (MacPList "<plist version=\"1.0\">
<dict>
<key>CFBundleShortVersionString</key>
<string>${SONOBUS_PLUS_VERSION}</string>
<key>CFBundleVersion</key>
<string>${BUILDVERSION}</string>
<key>SBPlusUpstreamBase</key>
<string>${PROJECT_VERSION}</string>
<key>CFBundleURLTypes</key>
<array>
     <dict>
         <key>CFBundleURLName</key>
         <string>com.randomplanet.sonobusrp</string>
         <key>CFBundleURLSchemes</key>
         <array>
              <string>sonobusrp</string>
              <string>sonobusplus</string>
         </array>
      </dict>
   </array>
<key>NSLocalNetworkUsageDescription</key>
<string>SonoBus RP connects to the hub and to other players on your local network.</string>
</dict>
</plist>")


# `juce_add_plugin` adds a static library target with the name passed as the first argument
# (AudioPluginExample here). This target is a normal CMake target, but has a lot of extra properties set
# up by default. As well as this shared code static library, this function adds targets for each of
# the formats specified by the FORMATS arguments. This function accepts many optional arguments.
# Check the readme at `docs/CMake API.md` in the JUCE repo for the full list.

function(sono_add_custom_plugin_target target_name product_name bundle_id formats is_instrument plugincode)

   if (is_instrument)
        set (vst3cats Instrument Network)
        set (vst2cat "kPlugCategSynth")
    else()
        set (vst3cats Fx Network)        
        set (vst2cat "kPlugCategEffect")
   endif()

    juce_add_plugin("${target_name}"
        IS_SYNTH "${is_instrument}"
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COMPANY_NAME "Random Planet"   # Random Planet publishes the player too
        COMPANY_COPYRIGHT "© 2026 Random Planet"   # Finder Get Info "Copyright" line (NSHumanReadableCopyright)
        BUNDLE_ID "${bundle_id}"
        MICROPHONE_PERMISSION_ENABLED TRUE
        
        # App icon: the SonoBus icon with the magenta pair re-coloured to the
        # hub palette's teal (blue pair kept); the in-app logo (sonobus_logo_96.png) carries the same scheme.
        ICON_BIG "images/sonobus_rp_icon_mac_1024.png"   # "RP" in the lens (Avenir Next Heavy, grey) instead of the "+"
        ICON_SMALL "images/sonobus_rp_icon_mac_256.png"
        NEEDS_WEB_BROWSER FALSE
        VERSION "${SONOBUS_PLUS_VERSION}"   # marketing version everywhere (Windows VERSIONINFO 1.0.0.0, JucePlugin_VersionString); the base SonoBus version is SONOBUS_UPSTREAM_BASE
        VST2_CATEGORY "${vst2cat}"
        VST3_CATEGORIES "${vst3cats}"
        AAX_CATEGORY "AAX_ePlugInCategory_None"

	LV2URI "https://sonobus.net/lv2/sonobus"
	
        # mac settings
        HARDENED_RUNTIME_ENABLED TRUE
        HARDENED_RUNTIME_OPTIONS "com.apple.security.device.audio-input"
        PLIST_TO_MERGE "${MacPList}"
        AU_MAIN_TYPE "kAudioUnitType_MusicEffect"
        
        
        # other settings...
        PLUGIN_MANUFACTURER_CODE Sono
        PLUGIN_CODE ${plugincode}
        FORMATS ${formats}
        DESCRIPTION "${product_name} - Network Audio"
        PRODUCT_NAME "${product_name}")

    juce_generate_juce_header("${target_name}")


    set (HEADER_INCLUDES
        deps/aoo/lib
        deps/aoo/deps
    )

    set (LIB_PATHS  "")

    set (PLAT_COMPILE_DEFS
        $<$<CONFIG:Debug>:LOGLEVEL=2>
        USE_CODEC_OPUS=1
        AOO_TIMEFILTER_CHECK=0
        AOO_STATIC)

    set(PlatSourceFiles
        Source/CrossPlatformUtils.h
      )


    # platform specific stuff
    if (APPLE)
        list (APPEND HEADER_INCLUDES deps/mac/include)
        list (APPEND LIB_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/deps/mac/lib)
        list (APPEND PlatSourceFiles  Source/CrossPlatformUtilsMac.mm)
    elseif (WIN32)
        list (APPEND HEADER_INCLUDES deps/windows ../asiosdk/common)
        list (APPEND PlatSourceFiles  Source/CrossPlatformUtilsWindows.cpp)

	message (STATUS "Win generator platform is: ${CMAKE_VS_PLATFORM_NAME}" )
        if ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
            list (APPEND LIB_PATHS
                $<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/deps/windows/Debug32>
                $<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/deps/windows/Release32>
            )
        else()
            list (APPEND LIB_PATHS
                $<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/deps/windows/Debug>
                $<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/deps/windows/Release>
            )
        endif()

        list (APPEND PLAT_COMPILE_DEFS
            _USE_MATH_DEFINES
            WINVER=0x0601
            _WIN32_WINNT=0x0601)
    else()
        # Linux
        list (APPEND PlatSourceFiles  Source/CrossPlatformUtilsLinux.cpp)
	list ( APPEND PLAT_COMPILE_DEFS
		JUCE_USE_MP3AUDIOFORMAT=1 )
    endif()



    set(SourceFiles
        ${PlatSourceFiles}
        Source/AutoUpdater.cpp
        Source/AutoUpdater.h
        Source/BeatToggleGrid.cpp
        Source/BeatToggleGrid.h
        Source/ChannelGroup.cpp
        Source/ChannelGroup.h
        Source/ChannelGroupsView.cpp
        Source/ChannelGroupsView.h
        Source/ChatView.cpp
        Source/ChatView.h
        Source/CompressorView.h
        Source/ConnectView.cpp
        Source/ConnectView.h
        Source/DebugLogC.h
        Source/EffectParams.cpp
        Source/EffectParams.h
        Source/EffectsBaseView.h
        Source/ExpanderView.h
        Source/GenericItemChooser.cpp
        Source/GenericItemChooser.h
        Source/JitterBufferMeter.cpp
        Source/JitterBufferMeter.h
        Source/LatencyMatchView.cpp
        Source/LatencyMatchView.h
        Source/LatencyMeasurer.cpp
        Source/LatencyMeasurer.h
        Source/LevelMeterLookAndFeelMethods.h
        Source/LocalLatencyMeasurer.h
        Source/MVerb.h
        Source/Metronome.cpp
        Source/Metronome.h
        Source/MonitorDelayView.h
        Source/OptionsView.cpp
        Source/OptionsView.h
        Source/ParametricEqView.h
        Source/PeersContainerView.cpp
        Source/PeersContainerView.h
        Source/PolarityInvertView.h
        Source/RandomSentenceGenerator.cpp
        Source/RandomSentenceGenerator.h
        Source/ReverbSendView.h
        Source/ReverbView.h
        Source/RunCumulantor.cpp
        Source/RunCumulantor.h
        Source/RunningCumulant.h
        Source/SampleEditView.cpp
        Source/SampleEditView.h
        Source/SonoCallOutBox.cpp
        Source/SonoCallOutBox.h
        Source/SonoChoiceButton.cpp
        Source/SonoChoiceButton.h
        Source/SonoDrawableButton.cpp
        Source/SonoDrawableButton.h
        Source/SonoLookAndFeel.cpp
        Source/SonoLookAndFeel.h
        Source/SonoMultiStateDrawableButton.cpp
        Source/SonoMultiStateDrawableButton.h
        Source/SonoPlaybackProgressButton.cpp
        Source/SonoPlaybackProgressButton.h
        Source/SonoStandaloneFilterApp.cpp
        Source/SonoStandaloneFilterWindow.h
        Source/SonoTextButton.cpp
        Source/SonoTextButton.h
        Source/SonoUtility.h
        Source/Soundboard.cpp
        Source/Soundboard.h
        Source/SoundboardButtonColors.h
        Source/SoundboardChannelProcessor.cpp
        Source/SoundboardChannelProcessor.h
        Source/SoundboardEditView.cpp
        Source/SoundboardEditView.h
        Source/SoundboardProcessor.cpp
        Source/SoundboardProcessor.h
        Source/SoundboardView.cpp
        Source/SoundboardView.h
        Source/SoundSampleButtonColourPicker.cpp
        Source/SoundSampleButtonColourPicker.h
        Source/SonobusPluginEditor.cpp
        Source/SonobusPluginEditor.h
        Source/SonobusPluginProcessor.cpp
        Source/SonobusPluginProcessor.h
        Source/SonobusTypes.h
        Source/SuggestNewGroupView.cpp
        Source/SuggestNewGroupView.h
        Source/VDONinjaView.h
        Source/VersionInfo.cpp
        Source/VersionInfo.h
        Source/WaveformTransportComponent.h
        Source/faustCompressor.h
        Source/faustExpander.h
        Source/faustLimiter.h
        Source/faustParametricEQ.h
        Source/mtdm.h
        Source/zitaRev.h
    )
    set(AOOSourceFiles        
        deps/aoo/lib/src/SLIP.hpp
        deps/aoo/lib/src/client.cpp
        deps/aoo/lib/src/client.hpp
        deps/aoo/lib/src/codec_opus.cpp
        deps/aoo/lib/src/codec_pcm.cpp
        deps/aoo/lib/src/common.cpp
        deps/aoo/lib/src/common.hpp
        deps/aoo/lib/src/lockfree.hpp
        deps/aoo/lib/src/net_utils.cpp
        deps/aoo/lib/src/net_utils.hpp
        deps/aoo/lib/src/server.cpp
        deps/aoo/lib/src/server.hpp
        deps/aoo/lib/src/sink.cpp
        deps/aoo/lib/src/sink.hpp
        deps/aoo/lib/src/source.cpp
        deps/aoo/lib/src/source.hpp
        deps/aoo/lib/src/sync.cpp
        deps/aoo/lib/src/sync.hpp
        deps/aoo/lib/src/time.cpp
        deps/aoo/lib/src/time.hpp
        deps/aoo/lib/src/time_dll.hpp
        deps/aoo/lib/aoo/aoo.h
        deps/aoo/lib/aoo/aoo.hpp
        deps/aoo/lib/aoo/aoo_net.h
        deps/aoo/lib/aoo/aoo_net.hpp
        deps/aoo/lib/aoo/aoo_opus.h
        deps/aoo/lib/aoo/aoo_pcm.h
        deps/aoo/lib/aoo/aoo_types.h
        deps/aoo/lib/aoo/aoo_utils.hpp
        
        deps/aoo/deps/md5/md5.c
        deps/aoo/deps/md5/md5.h
        deps/aoo/deps/oscpack/osc/OscOutboundPacketStream.cpp
        deps/aoo/deps/oscpack/osc/OscPrintReceivedElements.cpp
        deps/aoo/deps/oscpack/osc/OscReceivedElements.cpp
        deps/aoo/deps/oscpack/osc/OscTypes.cpp
        deps/aoo/deps/oscpack/osc/MessageMappingOscPacketListener.h
        deps/aoo/deps/oscpack/osc/OscException.h
        deps/aoo/deps/oscpack/osc/OscHostEndianness.h
        deps/aoo/deps/oscpack/osc/OscOutboundPacketStream.h
        deps/aoo/deps/oscpack/osc/OscPacketListener.h
        deps/aoo/deps/oscpack/osc/OscPrintReceivedElements.h
        deps/aoo/deps/oscpack/osc/OscReceivedElements.h
        deps/aoo/deps/oscpack/osc/OscTypes.h
    )

    target_sources("${target_name}" PRIVATE 
           ${SourceFiles} 
           ${AOOSourceFiles}
       )

    # No, we don't want our source buried in extra nested folders
    set_target_properties("${target_name}" PROPERTIES FOLDER "")

    # The source tree should uhhh, still look like the source tree, yo
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Source PREFIX "" FILES ${SourceFiles})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/deps/aoo PREFIX "aoo" FILES ${AOOSourceFiles})

    # Move the INTERFACE auto-created JUCE library stuff into its own folder
    source_group("JUCE Library Code" REGULAR_EXPRESSION "juce_")


    #target_include_directories("${target_name}"
    #        INTERFACE
    #            $<TARGET_PROPERTY:"${target_name}",INCLUDE_DIRECTORIES>)

    

    target_include_directories("${target_name}"
        PUBLIC
        ${HEADER_INCLUDES}
    )


    # Require at least C++17 to build `my_target`
    target_compile_features("${target_name}" PRIVATE cxx_std_17)
        

    # This cleans up the folder organization, especially on Xcode.
    # It tucks the Plugin varieties into a "Targets" folder and generate an Xcode Scheme manually
    # Xcode scheme generation is turned off globally to limit noise from other targets
    # The non-hacky way of doing this is via the global PREDEFINED_TARGETS_FOLDER property
    # However that doesn't seem to be working in Xcode
    # Not all plugin types (au, vst) available on each build type (win, macos, linux)
    foreach(target ${formats} "All")
        if(TARGET ${target_name}_${target})
            set_target_properties(${target_name}_${target} PROPERTIES
                # Tuck the actual plugin targets into a folder where they won't bother us
                FOLDER "Targets"
            
                # MacOS only: Sets the default executable that Xcode will open on build
                # For this exact path to to work, manually build the AudioPluginHost.xcodeproj in the JUCE subdir 
                # XCODE_SCHEME_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/deps/juce/extras/AudioPluginHost/Builds/MacOSX/build/Debug/AudioPluginHost.app"
            
                # Let us build the target in Xcode 
                XCODE_GENERATE_SCHEME ON)
        endif()
    endforeach()

    target_compile_definitions("${target_name}"
        PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        # Uncaught C++ exceptions on the message thread call unhandledException() (logged +
        # breadcrumb file, see SonoStandaloneFilterApp.cpp) instead of aborting with no type.
        JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
        # without this, JUCE's macOS peer paints
        # the UNION of all dirty rects per frame (macOS >=10.13 no longer supplies
        # sub-rects), so two level meters at opposite ends of a row re-composite
        # the whole row 8x/s. With it, JUCE 7 uses CoreGraphicsMetalLayerRenderer
        # and paints each dirty rect separately (falls back gracefully w/o Metal).
        JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
	JUCE_USE_WINDOWS_MEDIA_FORMAT=1
        JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
        JUCE_ASIO=1
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=0
        JUCE_JACK=1
        JUCE_ALSA=1
        JUCE_USE_ANDROID_OBOE=1
        JUCE_USE_OBOE_STABILIZED_CALLBACK=1
        JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
        FF_AUDIO_ALLOW_ALLOCATIONS_IN_MEASURE_BLOCK=0
        SONOBUS_BUILD_VERSION="${VERSION}"
        SONOBUS_PLUS_VERSION="${SONOBUS_PLUS_VERSION}"
        SONOBUS_PLUS_BUILD="${SONOBUS_PLUS_BUILD}"
        SONOBUS_PLUS_COMMIT="${SONOBUS_PLUS_COMMIT}"
        SONOBUS_UPSTREAM_BASE="${PROJECT_VERSION}"
        SONOBUSPLUS_BUILD=1
        ${PLAT_COMPILE_DEFS} )

    juce_add_binary_data("${target_name}_SBData" SOURCES
        Source/wordmaker.g
        Source/GoNotoKurrent-Regular.ttf
	Source/DejaVuSans.ttf
        localization/localized_de.txt
        localization/localized_es.txt
        localization/localized_fr.txt
        localization/localized_it.txt
        localization/localized_ja.txt
        localization/localized_nl.txt
        localization/localized_pt-br.txt
        localization/localized_pt-pt.txt
        localization/localized_ko.txt
        localization/localized_ru.txt
        localization/localized_zh-hans.txt
        images/arrow-up-narrow.svg
        images/arrow-down-narrow.svg
        images/dots.svg
        images/bar_click.wav
        images/beat_click.wav
        images/chat.svg
        images/chat_dots.svg
        images/checkmark.svg
        images/chevron_forward.svg
        images/continue.svg
        images/copy_icon.svg
        images/dice_icon_128.png
        images/dispfull.svg
        images/dispminimal.svg
        images/dots.svg
        images/dots_icon.png
        images/expand_arrow_active.svg
        images/expand_arrow_inactive.svg
        images/eyedropper.svg
        images/eye.svg
        images/eye-off.svg
        images/folder_icon.svg
        images/hear-others.svg
        images/hold.svg
        images/keyboard.svg
        images/keyboard_disabled.svg
        images/keypad-num-disabled.svg
        images/keypad-num.svg
        images/lgc_bar.wav
        images/link.svg
        images/loop_icon.svg
        images/met.svg
        images/mic.svg
        images/mic_disabled.svg
        images/mic_pointing.svg
        images/mesg-unread.svg
        images/move_updown.svg
        images/mute-others.svg
        images/network.svg
        images/oneshot.svg
        images/paste_icon.svg
        images/pause_icon.svg
        images/people.png
        images/person.png
        images/play_back_to_back.svg
        images/play_icon.svg
        images/play_simultaneous.svg
        images/play_background.svg
        images/plus_icon.svg
        images/power.svg
        images/power_sel.svg
        images/record.svg
        images/record_active_alt.svg
        images/rectape.svg
        images/reset_buffer_icon.svg
        images/send.svg
        images/send_group.svg
        images/send_group_small.svg
        images/replay_icon.svg
        images/settings_icon.svg
        images/skipback_icon.svg
        images/skipforward_icon.svg
        images/sonobus_logo_96.png
        images/soundboard.svg
        images/speaker_disabled.svg
        images/speaker_disabled_grey.svg
        images/stop.svg
        images/toggle.svg
        images/triangle_disclosure.svg
        images/urei_main.wav
        images/videocam-outline.svg
        images/x_icon.svg
    )

    set_target_properties(${target_name}_SBData PROPERTIES FOLDER "Targets")

    if (UNIX AND NOT APPLE)
       target_compile_options("${target_name}_SBData"
         PRIVATE
         -fPIC
       )

       find_library(OPUS_LIB opus)
       if (NOT OPUS_LIB)
	 message(FATAL_ERROR "opus library not found, please install libopus develop package")
       endif()

       if (JUCE_LINUX_TARGET_ARCHITECTURE MATCHES "arm" )
          message(STATUS "ARM platform, adding -march=native")
          target_compile_options(${target_name} PUBLIC
           -march=native
          )
       endif()
     
       if (TARGET ${target_name}_Standalone)
           # make linux executable all lower case
           string(TOLOWER ${target_name} tmptargname)

           set_target_properties("${target_name}_Standalone"
	     PROPERTIES
	       OUTPUT_NAME ${tmptargname}
           )
       endif()

   endif()


    target_link_directories("${target_name}" INTERFACE
        ${LIB_PATHS}
    )

    target_link_libraries("${target_name}" 
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
	    juce::juce_cryptography
            juce::juce_audio_plugin_client
            
            ff_meters
            
            ${target_name}_SBData

            opus
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
        #   juce::juce_recommended_warning_flags
        )
    if (APPLE)
        target_link_libraries("${target_name}" PRIVATE "-framework AVFoundation")   # sbRequestMicrophoneAccess
    endif()
            
endfunction()

# release identity — "SonoBus RP", com.randomplanet.SonoBusRP. A new bundle id means macOS asks
# Local Network / microphone once more on every machine (wanted: the stale Beta rules on RP never prompted again) and
# the app is no longer tied to the stock SonoBus or the Beta era. Settings migrate from the Beta file on first launch.
set(SONOBUS_MAIN_PRODUCT_NAME "SonoBus RP")
set(SONOBUS_MAIN_BUNDLE_ID "com.randomplanet.SonoBusRP")
set(SONOBUS_INST_BUNDLE_ID "com.Sonosaurus.SonoBusInstrument")

# most of the targets
sono_add_custom_plugin_target(SonoBus "${SONOBUS_MAIN_PRODUCT_NAME}" "${SONOBUS_MAIN_BUNDLE_ID}" "${FormatsToBuild}" FALSE  "NBus")

# Mobile targets
#sono_add_custom_plugin_target(SonoBusMobile "AUv3 Standalone" FALSE "NBus")

# add VSTi target
sono_add_custom_plugin_target(SonoBusInst "SonoBusInstrument" "${SONOBUS_INST_BUNDLE_ID}" "VST3" TRUE  "IBus")

# Hosted-path unit tests
if (BUILD_TESTING)
    add_executable(HostedPathUnitTests
        Source/HostedPathUnitTestsMain.cpp
    )

    target_include_directories(HostedPathUnitTests
        PRIVATE
            ${CMAKE_BINARY_DIR}/SonoBus_artefacts/JuceLibraryCode
            ${CMAKE_BINARY_DIR}/juce_binarydata_SonoBus_SBData/JuceLibraryCode
            ${CMAKE_CURRENT_SOURCE_DIR}/deps
    )

    target_link_libraries(HostedPathUnitTests
        PRIVATE
            SonoBus
            juce::juce_audio_plugin_client
            juce::juce_audio_utils
            juce::juce_dsp
    )

    add_test(NAME HostedPathUnitTests COMMAND HostedPathUnitTests)
    set_tests_properties(HostedPathUnitTests PROPERTIES LABELS "hosted;unit")

    add_executable(HostedPathIntegrationTests
        Source/HostedPathIntegrationTestsMain.cpp
    )

    target_include_directories(HostedPathIntegrationTests
        PRIVATE
            ${CMAKE_BINARY_DIR}/SonoBus_artefacts/JuceLibraryCode
            ${CMAKE_BINARY_DIR}/juce_binarydata_SonoBus_SBData/JuceLibraryCode
            ${CMAKE_CURRENT_SOURCE_DIR}/deps
    )

    target_link_libraries(HostedPathIntegrationTests
        PRIVATE
            SonoBus
            juce::juce_audio_plugin_client
            juce::juce_audio_utils
            juce::juce_dsp
    )

    add_test(NAME HostedPathIntegrationTests COMMAND HostedPathIntegrationTests)
    set_tests_properties(HostedPathIntegrationTests PROPERTIES LABELS "hosted;integration")

    add_executable(HostedPathErrorHandlingTests
        Source/HostedPathErrorHandlingTestsMain.cpp
    )

    target_include_directories(HostedPathErrorHandlingTests
        PRIVATE
            ${CMAKE_BINARY_DIR}/SonoBus_artefacts/JuceLibraryCode
            ${CMAKE_BINARY_DIR}/juce_binarydata_SonoBus_SBData/JuceLibraryCode
            ${CMAKE_CURRENT_SOURCE_DIR}/deps
    )

    target_link_libraries(HostedPathErrorHandlingTests
        PRIVATE
            SonoBus
            juce::juce_audio_plugin_client
            juce::juce_audio_utils
            juce::juce_dsp
    )

    add_test(NAME HostedPathErrorHandlingTests COMMAND HostedPathErrorHandlingTests)
    set_tests_properties(HostedPathErrorHandlingTests PROPERTIES LABELS "hosted;error-handling")
endif()

if (APPLE)   # the hub is macOS-only (mach timing, bundle stamping); Linux/Windows build the player only
# ─── SonoBus RP Hub Server (Jamulus-style GUI + headless via --nogui) ───
# Built as a Mac .app bundle so it can be launched via `open -a` on the hub
# host (the Mac the hub shares with a player). Inner binary stays callable as
# SonoBus-Hub.app/Contents/MacOS/SonoBus-Hub --nogui for existing launchd /
# restart-hub*.sh scripts.
# re-introduce JUCE GUI build
# but with CoutTee install gated off — isolates whether the prior /sb/hxfeedreq
# dispatch regression was caused by (a) CoutTee streambuf interception of
# std::cout/std::cerr from AOO threads, or (b) juce::JUCEApplicationBase::main
# event-loop integration. If dispatch works with CoutTee disabled, (a) is the
# culprit. Built as a GUI app bundle; --nogui still runs the pure headless
# code path verbatim from SonobusHubMain.cpp::runHeadless.
juce_add_gui_app(SonoBusHub
    PRODUCT_NAME "sonobus-hub"
    COMPANY_NAME "Random Planet"   # the hub is Random Planet's own product (the client keeps Sonosaurus)
    COMPANY_COPYRIGHT "© 2026 Random Planet"   # NSHumanReadableCopyright
    BUNDLE_ID "com.randomplanet.SonoBusRPHub"   # release identity
    # the hub gets its own app icon — the SonoBus icon re-coloured to the hub
    # GUI's teal palette (images/make_hub_icon.py regenerates it from sonobus_icon_mac_1024.png).
    ICON_BIG "images/sonobus_hub_icon_mac_1024.png"
    ICON_SMALL "images/sonobus_hub_icon_mac_256.png"
    # the hub opens no audio device, so macOS saw an idle
    # background GUI app and App-Napped it whenever its window lost focus —
    # recv thread demoted, hub sinks starved, relay collapsed until refocused
    #. Plist key = belt; disableAppNap() at startup = suspenders.
    # Finder/Dock/menu-bar name "SonoBus RP Hub" (executable + artefact stay sonobus-hub)
    PLIST_TO_MERGE "<plist version=\"1.0\"><dict><key>NSLocalNetworkUsageDescription</key><string>SonoBus RP Hub relays audio to players on your local network.</string><key>LSAppNapIsDisabled</key><true/><key>CFBundleDisplayName</key><string>SonoBus RP Hub</string><key>CFBundleName</key><string>SonoBus RP Hub</string><key>CFBundleShortVersionString</key><string>${SONOBUS_HUB_VERSION}</string><key>SBPlusUpstreamBase</key><string>${PROJECT_VERSION}</string></dict></plist>"
)

# the hub's About window shows its own icon
juce_add_binary_data(SonoBusHubData HEADER_NAME HubBinaryData.h NAMESPACE HubBinaryData SOURCES images/sonobus_hub_icon_mac_256.png)
target_link_libraries(SonoBusHub PRIVATE SonoBusHubData)

target_sources(SonoBusHub PRIVATE
    Source/SonobusHubMain.cpp
    Source/SonobusHubServer.cpp
    Source/SonobusHubServer.h
    Source/SonobusHubMainWindow.cpp
    Source/SonobusHubMainWindow.h
    Source/SonobusHubSettings.cpp
    Source/SonobusHubSettings.h
    Source/SonobusHubWeb.cpp
    Source/SonobusHubWeb.h
    Source/SonobusHubGeo.cpp
    Source/SonobusHubGeo.h
    Source/SonobusHubUpdater.cpp
    Source/SonobusHubUpdater.h
    Source/SonobusHubLayout.cpp
    Source/SonobusHubLayout.h
    Source/SonobusHubWebIcon.h

    deps/aoo/lib/src/source.cpp
    deps/aoo/lib/src/sink.cpp
    deps/aoo/lib/src/common.cpp
    deps/aoo/lib/src/sync.cpp
    deps/aoo/lib/src/time.cpp
    deps/aoo/lib/src/net_utils.cpp
    deps/aoo/lib/src/client.cpp
    deps/aoo/lib/src/server.cpp
    deps/aoo/lib/src/codec_opus.cpp
    deps/aoo/lib/src/codec_pcm.cpp
    deps/aoo/lib/src/SLIP.hpp
    deps/aoo/deps/md5/md5.c
    deps/aoo/deps/oscpack/osc/OscOutboundPacketStream.cpp
    deps/aoo/deps/oscpack/osc/OscPrintReceivedElements.cpp
    deps/aoo/deps/oscpack/osc/OscReceivedElements.cpp
    deps/aoo/deps/oscpack/osc/OscTypes.cpp
)

if(APPLE)
    # the stock disableAppNap helper lived only in the
    # client build; the hub never linked it (App Nap starvation, see above).
    # The hub uses its own hubDisableAppNap instead of CrossPlatformUtilsMac.mm
    # (the client target above keeps the original file).
    target_sources(SonoBusHub PRIVATE Source/SonobusHubMacUtils.mm Source/SonobusHubMacUtils.h)
endif()

juce_generate_juce_header(SonoBusHub)
# renamed the hub shows as "SonoBus RP Hub" in Finder / Dock / menu bar. The
# executable and the artefact stay "sonobus-hub" (deploy scripts, pgrep patterns and the on-disk bundle path key on it).
add_custom_command(TARGET SonoBusHub POST_BUILD
    COMMAND /usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName SonoBus RP Hub" "$<TARGET_BUNDLE_DIR:SonoBusHub>/Contents/Info.plist"
    COMMAND /usr/libexec/PlistBuddy -c "Set :CFBundleName SonoBus RP Hub" "$<TARGET_BUNDLE_DIR:SonoBusHub>/Contents/Info.plist"
    COMMENT "Stamping hub display name SonoBus RP Hub")
# third-party notices ship inside the bundle (About window points at them)
add_custom_command(TARGET SonoBusHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES-HUB.txt" "$<TARGET_BUNDLE_CONTENT_DIR:SonoBusHub>/Resources/LICENSES-HUB.txt"
    COMMENT "Copying LICENSES-HUB.txt into the hub bundle")

target_include_directories(SonoBusHub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/aoo/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/aoo/deps
    $<$<PLATFORM_ID:Darwin>:${CMAKE_CURRENT_SOURCE_DIR}/deps/mac/include>
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/deps/windows>
)

target_compile_definitions(SonoBusHub PRIVATE
    USE_CODEC_OPUS=1
    AOO_TIMEFILTER_CHECK=0
    AOO_STATIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1  # per-rect painting (see client block)
    SONOBUS_HUB_WEB_VERSION="${DISPLAY_VERSION}"
    SONOBUS_HUB_VERSION="${SONOBUS_HUB_VERSION}"
    SONOBUS_HUB_BUILD="${SONOBUS_HUB_BUILD}"
    SONOBUS_HUB_COMMIT="${SONOBUS_HUB_COMMIT}"
)

target_link_libraries(SonoBusHub PRIVATE
    # restored juce_gui_basics/extra/graphics.
    juce::juce_gui_extra
    juce::juce_gui_basics
    juce::juce_graphics
    juce::juce_events
    juce::juce_core
    juce::juce_recommended_config_flags
)

if (APPLE)
    target_link_directories(SonoBusHub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/mac/lib)
    find_library(COREFOUNDATION CoreFoundation)
    target_link_libraries(SonoBusHub PRIVATE opus ${COREFOUNDATION})
    # No "-framework AVFoundation" (only CrossPlatformUtilsMac.mm's microphone request needed it):
    # the hub no longer compiles that file; Foundation/AppKit come via JUCE.
elseif (WIN32)
    target_link_libraries(SonoBusHub PRIVATE opus ws2_32)
else()
    target_link_libraries(SonoBusHub PRIVATE opus)
endif()
endif()   # APPLE (hub target)

# LINUX HUB — headless console build of the same hub (SonobusHubServer + Settings + web status page),
# no JUCE GUI modules, no X11, same AOO sources. Target name SonoBusHub, executable sonobus-hub (matches the Mac bundle binary).
if (UNIX AND NOT APPLE)
juce_add_console_app(SonoBusHub PRODUCT_NAME "sonobus-hub" COMPANY_NAME "Random Planet")   # Random Planet
target_sources(SonoBusHub PRIVATE
    Source/SonobusHubMain.cpp
    Source/SonobusHubServer.cpp
    Source/SonobusHubServer.h
    Source/SonobusHubSettings.cpp
    Source/SonobusHubSettings.h
    Source/SonobusHubWeb.cpp
    Source/SonobusHubWeb.h
    Source/SonobusHubGeo.cpp
    Source/SonobusHubGeo.h
    Source/SonobusHubUpdater.cpp
    Source/SonobusHubUpdater.h
    Source/SonobusHubLayout.cpp
    Source/SonobusHubLayout.h
    Source/SonobusHubWebIcon.h
    deps/aoo/lib/src/source.cpp
    deps/aoo/lib/src/sink.cpp
    deps/aoo/lib/src/common.cpp
    deps/aoo/lib/src/sync.cpp
    deps/aoo/lib/src/time.cpp
    deps/aoo/lib/src/net_utils.cpp
    deps/aoo/lib/src/client.cpp
    deps/aoo/lib/src/server.cpp
    deps/aoo/lib/src/codec_opus.cpp
    deps/aoo/lib/src/codec_pcm.cpp
    deps/aoo/deps/md5/md5.c
    deps/aoo/deps/oscpack/osc/OscOutboundPacketStream.cpp
    deps/aoo/deps/oscpack/osc/OscPrintReceivedElements.cpp
    deps/aoo/deps/oscpack/osc/OscReceivedElements.cpp
    deps/aoo/deps/oscpack/osc/OscTypes.cpp
)
juce_generate_juce_header(SonoBusHub)
target_include_directories(SonoBusHub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/aoo/lib ${CMAKE_CURRENT_SOURCE_DIR}/deps/aoo/deps)
target_compile_definitions(SonoBusHub PRIVATE USE_CODEC_OPUS=1 AOO_TIMEFILTER_CHECK=0 AOO_STATIC SONOBUS_HUB_NOGUI=1 SONOBUS_HUB_WEB_VERSION="${DISPLAY_VERSION}" SONOBUS_HUB_VERSION="${SONOBUS_HUB_VERSION}" SONOBUS_HUB_BUILD="${SONOBUS_HUB_BUILD}" SONOBUS_HUB_COMMIT="${SONOBUS_HUB_COMMIT}" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(SonoBusHub PRIVATE juce::juce_core juce::juce_events juce::juce_data_structures juce::juce_recommended_config_flags opus pthread)   # data_structures for the layout ValueTree
endif()   # Linux hub

# Auto-sign standalone app bundles after build, when the developer's signing script is present
if (APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/sign_sonobus_apps.sh")
    add_custom_target(sign_sonobus_apps ALL
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sign_sonobus_apps.sh "${CMAKE_BINARY_DIR}"
        DEPENDS SonoBus_Standalone
        COMMENT "Signing SonoBus app bundles with Apple Development identity"
    )
endif()

