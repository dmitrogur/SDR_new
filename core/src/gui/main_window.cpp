#include <gui/main_window.h>
#include <gui/gui.h>
#include "imgui.h"
#include <stdio.h>
#include <thread>
#include <complex>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <signal_path/iq_frontend.h>
#include <gui/icons.h>
#include <gui/widgets/bandplan.h>
#include <gui/style.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/menus/source.h>
#include <gui/menus/display.h>
#include <gui/menus/bandplan.h>
#include <gui/menus/sink.h>
#include <gui/menus/vfo_color.h>
#include <gui/menus/module_manager.h>
#include <gui/menus/theme.h>
#include <gui/dialogs/credits.h>
#include <filesystem>
#include <signal_path/source.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/colormaps.h>
#include <gui/widgets/snr_meter.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <gui/smgui.h>

#define DEBUG false
std::string channal = "Канал приймання";
int old_work = 0;
bool _choice = false;

int count_vfos = 0;
int mainVFO = 0;
int selectedListId = 0;
std::string listNamesTxt = "";
std::vector<std::string> listNames;

MainWindow::~MainWindow()
{
    flog::info("MainWindow destructor called. Stopping player if active.");
    if (playing)
    {
        setPlayState(false); // Теперь этот вызов корректен
    }
    // ... (другой код очистки, если он нужен)
}

