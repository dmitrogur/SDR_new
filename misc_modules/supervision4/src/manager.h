#pragma once

#include <string>
#include "config.h"

struct FrequencyBookmark {
    double frequency;
    double bandwidth;
    int modeIndex;
    bool selected;
    int Signal;
};

struct ObservationBookmark {
    double frequency;
    float bandwidth;
    int mode;
    int level;
    std::string scard;
    bool selected;
    std::string dopinfo;
    int Signal;
};

struct WaterfallBookmark {
    std::string listName;
    std::string bookmarkName;
    std::string extraInfo;
    bool worked;
    ObservationBookmark bookmark;
    long long notValidAfter;
};

struct TransientBookmarkManager {
    std::vector<WaterfallBookmark> transientBookmarks;

    virtual void refreshWaterfallBookmarks(bool flag_vfo = false) = 0;
    // virtual const char *getModesList() = 0;
};

void applyBookmark(FrequencyBookmark bm, std::string vfoName);
ConfigManager &getFrequencyManagerConfig();