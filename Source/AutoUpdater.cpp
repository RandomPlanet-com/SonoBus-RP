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



#include "JuceHeader.h"
#include "SbDataPaths.h"
//#include "jucer_Application.h"
#include "AutoUpdater.h"
#if ! JUCE_WINDOWS
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstdio>
#include <atomic>
#if SONOBUSPLUS_BUILD
extern std::atomic<bool> sbPlusQuitWithoutConfirm;   // Defined in SonobusPluginEditor.cpp (SIGUSR1 quit path)
#endif

//==============================================================================
LatestVersionCheckerAndUpdater::LatestVersionCheckerAndUpdater()
    : Thread ("VersionChecker")
{
}

LatestVersionCheckerAndUpdater::~LatestVersionCheckerAndUpdater()
{
    stopThread (1000);
    clearSingletonInstance();
}

void LatestVersionCheckerAndUpdater::checkForNewVersion (bool showAlerts)
{
    // SonoBus RP the check goes to the project's update server (see VersionInfo::fetch), never to upstream.
    if (! isThreadRunning())
    {
        showAlertWindows = showAlerts;
        startThread (Thread::Priority::low);
    }
}

//==============================================================================
void LatestVersionCheckerAndUpdater::run()
{
    auto info = VersionInfo::fetchLatestFromUpdateServer();

    if (info == nullptr)
    {
        if (showAlertWindows)
            AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon,
                                              "Update Server Communication Error",
                                             #if SONOBUSPLUS_BUILD
                                              "Could not reach the SonoBus RP update server (randomplanet.com).\n"
                                              "Please try again in a few minutes.");
                                             #else
                                              "Failed to communicate with the SonoBus update server.\n"
                                              "Please try again in a few minutes.\n\n"
                                              "If this problem persists you can download the latest version of SonoBus from sonobus.net");
                                             #endif

        return;
    }

    if (! info->isNewerVersionThanCurrent())
    {
        if (showAlertWindows)
            AlertWindow::showMessageBoxAsync (AlertWindow::InfoIcon,
                                              "No New Version Available",
                                             #if SONOBUSPLUS_BUILD
                                              "Your SonoBus RP version is up to date.");
                                             #else
                                              "Your SonoBus version is up to date.");
                                             #endif
        return;
    }

    auto osString = []
    {
       #if JUCE_MAC
        return "mac";
       #elif JUCE_WINDOWS
        return "win";
       #elif JUCE_LINUX
        return "linux";
       #else
        jassertfalse;
        return "Unknown";
       #endif
    }();

    String requiredFilename ("sonobus-" + info->versionString + "-" + osString + "."); // to end with any extension

    // The appcast may carry two assets for a release: the DMG (first installs, fallback) and the notarized zip twin
    // (sonobus-<stamp>-mac.zip, with sha256 + size) that the app can install over itself. The DMG stays first because
    // older clients take the first match.
    VersionInfo::Asset dmgAsset { {}, {} }, zipAsset { {}, {} };
    for (auto& asset : info->assets)
    {
        if (! asset.name.startsWith (requiredFilename)) continue;
        if (asset.name.endsWithIgnoreCase (".zip")) { if (zipAsset.name.isEmpty()) zipAsset = asset; }
        else if (dmgAsset.name.isEmpty()) dmgAsset = asset;
    }
    if (dmgAsset.name.isNotEmpty() || zipAsset.name.isNotEmpty())
    {
        auto versionString = info->versionString;
        auto releaseNotes  = info->releaseNotes;
        if (dmgAsset.name.isEmpty()) dmgAsset = zipAsset;   // zip-only appcast: the download path hands out the zip

        MessageManager::callAsync ([this, versionString, releaseNotes, dmgAsset, zipAsset]
        {
            askUserAboutNewVersion (versionString, releaseNotes, dmgAsset, zipAsset);
        });

        return;
    }

    if (showAlertWindows)
        AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon,
                                          "Failed to find any new downloads",
                                          "Please try again in a few minutes.");
}

