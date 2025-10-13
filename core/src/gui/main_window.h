#pragma once
#include <imgui/imgui.h>
#include <fftw3.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <utils/event.h>
#include <mutex>
#include <gui/tuner.h>
#include <atomic>

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground
#define MAX_SERVERS 8
#define SOURCE_ARM "APM"
#define MAIN_SAMPLE_RATE 10000000
#define VIEWBANDWICH 8500150
#define ALL_MOD 5
// 1MSample buffer
// #define STREAM_BUFFER_SIZE 1000000
// s     (STREAM_BUFFER_SIZE * sizeof(dsp::complex_t) * 2)
#define MAX_STRUCT_SIZE 1024

enum
{
    ARM_STATUS_NOT_CONTROL,
    ARM_STATUS_STAT_CONTROL,
    ARM_STATUS_FULL_CONTROL
};

enum {
    RECORDER_IFACE_CMD_GET_MODE,
    RECORDER_IFACE_CMD_SET_MODE,
    RECORDER_IFACE_CMD_START,
    RECORDER_IFACE_CMD_STOP,
    RECORDER_IFACE_CMD_SET_STREAM,
    MAIN_SET_START,
    MAIN_SET_STOP,
    MAIN_GET_PROCESSING,
    RECORDER_IFACE_GET_TEMPL,
    RECORDER_IFACE_CMD_SET_FREQ,
    MAIN_GET_STATUS_CHANGE,
    MAIN_SET_STATUS_CHANGE,
    RECORDER_IFACE_CMD_START_AKF,
    RECORDER_IFACE_GET_RECORD,
    RECORDER_IFACE_GET_SIGNAL,
    RECORDER_IFACE_GET_AKF_TIMEOUT
};

enum
{
    AIRSPY_IFACE_CMD_GET_RECORDING,
    AIRSPY_IFACE_NAME_SER_NOM
};

class MainWindow
{
public:
    ~MainWindow();
    void init();
    void draw();
    void setViewBandwidthSlider(float bandwidth);
    float getViewBandwidthSlider(uint8_t srv);
    void setFirstMenuRender();

    void setFFTMaxSlider(float fft);
    float getFFTMaxSlider(uint8_t srv);
    void setFFTMinSlider(float fft);
    float getFFTMinSlider(uint8_t srv);

    static float *acquireFFTBuffer(void *ctx);
    static void releaseFFTBuffer(void *ctx);

    // TODO: Replace with it's own class
    // void setVFO(double freq);

    bool getIsServer()
    {
        return isServer;
    }
    bool getIsARM()
    {
        return isARM;
    }

    void setPlayState(bool _playing);
    bool isPlaying();
    void setServerPlayState(uint8_t srv, bool _playing);
    bool isServerPlaying(uint8_t srv);
    void setARMPlayState(bool _playing);
    bool isARMPlaying();
    void setServerIsNotPlaying(uint8_t srv, bool _playing);
    bool isServerIsNotPlaying(uint8_t srv);

    bool lockWaterfallControls = false;
    bool playButtonLocked = false;

    void setSource(std::string source)
    {
        sourceName = source;
    }

    std::atomic<bool> pleaseRestartPlayer{false};
    void restoreUIStateForServer(uint8_t serverId);
    /*
    void setSizeofSearchModeList(size_t val) {
        sizeofSearchModeList = val;
    }
    size_t getSizeofSearchModeList() {
        return sizeofSearchModeList;
    }
    void setSizeofScanModeList(size_t val) {
        sizeofScanModeList = val;
    }
    size_t getSizeofScanModeList() {
        return sizeofScanModeList;
    }
    void setSizeofCtrlModeList(size_t val) {
        sizeofCtrlModeList = val;
    }
    size_t getSizeofCtrlModeList() {
        return sizeofCtrlModeList;
    }
    */
    //==========================================================================================
    // MAIN
    bool getFirstConn(uint8_t modul)
    {
        return _firstConn[modul];
    }
    void setFirstConn(uint8_t modul, bool val)
    {
        if (modul >= 5)
        {
            _firstConn[0] = val;
            _firstConn[1] = val;
            _firstConn[2] = val;
            _firstConn[3] = val;
            _firstConn[4] = val;
        }
        else
            _firstConn[modul] = val;
    }