void MainWindow::init()
{
    LoadingScreen::show("Initializing UI");
    gui::waterfall.init();
    gui::waterfall.setRawFFTSize(fftSize);

    credits::init();

    cleanupStaleInstances();

    core::configManager.acquire();
    json menuElements = core::configManager.conf["menuElements"];
    std::string modulesDir = core::configManager.conf["modulesDirectory"];
    std::string resourcesDir = core::configManager.conf["resourcesDirectory"];
    core::configManager.release();

    int count_vfos = 0;
    int mainVFO = 0;
    listNamesTxt = "";
    listNames.clear();

    for (auto const &[_name, vfo] : gui::waterfall.vfos)
    {
        if (_name == "Канал приймання")
        {
            mainVFO = count_vfos;
            selectedListId = mainVFO;
            // flog::info("Changing VFO. mainVFO =  {0}", mainVFO);
        }
        listNames.push_back(_name);
        listNamesTxt += _name;
        listNamesTxt += '\0';
        count_vfos++;
    }

    // Assert that directories are absolute
    modulesDir = std::filesystem::absolute(modulesDir).string();
    resourcesDir = std::filesystem::absolute(resourcesDir).string();

    // Load menu elements
    gui::menu.order.clear();
    for (auto &elem : menuElements)
    {
        if (!elem.contains("name"))
        {
            flog::error("Menu element is missing name key");
            continue;
        }
        if (!elem["name"].is_string())
        {
            flog::error("Menu element name isn't a string");
            continue;
        }
        if (!elem.contains("open"))
        {
            flog::error("Menu element is missing open key");
            continue;
        }
        if (!elem["open"].is_boolean())
        {
            flog::error("Menu element name isn't a string");
            continue;
        }
        Menu::MenuOption_t opt;
        opt.name = elem["name"];
        opt.open = elem["open"];
        gui::menu.order.push_back(opt);
    }

    gui::menu.registerEntry("Завантаження", sourcemenu::draw, NULL);
    // gui::menu.registerEntry("Радіоприймач", sourcemenu::draw, NULL);

    gui::menu.registerEntry("Виводи ЦП", sinkmenu::draw, NULL);
    //    gui::menu.registerEntry("Частотний план", bandplanmenu::draw, NULL);
    gui::menu.registerEntry("Налаштування", displaymenu::draw, NULL);
    ///    gui::menu.registerEntry("Тема", thememenu::draw, NULL);
    ///    gui::menu.registerEntry("Колір каналу приймання", vfo_color_menu::draw, NULL);
    ///    gui::menu.registerEntry("Менеджер модулів", module_manager_menu::draw, NULL);

    gui::freqSelect.init();

    // Read module config
    isServer = false;
    core::configManager.acquire();
    std::vector<std::string> modules = core::configManager.conf["modules"];
    auto modList = core::configManager.conf["moduleInstances"].items();
    std::string _source = core::configManager.conf["source"];

    bool error = false;
    try
    {
        isServer = core::configManager.conf["IsServer"];
        isARM = core::configManager.conf["IsARM"];
        flog::info("IsServer {0}, IsARM {1}", isServer, isARM);
    }
    catch (const std::exception &e)
    {
        isServer = false;
        isARM = false;
        std::cerr << e.what() << '\n';
    }
    core::configManager.release();

    if (error)
    {
        core::configManager.acquire();
        core::configManager.conf["IsServer"] = isServer;
        core::configManager.conf["IsARM"] = false;
        core::configManager.release(true);
    }
    if (isServer)
        fullConnection[0] = true;
    if (isARM)
        fullConnection[0] = false;
    if (!isServer && !isARM)
        fullConnection[0] = true;

    if (_source == "Airspy" || isARM)
    {
        flog::info("Airspy or ARM. setViewBandwidth({0})", VIEWBANDWICH);
        // Set default values for waterfall in case no source init's it
        gui::waterfall.setBandwidth(MAIN_SAMPLE_RATE); // 8000000);
        gui::waterfall.setViewBandwidth(VIEWBANDWICH); // 8000000);
        gui::mainWindow.setViewBandwidthSlider(0.922);
        fft_in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
        fft_out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
        fftwPlan = fftwf_plan_dft_1d(fftSize, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
        sigpath::iqFrontEnd.init(&dummyStream, MAIN_SAMPLE_RATE, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFTBuffer, releaseFFTBuffer, this);
    }
    else
    {
        // Set default values for waterfall in case no source init's it
        flog::info("NOT Airspy or ARM. setViewBandwidth({0})", 8000000);
        gui::waterfall.setBandwidth(8000000);
        gui::waterfall.setViewBandwidth(8000000);
        fft_in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
        fft_out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
        fftwPlan = fftwf_plan_dft_1d(fftSize, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
        sigpath::iqFrontEnd.init(&dummyStream, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFTBuffer, releaseFFTBuffer, this);
    }

    // sigpath::iqFrontEnd.start();

    vfoCreatedHandler.handler = vfoAddedHandler;
    vfoCreatedHandler.ctx = this;
    sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);

    flog::info("Loading modules");

    // Load modules from /module directory
    if (std::filesystem::is_directory(modulesDir))
    {
        for (const auto &file : std::filesystem::directory_iterator(modulesDir))
        {
            std::string path = file.path().generic_string();
            if (file.path().extension().generic_string() != SDRPP_MOD_EXTENTSION)
            {
                continue;
            }
            if (!file.is_regular_file())
            {
                continue;
            }
            flog::info("Loading {0}", path);
            LoadingScreen::show("Loading " + file.path().filename().string());
            core::moduleManager.loadModule(path);
        }
    }
    else
    {
        flog::warn("Module directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    // Load additional modules specified through config
    for (auto const &path : modules)
    {
        core::moduleManager.loadModule(path);
    }

    // Create module instances
    for (auto const &[name, _module] : modList)
    {
        std::string mod = _module["module"];
        bool enabled = _module["enabled"];
        flog::info("Initializing {0} ({1})", name, mod);
        LoadingScreen::show("Initializing " + name + " (" + mod + ")");
        core::moduleManager.createInstance(name, mod);
        if (!enabled)
        {
            core::moduleManager.disableInstance(name);
        }
    }

    // Load color maps
    LoadingScreen::show("Loading color maps");
    flog::info("Loading color maps");
    if (std::filesystem::is_directory(resourcesDir + "/colormaps"))
    {
        for (const auto &file : std::filesystem::directory_iterator(resourcesDir + "/colormaps"))
        {
            std::string path = file.path().generic_string();
            LoadingScreen::show("Loading " + file.path().filename().string());
            flog::info("Loading {0}", path);
            if (file.path().extension().generic_string() != ".json")
            {
                continue;
            }
            if (!file.is_regular_file())
            {
                continue;
            }
            colormaps::loadMap(path);
        }
    }
    else
    {
        flog::warn("Color map directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    gui::waterfall.updatePalletteFromArray(colormaps::maps["Turbo"].map, colormaps::maps["Turbo"].entryCount);

    sourcemenu::init();
    sinkmenu::init();
    bandplanmenu::init();
    displaymenu::init();
    vfo_color_menu::init();
    module_manager_menu::init();

    // TODO for 0.2.5
    // Fix gain not updated on startup, soapysdr

    // Update UI settings
    LoadingScreen::show("Loading configuration");
    core::configManager.acquire();
    if (isARM)
    {
        for (int serverId = 0; serverId < 8; serverId++)
        {
            std::string keyMax = "fftMax_server_" + std::to_string(serverId);
            std::string keyMin = "fftMin_server_" + std::to_string(serverId);

            if (core::configManager.conf.contains(keyMax))
            {
                fftMax = core::configManager.conf[keyMax];
                server_fftmax[serverId] = fftMax;
            }
            if (core::configManager.conf.contains(keyMin))
            {
                fftMin = core::configManager.conf[keyMin];
                server_fftmin[serverId] = fftMin;
            }
        }
    }
    fftMin = core::configManager.conf["min"];
    fftMax = core::configManager.conf["max"];

    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMax(fftMax);

    double frequency = core::configManager.conf["frequency"];

    showMenu = core::configManager.conf["showMenu"];
    startedWithMenuClosed = !showMenu;

    int64_t old_freq = gui::freqSelect.frequency;
    flog::info("0 frequency {0}, old_freq {1}", frequency, old_freq);

    gui::freqSelect.setFrequency(frequency);
    gui::freqSelect.frequencyChanged = false;
    sigpath::sourceManager.tune(frequency);
    gui::waterfall.setCenterFrequency(frequency);

    bw = 0.922;
    gui::waterfall.vfoFreqChanged = false;
    gui::waterfall.centerFreqMoved = false;
    gui::waterfall.selectFirstVFO();
    menuWidth = core::configManager.conf["menuWidth"];
    newWidth = menuWidth;

    fftHeight = core::configManager.conf["fftHeight"];
    gui::waterfall.setFFTHeight(fftHeight);

    tuningMode = core::configManager.conf["centerTuning"] ? tuner::TUNER_MODE_CENTER : tuner::TUNER_MODE_NORMAL;
    sourceName = core::configManager.conf["source"];

    gui::waterfall.setSource(sourceName);

    if (sourceName == "Азалія-клієнт" || sourceName == "Azalea Client")
    {
        tuningMode = tuner::TUNER_MODE_CENTER;
        core::configManager.conf["centerTuning"] = (bool)tuningMode;
    }
    if (sourceName == "Файл")
    {
        tuningMode = tuner::TUNER_MODE_NORMAL;
        gui::waterfall.VFOMoveSingleClick = false;
        core::configManager.conf["centerTuning"] = false;
    }

    gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);

    core::configManager.release(true);
    // flog::info("6");

    // Correct the offset of all VFOs so that they fit on the screen
    float finalBwHalf = gui::waterfall.getBandwidth() / 2.0;
    for (auto &[_name, _vfo] : gui::waterfall.vfos)
    {
        if (_vfo->lowerOffset < -finalBwHalf)
        {
            sigpath::vfoManager.setCenterOffset(_name, (_vfo->bandwidth / 2) - finalBwHalf);
            continue;
        }
        if (_vfo->upperOffset > finalBwHalf)
        {
            sigpath::vfoManager.setCenterOffset(_name, finalBwHalf - (_vfo->bandwidth / 2));
            continue;
        }
    }

    autostart = core::args["autostart"].b();
    initComplete = true;
    core::moduleManager.doPostInitAll();
    sigpath::iqFrontEnd.start();

    if (isServer)
    {
        //        sigpath::remoteRadio.init(&dummyStream);
        sigpath::remoteRadio.init();
        autostart = true;
    }
}

float *MainWindow::acquireFFTBuffer(void *ctx)
{
    return gui::waterfall.getFFTBuffer();
}

void MainWindow::releaseFFTBuffer(void *ctx)
{
    gui::waterfall.pushFFT();
}

void MainWindow::vfoAddedHandler(VFOManager::VFO *vfo, void *ctx)
{
    MainWindow *_this = (MainWindow *)ctx;
    std::string name = vfo->getName();
    core::configManager.acquire();
    if (!core::configManager.conf["vfoOffsets"].contains(name))
    {
        core::configManager.release();
        return;
    }
    double offset = core::configManager.conf["vfoOffsets"][name];
    core::configManager.release();

    double viewBW = gui::waterfall.getViewBandwidth();
    double viewOffset = gui::waterfall.getViewOffset();

    double viewLower = viewOffset - (viewBW / 2.0);
    double viewUpper = viewOffset + (viewBW / 2.0);

    double newOffset = std::clamp<double>(offset, viewLower, viewUpper);

    sigpath::vfoManager.setCenterOffset(name, _this->initComplete ? newOffset : offset);
}

void MainWindow::restoreUIStateForServer(uint8_t serverId)
{
    flog::info("Restoring UI state for server {0}", serverId);

    // Восстанавливаем FFT Min/Max
    float new_fft_max = getFFTMaxSlider(serverId);
    float new_fft_min = getFFTMinSlider(serverId);

    // Проверяем, что значения не нулевые (если для сервера еще ничего не сохранено)
    if (new_fft_max != 0.0f)
    {
        this->fftMax = new_fft_max;
    }
    if (new_fft_min != 0.0f)
    {
        this->fftMin = new_fft_min;
    }

    // Восстанавливаем Bandwidth
    float new_bw = getViewBandwidthSlider(serverId);
    if (new_bw > 0.0f)
    { // Проверка, что значение валидно
        this->bw = new_bw;

        // Пересчитываем и применяем viewBandwidth
        double factor = (double)this->bw * (double)this->bw;
        if (factor > 0.85)
            factor = 0.85;
        double wfBw = gui::waterfall.getBandwidth();
        double delta = wfBw - 1000.0;
        double finalBw = std::min<double>(1000.0 + factor * delta, wfBw);
        if (finalBw > VIEWBANDWICH)
        {
            finalBw = VIEWBANDWICH;
        }
        gui::waterfall.setViewBandwidth(finalBw);
    }
}

void MainWindow::draw()
{
    // ==== БЛОК ПРОВЕРКИ СМЕНЫ СЕРВЕРА ====
    static uint8_t last_checked_server = getCurrServer();
    uint8_t current_server = getCurrServer();

    if (last_checked_server != current_server)
    {
        flog::warn("DRAW: Active server has changed from {0} to {1}.", last_checked_server, current_server);

        // 1. Восстанавливаем сохраненные настройки для нового сервера
        restoreUIStateForServer(current_server);

        // 2. Если плеер был запущен, перезапускаем его
        if (isPlaying())
        {
            flog::info("DRAW: Restarting player for new server.");
            setPlayState(false);
            pleaseRestartPlayer = true; // Используем флаг, чтобы запустить на следующем кадре
        }

        last_checked_server = current_server;
    }

    // Обработка флага перезапуска
    if (pleaseRestartPlayer.load())
    {
        pleaseRestartPlayer = false;
        // Пауза не нужна, т.к. setPlayState(false) уже был вызван выше
        setPlayState(true);
    }
    /*
    if (pleaseRestartPlayer.load()) {
        pleaseRestartPlayer = false; // Сразу сбрасываем флаг

        // Выполняем полный цикл остановки и запуска, если плеер должен играть
        if (isARMPlaying()) {
            flog::warn("DRAW: Detected player restart request. Restarting...");
            setPlayState(false);
            // Пауза не обязательна, но может помочь
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            setPlayState(true);
        }
    }
    */
    ImGui::Begin("Main", NULL, WINDOW_FLAGS);
    ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    bool _redraw = false;
    bool _work = false;
    bool _change = false;
    // Получаем статус модуля "Запись": идет ли процесс и произошло ли изменение статуса
    core::modComManager.callInterface("Запис", MAIN_GET_PROCESSING, NULL, &_work);
    core::modComManager.callInterface("Запис", MAIN_GET_STATUS_CHANGE, NULL, &_change);
    // Определяем текущий источник (например, удаленный ARM-сервер)
    std::string currSource = sourcemenu::getCurrSource();
    if (currSource == SOURCE_ARM)
    {
        // В режиме удаленного источника ARM `_work` используется для OneButtonStart
        _work = getIfOneButtonStart();
    }
    // Если состояние `_work` изменилось с прошлого кадра, обновляем список каналов
    static bool old_work = false;
    if (old_work != _work)
    {
        if (DEBUG)
        {
            flog::info("TRACE. MainWindow::draw. old_work {0}, _work {1}", old_work, _work);
        }
        channal = "Канал приймання";
        selectedListId = 0;
        count_vfos = 0;
        if (_work)
        {
            // Если началась запись или удаленный сеанс – находим индекс главного канала "Канал приймання"
            for (auto const &[_name, vfoPtr] : gui::waterfall.vfos)
            {
                if (_name == "Канал приймання")
                {
                    selectedListId = count_vfos;
                    flog::info("Changing VFO 5. mainVFO =  {0}", selectedListId);
                    // не прерываем цикл, чтобы подсчитать общее число VFO
                }
                count_vfos++;
            }
            if (DEBUG)
            {
                flog::info("TRACE. MainWindow::draw. 1 selectedListId = {0}", selectedListId);
            }
        }
        else
        {
            // Если запись/сеанс завершились – сбрасываем список VFO до одного главного канала
            if (gui::waterfall.vfos.size() > 0)
            {
                listNames.clear();
                listNames.push_back("Канал приймання");
                listNamesTxt = "Канал приймання";
                listNamesTxt += '\0';
                count_vfos = 1;
                selectedListId = 0;
                mainVFO = 0;
                channal = "Канал приймання";
                flog::info("Changing VFO 3. mainVFO =  {0}", mainVFO);
                // sigpath::sinkManager.setVolumeAndMute(channal, false, -0.1);
                flog::info("Changing VFO 31. mainVFO =  {0}", mainVFO);
            }
        }
    }
    old_work = _work;
    if (currSource == SOURCE_ARM)
    {
        if (pleaseRestartPlayer.load())
        {
            pleaseRestartPlayer = false; // Сразу сбрасываем флаг
            flog::warn("DRAW: Detected player restart request. Restarting...");
            // Выполняем полный цикл остановки и запуска
            setPlayState(false);
            // Может понадобиться небольшая пауза, чтобы все успело остановиться
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            setPlayState(true);
        }
    }

    // Определяем текущий VFO (если выбран)
    ImGui::WaterfallVFO *vfo = NULL;
    if (gui::waterfall.selectedVFO != "")
    {
        vfo = gui::waterfall.vfos[gui::waterfall.selectedVFO];
    }

    // Если идет запись или удаленный сеанс, отключаем интерактивные элементы (делаем их неактивными)
    if (_work)
    {
        style::beginDisabled();
    }

    // Обработка смещения центрального VFO (при изменении centerOffset)
    if (vfo != NULL && vfo->centerOffsetChanged)
    {
        if (!_work)
        { // не выполняем перестройку во время записи
            if (DEBUG)
            {
                flog::info("draw 426  Handle VFO movement");
            }
            // Если включен режим центрирования тюнера, перестраиваем частоту с учетом смещения
            if (tuningMode == tuner::TUNER_MODE_CENTER)
            {
                tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO,
                            gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            }
            // Обновляем отображаемую частоту (частота выбора) с учетом нового смещения VFO
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            gui::freqSelect.frequencyChanged = false;
            // Сохраняем новое смещение VFO в конфигурацию
            core::configManager.acquire();
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            // Минимизируем запись конфигурации: записываем на диск только если не идет запись (_work == false)
            core::configManager.release(!_work);
        }
        // (Не сбрасываем флаг centerOffsetChanged здесь, он будет сброшен после применения частоты)
    }

    if (DEBUG)
    {
        flog::info("TRACE 2");
    }
    // Синхронизируем объекты VFO с параметрами водопада
    sigpath::vfoManager.updateFromWaterfall(&gui::waterfall);
    if (DEBUG)
    {
        flog::info("TRACE 3");
    }

    // Обработка смены активного VFO (если выбран другой VFO на водопаде)
    if (gui::waterfall.selectedVFOChanged && !_work)
    {
        if (DEBUG)
        {
            flog::info("draw 443 ____ gui::freqSelect.setFrequency {0}, generalOffset = {1} ",
                       gui::waterfall.getCenterFrequency(), (vfo ? vfo->generalOffset : 0));
        }
        // Устанавливаем поле ввода частоты на новую частоту выбранного VFO
        if (vfo != NULL)
        {
            gui::freqSelect.setFrequency(vfo->generalOffset + gui::waterfall.getCenterFrequency());
        }
        else
        {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency());
        }
        gui::waterfall.selectedVFOChanged = false;
        gui::freqSelect.frequencyChanged = false;
        // Загружаем режим тюнера (centerTuning) из конфигурации, чтобы обновить поведение однократного щелчка VFO
        core::configManager.acquire();
        tuningMode = core::configManager.conf["centerTuning"] ? tuner::TUNER_MODE_CENTER
                                                              : tuner::TUNER_MODE_NORMAL;
        gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);
        core::configManager.release();
    }

    if (DEBUG)
    {
        flog::info("TRACE 4");
    }

    // Обработка изменения частоты (например, введена вручную в поле ввода)
    if (gui::freqSelect.frequencyChanged)
    {
        gui::freqSelect.frequencyChanged = false;
        core::configManager.acquire();
        tuningMode = core::configManager.conf["centerTuning"] ? tuner::TUNER_MODE_CENTER
                                                              : tuner::TUNER_MODE_NORMAL;
        gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);
        core::configManager.release();
        // Применяем новую частоту тюнеру
        tuner::tune(tuningMode, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
        if (DEBUG)
        {
            flog::info("draw 464 ____ gui::freqSelect.frequencyChanged {0}, generalOffset = {1} ",
                       gui::waterfall.getCenterFrequency(), (vfo ? vfo->generalOffset : 0));
        }
        // Сбрасываем флаги изменения смещения VFO, т.к. новая частота установлена
        if (vfo != NULL)
        {
            vfo->centerOffsetChanged = false;
            vfo->lowerOffsetChanged = false;
            vfo->upperOffsetChanged = false;
        }
        // Сохраняем новую частоту и смещение VFO в конфигурацию
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        if (vfo != NULL)
        {
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
        }
        // Выполняем немедленную запись на диск только если не идет запись (чтобы не вызвать дрожание при записи)
        core::configManager.release(!_work);
    }

    if (DEBUG)
    {
        flog::info("TRACE 5");
    }

    // Обработка перемещения центральной частоты (перетаскивание спектра)
    if (gui::waterfall.centerFreqMoved)
    {
        gui::waterfall.centerFreqMoved = false;
        if (DEBUG)
        {
            flog::info("draw 487   gui::waterfall.centerFreqMoved {0}, generalOffset = {1} ",
                       gui::waterfall.getCenterFrequency(), (vfo ? vfo->generalOffset : 0));
        }
        // Настраиваем источник (радиоприемник) на новую центральную частоту
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        // Если в режиме центрированного тюнинга, обновляем общее смещение VFO, чтобы VFO остался на месте
        tuner::setVFOGeneralOffset(gui::waterfall.getPrevCenterFrequency());
        // Обновляем отображаемую частоту выбора в зависимости от того, есть ли смещение VFO
        if (vfo != NULL)
        {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
        }
        else
        {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency());
        }
        // Сохраняем новую центральную частоту в конфигурацию
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        core::configManager.release(!_work); // не записываем на диск во время записи, чтобы избежать лагов
    }

    if (_work)
    {
        style::endDisabled(); // снова включаем элементы, если они были отключены
    }

    if (DEBUG)
    {
        flog::info("TRACE 6");
    }

    // Отслеживаем изменение высоты FFT-окна (например, при изменении размера окна приложения)
    int _fftHeight = gui::waterfall.getFFTHeight();
    if (fftHeight != _fftHeight)
    {
        fftHeight = _fftHeight;
        core::configManager.acquire();
        core::configManager.conf["fftHeight"] = fftHeight;
        core::configManager.release(true); // сохраняем новую высоту FFT в конфиг (единоразовая запись)
    }

    if (DEBUG)
    {
        flog::info("TRACE 7");
    }

    // Если открыто меню настроек (левая панель), рисуем кнопку "показать/скрыть элементы меню"
    if (showMenu)
    {
        ImVec2 btnSize(30 * style::uiScale, 30 * style::uiScale);
        ImGui::PushID(ImGui::GetID("sdrpp_showMenuElements"));
        if (ImGui::ImageButton(icons::SHOW, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                               5, ImVec4(0, 0, 0, 0), textCol))
        {
            // Переключаем флаг отображения всех элементов меню
            showMenuElements = !showMenuElements;
            _redraw = true;
            // Обновляем список элементов меню: устанавливаем всем open = showMenuElements
            gui::menu.order.clear();
            core::configManager.acquire();
            json menuElements = core::configManager.conf["menuElements"];
            int i = 0;
            for (auto &elem : menuElements)
            {
                if (!elem.contains("name") || !elem.contains("open"))
                {
                    // пропускаем некорректно сохраненные элементы
                    continue;
                }
                Menu::MenuOption_t opt;
                opt.name = elem["name"];
                opt.open = showMenuElements;
                gui::menu.order.push_back(opt);
                i++;
            }
            // Записывать открытость элементов сразу не будем (сделаем при отрисовке меню ниже)
            core::configManager.release(true);
            firstMenuRender = true;
        }
        ImGui::PopID();
    }

    if (DEBUG)
    {
        flog::info("TRACE 8");
    }

    ImGui::SameLine();
    // Кнопка открытия/закрытия левого меню (иконка "MENU")
    ImVec2 btnSize(30 * style::uiScale, 30 * style::uiScale);
    ImGui::PushID(ImGui::GetID("sdrpp_menu_btn"));
    if (ImGui::ImageButton(icons::MENU, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                           5, ImVec4(0, 0, 0, 0), textCol) ||
        ImGui::IsKeyPressed(ImGuiKey_Menu, false))
    {
        showMenu = !showMenu;
        core::configManager.acquire();
        core::configManager.conf["showMenu"] = showMenu;
        core::configManager.release(true); // сохраняем состояние панели меню
    }
    ImGui::PopID();

    ImGui::SameLine();

    // Блок кнопок Play/Stop: логика различается для локального и ARM-режима
    bool tmpPlayState = playing;
    // =================================================================
    // Если текущий источник – удаленный сервер (ARM)
    if (currSource == SOURCE_ARM)
    {
        uint8_t _server = gui::mainWindow.getCurrServer();
        int _control = gui::mainWindow.getServerStatus(_server);
        // Проверяем, нужно ли обновить состояние кнопки воспроизведения
        if (getUpdateMenuRcv0Main(_server))
        {
            setUpdateMenuRcv0Main(_server, false);
            // Если клиент не воспроизводит, но сервер начал передачу – запускаем воспроизведение
            if (!playing && isServerPlaying(_server))
            {
                setPlayState(true);
                setARMPlayState(true);
            }
            // Если локально воспроизводим, а сервер остановился (или рассинхрон с arm_playing) – останавливаем воспроизведение
            else if ((playing && !arm_playing) || (playing && !isServerPlaying(_server)))
            {
                setPlayState(false);
                setARMPlayState(false);
            }
        }
        if (_control < ARM_STATUS_FULL_CONTROL)
        {
            style::beginDisabled(); // если у клиента нет полного управления сервером, делаем кнопку неактивной
        }
        if (arm_playing)
        {
            // Кнопка STOP (для ARM) – останавливает воспроизведение на сервере
            ImGui::PushID(ImGui::GetID("sdrpp_stop_btn"));
            if (ImGui::ImageButton(icons::STOP, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                                   5, ImVec4(0, 0, 0, 0), textCol) ||
                ImGui::IsKeyPressed(ImGuiKey_End, false))
            {
                setARMPlayState(false);
                gui::mainWindow.setUpdateMenuSnd0Main(getCurrServer(), true);
            }
            ImGui::PopID();
        }
        else
        {
            // Кнопка PLAY (для ARM) – запускает воспроизведение на сервере
            ImGui::PushID(ImGui::GetID("sdrpp_play_btn"));
            if (ImGui::ImageButton(icons::PLAY, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                                   5, ImVec4(0, 0, 0, 0), textCol) ||
                ImGui::IsKeyPressed(ImGuiKey_End, false))
            {
                setARMPlayState(true);
                gui::mainWindow.setUpdateMenuSnd0Main(getCurrServer(), true);
            }
            ImGui::PopID();
        }
        if (_control < ARM_STATUS_FULL_CONTROL)
        {
            style::endDisabled();
        }
    }
    // =================================================================
    // Локальный источник (обычный режим)
    else
    {
        if (DEBUG)
        {
            flog::info("TRACE 9");
        }
        // Если кнопка Play заблокирована (например, устройство недоступно) и проигрывание остановлено, делаем кнопку неактивной
        if (playButtonLocked && !tmpPlayState)
        {
            style::beginDisabled();
        }
        if (playing)
        {
            // Кнопка STOP (локально) – остановка воспроизведения
            ImGui::PushID(ImGui::GetID("sdrpp_stop_btn"));
            if (ImGui::ImageButton(icons::STOP, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                                   5, ImVec4(0, 0, 0, 0), textCol) ||
                ImGui::IsKeyPressed(ImGuiKey_End, false))
            {
                setPlayState(false);
            }
            ImGui::PopID();
        }
        else
        {
            // Кнопка PLAY (локально) – запуск воспроизведения
            ImGui::PushID(ImGui::GetID("sdrpp_play_btn"));
            if (ImGui::ImageButton(icons::PLAY, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                                   5, ImVec4(0, 0, 0, 0), textCol) ||
                ImGui::IsKeyPressed(ImGuiKey_End, false))
            {
                setPlayState(true);
            }
            ImGui::PopID();
        }
        if (playButtonLocked && !tmpPlayState)
        {
            style::endDisabled();
        }
        if (DEBUG)
        {
            flog::info("TRACE 10");
        }
    }

    // Обработка автостарта (если необходимо, сейчас autostart всегда false)
    // autostart = false;
    if (autostart)
    {
        autostart = false;
        setPlayState(true);
        flog::info("autostart setPlayState {0}", playing);
    }

    // Если произошло изменение списка VFO (например, добавлен или удален приемник)
    if (_change)
    {
        flog::info("TRACE 726, _work {0}", _work);
        listNamesTxt.clear();
        listNames.clear();
        count_vfos = 0;
        // Пересоздаем список имен VFO
        for (auto const &[_name, vfoPtr] : gui::waterfall.vfos)
        {
            if (_name == "Канал приймання")
            {
                mainVFO = count_vfos;
                selectedListId = mainVFO;
                flog::info("Changing VFO 4. mainVFO =  {0}", mainVFO);
            }
            //    flog::info("Changing VFO 41. _name =  {0}", _name);
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
            count_vfos++;
            // flog::info("Changing VFO 42. _name =  {0}", _name);
        }
        channal = listNames[selectedListId];
        flog::info("TRACE... _change={0}. selectedListId {1}, listNamesTxt = {2}, ",
                   _change, selectedListId, listNamesTxt);
        // sigpath::sinkManager.setVolumeAndMute(channal, false, -0.5);
    }
    _change = false;

    ImGui::SameLine();

    // Поле ввода частоты (gui::freqSelect) - отображаем на том же горизонтальном уровне
    float origY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(origY);
    if (sourceName == "Файл")
    {
        _work = true; // Если источник - файл, ведем себя как при _work (отключаем управление)
    }
    if (_work)
    {
        style::beginDisabled();
    }
    gui::freqSelect.draw(_work);
    if (_work)
    {
        style::endDisabled();
    }
    ImGui::SameLine();

    ImGui::SetCursorPosY(origY);
    // Кнопка переключения режима тюнера (центрирование VFO или нормальный режим)
    if (tuningMode == tuner::TUNER_MODE_CENTER)
    {
        // Если сейчас центрированный режим - показываем кнопку для выключения центрирования (иконка мишени включена)
        ImGui::PushID(ImGui::GetID("sdrpp_ena_st_btn"));
        if (ImGui::ImageButton(icons::CENTER_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                               5, ImVec4(0, 0, 0, 0), textCol))
        {
            // Отключаем центрирование
            tuningMode = tuner::TUNER_MODE_NORMAL;
            gui::waterfall.VFOMoveSingleClick = false;
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = false;
            core::configManager.release(true); // сохраняем режим центрирования (false)
            gui::mainWindow.setUpdateMenuSnd0Main(getCurrServer(), true);
        }
        ImGui::PopID();
    }
    else
    {
        // Если сейчас обычный режим - показываем кнопку для включения центрирования (иконка мишени выключена)
        ImGui::PushID(ImGui::GetID("sdrpp_dis_st_btn"));
        if (ImGui::ImageButton(icons::NORMAL_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                               5, ImVec4(0, 0, 0, 0), textCol))
        {
            // Включаем центрирование тюнера
            tuningMode = tuner::TUNER_MODE_CENTER;
            gui::waterfall.VFOMoveSingleClick = true;
            tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = true;
            core::configManager.release(true); // сохраняем режим центрирования (true)
            gui::mainWindow.setUpdateMenuSnd0Main(getCurrServer(), true);
        }
        ImGui::PopID();
    }

    // Обработка нажатий цифровых клавиш (0-8) для быстрого выбора каналов
    origY = ImGui::GetCursorPosY();
    if (playing)
    {
        if (DEBUG)
        {
            flog::info("TRACE 12");
        }
        struct KeyLayoutData
        {
            const char *Label;
            ImGuiKey Key;
        };
        const KeyLayoutData keys_to_display[] = {
            {"0", ImGuiKey_0}, {"1", ImGuiKey_1}, {"2", ImGuiKey_2}, {"3", ImGuiKey_3}, {"4", ImGuiKey_4}, {"5", ImGuiKey_5}, {"6", ImGuiKey_6}, {"7", ImGuiKey_7}, {"8", ImGuiKey_8}};
        for (int n = 0; n < IM_ARRAYSIZE(keys_to_display); n++)
        {
            const KeyLayoutData *key_data = &keys_to_display[n];
            if (ImGui::IsKeyPressed(key_data->Key))
            {
                flog::warn("[{0}] {1} count_vfos = {2}", key_data->Key, key_data->Label, count_vfos);
                selectedListId = 0;
                // В зависимости от нажатой клавиши выбираем соответствующий VFO (если есть)
                switch (key_data->Key)
                {
                case ImGuiKey_0: // '0' — выбираем последний канал
                    selectedListId = count_vfos - 1;
                    break;
                case ImGuiKey_1: // '1' — первый канал (главный)
                    selectedListId = 0;
                    break;
                case ImGuiKey_2:
                    selectedListId = 1;
                    break;
                case ImGuiKey_3:
                    selectedListId = 2;
                    break;
                case ImGuiKey_4:
                    selectedListId = 3;
                    break;
                case ImGuiKey_5:
                    selectedListId = 4;
                    break;
                case ImGuiKey_6:
                    selectedListId = 5;
                    break;
                case ImGuiKey_7:
                    selectedListId = 6;
                    break;
                case ImGuiKey_8:
                    selectedListId = 7;
                    break;
                default:
                    break;
                }
                if (selectedListId >= count_vfos)
                {
                    selectedListId = count_vfos - 1;
                }
            }
        }
    }

    origY = ImGui::GetCursorPosY();

    // ComboBox со списком каналов (VFO) и ползунок громкости
    ImGui::SameLine();
    ImGui::SetCursorPosY(origY - 40);
    ImGui::SetNextItemWidth(164 * style::uiScale);
    if (DEBUG)
    {
        flog::info("TRACE 014 selectedListId {0}, listNames.size() {1} ", selectedListId, listNames.size());
    }
    channal = listNames[selectedListId];
    if (DEBUG)
    {
        flog::info("TRACE 14  channal {0}", channal);
    }
    // Выпадающий список каналов приема (список VFO)
    if (ImGui::Combo("##aster_list_valume_", &selectedListId, listNamesTxt.c_str()) ||
        selectedListId >= count_vfos)
    {
        flog::info("channal {0}, selectedListId {1}", channal, selectedListId);
        if (selectedListId >= count_vfos)
        {
            selectedListId = count_vfos - 1;
        }
        channal = listNames[selectedListId];
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY(origY - 50);
    // Ползунок громкости для текущего канала (используем внешний менеджер sinkManager)
    sigpath::sinkManager.showVolumeSlider(channal.c_str(), "##_aster_main_volume_",
                                          200 * style::uiScale, btnSize.x, 5, true);

    if (DEBUG)
    {
        flog::info("TRACE 15");
    }

    // Кнопка FFT Hold (удержание пиков спектра)
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowSize().x - (140 * style::uiScale));
    ImGui::SetCursorPosY(5.0f * style::uiScale);
    ImGui::PushID(ImGui::GetID("sdrpp_ffthold_btn"));
    if (ImGui::ImageButton(icons::FFTHOLD, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                           5, ImVec4(0, 0, 0, 0), textCol))
    {
        bool ffthold = displaymenu::getFFTHold();
        if (ffthold)
        {
            // Если режим удержания максимума включен – выключаем его
            displaymenu::setFFTHold(false);
            gui::waterfall.setFFTHold(false);
        }
        else
        {
            // Если выключен – включаем (заморозка текущего спектра)
            displaymenu::setFFTHold(true);
            gui::waterfall.setFFTHold(true);
        }
        ffthold = !ffthold;
        core::configManager.acquire();
        core::configManager.conf["fftHold"] = ffthold;
        core::configManager.release(true); // сохраняем настройку fftHold в конфиг
    }
    ImGui::PopID();

    // Кнопка показа/скрытия водопада (включение/выключение отображения waterfall)
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowSize().x - (100 * style::uiScale));
    ImGui::SetCursorPosY(5.0f * style::uiScale);
    ImGui::PushID(ImGui::GetID("sdrpp_waterfall_btn"));
    if (ImGui::ImageButton(icons::WATERFALL, btnSize, ImVec2(0, 0), ImVec2(1, 1),
                           5, ImVec4(0, 0, 0, 0), textCol))
    {
        bool showWaterfall = displaymenu::getWaterfall();
        if (showWaterfall)
        {
            // Если водопад отображался – скрываем его
            displaymenu::setWaterfall(false);
            gui::waterfall.hideWaterfall();
        }
        else
        {
            // Если был скрыт – показываем
            displaymenu::setWaterfall(true);
            gui::waterfall.showWaterfall();
        }
        showWaterfall = !showWaterfall;
        core::configManager.acquire();
        core::configManager.conf["showWaterfall"] = showWaterfall;
        core::configManager.release(true); // сохраняем настройку showWaterfall
    }
    ImGui::PopID();

    // Кнопка логотипа (открывает окно "О программе")
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowSize().x - (48 * style::uiScale));
    ImGui::SetCursorPosY(10.0f * style::uiScale);
    if (ImGui::ImageButton(icons::LOGO, ImVec2(32 * style::uiScale, 32 * style::uiScale),
                           ImVec2(0, 0), ImVec2(1, 1), 0))
    {
        showCredits = true;
    }
    // Закрываем окно Credits по клику мыши или Esc
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        showCredits = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        showCredits = false;
    }
    if (DEBUG)
    {
        flog::info("TRACE 16");
    }

    // Если открыто окно Credits, блокируем управление водопадом
    lockWaterfallControls = showCredits;

    // Обработка изменения ширины меню (перетаскивание разделителя между колонками)
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 mousePos = ImGui::GetMousePos();
    if (!lockWaterfallControls && showMenu)
    {
        if (DEBUG)
        {
            flog::info("TRACE 14");
        }
        float curY = ImGui::GetCursorPosY();
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        // Отрисовываем линию-разделитель при перетаскивании
        if (grabbingMenu)
        {
            newWidth = mousePos.x;
            newWidth = std::clamp<float>(newWidth, 250.0f, winSize.x - 250.0f);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(newWidth, curY),
                                                    ImVec2(newWidth, winSize.y - 10),
                                                    ImGui::GetColorU32(ImGuiCol_SeparatorActive));
        }
        // Если курсор в зоне разделителя – меняем вид курсора
        if (mousePos.x >= newWidth - (2.0f * style::uiScale) &&
            mousePos.x <= newWidth + (2.0f * style::uiScale) &&
            mousePos.y > curY)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (click)
            {
                grabbingMenu = true;
            }
        }
        else
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        // Если кнопка мыши отпущена после перетаскивания – фиксируем новую ширину меню
        if (!down && grabbingMenu)
        {
            grabbingMenu = false;
            menuWidth = newWidth;
            core::configManager.acquire();
            core::configManager.conf["menuWidth"] = menuWidth;
            core::configManager.release(true); // сохраняем новую ширину меню
        }
    }

    if (showMenu)
    {
        if (DEBUG)
        {
            flog::info("TRACE 15");
        }
        // Разбиваем окно на 3 колонки (меню, водопад, панель управления)
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, menuWidth);
        ImGui::SetColumnWidth(1, std::max<int>(static_cast<int>(winSize.x - menuWidth - (60.0f * style::uiScale)),
                                               static_cast<int>(100.0f * style::uiScale)));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);

        ImGui::BeginChild("Left Column");
        // Отрисовываем левое меню. Если порядок/открытость элементов изменились (_redraw) – сохраняем их конфигурацию
        if (gui::menu.draw(firstMenuRender) || _redraw)
        {
            _redraw = false;
            core::configManager.acquire();
            // Сохраняем порядок и флаги open всех элементов меню в конфиг
            json arr = json::array();
            for (int i = 0; i < gui::menu.order.size(); i++)
            {
                arr[i]["name"] = gui::menu.order[i].name;
                arr[i]["open"] = gui::menu.order[i].open;
            }
            core::configManager.conf["menuElements"] = arr;
            // Также сохраняем включенность/выключенность модулей
            for (auto &[_name, inst] : core::moduleManager.instances)
            {
                if (!core::configManager.conf["moduleInstances"].contains(_name))
                {
                    continue;
                }
                core::configManager.conf["moduleInstances"][_name]["enabled"] = inst.instance->isEnabled();
            }
            core::configManager.release(true);
        }
        // Сбрасываем флаг первого рендера меню
        if (startedWithMenuClosed)
        {
            startedWithMenuClosed = false;
        }
        else
        {
            firstMenuRender = false;
        }
        ImGui::EndChild();
    }
    else
    {
        // Если меню скрыто, все равно создаем 3 колонки, но первая почти пустая
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, 8 * style::uiScale);
        ImGui::SetColumnWidth(1, winSize.x - ((8 + 60) * style::uiScale));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);
    }

    // Правая область: водопад (центральная колонка) и элементы управления (правая колонка)
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::NextColumn();
    ImGui::PopStyleVar();

    // Окно с водопадом (спектр + водопад)
    ImGui::BeginChild("Waterfall");
    gui::waterfall.draw();
    if (DEBUG)
    {
        flog::info("TRACE 16");
    }
    ImGui::EndChild();

    // Если управление водопадом не заблокировано, обрабатываем ввод с клавиатуры/колеса мыши для изменения частоты
    if (!lockWaterfallControls)
    {
        // Обработка стрелок влево/вправо для тонкой настройки частоты (только если курсор над спектром/водопадом)
        if (vfo != NULL && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall))
        {
            bool freqChanged = false;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !gui::freqSelect.digitHovered)
            {
                // Стрелка влево – уменьшаем частоту на один шаг (snapInterval)
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset - vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !gui::freqSelect.digitHovered)
            {
                // Стрелка вправо – увеличиваем частоту на один шаг (snapInterval)
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (freqChanged)
            {
                // Если частота изменена стрелками – сохраняем новую частоту и смещение в конфиг
                core::configManager.acquire();
                core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
                if (vfo != NULL)
                {
                    core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
                }
                // Запись конфигурации на диск только при отсутствии записи (_work == false) во избежание дребезга при записи
                core::configManager.release(!_work);
            }
        }
        // Обработка колесика мыши для изменения частоты
        int wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall))
        {
            double nfreq;
            if (vfo != NULL)
            {
                // В режиме VFO вычисляем шаг прокрутки: Shift ускоряет в 10 раз, Alt замедляет в 10 раз
                double interval;
                if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
                {
                    interval = vfo->snapInterval * 10.0;
                }
                else if (ImGui::IsKeyDown(ImGuiKey_LeftAlt))
                {
                    interval = vfo->snapInterval * 0.1;
                }
                else
                {
                    interval = vfo->snapInterval;
                }
                nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + (interval * wheel);
                nfreq = roundl(nfreq / interval) * interval;
            }
            else
            {
                // Если VFO не используется, прокрутка изменяет центральную частоту пропорционально полосе обзора
                nfreq = gui::waterfall.getCenterFrequency() - (gui::waterfall.getViewBandwidth() * wheel / 20.0);
            }
            tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
            // Обновляем отображаемую частоту в поле ввода
            gui::freqSelect.setFrequency(nfreq);
            // Сохраняем новую частоту (и смещение) в конфигурацию
            core::configManager.acquire();
            core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
            if (vfo != NULL)
            {
                core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            }
            core::configManager.release(!_work); // при записи не выполняем flush на диск
        }
    }

    ImGui::NextColumn();
    ImGui::BeginChild("WaterfallControls");

    // Элементы управления масштабом и уровнем FFT/водопада (справа, вертикальные слайдеры)
    ImVec2 wfSliderSize(20.0f * style::uiScale, 150.0f * style::uiScale);
    if (!gui::mainWindow.getStopMenuUI()) {
        //  style::beginDisabled(); // отключаем управление, если запись активна
        // if (_work)
        //  Надпись "Масш." (масштаб FFT/водопада)
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0f) - (ImGui::CalcTextSize("Масш.").x / 2.0f));
        ImGui::TextUnformatted("Масш.");
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0f) - 10 * style::uiScale);
        // Вертикальный ползунок для масштаба спектра/водопада (bw от 0.0 до 1.0)
        if (ImGui::VSliderFloat("##_7_", wfSliderSize, &bw, 1.0f, 0.0f, "%0.3f"))
        {
            // Если был включен Hold (заморозка спектра), при изменении масштаба отключаем его

            if (displaymenu::getFFTHold())
            {
                displaymenu::setFFTHold(false);
                gui::waterfall.setFFTHold(false);
            }
            double factor = (double)bw * (double)bw;
            // Вычисляем новую полосу обзора (finalBw) с учетом желаемого масштаба

            if (factor > 0.85)
                factor = 0.85;
            setViewBandwidthSlider(bw);    
            double wfBw = gui::waterfall.getBandwidth(); // полоса обзора всего сигнала (максимум)
            double delta = wfBw - 1000.0;
            double finalBw = std::min<double>(1000.0 + factor * delta, wfBw);
            // Ограничиваем полосу FFT/водопада значением 10 МГц, чтобы не превышать эту границу
            
            if (finalBw > VIEWBANDWICH)
            {
                finalBw = VIEWBANDWICH;
            }
            
            gui::waterfall.setViewBandwidth(finalBw);
            if (vfo != NULL)
            {
                gui::waterfall.setViewOffset(vfo->centerOffset); // центрируем основной VFO на экране после изменения масштаба
            }
            /// setViewBandwidthSlider(bw); // сохраняем положение слайдера масштаба (для синхронизации, если необходимо)
        }
        else
        {
            // Если мы в ARM-режиме, синхронизируем положение слайдера масштаба с сервером
            if (currSource == SOURCE_ARM)
            {
                if (bw != getViewBandwidthSlider(currServer))
                {
                    bw = getViewBandwidthSlider(currServer);
                    double factor = (double)bw * (double)bw;
                    if (factor > 0.85)
                        factor = 0.85; // гарантируем factor <= 0.85 (верхний предел)
                    double wfBw = gui::waterfall.getBandwidth();
                    double delta = wfBw - 1000.0;
                    double finalBw = std::min<double>(1000.0 + factor * delta, wfBw);
                    if (finalBw > VIEWBANDWICH)
                    {
                        finalBw = VIEWBANDWICH;
                    }
                    gui::waterfall.setViewBandwidth(finalBw);
                    if (vfo != NULL)
                    {
                        gui::waterfall.setViewOffset(vfo->centerOffset);
                    }
                }
            }
        }

        // if (_work)
        //      style::endDisabled();

        ImGui::NewLine();
        // Надпись "Макс." (верхний уровень FFT)
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0f) - (ImGui::CalcTextSize("Макс.").x / 2.0f));
        ImGui::TextUnformatted("Макс.");
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0f) - 10 * style::uiScale);
        // Ползунок регулировки максимального уровня FFT (fftMax)
        if (ImGui::VSliderFloat("##_8_", wfSliderSize, &fftMax, 0.0f, -160.0f, ""))
        {
            fftMax = std::max<float>(fftMax, fftMin + 10.0f);
            setFFTMaxSlider(fftMax); // Просто сохраняем новое значение
        }
        /*
        if (ImGui::VSliderFloat("##_8_", wfSliderSize, &fftMax, 0.0f, -160.0f, ""))
        {
            float t_fftMax = fftMax;
            fftMax = std::max<float>(t_fftMax, fftMin + 10.0f);
            // Сохраняем новый max только для основного сервера (currServer == 0) и если значение реально изменилось (не было ограничено)
            if (currServer == 0 && t_fftMax == fftMax)
            {
                core::configManager.acquire();
                core::configManager.conf["max"] = fftMax;
                core::configManager.release(true);
            }
            setFFTMaxSlider(fftMax);
        }
        else
        {
            if (currSource == SOURCE_ARM)
            {
                float t_fftMax = getFFTMaxSlider(currServer);
                if (t_fftMax != 0.0f)
                {
                    if (fftMax != t_fftMax)
                    {
                        fftMax = std::max<float>(t_fftMax, fftMin + 10.0f);
                        if (currServer == 0)
                        {
                            core::configManager.acquire();
                            core::configManager.conf["max"] = fftMax;
                            core::configManager.release(true);
                        }
                    }
                }
            }
        }
        */

        ImGui::NewLine();
        // Надпись "Мін." (нижний уровень FFT, на украинском)
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0f) - (ImGui::CalcTextSize("Мін.").x / 2.0f));
        ImGui::TextUnformatted("Мін.");
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0f) - 10 * style::uiScale);
        ImGui::SetItemUsingMouseWheel(); // позволяем менять значение колесом мыши при наведении
        // Ползунок регулировки минимального уровня FFT (fftMin)
        if (ImGui::VSliderFloat("##_9_", wfSliderSize, &fftMin, 0.0f, -160.0f, ""))
        {
            fftMin = std::min<float>(fftMax - 10.0f, fftMin);
            setFFTMinSlider(fftMin); // Просто сохраняем новое значение
            /*
            if (currSource == SOURCE_ARM)
            {
            }
            else
            {
                core::configManager.acquire();
                core::configManager.conf["min"] = fftMin;
                core::configManager.release(true);
            }
            */
        }
    }
    /*
    if (ImGui::VSliderFloat("##_9_", wfSliderSize, &fftMin, 0.0f, -160.0f, ""))
    {
        fftMin = std::min<float>(fftMax - 10.0f, fftMin);
        if (currServer == 0)
        {
            core::configManager.acquire();
            core::configManager.conf["min"] = fftMin;
            core::configManager.release(true);
        }
        setFFTMinSlider(fftMin);
    }
    else
    {
        if (currSource == SOURCE_ARM)
        {
            float t_fftMin = getFFTMinSlider(currServer);
            fftMin = std::min<float>(t_fftMin - 10.0f, fftMin);
            if (t_fftMin != 0.0f)
            {
                if (fftMin != t_fftMin)
                {
                    fftMin = t_fftMin;
                    if (currServer == 0)
                    {
                        core::configManager.acquire();
                        core::configManager.conf["min"] = fftMin;
                        core::configManager.release(true);
                    }
                }
            }
        }
    }
    */
    ImGui::EndChild(); // Завершаем блок "WaterfallControls"
    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

    ImGui::End(); // Завершаем окно "Main"
    if (showCredits)
    {
        credits::show();
    }

    if (demoWindow)
    {
        ImGui::ShowDemoWindow();
    }
}