//==============================================================================
class UpdateDialog  : public Component
{
public:
    UpdateDialog (const String& newVersion, const String& releaseNotes, bool canInstallInPlace = false)
    {
        titleLabel.setText ("SonoBus version " + newVersion, dontSendNotification);
        titleLabel.setFont ({ 15.0f, Font::bold });
        titleLabel.setJustificationType (Justification::centred);
        addAndMakeVisible (titleLabel);

       #if SONOBUSPLUS_BUILD
        // "Install and relaunch" replaces this app in place (notarized zip, verified) and restarts it.
        if (canInstallInPlace) {
            contentLabel.setText ("A new version of SonoBus RP is available. Install and relaunch replaces this app in place "
                                  "(you will be disconnected for a few seconds).", dontSendNotification);
            addAndMakeVisible (installButton);
            installButton.onClick = [this] { exitModalStateWithResult (2); };
        } else
        contentLabel.setText ("A new version of SonoBus RP is available - would you like to download it?", dontSendNotification);
       #else
        contentLabel.setText ("A new version of SonoBus is available - would you like to download it?", dontSendNotification);
       #endif
        contentLabel.setFont (15.0f);
        contentLabel.setJustificationType (Justification::topLeft);
        addAndMakeVisible (contentLabel);

        releaseNotesLabel.setText ("Release notes:", dontSendNotification);
        releaseNotesLabel.setFont (15.0f);
        releaseNotesLabel.setJustificationType (Justification::topLeft);
        addAndMakeVisible (releaseNotesLabel);

        releaseNotesEditor.setMultiLine (true);
        releaseNotesEditor.setReadOnly (true);
        releaseNotesEditor.setText (releaseNotes);
        addAndMakeVisible (releaseNotesEditor);

        addAndMakeVisible (chooseButton);
        chooseButton.onClick = [this] { exitModalStateWithResult (1); };

        addAndMakeVisible (cancelButton);
        cancelButton.onClick = [this]
        {
            //if (dontAskAgainButton.getToggleState())
            //    getGlobalProperties().setValue (Ids::dontQueryForUpdate.toString(), 1);
            //else
            //    getGlobalProperties().removeValue (Ids::dontQueryForUpdate);

            exitModalStateWithResult (-1);
        };

        //dontAskAgainButton.setToggleState (getGlobalProperties().getValue (Ids::dontQueryForUpdate, {}).isNotEmpty(), dontSendNotification);
        //addAndMakeVisible (dontAskAgainButton);

        appIcon = Drawable::createFromImageData (BinaryData::sonobus_logo_96_png,
                                                  BinaryData::sonobus_logo_96_pngSize);
        lookAndFeelChanged();

        setSize (500, 300);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10);

        auto topSlice = b.removeFromTop (appIconBounds.getHeight())
                         .withTrimmedLeft (appIconBounds.getWidth());

        titleLabel.setBounds (topSlice.removeFromTop (25));
        topSlice.removeFromTop (5);
        contentLabel.setBounds (topSlice.removeFromTop (25));

        auto buttonBounds = b.removeFromBottom (40); // 60
        //buttonBounds.removeFromBottom (8); // 25

        if (installButton.isVisible()) {   // Three buttons
            auto third = buttonBounds.getWidth() / 3;
            installButton.setBounds (buttonBounds.removeFromLeft (third).reduced (10, 0));
            chooseButton.setBounds (buttonBounds.removeFromLeft (third).reduced (10, 0));
            cancelButton.setBounds (buttonBounds.reduced (10, 0));
        } else {
        chooseButton.setBounds (buttonBounds.removeFromLeft (buttonBounds.getWidth() / 2).reduced (20, 0));
        cancelButton.setBounds (buttonBounds.reduced (20, 0));
        }
        //dontAskAgainButton.setBounds (cancelButton.getBounds().withY (cancelButton.getBottom() + 5).withHeight (20));