    uint8_t getCurrServer()
    {
        return currServer;
    }
    void setCurrServer(uint8_t val)
    {
        flog::info("setCurrServer val {0}", val);
        currServer = val;
    }

    void setServersName(uint8_t srv, std::string name);
    std::string getServersName(uint8_t srv);

    void setVersion(uint8_t srv, std::string name);
    std::string getVersion(uint8_t srv);

    void setServerStatus(uint8_t srv, uint8_t vol);
    int getServerStatus(uint8_t srv);

    void setUpdateServerStatus(uint8_t srv, bool val);
    bool getUpdateServerStatus(uint8_t srv);

    void setstatusControl(uint8_t srv, int val)
    {
        // flog::info("setstatusControl. srv {0}, val {1}", srv, val);
        if (srv >= MAX_SERVERS)
            return;
        if (val == ARM_STATUS_FULL_CONTROL)
        {
            for (int i = 0; i++; i < MAX_SERVERS)
            {
                if (ServerStatusControl[i] == ARM_STATUS_FULL_CONTROL)
                {
                    ServerStatusControl[i] = ARM_STATUS_STAT_CONTROL;
                    // flog::info("1. ServerStatusControl[{0}] {1}", i, (int) ARM_STATUS_STAT_CONTROL);
                }
            }
        }
        else
        {
            int num = -1;
            for (int i = 0; i++; i < MAX_SERVERS)
            {
                if (ServerStatusControl[i] == ARM_STATUS_STAT_CONTROL)
                {
                    num = i;
                    // flog::info("2. num {0} = i {1}", num, i);
                }
            }
            if (num == -1)
                ServerStatusControl[0] = ARM_STATUS_FULL_CONTROL;
        }
        ServerStatusControl[srv] = val;
    }
    int getstatusControl(uint8_t srv)
    {
        // flog::info("srv {0}, val {1}", srv, (int) ServerStatusControl[srv]);
        if (srv < MAX_SERVERS)
            return ServerStatusControl[srv];
        else
            return 0;
    }

    void setServersFreq(uint8_t srv, double val)
    {
        // flog::info("setServersFreq srv {0} = {1}", srv, val);
        serversFreq[srv] = val;
    }
    double getServersFreq(uint8_t srv)
    {
        return serversFreq[srv];
    }

    // Bottom Panel
    void setUpdateSrchListForBotton(uint8_t srv, bool val);
    bool getUpdateSrchListForBotton(uint8_t srv);
    void setUpdateScanListForBotton(uint8_t srv, bool val);
    bool getUpdateScanListForBotton(uint8_t srv);
    void setUpdateCTRLListForBotton(uint8_t srv, bool val);
    bool getUpdateCTRLListForBotton(uint8_t srv);

    void setSearchListNamesTxt(std::string val);
    void setScanListNamesTxt(std::string val);
    void setCTRLListNamesTxt(std::string val);
    void setSearchListNamesTxtOld(uint8_t srv, std::string val);

    std::string getSearchListNamesTxt(uint8_t srv);
    std::string getScanListNamesTxt(uint8_t srv);
    std::string getCTRLListNamesTxt(uint8_t srv);

    void setUpdateMenuSnd0Main(uint8_t srv, bool val);
    bool getUpdateMenuSnd0Main(uint8_t srv);
    void setUpdateMenuRcv0Main(uint8_t srv, bool val);
    bool getUpdateMenuRcv0Main(uint8_t srv);
    void setUpdateFreq(bool val);
    bool getUpdateFreq();

    void settuningMode(bool val);
    bool gettuningMode();

    // AIRSPY
    void setChangeGainFalse();
    bool getChangeGain();
    void setGainMode(int val);
    int getGainMode();
    void setLinearGain(int val, bool updt = false);
    int getLinearGain();

    int getlnaGain();
    int getvgaGain();
    int getmixerGain();
    int getlinearGain();
    int getsensitiveGain();
    int getgainMode();
    bool getlnaAgc();
    bool getmixerAgc();
    bool getselect();
    bool get_updateLinearGain();
    bool getUpdateMenuRcv();
    bool getUpdateMenuSnd();