//------------------------
void MainWindow::setARMPlayState(bool _playing)
{
    arm_playing = _playing;
    flog::info("         arm_playing 1 {0}", arm_playing);
}
void MainWindow::setServerPlayState(uint8_t srv, bool _playing)
{
    server_playing[srv] = _playing;
    // flog::info("server_playing[{0}] {1}", srv, _playing);
    // gui::mainWindow.setUpdateMenuSnd0Main(true);
}
void MainWindow::setPlayState(bool _playing)
{
    if (core::g_isExiting.load() && _playing)
    {
        return;
    }
    if (_playing == playing)
    {
        return;
    }

    // flog::info("!!!!!!!!!! setPlayState_RPM CHANGING to {0} !!!!!!!!!!", _playing);
    playing = _playing;
    
    flog::info(">>>> setPlayState on RPM <<<< playing {0}", playing);

    if (playing)
    {                                   // === ЗАПУСК ===
        sigpath::sourceManager.start(); // Запускаем Airspy

        // ==== НАЧАЛО ИЗМЕНЕНИЯ ====
        if (isServer)
        {
            flog::info(">>>> Explicitly calling remoteRadio.start() on RPM <<<<");
            sigpath::remoteRadio.start(); // ПРИНУДИТЕЛЬНО ЗАПУСКАЕМ СЕТЕВОЙ МОДУЛЬ
        }
        // ==== КОНЕЦ ИЗМЕНЕНИЯ ====

        sigpath::iqFrontEnd.flushInputBuffer();
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        onPlayStateChange.emit(true);
    }
    else
    { // === ОСТАНОВКА ===
        onPlayStateChange.emit(false);

        // ==== НАЧАЛО ИЗМЕНЕНИЯ ====
        if (isServer)
        {
            flog::info(">>>> Explicitly calling remoteRadio.stop() on RPM <<<<");
            sigpath::remoteRadio.stop(); // ПРИНУДИТЕЛЬНО ОСТАНАВЛИВАЕМ СЕТЕВОЙ МОДУЛЬ
        }
        // ==== КОНЕЦ ИЗМЕНЕНИЯ ====

        sigpath::sourceManager.stop(); // Останавливаем Airspy
        sigpath::iqFrontEnd.flushInputBuffer();
    }

    flog::info("!!!!!!!!!! setPlayState_RPM FINISHED. Current state is now {0} !!!!!!!!!!", playing);
}