        releaseNotesEditor.setBounds (b.reduced (0, 10));
    }

    void paint (Graphics& g) override
    {
        
        g.fillAll (Colour(0xff222222));

        if (appIcon != nullptr)
            appIcon->drawWithin (g, appIconBounds.toFloat(),
                                  RectanglePlacement::stretchToFit, 1.0f);
    }

    static std::unique_ptr<DialogWindow> launchDialog (const String& newVersionString,
                                                       const String& releaseNotes, bool canInstallInPlace = false)
    {
        DialogWindow::LaunchOptions options;

       #if SONOBUSPLUS_BUILD
        options.dialogTitle = "Download SonoBus RP " + newVersionString + "?";
       #else
        options.dialogTitle = "Download SonoBus version " + newVersionString + "?";
       #endif
        options.resizable = false;

        auto* content = new UpdateDialog (newVersionString, releaseNotes, canInstallInPlace);
        options.content.set (content, true);

        std::unique_ptr<DialogWindow> dialog (options.create());

        content->setParentWindow (dialog.get());
        dialog->enterModalState (true, nullptr, true);

        return dialog;
    }

private:
    void lookAndFeelChanged() override
    {
        //cancelButton.setColour (TextButton::buttonColourId, findColour (secondaryButtonBackgroundColourId));
        releaseNotesEditor.applyFontToAllText (releaseNotesEditor.getFont());
    }

    void setParentWindow (DialogWindow* parent)
    {
        parentWindow = parent;
    }

    void exitModalStateWithResult (int result)
    {
        if (parentWindow != nullptr)
            parentWindow->exitModalState (result);
    }

    Label titleLabel, contentLabel, releaseNotesLabel;
    TextEditor releaseNotesEditor;
    TextButton chooseButton { TRANS("Download Installer") }, cancelButton { TRANS("Cancel") };
    TextButton installButton { TRANS("Install and relaunch") };   
    ToggleButton dontAskAgainButton { "Don't ask again" };
    std::unique_ptr<Drawable> appIcon;
    Rectangle<int> appIconBounds { 10, 10, 64, 64 };

    DialogWindow* parentWindow = nullptr;
};

void LatestVersionCheckerAndUpdater::askUserForLocationToDownload (const VersionInfo::Asset& asset)
{
    File dloadLoc = File::getSpecialLocation(File::userHomeDirectory).getChildFile("Downloads");
    if (!dloadLoc.exists()) {
        dloadLoc = File::getSpecialLocation(File::userDesktopDirectory);
    }

    // only ask if Downloads or desktop doesn't exist
    if (!dloadLoc.exists())
    {
        FileChooser chooser ("Please select the location into which you would like to download the new version",
                             //{ getAppSettings().getStoredPath (Ids::jucePath, TargetOS::getThisOS()).get() });
                             { dloadLoc });

#if JUCE_MODAL_LOOPS_PERMITTED
        if (chooser.browseForDirectory())
#endif
        {


            dloadLoc = chooser.getResult();
        }

    }

    downloadAndInstall (asset, dloadLoc);

}

// Can THIS running copy be replaced in place? macOS .app bundle, owned folder we
// can write (a user folder, /Applications when the user owns it), not app-translocated, not running off a mounted DMG.
static bool sbInPlaceUpdatePossible (File& bundleOut)
{
   #if JUCE_MAC && SONOBUSPLUS_BUILD
    File app = File::getSpecialLocation (File::currentApplicationFile);
    if (! app.isDirectory() || ! app.hasFileExtension (".app")) return false;
    const String path = app.getFullPathName();
    if (path.startsWith ("/private/var/folders") || path.startsWith ("/Volumes/")) return false;   // translocated / DMG
    if (! app.getParentDirectory().hasWriteAccess() || ! app.hasWriteAccess()) return false;
    bundleOut = app;
    return true;
   #else
    ignoreUnused (bundleOut); return false;
   #endif
}

