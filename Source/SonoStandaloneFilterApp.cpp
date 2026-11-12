// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell
// Modifications Copyright (C) 2026 Jason Buecker (Random Planet)

/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2017 - ROLI Ltd.

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 5 End-User License
   Agreement and JUCE 5 Privacy Policy (both updated and effective as of the
   27th April 2017).

   End User License Agreement: www.juce.com/juce-5-licence
   Privacy Policy: www.juce.com/juce-5-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

// needed for crappy windows
#define NOMINMAX


#include <iostream>
#include "JuceHeader.h"
#include "SbDataPaths.h"
// System headers for the unhandled-exception breadcrumb MUST be at global scope
#ifdef __GNUG__
#include <cxxabi.h>   // GCC/Clang only; MSVC has no abi::
#endif
#include <typeinfo>
#include <exception>
#include <ctime>
#include <csignal>
#include <atomic>

#include "juce_core/system/juce_TargetPlatform.h"
#if SONOBUSPLUS_BUILD
static std::atomic<bool> sbPlusSigQuit{false};   // Set by the SIGUSR1 handler
#endif
#include "juce_audio_plugin_client/detail/juce_CheckSettingMacros.h"

#if !JUCE_LINUX
#include "juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h"
#include "juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h"
#include "juce_gui_basics/native/juce_WindowsHooks_windows.h"
#endif

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>


// You can set this flag in your build if you need to specify a different
// standalone JUCEApplication class for your app to use. If you don't
// set it then by default we'll just create a simple one as below.
//#if ! JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

extern juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

#include "SonoStandaloneFilterWindow.h"
#include "SonoLookAndFeel.h"

#include "SonobusPluginEditor.h"

#if JUCE_ANDROID
#include "android/SonoBusActivity.h"

#if JUCE_USE_ANDROID_OPENSLES || JUCE_USE_ANDROID_OBOE
  #include "juce_audio_devices/native/juce_HighPerformanceAudioHelpers_android.h"
#endif

#endif

namespace juce
{

//==============================================================================
// Unhandled-exception breadcrumb: every caught message-thread exception and every terminate() writes type/what/site
// to stderr and appends to unhandled_exceptions.log in the log folder, which survives the process.
static void sbLogUnhandled(const char* where, const char* what, const char* file, int line)
{
    const char* tname = "(unknown)";
#ifdef __GNUG__
    if (auto* ti = abi::__cxa_current_exception_type()) { int st = 0; if (char* dm = abi::__cxa_demangle(ti->name(), nullptr, nullptr, &st)) tname = dm; else tname = ti->name(); }
#else
    tname = "(exception type not available on this platform)";   // (Windows port)
#endif
    char ts[32]; ::std::time_t now = ::std::time(nullptr); ::std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", ::std::localtime(&now));
    fprintf(stderr, "[SB-EXC] %s %s type=%s what=%s at %s:%d\n", ts, where, tname, what ? what : "", file ? file : "?", line);
    if (const char* home = getenv("HOME")) {
        ::std::string path = sbLogDir() + "/unhandled_exceptions.log";
        if (FILE* f = fopen(path.c_str(), "a")) { fprintf(f, "%s %s type=%s what=%s at %s:%d\n", ts, where, tname, what ? what : "", file ? file : "?", line); fclose(f); }
    }
}
// One-time move out of the legacy home-folder location. A folder that also holds other things stays, with links left
// for the moved files.
static void sbMoveOldDataFolder()
{
    const File oldDir = File::getSpecialLocation(File::userHomeDirectory).getChildFile("SonoBus-Plus");
    const File dataDir { String { sbDataDir() } }, logDir { String { sbLogDir() } };
    dataDir.createDirectory();
    logDir.createDirectory();
    if (!oldDir.isDirectory()) return;

    struct Item { const char* oldName; File target; };
    const Item items[] = {
        { "sb_taps.conf",            dataDir.getChildFile("sb_taps.conf") },
        { "sbplus-auto.log",         logDir.getChildFile("auto.log") },
        { "sbplus-drops.log",        logDir.getChildFile("drops.log") },
        { "sbplus-hxlat.log",        logDir.getChildFile("hxlat.log") },
        { "unhandled_exceptions.log", logDir.getChildFile("unhandled_exceptions.log") },
    };
    bool otherThings = false;
    for (const auto& entry : oldDir.findChildFiles(File::findFilesAndDirectories | File::ignoreHiddenFiles, false)) {
        bool known = false;
        for (const auto& item : items) known = known || entry.getFileName() == item.oldName;
        otherThings = otherThings || !known;
    }
    for (const auto& item : items) {
        const File oldFile = oldDir.getChildFile(item.oldName);
        if (!oldFile.existsAsFile() || oldFile.isSymbolicLink()) continue;
        if (!item.target.existsAsFile() && !oldFile.moveFileTo(item.target)) continue;
        if (oldFile.existsAsFile()) oldFile.deleteFile();
       #if !JUCE_WINDOWS
        if (otherThings) item.target.createSymbolicLink(oldFile, true);
       #endif
    }
    if (!otherThings) oldDir.deleteRecursively();
}

static void sbTerminateHandler()
{
    const char* what = "";
    try { if (auto ep = ::std::current_exception()) ::std::rethrow_exception(ep); } catch (const ::std::exception& e) { what = e.what(); } catch (...) {}
    sbLogUnhandled("TERMINATE", what, nullptr, 0);
    abort();
}

class SonobusStandaloneFilterApp  : public JUCEApplication, public Timer
{
public:
    SonobusStandaloneFilterApp()
    {
        PluginHostType::jucePlugInClientCurrentWrapperType = AudioProcessor::wrapperType_Standalone;

        PropertiesFile::Options options;

        options.applicationName     = getApplicationName();
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support/SonoBus";
       #if JUCE_LINUX
        options.folderName          = "~/.config/sonobus";
       #else
        options.folderName          = "";
       #endif

        appProperties.setStorageParameters (options);
        {   // / carry the player's settings over from the previous app name, once
            const File newf = options.getDefaultFile();
            if (!newf.existsAsFile()) {
                for (const char* prev : { "SonoBus Plus.settings", "SonoBus Plus Beta.settings" }) {
                    // Mac/Linux: one shared folder, the old file sits beside the new one. Windows: JUCE keys the
                    // folder on the app name too (%APPDATA%\SonoBus RP\), so also look in the sibling folder named
                    // after the old app.
                    File oldf = newf.getSiblingFile(prev);
                    if (!oldf.existsAsFile()) {
                        const String oldApp = String(prev).upToLastOccurrenceOf(".settings", false, false);
                        oldf = newf.getParentDirectory().getSiblingFile(oldApp).getChildFile(prev);
                    }
                    if (!oldf.existsAsFile()) continue;
                    newf.getParentDirectory().createDirectory();
                    oldf.copyFileTo(newf);
                    std::cerr << "[SB-SETTINGS] migrated " << oldf.getFullPathName() << " -> " << newf.getFullPathName() << "\n";
                    break;
                }
            }
        }

#if JUCE_LINUX
        // we moved linux settings location in 1.3.19, one time change
        File oldsettings("~/.config/SonoBus.settings");
        if (oldsettings.exists()) {
            File newsettings = options.getDefaultFile();
            if (!newsettings.getParentDirectory().exists()) {
                newsettings.getParentDirectory().createDirectory();
                oldsettings.moveFileTo(newsettings);
            }
        }
#endif
    }

