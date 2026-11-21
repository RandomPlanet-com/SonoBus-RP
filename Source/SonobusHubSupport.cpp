// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Jason Buecker (Random Planet)
// See SonobusHubSupport.h
#include "SonobusHubSupport.h"
#include "SonobusHubGeo.h"        // version, build and platform texts
#include "SonobusHubSettings.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <set>
#include <sstream>
#include <vector>

#ifndef SONOBUS_HUB_VERSION
#define SONOBUS_HUB_VERSION "dev"
#endif

namespace hub_support {

namespace {

const char kMask[] = "********";

bool isKeyChar(char c) { return std::isalnum((unsigned char) c) != 0 || c == '_' || c == '-'; }
bool isBlank(char c) { return c == ' ' || c == '\t'; }
bool endsLinkValue(char c) {
    return c == '&' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\';
}
bool endsBareValue(char c) { return endsLinkValue(c) || c == ',' || c == ';' || c == '}' || c == ']' || c == ')'; }
bool isPasswordWord(const char* s) {
    static const char word[] = "password";
    for (int i = 0; i < 8; ++i) if (std::tolower((unsigned char) s[i]) != word[i]) return false;
    return true;
}

// Appends s[0..n) to `out` with every secret value replaced by the mask.
void maskInto(const char* s, size_t n, std::string& out) {
    size_t copied = 0;
    auto replaceRange = [&](size_t from, size_t to) { out.append(s + copied, from - copied); out += kMask; copied = to; };
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (c != 'p' && c != 'P') continue;
        // ?p=VALUE or &p=VALUE in a stage link
        if (c == 'p' && i > 0 && i + 1 < n && s[i + 1] == '=' && (s[i - 1] == '?' || s[i - 1] == '&' || s[i - 1] == ';')) {
            size_t e = i + 2;
            while (e < n && !endsLinkValue(s[e])) ++e;
            if (e > i + 2) replaceRange(i + 2, e);
            i = e - 1;
            continue;
        }
        if (i + 8 > n || !isPasswordWord(s + i)) continue;
        // A name with "password" in it, then : or = and a value; or an option --name-password and its value.
        size_t k = i + 8;
        while (k < n && isKeyChar(s[k])) ++k;
        size_t w = i;
        while (w > 0 && isKeyChar(s[w - 1])) --w;
        const bool option = w + 2 <= i && s[w] == '-' && s[w + 1] == '-';
        size_t v = k;
        if (v + 1 < n && s[v] == '\\' && s[v + 1] == '"') v += 2;
        else if (v < n && (s[v] == '"' || s[v] == '\'')) ++v;
        size_t a = v;
        while (a < n && isBlank(s[a])) ++a;
        if (a < n && (s[a] == ':' || s[a] == '=')) { ++a; while (a < n && isBlank(s[a])) ++a; }
        else if (!(option && a > v)) { i = k - 1; continue; }
        size_t b = a, e = a;
        if (a + 1 < n && s[a] == '\\' && s[a + 1] == '"') {          // a quoted value inside a quoted text
            b = e = a + 2;
            while (e < n && !(s[e] == '\\' && e + 1 < n && s[e + 1] == '"')) ++e;
        } else if (a < n && (s[a] == '"' || s[a] == '\'')) {
            const char q = s[a];
            b = e = a + 1;
            while (e < n && s[e] != q) e += (s[e] == '\\' && e + 1 < n) ? 2 : 1;
            if (e > n) e = n;
        } else {
            while (e < n && !endsBareValue(s[e])) ++e;
        }
        if (e > b) replaceRange(b, e);
        i = std::max(e, k) - 1;
    }
    out.append(s + copied, n - copied);
}

// A text file read line by line with its secrets masked, so the zip never holds them.
class MaskedTextStream : public juce::InputStream {
public:
    MaskedTextStream(const juce::File& f, juce::int64 startAt, const std::atomic<bool>* cancel) : mIn(f), mCancel(cancel) {
        if (!mIn.openedOk()) { mDone = true; return; }
        if (startAt > 0 && mIn.setPosition(startAt)) {   // start at the next whole line
            char c = 0;
            int guard = 1 << 20;
            while (--guard > 0 && mIn.read(&c, 1) == 1 && c != '\n') {}
        }
    }
    juce::int64 getTotalLength() override { return -1; }
    bool isExhausted() override { fill(); return mOutPos >= mOut.size(); }
    int read(void* dest, int maxBytes) override {
        fill();
        const int n = (int) std::min<size_t>((size_t) std::max(0, maxBytes), mOut.size() - mOutPos);
        if (n > 0) { std::memcpy(dest, mOut.data() + mOutPos, (size_t) n); mOutPos += (size_t) n; mPos += n; }
        return n;
    }
    juce::int64 getPosition() override { return mPos; }
    bool setPosition(juce::int64) override { return false; }

private:
    static constexpr int kChunk = 64 * 1024;
    static constexpr size_t kLongestLine = 1024 * 1024;   // a longer run without a line end is cut there
    void fill() {
        while (mOutPos >= mOut.size() && !mDone) {
            mOut.clear();
            mOutPos = 0;
            if (mCancel != nullptr && mCancel->load()) { mDone = true; break; }
            const size_t had = mCarry.size();
            mCarry.resize(had + (size_t) kChunk);
            const int got = mIn.read(&mCarry[had], kChunk);
            mCarry.resize(had + (size_t) std::max(0, got));
            const bool atEnd = got <= 0;
            size_t end = mCarry.size();
            if (!atEnd) {
                const size_t nl = mCarry.rfind('\n');
                if (nl != std::string::npos) end = nl + 1;
                else if (mCarry.size() < kLongestLine) continue;
            }
            size_t start = 0;
            while (start < end) {
                const void* p = std::memchr(mCarry.data() + start, '\n', end - start);
                const size_t lineEnd = p != nullptr ? (size_t) ((const char*) p - mCarry.data()) + 1 : end;
                maskInto(mCarry.data() + start, lineEnd - start, mOut);
                start = lineEnd;
            }
            mCarry.erase(0, end);
            if (atEnd) mDone = true;
        }
    }
    juce::FileInputStream mIn;
    const std::atomic<bool>* mCancel;
    std::string mCarry, mOut;
    size_t mOutPos = 0;
    juce::int64 mPos = 0;
    bool mDone = false;
};

constexpr juce::int64 kMostLogBytes = 64LL * 1024 * 1024;     // per file; a larger one gives its newest part
constexpr juce::int64 kMostCrashBytes = 8LL * 1024 * 1024;    // per report
constexpr int kMostCrashReports = 20;
constexpr int kCrashDays = 30;
constexpr int kCompression = 5;

struct Entry { juce::File file; juce::String stored; juce::int64 startAt = 0; };

juce::String safeName(const juce::String& text, int most) {
    juce::String out;
    bool dash = false;
    for (auto c : text) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (keep) { out << juce::String::charToString(c); dash = false; }
        else if (!dash && out.isNotEmpty()) { out << "-"; dash = true; }
        if (out.length() >= most) break;
    }
    while (out.endsWithChar('-')) out = out.dropLastCharacters(1);
    return out;
}