//============================================================
// Bottom Panel
void MainWindow::setUpdateSrchListForBotton(uint8_t srv, bool val)
{
    UpdateSrchListForBotton[srv] = val;
}
bool MainWindow::getUpdateSrchListForBotton(uint8_t srv)
{
    return UpdateSrchListForBotton[srv];
}
void MainWindow::setUpdateScanListForBotton(uint8_t srv, bool val)
{
    UpdateScanListForBotton[srv] = val;
}
bool MainWindow::getUpdateScanListForBotton(uint8_t srv)
{
    return UpdateScanListForBotton[srv];
}
void MainWindow::setUpdateCTRLListForBotton(uint8_t srv, bool val)
{
    UpdateCTRLListForBotton[srv] = val;
}
bool MainWindow::getUpdateCTRLListForBotton(uint8_t srv)
{
    return UpdateCTRLListForBotton[srv];
}

void MainWindow::setSearchListNamesTxt(std::string val)
{
    SearchList1 = val;
    SearchList2 = val;
    SearchList3 = val;
    SearchList4 = val;
    SearchList5 = val;
    SearchList6 = val;
    SearchList7 = val;
    SearchList8 = val;
}

void MainWindow::setSearchListNamesTxtOld(uint8_t srv, std::string val)
{
    switch (srv)
    {
    case 0:
        SearchList1 = val;
        break;
    case 1:
        SearchList2 = val;
        break;
    case 2:
        SearchList3 = val;
        break;
    case 3:
        SearchList4 = val;
        break;
    case 4:
        SearchList5 = val;
        break;
    case 5:
        SearchList6 = val;
        break;
    case 6:
        SearchList7 = val;
        break;
    case 7:
        SearchList8 = val;
        break;
    }
}
void MainWindow::setScanListNamesTxt(std::string val)
{
    ScanList1 = val;
    ScanList2 = val;
    ScanList3 = val;
    ScanList4 = val;
    ScanList5 = val;
    ScanList6 = val;
    ScanList7 = val;
    ScanList8 = val;
}