    void setlnaGain(int val);
    void setvgaGain(int val);
    void setmixerGain(int val);
    void setlinearGain(int val);
    void setsensitiveGain(int val);
    void setgainMode(int val);
    void setlnaAgc(bool val);
    void setmixerAgc(bool val);
    void setselect(bool val);
    void set_updateLinearGain(bool val);
    void setUpdateMenuRcv(bool val);
    void setUpdateMenuSnd(bool val);

    // RADIO
    int getselectedDemodID();
    float getbandwidth();
    int getsnapInterval();
    int getsnapIntervalId();
    int getdeempId();
    bool getnbEnabled();
    float getnbLevel();
    int getCMO_BBand();
    bool getsquelchEnabled();
    float getsquelchLevel();
    bool getFMIFNREnabled();
    int getfmIFPresetId();

    bool getlowPass();
    bool gethighPass();
    float getagcAttack();
    float getagcDecay();
    bool getcarrierAgc();
    int gettone();
    bool getUpdateMenuRcv2Radio();
    bool getUpdateMenuSnd2Radio();
    bool getUpdateMODRadio();

    void setselectedDemodID(int val);
    void setbandwidth(float val);
    void setsnapInterval(int val);
    void setsnapIntervalId(int val);
    void setdeempId(int val);
    void setnbEnabled(bool val);
    void setnbLevel(float val);
    void setCMO_BBand(int val);
    void setsquelchEnabled(bool val);
    void setsquelchLevel(float val);
    void setFMIFNREnabled(bool val);
    void setfmIFPresetId(int val);

    void setlowPass(bool val);
    void sethighPass(bool val);
    void setagcAttack(float val);
    void setagcDecay(float val);
    void setcarrierAgc(bool val);
    void settone(int val);

    void setUpdateMenuRcv2Radio(bool val);
    void setUpdateMenuSnd2Radio(bool val);
    void setUpdateMODRadio(bool val);

    int getSelectedMode(int srv) {
        return SelectedMode[srv];    
    };
    void setSelectedMode(int srv, int val) {
        SelectedMode[srv] = val;
    };

    // RECORD
    bool getRecording();
    void setRecording(bool val);
    bool getServerRecording(int srv);
    void setServerRecordingStop(int srv);
    void setServerRecordingStart(int srv);
    void setUpdateMenuSnd3Record(bool val);
    bool getUpdateMenuSnd3Record();
    void setUpdateMenuRcv3Record(bool val);
    bool getUpdateMenuRcv3Record();

    // SINK

    // SEARCH
#define MAX_BM_SIZE 64
#define MAX_COUNT_OF_DATA 100
#define MAX_COUNT_OF_CTRL_LIST 8
#define MAX_LIST_PACKET_SRCH_SIZE (MAX_BM_SIZE + 1) * 120 // MAX_BM_SIZE * sizeof(pSearch){96/120}
#define MAX_LIST_PACKET_SCAN_SIZE 157600                  // MAX_LIST_PACKET_SIZE MAX_BM_SIZE * sizeof(pScan)  156160
#define SIZE_OF_CTRL 264 * 8
#define MAX_LIST_PACKET_CTRL_SIZE (MAX_BM_SIZE + 1) * SIZE_OF_CTRL // 17160 MAX_LIST_PACKET_SIZE MAX_BM_SIZE * sizeof(pScan)  156160

