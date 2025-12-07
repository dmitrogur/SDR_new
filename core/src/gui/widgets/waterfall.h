#pragma once
#include <vector>
#include <mutex>
#include <gui/widgets/bandplan.h>
#include <gui/file_dialogs.h>
// #include <gui/main_window.h>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <utils/event.h>

#include <utils/opengl_include_code.h>


#define WATERFALL_RESOLUTION 1000000
#define MAX_SERVERS          8

namespace ImGui {
    class WaterfallVFO {
    public:
        void setOffset(double offset);
        void setCenterOffset(double offset);
        void setBandwidth(double bw);
        void setReference(int ref);
        void setSnapInterval(double interval);
        void setNotchOffset(double offset);
        void setNotchVisible(bool visible);
        void updateDrawingVars(double viewBandwidth, float dataWidth, double viewOffset, ImVec2 widgetPos, int fftHeight); // NOTE: Datawidth double???
        void draw(ImGuiWindow* window, bool selected);
        enum {
            REF_LOWER,
            REF_CENTER,
            REF_UPPER,
            _REF_COUNT
        };

        double generalOffset;
        double centerOffset;
        double lowerOffset;
        double upperOffset;
        double bandwidth;
        double snapInterval = 5000;
        int reference = REF_CENTER;

        double notchOffset = 0;
        bool notchVisible = false;

        bool leftClamped;
        bool rightClamped;

        ImVec2 rectMin;
        ImVec2 rectMax;
        ImVec2 lineMin;
        ImVec2 lineMax;
        ImVec2 wfRectMin;
        ImVec2 wfRectMax;
        ImVec2 wfLineMin;
        ImVec2 wfLineMax;
        ImVec2 lbwSelMin;
        ImVec2 lbwSelMax;
        ImVec2 rbwSelMin;
        ImVec2 rbwSelMax;
        ImVec2 wfLbwSelMin;
        ImVec2 wfLbwSelMax;
        ImVec2 wfRbwSelMin;
        ImVec2 wfRbwSelMax;
        ImVec2 notchMin;
        ImVec2 notchMax;

        bool centerOffsetChanged = false;
        bool lowerOffsetChanged = false;
        bool upperOffsetChanged = false;
        bool redrawRequired = true;
        bool lineVisible = true;
        bool bandwidthChanged = false;

        double minBandwidth;
        double maxBandwidth;
        bool bandwidthLocked;

        ImU32 color = IM_COL32(255, 255, 255, 50);

        Event<double> onUserChangedBandwidth;
        Event<double> onUserChangedNotch;
        Event<int> onUserChangedDemodulator;
    };

    class WaterFall {
    public:
        WaterFall();

        void init();

        void loadSearchBlacklistFromFile(const std::string &dir);
        

        void draw();
        float* getFFTBuffer();
        void pushFFT();
        bool menuDialog();
        bool menuListOpen = false;
        bool iffinded_freq = true;

        void updatePallette(float colors[][3], int colorCount);
        void updatePalletteFromArray(float* colors, int colorCount);

        void setCenterFrequency(double freq);
        double getCenterFrequency();

        void setBandwidth(double bandWidth);
        double getBandwidth();

        void setViewBandwidth(double bandWidth);
        double getViewBandwidth();

        void setViewOffset(double offset);
        double getViewOffset();

        void setFFTMin(float min);
        float getFFTMin();

        void setFFTMax(float max);
        float getFFTMax();

        void setWaterfallMin(float min);
        float getWaterfallMin();

        void setWaterfallMax(float max);
        float getWaterfallMax();

        // void setZoom(double zoomLevel);
        // void setOffset(double zoomOffset);

        void autoRange();

        void selectFirstVFO();
        bool setCurrVFO(std::string newName);

        void showWaterfall();
        void hideWaterfall();

        void showFFT();
        void hideFFT();

        void showBandplan();
        void hideBandplan();

        void setFFTHeight(int height);
        int getFFTHeight();

        void setRawFFTSize(int size);

        void setFullWaterfallUpdate(bool fullUpdate);

        void setBandPlanPos(int pos);

        void setFFTHold(bool hold);
        void setFFTHoldSpeed(float speed);