// hub.log and hub.log.1 of one hub, under `folder` in the zip ("" = the top).
void addLogs(const juce::File& log, const juce::String& folder, std::vector<Entry>& plan, juce::StringArray& notes) {
    if (log == juce::File()) return;
    bool any = false;
    for (const juce::File& f : { log, juce::File(log.getFullPathName() + ".1") }) {
        if (!f.existsAsFile()) continue;
        any = true;
        Entry e;
        e.file = f;
        e.stored = folder + f.getFileName();
        const juce::int64 size = f.getSize();
        if (size > kMostLogBytes) {
            e.startAt = size - kMostLogBytes;
            notes.add(e.stored + ": only its newest " + juce::String(kMostLogBytes / (1024 * 1024)) + " MB (the file has " + juce::String(size / (1024 * 1024)) + " MB)");
        }
        plan.push_back(e);
    }
    if (!any) notes.add(folder + log.getFileName() + ": no log file found");
}

#if JUCE_MAC
void addCrashReports(std::vector<Entry>& plan, juce::StringArray& notes) {
    const juce::File root = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Logs/DiagnosticReports");
    const juce::Time oldest = juce::Time::getCurrentTime() - juce::RelativeTime::days(kCrashDays);
    std::vector<std::pair<juce::Time, Entry>> found;
    for (const auto& item : { std::make_pair(root, juce::String("crash-reports/")), std::make_pair(root.getChildFile("Retired"), juce::String("crash-reports/Retired/")) }) {
        if (!item.first.isDirectory()) continue;
        for (const auto& f : item.first.findChildFiles(juce::File::findFiles, false, "*")) {
            if (!f.getFileName().startsWithIgnoreCase("sonobus-hub")) continue;
            const juce::Time when = f.getLastModificationTime();
            if (when < oldest) continue;
            if (f.getSize() > kMostCrashBytes) { notes.add(item.second + f.getFileName() + ": left out, too large"); continue; }
            Entry e;
            e.file = f;
            e.stored = item.second + f.getFileName();
            found.push_back({ when, e });
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return y.first < x.first; });
    if ((int) found.size() > kMostCrashReports) {
        notes.add("crash reports: the newest " + juce::String(kMostCrashReports) + " of " + juce::String((int) found.size()));
        found.resize((size_t) kMostCrashReports);
    }
    for (const auto& f : found) plan.push_back(f.second);
}
#endif

std::string utcText(juce::Time t) {
    const std::time_t secs = (std::time_t) (t.toMilliseconds() / 1000);
    std::tm tm {};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char b[40];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return b;
}