    void setbbuf_srch(void *buf, int sz_val)
    {
        if (bbuf_srch == NULL)
            bbuf_srch = ::operator new(MAX_LIST_PACKET_SRCH_SIZE);
        memcpy(bbuf_srch, buf, sz_val);
        sizeOfbbuf_srch = sz_val;
        flog::info("setbbuf_srch sizeOfbbuf_srch {0}", sizeOfbbuf_srch);
        updateLists_srch = true;
    }
    void *getbbuf_srch()
    {
        // flog::info("getbbuf_srch sizeOfbbuf_srch {0}", sizeOfbbuf_srch);
        return bbuf_srch;
    }
    int getsizeOfbbuf_srch()
    {
        // flog::info("getsizeOfbbuf_srch sizeOfbbuf_srch {0}", sizeOfbbuf_srch);
        return sizeOfbbuf_srch;
    }
    int getsizeOfbbuf_srch_stat()
    {
        // flog::info("getsizeOfbbuf_srch_stat sizeOfbbuf_srch {0}", sizeOfbbuf_srch_stat);
        return sizeOfbbuf_srch_stat;
    }
    bool getUpdateLists_srch()
    {
        return updateLists_srch;
    }
    void setUpdateLists_srch(bool val)
    {
        updateLists_srch = val;
    }
    bool getUpdateLists_srch_stat()
    {
        return updateLists_srch_stat;
    }
    void setUpdateLists_srch_stat(bool val)
    {
        updateLists_srch_stat = val;
    }
    void setbbuf_srch_stat(void *buf, int sz_val)
    {
        if (bbuf_srch_stat == NULL)
            bbuf_srch_stat = ::operator new(MAX_LIST_PACKET_SCAN_SIZE);
        memcpy(bbuf_srch_stat, buf, sz_val);
        sizeOfbbuf_srch_stat = sz_val;
        flog::info("setbbuf_srch_stat sizeOfbbuf_srch_stat {0}", sizeOfbbuf_srch);
        updateLists_srch_stat = true;
    }

    bool getbutton_srch(uint8_t srv);
    void setbutton_srch(uint8_t srv, bool val);
    int getidOfList_srch(uint8_t srv);
    void setidOfList_srch(uint8_t srv, int val);
    int getselectedLogicId(uint8_t srv);
    void setselectedLogicId(uint8_t srv, int val);
    int getselectedSrchMode(uint8_t srv);
    void setselectedSrchMode(uint8_t srv, int val);

    int getLevelDbSrch(uint8_t srv);
    void setLevelDbSrch(uint8_t srv, int val);
    int getLevelDbScan(uint8_t srv);
    void setLevelDbScan(uint8_t srv, int val);
    int getLevelDbCtrl(uint8_t srv);
    void setLevelDbCtrl(uint8_t srv, int val);

    bool getAuto_levelSrch(uint8_t srv);
    void setAuto_levelSrch(uint8_t srv, bool val);
    bool getAuto_levelScan(uint8_t srv);
    void setAuto_levelScan(uint8_t srv, bool val);
    bool getAuto_levelCtrl(uint8_t srv);
    void setAuto_levelCtrl(uint8_t srv, bool val);
    int getSNRLevelDb(uint8_t srv);
    void setSNRLevelDb(uint8_t srv, int val);
    bool getAKFInd(uint8_t srv);
    void setAKFInd(uint8_t srv, bool val);
    bool getAKFInd_ctrl(uint8_t srv);
    void setAKFInd_ctrl(uint8_t srv, bool val);
    void setFirstStart_ctrl(uint8_t srv, bool val)
    {
        if (srv >= MAX_SERVERS)
        {
            for (int i = 0; i < MAX_SERVERS; i++)
            {
                FirstStart_ctrl[i] = val;
            }
        }
        else
            FirstStart_ctrl[srv] = val;
    };
    bool getFirstStart_ctrl(uint8_t srv)
    {
        return FirstStart_ctrl[srv];
    }

    bool getUpdateModule_srch(uint8_t srv);
    void setUpdateModule_srch(uint8_t srv, bool val);
    void setUpdateListRcv5Srch(uint8_t srv, bool val);
    bool getUpdateListRcv5Srch(uint8_t srv);

    void setUpdateMenuSnd5Srch(uint8_t srv, bool val);
    bool getUpdateMenuSnd5Srch(uint8_t srv);
    void setUpdateMenuRcv5Srch(bool val);
    bool getUpdateMenuRcv5Srch();

    void setUpdateStatSnd5Srch(bool val);
    bool getUpdateStatSnd5Srch();

    // SCAN ================================================

    void setbbuf_scan(void *buf, int sz_val)
    {
        if (bbuf_scan == NULL)
            bbuf_scan = ::operator new(MAX_LIST_PACKET_SCAN_SIZE);
        memcpy(bbuf_scan, buf, sz_val);
        sizeOfbbuf_scan = sz_val;
        // flog::info("setbbuf_scan sizeOfbbuf_scan {0}", sizeOfbbuf_scan);
        updateLists_scan = true;
    }
    void *getbbuf_scan()
    {
        return bbuf_scan;
    }
    int getsizeOfbbuf_scan()
    {
        return sizeOfbbuf_scan;
    }
    bool getUpdateLists_scan()
    {
        return updateLists_scan;
    }
    void setUpdateLists_scan(bool val)
    {
        updateLists_scan = val;
    }