void LatestVersionCheckerAndUpdater::askUserAboutNewVersion (const String& newVersionString,
                                                             const String& releaseNotes,
                                                             const VersionInfo::Asset& asset,
                                                             const VersionInfo::Asset& zipAsset)
{
    File bundle;
    const bool canInstall = zipAsset.name.isNotEmpty() && sbInPlaceUpdatePossible (bundle);
    // SB_UPDATE_AUTO_INSTALL=1 (env/taps, TEST + unattended hosts): skip the dialog and install in place straight away.
    if (canInstall) {
        bool autoInstall = false;
        if (const char* e = std::getenv ("SB_UPDATE_AUTO_INSTALL"); e && e[0] == '1') autoInstall = true;
        String tapsPath;
        if (const char* f = std::getenv ("SB_TAPS_FILE"); f && f[0]) tapsPath = f;
        else tapsPath = sbTapsPath();
        StringArray lines; if (tapsPath.isNotEmpty()) File (tapsPath).readLines (lines);
        for (auto& l : lines) if (l.startsWith ("SB_UPDATE_AUTO_INSTALL=1")) autoInstall = true;
        if (autoInstall) { fprintf (stderr, "[SB-UPDATE] auto-install %s\n", newVersionString.toRawUTF8()); installInPlace (zipAsset); return; }
    }
    dialogWindow = UpdateDialog::launchDialog (newVersionString, releaseNotes, canInstall);

    if (auto* mm = ModalComponentManager::getInstance())
        mm->attachCallback (dialogWindow.get(),
                            ModalCallbackFunction::create ([this, asset, zipAsset] (int result)
                                                           {
                                                               if (result == 1)
                                                                    askUserForLocationToDownload (asset);
                                                               else if (result == 2)
                                                                    installInPlace (zipAsset);   

                                                                dialogWindow.reset();
                                                            }));
}

//==============================================================================
class DownloadAndInstallThread   : private ThreadWithProgressWindow
{
public:
    DownloadAndInstallThread  (const VersionInfo::Asset& a, const File& t, std::function<void()>&& cb)
        : ThreadWithProgressWindow ("Downloading New Version", true, true),
          asset (a), targetFolder (t), completionCallback (std::move (cb))
    {
        launchThread (Thread::Priority::low);
    }

private:
    
    File dloadfile;
    
#if 1
    
    void run() override
    {
        setProgress (-1.0);

        //MemoryBlock zipData;
        auto result = download ();

        if (result.wasOk() && ! threadShouldExit())
            result = install ();

        if (result.failed())
            MessageManager::callAsync ([result] { AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon,
                                                                                    "Installation Failed",
                                                                                    result.getErrorMessage()); });
        else
            MessageManager::callAsync (completionCallback);
    }
    
    Result download ()
    {
        setStatusMessage ("Downloading...");

        int statusCode = 0;
        auto inStream = VersionInfo::createInputStreamForAsset (asset, statusCode);

        if (inStream != nullptr && statusCode == 200)
        {
            int64 total = 0;

            struct ScopedDownloadFile
            {
                ScopedDownloadFile (const File& dloadfile)
                {
                    file = dloadfile.getSiblingFile (dloadfile.getFileNameWithoutExtension() + "_download").getNonexistentSibling();
                    //jassert (folder.createDirectory());
                }

                ~ScopedDownloadFile()   { file.deleteFile(); }

                File file;
            };
            
            DBG("asset name: " << asset.name << "  url: " << asset.url);
            dloadfile = targetFolder.getChildFile(asset.name).getNonexistentSibling();

            ScopedDownloadFile sdload(dloadfile);

            {
                FileOutputStream fo (sdload.file);
                
                if (fo.openedOk())
                {
                    fo.setPosition (0);
                    fo.truncate();
                } else {
                    return Result::fail ("Failed to write download from: " + asset.url);
                }
                
                for (;;)
                {
                    if (threadShouldExit())
                        return Result::fail ("Cancelled");
                    
                    auto written = fo.writeFromInputStream (*inStream, 8192);
                    
                    if (written == 0)
                        break;
                    
                    total += written;
                    
                    setStatusMessage ("Downloading... " + File::descriptionOfSizeInBytes (total));
                }

            }                

            // completed, now move to final name
            sdload.file.moveFileTo(dloadfile);
            
            return Result::ok();
        }

        return Result::fail ("Failed to download from: " + asset.url);
    }

    Result install ()
    {
        //setStatusMessage ("Installing...");

        // "show file"
        File dfile(dloadfile);
        MessageManager::callAsync ([dfile] { dfile.revealToUser(); });
        

        return Result::ok();
    }
    