    ~SonobusStandaloneFilterApp()
    {
#if JUCE_ANDROID && JUCE_OPENGL
        detachGL();
#endif
    }

    const String getApplicationName() override
    {
       #if SONOBUSPLUS_BUILD
        return "SonoBus RP";   // No more "Beta"
       #else
        return JucePlugin_Name;
       #endif
    }
    const String getApplicationVersion() override           { return JucePlugin_VersionString; }
   #if SONOBUSPLUS_BUILD
    // One instance: two copies logged in under the same name would make the hub's reconnect-wins eviction bounce them
    // forever, with every player rebuilding that row. A second launch hands its sonobus:// URL (if any) to the
    // running copy and brings it to the front instead.
    bool moreThanOneInstanceAllowed() override              { return false; }
   #else
    bool moreThanOneInstanceAllowed() override              { return true; }
   #endif

    SonoLookAndFeel  sonoLNF;
   #if SONOBUSPLUS_BUILD
    struct SbQuitWatch : public Timer {
        File quitFile;   // No signals on Windows, so an external tool drops a QUIT file next to the settings
        void timerCallback() override {
#ifdef _WIN32
            if (quitFile != File() && quitFile.existsAsFile()) { quitFile.deleteFile(); sbPlusSigQuit.store(true); }
#endif
            if (!sbPlusSigQuit.exchange(false)) return;
            sbPlusQuitWithoutConfirm.store(true);
            if (auto* app = JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
        }
    } mQuitWatch;
    // Headless players have no editor timer, so drive the 10-s health tick from a plain 1-s timer: /sb/hxbuf (live
    // buffer depth, underruns/pops/xruns), the [SB-AUTO] decision log and the hxlat accounting.
    struct SbHeadlessHealth : public Timer {
        StandalonePluginHolder* holder = nullptr;
        void timerCallback() override {
            if (holder == nullptr) return;
            if (auto* sp = dynamic_cast<SonobusAudioProcessor*>(holder->processor.get()))
                if (auto* ad = holder->deviceManager.getCurrentAudioDevice())
                    sp->sbAudioHealthTick(ad->getXRunCount(), ad->getCurrentSampleRate(), ad->getCurrentBufferSizeSamples());
        }
    } mHeadlessHealth;
   #endif

    AooServerConnectionInfo cmdlineConnInfo;

    bool copyInfo = false;
    bool doInitialConnect = false;
    bool doImmediateQuit = false;
    bool doHeadless = false;
    String loadSetupFilename;
    String cmdlineArgUrl;