        void setFFTSmoothing(bool enabled);
        void setFFTSmoothingSpeed(float speed);

        void setSNRSmoothing(bool enabled);
        void setSNRSmoothingSpeed(float speed);

        float* acquireLatestFFT(int& width);
        void releaseLatestFFT();

        void setSource(std::string source);
        std::string getSource();
        void setPrevCenterFrequency(double val);
        double getPrevCenterFrequency();

        bool centerFreqMoved = false;
        bool vfoFreqChanged = false;
        bool bandplanEnabled = false;
        bandplan::BandPlan_t* bandplan = NULL;

        bool mouseInFFTResize = false;
        bool mouseInFreq = false;
        bool mouseInFFT = false;
        bool mouseInWaterfall = false;

        float selectedVFOSNR = NAN;

        bool centerFrequencyLocked = false;

        std::map<std::string, WaterfallVFO*> vfos;
        std::string selectedVFO = "";
        bool selectedVFOChanged = false;

        struct FFTRedrawArgs {
            ImVec2 min;
            ImVec2 max;
            double lowFreq;
            double highFreq;
            double freqToPixelRatio;
            double pixelToFreqRatio;
            ImGuiWindow* window;
        };

        Event<FFTRedrawArgs> onFFTRedraw;

        struct InputHandlerArgs {
            ImVec2 fftRectMin;
            ImVec2 fftRectMax;
            ImVec2 freqScaleRectMin;
            ImVec2 freqScaleRectMax;
            ImVec2 waterfallRectMin;
            ImVec2 waterfallRectMax;
            double lowFreq;
            double highFreq;
            double freqToPixelRatio;
            double pixelToFreqRatio;
        };

        bool inputHandled = false;
        bool VFOMoveSingleClick = false;
        Event<InputHandlerArgs> onInputProcess;

        enum {
            REF_LOWER,
            REF_CENTER,
            REF_UPPER,
            _REF_COUNT
        };

        enum {
            BANDPLAN_POS_BOTTOM,
            BANDPLAN_POS_TOP,
            _BANDPLAN_POS_COUNT
        };

        ImVec2 fftAreaMin;
        ImVec2 fftAreaMax;
        ImVec2 freqAreaMin;
        ImVec2 freqAreaMax;
        ImVec2 wfMin;
        ImVec2 wfMax;

        struct FoundBookmark {
            double frequency;
            float bandwidth;
            int mode;
            int level;
            bool selected;
            std::time_t ftime;
            int Signal = -1;
        };

        struct SkipFoundBookmark {
            double frequency;
            float bandwidth;
            int mode;
            int level;
            bool selected;
            std::time_t ftime;
            // int signal;
        };
        void UpdateBlackList(const std::vector<SkipFoundBookmark> &items);
        
        FoundBookmark addFreq;

        std::map<double, FoundBookmark> finded_freq;
        std::map<double, FoundBookmark> finded_freq_copy;
        std::mutex findedFreqMtx;

        std::map<double, SkipFoundBookmark> skip_finded_freq;
        std::mutex skipFreqMutex;
        std::string selectedListName = "";
        unsigned int _count_Bookmark = 0;
        bool scan2_running = false;

        // std::vector<std::string> listNames;

        // gui::waterfall.
    private:
        void drawWaterfall();
        void drawFFT();
        void drawVFOs();
        void drawBandPlan();
        void processInputs();
        void onPositionChange();
        void onResize();
        void updateWaterfallFb();
        void updateWaterfallTexture();
        void updateAllVFOs(bool checkRedrawRequired = false);
        bool calculateVFOSignalInfo(float* fftLine, WaterfallVFO* vfo, float& strength, float& snr);
        // --- for scanner2
        void applyBookmark(FoundBookmark bm, std::string vfoName);
        void applySkipBookmark(SkipFoundBookmark bm, std::string vfoName);
        void exportBookmarks(std::string path);
        bool importBookmarks(std::string path, bool val);

        
        bool oldwaterfallVisible = 0;
        json rez_exportedBookmarks;
        bool rez_importOpen1 = false;
        bool rez_importOpen2 = false;
        bool rez_exportOpen = false;
        pfd::open_file* importDialog;
        pfd::save_file* exportDialog;