    bool getbutton_scan(uint8_t srv);
    void setbutton_scan(uint8_t srv, bool val);
    int getidOfList_scan(uint8_t srv);
    void setidOfList_scan(uint8_t srv, int val);
    void setMaxRecWaitTime_scan(uint8_t srv, int val);
    int getMaxRecWaitTime_scan(uint8_t srv);
    void setMaxRecDuration_scan(uint8_t srv, int val);
    int getMaxRecDuration_scan(uint8_t srv);

    bool getUpdateModule_scan(uint8_t srv);
    void setUpdateModule_scan(uint8_t srv, bool val);
    void setUpdateListRcv6Scan(uint8_t srv, bool val);
    bool getUpdateListRcv6Scan(uint8_t srv);
    void setUpdateMenuSnd6Scan(uint8_t srv, bool val);
    bool getUpdateMenuSnd6Scan(uint8_t srv);
    void setUpdateMenuRcv6Scan(bool val);
    bool getUpdateMenuRcv6Scan();

    // CONTROl Ctrl
    void setflag_level_ctrl(uint8_t srv, bool val);
    // void setlevel_ctrl(uint8_t srv, int val);
    bool getflag_level_ctrl(uint8_t srv);
    // int getlevel_ctrl(uint8_t srv);

    void setMaxRecWaitTime_ctrl(uint8_t srv, int val);
    int getMaxRecWaitTime_ctrl(uint8_t srv);
    void setbbuf_ctrl(void *buf, int sz_val)
    {
        flog::info("setbbuf_ctrl sz_val {0}", sz_val);
        if (bbuf_ctrl == NULL)
            bbuf_ctrl = ::operator new(MAX_LIST_PACKET_CTRL_SIZE);
        memcpy(bbuf_ctrl, buf, sz_val);
        sizeOfbbuf_ctrl = sz_val;
        flog::info("setbbuf_ctrl sizeOfbbuf_ctrl {0}", sizeOfbbuf_ctrl);
        updateLists_ctrl = true;
    }
    void *getbbuf_ctrl()
    {
        return bbuf_ctrl;
    }
    int getsizeOfbbuf_ctrl()
    {
        return sizeOfbbuf_ctrl;
    }
    bool getUpdateLists_ctrl()
    {
        return updateLists_ctrl;
    }
    bool getbutton_ctrl(uint8_t srv);
    void setbutton_ctrl(uint8_t srv, bool val);
    bool getupdateStart_ctrl(uint8_t srv);
    void setupdateStart_ctrl(uint8_t srv, bool val);

    int getidOfList_ctrl(uint8_t srv);
    void setidOfList_ctrl(uint8_t srv, int val);
    bool getUpdateModule_ctrl(uint8_t srv);
    void setUpdateModule_ctrl(uint8_t srv, int val);
    void setUpdateListRcv7Ctrl(uint8_t srv, bool val);
    bool getUpdateListRcv7Ctrl(uint8_t srv);
    void setUpdateMenuSnd7Ctrl(uint8_t srv, bool val);
    bool getUpdateMenuSnd7Ctrl(uint8_t srv);
    void setUpdateMenuRcv7Ctrl(bool val);
    bool getUpdateMenuRcv7Ctrl();

    Event<bool> onPlayStateChange;

    void setFullConnection(uint8_t srv, bool val);
    bool getFullConnection(uint8_t srv);
    bool getIfOneButtonStart();
    bool getButtonStart(uint8_t srv);
    bool fullConnection[MAX_SERVERS] = {false, true, true, true, true, true, true, true};
    void setServerSampleRate(uint8_t srv, double val);
    double getServerSampleRate(uint8_t srv);
    void setStopMenuUI(bool val) {
        suppressSinkUI =val;
    }
    bool getStopMenuUI() {
        return suppressSinkUI;
    }

private:
    static void vfoAddedHandler(VFOManager::VFO *vfo, void *ctx);
    void cleanupStaleInstances();

    // void selectSource(std::string name);
    // void refreshSources();
    // static void onSourceRegistered(std::string name, void* ctx);
    // static void onSourceUnregister(std::string name, void* ctx);
    // static void onSourceUnregistered(std::string name, void* ctx);

