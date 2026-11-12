// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// The player's list of public stages from the stage directory, with a ping to each one.
// Control path only: the list and the pings run on their own thread and never touch audio.
#pragma once
#include "JuceHeader.h"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

class StagesView : public juce::Component,
                   private juce::TableHeaderComponent::Listener,
                   private juce::AsyncUpdater,
                   private juce::Timer
{
public:
    struct Stage {
        juce::String id, name, host, city, country, genre, about, version, os;
        juce::StringArray players, instruments;   // one instrument per player, empty when not given
        int port = 0, count = 0, maxPlayers = 0, maxChannels = 0;
        juce::int64 since = 0;
        bool locked = false, test = false, older = false;
        float pingMs = -2.0f;   // -2 = not measured yet, -1 = no answer
    };
    struct JoinRequest { juce::String host; int port = 0; juce::String name, password; };

    StagesView();
    ~StagesView() override;

    std::function<void(const JoinRequest&)> onJoin;
    std::function<juce::String()> currentStage;   // the stage this player is in, empty when in none
    std::function<juce::String()> userName;
    std::function<juce::String()> userInstrument;

    void setActive(bool active);   // the list refreshes only while it is showing
    void setDirectoryMemory(const juce::File& file);   // where a moved directory address is kept between runs
    void setWantTestStages(bool want);                 // also ask the directory for its made-up test stages

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class Worker;
    class StageItem;
    class SubItem;

    void tableColumnsChanged(juce::TableHeaderComponent*) override;
    void tableColumnsResized(juce::TableHeaderComponent*) override;
    void tableSortOrderChanged(juce::TableHeaderComponent*) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void rebuild();
    void updateStatus();
    void flash(const juce::String& note);
    void fillChoice(juce::ComboBox& box, const juce::String& allText, const juce::StringArray& items);
    juce::Rectangle<int> columnRect(int columnId) const;
    void requestJoin(const Stage& s);
    void askPassword(const JoinRequest& req);

    juce::TextEditor mSearch;
    juce::ComboBox mGenre, mCountry, mKind;
    juce::ToggleButton mOpenOnly, mHideFull;
    juce::TextButton mRefresh;
    juce::Label mStatus;
    juce::TableHeaderComponent mHeader;
    juce::TreeView mTree;
    std::unique_ptr<juce::TreeViewItem> mRoot;
    juce::Rectangle<int> mFooter;

    std::vector<Stage> mStages;
    std::set<juce::String> mOpen;
    juce::String mSelectedId, mNote;
    juce::uint32 mNoteUntil = 0;
    juce::int64 mFetchedAt = 0;
    int mShown = 0, mShownPlayers = 0, mAttempts = 0;
    bool mReached = false, mEverReached = false, mTestData = false, mRebuilding = false;

    std::unique_ptr<Worker> mWorker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StagesView)
};
