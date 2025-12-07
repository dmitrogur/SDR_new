#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <gui/widgets/folder_select.h>
#include <gui/file_dialogs.h>
#include <recorder_interface.h>
#include <dsp/routing/splitter.h>
#include <dsp/audio/volume.h>
#include <dsp/bench/peak_level_meter.h>
#include "../../decoder_modules/radio/src/radio_interface.h"
#include <gui/menus/source.h>
#include <regex>
#include <core.h>
#include <ctime>
#include <chrono>
#include <gui/dialogs/dialog_box.h>
#include <utils/freq_formatting.h>
#include <cmath>
#include <iomanip>
#include <unistd.h>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <numeric>

namespace fs = std::filesystem;

// --- Constants ---
constexpr bool STEP = true;
constexpr bool SCAN_UP = true;
constexpr int NUM_MOD = 2;
constexpr int MAX_SNR_LEVEL = 20;
constexpr int COUNT_FOR_REFIND_SKIP = 10;
constexpr int INTERVAL_FOR_FIND_THRESHOLD_MIN = 15;
constexpr int maxSNRLevel = 20;
constexpr int SIGNAL_LOST_HYSTERESIS_COUNT = 10;
constexpr int SIGNAL_RETURN_HYSTERESIS_COUNT = 2;
// constexpr int MAX_COUNT_OF_DATA = 256;

struct FindedFreq
{
    double frequency;
    int level;
    bool selected;
};

struct SearchMode
{
    std::string listName;
    int _mode;
    double _bandwidth;
    double _startFreq;
    double _stopFreq;
    double _interval;
    double _passbandRatio;
    int _tuningTime;   //  = 350;
    bool _status_stop; // = false;
    int _waitingTime;  //  = 1000
    bool _status_record;
    int _lingerTime;    //
    bool _status_ignor; //
    int _level;         //  = -50.0
    bool selected;
    bool _status_direction;
    int _selectedLogicId;
    int _selectedSrchMode = 0;
};

struct SearchModeList
{
    char listName[32];
    int _mode;
    double _bandwidth;
    double _startFreq;
    double _stopFreq;
    double _interval;
    double _passbandRatio;
    int _tuningTime;   //  = 350;
    bool _status_stop; // = false;
    int _waitingTime;  //  = 1000
    bool _status_record;
    int _lingerTime;    //
    bool _status_ignor; //
    int _level;         //  = -50.0
    bool selected;
    bool _status_direction;
    int _selectedLogicId;
    int _selectedSrchMode;
};

SDRPP_MOD_INFO{
    /* Name:            */ "scanner2",
    /* Description:     */ "Frequency scanner2 for Aster",
    /* Author:          */ "DMH",
    /* Version:         */ 0, 3, 0, // Version bump for refactoring
    /* Max instances    */ 1};

ConfigManager config;

class ScannerModule2 : public ModuleManager::Instance
{
public:
    // --- State Machine ---
    enum class State
    {
        STOPPED,
        STARTING,
        TUNING,
        LEVEL_CALIBRATION,
        SEARCHING,
        RECEIVING,
        WAITING_FOR_AKF,
        LINGERING
    };

    // ВАШ КОНСТРУКТОР ОСТАЕТСЯ ЗДЕСЬ С МИНИМАЛЬНЫМИ ИЗМЕНЕНИЯМИ
    ScannerModule2(std::string name)
    {
        this->name = name;
        state.store(State::STOPPED);
        running.store(false);

        bool update_conf = false;
        core::configManager.acquire();
        if (core::configManager.conf.contains("USE_curl"))
        {
            use_curl = core::configManager.conf["USE_curl"];
        }
        else
        {
            use_curl = true;
            update_conf = true;
            core::configManager.conf["USE_curl"] = use_curl;
        }
        flog::info("ScannerModule2:  use_curl {0}", use_curl);

        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"];
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            radioMode = 0;
        }

        thisURL = core::configManager.conf["Url"];
        thisInstance = core::configManager.conf["InstanceName"];
        thisInstance = thisInstance + "-2";
        int numInstance = 0;
        try
        {
            numInstance = core::configManager.conf["InstanceNum"];
        }
        catch (const std::exception &e)
        {
            numInstance = 0;
        }
        try
        {
            Admin = core::configManager.conf["Admin"];
        }
        catch (const std::exception &e)
        {
            Admin = false;
        }

        try
        {
            maxRecDuration = core::configManager.conf["maxRecDuration"];
        }
        catch (const std::exception &e)
        {
            maxRecDuration = 1;
        }
        maxRecDuration = maxRecDuration * 60;
        try
        {
            maxCountInScanBank = core::configManager.conf["maxCountInScanBank"];
        }
        catch (const std::exception &e)
        {
            maxCountInScanBank = 0;
        }
        try
        {
            maxRecShortDur_sec = core::configManager.conf["maxRecShortDur_sec"];
        }
        catch (const std::exception &e)
        {
            maxRecShortDur_sec = 2;
            // core::configManager.conf["maxRecShortDur_sec"] = 3;
        }
        WAIT_MS = (maxRecShortDur_sec) * 1000 + 500;
        MAX_WAIT_MS = (maxRecShortDur_sec + 2) * 1000 + 200;

        try
        {
            if (core::configManager.conf["IsServer"] == true)
                isServer = true;
            else
                isServer = false;

            if (core::configManager.conf["IsARM"] == true)
                isARM = true;
            else
                isARM = false;
        }
        catch (const std::exception &e)
        {
            isServer = false;
            isARM = false;
            std::cerr << e.what() << '\n';
        }
        if (core::configManager.conf.contains("SignalIndf") && !core::configManager.conf["SignalIndf"].is_null())
        {
            // Значение существует и не null
            if (core::configManager.conf["SignalIndf"] == true)
                SignalIndf = true;
            else
                SignalIndf = false;
        }
        else
        {
            SignalIndf = false;
            core::configManager.conf["SignalIndf"] = false;
            update_conf = true;
        }
        core::configManager.release(update_conf);

        flog::info("1. radioMode {0}, maxRecDuration {1}, SignalIndf {2}  ", radioMode, maxRecDuration, SignalIndf);

        if (radioMode == 2)
        {
            // not registerEntry
            return;
        }

        update_conf = false;
        config.acquire();
        std::string selList = config.conf["selectedList"];
        try
        {
            status_AKF = config.conf["status_AKF"];
            status_auto_level = config.conf["status_auto_level"];
        }
        catch (const std::exception &e)
        {
            status_auto_level = false;
            status_AKF = false;
            update_conf = true;
            std::cerr << e.what() << '\n';
        }
        if (SignalIndf == false)
            status_AKF = false;

        if (update_conf)
        {
            config.conf["status_AKF"] = status_AKF;
            config.conf["status_auto_level"] = status_auto_level;
        }

        if (core::configManager.conf.contains("_level") && !core::configManager.conf["_level"].is_null())
        {
            intLevel = config.conf["_level"];
        }
        else
        {
            intLevel = -50;
        }

        if (core::configManager.conf.contains("SNRlevel") && !core::configManager.conf["SNRlevel"].is_null())
        {
            snr_level = config.conf["SNRlevel"];
        }
        else
        {
            snr_level = 9;
            config.conf["SNRlevel"] = snr_level;
            update_conf = true;
        }
        config.release(update_conf);

        flog::info("selList {0}, snr_level {1}  ", selList, snr_level);
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        root = (std::string)core::args["root"];
        {
            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
            gui::waterfall.finded_freq.clear();
        }
        listmaxLevel.clear();

        maxCountInScanBank = 256;

        logicList.clear();
        logicListTxt = "";
        logicList.push_back("Налаштування");
        logicListTxt += "Налаштування";
        logicListTxt += '\0';
        logicList.push_back("Звичайний пошук");
        logicListTxt += "Звичайний пошук";
        logicListTxt += '\0';
        logicList.push_back("Адаптивний  пошук");
        logicListTxt += "Адаптивний  пошук";
        logicListTxt += '\0';

        srchModeList.clear();
        srchModeListTxt = "";
        srchModeList.push_back("Користувацький");
        srchModeListTxt += "Користувацький";
        srchModeListTxt += '\0';
        srchModeList.push_back("АМ Аналог");
        srchModeListTxt += "АМ Аналог";
        srchModeListTxt += '\0';
        srchModeList.push_back("ЧМ Аналог");
        srchModeListTxt += "ЧМ Аналог";
        srchModeListTxt += '\0';
        srchModeList.push_back("DMR");
        srchModeListTxt += "DMR";
        srchModeListTxt += '\0';
        srchModeList.push_back("FM Радио");
        srchModeListTxt += "FM Радио";
        srchModeListTxt += '\0';
        // srchModeList.push_back("Tetra");
        // srchModeListTxt += "Tetra";
        // srchModeListTxt += '\0';

        // 2,5; 5; 10; 12,5; 20; 25; 30 или 50 кГц.
        snapintervalsList.clear();
        intervalsListTxt = "";
        snapintervalsList.push_back(1000);
        intervalsListTxt += "1";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(2500);
        intervalsListTxt += "2.5";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(5000);
        intervalsListTxt += "5";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(6250);
        intervalsListTxt += "6.25";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(10000);
        intervalsListTxt += "10";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(12500);
        intervalsListTxt += "12.5";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(20000);
        intervalsListTxt += "20";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(25000);
        intervalsListTxt += "25";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(30000);
        intervalsListTxt += "30";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(50000);
        intervalsListTxt += "50";
        intervalsListTxt += '\0';

        snapintervalsList.push_back(100000);
        intervalsListTxt += "100";
        intervalsListTxt += '\0';

        selectedIntervalId = 0;
        for (int i = 0; i < snapintervalsList.size(); i++)
        {
            if (snapintervalsList[i] == snapInterval)
            {
                selectedIntervalId = i;
                break;
            }
        }

        maxLevel = 0;
        bandwidthsList.clear();
        bandwidthsList.push_back(1000);
        bandwidthsList.push_back(2700);
        bandwidthsList.push_back(4000);
        bandwidthsList.push_back(6250);
        bandwidthsList.push_back(12500);
        bandwidthsList.push_back(25000);
        bandwidthsList.push_back(50000);
        bandwidthsList.push_back(100000);
        bandwidthsList.push_back(220000);
        bandwidthsList.push_back(250000);
        refreshLists();

        // SearchModeList fbm;
        // gui::mainWindow.setSizeofSearchModeList(sizeof(fbm));
        loadByName(selList, false);
        flog::info("OK ");
    }

    ~ScannerModule2()
    {
        stop();
        gui::menu.removeEntry(name);
        if (workerThread.joinable())
        {
            workerThread.join();
        }
        if (workerInfoThread.joinable())
        {
            workerInfoThread.join();
        }
    }

    void postInit() override
    {
        currSource = sourcemenu::getCurrSource(); //  sigpath::sourceManager::getCurrSource();

        if (currSource == SOURCE_ARM)
            isARM = true;
        else
            isARM = false;

        if (isARM)
        {
            getSearchLists();
            gui::mainWindow.setidOfList_srch(gui::mainWindow.getCurrServer(), selectedListId);
            gui::mainWindow.setSearchListNamesTxt(listNamesTxt);
            gui::mainWindow.setAuto_levelSrch(MAX_SERVERS, status_auto_level);
            gui::mainWindow.setLevelDbSrch(MAX_SERVERS, intLevel);
            gui::mainWindow.setAKFInd(MAX_SERVERS, status_AKF);
            gui::mainWindow.setSNRLevelDb(MAX_SERVERS, snr_level);

            gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
            gui::mainWindow.setUpdateSrchListForBotton(gui::mainWindow.getCurrServer(), true);
        }
        else
        {
            getSearchLists();
            gui::mainWindow.setUpdateModule_srch(0, false);
            gui::mainWindow.setLevelDbSrch(0, intLevel);
        }

        if (isARM || isServer)
            workerInfoThread = std::thread(&ScannerModule2::workerInfo, this);
        flog::info("\n Search. currSource {0}, isARM {1}, isServer {2}, status_auto_level {3}, snr_level {4}", currSource, isARM, isServer, status_auto_level, snr_level);
    }

    void enable() override { enabled = true; }
    void disable() override { enabled = false; }
    bool isEnabled() override { return enabled; }

