// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell



#pragma once


//==============================================================================
class VersionInfo
{
public:
    struct Asset   // Members no longer const (the updater picks/assigns assets)
    {
        String name;
        String url;
        String sha256;    // Optional, from the appcast (zip assets)
        int64  size = 0;  // Optional
    };

    static std::unique_ptr<VersionInfo> fetchFromUpdateServer (const String& versionString);
    static std::unique_ptr<VersionInfo> fetchLatestFromUpdateServer();
    static std::unique_ptr<InputStream> createInputStreamForAsset (const Asset& asset, int& statusCode);

    bool isNewerVersionThanCurrent();

    const String versionString;
    const String releaseNotes;
    const std::vector<Asset> assets;

private:
    VersionInfo() = default;

    static std::unique_ptr<VersionInfo> fetch (const String&);
};