    virtual StandalonePluginHolder* createHeadlessPlugin ()
    {
#ifdef JucePlugin_PreferredChannelConfigurations
        StandalonePluginHolder::PluginInOuts channels[] = { JucePlugin_PreferredChannelConfigurations };
#endif

        AudioDeviceManager::AudioDeviceSetup setupOptions;
        setupOptions.sampleRate = 48000;
#if JUCE_MAC
        setupOptions.bufferSize = 128;
#elif JUCE_ANDROID
        setupOptions.bufferSize = 192;
        setupOptions.sampleRate = AndroidHighPerformanceAudioHelpers::getNativeSampleRate();
#else
        setupOptions.bufferSize = 256;
#endif

        File settingsFile = appProperties.getStorageParameters().getDefaultFile();
        File crashSentinelFile = settingsFile.getSiblingFile("SENTINEL");
        if (crashSentinelFile.existsAsFile()) {
            DBG("CRASH SENTINEL STILL EXISTS, moving old settings away!");
            File oldfile = settingsFile.getSiblingFile(String("POSSIBLY_BAD_") + Time::getCurrentTime().formatted("%Y-%m-%d_%H.%M.%S"));
            settingsFile.moveFileTo(oldfile);
            Timer::callAfterDelay (800, []()
                                   {
                AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon,
                                                  TRANS("Crashed Last Time"),
                                                  TRANS("Looks like you crashed on launch last time, restoring default settings!"));
            });
        }
        else {
            crashSentinelFile.create();
        }

        String prefDevname;

        auto plugh = new StandalonePluginHolder (appProperties.getUserSettings(), false,
                                                 prefDevname, &setupOptions
#ifdef JucePlugin_PreferredChannelConfigurations
                                                 , juce::Array<StandalonePluginHolder::PluginInOuts> (channels, juce::numElementsInArray (channels))
#else
                                                 , {}
#endif
                                                 , false);

        // JUCE's reloadPluginState() only reads base64 "filterState".
        // SonoBus settings files use "filterStateXML" (XML format).
        // If JUCE didn't load state, load from filterStateXML here.
        if (auto* settings = appProperties.getUserSettings()) {
            bool hasBase64 = settings->containsKey("filterState");
            bool hasXml = settings->containsKey("filterStateXML");
            if (!hasBase64 && hasXml) {
                String filtxml = settings->getValue("filterStateXML");
                MemoryBlock data;
                data.replaceAll(filtxml.toUTF8(), filtxml.getNumBytesAsUTF8());
                if (data.getSize() > 0 && plugh->processor != nullptr) {
                    if (auto* sp = dynamic_cast<SonobusAudioProcessor*>(plugh->processor.get()))
                        sp->setStateInformationWithOptions(data.getData(), (int) data.getSize(), true, true, true);   // Xmlformat=true (was binary parse of XML text)
                    else
                        plugh->processor->setStateInformation(data.getData(), (int) data.getSize());
                }
            }
        }

        // if we got here, we didn't crash on initialization!
        crashSentinelFile.deleteFile();

        return plugh;
    }

    virtual StandaloneFilterWindow* createWindow()
    {
       #ifdef JucePlugin_PreferredChannelConfigurations
        StandalonePluginHolder::PluginInOuts channels[] = { JucePlugin_PreferredChannelConfigurations };
       #endif

        AudioDeviceManager::AudioDeviceSetup setupOptions;
        setupOptions.sampleRate = 48000;
#if JUCE_MAC
        setupOptions.bufferSize = 128;
#elif JUCE_ANDROID
        setupOptions.bufferSize = 192;
        setupOptions.sampleRate = AndroidHighPerformanceAudioHelpers::getNativeSampleRate();
#else
        setupOptions.bufferSize = 256;
#endif

        File settingsFile = appProperties.getStorageParameters().getDefaultFile();
        File crashSentinelFile = settingsFile.getSiblingFile("SENTINEL");
        if (crashSentinelFile.existsAsFile()) {
            DBG("CRASH SENTINEL STILL EXISTS, moving old settings away!");
            File oldfile = settingsFile.getSiblingFile(String("POSSIBLY_BAD_") + Time::getCurrentTime().formatted("%Y-%m-%d_%H.%M.%S"));
            settingsFile.moveFileTo(oldfile);
            Timer::callAfterDelay (800, []()
            {
                AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon,
                                                  TRANS("Crashed Last Time"),
                                                  TRANS("Looks like you crashed on launch last time, restoring default settings!"));
            });
        }
        else {
            crashSentinelFile.create();
        }

        LookAndFeel::setDefaultLookAndFeel(&sonoLNF);

        auto wind = new StandaloneFilterWindow (getApplicationName(),
                                           LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                                           appProperties.getUserSettings(),
                                           false, {}, &setupOptions
                                          #ifdef JucePlugin_PreferredChannelConfigurations
                                           , juce::Array<StandalonePluginHolder::PluginInOuts> (channels, juce::numElementsInArray (channels))
                                          #else
                                           , {}
                                          #endif
                                          //#if JUCE_DONT_AUTO_OPEN_MIDI_DEVICES_ON_MOBILE
                                           , false
                                          //#endif
                                           );