void MainWindow::setCTRLListNamesTxt(std::string val)
{
    std::string val2 = val;
    CTRLList1 = val2;
    CTRLList2 = val2;
    CTRLList3 = val2;
    CTRLList4 = val2;
    CTRLList5 = val2;
    CTRLList6 = val2;
    CTRLList7 = val2;
    CTRLList8 = val2;
    // flog::info("      set  CTRLList {0}", CTRLList8.c_str());
}

// -----------------------
std::string MainWindow::getSearchListNamesTxt(uint8_t srv)
{
    std::string SearchList = "";
    switch (srv)
    {
    case 0:
        SearchList = SearchList1;
        break;
    case 1:
        SearchList = SearchList2;
        break;
    case 2:
        SearchList = SearchList3;
        break;
    case 3:
        SearchList = SearchList4;
        break;
    case 4:
        SearchList = SearchList5;
        break;
    case 5:
        SearchList = SearchList6;
        break;
    case 6:
        SearchList = SearchList7;
        break;
    case 7:
        SearchList = SearchList8;
        break;
    }
    return SearchList;
}
std::string MainWindow::getScanListNamesTxt(uint8_t srv)
{
    std::string ScanList = "";
    switch (srv)
    {
    case 0:
        ScanList = ScanList1;
        break;
    case 1:
        ScanList = ScanList2;
        break;
    case 2:
        ScanList = ScanList3;
        break;
    case 3:
        ScanList = ScanList4;
        break;
    case 4:
        ScanList = ScanList5;
        break;
    case 5:
        ScanList = ScanList6;
        break;
    case 6:
        ScanList = ScanList7;
        break;
    case 7:
        ScanList = ScanList8;
        break;
    }
    return ScanList;
}
std::string MainWindow::getCTRLListNamesTxt(uint8_t srv)
{
    std::string CTRLList = "";
    switch (srv)
    {
    case 0:
        CTRLList = CTRLList1;
        break;
    case 1:
        CTRLList = CTRLList2;
        break;
    case 2:
        CTRLList = CTRLList3;
        break;
    case 3:
        CTRLList = CTRLList4;
        break;
    case 4:
        CTRLList = CTRLList5;
        break;
    case 5:
        CTRLList = CTRLList6;
        break;
    case 6:
        CTRLList = CTRLList7;
        break;
    case 7:
        CTRLList = CTRLList8;
        break;
    }
    // flog::info("      get  CTRLList {0} = {1}", srv, CTRLList.c_str());
    return CTRLList;
}
//============================================================
void MainWindow::setServersName(uint8_t srv, std::string name)
{
    flog::info("        setServersName {0} = {1}", srv, name);
    switch (srv)
    {
    case 0:
        Server1Name = name;
        break;
    case 1:
        Server2Name = name;
        break;
    case 2:
        Server3Name = name;
        break;
    case 3:
        Server4Name = name;
        break;
    case 4:
        Server5Name = name;
        break;
    case 5:
        Server6Name = name;
        break;
    case 6:
        Server7Name = name;
        break;
    case 7:
        Server8Name = name;
        break;
    }
}
std::string MainWindow::getServersName(uint8_t srv)
{
    std::string server = "";
    switch (srv)
    {
    case 0:
        server = Server1Name;
        break;
    case 1:
        server = Server2Name;
        break;
    case 2:
        server = Server3Name;
        break;
    case 3:
        server = Server4Name;
        break;
    case 4:
        server = Server5Name;
        break;
    case 5:
        server = Server6Name;
        break;
    case 6:
        server = Server7Name;
        break;
    case 7:
        server = Server8Name;
        break;
    }
    return server;
}
//============================================================
void MainWindow::setVersion(uint8_t srv, std::string name)
{
    // flog::info("        setVersion {0} = {1}", srv, name);
    switch (srv)
    {
    case 0:
        Version1Name = name;
        break;
    case 1:
        Version2Name = name;
        break;
    case 2:
        Version3Name = name;
        break;
    case 3:
        Version4Name = name;
        break;
    case 4:
        Version5Name = name;
        break;
    case 5:
        Version6Name = name;
        break;
    case 6:
        Version7Name = name;
        break;
    case 7:
        Version8Name = name;
        break;
    }
}
std::string MainWindow::getVersion(uint8_t srv)
{
    std::string server = "";
    switch (srv)
    {
    case 0:
        server = Version1Name;
        break;
    case 1:
        server = Version2Name;
        break;
    case 2:
        server = Version3Name;
        break;
    case 3:
        server = Version4Name;
        break;
    case 4:
        server = Version5Name;
        break;
    case 5:
        server = Version6Name;
        break;
    case 6:
        server = Version7Name;
        break;
    case 7:
        server = Version8Name;
        break;
    }
    return server;
}
//============================================================
void MainWindow::setUpdateServerStatus(uint8_t srv, bool vol)
{
    if (vol == ARM_STATUS_FULL_CONTROL)
    {
        for (int i = 0; i < 8; i++)
        {
            updateServerStatus[i] = false;
        }
    }
    updateServerStatus[srv] = vol;
}
bool MainWindow::getUpdateServerStatus(uint8_t srv)
{
    return updateServerStatus[srv];
}