#else
    
    void run() override
    {
        setProgress (-1.0);

        MemoryBlock zipData;
        auto result = download (zipData);

        if (result.wasOk() && ! threadShouldExit())
            result = install (zipData);

        if (result.failed())
            MessageManager::callAsync ([result] { AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon,
                                                                                    "Installation Failed",
                                                                                    result.getErrorMessage()); });
        else
            MessageManager::callAsync (completionCallback);
    }
    
    Result download (MemoryBlock& dest)
    {
        setStatusMessage ("Downloading...");

        int statusCode = 0;
        auto inStream = VersionInfo::createInputStreamForAsset (asset, statusCode);

        if (inStream != nullptr && statusCode == 200)
        {
            int64 total = 0;
            MemoryOutputStream mo (dest, true);

            for (;;)
            {
                if (threadShouldExit())
                    return Result::fail ("Cancelled");

                auto written = mo.writeFromInputStream (*inStream, 8192);

                if (written == 0)
                    break;

                total += written;

                setStatusMessage ("Downloading... " + File::descriptionOfSizeInBytes (total));
            }

            return Result::ok();
        }

        return Result::fail ("Failed to download from: " + asset.url);
    }

    Result install (const MemoryBlock& data)
    {
        setStatusMessage ("Installing...");

        MemoryInputStream input (data, false);
        ZipFile zip (input);

        if (zip.getNumEntries() == 0)
            return Result::fail ("The downloaded file was not a valid SonoBus release file!");

        struct ScopedDownloadFolder
        {
            ScopedDownloadFolder (const File& installTargetFolder)
            {
                folder = installTargetFolder.getSiblingFile (installTargetFolder.getFileNameWithoutExtension() + "_download").getNonexistentSibling();
                jassert (folder.createDirectory());
            }

            ~ScopedDownloadFolder()   { folder.deleteRecursively(); }

            File folder;
        };

        ScopedDownloadFolder unzipTarget (targetFolder);

        if (! unzipTarget.folder.isDirectory())
            return Result::fail ("Couldn't create a temporary folder to unzip the new version!");

        auto r = zip.uncompressTo (unzipTarget.folder);

        if (r.failed())
            return r;

        if (threadShouldExit())
            return Result::fail ("Cancelled");

       #if JUCE_LINUX || JUCE_MAC
        r = setFilePermissions (unzipTarget.folder, zip);

        if (r.failed())
            return r;

        if (threadShouldExit())
            return Result::fail ("Cancelled");
       #endif

        if (targetFolder.exists())
        {
            auto oldFolder = targetFolder.getSiblingFile (targetFolder.getFileNameWithoutExtension() + "_old").getNonexistentSibling();

            if (! targetFolder.moveFileTo (oldFolder))
                return Result::fail ("Could not remove the existing folder!\n\n"
                                     "This may happen if you are trying to download into a directory that requires administrator privileges to modify.\n"
                                     "Please select a folder that is writable by the current user.");
        }

        if (! unzipTarget.folder.getChildFile ("SonoBus").moveFileTo (targetFolder))
            return Result::fail ("Could not overwrite the existing folder!\n\n"
                                 "This may happen if you are trying to download into a directory that requires administrator privileges to modify.\n"
                                 "Please select a folder that is writable by the current user.");

        return Result::ok();
    }
