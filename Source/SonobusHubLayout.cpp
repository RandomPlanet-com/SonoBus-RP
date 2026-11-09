// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
#include "SonobusHubLayout.h"
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

std::vector<std::string> hubParseChannelNames(const char* blob, size_t size)
{
    std::vector<std::string> out;
    if (!blob || size == 0) return out;
    juce::ValueTree tree = juce::ValueTree::readFromData(blob, size);
    if (!tree.isValid()) return out;
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        auto g = tree.getChild(i);
        const juce::String name = g.getProperty("name").toString().trim();
        const int n = juce::jmax(1, (int) g.getProperty("numchan", 1));
        for (int c = 0; c < n; ++c) {
            juce::String label = name;
            if (n > 1) label = (name.isEmpty() ? juce::String("Ch") : name) + " " + juce::String(c + 1);
            if (label.isEmpty()) label = "Ch " + juce::String((int) out.size() + 1);
            out.push_back(label.toStdString());
        }
        if (out.size() > 256) break;
    }
    return out;
}