private:
    // --- AFC Configuration ---
    struct AfcConfig
    {
        bool enabled = true;             // Включено по умолчанию (можно вывести в GUI)
        double searchSpanHz = 4000.0;    // Ищем пик в окне +/- 2 кГц (для NFM/DMR с головой)
        double deadZoneHz = 50.0;        // Мертвая зона (не дрыгаемся из-за мелочей)
        double maxCorrectionHz = 1800.0; // Максимальный прыжок за раз
    } afc;

    // Хелпер для округления (из моего варианта, полезно для красоты)
    double roundFreq(double freq, double step) const
    {
        return std::round(freq / step) * step;
    }

    void applyPreset(int presetId)
    {
        flog::info("TRACE. Apply Preset: {0}", presetId);
        bool updt = false;

        switch (presetId)
        {
        case 1:                  // АМ Voice (Авиа / СВ)
            mode = 2;            // AM
            _bandwidthId = 3;    // ~6000-8000 Hz (обычный AM)
            snapInterval = 5000; // Было 1000. 5кГц - стандартный шаг для быстрого поиска.
            passbandRatio = 40;  // Было 15. Смотрим шире, чтобы не пропустить неточную настройку.
            tuningTime = 200;    // Было 100. Даем время на стабилизацию PLL.
            updt = true;
            break;

        case 2:                   // FM Voice (Аналог, такси, жд)
            mode = 0;             // NFM
            _bandwidthId = 3;     // ~12500 Hz
            snapInterval = 12500; // Было 6250. 12.5кГц - стандарт. 6.25 нужно только для PMR.
            passbandRatio = 50;   // Берем центральные 50% канала.
            tuningTime = 200;
            updt = true;
            break;

        case 3:                   // DMR (Цифра) - ВАЖНЫЕ ИЗМЕНЕНИЯ
            mode = 0;             // NFM (SDR++ декодирует DMR через NFM демодулятор)
            _bandwidthId = 4;     // ~12500 Hz (Обязательно, DMR шире узкого NFM)
            snapInterval = 12500; // Сетка жесткая 12.5 кГц

            // PassbandRatio: Увеличиваем до 40-50%.
            // При 10% (было) малейшее отклонение частоты давало просадку уровня.
            passbandRatio = 40;

            // TuningTime: Увеличиваем до 250 мс.
            // При прыжке частоты (Look-ahead) нужно полностью сбросить буфер,
            // иначе сканер увидит "хвост" сигнала с прошлой частоты.
            tuningTime = 250;
            updt = true;
            break;

        case 4:                    // FM Radio (Вещательные)
            mode = 1;              // WFM
            _bandwidthId = 8;      // ~200000 Hz
            snapInterval = 100000; // 100 кГц
            passbandRatio = 50;    // Смотрим центр
            tuningTime = 150;      // WFM мощный, его видно сразу, можно быстрее.
            updt = true;
            break;
        }

        if (updt)
        {
            // Защита от вылета индекса массива
            if (_bandwidthId >= bandwidthsList.size())
                _bandwidthId = bandwidthsList.size() - 1;

            bandwidth = bandwidthsList[_bandwidthId];

            // Важно: Сначала режим, потом полоса
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SNAPINTERVAL, &snapInterval, NULL);

            double bw = (double)bandwidth;
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &bw, NULL);

            flog::info("Preset Applied: Mode={0}, BW={1}, Snap={2}, TuneTime={3}", mode, bandwidth, snapInterval, tuningTime);
        }
    }

    void applyPreset2(int presetId)
    {
        flog::info("TRACE. _this->selectedSrchMode = {0}!", presetId);
        bool updt = false;

        switch (presetId)
        {
        case 1:               // АМ Voice
            mode = 2;         // AM
            _bandwidthId = 3; // 6250
            snapInterval = 1000;
            passbandRatio = 15;
            tuningTime = 100;
            updt = true;
            break;
        case 2:               // FM Voice
            mode = 0;         // ЧМ
            _bandwidthId = 3; // 12500
            snapInterval = 6250;
            passbandRatio = 15;
            tuningTime = 200;
            updt = true;
            break;
        case 3:               // DMR
            mode = 0;         // ЧМ
            _bandwidthId = 4; // 12500
            snapInterval = 12500;
            passbandRatio = 10;
            tuningTime = 200;
            updt = true;
            break;
        case 4:               // FM Radio
            mode = 1;         // FM
            _bandwidthId = 8; // 220000
            snapInterval = 100000;
            passbandRatio = 20;
            tuningTime = 200;
            updt = true;
            break;
        }

        if (updt)
        {
            bandwidth = bandwidthsList[_bandwidthId];
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SNAPINTERVAL, &snapInterval, NULL);
            double bw = (double)bandwidth;
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &bw, NULL);
        }
    }

    void start()
    {
        if (isARM)
            return;
        if (running.load())
            return;

        if (workerThread.joinable())
        {
            workerThread.join();
        }

        core::modComManager.callInterface("Запис", MAIN_SET_START, NULL, NULL);

        flog::info("Manual START command issued.");
        // Запоминаем время, чтобы workerInfo временно не вмешивался
        last_manual_command_time = std::chrono::steady_clock::now();

        initial_find_level = true;
        skip_for_relevel_counter = 0;
        akf_confirmation_time = std::chrono::high_resolution_clock::now() - std::chrono::hours(1);
        core::modComManager.callInterface("Airspy", 0, NULL, &_air_recording);
        flog::info("AIR Recording is '{0}', selectedListName {1}", _air_recording, selectedListName);
        if (_air_recording == 0)
        {
            return;
        }
        firstEditedListName = listNames[selectedListId];
        editedListName = firstEditedListName;
        onlySaveListOpen = startListSave();

        flog::info("START selectedLogicId {0}", selectedLogicId);

        core::configManager.acquire();
        try
        {
            radioMode = (int)core::configManager.conf["RadioMode"];
        }
        catch (const std::exception &e)
        {
            radioMode = 0;
        }
        // bool showWaterfall = core::configManager.conf["showWaterfall"];
        core::configManager.release();

        {
            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
            gui::waterfall.finded_freq.clear();
        }
        skipNoise.clear();
        /*
        current = startFreq; // Set initial frequency
        last_current = current;
        if (mode == 1 && selectedLogicId == 2)
            selectedIntervalId = 7;

        calculateScanSegment(startFreq, stopFreq, scan_band, sigmentLeft, sigmentRight);
        initial_find_level = true;

        applyPreset(selectedSrchMode);
        gui::mainWindow.settuningMode(tuner::TUNER_MODE_NORMAL);
        tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, startFreq);
        gui::waterfall.VFOMoveSingleClick = false;
        */
        current = startFreq; // Set initial frequency
        last_current = current;
        if (mode == 1 && selectedLogicId == 2)
            selectedIntervalId = 7;

        calculateScanSegment(startFreq, stopFreq, scan_band, sigmentLeft, sigmentRight);
        initial_find_level = true;

        applyPreset(selectedSrchMode);
        gui::mainWindow.settuningMode(tuner::TUNER_MODE_NORMAL);

        // --- ТЮНИМ НЕ НА startFreq, А НА ЦЕНТР, СДВИНУТЫЙ НА 25% ШИРИНЫ ---
        double wfWidth = gui::waterfall.getViewBandwidth(); // видимая ширина спектра
        double centerFreq = startFreq + wfWidth * 0.15;     // startFreq на 25% слева

        tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, centerFreq);

        gui::waterfall.VFOMoveSingleClick = false;
        // int tuningMode = gui::mainWindow.gettuningMode();
        // tuner::tune(tuningMode, gui::waterfall.selectedVFO, startFreq);
        gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
        flog::info("TRACE.       startFreq = {0}, stopFreq = {1}, current {2}, snapInterval {3}", startFreq, stopFreq, current, snapInterval);

        // --- Start the worker ---
        running.store(true);
        state.store(State::STARTING);
        gui::waterfall.scan2_running = true;

        workerThread = std::thread(&ScannerModule2::worker, this);
    }

    void stop()
    {
        if (!running.load())
            return;
        flog::info("Manual STOP command issued.");
        // Запоминаем время, чтобы workerInfo временно не вмешивался
        last_manual_command_time = std::chrono::steady_clock::now();

        running.store(false);
        /*
        if (workerThread.joinable())
        {
            workerThread.join();
        }
        */
        // Ensure state is clean after stopping
        state.store(State::STOPPED);

        if (_recording.load())
        {
            stopRecording();
        }

        gui::waterfall.scan2_running = false;
        SaveInJsonSrch();
        core::modComManager.callInterface("Запис", MAIN_SET_STOP, NULL, NULL);
    }

    void worker()
    {
        auto init_level_time = std::chrono::high_resolution_clock::now();
        int skip_for_relevel_counter = 0;
        bool timer_started = false;
        std::chrono::steady_clock::time_point trigger_time;

        auto last_cleanup_time = std::chrono::steady_clock::now();
        const auto cleanup_interval = std::chrono::seconds(180);

        while (running.load())
        {
            // 1. Пауза, чтобы не грузить CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(75));

            auto now_steady = std::chrono::steady_clock::now();
            auto now = std::chrono::high_resolution_clock::now();

            State st = state.load();

            // --- Логика перекалибровки (Auto Level) ---
            if (st == State::SEARCHING || st == State::TUNING)
            {
                bool local_status_auto_level;
                {
                    std::lock_guard<std::mutex> lock(paramMtx);
                    local_status_auto_level = status_auto_level;
                }

                if (local_status_auto_level)
                {
                    if (skip_for_relevel_counter >= COUNT_FOR_REFIND_SKIP)
                    {
                        initial_find_level = true;
                    }
                    bool new_gain = gui::mainWindow.getChangeGain();
                    if (new_gain)
                    {
                        gui::mainWindow.setChangeGainFalse();
                        trigger_time = now_steady + std::chrono::seconds(1);
                        timer_started = true;
                    }
                    if (timer_started && now_steady >= trigger_time)
                    {
                        initial_find_level = true;
                        timer_started = false;
                    }
                    if ((now - init_level_time) > std::chrono::minutes(INTERVAL_FOR_FIND_THRESHOLD_MIN))
                    {
                        initial_find_level = true;
                    }
                }
            }

            // --- Очистка ---
            if ((now_steady - last_cleanup_time) > cleanup_interval)
            {
                cleanupSkipNoise();
                last_cleanup_time = now_steady;
            }

            // 2. АВТО-ТЮНИНГ (Строгое ограничение состояний)
            // Исключили TUNING и LEVEL_CALIBRATION. Двигаем VFO только когда ищем или принимаем.
            if (st == State::SEARCHING || st == State::RECEIVING || st == State::LINGERING || st == State::WAITING_FOR_AKF)
            {
                std::string currentSelectedVFO = gui::waterfall.selectedVFO;
                if (!currentSelectedVFO.empty() && gui::waterfall.vfos.count(currentSelectedVFO))
                {
                    double vfo_freq = gui::waterfall.getCenterFrequency() + gui::waterfall.vfos.at(currentSelectedVFO)->generalOffset;

                    if (std::abs(vfo_freq - current) > 100.0)
                    {
                        tuner::normalTuning(currentSelectedVFO, current);
                    }
                }
            }

            // --- Получение данных FFT ---
            int dataWidth = 0;
            float *data = gui::waterfall.acquireLatestFFT(dataWidth);
            if (!data)
            {
                continue;
            }

            // --- Переход в калибровку ---
            if (initial_find_level && st != State::LEVEL_CALIBRATION)
            {
                state.store(State::LEVEL_CALIBRATION);
                st = State::LEVEL_CALIBRATION;
            }

            // --- State Machine ---
            switch (st)
            {
            case State::STOPPED:
                running.store(false);
                break;

            case State::STARTING:
                tuner::normalTuning(gui::waterfall.selectedVFO, current);
                lastTuneTime = std::chrono::high_resolution_clock::now();
                state.store(State::TUNING);
                break;

            case State::TUNING:
                handleTuning(now);
                break;

            case State::LEVEL_CALIBRATION:
                if (!_recording.load())
                {
                    handleLevelCalibration(data, dataWidth);
                    init_level_time = now;
                    state.store(State::SEARCHING);
                }
                break;

            case State::SEARCHING:
                handleSearching(data, dataWidth, now);
                // Дубль проверки таймера (можно убрать, если есть выше, но не мешает)
                {
                    std::lock_guard<std::mutex> lock(paramMtx);
                    if (status_auto_level && (now - init_level_time) > std::chrono::minutes(INTERVAL_FOR_FIND_THRESHOLD_MIN))
                    {
                        state.store(State::LEVEL_CALIBRATION);
                    }
                }
                break;

            case State::RECEIVING:
                handleReceiving(data, dataWidth, now);
                break;

            case State::WAITING_FOR_AKF:
                handleWaitingForAKF(data, dataWidth, now);
                break;

            case State::LINGERING:
                handleLingering(data, dataWidth, now);
                break;
            }

            gui::waterfall.releaseLatestFFT();
        }
    }

    // =================================================================================================
    // STATE HANDLERS
    // =================================================================================================

    void handleTuning(const std::chrono::time_point<std::chrono::high_resolution_clock> &now)
    {
        // --- Читаем параметр времени тюнинга потокобезопасно ---
        int localTuningTime;
        bool needs_calibration;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localTuningTime = tuningTime;
            // Проверяем, нужна ли калибровка, ТОЛЬКО если автоуровень включен
            // и флаг initial_find_level взведен (например, при первом старте)
            needs_calibration = status_auto_level && initial_find_level;
        }

        if ((now - lastTuneTime) > std::chrono::milliseconds(localTuningTime))
        {
            if (needs_calibration)
            {
                state.store(State::LEVEL_CALIBRATION);
            }
            else
            {
                state.store(State::SEARCHING);
            }
        }
    }

    void handleLevelCalibration(float *data, int dataWidth)
    {
        if (status_auto_level)
        {
            flog::info("Auto level calibrating ...");
            float signalThreshold;
            {
                std::lock_guard<std::mutex> lock(paramMtx);
                double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
                double wfWidth = gui::waterfall.getViewBandwidth();
                double wfStart = wfCenter - (wfWidth / 2.0);

                int numSegments = static_cast<int>((sigmentRight - sigmentLeft) / scan_band);
                signalThreshold = scanRange(numSegments, data, dataWidth, wfStart, wfWidth, snr_level);
            }

            {
                std::lock_guard<std::mutex> lock(paramMtx);
                intLevel = static_cast<int>(signalThreshold);
                Averaging = signalThreshold - snr_level;
            }
            flog::info("Auto level calibrated. New threshold: {0} dB", intLevel);
        }

        // --- ВАЖНО: Сбрасываем флаг, иначе зациклимся ---
        initial_find_level = false;

        skip_for_relevel_counter = 0;
        if (gui::mainWindow.getChangeGain())
        {
            gui::mainWindow.setChangeGainFalse();
        }

        init_level_time = std::chrono::high_resolution_clock::now();

        // Возвращаемся к поиску
        state.store(State::SEARCHING);

        gui::mainWindow.setLevelDbSrch(CurrSrvr, intLevel);
        gui::mainWindow.setUpdateMenuSnd5Srch(CurrSrvr, true);
    }

    // --- НОВАЯ ФУНКЦИЯ: Поиск точного пика в пределах полосы ---
    double findLocalPeakFrequency(const float *data, int dataWidth, double wfStart, double wfWidth, double approxFreq, double searchSpanHz)
    {
        if (!data || dataWidth <= 0 || wfWidth <= 0.0 || searchSpanHz <= 0.0)
            return approxFreq;

        const double binWidth = wfWidth / static_cast<double>(dataWidth);
        if (binWidth <= 0.000001)
            return approxFreq; // Защита

        double lowFreq = approxFreq - searchSpanHz * 0.5;
        double highFreq = approxFreq + searchSpanHz * 0.5;

        // Перевод частот в индексы массива
        int lowId = std::clamp<int>(
            static_cast<int>(std::floor((lowFreq - wfStart) / binWidth)),
            0, dataWidth - 1);

        int highId = std::clamp<int>(
            static_cast<int>(std::ceil((highFreq - wfStart) / binWidth)),
            0, dataWidth - 1);

        if (lowId > highId)
            std::swap(lowId, highId);

        float maxVal = -10000.0f; // Используем заведомо низкое значение
        int maxIdx = -1;

        for (int i = lowId; i <= highId; ++i)
        {
            if (data[i] > maxVal)
            {
                maxVal = data[i];
                maxIdx = i;
            }
        }

        if (maxIdx < 0)
            return approxFreq;

        // Возвращаем точную частоту центра бина
        double peakFreq = wfStart + (static_cast<double>(maxIdx) + 0.5) * binWidth;
        return peakFreq;
    }

    void handleSearching(float *data, int dataWidth, const std::chrono::time_point<std::chrono::high_resolution_clock> &now)
    {
        // 1. Логирование
        static std::chrono::time_point<std::chrono::steady_clock> last_log_time;
        if ((std::chrono::steady_clock::now() - last_log_time) > std::chrono::seconds(1))
        {
            flog::info("Searching... current frequency: {0} MHz", utils::formatFreqMHz(current));
            last_log_time = std::chrono::steady_clock::now();
        }

        // 2. Поиск
        double bottomLimit = current, topLimit = current;

        if (findSignal(SCAN_UP, bottomLimit, topLimit, data, dataWidth))
        {
            // === Пред-фильтр ===
            const double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
            const double wfWidth = gui::waterfall.getViewBandwidth();
            const double wfStart = wfCenter - (wfWidth / 2.0);
            const double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);

            int localIntLevel;
            {
                std::lock_guard<std::mutex> lock(paramMtx);
                localIntLevel = intLevel;
            }

            float preciseLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);

            if (preciseLevel < (float)localIntLevel)
            {
                skipNoise[current] = std::chrono::steady_clock::now();
                return;
            }

            // === AFC ===
            if (afc.enabled)
            {
                double localSnapInterval;
                {
                    std::lock_guard<std::mutex> lock(paramMtx);
                    localSnapInterval = snapInterval;
                }

                // 1. Ищем пик в окне (как раньше)
                double span = std::min(afc.searchSpanHz, localSnapInterval);
                double peakFreq = findLocalPeakFrequency(data, dataWidth, wfStart, wfWidth, current, span);

                // 2. Привязываем к сетке каналов (DMR 12.5 кГц / AM/FM – своя)
                double snappedFreq = std::round(peakFreq / localSnapInterval) * localSnapInterval;

                // 3. Считаем дельту уже от snappedFreq
                double deltaHz = snappedFreq - current;
                double absDeltaHz = std::abs(deltaHz);

                if (absDeltaHz > afc.deadZoneHz && absDeltaHz < afc.maxCorrectionHz)
                {
                    double correctedFreq = snappedFreq; // РОВНО по сетке, без хвостов
                    flog::info("AFC (search): Adjusted {0} -> {1} (Delta={2} Hz)",
                               utils::formatFreqMHz(current),
                               utils::formatFreqMHz(correctedFreq),
                               deltaHz);
                    current = correctedFreq;
                }
            }

            // === Старт ===
            flog::info("Signal LOCKED at {0}. RX.", utils::formatFreqMHz(current));
            firstSignalTime = std::chrono::high_resolution_clock::now();
            lastSignalTime = firstSignalTime;
            signal_lost_counter = 0;
            signal_returned_counter = 0;

            startRecording();

            if (state.load() == State::SEARCHING)
                state.store(State::RECEIVING);
            return;
        }

        // 3. Следующий шаг
        double localStartFreq, localStopFreq, localSnapInterval;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localStartFreq = startFreq;
            localStopFreq = stopFreq;
            localSnapInterval = snapInterval;
        }

        current = topLimit + localSnapInterval;
        if (current > localStopFreq)
            current = localStartFreq;

        // 4. Проверка границ
        const double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
        const double wfWidth = gui::waterfall.getViewBandwidth();

        if (wfWidth <= 0.0)
            return;

        const double wfStart = wfCenter - wfWidth / 2.0;
        const double wfEnd = wfCenter + wfWidth / 2.0;

        // Вылетели за экран полностью
        bool outOfBounds = (current < wfStart) || (current > wfEnd);

        // "Серая зона" — далеко от центра, но ещё на экране
        double distFromCenter = std::abs(current - wfCenter);
        double centerThreshold = wfWidth * 0.35; // 35% от ширины

        // Если уже в режиме TUNING — тут вообще ничего не делаем
        if (state.load() == State::TUNING)
            return;

        // Гистерезис по времени, чтобы не спамить переключениями
        static std::chrono::steady_clock::time_point lastRecenterTime{};
        auto nowSteady = std::chrono::steady_clock::now();

        // Не чаще одного раза в 500 мс
        if ((nowSteady - lastRecenterTime) < std::chrono::milliseconds(500))
            return;

        // Финальное условие: либо совсем ушли за экран, либо сильно сместились от центра
        if (outOfBounds || distFromCenter > centerThreshold)
        {
            lastRecenterTime = nowSteady;

            flog::info("Re-centering REQUEST. current={0} MHz, wf=[{1} .. {2}]",
                       utils::formatFreqMHz(current),
                       utils::formatFreqMHz(wfStart),
                       utils::formatFreqMHz(wfEnd));

            // НИКАКОГО tuner::normalTuning здесь нет.
            // Только перевод в TUNING, чтобы handleTuning/worker сами делали нужное.
            lastTuneTime = std::chrono::high_resolution_clock::now();
            state.store(State::TUNING);
        }
    }

    void handleReceiving(float *data, int dataWidth, const std::chrono::time_point<std::chrono::high_resolution_clock> &now)
    {
        // --- 1. Читаем параметры и уровень сигнала ---
        const double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
        // const double wfStart = gui::waterfall.getCenterFrequency() - (gui::waterfall.getViewBandwidth() / 2.0);
        // const double wfWidth = gui::waterfall.getViewBandwidth();
        // maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);

        const double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
        const double wfWidth = gui::waterfall.getViewBandwidth();
        const double wfStart = wfCenter - (wfWidth / 2.0);

        maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);

        // --- END AFC ---
        int localIntLevel, localLingerTime;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localIntLevel = intLevel;
            localLingerTime = lingerTime;
        }

        // --- 2. Проверка на общий таймаут записи ---
        if ((now - firstSignalTime) > std::chrono::seconds(localLingerTime))
        {
            flog::info("Max dwell time ({0}s) reached for {1}. Resuming search.", localLingerTime, current);
            stopRecording();
            skipNoise[current] = std::chrono::steady_clock::now();
            state.store(State::SEARCHING);
            return;
        }

        // --- 3. Логика гистерезиса ---
        // Сначала проверяем, есть ли сигнал ВООБЩЕ (по обычному уровню)
        if (maxLevel >= localIntLevel)
        {
            // Сигнал есть! Сбрасываем счетчик потери.
            signal_lost_counter = 0;
            lastSignalTime = now;

            // --- AFC запускаем только если сигнал СИЛЬНЫЙ (hysteresis +3dB) ---
            if (afc.enabled && maxLevel > (localIntLevel + 3.0f))
            {
                // 1. Берём шаг сетки (для DMR: 12500 Гц)
                double localSnapInterval;
                {
                    std::lock_guard<std::mutex> lock(paramMtx);
                    localSnapInterval = snapInterval;
                }

                // 2. Ищем пик в окне вокруг current
                double span = std::min(afc.searchSpanHz, localSnapInterval);
                double peakFreq = findLocalPeakFrequency(
                    data, dataWidth,
                    wfStart, wfWidth,
                    current, span);

                // 3. Привязываем частоту к сетке каналов (DMR: кратно 12.5 кГц)
                double snappedFreq = std::round(peakFreq / localSnapInterval) * localSnapInterval;

                // 4. Считаем дельту уже от snappedFreq
                double deltaHz = snappedFreq - current;
                double absDeltaHz = std::abs(deltaHz);

                if (absDeltaHz > afc.deadZoneHz && absDeltaHz < afc.maxCorrectionHz)
                {
                    flog::info("AFC (RX): Adjusted {0} -> {1} (Delta={2} Hz)",
                               utils::formatFreqMHz(current),
                               utils::formatFreqMHz(snappedFreq),
                               deltaHz);

                    current = snappedFreq;
                }
            }
        }
        else
        {
            // Сигнала нет. Увеличиваем счетчик потери. `lastSignalTime` НЕ трогаем.
            signal_lost_counter++;
        }

        // --- 4. Переход в LINGERING, если сигнал пропал надолго ---
        if (signal_lost_counter >= SIGNAL_LOST_HYSTERESIS_COUNT)
        {
            flog::info("Signal at {0} has been consistently below threshold ({1} checks). Entering lingering state.", current, SIGNAL_LOST_HYSTERESIS_COUNT);

            // Сбрасываем счетчики для чистого старта в LINGERING.
            // `lastSignalTime` здесь НЕ трогаем. Он хранит правильное время.
            signal_lost_counter = 0;
            signal_returned_counter = 0;

            state.store(State::LINGERING);
        }
    }

    // handleLingering
    void handleLingering(float *data, int dataWidth, const std::chrono::time_point<std::chrono::high_resolution_clock> &now)
    {
        // --- 1. Читаем параметры ---
        int localWaitingTime, localIntLevel;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localWaitingTime = _waitingTime;
            localIntLevel = intLevel;
        }

        // --- 2. Получаем параметры водопада и проверяем сигнал ---
        const double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
        const double wfStart = gui::waterfall.getCenterFrequency() - (gui::waterfall.getViewBandwidth() / 2.0);
        const double wfWidth = gui::waterfall.getViewBandwidth();

        maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);

        if (maxLevel >= localIntLevel)
        {
            signal_returned_counter++;
        }
        else
        {
            signal_returned_counter = 0;
        }

        if (signal_returned_counter >= SIGNAL_RETURN_HYSTERESIS_COUNT)
        {
            flog::info("Signal at {0} has returned. Resuming reception.", current);
            lastSignalTime = now; // Важно обновить!
            state.store(State::RECEIVING);
            return;
        }

        // --- 3. Проверяем таймаут тишины ---
        if ((now - lastSignalTime) > std::chrono::seconds(localWaitingTime))
        {
            flog::info("Signal at {0} did not return within wait time ({1}s). Resuming search.", current, localWaitingTime);
            stopRecording();
            skipNoise[current] = std::chrono::steady_clock::now();
            state.store(State::SEARCHING);
        }
    }

    // Новая, исправленная версия
    void handleWaitingForAKF(float *data, int dataWidth, const std::chrono::time_point<std::chrono::high_resolution_clock> &now)
    {
        int cnt_ms = (std::chrono::duration_cast<std::chrono::milliseconds>(now - startTimeAKF)).count();

        if (cnt_ms <= (WAIT_MS + ADD_WAIT_MS))
        {
            return;
        }

        int _SIGNAL = -1;
        core::modComManager.callInterface("Запис", RECORDER_IFACE_GET_SIGNAL, NULL, &_SIGNAL);

        if (_SIGNAL != -1 || cnt_ms > MAX_WAIT_MS)
        {
            // --- Обработка результата: Шум или Таймаут ---
            if (_SIGNAL <= 0)
            {
                std::string reason = "NA";
                if (_SIGNAL == 0)
                {
                    reason = "NOISE";
                }
                else
                {
                    reason = "TIMEOUT/ERROR";
                }

                flog::info("AKF result for {0}: {1}. Adding to skip list and resuming search.", current, reason);

                // Удаляем ложное срабатывание и добавляем в список пропуска
                {
                    std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                    gui::waterfall.finded_freq.erase(current);
                }
                gui::mainWindow.setUpdateStatSnd5Srch(true);
                _count_Bookmark--;
                skipNoise[current] = std::chrono::steady_clock::now();
                flog::info("stopRecording. 1058");
                stopRecording();
                state.store(State::SEARCHING);
            }
            // --- Обработка результата: Сигнал подтвержден ---
            else
            {
                std::string info_signal = "HB";
                if (_SIGNAL == 1)
                {
                    info_signal = "VOICE";
                }
                else if (_SIGNAL == 2)
                {
                    info_signal = "DMR";
                }

                flog::info("AKF result for {0}: SIGNAL ({1}). Continuing to monitor.", current, info_signal);

                // Обновляем тип сигнала в списке найденных
                {
                    std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                    auto it = gui::waterfall.finded_freq.find(current);
                    if (it != gui::waterfall.finded_freq.end())
                    {
                        it->second.Signal = _SIGNAL;
                    }
                }
                gui::mainWindow.setUpdateStatSnd5Srch(true);

                // Потокобезопасно читаем параметр для CURL
                bool localStatusDirection;
                {
                    std::lock_guard<std::mutex> lock(paramMtx);
                    localStatusDirection = status_direction;
                }

                // Начинаем отправку по CURL
                /*
                if (localStatusDirection)
                {
                    curlPOST_begin(curr_nameWavFile);
                }
                */

                // Переходим к мониторингу
                state.store(State::RECEIVING);
            }
        }
        // --- Ответ еще не готов, увеличиваем ожидание ---
        else
        {
            ADD_WAIT_MS += 500;
        }
    }
    // =================================================================================================
    // HELPER FUNCTIONS
    // =================================================================================================
    // Новая, исправленная версия
    void startRecording()
    {
        // 1. Проверяем, не идет ли уже запись или не нужно ли пропустить частоту
        if (_recording.load())
        {
            return;
        }
        if (searchInIdentified(current))
        {
            flog::info("Frequency {0} is in skip list, not processing.", current);
            state.store(State::SEARCHING);
            return;
        }
        flog::info("startRecording current = {0}", current);
        // gui::mainWindow.settuningMode(tuner::TUNER_MODE_CENTER);
        // tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, current);
        // gui::waterfall.centerFreqMoved = true;

        // gui::waterfall.VFOMoveSingleClick = false;
        // tuner::centerTuning(gui::waterfall.selectedVFO, current);
        tuner::tune(gui::mainWindow.gettuningMode(), gui::waterfall.selectedVFO, current);
        /*
        tuner::centerTuning(gui::waterfall.selectedVFO, current);
        gui::waterfall.centerFreqMoved = true;
        int tuningMode = gui::mainWindow.gettuningMode();
        tuner::tune(tuningMode, gui::waterfall.selectedVFO, startFreq);
        int tuningMode = gui::mainWindow.gettuningMode();
        tuner::tune(tuningMode, gui::waterfall.selectedVFO, startFreq);
        */
        gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
        flog::info("TRACE. startRecording. current = {0}", current);
        gui::waterfall.centerFreqMoved = true;
        // 2. Считываем все нужные параметры за одну блокировку
        bool localStatusRecord, localStatusAKF, localStatusDirection;
        int localMode, localSelectedSrchMode;
        {
            flog::info("startRecording localSelectedSrchMode = {0}", selectedSrchMode);

            std::lock_guard<std::mutex> lock(paramMtx);
            localStatusRecord = status_record;
            localStatusAKF = status_AKF;
            localStatusDirection = status_direction;
            localMode = mode;
            localSelectedSrchMode = selectedSrchMode;
        }
        flog::info("startRecording _count_Bookmark = {0}", _count_Bookmark);
        // 3. Добавляем частоту в список найденных (закладки)
        _count_Bookmark++;
        {
            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
            gui::waterfall.addFreq.frequency = current;
            gui::waterfall.addFreq.level = static_cast<int>(maxLevel);
            gui::waterfall.addFreq.mode = localMode;
            gui::waterfall.addFreq.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
            gui::waterfall.addFreq.selected = false;
            gui::waterfall.addFreq.ftime = time(0);
            gui::waterfall.addFreq.Signal = 0; // 0 - пока не определен

            gui::waterfall.finded_freq[current] = gui::waterfall.addFreq;
        }
        gui::mainWindow.setUpdateStatSnd5Srch(true);
        flog::info("Found new signal at {0}. Added to temporary list.", current);

        // 4. Генерируем имя файла
        curr_nameWavFile = genWavFileName("$y$M$d-$u-$f-$b-$n-$e.wav", current, localMode);

        // 5. Начинаем запись, если она включена
        if (localStatusRecord)
        {
            int recMode = 1; // RECORDER_MODE_AUDIO;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_SET_MODE, &recMode, NULL);

            int typeSrch = localStatusAKF ? (localSelectedSrchMode + 1) : 0;
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_START_AKF, &typeSrch, NULL);

            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_START, (void *)curr_nameWavFile.c_str(), NULL);
            _recording.store(true);
            flog::info("Started recording for {0} to file {1}", current, curr_nameWavFile);
        }

        // 6. Принимаем решение о дальнейших действиях (АКФ или CURL)
        if (localStatusAKF)
        {
            // Если АКФ включен, переходим в состояние ожидания его ответа.
            // curlPOST_begin НЕ вызываем здесь.
            startTimeAKF = std::chrono::high_resolution_clock::now();
            ADD_WAIT_MS = 0;
            state.store(State::WAITING_FOR_AKF);
        }
        else
        {
            // Если АКФ выключен, но пеленгация включена, сразу отправляем событие о начале.
            /*
            if (localStatusDirection && !curr_nameWavFile.empty())
            {
                curlPOST_begin(curr_nameWavFile);
            }
            */
            // Состояние останется RECEIVING, так как startRecording не изменил его.
        }
    }

    // --- stopRecording ---
    void stopRecording()
    {
        // Теперь проверки с .load() и .store() будут работать
        if (!_recording.load()) //  && !Curl_send_begin.load()
        {
            return;
        }

        if (_recording.load())
        {
            _recording.store(false);
            core::modComManager.callInterface("Запис", RECORDER_IFACE_CMD_STOP, NULL, NULL);
            flog::info("Stopped recording for frequency: {0}", current);
        }
        /*
        bool was_sending = true;
        if (Curl_send_begin.compare_exchange_strong(was_sending, false))
        {
            flog::info("Recording/Event ended for {0}. Initiating CURL 'end' session.", current);

            std::string filename_to_end = curr_nameWavFile;

            if (!filename_to_end.empty())
            {
                curlPOST_end(filename_to_end);
            }
        }
        */
        flog::info("Stopped recording. OK");
        curr_nameWavFile = "";
    }

    bool findSignal(bool scanDir, double &bottomLimit, double &topLimit, const float *data, int dataWidth)
    {
        // This function can remain largely as-is from your last version, as it was already good.
        // It reads parameters but doesn't modify them, so lock is only needed for reading them once.
        int localIntLevel;
        double localPassbandRatio, localSnapInterval;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localIntLevel = intLevel;
            localPassbandRatio = passbandRatio;
            localSnapInterval = snapInterval;
        }

        const double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
        const double wfWidth = gui::waterfall.getViewBandwidth();
        const double wfStart = wfCenter - (wfWidth / 2.0);
        const double wfEnd = wfCenter + (wfWidth / 2.0);
        const double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
        const double step = scanDir ? localSnapInterval : -localSnapInterval;

        for (double freq = current + step; scanDir ? (freq <= stopFreq) : (freq >= startFreq); freq += step)
        {
            if (freq < wfStart || freq > wfEnd)
                break;
            if (freq < bottomLimit)
                bottomLimit = freq;
            if (freq > topLimit)
                topLimit = freq;
            if (searchInIdentified(freq))
                continue;

            const float level = getMaxLevel(data, freq, vfoWidth * (localPassbandRatio * 0.01f), dataWidth, wfStart, wfWidth);

            if (level >= localIntLevel)
            {
                const float nextLevel = getMaxLevel(data, freq + step, vfoWidth, dataWidth, wfStart, wfWidth);
                if (nextLevel > level)
                {
                    flog::warn("SKIP on freq {0} (level {1}) because next is stronger ({2})", freq, level, nextLevel);
                    skip_for_relevel_counter++;
                    continue;
                }
                skip_for_relevel_counter = 0;
                current = freq;
                return true;
            }
        }
        return false;
    }