#endif
    
    Result setFilePermissions (const File& root, const ZipFile& zip)
    {
        constexpr uint32 executableFlag = (1 << 22);

        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            auto* entry = zip.getEntry (i);

            if ((entry->externalFileAttributes & executableFlag) != 0 && entry->filename.getLastCharacter() != '/')
            {
                auto exeFile = root.getChildFile (entry->filename);

                if (! exeFile.exists())
                    return Result::fail ("Failed to find executable file when setting permissions " + exeFile.getFileName());

                if (! exeFile.setExecutePermission (true))
                    return Result::fail ("Failed to set executable file permission for " + exeFile.getFileName());
            }
        }

        return Result::ok();
    }

    VersionInfo::Asset asset;
    File targetFolder;
    std::function<void()> completionCallback;
};

void restartProcess (const File& targetFolder)
{
#if JUCE_MAC || JUCE_LINUX || JUCE_WINDOWS

   #if JUCE_MAC || JUCE_LINUX
    #if JUCE_MAC
     auto newProcess = targetFolder.getChildFile ("SonoBus.app").getChildFile ("Contents").getChildFile ("MacOS").getChildFile ("SonoBus");
    #elif JUCE_LINUX
     auto newProcess = targetFolder.getChildFile ("SonoBus");
    #endif

    StringArray command ("/bin/sh", "-c", "while killall -0 SonoBus; do sleep 5; done; " + newProcess.getFullPathName().quoted());
   #elif JUCE_WINDOWS
    auto newProcess = targetFolder.getChildFile ("SonoBus.exe");

    auto command = "cmd.exe /c\"@echo off & for /l %a in (0) do ( tasklist | find \"SonoBus\" >nul & ( if errorlevel 1 ( "
                    + targetFolder.getChildFile ("SonoBus.exe").getFullPathName().quoted() + " & exit /b ) else ( timeout /t 10 >nul ) ) )\"";
   #endif

    if (newProcess.existsAsFile())
    {
        ChildProcess restartProcess;
        restartProcess.start (command, 0);

        JUCEApplicationBase::getInstance()->systemRequestedQuit();
        //StandaloneFilterApp::getApp().systemRequestedQuit();
    }
#endif
}

void LatestVersionCheckerAndUpdater::downloadAndInstall (const VersionInfo::Asset& asset, const File& targetFolder)
{
    installer.reset (new DownloadAndInstallThread (asset, targetFolder,
                                                   [this, targetFolder]
                                                   {
                                                       installer.reset();
                                                       //restartProcess (targetFolder);
                                                   }));
}

//==============================================================================
//
// Download the notarized zip next to the bundle (same volume, so the rename is atomic), verify size + sha256 from the
// appcast, unpack with ditto, and verify the staged app before touching anything (codesign --verify --deep --strict,
// Developer ID team, spctl notarization assessment, same bundle id, newer CFBundleVersion stamp). Swap keeping one
// "<name>.app.previous", then relaunch via `open -n` once this process has exited. Settings live in Application
// Support and are untouched. Any failure leaves the running bundle alone.
class InPlaceUpdateThread : public ThreadWithProgressWindow
{
public:
    InPlaceUpdateThread (const VersionInfo::Asset& a, std::function<void (Result)>&& done)
        : ThreadWithProgressWindow ("Installing SonoBus RP update", true, true), asset (a), completion (std::move (done))
    {
        launchThread (Thread::Priority::low);
    }

    void run() override
    {
        setProgress (-1.0);
        auto r = doUpdate();
        MessageManager::callAsync ([this, r] { completion (r); });
    }

private:
    static String runTool (const StringArray& cmd, int& exitCode, int timeoutMs = 120000)
    {
        ChildProcess cp;
        if (! cp.start (cmd, ChildProcess::wantStdOut | ChildProcess::wantStdErr)) { exitCode = -1; return "could not start " + cmd[0]; }
        String out = cp.readAllProcessOutput();
        cp.waitForProcessToFinish (timeoutMs);
        exitCode = (int) cp.getExitCode();
        return out;
    }