juce::String uptimeText(long long sec) {
    if (sec < 0) return "not known";
    juce::String s;
    if (sec >= 86400) s << (sec / 86400) << " d ";
    s << juce::String((sec % 86400) / 3600) << " h " << juce::String((sec % 3600) / 60) << " min";
    return s;
}

} // namespace

std::string maskSecrets(const std::string& line) {
    std::string out;
    out.reserve(line.size() + 8);
    maskInto(line.data(), line.size(), out);
    return out;
}

juce::String zipFileName(const std::string& stageName, juce::Time now) {
    juce::String stage = safeName(juce::String::fromUTF8(stageName.c_str()), 40);
    if (stage.isEmpty()) stage = "stage";
    return "SonoBus-Hub-logs-" + stage + "-" + now.formatted("%Y%m%d-%H%M") + ".zip";
}

bool writeLogsZip(const Info& info, juce::OutputStream& out, std::string& err) {
    namespace hs = sb_hub_gui::hub_settings;
    const juce::Time now = juce::Time::getCurrentTime();
    std::vector<Entry> plan;
    juce::StringArray notes;
    std::ostringstream about;
    about << "SonoBus RP Hub: logs for support\n"
          << "No passwords are in this zip. Settings files are left out; passwords in log text are replaced by " << kMask << ".\n\n"
          << "Hub version:  " << SONOBUS_HUB_VERSION << "  (build " << hubBuildId() << ", " << hubCommitId() << ")\n"
          << "System:       " << hubPlatformString() << "\n"
          << "Stage:        " << info.stageName << "\n"
          << "Ports:        connection " << info.serverPort << " (TCP + UDP), audio " << info.udpPort << " (UDP), web page "
          << (info.webPort > 0 ? std::to_string(info.webPort) : std::string("off")) << "\n"
          << "Main hub:     " << (hs::isStage() ? "no, a stage of the main hub in " + hs::stageMainFolder().getFullPathName().toStdString() : std::string("yes")) << "\n"
          << "Uptime:       " << uptimeText(info.uptimeSec) << "\n"
          << "Zip made:     " << now.formatted("%Y-%m-%d %H:%M:%S") << " local (" << now.getUTCOffsetString(true) << "), " << utcText(now) << "\n"
          << "Log file:     " << (info.logFile == juce::File() ? juce::String("none") : info.logFile.getFullPathName()) << "\n";

    addLogs(info.logFile, "", plan, notes);

    if (hs::canManageOtherStages()) {
        const auto stages = hs::otherStages();
        about << "\nOther stages: " << stages.size() << "\n";
        std::set<juce::String> used;
        for (const auto& st : stages) {
            const juce::File dir = st.file.getParentDirectory();
            juce::String folder = safeName(dir.getFileName(), 60);
            if (folder.isEmpty()) folder = "stage";
            for (int n = 2; used.count(folder) != 0; ++n) folder = safeName(dir.getFileName(), 60) + "-" + juce::String(n);
            used.insert(folder);
            about << "  - " << (st.group.empty() ? std::string("(no name)") : st.group)
                  << ": connection " << st.serverPort << ", audio " << st.udpPort << ", web page " << (st.webPort > 0 ? std::to_string(st.webPort) : std::string("off"))
                  << ", " << (st.running ? (st.stopping ? "stopping" : "running") : "stopped");
            if (st.running && st.players >= 0) about << ", " << st.players << " players";
            if (!st.problem.empty()) about << ", problem: " << maskSecrets(st.problem);
            about << "  [stages/" << folder << "/]\n";
            addLogs(dir.getChildFile("hub.log"), "stages/" + folder + "/", plan, notes);
        }
    }
#if JUCE_MAC
    addCrashReports(plan, notes);
#endif

    about << "\nFiles in this zip:\n";
    for (const auto& e : plan)
        about << "  " << e.stored << "  (" << juce::String((double) (e.file.getSize() - e.startAt) / (1024.0 * 1024.0), 1) << " MB, changed "
              << e.file.getLastModificationTime().formatted("%Y-%m-%d %H:%M") << ")\n";
    if (plan.empty()) about << "  none\n";
    if (!notes.isEmpty()) {
        about << "\nNotes:\n";
        for (const auto& n : notes) about << "  " << n << "\n";
    }

    juce::ZipFile::Builder zip;
    const std::string aboutText = about.str();
    zip.addEntry(new juce::MemoryInputStream(aboutText.data(), aboutText.size(), true), kCompression, "about.txt", now);
    for (const auto& e : plan)
        zip.addEntry(new MaskedTextStream(e.file, e.startAt, info.cancel), kCompression, e.stored, e.file.getLastModificationTime());

    const bool written = zip.writeToStream(out, nullptr);
    out.flush();
    if (info.cancel != nullptr && info.cancel->load()) { err = "it was stopped before it finished"; return false; }
    if (!written) { err = "the zip file could not be written"; return false; }
    return true;
}

} // namespace hub_support