    int sourceId = 0;
    std::vector<std::string> sourceNames;
    std::string sourceNamesTxt;
    std::string selectedSource;

    // FFT Variables
    int fftSize = 8192 * 8;
    std::mutex fft_mtx;
    fftwf_complex *fft_in, *fft_out;
    fftwf_plan fftwPlan;

    // GUI Variables
    bool firstMenuRender = true;
    bool startedWithMenuClosed = false;
    float fftMin = -70.0;
    float fftMax = 0.0;
    float bw = 0.922;
    float server_bw[MAX_SERVERS] = {0.922, 0.922, 0.922, 0.922, 0.922, 0.922, 0.922, 0.922};
    float server_fftmax[MAX_SERVERS] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    float server_fftmin[MAX_SERVERS] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    bool playing = false;
    bool arm_playing = false;
    bool showCredits = false;
    std::string audioStreamName = "";
    std::string sourceName = "";
    int menuWidth = 300;
    bool grabbingMenu = false;
    int newWidth = 300;
    int fftHeight = 300;
    bool showMenu = true;
    bool showMenuElements = false;
    bool tuningMode = tuner::TUNER_MODE_CENTER;
    dsp::stream<dsp::complex_t> dummyStream;
    bool demoWindow = false;
    int selectedWindow = 0;

    bool initComplete = false;
    bool autostart = false;

    bool isServer = false;
    bool isARM = false;
    bool _firstConn[5] = {false, false, false, false, false};

    EventHandler<VFOManager::VFO *> vfoCreatedHandler;
    EventHandler<std::string> sourceRegisteredHandler;
    EventHandler<std::string> sourceUnregisterHandler;
    EventHandler<std::string> sourceUnregisteredHandler;