    Result doUpdate()
    {
       #if JUCE_MAC
        File current;
        if (! sbInPlaceUpdatePossible (current)) return Result::fail ("This copy cannot be updated in place. Use Download Installer.");
        const File parent = current.getParentDirectory();
        const File work   = parent.getChildFile (".sbrp-update");
        work.deleteRecursively(); work.createDirectory();
        if (! work.isDirectory()) return Result::fail ("Could not create " + work.getFullPathName());
        struct Cleanup { File w; ~Cleanup() { w.deleteRecursively(); } } cleanup { work };

        // 1. download
        setStatusMessage ("Downloading " + asset.name + "...");
        int statusCode = 0;
        auto in = VersionInfo::createInputStreamForAsset (asset, statusCode);
        if (in == nullptr || statusCode != 200) return Result::fail ("Download failed (" + String (statusCode) + "): " + asset.url);
        const File zip = work.getChildFile (asset.name);
        {
            FileOutputStream fo (zip);
            if (! fo.openedOk()) return Result::fail ("Could not write " + zip.getFullPathName());
            int64 total = 0;
            for (;;) {
                if (threadShouldExit()) return Result::fail ("Cancelled");
                auto n = fo.writeFromInputStream (*in, 65536);
                if (n == 0) break;
                total += n;
                setStatusMessage ("Downloading... " + File::descriptionOfSizeInBytes (total));
            }
        }
        if (asset.size > 0 && zip.getSize() != asset.size)
            return Result::fail ("Download size mismatch (" + String (zip.getSize()) + " vs " + String (asset.size) + ")");
        if (asset.sha256.isNotEmpty())
        {
            setStatusMessage ("Verifying download...");
            const String got = SHA256 (zip).toHexString().toLowerCase();
            if (got != asset.sha256.trim().toLowerCase()) return Result::fail ("Download checksum mismatch");
        }

        // 2. unpack
        setStatusMessage ("Unpacking...");
        const File stage = work.getChildFile ("stage"); stage.createDirectory();
        int rc = 0;
        runTool ({ "/usr/bin/ditto", "-x", "-k", zip.getFullPathName(), stage.getFullPathName() }, rc);
        if (rc != 0) return Result::fail ("Unpack failed");
        File staged;
        for (const auto& e : stage.findChildFiles (File::findDirectories, false)) if (e.hasFileExtension (".app")) { staged = e; break; }
        if (staged == File()) return Result::fail ("No application bundle in the download");

        // 3. verify before touching anything
        setStatusMessage ("Verifying signature...");
        runTool ({ "/usr/bin/codesign", "--verify", "--deep", "--strict", staged.getFullPathName() }, rc);
        if (rc != 0) return Result::fail ("Signature check failed");
        String info = runTool ({ "/usr/bin/codesign", "-dv", "--verbose=2", staged.getFullPathName() }, rc);
        if (! info.contains ("TeamIdentifier=NJXTYJNUF5")) return Result::fail ("Not signed by Random Planet (team mismatch)");
        // SB_UPDATE_ALLOW_UNNOTARIZED=1 (env/taps, test only): dev builds are Developer-ID signed but not notarized.
        // The signature + team checks above still apply; only the notarization assessment is skipped.
        bool allowUnnotarized = false;
        {
            if (const char* e = std::getenv ("SB_UPDATE_ALLOW_UNNOTARIZED"); e && e[0] == '1') allowUnnotarized = true;
            String tapsPath;
            if (const char* f = std::getenv ("SB_TAPS_FILE"); f && f[0]) tapsPath = f;
            else tapsPath = sbTapsPath();
            StringArray lines; if (tapsPath.isNotEmpty()) File (tapsPath).readLines (lines);
            for (auto& l : lines) if (l.startsWith ("SB_UPDATE_ALLOW_UNNOTARIZED=1")) allowUnnotarized = true;
        }
        String spctl = runTool ({ "/usr/sbin/spctl", "-a", "-t", "exec", "-vv", staged.getFullPathName() }, rc);
        if ((rc != 0 || ! spctl.contains ("accepted")) && ! allowUnnotarized) return Result::fail ("Notarization check failed:\n" + spctl.trim());
        auto plistOf = [] (const File& app) { return parseXML (app.getChildFile ("Contents").getChildFile ("Info.plist")); };
        auto plistValue = [] (const XmlElement* plist, const String& key) -> String {
            if (plist == nullptr) return {};
            if (auto* dict = plist->getChildByName ("dict"))
                for (auto* k = dict->getFirstChildElement(); k != nullptr; k = k->getNextElement())
                    if (k->hasTagName ("key") && k->getAllSubText() == key)
                        if (auto* v = k->getNextElement()) return v->getAllSubText();
            return {};
        };
        auto newPlist = plistOf (staged), curPlist = plistOf (current);
        const String newId = plistValue (newPlist.get(), "CFBundleIdentifier"), curId = plistValue (curPlist.get(), "CFBundleIdentifier");
        if (newId.isEmpty() || newId != curId) return Result::fail ("Bundle identifier mismatch (" + newId + " vs " + curId + ")");
        const String newVer = plistValue (newPlist.get(), "CFBundleVersion"), curVer = plistValue (curPlist.get(), "CFBundleVersion");
        auto stamp = [] (const String& v) { auto t = StringArray::fromTokens (v.upToFirstOccurrenceOf ("-", false, false), ".", {});
                                            Array<int> o; for (int i = 0; i < 4; ++i) o.add (i < t.size() ? t[i].getIntValue() : 0); return o; };
        {
            auto a = stamp (curVer), b = stamp (newVer); bool newer = false, decided = false;
            for (int i = 0; i < 4 && ! decided; ++i) if (a[i] != b[i]) { newer = b[i] > a[i]; decided = true; }
            if (decided && ! newer) return Result::fail ("The downloaded build (" + newVer + ") is not newer than this one (" + curVer + ")");
        }
        if (threadShouldExit()) return Result::fail ("Cancelled");

        // 4. swap (atomic renames on the same volume), ONE .previous kept
        setStatusMessage ("Installing...");
        const File previous = parent.getChildFile (current.getFileName() + ".previous");
        previous.deleteRecursively();
        if (! current.moveFileTo (previous)) return Result::fail ("Could not set the current app aside");
        if (! staged.moveFileTo (current)) {
            previous.moveFileTo (current);   // restore
            return Result::fail ("Could not move the new app into place (restored the current one)");
        }
        current.setLastModificationTime (Time::getCurrentTime());
        installedPath = current;
        return Result::ok();
       #else
        return Result::fail ("In-place update is macOS only");
       #endif
    }