//============================================================
void MainWindow::setServerStatus(uint8_t srv, uint8_t vol)
{
    // if (srv == 1 && vol == 2)
    flog::info("        setServerStatus {0} = {1}", srv, vol);
    if (vol == ARM_STATUS_FULL_CONTROL)
    {
        if (isPlaying())
        {
            setARMPlayState(false);
            setPlayState(false);
            setUpdateMenuSnd0Main(getCurrServer(), true);
        }
    }

    if (vol == ARM_STATUS_FULL_CONTROL)
    {
        for (int i = 0; i < 8; i++)
        {
            if (i != srv && serverStatus[i] == ARM_STATUS_FULL_CONTROL)
            {
                serverStatus[i] = ARM_STATUS_STAT_CONTROL;
                setUpdateServerStatus(i, true);
                flog::info("       UPDATE setServerStatus {0} = {1}", i, (int)ARM_STATUS_STAT_CONTROL);
            }
        }
    }
    if (serverStatus[srv] == ARM_STATUS_FULL_CONTROL && vol != ARM_STATUS_FULL_CONTROL)
    {
        for (int i = 0; i < 8; i++)
        {
            if (i != srv && serverStatus[i] == ARM_STATUS_STAT_CONTROL)
            {
                serverStatus[i] = ARM_STATUS_FULL_CONTROL;
                setUpdateServerStatus(i, true);
                break;
            }
        }
    }

    serverStatus[srv] = vol;
}
int MainWindow::getServerStatus(uint8_t srv)
{
    // flog::info("server1Status {0}", server1Status);
    return serverStatus[srv];
}
//============================================================
void MainWindow::setUpdateMenuSnd0Main(uint8_t srv, bool val)
{
    int i = UpdateMenuSnd0Main[srv];
    // flog::info("setUpdateMenuSnd0Main {0} = {1}, i {2}", srv, val, i);
    if (val == true)
    {
        if (i < 5)
            i++;
    }
    else
    {
        if (i > 0)
            i--;
        else
            i = 0;
    }
    UpdateMenuSnd0Main[srv] = i;
    // UpdateMenuSnd0Main[srv] = val;
}
bool MainWindow::getUpdateMenuSnd0Main(uint8_t srv)
{
    int i = UpdateMenuSnd0Main[srv];
    if (i > 0)
        return true;
    else
        return false;
}
void MainWindow::setUpdateMenuRcv0Main(uint8_t srv, bool val)
{
    // flog::info("setUpdateMenuRcv0Main {0} = {1}", srv, val);
    int i = UpdateMenuRcv0Main[srv];
    // flog::info("setUpdateMenuRcv0Main {0} = {1}, i {2}", srv, val, i);
    if (val == true)
    {
        if (i < 10)
            i++;
    }
    else
    {
        if (i > 1)
            i--;
        else
            i = 0;
    }
    UpdateMenuRcv0Main[srv] = i;
}
bool MainWindow::getUpdateMenuRcv0Main(uint8_t srv)
{
    int i = UpdateMenuRcv0Main[srv];
    if (i > 0)
        return true;
    else
        return false;
    // return UpdateMenuRcv0Main[srv];
}
void MainWindow::setUpdateFreq(bool val)
{
    UpdateFreq = val;
}
bool MainWindow::getUpdateFreq()
{
    return UpdateFreq;
}
void MainWindow::settuningMode(bool val)
{
    tuningMode = val;
}
bool MainWindow::gettuningMode()
{
    return tuningMode;
}
//============================================================
void MainWindow::setGainMode(int val)
{
    changeGain = true;
    gainMode = val;
}
void MainWindow::setLinearGain(int val, bool updt)
{
    changeGain = true;
    linearGain = val;
    if (updt)
    {
        // flog::info("Update Airspy linerGain {0}", linearGain);
        // core::modComManager.callInterface("Airspy", AIRSPY_IFACE_CMD_SET_LINEARGAIN, &linearGain, NULL);
        // score::modComManager.callInterface(selectedVfo, RADIO_IFACE_CMD_SET_MODE, &newMode, NULL);
    }
}
int MainWindow::getGainMode()
{
    return gainMode;
}
int MainWindow::getLinearGain()
{
    return linearGain;
}
//============================================================
void MainWindow::setViewBandwidthSlider(float bandwidth)
{
    // flog::info("Update setViewBandwidthSlider bandwidth {0}, bw {1}", bandwidth, bw);
    bw = bandwidth;
    server_bw[currServer] = bw;
}
float MainWindow::getViewBandwidthSlider(uint8_t srv)
{
    return server_bw[srv];
}

void MainWindow::setFFTMaxSlider(float fft)
{
    server_fftmax[currServer] = fft;
    core::configManager.acquire();
    core::configManager.conf["max"] = fft;
    core::configManager.conf["fftMax_server_" + std::to_string(currServer)] = fft;
    core::configManager.release(true);
}

void MainWindow::setFFTMinSlider(float fft)
{
    server_fftmin[currServer] = fft;
    core::configManager.acquire();
    core::configManager.conf["min"] = fft;
    core::configManager.conf["fftMin_server_" + std::to_string(currServer)] = fft;
    core::configManager.release(true);
}

float MainWindow::getFFTMinSlider(uint8_t srv)
{
    return server_fftmin[srv];
}

float MainWindow::getFFTMaxSlider(uint8_t srv)
{
    return server_fftmax[srv];
}

bool MainWindow::isARMPlaying()
{
    return arm_playing;
}
bool MainWindow::isServerPlaying(uint8_t srv)
{
    return server_playing[srv];
}
bool MainWindow::isPlaying()
{
    return playing;
}
void MainWindow::setFirstMenuRender()
{
    firstMenuRender = true;
}

bool MainWindow::isServerIsNotPlaying(uint8_t srv)
{
    return isNotPlaying[srv];
}

void MainWindow::setServerIsNotPlaying(uint8_t srv, bool val)
{
    isNotPlaying[srv] = val;
}
//============================================================
int MainWindow::getlnaGain()
{
    return lnaGain;
}
int MainWindow::getvgaGain()
{
    return vgaGain;
}

int MainWindow::getmixerGain()
{
    return mixerGain;
}
int MainWindow::getlinearGain()
{
    return linearGain;
}
int MainWindow::getsensitiveGain()
{
    return sensitiveGain;
}
int MainWindow::getgainMode()
{
    return gainMode;
}
bool MainWindow::getlnaAgc()
{
    return lnaAgc;
}
bool MainWindow::getmixerAgc()
{
    return mixerAgc;
}
bool MainWindow::getselect()
{
    return select;
}
bool MainWindow::get_updateLinearGain()
{
    return _updateLinearGain;
}
bool MainWindow::getUpdateMenuRcv()
{
    bool val = UpdateMenuRcv;
    // UpdateMenu = false;
    if (val)
        flog::info("UpdateMenuRcv {0}", UpdateMenuRcv);

    return val;
}
bool MainWindow::getUpdateMenuSnd()
{
    bool val = UpdateMenuSnd;
    // UpdateMenu = false;
    // flog::info("getUpdateMenuSnd {0}", UpdateMenuSnd);
    return val;
}

void MainWindow::setlnaGain(int val)
{
    changeGain = true;
    lnaGain = val;
}
void MainWindow::setvgaGain(int val)
{
    changeGain = true;
    vgaGain = val;
}
void MainWindow::setmixerGain(int val)
{
    changeGain = true;
    mixerGain = val;
}
void MainWindow::setlinearGain(int val)
{
    changeGain = true;
    linearGain = val;
}
void MainWindow::setsensitiveGain(int val)
{
    changeGain = true;
    sensitiveGain = val;
}
void MainWindow::setgainMode(int val)
{
    changeGain = true;
    gainMode = val;
}
void MainWindow::setlnaAgc(bool val)
{
    changeGain = true;
    lnaAgc = val;
}
void MainWindow::setmixerAgc(bool val)
{
    changeGain = true;
    mixerAgc = val;
}
void MainWindow::setselect(bool val)
{
    select = val;
}
void MainWindow::set_updateLinearGain(bool val)
{
    changeGain = true;
    _updateLinearGain = val;
}