    bool updateServerStatus[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    int serverStatus[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    int ServerStatusControl[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool server_playing[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool isNotPlaying[MAX_SERVERS]   = {false, false, false, false, false, false, false, false};

    // size_t sizeofSearchModeList = 0;
    // size_t sizeofScanModeList  = 0;
    // size_t sizeofCtrlModeList  = 0;
    // ========== Bottom Panel

    std::string Server1Name = "П1";
    std::string Server2Name = "П2";
    std::string Server3Name = "П3";
    std::string Server4Name = "П4";
    std::string Server5Name = "П5";
    std::string Server6Name = "П6";
    std::string Server7Name = "П7";
    std::string Server8Name = "П8";

    std::string Version1Name = "4.0.0";
    std::string Version2Name = "4.0.0";
    std::string Version3Name = "4.0.0";
    std::string Version4Name = "4.0.0";
    std::string Version5Name = "4.0.0";
    std::string Version6Name = "4.0.0";
    std::string Version7Name = "4.0.0";
    std::string Version8Name = "4.0.0";

    uint8_t currServer = 0;
    std::string SearchList1 = "";
    std::string ScanList1 = "";
    std::string CTRLList1 = "";
    std::string SearchList2 = "";
    std::string ScanList2 = "";
    std::string CTRLList2 = "";
    std::string SearchList3 = "";
    std::string ScanList3 = "";
    std::string CTRLList3 = "";
    std::string SearchList4 = "";
    std::string ScanList4 = "";
    std::string CTRLList4 = "";
    std::string SearchList5 = "";
    std::string ScanList5 = "";
    std::string CTRLList5 = "";
    std::string SearchList6 = "";
    std::string ScanList6 = "";
    std::string CTRLList6 = "";
    std::string SearchList7 = "";
    std::string ScanList7 = "";
    std::string CTRLList7 = "";
    std::string SearchList8 = "";
    std::string ScanList8 = "";
    std::string CTRLList8 = "";

    bool UpdateSrchListForBotton[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateScanListForBotton[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateCTRLListForBotton[MAX_SERVERS] = {false, false, false, false, false, false, false, false};

    double serversFreq[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};

    int UpdateMenuSnd0Main[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    int UpdateMenuRcv0Main[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool changeGain = false;
    int lnaGain = 0;
    int vgaGain = 0;
    int mixerGain = 0;
    int linearGain = 0;
    int sensitiveGain = 0;
    int gainMode = 0;
    bool lnaAgc = 0;
    bool mixerAgc = 0;
    bool select = 0;
    bool _updateLinearGain = 0;
    bool UpdateMenuSnd = false;
    bool UpdateMenuRcv = false;
    // RADIO
    int selectedDemodID = 1;
    int snapInterval = 0;
    int snapIntervalId = 0;
    int deempId = 0;
    bool nbEnabled = false;
    float nbLevel = 10.0f;
    int baseband_band = 100000;
    bool squelchEnabled = false;
    float squelchLevel = 0;
    bool FMIFNREnabled = false;
    int fmIFPresetId = 0;

    bool _lowPass = true;
    bool _highPass = false;
    float agcAttack = 50.0f;
    float agcDecay = 5.0f;
    bool carrierAgc = false;
    int tone = 800;
    float bandwidth = 10000;
    float sbandwidth = 125000;
    bool UpdateMenuSndRadio = false;
    bool UpdateMenuRcvRadio = false;
    bool UpdateMODRadio = false;
    bool UpdateFreq = false;
    // RECORD
    bool curr_recording = false;
    bool recording[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateMenuSndRecord = false;
    bool UpdateMenuRcvRecord = false;

    // SEARCH
    void *bbuf_srch = NULL;
    int sizeOfbbuf_srch = 0;
    void *bbuf_srch_stat = NULL;
    int sizeOfbbuf_srch_stat = 0;

    bool updateLists_srch = false;
    bool updateLists_srch_stat = false;

    bool button_srch[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    int idOfList_srch[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    int selectedLogicId[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    int selectedSrchMode[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    int LevelDbSrch[MAX_SERVERS] = {-50, -50, -50, -50, -50, -50, -50, -50};
    int SNRLevelDb[MAX_SERVERS] = {8, 8, 8, 8, 8, 8, 8, 8};
    bool AutoLevel_srch[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool AutoLevel_scan[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool AutoLevel_ctrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool AKFInd[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool AKFInd_ctrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateModule_srch[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool updateListRcv5Srch[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateMenuSndSearch[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateMenuRcvSearch = false;

    int SelectedMode[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    // SEARCH STAT
    bool UpdateStatSndSearch = false;
    bool UpdateStatRcvSearch = false;

    // SCAN
    void *bbuf_scan = NULL;
    int sizeOfbbuf_scan = 0;
    bool updateLists_scan = false;
    int idOfList_scan[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    int maxRecWaitTime_scan[MAX_SERVERS] = {10, 10, 10, 10, 10, 10, 10, 10};
    int maxRecDuration_scan[MAX_SERVERS] = {10, 10, 10, 10, 10, 10, 10, 10};
    int LevelDbScan[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};

    bool button_scan[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateModule_scan[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool updateListRcv6Scan[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateMenuSndScan[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateMenuRcvScan = false;
    // CONTROL
    void *bbuf_ctrl = NULL;
    bool FirstStart_ctrl[MAX_SERVERS] = {true, true, true, true, true, true, true, true};
    int maxRecWaitTime_ctrl[MAX_SERVERS] = {10, 10, 10, 10, 10, 10, 10, 10};
    bool flag_level_ctrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    int LevelDbCtrl[MAX_SERVERS] = {-100, -100, -100, -100, -100, -100, -100, -100};
    // int level_ctrl[MAX_SERVERS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    bool updateLists_ctrl = false;
    int idOfList_ctrl[MAX_SERVERS] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool button_ctrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool start_ctrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateModule_ctrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool updateListRcv7Ctrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    int sizeOfbbuf_ctrl = 0;
    bool UpdateMenuSndCtrl[MAX_SERVERS] = {false, false, false, false, false, false, false, false};
    bool UpdateMenuRcvCtrl = false;
    double serverSampleRate[MAX_SERVERS] = {MAIN_SAMPLE_RATE, MAIN_SAMPLE_RATE, MAIN_SAMPLE_RATE, MAIN_SAMPLE_RATE, MAIN_SAMPLE_RATE, MAIN_SAMPLE_RATE, MAIN_SAMPLE_RATE, MAIN_SAMPLE_RATE};
    bool suppressSinkUI = false;
};