    VersionInfo::Asset asset;
    std::function<void (Result)> completion;
public:
    File installedPath;
};

void LatestVersionCheckerAndUpdater::installInPlace (const VersionInfo::Asset& zipAsset)
{
    inplace.reset (new InPlaceUpdateThread (zipAsset, [this] (Result r)
    {
        File installed = inplace != nullptr ? inplace->installedPath : File();
        inplace.reset();
        if (r.failed()) {
            AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon, "Update not installed",
                                              r.getErrorMessage() + "\n\nThe current version keeps running. You can use Download Installer instead.");
            return;
        }
       #if JUCE_MAC && SONOBUSPLUS_BUILD
        // relaunch once this process has exited: a detached shell waits on our pid, then `open -n` the new bundle.
        const String pid = String ((int64) getpid());
        ChildProcess relauncher;
        relauncher.start (StringArray { "/bin/sh", "-c",
            "while kill -0 " + pid + " 2>/dev/null; do sleep 0.3; done; /usr/bin/open -n " + installed.getFullPathName().quoted() }, 0);
        sbPlusQuitWithoutConfirm.store (true);   // no "you are connected" confirmation — the user just chose to relaunch
        if (auto* app = JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
       #else
        ignoreUnused (installed);
       #endif
    }));
}

//==============================================================================
JUCE_IMPLEMENT_SINGLETON (LatestVersionCheckerAndUpdater)