        std::string txt_error = "";
        bool _error = false;

        //--------------------------------------
        std::string sourceName = "File";
        double prevCenterFrequency = 0.0;

        bool waterfallUpdate = false;
        uint32_t waterfallPallet[WATERFALL_RESOLUTION];

        ImVec2 widgetPos;
        ImVec2 widgetEndPos;
        ImVec2 widgetSize;

        ImVec2 lastWidgetPos;
        ImVec2 lastWidgetSize;

        ImGuiWindow* window;

        GLuint textureId;

        std::recursive_mutex buf_mtx;
        std::recursive_mutex latestFFTMtx;
        std::mutex texMtx;
        std::mutex smoothingBufMtx;

        float vRange;

        int maxVSteps;
        int maxHSteps;

        int dataWidth;           // Width of the FFT and waterfall
        int fftHeight;           // Height of the fft graph
        int waterfallHeight = 0; // Height of the waterfall

        double viewBandwidth;
        double viewOffset;

        double lowerFreq;
        double upperFreq;
        double range;

        float lastDrag;

        int vfoRef = REF_CENTER;

        // Absolute values
        double centerFreq;
        double wholeBandwidth;

        // Ranges
        float fftMin;
        float fftMax;
        float waterfallMin;
        float waterfallMax;

        // std::vector<std::vector<float>> rawFFTs;
        int rawFFTSize;
        float* rawFFTs = NULL;
        float* latestFFT = NULL;
        float* latestFFTHold = NULL;
        float* smoothingBuf = NULL;
        int currentFFTLine = 0;
        int fftLines = 0;

        uint32_t* waterfallFb;

        bool draggingFW = false;
        int FFTAreaHeight;
        int newFFTAreaHeight;

        bool waterfallVisible = true;
        bool fftVisible = true;
        bool bandplanVisible = false;

        bool _fullUpdate = true;

        int bandPlanPos = BANDPLAN_POS_BOTTOM;

        bool fftHold = false;
        float fftHoldSpeed = 0.3f;

        bool fftSmoothing = false;
        float smoothingAlpha = 0.5;
        float smoothingBeta = 0.5;

        bool snrSmoothing = false;
        float snrSmoothingAlpha = 0.5;
        float snrSmoothingBeta = 0.5;

        // UI Select elements
        bool fftResizeSelect = false;
        bool freqScaleSelect = false;
        bool vfoSelect = false;
        bool vfoBorderSelect = false;
        WaterfallVFO* relatedVfo = NULL;
        ImVec2 mouseDownPos;

        ImVec2 lastMousePos;

        int radioMode = 0;
        uint8_t SERVERS_Count = 8;
        std::string ModeTxt = "Ручний режим";
        std::string SearchListTxt1 = "General";
        std::string ScanListTxt1 = "General";
        std::string CTRLListTxt1 = "General";
        std::string SearchListTxt2 = "General";
        std::string ScanListTxt2 = "General";
        std::string CTRLListTxt2 = "General";
        std::string SearchListTxt3 = "General";
        std::string ScanListTxt3 = "General";
        std::string CTRLListTxt3 = "General";
        std::string SearchListTxt4 = "General";
        std::string ScanListTxt4 = "General";
        std::string CTRLListTxt4 = "General";
        std::string SearchListTxt5 = "General";
        std::string ScanListTxt5 = "General";
        std::string CTRLListTxt5 = "General";
        std::string SearchListTxt6 = "General";
        std::string ScanListTxt6 = "General";
        std::string CTRLListTxt6 = "General";
        std::string SearchListTxt7 = "General";
        std::string ScanListTxt7 = "General";
        std::string CTRLListTxt7 = "General";
        std::string SearchListTxt8 = "General";
        std::string ScanListTxt8 = "General";
        std::string CTRLListTxt8 = "General";

        int selectedMode[MAX_SERVERS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int searchListId[MAX_SERVERS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int scanListId[MAX_SERVERS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int ctrlListId[MAX_SERVERS] = { 0, 0, 0, 0, 0, 0, 0, 0 };

        // skip_finded_freq blacklistPerInstance[MAX_SERVERS];
    };
};