        // JUCE's reloadPluginState() only reads base64 "filterState".
        // SonoBus settings files use "filterStateXML" (XML format).
        // If JUCE didn't load state, load from filterStateXML here.
        if (wind->pluginHolder != nullptr && wind->pluginHolder->processor != nullptr) {
            File settingsXmlFile = appProperties.getStorageParameters().getDefaultFile();
            if (settingsXmlFile.existsAsFile()) {
                XmlDocument doc(settingsXmlFile);
                auto root = doc.getDocumentElement();
                if (root != nullptr) {
                    for (auto* child : root->getChildIterator()) {
                        if (child->getStringAttribute("name") == "filterStateXML") {
                            auto* stateXml = child->getFirstChildElement();
                            if (stateXml != nullptr) {
                                String filtxml = stateXml->toString();
                                MemoryBlock data;
                                data.replaceAll(filtxml.toUTF8(), filtxml.getNumBytesAsUTF8());
                                if (data.getSize() > 0) {
                                    if (auto* sp = dynamic_cast<SonobusAudioProcessor*>(wind->pluginHolder->processor.get()))
                                        sp->setStateInformationWithOptions(data.getData(), (int) data.getSize(), true, true, true);   // Xmlformat=true
                                    else
                                        wind->pluginHolder->processor->setStateInformation(data.getData(), (int) data.getSize());
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }

        // if we got here, we didn't crash on initialization!
        crashSentinelFile.deleteFile();

        return wind;
    }

    void setupDefaultConnInfo() {
        String username;// = processor.getCurrentUsername();

        if (username.isEmpty()) {
#if JUCE_IOS
            //String username = SystemStats::getFullUserName(); //SystemStats::getLogonName();
            username = SystemStats::getComputerName(); //SystemStats::getLogonName();
#else
            username = SystemStats::getFullUserName(); //SystemStats::getLogonName();
            //if (username.length() > 0) username = username.replaceSection(0, 1, username.substring(0, 1).toUpperCase());
#endif
        }
        if (username.isEmpty()) { // fallback
            username = SystemStats::getComputerName();
        }

        cmdlineConnInfo.userName = username.trim();

        cmdlineConnInfo.serverHost = DEFAULT_SERVER_HOST;
        cmdlineConnInfo.serverPort = DEFAULT_SERVER_PORT;
    }

    static void printCommandDescription (const ArgumentList& args, const ConsoleApplication::Command& command,
                                         int descriptionIndent)
    {
        auto carg = command.argumentDescription;

        if (carg.length() > descriptionIndent)
            std::cout << "  " << carg << std::endl << String().paddedRight (' ', descriptionIndent);
        else
            std::cout << "  " << carg.paddedRight (' ', descriptionIndent);

        std::cout << command.shortDescription << std::endl;

        if (command.longDescription.isNotEmpty()) {
            std::cout << "     " << command.longDescription << std::endl;
        }
    }

    void printCommandList (ConsoleApplication & capp, const ArgumentList& args) const
    {
        int descriptionIndent = 4;
        auto commands = capp.getCommands();

        for (auto& c : commands)
            descriptionIndent = std::max (descriptionIndent, c.argumentDescription.length());

        descriptionIndent = std::min (descriptionIndent + 2, 40);

        for (auto& c : commands)
            printCommandDescription (args, c, descriptionIndent);

        std::cout << std::endl;
    }

    void handleCommandLine()
    {
        ConsoleApplication app;

        const String versionSpec("-v|--version");
        const String helpSpec("-h|--help");

        const String serverSpec("-c|--connectionserver");
        const String serverSpecDesc("-c|--connectionserver <address[:port]>");

        const String groupSpec("-g|--group");
        const String groupSpecDesc("-g|--group <groupname>");

        const String groupPassSpec("-p|--group-password");
        const String groupPassSpecDesc("-p|--group-password <password>");

        const String userNameSpec("-n|--username");
        const String userNameSpecDesc("-n|--username <username>");

        const String headlessSpec("-q|--headless");
        const String headlessSpecDesc("-q|--headless");

        const String loadSetupSpec("-l|--load-setup");
        const String loadSetupSpecDesc("-l|--load-setup <setup-filename>");

        

        app.addCommand ({ helpSpec, helpSpec, TRANS("Prints the list of commands"), {}, nullptr });
        app.addCommand ({ versionSpec, versionSpec, TRANS("Prints the current version number only"), {}, nullptr });


        auto params = getCommandLineParameterArray();
        ArgumentList arglist(getApplicationName(), params);

        app.addCommand ({ groupSpec, groupSpecDesc,
            TRANS("Specify the stage name to immediately connect to upon launch"), {},
            nullptr
        });

        app.addCommand ({ userNameSpec, userNameSpecDesc,
            TRANS("Specify the displayed username for yourself when connecting to a stage"), {},
            nullptr
        });

        app.addCommand ({ groupPassSpec, groupPassSpecDesc,
            TRANS("Specify the password to use for the stage name to connect to (optional)"), {},
            nullptr
        });

        app.addCommand ({ serverSpec, serverSpecDesc,
            TRANS("Specify connection server to use when connecting to a stage (optional)"), {},
            nullptr
        });

        app.addCommand ({ loadSetupSpec, loadSetupSpecDesc,
            TRANS("Specify the filename of a setup file to load."),
            TRANS("The setup file can be created using the Save Setup feature from the full application, and includes any device selection, input mixer setup, and all other options. If you don't specify a full or relative pathname it will look for a preset file by that name in the last used setup folder."),
            nullptr
        });

        app.addCommand ({ headlessSpec, headlessSpecDesc,
            TRANS("If specified, no GUI will be used and the application will be run headless."),
            TRANS("You'll need to use other command-line options to connect to a stage... eventually there will be an OSC remote control interface."),
            nullptr
        });



        if (arglist.removeOptionIfFound(versionSpec)) {
            std::cout << getApplicationName() << TRANS(" version ") << getApplicationVersion() << std::endl;
            doImmediateQuit = true;
        }

        if (arglist.containsOption("-h|--help")) {
            std::cout << TRANS("Usage: ") << getApplicationName() << " [options...]  [connect_URL]" << std::endl;

            printCommandList(app, arglist);
            doImmediateQuit = true;
        }

        if (doImmediateQuit) {
            return;
        }

        setupDefaultConnInfo();

        auto connserv = arglist.removeValueForOption(serverSpec);
        if (connserv.isNotEmpty()) {
            cmdlineConnInfo.serverHost =  connserv.upToFirstOccurrenceOf(":", false, true);
            String portpart = connserv.fromFirstOccurrenceOf(":", false, false);
            int port = portpart.getIntValue();
            if (port > 0) {
                cmdlineConnInfo.serverPort = port;
            } else {
                cmdlineConnInfo.serverPort = DEFAULT_SERVER_PORT;
            }
            copyInfo = true;
        }

        auto groupname = arglist.removeValueForOption(groupSpec);
        if (groupname.isNotEmpty()) {
            cmdlineConnInfo.groupName = groupname.trim();
            doInitialConnect = true;
            copyInfo = true;
        }

        auto grouppass = arglist.removeValueForOption(groupPassSpec);
        if (grouppass.isNotEmpty()) {
            cmdlineConnInfo.groupPassword = grouppass;
            copyInfo = true;
        }

        auto username = arglist.removeValueForOption(userNameSpec);
        if (username.isNotEmpty()) {
            cmdlineConnInfo.userName = username.trim();
            copyInfo = true;
        }

        auto setupfile = arglist.removeValueForOption(loadSetupSpec);
        if (setupfile.isNotEmpty()) {
            loadSetupFilename = setupfile;
        }


        if (arglist.removeOptionIfFound(headlessSpec)) {

            doHeadless = true;

            if (!doInitialConnect) {
                std::cout << TRANS("Error: you need to specify a stage to connect to for headless operation right now... eventually there will be an OSC interface.") << std::endl;
                doImmediateQuit = true;
            }
        }

        // what args remain? assume it's a URL
        if (arglist.arguments.size() > 0) {
            cmdlineArgUrl = arglist.arguments.getLast().text;
        }

    }

    //==============================================================================
    // JUCE_CATCH_UNHANDLED_EXCEPTIONS=1 routes message-thread exceptions here instead of std::terminate. Log +
    // breadcrumb, keep running: a paint() or event throw must not take a live session down.
    void unhandledException (const std::exception* e, const String& sourceFilename, int lineNumber) override
    {
        sbLogUnhandled("UNHANDLED", e ? e->what() : "(non-std exception)", sourceFilename.toRawUTF8(), lineNumber);
    }

    void initialise (const String&) override
    {
        sbMoveOldDataFolder();
#if defined(_WIN32) || JUCE_MAC
        // A GUI-subsystem exe has no console, and a GUI app launched from the Finder or `open` has no stderr, so
        // every std::cout/std::cerr line ([SB-*] diagnostics, hub handshake, crash breadcrumbs) would vanish. Append
        // them to sbplus-stdout.log in the settings folder (same folder as the .settings file). SB_DIAG=1 in
        // sb_taps.conf enables the [SB-RT]/[SB-AUDIO]/[SB-ADAPT] diagnostics.
        {
            File logf = appProperties.getStorageParameters().getDefaultFile().getSiblingFile("sbplus-stdout.log");
            logf.getParentDirectory().createDirectory();
            if (logf.existsAsFile() && logf.getSize() > 8 * 1024 * 1024) logf.moveFileTo(logf.getSiblingFile("sbplus-stdout.log.1"));
            // Not freopen_s: the _s variant opens exclusive (_SH_DENYRW), so a second call on the same path fails,
            // leaves stderr closed (fd -1), and the next std::cerr << fast-fails with c0000409/INVALID_ARG. Plain
            // freopen shares.
            const std::string lp = logf.getFullPathName().toStdString();
#ifdef _WIN32
            const char* devnull = "NUL";
#else
            const char* devnull = "/dev/null";
#endif
            if (!freopen(lp.c_str(), "a", stdout)) freopen(devnull, "w", stdout);
            if (!freopen(lp.c_str(), "a", stderr)) freopen(devnull, "w", stderr);
            setvbuf(stdout, nullptr, _IOLBF, 4096); setvbuf(stderr, nullptr, _IONBF, 0);
            std::cout << "=== SonoBus RP " << getApplicationVersion() << " start " << Time::getCurrentTime().toString(true, true) << " ===" << std::endl;
        }
#endif
        ::std::set_terminate(sbTerminateHandler);

        handleCommandLine();

        if (doImmediateQuit) {
            quit();
            return;
        };


       #if JUCE_LINUX
        // Without a display the window code has no screen to place itself on and crashes.
        if (!doHeadless && Desktop::getInstance().getDisplays().getPrimaryDisplay() == nullptr) {
            std::cerr << "SonoBus RP: no display found. Run it from a desktop session, or use --headless." << std::endl;
            quit();
            return;
        }
       #endif

        if (!doHeadless) {
            mainWindow.reset (createWindow());

#if JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE
            Desktop::getInstance().setKioskModeComponent (mainWindow.get(), false);
#endif

            mainWindow->setVisible (true);
           #if SONOBUSPLUS_BUILD
            // SIGUSR1 = clean quit without the "you are connected" confirmation: settings saved, crash sentinel
            // cleared, nothing shown to the user. The handler only sets a flag; the timer below acts on the message
            // thread.
#ifndef _WIN32
            signal(SIGUSR1, [](int){ sbPlusSigQuit.store(true); });
#endif   // No SIGUSR1 on Windows; the quit flag is simply never set there
#ifdef _WIN32
            mQuitWatch.quitFile = appProperties.getStorageParameters().getDefaultFile().getSiblingFile("QUIT");
            mQuitWatch.quitFile.deleteFile();   // stale from a previous run
#endif
            mQuitWatch.startTimer(250);
           #endif

            Desktop::getInstance().setScreenSaverEnabled(false);


            if (auto * sonoproc = dynamic_cast<SonobusAudioProcessor*>(mainWindow->pluginHolder->processor.get())) {
                if (sonoproc->hasEditor()) {
                    if (auto * sonoeditor = dynamic_cast<SonobusAudioProcessorEditor*>(sonoproc->createEditorIfNeeded())) {
                        sonoeditor->saveSettingsIfNeeded = [this]() {
                            mainWindow->pluginHolder->savePluginState();
                            mainWindow->pluginHolder->saveAudioDeviceState();
                            appProperties.saveIfNeeded();
                        };

                        // apply command line connection stuff

                        if (doInitialConnect) {
                            DBG("CONNECTING INITIAL");
                            sonoproc->setLaunchConnectRequested(true);   // -c/-g owns the launch; auto-reconnect-to-last stands down
                            sonoeditor->connectWithInfo(cmdlineConnInfo, false, false);
                        } else if (copyInfo) {
                            // only copy info
                            DBG("COPYING INITIAL");
                            sonoeditor->connectWithInfo(cmdlineConnInfo, false, true);
                        }
                        else if (cmdlineArgUrl.isNotEmpty()) {
#if JUCE_LINUX
                            // Linux only, as this is handled through other means on other platforms
                            // handle the last arg as a connect URL
                            sonoeditor->handleURL(cmdlineArgUrl);
#endif
                        }

                        if (loadSetupFilename.isNotEmpty()) {
                            File setupfile = File::getCurrentWorkingDirectory().getChildFile(loadSetupFilename);
                            if (!setupfile.exists()) {
                                // try the default location
                                String recentsfolder = mainWindow->pluginHolder->getLastRecentsFolder();
                                if (recentsfolder.isNotEmpty()) {
                                    setupfile = File(recentsfolder).getChildFile(setupfile.getFileName());
                                }
                            }
                            if (setupfile.exists()) {
                                Thread::sleep(200); // just in case
                                sonoeditor->loadSettingsFromFile(setupfile);
                            }
                            else {
                                std::cerr << "Settings file does not exist: " << loadSetupFilename << std::endl;
                            }
                        }
                    }
                }
            }

#if JUCE_ANDROID && JUCE_OPENGL
            attachGL();
#endif


#if JUCE_ANDROID
            startTimer(500);
#endif
        }
        else {
            // headless mode

            pluginHolder.reset (createHeadlessPlugin());
           #if SONOBUSPLUS_BUILD
            mHeadlessHealth.holder = pluginHolder.get(); mHeadlessHealth.startTimer(1000);   // Health reports in headless mode
           #endif

            if (auto * sonoproc = dynamic_cast<SonobusAudioProcessor*>(pluginHolder->processor.get())) {

                // apply command line connection stuff

                if (loadSetupFilename.isNotEmpty()) {
                    File setupfile = File::getCurrentWorkingDirectory().getChildFile(loadSetupFilename);
                    if (!setupfile.exists()) {
                        // try the default location
                        String recentsfolder = pluginHolder->getLastRecentsFolder();
                        if (recentsfolder.isNotEmpty()) {
                            setupfile = File(recentsfolder).getChildFile(setupfile.getFileName());
                        }
                    }
                    if (setupfile.exists()) {
                        Thread::sleep(200); // just in case

                        if (loadSettingsFromFile(setupfile)) {
                            std::cerr << "Loaded Settings file: " << setupfile.getFullPathName() << std::endl;
                        }
                        else {
                            std::cerr << "Error loading settings file: " << setupfile.getFullPathName() << std::endl;
                        }
                    }
                    else {
                        std::cerr << "Settings file does not exist: " << loadSetupFilename << std::endl;
                    }
                }


                if (doInitialConnect) {
                    DBG("CONNECTING HEADLESS INITIAL");
                    sonoproc->setLaunchConnectRequested(true);   // -c/-g owns the launch; auto-reconnect-to-last stands down
                    sonoproc->connectToServer(cmdlineConnInfo.serverHost, cmdlineConnInfo.serverPort, cmdlineConnInfo.userName, cmdlineConnInfo.userPassword);

                    // HACK FOR NOW - todo add listener
                    Thread::sleep(500);

                    cmdlineConnInfo.timestamp = Time::getCurrentTime().toMilliseconds();
                    sonoproc->addRecentServerConnectionInfo(cmdlineConnInfo);

                    sonoproc->setWatchPublicGroups(false);

                    sonoproc->joinServerGroup(cmdlineConnInfo.groupName, cmdlineConnInfo.groupPassword, cmdlineConnInfo.groupIsPublic);
                }
            }

        }


#if JUCE_MAC
        disableAppNap();
#endif

    }


    bool loadSettingsFromFile(const File & file)
    {
        SonobusAudioProcessor * processor = nullptr;
        AudioDeviceManager * deviceManager = nullptr;
        StandalonePluginHolder * plugHolder = nullptr;

        if (mainWindow != nullptr && mainWindow->pluginHolder != nullptr) {
            processor = dynamic_cast<SonobusAudioProcessor*>(mainWindow->pluginHolder->processor.get());
            deviceManager = &mainWindow->getDeviceManager();
            plugHolder = mainWindow->pluginHolder.get();
        }
        else if (pluginHolder != nullptr) {
            processor = dynamic_cast<SonobusAudioProcessor*>(pluginHolder->processor.get());
            deviceManager = &pluginHolder->deviceManager;
            plugHolder = pluginHolder.get();
        }

        if (processor == nullptr || deviceManager == nullptr) return false;

        bool retval = true;

        PropertiesFile::Options opts;
        PropertiesFile propfile = PropertiesFile(file, opts);

        if (!propfile.isValidFile()) {
            std::cerr << "Error while loading setup, could not read from the specified file!" << std::endl;

            return false;
        }

        MemoryBlock data;

        if (propfile.containsKey("filterStateXML")) {
            String filtxml = propfile.getValue ("filterStateXML");
            if (filtxml.isEmpty()) {   // The key is saved as an XML child
                if (auto fx = propfile.getXmlValue ("filterStateXML")) filtxml = fx->toString();
            }
            data.replaceAll(filtxml.toUTF8(), filtxml.getNumBytesAsUTF8());
            if (data.getSize() > 0) {
                processor->setStateInformationWithOptions (data.getData(), (int) data.getSize(), false, true, true);
            }
            else {
                DBG("Empty XML filterstate");
                retval = false;
            }
        }
        else {
            if (data.fromBase64Encoding (propfile.getValue ("filterState")) && data.getSize() > 0) {
                processor->setStateInformationWithOptions (data.getData(), (int) data.getSize(), false, true);
            } else {
                retval = false;
            }
        }

        auto savedAudioState = propfile.getXmlValue ("audioSetup");

        if (savedAudioState.get()) {
            std::unique_ptr<AudioDeviceManager::AudioDeviceSetup> prefSetupOptions;
            String preferredDefaultDeviceName;

            // now remove samplerate from saved state if necessary
            // as well as preferredSetupOptions

            if (!((bool)plugHolder->getShouldOverrideSampleRateValue().getValue())) {
                DBG("NOT OVERRIDING SAMPLERATE");
                if (savedAudioState && savedAudioState->hasAttribute("audioDeviceRate")) {
                    savedAudioState->removeAttribute("audioDeviceRate");
                }
                if (prefSetupOptions) {
                    prefSetupOptions->sampleRate = 0;
                }
            }



            auto totalInChannels  = processor->getMainBusNumInputChannels();
            auto totalOutChannels = processor->getMainBusNumOutputChannels();

            deviceManager->initialise (totalInChannels,
                                       totalOutChannels,
                                       savedAudioState.get(),
                                       true,
                                       preferredDefaultDeviceName,
                                       prefSetupOptions.get());
        }

        if (!retval) {
            std::cerr << "Error while loading setup, invalid setup!" << std::endl;
        }

        return retval;
    }


    void timerCallback() override
    {
#if JUCE_ANDROID
        DBG("setting foreground service active");
        setAndroidForegroundServiceActive(true);
        stopTimer();
#endif
    }

    void shutdown() override
    {
        //DBG("shutdown");
        if (mainWindow.get() != nullptr) {
            mainWindow->pluginHolder->savePluginState();
            mainWindow->pluginHolder->saveAudioDeviceState();
        }

#if JUCE_ANDROID
        setAndroidForegroundServiceActive(false);
  #if JUCE_OPENGL
        detachGL();
  #endif
#endif

        mainWindow = nullptr;

        pluginHolder = nullptr;

        appProperties.saveIfNeeded();

    }
    
    void urlOpened(const URL & url) override {
        DBG("Url opened: " << url.toString(true));
        
        if (mainWindow.get() != nullptr) {
            
            if (auto * sonoproc = dynamic_cast<SonobusAudioProcessor*>(mainWindow->pluginHolder->processor.get())) {
                if (sonoproc->hasEditor()) {
                    if (auto * sonoeditor = dynamic_cast<SonobusAudioProcessorEditor*>(sonoproc->createEditorIfNeeded())) {
                        sonoeditor->handleURL(url.toString(true));
                        mainWindow->toFront(true);
                    }
                }
            }
        }        
    }
    
    void anotherInstanceStarted (const String& url) override    {
        
        DBG("Url handled from another instance: " << url);
        
        if (mainWindow.get() != nullptr && url.trim().isEmpty()) { mainWindow->toFront(true); return; }   // Plain second launch
        if (mainWindow.get() != nullptr) {
            
            if (auto * sonoproc = dynamic_cast<SonobusAudioProcessor*>(mainWindow->pluginHolder->processor.get())) {
                if (sonoproc->hasEditor()) {
                    if (auto * sonoeditor = dynamic_cast<SonobusAudioProcessorEditor*>(sonoproc->createEditorIfNeeded())) {
                        sonoeditor->handleURL(url);
                        mainWindow->toFront(true);
                    }
                }
            }
        }
    }
        
    void suspended() override
    {
        DBG("suspended");
        if (mainWindow.get() != nullptr) {
            mainWindow->pluginHolder->savePluginState();
            mainWindow->pluginHolder->saveAudioDeviceState();

            if (auto * sonoproc = dynamic_cast<SonobusAudioProcessor*>(mainWindow->pluginHolder->processor.get())) {
                if (sonoproc->getNumberRemotePeers() == 0 && !mainWindow->pluginHolder->isInterAppAudioConnected()) {
                    // shutdown audio engine
                    DBG("no connections shutting down audio");
                    mainWindow->getDeviceManager().closeAudioDevice();
                    sonoproc->setPlayHead (nullptr);
#if JUCE_ANDROID
                    setAndroidForegroundServiceActive(false);
#endif
                }
                else {
#if JUCE_ANDROID
                    setAndroidForegroundServiceActive(true);
#endif
                }
            }

        }

        appProperties.saveIfNeeded();

        auto props = appProperties.getUserSettings()->getAllProperties();
        for (auto & prop : props.getAllKeys()) {
            DBG("save state key: " << prop );
        }
        
        Desktop::getInstance().setScreenSaverEnabled(true);        
    }

    void setAndroidForegroundServiceActive(bool flag)
    {
    #if JUCE_ANDROID
        LocalRef<jobject> activity (getMainActivity());

        if (activity != nullptr) {
            getEnv()->CallVoidMethod(activity.get(), SonoBusActivity.setForegroundServiceActive, flag);
        }
    #endif
    }


    void resumed() override
    {
        Desktop::getInstance().setScreenSaverEnabled(false);
        
#if JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE
        Desktop::getInstance().setKioskModeComponent (nullptr, false);
        Desktop::getInstance().setKioskModeComponent (mainWindow.get(), false);
#endif

        if (auto * dev = mainWindow->getDeviceManager().getCurrentAudioDevice()) {
            if (!dev->isPlaying()) {
                DBG("dev not playing, restarting");
                mainWindow->getDeviceManager().restartLastAudioDevice();
            }
        }
        else {
            DBG("was not active: restarting");
            mainWindow->getDeviceManager().restartLastAudioDevice();
        }

#if JUCE_ANDROID
        setAndroidForegroundServiceActive(true);
#endif

    }
    
    //==============================================================================
    void systemRequestedQuit() override
    {
        DBG("Requested quit");
        if (mainWindow.get() != nullptr) {
            mainWindow->pluginHolder->savePluginState();
            mainWindow->pluginHolder->saveAudioDeviceState();
        }

        appProperties.saveIfNeeded();

        
        if (mainWindow.get() != nullptr) {
            if (auto * editor = mainWindow->getEditor()) {
                if (auto * sonoeditor = dynamic_cast<SonobusAudioProcessorEditor*>(editor)) {
                    if (!sonoeditor->requestedQuit()) {
                        // they'll handle it
                        return;
                    }
                }
            }
        }
        
        if (ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            Timer::callAfterDelay (100, []()
            {
                if (auto app = JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

    void memoryWarningReceived()  override
    {
        DBG("Memory warning");
    }

    void attachGL()
    {
        if (mainWindow) {
#if JUCE_OPENGL  && JUCE_ANDROID
            DBG("activating GL context");
            openGLContext.makeActive();
            DBG("OPEN GL attaching to " << (long) mainWindow.get());
            openGLContext.attachTo (*mainWindow);
#endif
        }
    }

    void detachGL()
    {

    #if JUCE_OPENGL && JUCE_ANDROID
        DBG("About to detach opengl context");
        if (openGLContext.isAttached()) {
            openGLContext.detach();
            DBG("About to deactivate opengl context");
            openGLContext.deactivateCurrentContext();
            DBG("done detach opengl context");
        }
    #endif
    }

protected:

#if JUCE_OPENGL
    OpenGLContext openGLContext;
#endif

    ApplicationProperties appProperties;
    std::unique_ptr<StandaloneFilterWindow> mainWindow;

    // used only in headless mode
    std::unique_ptr<StandalonePluginHolder> pluginHolder;

};

} // namespace juce

#if JucePlugin_Build_Standalone && JUCE_IOS

using namespace juce;

bool JUCE_CALLTYPE juce_isInterAppAudioConnected()
{
    if (auto holder = StandalonePluginHolder::getInstance())
        return holder->isInterAppAudioConnected();

    return false;
}

void JUCE_CALLTYPE juce_switchToHostApplication()
{
    if (auto holder = StandalonePluginHolder::getInstance())
        holder->switchToHostApplication();
}

#if JUCE_MODULE_AVAILABLE_juce_gui_basics
Image JUCE_CALLTYPE juce_getIAAHostIcon (int size)
{
    if (auto holder = StandalonePluginHolder::getInstance())
        return holder->getIAAHostIcon (size);

    return Image();
}
#endif
#endif

START_JUCE_APPLICATION (SonobusStandaloneFilterApp);

//#endif