void MainWindow::setChangeGainFalse()
{
    changeGain = false;
}
bool MainWindow::getChangeGain()
{
    return changeGain;
}

void MainWindow::setUpdateMenuRcv(bool val)
{
    UpdateMenuRcv = val;
    // if (val)
    //     flog::info("setUpdateMenuSnd {0}", UpdateMenuSnd);
}
void MainWindow::setUpdateMenuSnd(bool val)
{
    UpdateMenuSnd = val;
    // flog::info("setUpdateMenuSnd {0}", UpdateMenuSnd);
}

//===== RADIO ======================
int MainWindow::getselectedDemodID()
{
    return selectedDemodID;
}
float MainWindow::getbandwidth()
{
    return sbandwidth;
}
int MainWindow::getsnapInterval()
{
    return snapInterval;
}
int MainWindow::getsnapIntervalId()
{
    return snapIntervalId;
}
int MainWindow::getdeempId()
{
    return deempId;
}
bool MainWindow::getnbEnabled()
{
    return nbEnabled;
}
float MainWindow::getnbLevel()
{
    return nbLevel;
}
int MainWindow::getCMO_BBand()
{
    return baseband_band;
}
bool MainWindow::getsquelchEnabled()
{
    return squelchEnabled;
}
float MainWindow::getsquelchLevel()
{
    return squelchLevel;
}
bool MainWindow::getFMIFNREnabled()
{
    return FMIFNREnabled;
}
int MainWindow::getfmIFPresetId()
{
    return fmIFPresetId;
}
bool MainWindow::getlowPass()
{
    return _lowPass;
}
bool MainWindow::gethighPass()
{
    return _highPass;
}
float MainWindow::getagcAttack()
{
    return agcAttack;
}
float MainWindow::getagcDecay()
{
    return agcDecay;
}
bool MainWindow::getcarrierAgc()
{
    return carrierAgc;
}
int MainWindow::gettone()
{
    return tone;
}

bool MainWindow::getUpdateMenuRcv2Radio()
{
    bool val = UpdateMenuRcvRadio;
    // flog::info("getUpdateMenuRcv2Radio {0}", UpdateMenuRcvRadio);
    return val;
}
bool MainWindow::getUpdateMODRadio()
{
    return UpdateMODRadio;
}

bool MainWindow::getUpdateMenuSnd2Radio()
{
    bool val = UpdateMenuSndRadio;
    // flog::info("getUpdateMenuSnd2Radio {0}", UpdateMenuSndRadio);
    return val;
}
void MainWindow::setselectedDemodID(int val)
{
    selectedDemodID = val;
}
void MainWindow::setbandwidth(float val)
{
    flog::info("setbandwidth {0}", sbandwidth);
    sbandwidth = val;
}
void MainWindow::setsnapInterval(int val)
{
    snapInterval = val;
}
void MainWindow::setsnapIntervalId(int val)
{
    snapIntervalId = val;
}
void MainWindow::setdeempId(int val)
{
    deempId = val;
}
void MainWindow::setnbEnabled(bool val)
{
    nbEnabled = val;
}
void MainWindow::setnbLevel(float val)
{
    nbLevel = val;
}

void MainWindow::setCMO_BBand(int val)
{
    baseband_band = val;
}

void MainWindow::setsquelchEnabled(bool val)
{
    squelchEnabled = val;
}
void MainWindow::setsquelchLevel(float val)
{
    squelchLevel = val;
}
void MainWindow::setFMIFNREnabled(bool val)
{
    FMIFNREnabled = val;
}
void MainWindow::setfmIFPresetId(int val)
{
    fmIFPresetId = val;
}
void MainWindow::setlowPass(bool val)
{
    _lowPass = val;
}
void MainWindow::sethighPass(bool val)
{
    _highPass = val;
}
void MainWindow::setagcAttack(float val)
{
    agcAttack = val;
}
void MainWindow::setagcDecay(float val)
{
    agcDecay = val;
}
void MainWindow::setcarrierAgc(bool val)
{
    carrierAgc = val;
}
void MainWindow::settone(int val)
{
    tone = val;
}
void MainWindow::setUpdateMenuRcv2Radio(bool val)
{
    // flog::info("setUpdateMenuRcv2Radio {0}", UpdateMenuRcvRadio);
    UpdateMenuRcvRadio = val;
}
void MainWindow::setUpdateMODRadio(bool val)
{
    // flog::info("setUpdateMODRadio {0}", UpdateMODRadio);
    UpdateMODRadio = val;
}

void MainWindow::setUpdateMenuSnd2Radio(bool val)
{
    UpdateMenuSndRadio = val;
    flog::info("setUpdateMenuSnd2Radio {0}", UpdateMenuSndRadio);
}

// RECORD
bool MainWindow::getRecording()
{
    return curr_recording;
}
void MainWindow::setRecording(bool val)
{
    curr_recording = val;
}
bool MainWindow::getServerRecording(int srv)
{
    // if(srv==0)
    //    flog::info("recording[{0}] {1}", srv, recording[srv]);
    return recording[srv];
}
void MainWindow::setServerRecordingStop(int srv)
{
    recording[srv] = false;
    // flog::info("setServerRecordingStop recording{0} {1}", srv, recording[srv]);
    // UpdateMenuSndRecord = true;
}
void MainWindow::setServerRecordingStart(int srv)
{
    recording[srv] = true;
    flog::info("setServerRecordingStart recording{0} {1}", srv, recording[srv]);
    // UpdateMenuSndRecord = true;
}
void MainWindow::setUpdateMenuSnd3Record(bool val)
{
    flog::info("setUpdateMenuSnd3Record {0}", val);
    UpdateMenuSndRecord = val;
}
bool MainWindow::MainWindow::getUpdateMenuSnd3Record()
{
    return UpdateMenuSndRecord;
}
void MainWindow::setUpdateMenuRcv3Record(bool val)
{
    // flog::info("setUpdateMenuRcv3Record {0}", val);
    UpdateMenuRcvRecord = val;
}
bool MainWindow::getUpdateMenuRcv3Record()
{
    return UpdateMenuRcvRecord;
}

// SEARCH
bool MainWindow::getbutton_srch(uint8_t srv)
{
    return button_srch[srv];
}
void MainWindow::setbutton_srch(uint8_t srv, bool val)
{
    // flog::info("setbutton_srch {0}, srv {1}", val, srv);
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            button_srch[i] = val;
        }
    }
    else
        button_srch[srv] = val;
}
bool MainWindow::getUpdateModule_srch(uint8_t srv)
{
    return UpdateModule_srch[srv];
}
void MainWindow::setUpdateModule_srch(uint8_t srv, bool val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            UpdateModule_srch[i] = val;
        }
    }
    else
        UpdateModule_srch[srv] = val;
}

int MainWindow::getidOfList_srch(uint8_t srv)
{
    return idOfList_srch[srv];
}
void MainWindow::setidOfList_srch(uint8_t srv, int val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            idOfList_srch[i] = val;
        }
    }
    else
        idOfList_srch[srv] = val;
    // flog::info("setidOfList_srch idOfList_srch[{0}] = val {1}", srv, val);
}
int MainWindow::getselectedLogicId(uint8_t srv)
{
    return selectedLogicId[srv];
}
void MainWindow::setselectedLogicId(uint8_t srv, int val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            selectedLogicId[i] = val;
        }
    }
    else
        selectedLogicId[srv] = val;
}
int MainWindow::getselectedSrchMode(uint8_t srv)
{
    return selectedSrchMode[srv];
}
void MainWindow::setselectedSrchMode(uint8_t srv, int val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            selectedSrchMode[i] = val;
        }
    }
    else
        selectedSrchMode[srv] = val;
}
int MainWindow::getLevelDbSrch(uint8_t srv)
{
    return LevelDbSrch[srv];
}
void MainWindow::setLevelDbSrch(uint8_t srv, int val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            LevelDbSrch[i] = val;
        }
    }
    else
        LevelDbSrch[srv] = val;
}

int MainWindow::getLevelDbScan(uint8_t srv)
{
    return LevelDbScan[srv];
}
void MainWindow::setLevelDbScan(uint8_t srv, int val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            LevelDbScan[i] = val;
        }
    }
    else
        LevelDbScan[srv] = val;
}

int MainWindow::getLevelDbCtrl(uint8_t srv)
{
    return LevelDbCtrl[srv];
}
void MainWindow::setLevelDbCtrl(uint8_t srv, int val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            LevelDbCtrl[i] = val;
        }
    }
    else
        LevelDbCtrl[srv] = val;
}

int MainWindow::getSNRLevelDb(uint8_t srv)
{
    return SNRLevelDb[srv];
}
void MainWindow::setSNRLevelDb(uint8_t srv, int val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            SNRLevelDb[i] = val;
        }
    }
    else
        SNRLevelDb[srv] = val;
    flog::info("UPDATE. SNRLevelDb[{0}] {1}", 0, SNRLevelDb[0]);
}
bool MainWindow::getAKFInd(uint8_t srv)
{
    return AKFInd[srv];
}
void MainWindow::setAKFInd(uint8_t srv, bool val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            AKFInd[i] = val;
        }
    }
    else
        AKFInd[srv] = val;
}

bool MainWindow::getAKFInd_ctrl(uint8_t srv)
{
    return AKFInd_ctrl[srv];
}
void MainWindow::setAKFInd_ctrl(uint8_t srv, bool val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            AKFInd_ctrl[i] = val;
        }
    }
    else
        AKFInd_ctrl[srv] = val;
}
//==============================================================================
bool MainWindow::getAuto_levelSrch(uint8_t srv)
{
    // flog::info("getAutoLevel_srch[{0}] = {1}", srv, AutoLevel_srch[srv]);
    return AutoLevel_srch[srv];
}
void MainWindow::setAuto_levelSrch(uint8_t srv, bool val)
{
    flog::info("setAuto_levelSrch AutoLevel_srch[{0}] = {1}", srv, val);
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            AutoLevel_srch[i] = val;
        }
    }
    else
        AutoLevel_srch[srv] = val;
    // flog::info("setAuto_levelSrch AutoLevel_srch[0] = {0}", AutoLevel_srch[0]);
}

//==============================================================================
bool MainWindow::getAuto_levelScan(uint8_t srv)
{
    return AutoLevel_scan[srv];
}

void MainWindow::setAuto_levelScan(uint8_t srv, bool val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            AutoLevel_scan[i] = val;
        }
    }
    else
        AutoLevel_scan[srv] = val;
    // flog::info("setAuto_levelScan AutoLevel_scan[0] = {0}", AutoLevel_scan[0]);
}

//==============================================================================
bool MainWindow::getAuto_levelCtrl(uint8_t srv)
{
    return AutoLevel_ctrl[srv];
}
void MainWindow::setAuto_levelCtrl(uint8_t srv, bool val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            AutoLevel_ctrl[i] = val;
        }
    }
    else
        AutoLevel_ctrl[srv] = val;
}
//==============================================================================
void MainWindow::setUpdateListRcv5Srch(uint8_t srv, bool val)
{
    // updateListRcv5Srch[srv] = val;
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            updateListRcv5Srch[i] = val;
        }
    }
    else
        updateListRcv5Srch[srv] = val;
}
bool MainWindow::getUpdateListRcv5Srch(uint8_t srv)
{
    return updateListRcv5Srch[srv];
}
void MainWindow::setUpdateMenuSnd5Srch(uint8_t srv, bool val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            UpdateMenuSndSearch[i] = val;
        }
    }
    else
        UpdateMenuSndSearch[srv] = val;
    // flog::info("setUpdateMenuSnd5Srch val {0}, val {1}", srv, val);
}
bool MainWindow::getUpdateMenuSnd5Srch(uint8_t srv)
{
    // flog::info("getUpdateMenuSnd5Srch UpdateMenuSndSearch {0}", UpdateMenuSndSearch);
    return UpdateMenuSndSearch[srv];
}
void MainWindow::setUpdateMenuRcv5Srch(bool val)
{
    // flog::info("        setUpdateMenuRcv5Srch UpdateMenuSndSearch {0}", val);
    UpdateMenuRcvSearch = val;
}
bool MainWindow::getUpdateMenuRcv5Srch()
{
    return UpdateMenuRcvSearch;
}