private:
    static void workerInfo(void *ctx)
    {
        ScannerModule2 *_this = (ScannerModule2 *)ctx;
        bool _first = true;

        while (true) // TODO: Нужен флаг для безопасного выхода
        {
            if (core::g_isExiting)
            {
                // Программа завершается. Больше ничего не делаем.
                // Просто ждем, пока нас остановят через pleaseStop.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                flog::warn("if (core::g_isExiting) scanner2");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            {
                std::lock_guard<std::mutex> lck(_this->classMtx);

                if (!_this->isServer && !_this->isARM)
                {
                    continue;
                }
                // ====================================================================
                // Фаза 1: Сбор команд и данных из GUI (быстрые неблокирующие вызовы)
                // ====================================================================
                uint8_t currSrvr = gui::mainWindow.getCurrServer();
                bool list_update_requested = _this->isServer && gui::mainWindow.getUpdateMenuRcv5Srch() && gui::mainWindow.getUpdateListRcv5Srch(currSrvr);
                // bool list_update_requested = gui::mainWindow.getUpdateMenuRcv5Srch() && gui::mainWindow.getUpdateListRcv5Srch(currSrvr);

                int new_id_from_gui = gui::mainWindow.getidOfList_srch(currSrvr);

                // ====================================================================
                // Фаза 2: Выполнение тяжелой операции (обновление списка) БЕЗ мьютекса
                // ====================================================================
                bool list_was_updated = false;
                if (list_update_requested)
                {
                    gui::mainWindow.setUpdateMenuRcv5Srch(false);
                    gui::mainWindow.setUpdateListRcv5Srch(currSrvr, false);

                    flog::info("[workerInfo] List update request received. Processing...");
                    int cnt_bbuf = gui::mainWindow.getsizeOfbbuf_srch();
                    if (cnt_bbuf > 0)
                    {
                        void *bbufRCV = ::operator new(cnt_bbuf);
                        memcpy(bbufRCV, gui::mainWindow.getbbuf_srch(), cnt_bbuf);

                        config.acquire();
                        SearchModeList fbm;
                        for (int poz = 0; poz < cnt_bbuf; poz += sizeof(fbm))
                        {
                            memcpy(&fbm, static_cast<uint8_t *>(bbufRCV) + poz, sizeof(fbm));
                            std::string listname(fbm.listName);

                            json def = json::object();
                            def["listName"] = listname;
                            def["_mode"] = fbm._mode;
                            def["_bandwidth"] = fbm._bandwidth;
                            def["_startFreq"] = fbm._startFreq;
                            def["_stopFreq"] = fbm._stopFreq;
                            def["_interval"] = fbm._interval;
                            def["_passbandRatio"] = fbm._passbandRatio;
                            def["_tuningTime"] = fbm._tuningTime;
                            def["_waitingTime"] = fbm._waitingTime;
                            def["_lingerTime"] = fbm._lingerTime;
                            def["_level"] = fbm._level;
                            def["_selectedLogicId"] = fbm._selectedLogicId;
                            def["_status_stop"] = fbm._status_stop;
                            def["_status_record"] = fbm._status_record;
                            def["_status_direction"] = fbm._status_direction;
                            def["_status_ignor"] = fbm._status_ignor;
                            def["_selectedSrchMode"] = fbm._selectedSrchMode;
                            config.conf["lists"][listname] = def;
                        }
                        config.release(true);
                        ::operator delete(bbufRCV);
                    }

                    _this->refreshLists();

                    list_was_updated = true;
                }

                // ====================================================================
                // Фаза 3: Принятие решений и обновление простых параметров (короткая блокировка)
                // ====================================================================
                bool id_has_changed = false;
                bool should_start = false;
                bool should_stop = false;

                { // Начало короткой критической секции
                    // std::lock_guard<std::mutex> lock(_this->paramMtx);
                    // Обновляем простые переменные
                    _this->CurrSrvr = currSrvr;
                    _this->currSource = sourcemenu::getCurrSource();

                    // Проверяем, изменился ли ID списка
                    if (_this->selectedListId != new_id_from_gui)
                    {
                        id_has_changed = true;
                    }

                    // --- Блок для isARM ---
                    if (_this->isARM)
                    {
                        if (!gui::mainWindow.getUpdateMenuSnd5Srch(currSrvr))
                        {
                            if (_this->selectedListId != gui::mainWindow.getidOfList_srch(currSrvr))
                            {
                                // Важно! loadByName тяжелая, ее нельзя вызывать под мьютексом
                                // Мы просто запомним, что нужно сделать, и сделаем после
                                // Эта логика будет обработана ниже
                            }

                            if (_this->intLevel != gui::mainWindow.getLevelDbSrch(currSrvr))
                            {
                                _this->intLevel = gui::mainWindow.getLevelDbSrch(currSrvr);
                            }
                            if (_this->status_AKF != gui::mainWindow.getAKFInd(currSrvr))
                            {
                                _this->status_AKF = gui::mainWindow.getAKFInd(currSrvr);
                            }
                            if (_this->status_auto_level != gui::mainWindow.getAuto_levelSrch(currSrvr))
                            {
                                _this->status_auto_level = gui::mainWindow.getAuto_levelSrch(currSrvr);
                            }
                            if (_this->snr_level != gui::mainWindow.getSNRLevelDb(currSrvr))
                            {
                                _this->snr_level = std::clamp<int>(gui::mainWindow.getSNRLevelDb(currSrvr), 5, maxSNRLevel);
                            }
                            int guiSelectedLogicId = gui::mainWindow.getselectedLogicId(currSrvr);
                            if (_this->selectedLogicId != guiSelectedLogicId)
                            {
                                _this->selectedLogicId = guiSelectedLogicId;
                            }
                        }
                    }

                    // --- Блок для isServer ---
                    if (_this->isServer)
                    {
                        if (gui::mainWindow.getUpdateMenuRcv5Srch())
                        {
                            gui::mainWindow.setUpdateMenuRcv5Srch(false);
                        }

                        if (!_first)
                        {
                            _first = gui::mainWindow.getFirstConn(NUM_MOD);
                        }

                        if (_this->status_AKF != gui::mainWindow.getAKFInd(currSrvr))
                        {
                            _this->status_AKF = gui::mainWindow.getAKFInd(currSrvr);
                        }
                        if (_this->status_auto_level != gui::mainWindow.getAuto_levelSrch(currSrvr))
                        {
                            _this->status_auto_level = gui::mainWindow.getAuto_levelSrch(currSrvr);
                        }
                        if (_this->snr_level != gui::mainWindow.getSNRLevelDb(currSrvr))
                        {
                            _this->snr_level = std::clamp<int>(gui::mainWindow.getSNRLevelDb(currSrvr), 5, maxSNRLevel);
                        }
                        if (!_this->status_auto_level)
                        {
                            if (_this->intLevel != gui::mainWindow.getLevelDbSrch(currSrvr))
                            {
                                _this->intLevel = gui::mainWindow.getLevelDbSrch(currSrvr);
                            }
                        }
                        if (gui::mainWindow.getUpdateModule_srch(currSrvr))
                        {
                            _first = false;
                            gui::mainWindow.setFirstConn(NUM_MOD, false);
                            gui::mainWindow.setUpdateModule_srch(currSrvr, false);
                        }
                    }
                } // --- КОНЕЦ КОРОТКОЙ КРИТИЧЕСКОЙ СЕКЦИИ ---

                // ====================================================================
                // Фаза 4: Выполнение долгих или блокирующих действий ПОСЛЕ снятия мьютекса
                // ====================================================================

                // Действие 1: Перезагрузка списка, если нужно
                if (list_was_updated || id_has_changed)
                {
                    if (new_id_from_gui < _this->listNames.size())
                    {
                        std::string new_name = _this->listNames[new_id_from_gui];
                        flog::info("[workerInfo] Reloading scan list '{0}'", new_name);

                        _this->loadByName(new_name, true);

                        // Обновляем ID и сохраняем выбор в конфиг
                        _this->selectedListId = new_id_from_gui;
                        config.acquire();
                        config.conf["selectedList"] = new_name;
                        config.release(true);
                    }
                }

                // ====================================================================
                // Фаза 5: ЕДИНАЯ и БЕЗОПАСНАЯ логика START/STOP для ВСЕХ режимов
                // ====================================================================
                if (!_this->isARM)
                {
                    core::modComManager.callInterface("Airspy", 0, NULL, &_this->_air_recording);
                    bool remote_cmd = gui::mainWindow.getbutton_srch(_this->CurrSrvr);
                    bool is_running_now = _this->running.load();
                    // flog::info("[workerInfo DEBUG] is_running: {0}, remote_cmd_from_gui: {1}, isServer: {2}, isARM: {3}",
                    //           is_running_now, remote_cmd, _this->isServer, _this->isARM);
                    // Сравниваем наше состояние с командой из GUI
                    if (is_running_now != remote_cmd)
                    {
                        // Обнаружили несоответствие. Проверяем "льготный период".
                        const auto grace_period = std::chrono::seconds(2);
                        if ((std::chrono::steady_clock::now() - _this->last_manual_command_time) > grace_period)
                        {
                            // Льготный период прошел, значит, это настоящая команда, а не рассинхрон.
                            flog::info("[workerInfo] State mismatch detected outside grace period. Syncing state...");

                            if (remote_cmd)
                            { // Команда на СТАРТ
                                if (_this->_air_recording == 1)
                                {
                                    _this->start();
                                }
                            }
                            else
                            { // Команда на СТОП
                                flog::info("[workerInfo] stop");
                                _this->stop();
                            }
                        }
                        else
                        {
                            // Мы внутри льготного периода. Игнорируем несоответствие.
                            flog::info("[workerInfo] State mismatch detected INSIDE grace period. Ignoring to prevent race condition.");
                        }
                    }
                }
            }
        }
    }

    static void menuHandler(void *ctx)
    {
        ScannerModule2 *_this = (ScannerModule2 *)ctx;
        _this->currSource = sourcemenu::getCurrSource();
        uint8_t currSrvr = gui::mainWindow.getCurrServer();
        _this->CurrSrvr = currSrvr;

        int _work;
        core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);

        // gui::mainWindow.setLevelDb(currSrvr, _this->intLevel);
        float menuWidth = ImGui::GetContentRegionAvail().x;
        float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float btnSize = ImGui::CalcTextSize("Додати набір").x + 4;
        std::string _ListName = _this->selectedListName;
        bool _run = _this->running;

        bool showThisMenu = _this->isServer;
        if (_this->Admin)
            showThisMenu = false;

        if (_run || _work > 0 || showThisMenu)
        {
            ImGui::BeginDisabled();
        }
        {
            _this->selectedLogicId = 1;

            if (_this->isARM || _this->isServer)
            {
                _this->selectedLogicId = 1;
            }
            else
            {
                // ImGui::TextColored(ImVec4(1, 0, 1, 1), "      Режим пошуку:");
                // ... was ДОДАТИ НАБИР
            }
            std::vector<std::string> selectedNames;
            selectedNames.push_back("General");

            btnSize = ImGui::CalcTextSize("Зберегти як ...").x + 8;
            // ImGui::CalcTextSize("Зберегти як...").x + 20;
            ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);

            if (ImGui::Combo(("##step_scanner2_list_sel" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str()))
            {
                flog::info("TRACE. _this->listNames[{0}] = {1}!", _this->selectedListId, _this->listNames[_this->selectedListId]);
                _this->loadByName(_this->listNames[_this->selectedListId], true);
                config.acquire();
                config.conf["selectedList"] = _this->selectedListName;
                config.release(true);
                {
                    // tuner::centerTuning(gui::waterfall.selectedVFO, _this->startFreq);
                    // gui::waterfall.centerFreqMoved = true;
                }
                gui::waterfall.setPrevCenterFrequency(gui::waterfall.getCenterFrequency());
                tuner::centerTuning(gui::waterfall.selectedVFO, _this->startFreq);
                gui::waterfall.centerFreqMoved = true;
                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &_this->mode, NULL);
                double _bandwidth = (double)_this->bandwidth;
                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &_bandwidth, NULL);
                flog::info("            TRACE. _this->selectedListId = {0}!", _this->selectedListId);

                gui::mainWindow.setidOfList_srch(currSrvr, _this->selectedListId);
                gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
                gui::mainWindow.setUpdateSrchListForBotton(currSrvr, true);
            }
            //===========================================================================================
            ImGui::SameLine();

            if (_this->listNames.size() == 0 || _ListName == "" || _ListName == "General")
            {
                style::beginDisabled();
            }
            {
                if (ImGui::Button(("Зберегти як ...##scann3_ren_lst_3" + _this->name).c_str(), ImVec2(btnSize, 20)))
                {
                    _this->firstEditedListName = _this->listNames[_this->selectedListId];
                    _this->editedListName = _this->firstEditedListName;
                    _this->renameListOpen = true;
                }
            }
            if (_this->listNames.size() == 0 || _ListName == "" || _ListName == "General")
            {
                style::endDisabled();
            }
            //===========================================================================================
            ImGui::SameLine();
            if (ImGui::Button(("+##scann3_add_lst_3" + _this->name).c_str(), ImVec2(20, 26)))
            {
                // Find new unique default name

                if (std::find(_this->listNames.begin(), _this->listNames.end(), "New List") == _this->listNames.end())
                {
                    _this->editedListName = "New List";
                }
                else
                {
                    char buf[64];
                    for (int i = 1; i < 1000; i++)
                    {
                        sprintf(buf, "New List (%d)", i);
                        if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end())
                        {
                            break;
                        }
                    }
                    _this->editedListName = buf;
                }
                _this->newListOpen = true;
            }
            //===========================================================================================
            if (_ListName == "" || _ListName == "General")
            {
                style::beginDisabled();
            }
            {
                ImGui::SameLine();
                // ImGui::SetNextItemWidth(menuWidth - (2*btnSize));
                if (ImGui::Button(("-##scann3_del_lst_3" + _this->name).c_str(), ImVec2(20, 26)))
                {
                    _this->deleteListOpen = true;
                }
                // List delete confirmation
                if (ImGui::GenericDialog(("scann3_del_list_confirm3" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]()
                                         { ImGui::Text("Видалення банку \"%s\". Ви впевнені?", _this->selectedListName.c_str()); }) == GENERIC_DIALOG_BUTTON_YES)
                {
                    if (_this->selectedListName != "General")
                    {
                        config.acquire();
                        config.conf["lists"].erase(_this->selectedListName);
                        config.release(true);
                        _this->refreshLists();
                        _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
                        if (_this->listNames.size() > 0)
                        {
                            _this->loadByName(_this->listNames[_this->selectedListId], false);
                            // flog::info("SCANNER2. RADIO_IFACE_CMD_SET_MODE 703. snapInterval = {0}!", _this->snapInterval);
                            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &_this->mode, NULL);
                            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &_this->bandwidth, NULL);
                            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SNAPINTERVAL, &_this->snapInterval, NULL);
                        }
                        else
                        {
                            _this->selectedListName = "";
                            gui::waterfall.selectedListName = "";
                        }
                        // setidOfList_srch
                        _this->getSearchLists();
                        gui::mainWindow.setidOfList_srch(gui::mainWindow.getCurrServer(), _this->selectedListId);
                        gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
                        gui::mainWindow.setUpdateListRcv5Srch(MAX_SERVERS, true);
                        gui::mainWindow.setUpdateSrchListForBotton(currSrvr, true);
                        // gui::mainWindow.setSearchListNamesTxt(currSrvr, _this->listNamesTxt);
                        gui::mainWindow.setSearchListNamesTxt(_this->listNamesTxt);
                    }
                }
            }
            if (_ListName == "" || _ListName == "General")
            {
                style::endDisabled();
            }

            //=================================================================================================
            /*
            ImGui::TextColored(ImVec4(1, 0, 1, 1), "      Режим пошуку:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo(("##mode_scanner2" + _this->name).c_str(), &_this->selectedLogicId, _this->logicListTxt.c_str())) {
                if (_this->selectedLogicId == 0) {
                    _this->status_direction = false;
                    _this->status_record = false;
                }
                else {
                    _this->loadByName(_this->selectedListName, true);
                    config.acquire();
                    config.conf["selectedList"] = _this->selectedListName;
                    config.release(true);
                    _this->status_record = true;
                }
                if (_this->selectedLogicId == 2 && _this->selectedIntervalId > 7)
                    _this->selectedIntervalId = 7;
                flog::info("TRACE. _this->selectedLogicId = {0}!", _this->selectedLogicId);
                gui::mainWindow.setselectedLogicId(currSrvr, _this->selectedLogicId);
                gui::mainWindow.setUpdateModule_srch(currSrvr, true);
                gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
            }
            */
            //=================================================================================================
            // ImGui::LeftLabel("Старт, кГц   ");
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Старт, кГц   ");
            ImGui::SameLine();
            float inputsize = menuWidth / 2 - ImGui::GetCursorPosX();
            if (inputsize < menuWidth / 4)
                inputsize = menuWidth / 4;

            ImGui::SetNextItemWidth(inputsize); // menuWidth/2 - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##start_freq_scanner_2", &_this->startFreqKGz, 1, 999000000))
            {
                _this->startFreqKGz = std::clamp<int>(round(_this->startFreqKGz), 1, 999999000);
                _this->startFreq = _this->startFreqKGz * 1000;
                if (_this->startFreq >= _this->stopFreq)
                {
                    // _this->startFreq = _this->stopFreq - _this->snapInterval;
                    // _this->startFreqKGz = (_this->stopFreq - _this->snapInterval) / 1000;
                    _this->stopFreq = _this->startFreq + _this->snapInterval;
                    _this->stopFreqKGz = (_this->startFreq + _this->snapInterval) / 1000;
                }
                // flog::info("_this->startFreq {0}, _this->stopFreq {1}", _this->startFreq, _this->stopFreq);
            }
            ImGui::SameLine();
            //=================================================================================================
            // ImGui::LeftLabel("Стоп, кГц     ");
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Стоп, кГц   ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(inputsize); // menuWidth/2 - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##stop_freq_scanner_2", &_this->stopFreqKGz, 1, 999999999))
            {
                _this->stopFreqKGz = std::clamp<int>(round(_this->stopFreqKGz), 1, 999999999);
                _this->stopFreq = _this->stopFreqKGz * 1000;
                if (_this->startFreq >= _this->stopFreq)
                {
                    _this->stopFreq = _this->startFreq + _this->snapInterval;
                    _this->stopFreqKGz = (_this->startFreq + _this->snapInterval) / 1000;
                }
                // flog::info("_this->startFreq {0}, _this->stopFreq {1}", _this->startFreq, _this->stopFreq);
            }
            //=================================================================================================
            // ImGui::TextColored(ImVec4(1, 0, 1, 1), "      Режим пошуку:");
            ImGui::TextColored(ImVec4(1, 0, 1, 1), "Пресети:"); // Налаштування пошуку
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo(("##srchmode_scanner2" + _this->name).c_str(), &_this->selectedSrchMode, _this->srchModeListTxt.c_str()))
            {
                _this->applyPreset(_this->selectedSrchMode);
                /*
                flog::info("TRACE. _this->selectedSrchMode = {0}!", _this->selectedSrchMode);
                bool updt = false;
                switch (_this->selectedSrchMode)
                {
                case 1:                      // АМ Voice
                    _this->mode = 2;         // AM
                    _this->_bandwidthId = 3; // 6250
                    _this->snapInterval = 25000;
                    _this->passbandRatio = 40;
                    _this->tuningTime = 100;
                    updt = true;
                    break;
                case 2:                      // FM Voice
                    _this->mode = 0;         // ЧМ
                    _this->_bandwidthId = 4; // 12500
                    _this->snapInterval = 12500;
                    _this->passbandRatio = 20;
                    _this->tuningTime = 200;
                    updt = true;
                    break;
                case 3:                          // DMR
                    _this->mode = 0;             // ЧМ
                    _this->_bandwidthId = 4;     //  3 -  6250; 4 - 12500
                    _this->snapInterval = 12500; // 6250
                    _this->passbandRatio = 15;
                    _this->tuningTime = 100;
                    updt = true;
                    break;
                case 4:                      // FM Radio
                    _this->mode = 1;         // FM
                    _this->_bandwidthId = 8; // 220000
                    _this->snapInterval = 100000;
                    _this->passbandRatio = 80;
                    _this->tuningTime = 200;
                    updt = true;
                    break;
                }
                if (updt)
                {
                    _this->bandwidth = _this->bandwidthsList[_this->_bandwidthId]; // *1000
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &_this->mode, NULL);
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SNAPINTERVAL, &_this->snapInterval, NULL);
                    double _bandwidth = (double)_this->bandwidth;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &_bandwidth, NULL);
                }
                */
                gui::mainWindow.setselectedSrchMode(currSrvr, _this->selectedSrchMode);
                gui::mainWindow.setUpdateModule_srch(currSrvr, true);
                gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
            }
            //=================================================================================================
            if (_this->selectedSrchMode > 0)
            {
                style::beginDisabled();
            }
            {
                ImGui::LeftLabel("Вид демод. ");
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::Combo(("##scan_edit_mode_3" + _this->name).c_str(), &_this->mode, demodModeListTxt))
                {
                    _this->mode = _this->mode;
                    if (_this->mode == 0)
                        _this->_bandwidthId = 3;
                    if (_this->mode == 1)
                    {
                        _this->_bandwidthId = 6;
                        if (_this->selectedLogicId == 2)
                            _this->selectedIntervalId = 7;
                    }
                    if (_this->mode == 2)
                        _this->_bandwidthId = 3; // AM
                    if (_this->mode == 3)
                        _this->_bandwidthId = 3; // DSB
                    if (_this->mode == 4)
                        _this->_bandwidthId = 1; // USB
                    if (_this->mode == 5)
                        _this->_bandwidthId = 0; // CW
                    if (_this->mode == 6)
                        _this->_bandwidthId = 1;
                    if (_this->mode == 7)
                        _this->_bandwidthId = 6;

                    _this->bandwidth = _this->bandwidthsList[_this->_bandwidthId]; // *1000
                    // flog::info("SCANNER2. RADIO_IFACE_CMD_SET_MODE 761!");
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &_this->mode, NULL);
                    double _bandwidth = (double)_this->bandwidth;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &_bandwidth, NULL);
                    flog::info("TRACE. SET mode {0}, bandwidth = {1},getSampleRate() {2}  !", _this->mode, _this->bandwidth, sigpath::iqFrontEnd.getSampleRate());
                }
                //=================================================================================================
                ImGui::LeftLabel("Смуга, кГц  ");
                // ImGui::LeftLabel("Крок, кГц   ");
                inputsize = menuWidth / 2 - ImGui::GetCursorPosX();
                if (inputsize < menuWidth / 4)
                    inputsize = menuWidth / 4;
                ImGui::SetNextItemWidth(inputsize); // menuWidth - ImGui::GetCursorPosX());
                float kHzbandwidth = 1.0;
                if (_this->bandwidth < 1000)
                    kHzbandwidth = 1.0;
                else
                    kHzbandwidth = _this->bandwidth / 1000.0;

                if (ImGui::InputFloat(("##_radio_bw_" + _this->name).c_str(), &kHzbandwidth, 1.25, 10.0, "%.2f"))
                {
                    int max_bw = _this->bandwidthsList[_this->bandwidthsList.size() - 1];
                    // if (max_bw > _this->maxBandwidth)
                    //     max_bw = _this->maxBandwidth;
                    float bw = std::clamp<double>(kHzbandwidth * 1000, 1, max_bw);
                    if (bw >= _this->bandwidth)
                    {
                        for (int i = 1; i < _this->bandwidthsList.size(); i++)
                        {
                            if (bw < _this->bandwidthsList[i])
                            {
                                bw = _this->bandwidthsList[i];
                                break;
                            }
                        }
                    }
                    else
                    {
                        for (int i = _this->bandwidthsList.size() - 1; i >= 0; i--)
                        {
                            if (bw > _this->bandwidthsList[i])
                            {
                                bw = _this->bandwidthsList[i];
                                break;
                            }
                        }
                    }
                    if (bw < 1000)
                    {
                        bw = 1000;
                    }

                    _this->bandwidth = bw;
                    double _bandwidth = (double)bw;
                    // flog::info("bw {0}", bw);
                    // flog::info("kHzbandwidth {0}, _this->bandwidth {1}, max_bw {2}, _bandwidth {3}, bw {4}", kHzbandwidth, _this->bandwidth, max_bw, _bandwidth, bw);
                    // flog::info("SCANNER2. RADIO_IFACE_CMD_SET_MODE 803!");
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &_this->mode, NULL);
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &_bandwidth, NULL);

                    for (int i = 0; i < _this->bandwidthsList.size(); i++)
                    {
                        if ((_this->bandwidthsList[i]) >= _this->bandwidth)
                        {
                            _this->_bandwidthId = i;
                            break;
                        }
                    }
                    flog::info("TRACE. SET bandwidth = {0}, _this->_bandwidthId {1} !", _this->bandwidth, _this->_bandwidthId);
                    if (_this->bandwidth > _this->snapInterval)
                    {
                        _this->snapInterval = _this->bandwidth;
                        float snap = _this->bandwidth;

                        for (int i = 1; i < _this->snapintervalsList.size(); i++)
                        {
                            if (snap <= _this->snapintervalsList[i])
                            {
                                snap = _this->snapintervalsList[i];
                                break;
                            }
                        }
                        _this->snapInterval = snap;
                    }
                }
                //=================================================================================================
                // ImGui::LeftLabel("Крок, кГц              ");
                ImGui::SameLine();
                ImGui::LeftLabel("Крок, кГц  ");
                // ImGui::TextColored(ImVec4(0, 0, 0, 1), "Крок, кГц   ");
                ImGui::SameLine();

                // ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                ImGui::SetNextItemWidth(inputsize); // menuWidth - ImGui::GetCursorPosX());

                float _snap = 1.0;
                if (_this->snapInterval < 1000)
                    _snap = 1.0;
                else
                    _snap = _this->snapInterval / 1000.0;
                // flog::info("_snap {0}, _this->snapInterval  {1}", _snap, _this->snapInterval);
                if (ImGui::InputFloat(("##_radio_snap_" + _this->name).c_str(), &_snap, 1.05, 10.0, "%.2f"))
                {
                    //_this->snapInterval = _this->snapintervalsList[_this->snapIntervalId];
                    float max_snap = _this->snapintervalsList[_this->snapintervalsList.size() - 1];
                    float snap = std::clamp<float>(_snap * 1000, 1, max_snap);
                    if (snap > max_snap)
                        snap = max_snap;
                    if (snap >= _this->snapInterval)
                    {
                        for (int i = 1; i < _this->snapintervalsList.size(); i++)
                        {
                            if (snap < _this->snapintervalsList[i])
                            {
                                snap = _this->snapintervalsList[i];
                                break;
                            }
                        }
                    }
                    else
                    {
                        for (int i = _this->snapintervalsList.size() - 1; i >= 0; i--)
                        {
                            if (snap > _this->snapintervalsList[i])
                            {
                                snap = _this->snapintervalsList[i];
                                break;
                            }
                        }
                    }
                    if (snap < 1000)
                    {
                        snap = 1000;
                    }
                    flog::info("_snap {0}, max_snap {1}, _this->snapInterval {2}, snap_new {3}", _snap, max_snap, _this->snapInterval, snap);
                    _this->snapInterval = snap;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_SNAPINTERVAL, &_this->snapInterval, NULL);
                    for (int i = 0; i < _this->snapintervalsList.size(); i++)
                    {
                        if (_this->snapintervalsList[i] >= _this->snapInterval)
                        {
                            _this->selectedIntervalId = i;
                            break;
                        }
                    }
                }
                //=================================================================================================
                if (_this->Admin)
                {
                    if (STEP == true)
                    {
                        // ImGui::BeginDisabled();
                        ImGui::LeftLabel("Коеф. смуги пропускання (%)");
                        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                        // passbandRatio = относительную ширину полосы пропускания (passband) по сравнению с полосой фильтра.
                        // Passband Width=Filter Width×passbandRatio
                        if (ImGui::InputDouble("##pb_ratio_scanner_2", &_this->passbandRatio, 1.0, 10.0, "%0.0f"))
                        {
                            _this->passbandRatio = std::clamp<double>(round(_this->passbandRatio), 1.0, 100.0);
                        }
                        // ImGui::EndDisabled();
                    }
                    //=================================================================================================
                    ImGui::LeftLabel("Тривалість налаштування, мс");
                    ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                    if (ImGui::InputInt("##tuning_time_scanner_2", &_this->tuningTime, 100, 10000))
                    {
                        _this->tuningTime = std::clamp<int>(_this->tuningTime, 100, 10000.0);
                    }
                }
                else
                {
                    // _this->tuningTime =
                    // _this->passbandRatio =
                }
            }
            //=================================================================================================            7

            if (_this->selectedSrchMode > 0)
            {
                style::endDisabled();
            }
        }
        if (_run || _work > 0 || showThisMenu)
        {
            ImGui::EndDisabled();
        }
        //====================================================================================
        // 2 endDisabled
        if (_this->Admin)
        {

            if (showThisMenu)
            {
                ImGui::BeginDisabled();
            }
            {
                //---------------------------------------------------
                ImGui::Checkbox("Призупиняти пошук при виявленні сигналу##_status_scanner2", &_this->status_stop); // status_stop

                if (_this->status_stop)
                {
                    if (_run || _work > 0)
                    {
                        ImGui::BeginDisabled();
                    }
                    {
                        ImGui::LeftLabel("Час очікування, сек");
                        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                        if (ImGui::InputInt("##linger_timeWait_scanner_2", &_this->_waitingTime, 2, 100))
                        {
                            _this->_waitingTime = std::clamp<int>(_this->_waitingTime, 2, _this->lingerTime);
                        }
                    }
                    if (_run || _work > 0)
                    {
                        ImGui::EndDisabled();
                    }
                }
                else
                {
                    _this->_waitingTime = 1;
                }
                //---------------------------------------------------

                if (_run || _work > 0)
                {
                    ImGui::BeginDisabled();
                }
                {
                    if (_this->radioMode == 0)
                    {                                                                                         //  || _this->selectedLogicId>0
                        ImGui::Checkbox("Пеленгувати##_status_direction_scanner2", &_this->status_direction); // status_stop
                        ImGui::SameLine();
                        ImGui::Checkbox("Реєструвати##_record_2", &_this->status_record);
                    }
                    else
                    {
                        _this->status_direction = true; // false;
                        _this->status_record = true;
                        // if (_this->selectedLogicId > 0) {
                        //    ImGui::BeginDisabled();
                        //    ImGui::Checkbox("Реєструвати##_record_22", &_this->status_record);
                        //    ImGui::EndDisabled();
                        // }
                        // else {
                        //     _this->status_record = false;
                        // }
                    }
                    if (_this->status_record)
                    {
                        ImGui::LeftLabel("Макс. тривалість запису, сек");
                        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                        if (ImGui::InputInt("##linger_time_scanner_2", &_this->lingerTime, 1, _this->maxRecDuration - 1))
                        {
                            _this->lingerTime = std::clamp<int>(_this->lingerTime, 1, _this->maxRecDuration - 1);
                        }
                    }
                    else
                    {
                        ImGui::LeftLabel("Тривалість затримки, сек");
                        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                        if (ImGui::InputInt("##linger_time_scanner_2_2", &_this->lingerTime, 1, _this->maxRecDuration - 1))
                        {
                            _this->lingerTime = std::clamp<int>(_this->lingerTime, 1, _this->maxRecDuration - 1);
                        }
                    }
                    // ColorButton(const char* desc_id, const ImVec4& col, ImGuiColorEditFlags flags = 0, ImVec2 size = ImVec2(0, 0));
                    // ImGuiColorEditFlags palette_button_flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
                    // if (ImGui::ColorButton(("Зберегти параметри##scann3_ren_lst_3" + _this->name).c_str(), ImVec4(0.9f, 0.9f, 0.9f, 1.0f), palette_button_flags, ImVec2(menuWidth, 0))) {
                }
                /*
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Ігнорувати відомі частоти   ");
                ImGui::SameLine();
                // ImGui::Checkbox("##_status_ignor_scanner2", &_this->status_ignor); // status_stop
                if (ImGui::Checkbox("##_status_ignor_scanner2", &_this->status_ignor))
                {
                }
                */
                _this->status_ignor = true;
                // ImGui::TextColored(ImVec4(1, 0, 1, 1), "Ігнорувати відомі частоти");
                //  ImGui::Checkbox("Ігнорувати відомі частоти ##_status_ignor_scanner2", &_this->status_ignor)
                if (_run || _work > 0)
                {
                    ImGui::EndDisabled();
                }

                //-------------------------------------------------------------
                if (!_this->isServer || _this->Admin)
                {
                    if (_run || _work > 0)
                    {
                        ImGui::BeginDisabled();
                    }
                    ImGui::BeginTable("scanner2_butt_table_2", 2);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Button(("Зберегти параметри##scann3_ren_lst_3" + _this->name).c_str(), ImVec2(menuWidth / 2, 22)))
                    {
                        _this->firstEditedListName = _this->listNames[_this->selectedListId];
                        _this->editedListName = _this->firstEditedListName;
                        _this->onlySaveListOpen = _this->newListSave();
                    }
                    ImGui::EndTable();
                    if (_run || _work > 0)
                    {
                        ImGui::EndDisabled();
                    }
                }

                if (ImGui::Checkbox("Автоматичне визначення порогу ##status_auto_level2", &_this->status_auto_level))
                {
                    // _this->status_auto_level = gui::mainWindow.getAuto_level(currSrvr);
                    config.acquire();
                    config.conf["status_auto_level"] = _this->status_auto_level;
                    config.release(true);
                    gui::mainWindow.setAuto_levelSrch(currSrvr, _this->status_auto_level);
                    if (_this->status_auto_level)
                    {
                        gui::mainWindow.setLevelDbSrch(currSrvr, _this->intLevel);
                    }
                    flog::info("UPDATE 3 _this->status_auto_level {0}, getAuto_level({1}) = {2}", _this->status_auto_level, currSrvr, gui::mainWindow.getAuto_levelSrch(currSrvr));

                    gui::mainWindow.setUpdateMenuSnd5Srch(currSrvr, true);
                }
                if (_this->status_auto_level)
                {
                    ImGui::LeftLabel("Відношення сигнал/шум, дБ");
                    ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                    if (ImGui::InputInt("##SNR_dB_scanner2", &_this->snr_level, 1, maxSNRLevel))
                    {
                        _this->snr_level = std::clamp<int>(_this->snr_level, 5, maxSNRLevel);
                        config.acquire();
                        config.conf["SNRlevel"] = _this->snr_level;
                        config.release(true);
                        gui::mainWindow.setSNRLevelDb(currSrvr, _this->snr_level);
                        gui::mainWindow.setUpdateMenuSnd5Srch(currSrvr, true);
                    }
                }
                if ((_run || _work > 0) && _this->status_auto_level)
                {
                    ImGui::BeginDisabled();
                }
                ImGui::LeftLabel("Поріг виявлення");
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                // ImGui::SliderFloat("##scanner_level", &_this->level, -150.0, 0.0);
                if (ImGui::SliderInt("##scanner2_level", &_this->intLevel, -150, 0))
                {
                    // _this->level = (float)_this->intLevel * 1.0;
                    config.acquire();
                    config.conf["_level"] = _this->intLevel;
                    config.release(true);
                    gui::mainWindow.setLevelDbSrch(currSrvr, _this->intLevel);
                    gui::mainWindow.setUpdateMenuSnd5Srch(currSrvr, true);
                }
                if ((_run || _work > 0) && _this->status_auto_level)
                {
                    ImGui::EndDisabled();
                }

                if (_this->SignalIndf)
                {
                    if (ImGui::Checkbox("Аналізувати АКФ##_auto_akf_srch", &_this->status_AKF))
                    {
                        gui::mainWindow.setAKFInd(currSrvr, _this->status_AKF);
                        _this->status_AKF = gui::mainWindow.getAKFInd(currSrvr);
                        config.acquire();
                        config.conf["status_AKF"] = _this->status_AKF;
                        config.release(true);
                        flog::info("UPDATE _this->status_AKF {0}", _this->status_AKF);
                        gui::mainWindow.setUpdateMenuSnd5Srch(currSrvr, true);
                    }
                    else
                    {
                        // _this->status_AKF = false;
                    }
                }
            }
            if (showThisMenu)
            {
                ImGui::EndDisabled();
            }
        }
        else
        {
            _this->status_stop = true;
            _this->status_direction = true; // false;
            _this->status_record = true;
            _this->status_ignor = true;
            if (_this->_waitingTime > 5)
                _this->_waitingTime = 2;
            if (_this->lingerTime > 15)
                _this->lingerTime = 10;
            //_this->status_auto_level = true;
            _this->status_AKF = true;
            ImGui::LeftLabel("Відношення сигнал/шум, дБ");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##SNR_dB_scannerMAIN", &_this->snr_level, 1, maxSNRLevel))
            {
                _this->snr_level = std::clamp<int>(_this->snr_level, 5, maxSNRLevel);
                config.acquire();
                config.conf["SNRlevel"] = _this->snr_level;
                config.release(true);
                gui::mainWindow.setSNRLevelDb(currSrvr, _this->snr_level);
                gui::mainWindow.setUpdateMenuSnd5Srch(currSrvr, true);
            }

            // if ((_run || _work > 0) && _this->status_auto_level)
            ImGui::BeginDisabled();
            ImGui::LeftLabel("Поріг виявлення");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderInt("##scanner2_levelMAIN", &_this->intLevel, -150, 0))
            {
                gui::mainWindow.setLevelDbSrch(currSrvr, _this->intLevel);
                gui::mainWindow.setUpdateMenuSnd5Srch(currSrvr, true);
            }
            // if ((_run || _work > 0) && _this->status_auto_level)
            ImGui::EndDisabled();
        }
        //=================================================================================================
        bool _ifStartElseBtn = gui::mainWindow.getIfOneButtonStart();
        // flog::info("currSource {0}, isARM {1}, isServer {2}, _this->running {3}, _ifStartElseBtn {4}", _this->currSource, _this->isARM, _this->isServer, _this->running, _ifStartElseBtn);
        if (_this->currSource == SOURCE_ARM)
        {
            uint8_t currSrv = gui::mainWindow.getCurrServer();
            bool button_srch = gui::mainWindow.getbutton_srch(currSrv);
            if (button_srch == true && (_this->running != button_srch))
            {
                std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                gui::waterfall.finded_freq.clear();
            }
            _this->running = button_srch;
            bool isPlaying = gui::mainWindow.isPlaying();
            bool rec_work = gui::mainWindow.getServerRecording(currSrv);
            int _control = gui::mainWindow.getServerStatus(currSrv);
            // flog::info("_this->running {0}, isPlaying {1}, _control {2}, _work {3}, rec_work {4}", _this->running, isPlaying, _control, _work, rec_work);
            if (_control != ARM_STATUS_FULL_CONTROL)
            {
                ImGui::BeginDisabled();
                ImGui::Button("СТАРТ##scanner2_arm_start_1", ImVec2(menuWidth, 0));
                ImGui::EndDisabled();
            }
            else
            {
                if (!_this->running)
                {
                    if (_work > 0 || rec_work > 0 || _ifStartElseBtn) //  || _air_recording == 0
                        ImGui::BeginDisabled();
                    if (ImGui::Button("СТАРТ##scanner2_arm_start_2", ImVec2(menuWidth, 0)))
                    {
                        flog::info("СТАРТ ARM {0}", currSrv);

                        _this->firstEditedListName = _this->listNames[_this->selectedListId];
                        _this->editedListName = _this->firstEditedListName;
                        _this->onlySaveListOpen = true;
                        {
                            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                            gui::waterfall.finded_freq.clear();
                        }
                        gui::mainWindow.setbutton_srch(currSrv, true);
                        gui::mainWindow.setselectedLogicId(currSrvr, 2);
                        gui::mainWindow.setAuto_levelSrch(currSrvr, _this->status_auto_level);
                        gui::mainWindow.setSNRLevelDb(currSrvr, _this->snr_level);
                        gui::mainWindow.setLevelDbSrch(currSrvr, _this->intLevel);
                        gui::mainWindow.setAKFInd(currSrvr, _this->status_AKF);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrv, true);
                        if (_this->selectedLogicId > 0)
                        {
                            // _this->working_list.clear();
                            _this->_count_list = 0;
                            _this->_curr_list = 0;
                            SearchMode addList;
                            addList.listName = _this->selectedListName;
                            addList._startFreq = _this->startFreq;
                            addList._stopFreq = _this->stopFreq;
                            // _this->working_list[_this->selectedListName] = addList;
                            _this->_count_list++;
                        }
                        else
                        {
                            _this->_count_list = 0;
                        }
                        _this->running = true;
                    }
                    if (_work > 0 || rec_work > 0 || _ifStartElseBtn) //  || _air_recording == 0
                        ImGui::EndDisabled();
                }
                else
                {
                    if (ImGui::Button("СТОП ##scanner2_arm_start_3", ImVec2(menuWidth, 0)))
                    {
                        flog::info("СТОП ARM {0}", currSrv);
                        gui::mainWindow.setbutton_srch(currSrv, false);
                        gui::mainWindow.setAuto_levelSrch(currSrvr, _this->status_auto_level);
                        gui::mainWindow.setLevelDbSrch(currSrvr, _this->intLevel);
                        gui::mainWindow.setAKFInd(currSrvr, _this->status_AKF);
                        gui::mainWindow.setUpdateMenuSnd0Main(currSrv, true);
                    }
                }
            }
            ImGui::BeginTable("scanner2_tbl_arm_srch_rez2", 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Button("Результати пошуку##butt_arm_srch_rez2", ImVec2(menuWidth / 2, 22)))
            {
                _this->openSrchList = true; // getSrchListTabl
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::EndTable();
        }
        else
        { // isServer
            if (!_this->running)
            {
                if (_work > 0 || (_this->isServer && _ifStartElseBtn))
                    ImGui::BeginDisabled();
                // bool isPlaying = gui::mainWindow.isPlaying();
                if (_this->_air_recording == 1)
                {
                    if (ImGui::Button("СТАРТ##scanner2_start_2", ImVec2(menuWidth, 0)))
                    {

                        _this->firstEditedListName = _this->listNames[_this->selectedListId];
                        _this->editedListName = _this->firstEditedListName;
                        _this->onlySaveListOpen = _this->newListSave();
                        gui::mainWindow.setbutton_srch(_this->CurrSrvr, true);
                        _this->start();
                    }
                }
                else
                {
                    style::beginDisabled();
                    ImGui::Button("СТАРТ##scanner2_start_2", ImVec2(menuWidth, 0));
                    style::endDisabled();
                }
                if (_work > 0 || (_this->isServer && _ifStartElseBtn))
                    ImGui::EndDisabled();

                ImGui::BeginTable(("scanner_status_add_bank" + _this->name).c_str(), 2);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Статус: Неактивний");
                ImGui::TableSetColumnIndex(1);
                ImGui::EndTable();
            }
            else
            {
                _this->currSource = sourcemenu::getCurrSource();
                // flog::info("\n currSource {0}, isARM {1}, isServer {2}", currSource, _this->isARM, _this->isServer);

                if (ImGui::Button("СТОП ##scanner_start_2", ImVec2(menuWidth, 0)))
                {
                    _this->stop();
                    gui::mainWindow.setbutton_srch(_this->CurrSrvr, false);
                }
                /*
                if (_this->_recording)
                {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Статус: Реєстрація");
                }
                else if (_this->Receiving)
                { //_finding
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Статус: Приймання");
                }
                else if (_this->tuning)
                {
                    ImGui::TextColored(ImVec4(0, 1, 1, 1), "Статус: Тюнінг");
                }
                else
                {
                    ImGui::TextColored(ImVec4(1, 1, 0, 1), "Статус: Сканування");
                }
                */
                // Загружаем текущее состояние атомарно
                ScannerModule2::State currentState = _this->state.load();

                switch (currentState)
                {
                case ScannerModule2::State::STOPPED:
                    ImGui::Text("Статус: Неактивний");
                    break;

                case ScannerModule2::State::STARTING:
                case ScannerModule2::State::TUNING:
                    ImGui::TextColored(ImVec4(0.0f, 0.7f, 1.0f, 1.0f), "Статус: Налаштування"); // Голубой цвет для тюнинга
                    break;

                case ScannerModule2::State::LEVEL_CALIBRATION:
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Статус: Калібрування рівня"); // Оранжевый цвет
                    break;

                case ScannerModule2::State::SEARCHING:
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Статус: Пошук"); // Желтый цвет
                    break;

                case ScannerModule2::State::RECEIVING:
                case ScannerModule2::State::WAITING_FOR_AKF:
                    if (_this->_recording.load())
                    {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Статус: Реєстрація"); // Зеленый для записи
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Статус: Приймання"); // Серый для приема
                    }
                    break;

                case ScannerModule2::State::LINGERING:
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Статус: Очікування"); // Серый для ожидания
                    break;

                default:
                    ImGui::Text("Статус: Невідомий");
                    break;
                }
            }
        }

        if (!_this->status_auto_level)
            _this->Averaging = _this->maxLevel;
        if (_this->Averaging != 0)
        {
            ImGui::SameLine();
            int iLevel = round(_this->Averaging); // maxLevel);
            std::string str = "          Рівень: " + std::to_string(_this->intLevel) + " / " + std::to_string(iLevel) + " дБ";
            const char *ccstr = str.c_str();
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", ccstr);
        }

        if (_this->newListOpen)
        {
            _this->newListOpen = _this->newListDialog();
        }
        if (_this->renameListOpen)
        {
            _this->renameListOpen = _this->newListDialog();
        }
        if (_this->onlySaveListOpen)
        {
            _this->onlySaveListOpen = _this->newListSave();
        }
        if (_this->openSrchList)
        {
            _this->openSrchList = _this->getSrchListTabl();
        }
    }
    //=====================================================================================================
    //=====================================================================================================
    //=====================================================================================================

    void cleanupSkipNoise()
    {
        // Определяем время жизни записи (например, 5 минут)
        const auto ttl = std::chrono::minutes(5);
        const auto now = std::chrono::steady_clock::now();

// Используем C++20 `std::erase_if` для удобной очистки
// Если у вас C++17 или старше, используем классический цикл с итератором
#if __cplusplus >= 202002L
        auto num_erased = std::erase_if(skipNoise, [&](const auto &item)
                                        {
            auto const& [freq, timestamp] = item;
            return (now - timestamp) > ttl; });
        if (num_erased > 0)
        {
            flog::info("Cleaned up {0} expired frequencies from skip list.", num_erased);
        }
#else
        // Версия для C++17
        int num_erased = 0;
        for (auto it = skipNoise.begin(); it != skipNoise.end();)
        {
            if ((now - it->second) > ttl)
            {
                it = skipNoise.erase(it);
                num_erased++;
            }
            else
            {
                ++it;
            }
        }
        if (num_erased > 0)
        {
            flog::info("Cleaned up {0} expired frequencies from skip list.", num_erased);
        }
#endif
    }
    //=====================================================================================================
    //=====================================================================================================
    //=====================================================================================================
    bool shouldSkipFrequency(double frequency)
    {
        return skipNoise.find(frequency) != skipNoise.end();
    }

    bool searchInIdentified(double current)
    {
        // Проверка на пропуск по шуму (если она не требует мьютекса, оставляем как есть)
        if (shouldSkipFrequency(current))
        {
            return true;
        }

        if (status_ignor == false)
        {
            return false; // Если не нужно игнорировать, то ничего не пропускаем
        }

        bool is_known = false;

        // 1. Проверяем в "живой" карте найденных частот под мьютексом
        {
            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
            // Выполняем быстрый поиск find() в ОРИГИНАЛЬНОЙ карте. Не копируем!
            if (gui::waterfall.finded_freq.count(current) > 0)
            {
                is_known = true;
            }
        }

        // 2. Если частота ещё не найдена, проверяем в списке "пропускаемых"
        // Если нет, то мьютекс не нужен.
        if (!is_known)
        {
            std::lock_guard<std::mutex> lock(gui::waterfall.skipFreqMutex);
            if (gui::waterfall.skip_finded_freq.count(current) > 0)
            {
                is_known = true;
            }
        }

        // 3. Принимаем решение
        if (is_known)
        {
            // flog::info("TRACE. SKIP... Known frequency: {0}!", current);
            return true; // Пропускаем, так как частота известна
        }
        else
        {
            // flog::info("TRACE. OK! It frequency is new! Current = {0}!", current);
            return false; // Не пропускаем, это новая частота
        }
    }
    //=====================================================================================================
    // Модифицированная функция для получения максимального уровня и оценки шума
    struct LevelInfo
    {
        float maxLevel;   // Максимальный уровень (сигнал)
        float noiseLevel; // Оценка шума (среднее значение нижних уровней)
    };

    LevelInfo getMaxLevelNew(const float *data, double freq, double width, int dataWidth, double wfStart, double wfWidth)
    {
        if (!data || dataWidth <= 0 || wfWidth <= 0)
        {
            return {-150.0f, -150.0f};
        }

        double halfWidth = width / 2.0;
        double low = freq - halfWidth;
        double high = freq + halfWidth;

        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);

        float max = -INFINITY;
        std::vector<float> levels; // Для хранения всех значений в поддиапазоне
        for (int i = lowId; i <= highId; i++)
        {
            if (data[i] > max)
            {
                max = data[i];
            }
            levels.push_back(data[i]);
        }

        if (max == -INFINITY || max < -150.0f)
        {
            max = -150.0f;
        }

        // Оценка шума: среднее значение нижних 50% уровней
        float noiseLevel = -150.0f;
        if (!levels.empty())
        {
            std::sort(levels.begin(), levels.end());
            int mid = static_cast<int>(levels.size()) / 2;

            // noiseLevel = std::accumulate(levels.begin(), levels.begin() + mid, 0.0f) / mid;
            // if (noiseLevel < -150.0f)
            ///    noiseLevel = -150.0f;

            if (mid > 0)
            {
                noiseLevel = std::accumulate(levels.begin(), levels.begin() + mid, 0.0f) / mid;
            }
            else
            {
                // всего один элемент – считаем его шумом
                noiseLevel = levels.front();
            }

            if (noiseLevel < -150.0f)
                noiseLevel = -150.0f;
        }
        return {max, noiseLevel};
    }

    float getMaxLevel(const float *data, double freq, double width, int dataWidth, double wfStart, double wfWidth)
    {
        // Проверка на корректность входных данных
        if (!data || dataWidth <= 0 || width <= 0.0 || wfWidth <= 0.0)
            return -150.0f;

        // Вычисляем границы интересующей полосы частот
        double halfWidth = width / 2.0;
        double lowFreq = freq - halfWidth;
        double highFreq = freq + halfWidth;

        // Преобразуем частоты в индексы массива с округлением
        int lowId = std::clamp<int>(std::round((lowFreq - wfStart) * dataWidth / wfWidth), 0, dataWidth - 1);
        int highId = std::clamp<int>(std::round((highFreq - wfStart) * dataWidth / wfWidth), 0, dataWidth - 1);

        // Защита от ситуации, когда индексы перепутаны
        if (lowId > highId)
            std::swap(lowId, highId);

        // Поиск максимального значения в диапазоне
        float maxLevel = -INFINITY;
        for (int i = lowId; i <= highId; ++i)
        {
            if (data[i] > maxLevel)
                maxLevel = data[i];
        }

        // Ограничим минимальное значение уровня
        if (maxLevel == -INFINITY || maxLevel < -150.0f)
            maxLevel = -150.0f;

        return maxLevel;
    }
    //=====================================================================================================
    // Функция для определения границ поддиапазона
    void calculateScanSegment(double min_frec, double max_frec, const double band,
                              double &sigmentLeft, double &sigmentRight)
    {
        const int minScans = 20; // Минимальное количество сканирований

        // Текущая ширина диапазона
        double range = max_frec - min_frec;

        // Если min_frec == max_frec, расширяем диапазон симметрично от центра
        if (min_frec == max_frec)
        {
            double center = min_frec;
            sigmentLeft = center - (band * minScans / 2.0);
            sigmentRight = center + (band * minScans / 2.0);
            return;
        }

        // Проверяем, достаточно ли диапазона для 20+ сканирований
        if (range >= band * (minScans + 1))
        {
            // Идеальный случай: диапазон больше band * 21
            sigmentLeft = min_frec;
            sigmentRight = max_frec;
        }
        else if (range < band * minScans)
        {
            // Если диапазон меньше минимального (band * 20), расширяем его
            double neededRange = band * minScans;
            double center = (min_frec + max_frec) / 2.0;
            sigmentLeft = center - (neededRange / 2.0);
            sigmentRight = center + (neededRange / 2.0);
        }
        else
        {
            // Диапазон между band * 20 и band * 21 — используем исходные границы
            sigmentLeft = min_frec;
            sigmentRight = max_frec;
        }

        // Убеждаемся, что sigmentRight - sigmentLeft позволяет минимум 20 шагов
        if ((sigmentRight - sigmentLeft) < band * minScans)
        {
            double center = (sigmentLeft + sigmentRight) / 2.0;
            sigmentLeft = center - (band * minScans / 2.0);
            sigmentRight = center + (band * minScans / 2.0);
        }
    }

    //=====================================================================================================
    // Функция сканирования диапазона, возвращающая signalThreshold
    float scanRange(int numSegments, float *data, int dataWidth, double wfStart, double wfWidth, double threshold = 8.0)
    {

        const double scan_band = 12500.0;
        std::vector<double> noiseLevels; // Для хранения уровней шума
        double minSignalLevel = std::numeric_limits<double>::max();

        for (int i = 0; i < numSegments; ++i)
        {
            double startFreq = sigmentLeft + i * scan_band;
            double endFreq = startFreq + scan_band;
            double centerFreq = (startFreq + endFreq) / 2.0;

            // Получаем максимальный уровень и оценку шума
            LevelInfo levels = getMaxLevelNew(data, centerFreq, scan_band, dataWidth, wfStart, wfWidth);
            double signalLevel = static_cast<double>(levels.maxLevel);
            double noiseLevel = static_cast<double>(levels.noiseLevel);

            // Если разница между максимальным уровнем и шумом мала (например, < 10 дБ), считаем это шумом
            if (signalLevel - noiseLevel < 10.0)
            {
                noiseLevels.push_back(noiseLevel);
            }
            else
            {
                // Если есть сильный сигнал, не учитываем этот поддиапазон для шума
                noiseLevels.push_back(-150.0); // Минимальный уровень как заглушка
            }

            // Обновляем минимальный уровень сигнала (для информации)
            if (signalLevel < minSignalLevel)
            {
                minSignalLevel = signalLevel;
            }

            // std::cout << "Поддиапазон " << startFreq << " - " << endFreq
            //          << ": Уровень сигнала = " << signalLevel << " дБ, Уровень шума = " << noiseLevel << " дБ" << std::endl;
        }

        // Вычисление среднего уровня шума (игнорируем поддиапазоны с сильным сигналом)
        double avgNoiseLevel = 0.0;
        int validNoiseCount = 0;
        for (double level : noiseLevels)
        {
            if (level > -150.0)
            { // Учитываем только поддиапазоны с реальным шумом
                avgNoiseLevel += level;
                validNoiseCount++;
            }
        }
        if (validNoiseCount > 0)
        {
            avgNoiseLevel /= validNoiseCount;
        }
        else
        {
            avgNoiseLevel = -150.0; // Если нет данных о шуме
        }

        // Порог для сигнала
        double signalThreshold = avgNoiseLevel + threshold;

        // Вывод результатов
        flog::warn("Порог для отличия сигнала от шума: {0} дБ (шум + {1} дБ), minSignalLevel {2}, avgNoiseLevel {3}", signalThreshold, threshold, minSignalLevel, avgNoiseLevel);
        // std::cout << "\nМинимальный уровень сигнала: " << minSignalLevel << " дБ" << std::endl;
        // std::cout << "Средний уровень шума (без учета сигналов): " << avgNoiseLevel << " дБ" << std::endl;
        // std::cout << "Порог для отличия сигнала от шума: " << signalThreshold << " дБ (шум + " << threshold << " дБ)" << std::endl;

        return static_cast<float>(signalThreshold);
    }
    //=====================================================================================================
    //=====================================================================================================

    double RoundToMode(double _freq)
    {
        // 82500000
        /*
        if (_freq > minFMfreq && _freq < maxFMfreq) {
            _freq = floor((_freq / 100000) + 0.5) * 100000;
        }
        else {
            if (snapInterval < 10000) {
                _freq = _freq;
            }
            else {
                _freq = floor((_freq / 100) + 0.5) * 100;
            }
        }
        */
        if (snapInterval <= 10000)
        {
            _freq = _freq;
        }
        else
        {
            _freq = floor((_freq / 1000) + 0.5) * 1000;
        }
        return _freq;
    }

    //=================================================================================================
    void exportBookmarks(std::string path, json exportedBookmarks)
    {
        std::ofstream fs(path);
        // exportedBookmarks >> fs;
        fs << exportedBookmarks;
        fs.close();
    }

    bool SaveInJsonSrch()
    {
        try
        {
            core::configManager.acquire();
            std::string pathValid_srch = core::configManager.getPath() + "/Banks/";
            std::string pathValid_scan = core::configManager.getPath() + "/Banks/";
            std::string jsonPath = core::configManager.getPath();
            int InstNum = 0;
            try
            {
                jsonPath = core::configManager.conf["PathJson"];
                flog::info("jsonPath '{0}'", jsonPath);
                InstNum = core::configManager.conf["InstanceNum"];
                pathValid_srch = jsonPath + "/Search";
                pathValid_scan = jsonPath + "/Scan";
            }
            catch (const std::exception &e)
            {
                flog::error("Error SaveInJsonSrch {0}", e.what());
                pathValid_srch = core::configManager.getPath() + "/Banks";
                pathValid_scan = pathValid_srch;
            }
            core::configManager.release();

            try
            {
                fs::create_directory(jsonPath);
                fs::create_directory(pathValid_scan);
                fs::create_directory(pathValid_srch);
            }
            catch (const std::exception &e)
            {
                flog::error("Error SaveInJsonSrch create_directory {0}", e.what());
                pathValid_scan = pathValid_srch;
            }

            json rez_exportedBookmarks_srch = json::object();
            json rez_exportedBookmarks_scan = json::object();

            bool _frst = true;
            std::string firsfreq = "2";
            rez_exportedBookmarks_srch["domain"] = "freqs-bank";
            rez_exportedBookmarks_srch["rx-mode"] = "search";
            rez_exportedBookmarks_srch["bank-name"] = selectedListName;

            rez_exportedBookmarks_scan["domain"] = "freqs-bank";
            rez_exportedBookmarks_scan["rx-mode"] = "scanning";
            rez_exportedBookmarks_scan["bank-name"] = selectedListName;

            time_t rawtime;
            struct tm *timeinfo;
            char buffer[80];
            time(&rawtime);
            timeinfo = localtime(&rawtime);
            strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
            std::string s(buffer);

            rez_exportedBookmarks_srch["time_created"] = s;
            rez_exportedBookmarks_srch["InstNum"] = InstNum;
            rez_exportedBookmarks_scan["time_created"] = s;
            rez_exportedBookmarks_scan["InstNum"] = InstNum;

            {
                std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                gui::waterfall.finded_freq_copy = gui::waterfall.finded_freq;
                int i = 0;
                for (const auto &[key, bm] : gui::waterfall.finded_freq_copy)
                {
                    i++;
                    unsigned int _frec = round(bm.frequency);
                    std::string bmfreq = std::to_string(_frec) + ".0";
                    std::string bmName = std::to_string(i);

                    rez_exportedBookmarks_srch["scan"][bmfreq]["frequency"] = bm.frequency;
                    rez_exportedBookmarks_srch["scan"][bmfreq]["bandwidth"] = bm.bandwidth;
                    rez_exportedBookmarks_srch["scan"][bmfreq]["level"] = bm.level;
                    rez_exportedBookmarks_srch["scan"][bmfreq]["mode"] = bm.mode;
                    rez_exportedBookmarks_srch["scan"][bmfreq]["ftime"] = bm.ftime;

                    rez_exportedBookmarks_scan[selectedListName]["bookmarks"][bmName]["frequency"] = bm.frequency;
                    rez_exportedBookmarks_scan[selectedListName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
                    rez_exportedBookmarks_scan[selectedListName]["bookmarks"][bmName]["level"] = bm.level;
                    rez_exportedBookmarks_scan[selectedListName]["bookmarks"][bmName]["mode"] = bm.mode;

                    // flog::info("bm.frequency'{0}'", bm.frequency);
                }
            }

            // flog::info("pathValid '{0}'", pathValid);
            time_t now = time(0);
            using namespace std::chrono;
            uint64_t ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            ms = ms % 1000;
            // std::stringstream ss;
            // ss << now;
            auto lctm = *std::localtime(&now);
            std::stringstream ss;
            ss << std::put_time(&lctm, "%Y%m%d_%H%M%S");
            auto str = ss.str();
            std::string s_ms = std::to_string(ms);
            if (ms < 10)
                s_ms = "00" + s_ms;
            else if (ms < 100)
                s_ms = "0" + s_ms;

            std::string tm = ss.str() + s_ms;
            std::string expname_srch_srch = pathValid_srch + "/" + tm + "_" + selectedListName + "_srch.json";
            std::string expname_srch_scan = pathValid_scan + "/" + tm + "_" + selectedListName + "_scan.json";

            flog::info("expname {0} / {1}. ms = {2}", expname_srch_srch, expname_srch_scan, ms);
            exportBookmarks(expname_srch_srch, rez_exportedBookmarks_srch);
            exportBookmarks(expname_srch_scan, rez_exportedBookmarks_scan);
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
        }
        return true;
    }

    std::string genWavFileName(const std::string _templ, const double current, const int _mode)
    {
        // {yymmdd}-{uxtime_ms}-{freq}-{band}-{receivername}.wav
        //"$y$M$d-$u-$f-$b-$n-$e.wav";
        // Get data
        time_t now = time(0);
        tm *ltm = localtime(&now);
        using namespace std::chrono;
        milliseconds ms = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch());

        std::string templ = _templ;
        double band = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);

        // Format to string
        char freqStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        char bandStr[128];
        char hourStr[128];
        char minStr[128];
        char secStr[128];
        const char *type = "audio";

        sprintf(freqStr, "%.0lf", current);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year - 100);
        // sprintf(bandStr, "%02d", int(band/1000));
        sprintf(bandStr, "%d", int(band));

        sprintf(hourStr, "%02d", ltm->tm_hour);
        sprintf(minStr, "%02d", ltm->tm_min);
        sprintf(secStr, "%02d", ltm->tm_sec);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year + 1900);

        // 230615-1686831173_250-107400000-300-rp10.wav
        // Replace in template
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$u"), std::to_string(ms.count()));
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$e"), demodModeListFile[_mode]);
        templ = std::regex_replace(templ, std::regex("\\$b"), bandStr);
        templ = std::regex_replace(templ, std::regex("\\$n"), thisInstance);
        templ = std::regex_replace(templ, std::regex("\\$t"), type);

        templ = std::regex_replace(templ, std::regex("\\$h"), hourStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), minStr);
        templ = std::regex_replace(templ, std::regex("\\$s"), secStr);
        // templ = std::regex_replace(templ, std::regex("\\$r"), modeStr);
        return templ;
    }

    // Исправленная версия curlPOST_begin
    void curlPOST_begin(std::string fname)
    {
        // --- Шаг 1: Ранний выход, если CURL отключен ---
        bool localUseCurl;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localUseCurl = use_curl;
        }

        if (!localUseCurl)
        {
            return; // Ключевая проверка для 99% случаев
        }

        flog::info("curlPOST_begin: localUseCurl {0}, use_curl {1}", localUseCurl, use_curl);

        // --- Шаг 2: Проверяем остальные условия и атомарно устанавливаем флаг ---
        bool localStatusDirection;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localStatusDirection = status_direction;
        }
        if (!localStatusDirection)
        {
            return;
        }

        bool expected = false;

        if (!Curl_send_begin.compare_exchange_strong(expected, true))
        {
            flog::warn("curlPOST_begin called, but a 'begin' request is already in progress. Ignoring.");
            return;
        }

        flog::info("curlPOST_begin: Flag set to true. Initiating request for {0}", fname);

        // --- Шаг 3: Запускаем асинхронную задачу ---
        std::string url = thisURL + "/begin";
        std::string payload = "fname=" + fname;

        std::thread([url, payload, this]()
                    {
        CURL* curl = curl_easy_init();
        if (!curl) {
            flog::error("curl_easy_init() failed. Resetting flag.");
            this->Curl_send_begin.store(false);
            return;
        }

        char curlErrorBuffer[CURL_ERROR_SIZE];
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            flog::error("curl_easy_perform() for BEGIN failed: {0}. Resetting flag.", curl_easy_strerror(res));
            this->Curl_send_begin.store(false); // Сбрасываем флаг при ошибке
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            flog::info("curl POST 'begin' success to {0} with payload {1}. Server returned: {2}", url, payload, http_code);
            if (http_code != 200) {
                 this->Curl_send_begin.store(false); // Считаем ошибкой, если сервер ответил не 200
            }
        }
        
        curl_easy_cleanup(curl); })
            .detach();
    }

    // Исправленная версия curlPOST_end
    void curlPOST_end(std::string fname)
    {
        // --- Шаг 1: Проверяем, был ли вообще отправлен 'begin' ---
        // Если флаг false, значит, 'begin' не отправлялся или 'end' уже в процессе.
        bool expected = true;
        if (!Curl_send_begin.compare_exchange_strong(expected, false))
        {
            return;
        }

        // --- Шаг 2: Проверяем, включен ли еще CURL (его могли отключить) ---
        bool localUseCurl;
        {
            std::lock_guard<std::mutex> lock(paramMtx);
            localUseCurl = use_curl;
        }
        if (!localUseCurl)
        {
            // flog::warn("curlPOST_end: 'begin' was sent, but CURL is now disabled. No 'end' request will be sent.");
            return; // Важно выйти, если CURL отключили
        }

        flog::info("curlPOST_end: Flag set to false. Initiating request for {0}", fname);

        // --- Шаг 3: Запускаем асинхронную задачу ---
        std::string url = thisURL + "/end";
        std::string payload = "fname=" + fname + "&uxtime=" + utils::unixTimestamp();

        std::thread([=, this]()
                    { 
                        std::string url = thisURL + "/end"; 
                        std::string payload = "fname=" + fname + "&uxtime=" + utils::unixTimestamp();
                        CURL* curl = curl_easy_init();
                        if (!curl) {
                            flog::error("curl_easy_init() for END failed.");
                            return;
                        }                        

                        char curlErrorBuffer[CURL_ERROR_SIZE];
                        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuffer);
                        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                        curl_easy_setopt(curl, CURLOPT_POST, 1L);
                        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
                        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

                        CURLcode res = curl_easy_perform(curl);
                        if (res != CURLE_OK)
                        {
                            flog::error("curl_easy_perform() for END failed: {0}", curl_easy_strerror(res));
                        }
                        else
                        {
                            long http_code = 0;
                            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                            flog::info("curl POST 'end' success to {0} with payload {1}. Server returned: {2}", url, payload, http_code);
                        }

                        curl_easy_cleanup(curl); })
            .detach();
    }
    //                                flog::info("curl -i -X POST  {0} {1}", "http://localhost:18101/event/end", payload.c_str());
    //==================================================================================
    void refreshLists()
    {
        listNames.clear();
        listNamesTxt = "";
        config.acquire();
        int i = 0;
        for (auto [key, list] : config.conf["lists"].items())
        {
            std::string _name = key; // makeUniqueName(key, listNames);
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
            if (i > MAX_BM_SIZE)
                break;
            i++;
        }
        config.release();
    }

    void updateLists()
    {
        config.acquire();
        for (auto it = listNames.begin(); it != listNames.end(); ++it)
        {
            // listNames->first;
            std::string name = *it;
            // flog::error(" delete listName = {0}...", name);
            // config.conf["lists"].erase(name);
        }
        config.release();
    }

    void getSearchLists()
    {
        flog::info("    getSearchLists");

        // 1. ИСПОЛЬЗУЕМ ВЕКТОР вместо сырого указателя
        // Резервируем память, чтобы избежать лишних аллокаций
        std::vector<uint8_t> bbuf;
        bbuf.reserve(32000);

        // 2. ИСПОЛЬЗУЕМ make_unique для структуры
        // Это гарантирует, что память будет очищена, и не нагружает стек
        auto fbm_ptr = std::make_unique<SearchModeList>();
        SearchModeList *fbm = fbm_ptr.get();

        config.acquire();
        // Используем 'list' напрямую из итератора
        for (auto [_name, list] : config.conf["lists"].items())
        {
            // Очищаем структуру нулями перед заполнением (важно для безопасности данных)
            memset(fbm, 0, sizeof(SearchModeList));

            std::string listname = list["listName"];

            // 3. БЕЗОПАСНОЕ КОПИРОВАНИЕ СТРОКИ
            // sizeof(fbm->listName) - 1 гарантирует место под \0
            strncpy(fbm->listName, listname.c_str(), sizeof(fbm->listName) - 1);

            // Обращаемся через 'list', а не config.conf["lists"][_name]
            fbm->_interval = list["_interval"];
            fbm->_level = list["_level"];
            fbm->_passbandRatio = list["_passbandRatio"];
            fbm->_startFreq = list["_startFreq"];
            fbm->_stopFreq = list["_stopFreq"];

            try
            {
                fbm->_mode = list["_mode"];
            }
            catch (...)
            {
                fbm->_mode = 1;
            }
            try
            {
                fbm->_bandwidth = list["_bandwidth"];
            }
            catch (...)
            {
                fbm->_bandwidth = 220000;
            }

            // Логика bool (немного упрощена проверка через contains/value, но оставил вашу логику для совместимости)
            if (list.contains("_status_record") && list["_status_record"] == true)
                fbm->_status_record = true;
            else
                fbm->_status_record = false;

            if (list.contains("_status_direction") && list["_status_direction"] == true)
                fbm->_status_direction = true;
            else
                fbm->_status_direction = false;

            if (list.contains("_status_ignor") && list["_status_ignor"] == true)
                fbm->_status_ignor = true;
            else
                fbm->_status_ignor = false;

            if (list.contains("_status_stop") && list["_status_stop"] == true)
                fbm->_status_stop = true;
            else
                fbm->_status_stop = false;

            fbm->_tuningTime = list["_tuningTime"];
            fbm->_waitingTime = list["_waitingTime"];
            fbm->_lingerTime = list["_lingerTime"];

            if (fbm->_lingerTime >= maxRecDuration * 1000)
            {
                fbm->_lingerTime = maxRecDuration * 1000 - 1;
            }

            try
            {
                fbm->_selectedLogicId = list["_selectedLogicId"];
                fbm->_selectedSrchMode = list["_selectedSrchMode"];
            }
            catch (...)
            {
                fbm->_selectedLogicId = 1;
                fbm->_selectedSrchMode = 0;
            }

            fbm->selected = false;

            // 4. ЗАЩИТА ОТ ПЕРЕПОЛНЕНИЯ БУФЕРА
            // Если следующий элемент не влезает в 32000 (или другой лимит), останавливаемся
            if (bbuf.size() + sizeof(SearchModeList) > 32000)
            {
                flog::warn("getSearchLists: Buffer full (32000 bytes). Truncating list.");
                break;
            }

            // Добавляем данные структуры в конец вектора
            const uint8_t *pData = reinterpret_cast<const uint8_t *>(fbm);
            bbuf.insert(bbuf.end(), pData, pData + sizeof(SearchModeList));
        }
        config.release();

        // 5. ПЕРЕДАЧА ДАННЫХ
        // bbuf.data() возвращает указатель, bbuf.size() возвращает размер
        gui::mainWindow.setbbuf_srch(bbuf.data(), bbuf.size());

        // delete вызывать не нужно, вектор очистится сам

        gui::mainWindow.setselectedLogicId(gui::mainWindow.getCurrServer(), selectedLogicId);
        gui::mainWindow.setidOfList_srch(gui::mainWindow.getCurrServer(), selectedListId);
        gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
        gui::mainWindow.setUpdateListRcv5Srch(MAX_SERVERS, true);

        flog::warn("SEARCH (msgSearch) size: {0}, gui::mainWindow.getUpdateMenuSnd5Srch {1}, selectedListId {2}",
                   bbuf.size(),
                   gui::mainWindow.getUpdateMenuSnd5Srch(CurrSrvr),
                   selectedListId);
    }

    void loadFirst()
    {
        // flog::info("!!!! loadFirst");
        if (listNames.size() > 0)
        {
            loadByName(listNames[0], false);
            return;
        }
        selectedListName = "";
        gui::waterfall.selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName, bool _search)
    {
        // flog::info("!!!! loadByName");
        book.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end())
        {
            selectedListName = "";
            gui::waterfall.selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        gui::waterfall.selectedListName = selectedListName;

        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"].items())
        {
            flog::info("!!!! bmName = {0},  bm[listName] = {1} ", bmName, bm["listName"]);
            SearchMode fbm;
            fbm.listName = bm["listName"];
            fbm._interval = bm["_interval"];
            fbm._level = bm["_level"];
            fbm._passbandRatio = bm["_passbandRatio"];
            fbm._startFreq = bm["_startFreq"];
            fbm._stopFreq = bm["_stopFreq"];
            try
            {
                fbm._mode = bm["_mode"];
            }
            catch (...)
            {
                fbm._mode = 1;
            }
            try
            {
                fbm._bandwidth = bm["_bandwidth"];
            }
            catch (...)
            {
                fbm._bandwidth = 220000;
            }
            fbm._status_record = bm["_status_record"];
            fbm._status_direction = bm["_status_direction"];
            fbm._status_record = bm["_status_record"];
            fbm._status_ignor = bm["_status_ignor"];
            fbm._status_stop = bm["_status_stop"];
            fbm._tuningTime = bm["_tuningTime"];

            fbm._waitingTime = bm["_waitingTime"];
            fbm._lingerTime = bm["_lingerTime"];
            if (fbm._lingerTime >= maxRecDuration * 1000)
            {
                fbm._lingerTime = maxRecDuration * 1000 - 1;
            }

            try
            {
                fbm._selectedLogicId = bm["_selectedLogicId"];
                fbm._selectedSrchMode = bm["_selectedSrchMode"];
            }
            catch (...)
            {
                fbm._selectedLogicId = 0;
                fbm._selectedSrchMode = 0;
            }

            fbm.selected = false;

            flog::info("!!!! Set bmName = {0}, selectedListName = {1}, snapInterval={2}, fbm._startFreq {3} ", bmName, selectedListName, snapInterval, fbm._startFreq);

            if (bmName == selectedListName)
            {
                try
                {
                    mode = config.conf["lists"][selectedListName]["_mode"];
                }
                catch (...)
                {
                    mode = 1;
                }
                try
                {
                    bandwidth = config.conf["lists"][selectedListName]["_bandwidth"];
                }
                catch (...)
                {
                    bandwidth = 220000;
                }
                // flog::info("!!!! Set bandwidthsList.size() = {0}, bandwidth = {1}, _bandwidthId={2} ", bandwidthsList.size() , bandwidth, _bandwidthId);

                for (int i = 0; i < bandwidthsList.size(); i++)
                {
                    if ((bandwidthsList[i]) >= bandwidth)
                    {
                        _bandwidthId = i;
                        break;
                    }
                }
                snapInterval = config.conf["lists"][selectedListName]["_interval"];
                intLevel = static_cast<int>(config.conf["lists"][selectedListName]["_level"]);
                // intLevel = round(level);
                passbandRatio = config.conf["lists"][selectedListName]["_passbandRatio"];
                startFreq = config.conf["lists"][selectedListName]["_startFreq"];
                stopFreq = config.conf["lists"][selectedListName]["_stopFreq"];
                startFreqKGz = startFreq / 1000.0;
                stopFreqKGz = stopFreq / 1000.0;

                try
                {
                    selectedLogicId = config.conf["lists"][selectedListName]["_selectedLogicId"];
                    selectedSrchMode = config.conf["lists"][selectedListName]["_selectedSrchMode"];
                }
                catch (...)
                {
                    selectedLogicId = 1;
                    selectedSrchMode = 0;
                }

                try
                {
                    status_direction = config.conf["lists"][selectedListName]["_status_direction"];
                    status_ignor = config.conf["lists"][selectedListName]["_status_ignor"];
                    status_record = config.conf["lists"][selectedListName]["_status_record"];
                    status_stop = config.conf["lists"][selectedListName]["_status_stop"];
                }
                catch (...)
                {
                    status_direction = false;
                    status_ignor = false;
                    status_record = false;
                    status_stop = false;
                }

                tuningTime = config.conf["lists"][selectedListName]["_tuningTime"];
                int _tm = config.conf["lists"][selectedListName]["_lingerTime"];
                lingerTime = _tm / 1000;
                if (lingerTime >= maxRecDuration)
                {
                    lingerTime = maxRecDuration - 1;
                }

                _tm = config.conf["lists"][selectedListName]["_waitingTime"];
                _waitingTime = _tm / 1000;
                selectedIntervalId = 0;
                // if (_search == false) {
                //}
                for (int i = 0; i < snapintervalsList.size(); i++)
                {
                    if (snapintervalsList[i] == snapInterval)
                    {
                        selectedIntervalId = i;
                        break;
                    }
                }
                // flog::info("!!!! Set bmName = {0},  bandwidth = {1}, _bandwidthId={2}, snapInterval={3}, selectedIntervalId {4} ", bmName, bandwidth, _bandwidthId, snapInterval, selectedIntervalId);
            }

            book[bmName] = fbm;

            // flog::info("!!!! bmName = {0},  status_record = {1}, fbm[status_record] ={2} ", bmName, status_record, fbm._status_record);
        }
        config.release();
    }

    bool newListSave()
    {
        // flog::info("!!!! newListSave status_auto_level {0}", status_auto_level);
        config.conf["status_AKF"] = status_AKF;
        config.conf["status_auto_level"] = status_auto_level;
        config.conf["SNRlevel"] = snr_level;

        config.conf["lists"].erase(firstEditedListName);
        json def;
        def = json::object();
        config.acquire();
        def["listName"] = editedListName;
        def["_mode"] = mode;
        def["_bandwidth"] = bandwidth;
        def["_startFreq"] = startFreq;
        def["_stopFreq"] = stopFreq;
        def["_interval"] = snapInterval;
        def["_passbandRatio"] = passbandRatio;
        def["_tuningTime"] = tuningTime;
        def["_waitingTime"] = _waitingTime * 1000;
        def["_lingerTime"] = lingerTime * 1000;
        def["_level"] = intLevel;
        def["_status_stop"] = status_stop;
        def["_status_record"] = status_record;
        def["_status_direction"] = status_direction;
        def["_status_ignor"] = status_ignor;
        def["_selectedLogicId"] = selectedLogicId;
        def["_selectedSrchMode"] = selectedSrchMode;

        config.conf["lists"][editedListName] = def;
        config.release(true);
        refreshLists();
        // flog::info("!!!! newListSave");
        loadByName(editedListName, false);
        getSearchLists();
        gui::mainWindow.setUpdateModule_srch(0, true);

        // flog::info("!!!! getSearchLists()");
        gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
        gui::mainWindow.setUpdateListRcv5Srch(MAX_SERVERS, true);
        gui::mainWindow.setUpdateSrchListForBotton(CurrSrvr, true);
        // gui::mainWindow.setSearchListNamesTxt(CurrSrvr, listNamesTxt);
        gui::mainWindow.setSearchListNamesTxt(listNamesTxt);
        return false;
    }

    bool startListSave()
    {
        // flog::info("!!!! newListSave status_auto_level {0}", status_auto_level);
        config.conf["status_AKF"] = status_AKF;
        config.conf["status_auto_level"] = status_auto_level;
        config.conf["SNRlevel"] = snr_level;
        config.conf["lists"].erase(firstEditedListName);
        json def;
        def = json::object();
        config.acquire();
        def["listName"] = editedListName;
        def["_mode"] = mode;
        def["_bandwidth"] = bandwidth;
        def["_startFreq"] = startFreq;
        def["_stopFreq"] = stopFreq;
        def["_interval"] = snapInterval;
        def["_passbandRatio"] = passbandRatio;
        def["_tuningTime"] = tuningTime;
        def["_waitingTime"] = _waitingTime * 1000;
        def["_lingerTime"] = lingerTime * 1000;
        def["_level"] = intLevel;
        def["_status_stop"] = status_stop;
        def["_status_record"] = status_record;
        def["_status_direction"] = status_direction;
        def["_status_ignor"] = status_ignor;
        def["_selectedLogicId"] = selectedLogicId;
        def["_selectedSrchMode"] = selectedSrchMode;

        config.conf["lists"][editedListName] = def;
        config.release(true);
        refreshLists();
        flog::info("!!!! startListSave");
        loadByName(editedListName, false);
        return true;
    }

    static void applyBookmark2(double _frequency, std::string vfoName)
    {
        if (vfoName == "")
        {
            // TODO: Replace with proper tune call
            gui::waterfall.setPrevCenterFrequency(gui::waterfall.getCenterFrequency());
            gui::waterfall.setCenterFrequency(_frequency);
            // gui::waterfall.centerFreqMoved = true;
        }
        else
        {
            /*
            if (core::modComManager.interfaceExists(vfoName)) {
                if (core::modComManager.getModuleName(vfoName) == "radio") {
                    int mode = bm.mode;
                    double bandwidth = (double)bm.bandwidth;
                    // flog::info("CONTROL4. RADIO_IFACE_CMD_SET_MODE 685");
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }
            */
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, _frequency);
        }
    }

    // Проверка, является ли Unicode-символ допустимым (буква, цифра, пробел)
    bool isAllowedUnicodeChar(uint32_t cp)
    {
        return (cp >= '0' && cp <= '9') ||                             // цифры
               (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || // латиница
               (cp >= 0x0400 && cp <= 0x04FF) ||                       // кириллица
               (cp == ' ') || (cp == '-') || (cp == '_');              // пробел, тире, подчёркивание
    }

    // Простой UTF-8 парсер: очищает строку от "спецсимволов"
    std::string removeSpecialChars(const std::string &input)
    {
        std::string result;
        size_t i = 0;

        while (i < input.size())
        {
            uint32_t cp = 0;
            unsigned char c = input[i];

            if (c < 0x80)
            { // ASCII (1 байт)
                cp = c;
                ++i;
            }
            else if ((c >> 5) == 0x6)
            { // 110xxxxx 10xxxxxx
                if (i + 1 >= input.size())
                    break;
                cp = ((c & 0x1F) << 6) | (input[i + 1] & 0x3F);
                i += 2;
            }
            else if ((c >> 4) == 0xE)
            { // 1110xxxx 10xxxxxx 10xxxxxx
                if (i + 2 >= input.size())
                    break;
                cp = ((c & 0x0F) << 12) | ((input[i + 1] & 0x3F) << 6) | (input[i + 2] & 0x3F);
                i += 3;
            }
            else if ((c >> 3) == 0x1E)
            { // 11110xxx ...
                // слишком редкий случай, скипаем
                i += 4;
                continue;
            }
            else
            {
                // недопустимая последовательность
                ++i;
                continue;
            }

            if (isAllowedUnicodeChar(cp))
            {
                // перекодируем обратно в UTF-8
                if (cp < 0x80)
                {
                    result += static_cast<char>(cp);
                }
                else if (cp < 0x800)
                {
                    result += static_cast<char>((cp >> 6) | 0xC0);
                    result += static_cast<char>((cp & 0x3F) | 0x80);
                }
                else
                {
                    result += static_cast<char>((cp >> 12) | 0xE0);
                    result += static_cast<char>(((cp >> 6) & 0x3F) | 0x80);
                    result += static_cast<char>((cp & 0x3F) | 0x80);
                }
            }
        }

        return result;
    }

    // Генерация уникального имени
    std::string makeUniqueName(const std::string &rawName, const std::vector<std::string> &listNames)
    {
        // Встроенная функция toLower
        auto toLower = [](const std::string &str) -> std::string
        {
            std::string lowerStr;
            lowerStr.reserve(str.size());
            for (char c : str)
            {
                lowerStr += std::tolower(static_cast<unsigned char>(c));
            }
            return lowerStr;
        };
        // Очищенное базовое имя
        std::string baseName = removeSpecialChars(rawName.c_str());
        std::string uniqueName = baseName;
        int counter = 1;

        auto nameExists = [&](const std::string &name)
        {
            std::string lowerName = toLower(name);
            return std::any_of(listNames.begin(), listNames.end(), [&](const std::string &existing)
                               { return toLower(existing) == lowerName; });
        };
        while (nameExists(uniqueName))
        {
            uniqueName = baseName + "(" + std::to_string(counter++) + ")";
        }
        return uniqueName;
    }

    bool getSrchListTabl()
    {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;
        std::string id = "SrchList##scann3_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());
        char nameBuf[32];
        // strcpy(nameBuf, editedListName.c_str());
        // Вместо strcpy
        strncpy(nameBuf, editedListName.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0'; // Гарантируем нуль-терминатор
        {
            std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
            gui::waterfall.finded_freq_copy = gui::waterfall.finded_freq;
        }

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            // ImGui::LeftLabel("Назва");
            ImGui::TextColored(ImVec4(1, 0, 1, 1), "        Результати пошуку:     ");
            if (ImGui::BeginTable("scanner2_rez_draw_ARM", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 210.0f * style::uiScale)))
            { //  , ImVec2(0, 210) | ImGuiTableFlags_ScrollY
                ImGui::TableSetupColumn("Част., МГц");
                ImGui::TableSetupColumn("Вид демод.");
                ImGui::TableSetupColumn("Смуга, кГц");
                ImGui::TableSetupColumn("Сигнал");
                ImGui::TableSetupColumn("Рівень, дБ");
                ImGui::TableSetupColumn("Час");
                ImGui::TableSetupScrollFreeze(2, 1);
                ImGui::TableHeadersRow();

                for (auto &[key, fbm] : gui::waterfall.finded_freq_copy)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    double t_frequency = 0;
                    if (ImGui::Selectable((utils::formatFreqMHz(key) + "##wfscanner2_rez_name_ARM").c_str(), &fbm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick))
                    {
                        if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl)
                        {
                            for (auto &[_name, _bm] : gui::waterfall.finded_freq_copy)
                            {
                                if (key == _name)
                                {
                                    _bm.selected = true;
                                    t_frequency = _bm.frequency;
                                    // flog::info("t_frequency {0}", t_frequency);
                                    continue;
                                }
                                _bm.selected = false;
                                // flog::info("{0}, _name {1}, t_frequency {2}", key, _name, t_frequency);
                            }
                        }
                    }
                    if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        bool t_work = gui::mainWindow.getIfOneButtonStart();
                        flog::info("IsMouseDoubleClicked t_frequency {0}, t_work {1}", t_frequency, t_work);
                        if (t_frequency > 10000 && !t_work)
                            applyBookmark2(t_frequency, gui::waterfall.selectedVFO);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", demodModeList[fbm.mode]);

                    ImGui::TableSetColumnIndex(2);
                    int bw = round(fbm.bandwidth / 1000);
                    std::string sbw = std::to_string(bw);
                    if (bw == 13)
                        sbw = "12.5";
                    if (bw == 6)
                        sbw = "6.25";

                    ImGui::Text("%s", sbw.c_str());
                    ImGui::TableSetColumnIndex(3);
                    std::string signal = "";
                    if (SignalIndf)
                    {
                        if (fbm.Signal == 1)
                            signal = "ТЛФ"; // "Noise";
                        else if (fbm.Signal == 2)
                            signal = "DMR"; // "Noise";
                        else
                            signal = "НВ"; // "Noise";
                    }
                    ImGui::Text("%s", signal.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%s", std::to_string(fbm.level).c_str());
                    ImGui::TableSetColumnIndex(5);
                    char timeString[std::size("hh:mm:ss")];
                    std::strftime(std::data(timeString), std::size(timeString), "%T", std::localtime(&fbm.ftime)); // std::gmtime(&fbm.ftime));
                    ImGui::Text("%s", timeString);
                    // ImVec2 max = ImGui::GetCursorPos();
                }
                ImGui::EndTable();
            }

            float menuWidth = ImGui::GetContentRegionAvail().x;
            ImGui::BeginTable("scanner2_emplytable", 8);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TableSetColumnIndex(1);
            ImGui::TableSetColumnIndex(2);
            ImGui::TableSetColumnIndex(3);
            ImGui::TableSetColumnIndex(4);
            ImGui::TableSetColumnIndex(5);
            ImGui::TableSetColumnIndex(6);
            ImGui::TableSetColumnIndex(7);
            // ImGui::TableSetColumnIndex(8);
            // ImGui::TableSetColumnIndex(9);
            if (ImGui::Button("                    OK                    "))
            {
                open = false;
            }
            ImGui::EndTable();
            ImGui::EndPopup();
        }
        return open;
    }

    bool newListDialog()
    {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##scann3_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[32];
        // strcpy(nameBuf, editedListName.c_str());
        strncpy(nameBuf, editedListName.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0'; // Гарантируем нуль-терминатор

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize))
        {
            ImGui::LeftLabel("Назва");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##scann3_edit_name_3" + name).c_str(), nameBuf, 32))
            {
                editedListName = makeUniqueName(std::string(nameBuf), listNames);
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());
            if (renameListOpen)
                alreadyExists = false;

            if (strlen(nameBuf) == 0)
                alreadyExists = true;

            if (alreadyExists)
            {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("   OK   "))
            {
                open = false;
                flog::info("!!!! status_record = {0} ", status_record);

                if (renameListOpen)
                {
                    //                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.acquire();
                    config.conf["lists"].erase(firstEditedListName);
                    json def;
                    def = json::object();
                    def["listName"] = editedListName;
                    def["_mode"] = mode;
                    def["_bandwidth"] = bandwidth;
                    def["_startFreq"] = startFreq;
                    def["_stopFreq"] = stopFreq;
                    def["_interval"] = snapInterval;
                    def["_passbandRatio"] = passbandRatio;
                    def["_tuningTime"] = tuningTime;
                    def["_waitingTime"] = _waitingTime * 1000;
                    def["_lingerTime"] = lingerTime * 1000;
                    def["_level"] = intLevel;
                    def["_status_stop"] = status_stop;
                    def["_status_record"] = status_record;
                    def["_status_direction"] = status_direction;
                    def["_status_ignor"] = status_ignor;
                    def["_selectedLogicId"] = selectedLogicId;
                    def["_selectedSrchMode"] = selectedSrchMode;

                    // config.conf["lists"][editedListName] = true;
                    config.conf["lists"][editedListName] = def;
                    config.release(true);
                }
                else
                {
                    if (listNames.size() < MAX_BM_SIZE)
                    {
                        config.acquire();
                        json def;
                        def = json::object();
                        def["listName"] = editedListName;
                        def["_mode"] = mode;
                        def["_bandwidth"] = bandwidth;
                        def["_startFreq"] = startFreq;
                        def["_stopFreq"] = stopFreq;
                        def["_interval"] = snapInterval;
                        def["_passbandRatio"] = passbandRatio;
                        def["_tuningTime"] = tuningTime;
                        def["_waitingTime"] = _waitingTime * 1000;
                        def["_lingerTime"] = lingerTime * 1000;
                        def["_level"] = intLevel;
                        def["_status_stop"] = status_stop;
                        def["_status_record"] = status_record;
                        def["_status_direction"] = status_direction;
                        def["_status_ignor"] = status_ignor;
                        def["_selectedLogicId"] = selectedLogicId;
                        def["_selectedSrchMode"] = selectedSrchMode;

                        // config.conf["lists"][editedListName] = true;
                        config.conf["lists"][editedListName] = def;
                        config.release(true);
                    }
                }
                refreshLists();
                flog::info("!!!! newListDialog");
                loadByName(editedListName, false);
                getSearchLists();
                gui::mainWindow.setUpdateModule_srch(CurrSrvr, true);

                // flog::info("!!!! getSearchLists()");
                gui::mainWindow.setUpdateMenuSnd5Srch(MAX_SERVERS, true);
                gui::mainWindow.setUpdateListRcv5Srch(MAX_SERVERS, true);
                gui::mainWindow.setUpdateSrchListForBotton(CurrSrvr, true);
                // gui::mainWindow.setSearchListNamesTxt(CurrSrvr, listNamesTxt);
                gui::mainWindow.setSearchListNamesTxt(listNamesTxt);
            }

            if (alreadyExists)
            {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Скасувати"))
            {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    //=====================================================================================================
    //=====================================================================================================
    //=====================================================================================================

    // Threading and State
    std::atomic<bool> running;
    std::atomic<State> state;
    std::thread workerThread;
    std::thread workerInfoThread;
    std::mutex paramMtx; // Protects access to ALL scanner parameters below that can be changed by UI

    // Scanner Parameters (protected by paramMtx)
    std::string name = "";
    int mode = 1; // NFM
    double startFreq = 102000000.0;
    double stopFreq = 180000000.0;
    int startFreqKGz = 102000;
    int stopFreqKGz = 180000;
    unsigned int snapInterval = 10000;
    double passbandRatio = 10.0;
    int tuningTime = 100;
    int intLevel = -50;
    bool status_record = false;
    int lingerTime = 3;   // in seconds
    int _waitingTime = 2; // in seconds
    bool status_stop = false;
    bool status_ignor = true;
    double bandwidth = 220000.0;
    int _bandwidthId = 6;
    bool status_direction = false;
    int selectedLogicId = 1;
    int selectedSrchMode = 0;
    bool status_AKF = false;
    bool status_auto_level = true;
    int snr_level = 8;

    // Live State Variables (used mostly in worker thread, no mutex needed if only written by worker)
    double current = 0.0;
    float maxLevel = -150.0f;
    std::atomic<bool> _recording = false;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> firstSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> startTimeAKF;
    std::chrono::time_point<std::chrono::high_resolution_clock> init_level_time;
    std::chrono::time_point<std::chrono::high_resolution_clock> akf_confirmation_time;
    std::chrono::steady_clock::time_point last_manual_command_time;

    int skip_for_relevel_counter = 0;
    std::string expandString(std::string input)
    {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    // UI and Config state
    bool enabled = true;
    double last_current = startFreq;
    double _privFreq[3] = {0, 0, 0};
    bool scanUp = true;
    bool reverseLock = false;

    unsigned int _count_freq = 0;
    unsigned int _count_Bookmark = 0; // new
    double curr_bandwidth = 0;

    std::map<double, FindedFreq> listmaxLevel;
    std::map<double, FindedFreq>::iterator itFindedFreq; // new
    bool rez_importOpen = false;
    pfd::open_file *importDialog;
    pfd::save_file *exportDialog;

    std::ofstream logfile;
    std::string expandedLogPath;
    std::string selectedRecorder = "";

    std::vector<std::string> listNames;
    bool deleteListOpen = false;
    int selectedListId = 0;
    std::string selectedListName = "";
    std::string listNamesTxt = "";
    std::map<std::string, SearchMode> book;
    std::map<std::string, SearchMode>::iterator itbook;
    std::string editedListName;
    std::string firstEditedListName;
    bool renameListOpen = false;
    bool newListOpen = false;
    bool onlySaveListOpen = false;
    bool openSrchList = false;

    std::vector<uint32_t> snapintervalsList;
    std::string intervalsListTxt;
    int selectedIntervalId = 0;

    std::string root = (std::string)core::args["root"];
    std::string curr_nameWavFile = "";

    std::string thisURL = "http://localhost:18101/event/";
    std::string thisInstance = "test";

    // new -------------------
    std::vector<std::string> logicList;
    std::string logicListTxt;
    std::vector<std::string> srchModeList;
    std::string srchModeListTxt;

    // std::map<std::string, SearchMode> working_list;
    int _count_list = 0;
    int _curr_list = 0;

    std::vector<uint32_t> bandwidthsList;
    // std::vector<float> bandwidthsList;
    int radioMode = 0;

    std::string txt_error = "";
    bool _error = false;

    int maxRecDuration = 30;
    int maxCountInScanBank = 256;

    bool isARM = false;
    bool isServer = false;
    std::string currSource;
    uint8_t CurrSrvr = 0;

    // new
    bool Admin = false;
    bool SignalIndf = false;
    // float tmp_maxLevels[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    float Averaging = -50;
    // bool Curl_send_begin = false;
    std::atomic<bool> Curl_send_begin{false};
    int maxRecShortDur_sec = 3;
    int WAIT_MS = (maxRecShortDur_sec + 2) * 1000 + 300;
    int MAX_WAIT_MS = (maxRecShortDur_sec) * 1000 + 200;
    int ADD_WAIT_MS = 0;
    bool initial_find_level = true;
    double sigmentLeft, sigmentRight;
    const double scan_band = 12500.0; // Шаг сканирования в Гц
    bool timer_started = false;
    std::chrono::steady_clock::time_point trigger_time;
    bool use_curl = false;
    std::map<double, std::chrono::steady_clock::time_point> skipNoise;
    // Счетчики для гистерезиса
    int signal_lost_counter = 0;
    int signal_returned_counter = 0;
    int _air_recording = 1;
    std::mutex classMtx;
};

// =================================================================================================
// SDR++ MODULE EXPORTS
// =================================================================================================

MOD_EXPORT void _INIT_()
{
    json def = json({});
    def["selectedList"] = "General";
    def["status_AKF"] = false;
    def["status_auto_level"] = true;
    def["SNRlevel"] = 8;
    def["lists"]["General"] = json::object();
    def["lists"]["General"]["listName"] = "General";
    def["lists"]["General"]["_mode"] = 1;
    def["lists"]["General"]["_bandwidth"] = 6;
    def["lists"]["General"]["_startFreq"] = 105000000.0;
    def["lists"]["General"]["_stopFreq"] = 120000000.0;
    def["lists"]["General"]["_interval"] = 10000.0;
    def["lists"]["General"]["_passbandRatio"] = 100.0;
    def["lists"]["General"]["_tuningTime"] = 350;
    def["lists"]["General"]["_status_stop"] = false;
    def["lists"]["General"]["_waitingTime"] = 2 * 1000;
    def["lists"]["General"]["_lingerTime"] = 3 * 1000;
    def["lists"]["General"]["_status_record"] = true;
    def["lists"]["General"]["_status_direction"] = true;
    def["lists"]["General"]["_status_ignor"] = true;
    def["lists"]["General"]["_level"] = -50;
    def["lists"]["General"]["_selectedLogicId"] = 0;
    def["lists"]["General"]["_selectedSrchMode"] = 0;

    config.setPath(core::args["root"].s() + "/search.json");
    config.load(def);
    config.enableAutoSave();

    // Check if of list and convert if they're the old type
    config.acquire();

    for (auto [listName, list] : config.conf["lists"].items())
    {
        //        if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean()) { continue; }
        json newList;
        newList = json::object();
        //        newList["showOnWaterfall"] = true;
        newList[listName] = list;
        //        config.conf[listName] = newList;
        std::string name = config.conf["lists"][listName]["listName"];
        //        flog::info(" listName = {0},  listName = {1} ", listName, name.c_str());
    }

    config.release(true);
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name)
{
    return new ScannerModule2(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void *instance)
{
    delete (ScannerModule2 *)instance;
}

MOD_EXPORT void _END_()
{
    config.disableAutoSave();
    config.save();
}