// SEARCH STAT
void MainWindow::setUpdateStatSnd5Srch(bool val)
{
    flog::info("setUpdateStatSnd5Srch UpdateStatSndSearch {0}", UpdateStatSndSearch);
    UpdateStatSndSearch = val;
}
bool MainWindow::getUpdateStatSnd5Srch()
{
    return UpdateStatSndSearch;
}
/*
void MainWindow::setUpdateStatRcv5Srch(bool val) {
    UpdateStatRcvSearch = val;
}
bool MainWindow::getUpdateStatRcv5Srch() {
    return UpdateStatRcvSearch;
}
*/

// SCAN
bool MainWindow::getbutton_scan(uint8_t srv)
{
    return button_scan[srv];
}
void MainWindow::setbutton_scan(uint8_t srv, bool val)
{
    // flog::info("    setbutton_scan {0}", val);
    button_scan[srv] = val;
}
int MainWindow::getidOfList_scan(uint8_t srv)
{
    // flog::info("    getidOfList_scan {0} = {1}", srv, idOfList_scan[srv]);
    return idOfList_scan[srv];
}
void MainWindow::setidOfList_scan(uint8_t srv, int val)
{
    // flog::info("    setidOfList_scan {0} = {1}", srv, val);
    idOfList_scan[srv] = val;
}
int MainWindow::getMaxRecWaitTime_scan(uint8_t srv)
{
    return maxRecWaitTime_scan[srv];
}
int MainWindow::getMaxRecDuration_scan(uint8_t srv)
{
    return maxRecDuration_scan[srv];
}

void MainWindow::setMaxRecWaitTime_scan(uint8_t srv, int val)
{
    // flog::info("    maxRecDuration {0}", val);
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            maxRecWaitTime_scan[i] = val;
        }
    }
    else
        maxRecWaitTime_scan[srv] = val;
}
void MainWindow::setMaxRecDuration_scan(uint8_t srv, int val)
{
    // flog::info("    maxRecDuration {0}", val);
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            maxRecDuration_scan[i] = val;
        }
    }
    else
        maxRecDuration_scan[srv] = val;
}

bool MainWindow::getUpdateModule_scan(uint8_t srv)
{
    return UpdateModule_scan[srv];
}
void MainWindow::setUpdateModule_scan(uint8_t srv, bool val)
{
    UpdateModule_scan[srv] = val;
}
void MainWindow::setUpdateListRcv6Scan(uint8_t srv, bool val)
{
    //    updateListRcv6Scan[srv] = val;
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            updateListRcv6Scan[i] = val;
        }
    }
    else
        updateListRcv6Scan[srv] = val;
}
bool MainWindow::getUpdateListRcv6Scan(uint8_t srv)
{
    return updateListRcv6Scan[srv];
}

void MainWindow::setUpdateMenuSnd6Scan(uint8_t srv, bool val)
{
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            UpdateMenuSndScan[i] = val;
        }
    }
    else
        UpdateMenuSndScan[srv] = val;
}
bool MainWindow::getUpdateMenuSnd6Scan(uint8_t srv)
{
    return UpdateMenuSndScan[srv];
}
void MainWindow::setUpdateMenuRcv6Scan(bool val)
{
    UpdateMenuRcvScan = val;
}
bool MainWindow::getUpdateMenuRcv6Scan()
{
    return UpdateMenuRcvScan;
}

// CONTROl
int MainWindow::getMaxRecWaitTime_ctrl(uint8_t srv)
{

    return maxRecWaitTime_ctrl[srv];
}
void MainWindow::setMaxRecWaitTime_ctrl(uint8_t srv, int val)
{
    // flog::info("    maxRecDuration {0}", val);
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            maxRecWaitTime_ctrl[i] = val;
        }
    }
    else
        maxRecWaitTime_ctrl[srv] = val;
}
void MainWindow::setflag_level_ctrl(uint8_t srv, bool val)
{
    flag_level_ctrl[srv] = val;
}
// void MainWindow::setlevel_ctrl(uint8_t srv, int val) {
//     level_ctrl[srv] = val;
// }
bool MainWindow::getflag_level_ctrl(uint8_t srv)
{
    return flag_level_ctrl[srv];
}
// int MainWindow::getlevel_ctrl(uint8_t srv) {
//     return level_ctrl[srv];
// }
bool MainWindow::getbutton_ctrl(uint8_t srv)
{
    return button_ctrl[srv];
}
void MainWindow::setbutton_ctrl(uint8_t srv, bool val)
{
    // flog::info("[MW] setbutton_ctrl srv {0}, val {1}", srv, val);
    button_ctrl[srv] = val;
}
bool MainWindow::getupdateStart_ctrl(uint8_t srv)
{
    return start_ctrl[srv];
}
void MainWindow::setupdateStart_ctrl(uint8_t srv, bool val)
{
    start_ctrl[srv] = val;
}

int MainWindow::getidOfList_ctrl(uint8_t srv)
{
    return idOfList_ctrl[srv];
}
void MainWindow::setidOfList_ctrl(uint8_t srv, int val)
{
    flog::info("UPDATE {0}. setidOfList_ctrl = {1}", srv, val);
    idOfList_ctrl[srv] = val;
}
bool MainWindow::getUpdateModule_ctrl(uint8_t srv)
{
    return UpdateModule_ctrl[srv];
}
void MainWindow::setUpdateModule_ctrl(uint8_t srv, int val)
{
    // UpdateModule_ctrl[srv] = val;
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            UpdateModule_ctrl[i] = val;
        }
    }
    else
        UpdateModule_ctrl[srv] = val;
}
void MainWindow::setUpdateListRcv7Ctrl(uint8_t srv, bool val)
{
    // flog::info("    setUpdateListRcv7Ctrl [{0}] = {1};", srv, val);
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            updateListRcv7Ctrl[i] = val;
        }
    }
    else
        updateListRcv7Ctrl[srv] = val;
}
bool MainWindow::getUpdateListRcv7Ctrl(uint8_t srv)
{
    return updateListRcv7Ctrl[srv];
}
void MainWindow::setUpdateMenuSnd7Ctrl(uint8_t srv, bool val)
{
    // flog::info("    setUpdateMenuSnd7Ctrl [{0}] = {1};", srv, val);
    if (srv == MAX_SERVERS)
    {
        for (int i = 0; i < MAX_SERVERS; i++)
        {
            UpdateMenuSndCtrl[i] = val;
        }
    }
    else
        UpdateMenuSndCtrl[srv] = val;
}
bool MainWindow::getUpdateMenuSnd7Ctrl(uint8_t srv)
{
    return UpdateMenuSndCtrl[srv];
}
void MainWindow::setUpdateMenuRcv7Ctrl(bool val)
{
    UpdateMenuRcvCtrl = val;
}
bool MainWindow::getUpdateMenuRcv7Ctrl()
{
    return UpdateMenuRcvCtrl;
}

void MainWindow::setFullConnection(uint8_t srv, bool val)
{
    fullConnection[srv] = val;
    flog::info("    setFullConnection [{0}] = {1};", srv, fullConnection[srv]);
};
bool MainWindow::getFullConnection(uint8_t srv)
{
    // flog::info("    getFullConnection [{0}] = {1};", srv, fullConnection[srv]);
    return fullConnection[srv];
};

void MainWindow::setServerSampleRate(uint8_t srv, double val)
{
    serverSampleRate[srv] = val;
}
double MainWindow::getServerSampleRate(uint8_t srv)
{
    return serverSampleRate[srv];
}

bool MainWindow::getIfOneButtonStart()
{
    bool work = false;
    bool button_srch = getbutton_srch(getCurrServer());
    bool button_scan = getbutton_scan(getCurrServer());
    bool button_ctrl = getbutton_ctrl(getCurrServer());
    bool recording = getServerRecording(getCurrServer());
    bool fllConn = getFullConnection(getCurrServer()); //  || !fllConn
    if (button_srch || button_scan || button_ctrl || recording || !fllConn)
    {
        work = true;
        // flog::info("button_srch {0}, button_scan {1}, button_ctrl {2}, recording {3}, fllConn {4}", button_srch, button_scan, button_ctrl, recording, fllConn);
    }
    return work;
}

bool MainWindow::getButtonStart(uint8_t srv)
{
    bool work = false;
    bool button_srch = getbutton_srch(srv);
    bool button_scan = getbutton_scan(srv);
    bool button_ctrl = getbutton_ctrl(srv);
    bool recording = getServerRecording(srv);
    bool fllConn = getFullConnection(srv); //  || !fllConn
    if (button_srch || button_scan || button_ctrl || recording || !fllConn)
    {
        work = true;
        // flog::info("getButtonStart. button_srch {0}, button_scan {1}, button_ctrl {2}, recording {3}. srv = {4}", button_srch, button_scan, button_ctrl, recording, srv );
    }
    return work;
}

void MainWindow::cleanupStaleInstances()
{
    flog::info("[MW+supervision4] Performing cleanup of stale module instances...");

    bool config_was_modified = false;

    // Блокируем главный config.json для безопасного изменения
    core::configManager.acquire();

    try
    {
        // Проверяем, что секция вообще существует
        if (core::configManager.conf.contains("moduleInstances"))
        {

            auto &instances = core::configManager.conf["moduleInstances"];
            std::vector<std::string> keys_to_erase;

            // ШАГ 1: Собрать ключи для удаления.
            // Нельзя удалять элементы из json, пока вы по нему итерируетесь.
            for (auto &[key, value] : instances.items())
            {
                // Проверяем имена "C1", "C2", ..., "C<MAX_CHANNELS>"
                if (key.rfind("C", 0) == 0)
                { // Проверяем, что строка начинается с "C"
                    std::string number_part = key.substr(1);
                    // Проверяем, что после "C" идет число
                    if (!number_part.empty() && std::all_of(number_part.begin(), number_part.end(), ::isdigit))
                    {
                        keys_to_erase.push_back(key);
                        continue; // Переходим к следующему ключу
                    }
                }

                // Проверяем имена "Запис C1", "Запис C2", ...
                if (key.rfind("Запис C", 0) == 0)
                {                                                                // Проверяем, что строка начинается с "Запис C"
                    std::string number_part = key.substr(sizeof("Запис C") - 1); // sizeof считает '\0'
                                                                                 // Проверяем, что после "Запис C" идет число
                    if (!number_part.empty() && std::all_of(number_part.begin(), number_part.end(), ::isdigit))
                    {
                        keys_to_erase.push_back(key);
                    }
                }
            }

            // ШАГ 2: Удалить собранные ключи
            if (!keys_to_erase.empty())
            {
                flog::warn("[supervision4] Found {0} stale instances to remove.", keys_to_erase.size());
                for (const auto &key : keys_to_erase)
                {
                    flog::info("    - Removing stale instance: '{0}'", key);
                    instances.erase(key);
                }
                config_was_modified = true;
            }
            else
            {
                flog::info("[supervision4] No stale instances found. Cleanup not needed.");
            }
        }
    }
    catch (const std::exception &e)
    {
        flog::error("[supervision4] Exception during instance cleanup: {0}", e.what());
    }

    // Освобождаем мьютекс и сохраняем изменения на диск, ТОЛЬКО если они были.
    core::configManager.release(config_was_modified);
}
