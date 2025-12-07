#include "tcp_client_arm.h"
#include <utils/flog.h>
#include <dsp/sink.h>
#include <imgui.h>
// #include <gui/main_window.h>
#include <utils/flog.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>

// #include <zstd.h>
// #include <server_protocol.h>

using namespace std::chrono_literals;

namespace server
{
    // bool REMOTE = true;
    bool _init = true;
// ZSTD_DCtx* dctx;
#define PROTOCOL_TIMEOUT_MS 10000
    // #define currentSampleRate   8000000 // 10000000
    // #define BUFFERSIZE          262152

    enum PacketType
    {
        PACKET_TYPE_STATUS,
        PACKET_TYPE_MAIN,
        PACKET_TYPE_MAIN_STAT,
        PACKET_TYPE_COMMAND_ACK,
        PACKET_TYPE_BASEBAND,
        PACKET_TYPE_ERROR,
        PACKET_TYPE_SOURCES,
        PACKET_TYPE_AIRSPY,
        PACKET_TYPE_RADIO,
        PACKET_TYPE_SEARCH,
        PACKET_TYPE_SEARCH_STAT,
        PACKET_TYPE_SCAN,
        PACKET_TYPE_SCAN_STAT,
        PACKET_TYPE_CTRL,
        PACKET_TYPE_RECORD,
        PACKET_TYPE_SINK,
        PACKET_TYPE_SETTINGS
    };

#define MAX_BM_SIZE 64

    struct ScanModeList
    {
        char listName[32];
        int sizeOfList;
        int bookmarkName[MAX_BM_SIZE];
        double frequency[MAX_BM_SIZE];
        float bandwidth[MAX_BM_SIZE];
        int mode[MAX_BM_SIZE];
        int level[MAX_BM_SIZE];
        int Signal[MAX_BM_SIZE];
    };

    // TCPRemoveARM.cpp
    TCPRemoveARM::TCPRemoveARM(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t> *stream, bool datatype, uint8_t id)
    {
        this->sock = sock;
        this->currSrv = id;
        this->datatype = datatype;

        if (this->datatype)
        {
            // ===============================================
            // НОВАЯ УПРОЩЕННАЯ ЦЕПОЧКА
            // ===============================================
            // 1. Инициализируем сетевой буфер, как и раньше
            decompIn.setBufferSize((sizeof(dsp::complex_t) * STREAM_BUFFER_SIZE) * 2); // Можно увеличить
            decompIn.clearWriteStop();

            // 2. Инициализируем и запускаем `link`.
            //    Он будет читать из `decompIn` и писать в `stream`.
            link.init(&decompIn, stream);
            link.start();

            // 3. Декомпрессор больше не нужен.
            // decomp.init(&decompIn);
            // link.init(&decomp.out, stream); // Старый link тоже не нужен
            // decomp.start();
        }

        // Запуск рабочего потока остается без изменений
        if (this->datatype)
        {
            workerThread = std::thread(&TCPRemoveARM::worker, this);
        }
        else
        {
            workerThread = std::thread(&TCPRemoveARM::workerInfo, this);
        }
        _init = true;
        current = false;
        first = true;
    }

    TCPRemoveARM::~TCPRemoveARM()
    {
        // output = stream;
        this->datatype = datatype;
        flog::info("~TCPRemoveARM() {0}, currSrv  {1}", datatype, currSrv);
        if (_init)
            close();
        // delete[] bbuf;
    }

    bool TCPRemoveARM::isOpen()
    {
        return sock && sock->isOpen();
    }

    // TCPRemoveARM.cpp
    // tcp_client_arm.cpp

    void TCPRemoveARM::close()
    {
        flog::info("Closing connection for srv {0}, datatype {1}", this->currSrv, this->datatype);

        // 2. Если это поток данных, нужно разбудить .swap(), если он ждет
        if (this->datatype)
        {
            decompIn.stopWriter();
        }

        // 3. Закрываем сокет, это разблокирует .recv()
        if (sock && sock->isOpen())
        {
            sock->close();
        }

        // 4. Ждем завершения потока
        if (workerThread.joinable())
        {
            workerThread.join();
        }

        // 5. Обнуляем указатель на сокет
        sock = nullptr;

        // ВАЖНО: УДАЛЯЕМ ОСТАНОВКУ DSP-ЦЕПОЧКИ ОТСЮДА!
        // link.stop();
        // decompIn.clearWriteStop();
        // _init = false;
    }

    // tcp_client_arm.cpp

    void TCPRemoveARM::reconnect(std::shared_ptr<net::Socket> new_sock, uint8_t id)
    {
        // Сначала закрываем старое соединение, если оно было
        close();

        // Присваиваем новые параметры
        this->sock = new_sock;
        this->currSrv = id;

        // Если это поток данных, сбрасываем флаги буфера
        if (this->datatype)
        {
            this->decompIn.clearWriteStop();
        }

        if (this->datatype)
        {
            workerThread = std::thread(&TCPRemoveARM::worker, this);
        }
        else
        {
            workerThread = std::thread(&TCPRemoveARM::workerInfo, this);
        }
    }
    void TCPRemoveARM::setStatusControl(int val)
    {
        statusControl = val;
    }
    int TCPRemoveARM::getStatusControl()
    {
        return statusControl;
    }

    void TCPRemoveARM::updateStream()
    {
        if (current)
        {
            // dsp::stream<dsp::complex_t>* tmpstream = stream;
        }
    }

    void TCPRemoveARM::setUpdate(bool curr)
    {
        current = curr;
    }

    bool TCPRemoveARM::getUpdate()
    {
        return current;
    }

    void TCPRemoveARM::setFrequency(double freq)
    {
        // sendCommand(1, freq);
        // if (!isOpen()) { return; }
        currentFreq = freq;
        flog::info("rcv setFrequency currentFreq {0}", currentFreq);
    }

    void TCPRemoveARM::setSampleRate(double sr)
    {
        // if (!isOpen()) { return; }
        // currentSampleRate = 10000000;
        // bufferSize = 262152;
        /*        } else {
            currentSampleRate = (unsigned int) sr;
            bufferSize = sr / 200.0;
        }
        */
        flog::info("rcv setSampleRate currentSampleRate {0},  getCurrSampleRate() {2}", MAIN_SAMPLE_RATE, getCurrSampleRate());
    }

    double TCPRemoveARM::getCurrFrequency()
    {
        // flog::info("   get Frequency currentFreq {0}", currentFreq);
        return currentFreq;
    }

    unsigned int TCPRemoveARM::getCurrSampleRate()
    {
        return MAIN_SAMPLE_RATE;
    }

    // TCPRemoveARM.cpp
    void TCPRemoveARM::worker(void *ctx)
    {
        TCPRemoveARM *_this = (TCPRemoveARM *)ctx;

        // 1. Буфер для накопления данных из сети.
        std::vector<uint8_t> accumulation_buffer;
        accumulation_buffer.reserve((STREAM_BUFFER_SIZE * sizeof(dsp::complex_t)) * 2);

        // Временный буфер для чтения из сокета
        uint8_t net_buffer[16384]; // 16 KB

        flog::info("\nWORKER (data stream) started!");

        while (_this->sock && _this->sock->isOpen())
        {
            // 2. Читаем новую порцию данных из сети.
            int bytes_read = _this->sock->recv(net_buffer, sizeof(net_buffer));

            if (bytes_read <= 0)
            {
                flog::warn("WORKER: Connection lost or error. Exiting thread.");
                break;
            }

            // 3. Добавляем прочитанные данные в конец нашего буфера накопления.
            accumulation_buffer.insert(accumulation_buffer.end(), net_buffer, net_buffer + bytes_read);

            // 4. Пока в буфере накопления есть достаточно данных для обработки одной полной порции (STREAM_BUFFER_SIZE)...
            while (accumulation_buffer.size() >= (STREAM_BUFFER_SIZE * sizeof(dsp::complex_t)))
            {
                // ...получаем указатель на буфер DSP-стрима...
                dsp::complex_t *write_ptr = _this->decompIn.writeBuf;
                if (write_ptr == nullptr)
                {
                    flog::error("WORKER: writeBuf is null! Aborting.");
                    // Нужно выйти из обоих циклов
                    goto worker_exit;
                }

                // ...копируем туда ровно ОДНУ порцию...
                size_t bytes_to_copy = STREAM_BUFFER_SIZE * sizeof(dsp::complex_t);
                memcpy(write_ptr, accumulation_buffer.data(), bytes_to_copy);

                // ...и "проталкиваем" ее.
                if (!_this->decompIn.swap(STREAM_BUFFER_SIZE))
                {
                    flog::warn("WORKER: Stream swap failed. Stopping.");
                    goto worker_exit;
                }
                _this->bytes += bytes_to_copy;

                // 5. Удаляем обработанную часть из начала буфера накопления.
                accumulation_buffer.erase(accumulation_buffer.begin(), accumulation_buffer.begin() + bytes_to_copy);
            }
        }

    worker_exit:; // Метка для выхода из вложенного цикла
        flog::info("\nWORKER (data stream) finished.");
    }
    void TCPRemoveARM::start_server(bool val)
    {
        _start_server = val;
        // clntsending = true;
        // flog::info("_start_server={0}, val={1} !!!", _start_server, val);
    };

    bool TCPRemoveARM::server_status()
    {
        return _start_server;
    }

    float TCPRemoveARM::getVolLevel()
    {
        return _squelchLevel;
    }
    void TCPRemoveARM::setVolLevel(float val, bool updt)
    {
        flog::info("_squelchLevel = {0}, val = {1}", _squelchLevel, val);
        _squelchLevel = val;
        // if (updt)
        //    clntsending = true;
    }

    //====================================

    void TCPRemoveARM::workerInfo(void *ctx)
    {
        TCPRemoveARM *_this = (TCPRemoveARM *)ctx;

        struct MainConfig
        {
            uint8_t id;
            int status = 0;
            int id_control = 0;
            int statusServer = 0;
            double freq = 0;
            double offset = 0;
            bool tuningMode = 0;
            int selectedDemodID = 1;
            float bandwidth = 10000;
            bool playing = false;
            float level = 0.7;
            bool recording = false;
            bool search = false;
            int selectedLogicId = 0;
            int idOfList_srch = 0;
            bool scan = false;
            int idOfList_scan = 0;
            bool control = false;
            int idOfList_control = 0;
            int setLevelDbSrch = 0;
            int setLevelDbScan = 0;
            int setLevelDbCtrl = 0;
            bool statAutoLevelSrch = true;
            bool statAutoLevelScan = true;
            bool statAutoLevelCtrl = true;
            int SNRLevelDbSrch = -50;
            bool status_AKFSrch = false;
            bool status_AKFCtrl = false;
            int maxRecWaitTimeCtrl = 5;
            bool UpdateMenu;
            bool isNotPlaying = false;
        };
        // GENERAL STATE
        struct MainStat
        {
            uint8_t id;
            char nameInstance[32];
            char version[32];
            int statusServer = 0;
            double freq;
            double offset;
            bool tuningMode = 0;
            // int selectedDemodID = 1;
            float bandwidth = 10000;
            int sinkOfRadio;
            bool playing = false;
            bool recording = false;
            bool search = false;
            int selectedLogicId = 0;
            int idOfList_srch = 0;
            bool scan = false;
            int idOfList_scan = 0;
            bool control = false;
            int idOfList_control = 0;
            int setLevelDbSrch = 0;
            int setLevelDbScan = 0;
            int setLevelDbCtrl = 0;
            int SNRLevelDbSrch = -50;
            bool status_AKFSrch = false;
            bool status_AKFCtrl = false;
            bool UpdateStat = false;
            double sampleRate = 10000000;
            bool isNotPlaying = false;
        };
        // AIRSPY
        struct AirSpyConfig
        {
            uint8_t id;
            bool Airspy;
            int sourceId;
            int lnaGain;
            int vgaGain;
            int mixerGain;
            int linearGain;
            int sensitiveGain;
            int gainMode;
            bool lnaAgc;
            bool mixerAgc;
            bool select;
            bool _updateLinearGain;
            bool UpdateMenu;
        };
        // RADIO
        struct RadioConfig
        {
            int selectedDemodID = 1;
            int snapInterval = 0;
            int snapIntervalId = 0;
            int deempId = 0;
            int baseband_band = 1000000;
            int fmIFPresetId = 0;
            int tone = 800;

            float bandwidth = 10000;
            float nbLevel = 10.0f;
            float squelchLevel = 0;
            float agcAttack = 50.0f;
            float agcDecay = 5.0f;

            bool nbEnabled = false;
            bool squelchEnabled = false;
            bool FMIFNREnabled = false;
            bool _lowPass = true;
            bool _highPass = false;
            bool carrierAgc = false;
            bool UpdateMenu = false;
        };
        // SEARCH
        struct SearchConfig
        {
            int selectedLogicId = 0;
            int idOfList_srch = 0;
            bool button_srch = true;
            int levelDb = -50;
            int SNRlevelDb = 10;
            bool status_AKF = false;
            bool UpdateModule = false;
            bool UpdateLists = false;
            bool UpdateMenu = false;
            bool statAutoLevel = false;
        };
        struct FoundBookmark
        {
            double frequency;
            float bandwidth;
            int mode;
            int level;
            bool selected;
            std::time_t ftime;
            int Signal = -1;
        };

        // SEARCH
        struct ScanConfig
        {
            int idOfList_scan = 0;
            bool button_scan = false;
            int maxRecDuration = 5;
            int maxRecWaitTime = 10;
            bool flag_level = false;
            int level = -50;
            bool UpdateModule = false;
            bool UpdateLists = false;
            bool UpdateMenu = false;
            bool statAutoLevel = false;
        };
        // CTRL
        struct CTRLConfig
        {
            int idOfList_ctrl = 0;
            bool button_ctrl = false;
            bool button_scan = false;
            int maxRecWaitTime = 10;
            bool flag_level = false;
            int level = -50;
            bool UpdateModule = false;
            bool UpdateLists = false;
            bool UpdateMenu = false;
            bool statAutoLevel = false;
            bool status_AKF = false;
        };
        // RECORD
        struct RecordConfig
        {
            bool recording = 1;
            bool UpdateMenu = false;
        };
        // StatusServerConfig msgStatusServer;
        AirSpyConfig msgAir;
        MainConfig msgMain;
        MainStat msgMainStat;
        RadioConfig msgRadio;
        RecordConfig msgRecord;
        SearchConfig msgSearch;
        ScanConfig msgScan;
        CTRLConfig msgCTRL;

        FoundBookmark msgStatFinded;
        bool send_airspy = false;
        bool send_radio = false;
        bool send_main = false;
        bool send_record = false;
        int change_freq = 0;
        int curr_StatusServer = ARM_STATUS_NOT_CONTROL;
        _this->setVolLevel(0.1, false);

        const size_t MAX_INFO_BUFFER_SIZE = 256 * MAX_STRUCT_SIZE;
        _this->ibuf = new uint8_t[MAX_INFO_BUFFER_SIZE];
        _this->ib_pkt_hdr = (InfoHeader *)_this->ibuf;

        flog::info("WORKER (info channel) for srv {0} started!", _this->currSrv);
        // while (true)
        while (_this->sock && _this->sock->isOpen())
        {
            // Receive header
            _this->ib_pkt_hdr->size = 0;
            // flog::info("workerInfo!!!");
            // 1. Читаем заголовок пакета.
            if (_this->sock->recv(_this->ibuf, sizeof(InfoHeader), true) <= 0)
            {
                flog::warn("WORKER_INFO: Failed to receive header or connection closed for srv {0}.", _this->currSrv);
                break;
            }

            // 2. ИСПРАВЛЕНО (КРИТИЧЕСКАЯ УЯЗВИМОСТЬ): Проверяем размер пакета ПЕРЕД чтением тела.
            uint32_t received_size = _this->ib_pkt_hdr->size;
            if (received_size > MAX_INFO_BUFFER_SIZE || received_size < sizeof(InfoHeader))
            {
                flog::error("WORKER_INFO: Invalid packet size {0} from srv {1}. Max allowed: {2}. Closing connection.", received_size, _this->currSrv, MAX_INFO_BUFFER_SIZE);
                _this->sock->close();
                break;
            }

            // 3. Читаем тело пакета.
            int data_to_read = received_size - sizeof(InfoHeader);
            if (data_to_read > 0)
            {
                if (_this->sock->recv(&_this->ibuf[sizeof(InfoHeader)], data_to_read, true, PROTOCOL_TIMEOUT_MS) != data_to_read)
                {
                    flog::error("WORKER_INFO: Failed to receive full packet body from srv {0} or timeout. Closing connection.", _this->currSrv);
                    _this->sock->close();
                    break;
                }
            }

            int count = data_to_read;
            /*
            if (_this->sock->recv(_this->ibuf, sizeof(InfoHeader), true) <= 0)
            {
                break;
            }

            // flog::info("workerInfo!!!   count {0}, ib_pkt_hdr->type {1} ", count, _this->ib_pkt_hdr->type);
            if ((_this->ib_pkt_hdr->size - sizeof(InfoHeader)) > 0)
            {
                if (_this->sock->recv(&_this->ibuf[sizeof(InfoHeader)], _this->ib_pkt_hdr->size - sizeof(InfoHeader), true, PROTOCOL_TIMEOUT_MS) <= 0)
                {
                    break;
                }
            }
            count = _this->ib_pkt_hdr->size - sizeof(InfoHeader);
            // flog::info("workerInfo!!!   count {0}, ib_pkt_hdr->type {1} ", count, _this->ib_pkt_hdr->type);
            if (count < 0)
            {
                break;
            }
            */
            int _tuningMode = gui::mainWindow.gettuningMode();

            double _offset = 0;
            if (_tuningMode != tuner::TUNER_MODE_CENTER)
            {
                _offset = sigpath::vfoManager.getOffset("Канал приймання");
            }

            _init = false;
            // if (count == sizeof(msgAir)) {
            // flog::info("  count {0}, ib_pkt_hdr->type {1}, statusServer {2} ", count, _this->ib_pkt_hdr->type, statusServer);

            int statusServer = gui::mainWindow.getServerStatus(_this->currSrv);
            bool rcv_stat = false;
            bool rcv_main = false;

            if (_this->ib_pkt_hdr->type == PACKET_TYPE_MAIN && statusServer == ARM_STATUS_FULL_CONTROL)
            { // MAIN
                // memcpy(&msgMain, &_this->ibuf[sizeof(InfoHeader)], count);
                if (count != sizeof(msgMain))
                {
                    flog::error("WORKER_INFO: count {0} != sizeof(msgMain) {1}", count, sizeof(msgMain));
                    break;
                }
                memcpy(&msgMain, &_this->ibuf[sizeof(InfoHeader)], count);
                flog::info("    TRACE PACKET_TYPE_MAIN. RCV msgMain.freq {0}, msgMain.offset {1}, getUpdateMenuSnd0Main(_this->currSrv) {2}, getUpdateMenuSnd() {3}", msgMain.freq, msgMain.offset, gui::mainWindow.getUpdateMenuSnd0Main(_this->currSrv), gui::mainWindow.getUpdateMenuSnd());
                if (msgMain.UpdateMenu && !gui::mainWindow.getUpdateMenuSnd0Main(_this->currSrv) && !gui::mainWindow.getUpdateMenuSnd())
                {
                    flog::info("1. msgMain.freq {0}, msgMain.offset {1}, _offset {2}", msgMain.freq, msgMain.offset, _offset);

                    curr_StatusServer = msgMain.status;
                    if (msgMain.tuningMode != gui::mainWindow.gettuningMode())
                    {
                        gui::mainWindow.settuningMode(msgMain.tuningMode);
                        // tuningMode = core::configManager.conf["centerTuning"] ? tuner::TUNER_MODE_CENTER : tuner::TUNER_MODE_NORMAL;
                        core::configManager.acquire();
                        core::configManager.conf["centerTuning"] = !msgMain.tuningMode;
                        core::configManager.release(true);
                        gui::waterfall.VFOMoveSingleClick = (msgMain.tuningMode == tuner::TUNER_MODE_CENTER);
                        gui::waterfall.centerFreqMoved = true;
                    }

                    double t_freq = msgMain.freq;
                    double curr_freq = gui::waterfall.getCenterFrequency();
                    if (t_freq != curr_freq)
                    {
                        tuner::centerTuning(gui::waterfall.selectedVFO, t_freq);
                        sigpath::vfoManager.setOffset("Канал приймання", msgMain.offset);
                        gui::waterfall.centerFreqMoved = true;
                        // flog::info("2. msgMain.freq {0}, msgMain.offset {1}", msgMain.freq, msgMain.offset);
                        // usleep(100);
                    }
                    if (msgMain.freq == curr_freq && msgMain.offset == _offset)
                    {
                        // flog::info("3. msgMain.freq {0}, msgMain.offset {1}", msgMain.freq, msgMain.offset);
                        // tuner::tune(msgMain.tuningMode, gui::waterfall.selectedVFO, t_freq);
                        // sigpath::vfoManager.setOffset("Канал приймання", msgMain.offset);
                        gui::waterfall.centerFreqMoved = true;
                        // ok
                    }
                    else
                    {
                        gui::mainWindow.setUpdateFreq(true);
                        if (msgMain.tuningMode == tuner::TUNER_MODE_NORMAL)
                        {
                            t_freq = msgMain.freq + msgMain.offset;
                            tuner::tune(msgMain.tuningMode, gui::waterfall.selectedVFO, t_freq);
                            // flog::info("TUNER_MODE_NORMAL");
                            gui::waterfall.centerFreqMoved = true;
                        }
                        else
                        {
                            tuner::centerTuning(gui::waterfall.selectedVFO, t_freq);
                            flog::info("TUNER_MODE_CENTER");
                            gui::waterfall.centerFreqMoved = true;
                        }
                    }

                    if (msgMain.level != _this->getVolLevel())
                    {
                        flog::info("msgMain.level {0}, _this->getVolLevel {1}", msgMain.level, _this->getVolLevel());
                        _this->setVolLevel(msgMain.level, false);
                    }

                    // if (gui::mainWindow.isPlaying() != msgMain.playing) {
                    flog::info("   TRACE PACKET_TYPE_MAIN RCV.. _this->currSrv {0}", _this->currSrv);
                    gui::mainWindow.setServerPlayState(_this->currSrv, msgMain.playing);
                    gui::mainWindow.setServerIsNotPlaying(_this->currSrv, msgMain.isNotPlaying);

                    // }
                    rcv_main = true;
                    flog::info("TRACE RCV PACKET_TYPE_MAIN ib_pkt_hdr->type {0}, ib_pkt_hdr->size {1}, msgMain.offset {2}, _offset {3}, msgMain.tuningMode {4}, gui::waterfall.VFOMoveSingleClick {5}, _tuningMode {6}, msgMain.playing {7}, curr_StatusServer {8}", _this->ib_pkt_hdr->type, _this->ib_pkt_hdr->size, msgMain.offset, _offset, msgMain.tuningMode, gui::waterfall.VFOMoveSingleClick, _tuningMode, msgMain.playing, curr_StatusServer);
                }
            }
            else if (_this->ib_pkt_hdr->type == PACKET_TYPE_AIRSPY && statusServer == ARM_STATUS_FULL_CONTROL)
            {
                flog::info("PACKET_TYPE_AIRSPY! count {0}, ib_pkt_hdr->type {1} ", count, _this->ib_pkt_hdr->type);
                if (count != sizeof(msgAir))
                {
                    flog::error("WORKER_INFO: count {0} != sizeof(msgAir) {1}", count, sizeof(msgAir));
                    break;
                }
                memcpy(&msgAir, &_this->ibuf[sizeof(InfoHeader)], count);
                if (msgAir.UpdateMenu && !gui::mainWindow.getUpdateMenuSnd())
                {
                    gui::mainWindow.setlnaGain(msgAir.lnaGain);
                    gui::mainWindow.setvgaGain(msgAir.vgaGain);
                    gui::mainWindow.setmixerGain(msgAir.mixerGain);
                    gui::mainWindow.setlinearGain(msgAir.linearGain);
                    gui::mainWindow.setsensitiveGain(msgAir.sensitiveGain);
                    gui::mainWindow.setgainMode(msgAir.gainMode);
                    gui::mainWindow.setlnaAgc(msgAir.lnaAgc);
                    gui::mainWindow.setmixerAgc(msgAir.mixerAgc);
                    gui::mainWindow.setselect(msgAir.select);
                    gui::mainWindow.set_updateLinearGain(msgAir._updateLinearGain);
                    gui::mainWindow.setUpdateMenuRcv(msgAir.UpdateMenu);
                    flog::info("TRACE 1 (279)   msgAir.lnaGain {0}, msgAir.linearGain {1} !!!", msgAir.lnaGain, msgAir.linearGain);
                }
            }
            else if (_this->ib_pkt_hdr->type == PACKET_TYPE_RADIO && statusServer == ARM_STATUS_FULL_CONTROL)
            { // RADIO
                if (count != sizeof(msgRadio))
                {
                    flog::error("WORKER_INFO: count {0} != sizeof(msgRadio) {1}", count, sizeof(msgRadio));
                    break;
                }
                memcpy(&msgRadio, &_this->ibuf[sizeof(InfoHeader)], count);
                if (msgRadio.UpdateMenu)
                {
                    flog::info("PACKET_TYPE_RADIO RCV msgRadio.UpdateMenu {0}, selectedDemodID {1}, msgRadio.bandwidth {2}", msgRadio.UpdateMenu, msgRadio.selectedDemodID, msgRadio.bandwidth);
                    gui::mainWindow.setselectedDemodID(msgRadio.selectedDemodID);
                    gui::mainWindow.setbandwidth(msgRadio.bandwidth);
                    gui::mainWindow.setsnapInterval(msgRadio.snapInterval);
                    gui::mainWindow.setsnapIntervalId(msgRadio.snapIntervalId);
                    gui::mainWindow.setdeempId(msgRadio.deempId);
                    gui::mainWindow.setnbEnabled(msgRadio.nbEnabled);
                    gui::mainWindow.setnbLevel(msgRadio.nbLevel);
                    gui::mainWindow.setsquelchEnabled(msgRadio.squelchEnabled);
                    gui::mainWindow.setsquelchLevel(msgRadio.squelchLevel);
                    gui::mainWindow.setFMIFNREnabled(msgRadio.FMIFNREnabled);
                    gui::mainWindow.setfmIFPresetId(msgRadio.fmIFPresetId);
                    gui::mainWindow.setlowPass(msgRadio._lowPass);
                    gui::mainWindow.sethighPass(msgRadio._highPass);
                    gui::mainWindow.setagcAttack(msgRadio.agcAttack);
                    gui::mainWindow.setagcDecay(msgRadio.agcDecay);
                    gui::mainWindow.setcarrierAgc(msgRadio.carrierAgc);
                    gui::mainWindow.setUpdateMenuRcv2Radio(true);
                }
            }
            else if (_this->ib_pkt_hdr->type == PACKET_TYPE_RECORD && statusServer == ARM_STATUS_FULL_CONTROL)
            { // RADIO
                if (count != sizeof(msgRecord))
                {
                    flog::error("WORKER_INFO: count {0} != sizeof(msgRecord) {1}", count, sizeof(msgRecord));
                    break;
                }
                memcpy(&msgRecord, &_this->ibuf[sizeof(InfoHeader)], count);
                if (msgRecord.UpdateMenu)
                {
                    gui::mainWindow.setUpdateMenuRcv3Record(true);
                    if (msgRecord.recording == true)
                        gui::mainWindow.setServerRecordingStart(_this->currSrv);
                    else
                        gui::mainWindow.setServerRecordingStop(_this->currSrv);
                    flog::info("PACKET_TYPE_RECORD. RCV. msgRecord.UpdateMenu {0}, msgRecord.recording {1}", msgRecord.UpdateMenu, msgRecord.recording);
                }
            }
            else if (_this->ib_pkt_hdr->type == PACKET_TYPE_SEARCH_STAT && statusServer == ARM_STATUS_FULL_CONTROL)
            { // SEARCH
                {
                    std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                    gui::waterfall.finded_freq.clear();
                }
                for (int poz = sizeof(InfoHeader); poz < count + sizeof(InfoHeader); poz = poz + sizeof(FoundBookmark))
                {
                    memcpy((void *)&gui::waterfall.addFreq, &_this->ibuf[poz], sizeof(FoundBookmark));
                    {
                        std::lock_guard<std::mutex> lock(gui::waterfall.findedFreqMtx);
                        gui::waterfall.finded_freq[gui::waterfall.addFreq.frequency] = gui::waterfall.addFreq;
                    }
                    //    flog::info("TRACE! gui::waterfall.addFreq.frequency = {0}, msgStatFinded.bandwidth {1}, poz {2}", statFreq.frequency, statFreq.bandwidth, poz);
                }
                flog::info("PACKET_TYPE_SEARCH_STAT. count {0}, getsizeOfbbuf_srch_stat {1}", count, gui::mainWindow.getsizeOfbbuf_srch_stat());
            }
            // ====================================================================================
            else if (_this->ib_pkt_hdr->type == PACKET_TYPE_SEARCH && statusServer > ARM_STATUS_NOT_CONTROL)
            {
                // SEARCH CONFIG from RPM
                int msgSize = count;
                if (_this->ib_pkt_hdr->sizeOfExtension > 0)
                    msgSize -= _this->ib_pkt_hdr->sizeOfExtension;

                if (msgSize != (int)sizeof(msgSearch))
                {
                    flog::error("WORKER_INFO: PACKET_TYPE_SEARCH: msgSize {0} != sizeof(msgSearch) {1}",
                                msgSize, sizeof(msgSearch));
                    break;
                }

                memcpy(&msgSearch, &_this->ibuf[sizeof(InfoHeader)], sizeof(msgSearch));

                // Расширение — списки поиска
                if (_this->ib_pkt_hdr->sizeOfExtension > 0 && !gui::mainWindow.getbutton_srch(_this->currSrv))
                {
                    /*
                    int extSize = _this->ib_pkt_hdr->sizeOfExtension;
                    try
                    {
                        std::vector<uint8_t> tempBuf(extSize);
                        size_t dataOffset = sizeof(InfoHeader) + sizeof(msgSearch);
                        memcpy(tempBuf.data(),
                               &_this->ibuf[dataOffset],
                               extSize);
                        gui::mainWindow.setbbuf_srch(tempBuf.data(), extSize);
                        gui::mainWindow.setUpdateListRcv5Srch(_this->currSrv, true);
                    }
                    catch (const std::exception &e)
                    {
                        flog::error("WORKER_INFO: PACKET_TYPE_SEARCH alloc error: {0}", e.what());
                    }
                    */
                }

                if (msgSearch.UpdateMenu)
                {
                    flog::info("PACKET_TYPE_SEARCH (RPM->ARM). RCV. srv={0}, button_srch {1}, idOfList {2}, levelDb {3}, SNRlevelDb {4}, statAutoLevel {5}",
                               _this->currSrv,
                               msgSearch.button_srch,
                               msgSearch.idOfList_srch,
                               msgSearch.levelDb,
                               msgSearch.SNRlevelDb,
                               msgSearch.statAutoLevel);

                    gui::mainWindow.setbutton_srch(_this->currSrv, msgSearch.button_srch);
                    gui::mainWindow.setidOfList_srch(_this->currSrv, msgSearch.idOfList_srch);
                    gui::mainWindow.setLevelDbSrch(_this->currSrv, msgSearch.levelDb);
                    gui::mainWindow.setAuto_levelSrch(_this->currSrv, msgSearch.statAutoLevel);
                    gui::mainWindow.setSNRLevelDb(_this->currSrv, msgSearch.SNRlevelDb);
                    gui::mainWindow.setAKFInd(_this->currSrv, msgSearch.status_AKF);
                    gui::mainWindow.setselectedLogicId(_this->currSrv, msgSearch.selectedLogicId);
                    gui::mainWindow.setUpdateModule_srch(_this->currSrv, msgSearch.UpdateModule);
                    gui::mainWindow.setUpdateMenuRcv5Srch(true);
                    gui::mainWindow.setUpdateMenuSnd5Srch(_this->currSrv, false);
                }
            }
            else if (_this->ib_pkt_hdr->type == PACKET_TYPE_SCAN && statusServer > ARM_STATUS_NOT_CONTROL)
            {
                int msgSize = count;
                if (_this->ib_pkt_hdr->sizeOfExtension > 0)
                    msgSize -= _this->ib_pkt_hdr->sizeOfExtension;

                if (msgSize < (int)sizeof(msgScan))
                {
                    flog::error("WORKER_INFO: PACKET_TYPE_SCAN: msgSize {0} < sizeof(msgScan) {1}",
                                msgSize, sizeof(msgScan));
                    break;
                }

                memcpy(&msgScan, &_this->ibuf[sizeof(InfoHeader)], sizeof(msgScan));

                // Расширение — список частот
                if (_this->ib_pkt_hdr->sizeOfExtension > 0 && !gui::mainWindow.getbutton_scan(_this->currSrv))
                {
                    /*
                    int extSize = _this->ib_pkt_hdr->sizeOfExtension;
                    try
                    {
                        std::vector<uint8_t> tempBuf(extSize);
                        size_t dataOffset = sizeof(InfoHeader) + sizeof(msgScan);
                        memcpy(tempBuf.data(),
                               &_this->ibuf[dataOffset],
                               extSize);
                        gui::mainWindow.setbbuf_scan(tempBuf.data(), extSize);
                        gui::mainWindow.setUpdateListRcv6Scan(_this->currSrv, true);
                    }
                    catch (const std::exception &e)
                    {
                        flog::error("WORKER_INFO: PACKET_TYPE_SCAN alloc error: {0}", e.what());
                    }
                    */
                }

                flog::info("PACKET_TYPE_SCAN (RPM->ARM). RCV. srv={0}, button_scan {1}, idOfList {2}, level {3}",
                           _this->currSrv,
                           msgScan.button_scan,
                           msgScan.idOfList_scan,
                           msgScan.level);

                gui::mainWindow.setidOfList_scan(_this->currSrv, msgScan.idOfList_scan);

                if (msgScan.UpdateMenu)
                {
                    gui::mainWindow.setbutton_scan(_this->currSrv, msgScan.button_scan);
                    gui::mainWindow.setAuto_levelScan(_this->currSrv, msgScan.statAutoLevel);
                    gui::mainWindow.setMaxRecWaitTime_scan(_this->currSrv, msgScan.maxRecWaitTime);
                    gui::mainWindow.setMaxRecDuration_scan(_this->currSrv, msgScan.maxRecDuration);
                    gui::mainWindow.setLevelDbScan(_this->currSrv, msgScan.level);
                    gui::mainWindow.setUpdateModule_scan(_this->currSrv, msgScan.UpdateModule);
                    gui::mainWindow.setUpdateMenuRcv6Scan(true);
                    gui::mainWindow.setUpdateMenuSnd6Scan(_this->currSrv, false);
                }
            }
            else if (_this->ib_pkt_hdr->type == PACKET_TYPE_CTRL && statusServer > ARM_STATUS_NOT_CONTROL)
            {
                int msgSize = count;
                if (_this->ib_pkt_hdr->sizeOfExtension > 0)
                    msgSize -= _this->ib_pkt_hdr->sizeOfExtension;

                if (msgSize != (int)sizeof(msgCTRL))
                {
                    flog::error("WORKER_INFO: PACKET_TYPE_CTRL: msgSize {0} != sizeof(msgCTRL) {1}",
                                msgSize, sizeof(msgCTRL));
                    break;
                }

                memcpy(&msgCTRL, &_this->ibuf[sizeof(InfoHeader)], sizeof(msgCTRL));

                // Расширение — список CTRL-каналов
                if (_this->ib_pkt_hdr->sizeOfExtension > 0)
                {
                    /*
                    int extSize = _this->ib_pkt_hdr->sizeOfExtension;
                    try
                    {
                        std::vector<uint8_t> tempBuf(extSize);
                        size_t dataOffset = sizeof(InfoHeader) + sizeof(msgCTRL);
                        memcpy(tempBuf.data(),
                               &_this->ibuf[dataOffset],
                               extSize);
                        gui::mainWindow.setbbuf_ctrl(tempBuf.data(), extSize);
                        gui::mainWindow.setUpdateListRcv7Ctrl(_this->currSrv, true);
                    }
                    catch (const std::exception &e)
                    {
                        flog::error("WORKER_INFO: PACKET_TYPE_CTRL alloc error: {0}", e.what());
                    }
                    */
                }
                
                flog::info("PACKET_TYPE_CTRL (RPM->ARM). RCV. srv={0}, button_ctrl {1}, idOfList {2}, level {3}",
                           _this->currSrv,
                           msgCTRL.button_ctrl,
                           msgCTRL.idOfList_ctrl,
                           msgCTRL.level);

                gui::mainWindow.setidOfList_ctrl(_this->currSrv, msgCTRL.idOfList_ctrl);

                if (msgCTRL.UpdateMenu)
                {
                    gui::mainWindow.setbutton_ctrl(_this->currSrv, msgCTRL.button_ctrl);
                    gui::mainWindow.setMaxRecWaitTime_ctrl(_this->currSrv, msgCTRL.maxRecWaitTime);
                    gui::mainWindow.setAuto_levelCtrl(_this->currSrv, msgCTRL.statAutoLevel);
                    gui::mainWindow.setAKFInd_ctrl(_this->currSrv, msgCTRL.status_AKF);
                    gui::mainWindow.setflag_level_ctrl(_this->currSrv, msgCTRL.flag_level);
                    gui::mainWindow.setLevelDbCtrl(_this->currSrv, msgCTRL.level);
                    gui::mainWindow.setUpdateModule_ctrl(_this->currSrv, msgCTRL.UpdateModule);
                    gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
                    gui::mainWindow.setUpdateMenuSnd7Ctrl(_this->currSrv, false);
                }
            }
            // ================================================================
            else if (count == sizeof(msgMainStat) && _this->ib_pkt_hdr->type == PACKET_TYPE_MAIN_STAT && !gui::mainWindow.getUpdateMenuSnd())
            { // MAIN
                // && statusServer > ARM_STATUS_NOT_CONTROL && !gui::mainWindow.getUpdateMenuRcv0Main(_this->currSrv)
                if (count != sizeof(msgMainStat))
                {
                    flog::error("WORKER_INFO: count {0} != sizeof(msgMainStat) {1}", count, sizeof(msgMainStat));
                    break;
                }
                memcpy(&msgMainStat, &_this->ibuf[sizeof(InfoHeader)], count);
                curr_StatusServer = msgMainStat.statusServer;

                uint8_t id = msgMainStat.id;
                uint8_t currSrv = _this->currSrv;
                // flog::info("STATUS. currSrv {0}, (msgMainStat.freq + msgMainStat.offset) {1}, statusServer {2}, msgMainStat.search {3}", currSrv, (msgMainStat.freq + msgMainStat.offset), statusServer, msgMainStat.search);

                if (!gui::mainWindow.getUpdateMenuSnd0Main(_this->currSrv))
                {
                    // flog::info("1 currSrv {0}", currSrv);
                    gui::mainWindow.setbutton_srch(currSrv, msgMainStat.search);
                    // if (!gui::mainWindow.getUpdateMenuSnd5Srch(currSrv))
                    // {
                    if (msgMainStat.search)
                    {
                        if (statusServer == ARM_STATUS_FULL_CONTROL)
                        {
                            if (msgMainStat.idOfList_srch != gui::mainWindow.getidOfList_srch(currSrv))
                            {
                                gui::mainWindow.setselectedLogicId(currSrv, msgMainStat.selectedLogicId);
                                gui::mainWindow.setidOfList_srch(currSrv, msgMainStat.idOfList_srch);
                                // gui::mainWindow.setUpdateMenuRcv5Srch(true);
                            }
                        }
                        else
                        {
                            gui::mainWindow.setselectedLogicId(currSrv, msgMainStat.selectedLogicId);
                            gui::mainWindow.setidOfList_srch(currSrv, msgMainStat.idOfList_srch);
                        }
                    }
                    // }

                    gui::mainWindow.setbutton_scan(currSrv, msgMainStat.scan);
                    // if (!gui::mainWindow.getUpdateMenuSnd6Scan(currSrv))
                    // {
                    if (msgMainStat.scan)
                    {
                        if (statusServer == ARM_STATUS_FULL_CONTROL)
                        {
                            if (msgMainStat.idOfList_scan != gui::mainWindow.getidOfList_scan(currSrv))
                            {
                                gui::mainWindow.setidOfList_scan(currSrv, msgMainStat.idOfList_scan);
                                // gui::mainWindow.setUpdateMenuRcv6Scan(true);
                            }
                        }
                        else
                        {
                            gui::mainWindow.setidOfList_scan(currSrv, msgMainStat.idOfList_scan);
                        }
                    }
                    // }

                    gui::mainWindow.setbutton_ctrl(currSrv, msgMainStat.control);
                    if (msgMainStat.control)
                    {
                        if (statusServer == ARM_STATUS_FULL_CONTROL)
                        {
                            if (msgMainStat.idOfList_control != gui::mainWindow.getidOfList_ctrl(currSrv))
                            {
                                gui::mainWindow.setidOfList_ctrl(currSrv, msgMainStat.idOfList_control);
                                // gui::mainWindow.setUpdateMenuRcv7Ctrl(true);
                            }
                        }
                    }

                    if (msgMainStat.search && msgMainStat.setLevelDbSrch != gui::mainWindow.getLevelDbSrch(currSrv))
                    {
                        gui::mainWindow.setLevelDbSrch(currSrv, msgMainStat.setLevelDbSrch);
                    }
                    if (msgMainStat.scan && msgMainStat.setLevelDbScan != gui::mainWindow.getLevelDbScan(currSrv))
                    {
                        gui::mainWindow.setLevelDbScan(currSrv, msgMainStat.setLevelDbScan);
                    }
                    if (msgMainStat.control && msgMainStat.setLevelDbCtrl != gui::mainWindow.getLevelDbCtrl(currSrv))
                    {
                        gui::mainWindow.setLevelDbCtrl(currSrv, msgMainStat.setLevelDbCtrl);
                    }

                    if (msgMainStat.recording == true) // recording
                        gui::mainWindow.setServerRecordingStart(currSrv);
                    else
                        gui::mainWindow.setServerRecordingStop(currSrv);

                    gui::mainWindow.setServerPlayState(currSrv, msgMainStat.playing);
                    gui::mainWindow.setServerIsNotPlaying(currSrv, msgMainStat.isNotPlaying);
                    gui::mainWindow.setServersFreq(currSrv, (msgMainStat.freq + msgMainStat.offset));
                    if (msgMainStat.sampleRate > 0)
                    {
                        gui::mainWindow.setServerSampleRate(currSrv, msgMainStat.sampleRate);
                    }
                    /// flog::info("TRACE RCV MAIN STAT freq {0}, selectedDemodID {1}, bandwidth {2}, recording {3}, search {4}, selectedLogicId {5}, scan {6}, idOfList_scan {7}, curr_StatusServer {8}, _this->currSrv {9} ",
                    ///            msgMainStat.freq, gui::mainWindow.getselectedDemodID(), msgMainStat.bandwidth, msgMainStat.recording, msgMainStat.search, msgMainStat.selectedLogicId, msgMainStat.scan, msgMainStat.idOfList_scan, curr_StatusServer, _this->currSrv);
                    rcv_stat = true;
                    // gui::mainWindow.setFullConnection(_this->currSrv, true);
                    gui::mainWindow.fullConnection[_this->currSrv] = true;
                }
                if (_this->first == true)
                {
                    gui::mainWindow.setVersion(currSrv, std::string(msgMainStat.version));
                    gui::mainWindow.setServersName(currSrv, std::string(msgMainStat.nameInstance));
                    _this->first = false;
                }
            }

            // =========================================================================
            if (change_freq > 0)
                change_freq--;
            if (send_airspy)
            {
                gui::mainWindow.setUpdateMenuSnd(false);
                send_airspy = false;
            }
            if (send_radio)
            {
                gui::mainWindow.setUpdateMenuSnd2Radio(false);
                send_radio = false;
            }
            if (send_main)
            {
                // flog::info("send_main");
                gui::mainWindow.setUpdateMenuSnd0Main(_this->currSrv, false);
                send_main = false;
            }
            if (send_record)
            {
                gui::mainWindow.setUpdateMenuSnd3Record(false);
                send_record = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            bool _thiscontrol = false;
            if (statusServer == ARM_STATUS_FULL_CONTROL && curr_StatusServer == ARM_STATUS_FULL_CONTROL)
                _thiscontrol = true;
            bool _thisNetOK = false;
            if (statusServer > ARM_STATUS_NOT_CONTROL)
                _thisNetOK = true;

            // flog::info("statusServer {0}, curr_StatusServer {1}, _thiscontrol {2}", statusServer, curr_StatusServer, _thiscontrol);
            if (gui::mainWindow.getUpdateMenuSnd() && _thiscontrol)
            {
                /// flog::info("   send (msgAir.UpdateMenu)");
                msgAir.lnaGain = gui::mainWindow.getlnaGain();
                msgAir.vgaGain = gui::mainWindow.getvgaGain();
                msgAir.mixerGain = gui::mainWindow.getmixerGain();
                msgAir.linearGain = gui::mainWindow.getlinearGain();
                msgAir.sensitiveGain = gui::mainWindow.getsensitiveGain();
                msgAir.gainMode = gui::mainWindow.getgainMode();
                msgAir.lnaAgc = gui::mainWindow.getlnaAgc();
                msgAir.mixerAgc = gui::mainWindow.getmixerAgc();
                msgAir.select = gui::mainWindow.getselect();
                msgAir._updateLinearGain = gui::mainWindow.get_updateLinearGain();
                msgAir.UpdateMenu = true; // gui::mainWindow.getUpdateMenuSnd();
                flog::info("PACKET_TYPE_AIRSPY send (msgAir.UpdateMenu) {0}, linearGain() {1}, vgaGain = {2}", gui::mainWindow.getUpdateMenuSnd(), msgAir.linearGain, msgAir.vgaGain);
                _this->ib_pkt_hdr->type = PACKET_TYPE_AIRSPY;
                _this->ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgAir);
                _this->ib_pkt_hdr->sizeOfExtension = 0;
                memcpy(&_this->ibuf[sizeof(InfoHeader)], (uint8_t *)&msgAir, sizeof(msgAir));
                // gui::mainWindow.setUpdateMenuSnd(false);
                send_airspy = true;
            }
            else if (gui::mainWindow.getUpdateMenuSnd2Radio() && _thiscontrol)
            { // RADIO
                msgRadio.selectedDemodID = gui::mainWindow.getselectedDemodID();
                msgRadio.bandwidth = gui::mainWindow.getbandwidth();
                msgRadio.snapInterval = gui::mainWindow.getsnapInterval();
                msgRadio.snapIntervalId = gui::mainWindow.getsnapIntervalId();
                msgRadio.deempId = gui::mainWindow.getdeempId();
                msgRadio.nbEnabled = gui::mainWindow.getnbEnabled();
                msgRadio.nbLevel = gui::mainWindow.getnbLevel();
                msgRadio.baseband_band = gui::mainWindow.getCMO_BBand();
                msgRadio.squelchEnabled = gui::mainWindow.getsquelchEnabled();
                msgRadio.squelchLevel = gui::mainWindow.getsquelchLevel();
                msgRadio.FMIFNREnabled = gui::mainWindow.getFMIFNREnabled();
                msgRadio.fmIFPresetId = gui::mainWindow.getfmIFPresetId();
                msgRadio._lowPass = gui::mainWindow.getlowPass();
                msgRadio._highPass = gui::mainWindow.gethighPass();
                msgRadio.agcAttack = gui::mainWindow.getagcAttack();
                msgRadio.agcDecay = gui::mainWindow.getagcDecay();
                msgRadio.carrierAgc = gui::mainWindow.getcarrierAgc();
                msgRadio.UpdateMenu = true;
                flog::info("PACKET_TYPE_RADIO SEND (msgRadio.UpdateMenu) {0}, selectedDemodID {1}", msgRadio.UpdateMenu, msgRadio.selectedDemodID);
                _this->ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgRadio);
                _this->ib_pkt_hdr->type = PACKET_TYPE_RADIO;
                _this->ib_pkt_hdr->sizeOfExtension = 0;
                memcpy(&_this->ibuf[sizeof(InfoHeader)], (uint8_t *)&msgRadio, sizeof(msgRadio));
                // gui::mainWindow.setUpdateMenuSnd2Radio(false);
                send_radio = true;
            }
            else if (gui::mainWindow.getUpdateMenuSnd3Record() && _thiscontrol)
            { // RADIO
                msgRecord.recording = gui::mainWindow.getServerRecording(_this->currSrv);
                msgRecord.UpdateMenu = true;
                flog::info("PACKET_TYPE_RECORD send.  msgRecord.recording  {0}", msgRecord.recording);
                _this->ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgRecord);
                _this->ib_pkt_hdr->type = PACKET_TYPE_RECORD;
                _this->ib_pkt_hdr->sizeOfExtension = 0;
                memcpy(&_this->ibuf[sizeof(InfoHeader)], (uint8_t *)&msgRecord, sizeof(msgRecord));
                // gui::mainWindow.setUpdateMenuSnd3Record(false);
                send_record = true;
            }
            else if (gui::mainWindow.getUpdateMenuSnd5Srch(_this->currSrv) && _thisNetOK)
            { // SEARCH
                msgSearch.selectedLogicId = gui::mainWindow.getselectedLogicId(_this->currSrv);
                msgSearch.idOfList_srch  = gui::mainWindow.getidOfList_srch(_this->currSrv);
                msgSearch.button_srch    = gui::mainWindow.getbutton_srch(_this->currSrv);
                msgSearch.levelDb        = gui::mainWindow.getLevelDbSrch(_this->currSrv);
                msgSearch.SNRlevelDb     = gui::mainWindow.getSNRLevelDb(_this->currSrv);
                msgSearch.status_AKF     = gui::mainWindow.getAKFInd(_this->currSrv);
                msgSearch.UpdateModule   = gui::mainWindow.getUpdateModule_srch(_this->currSrv);
                msgSearch.UpdateLists    = gui::mainWindow.getUpdateLists_srch();
                msgSearch.statAutoLevel  = gui::mainWindow.getAuto_levelSrch(_this->currSrv);
                msgSearch.UpdateMenu     = true;

                // === 1. Размер extension ===
                int extSize = 0;
                if (msgSearch.UpdateLists)
                    extSize = gui::mainWindow.getsizeOfbbuf_srch();

                // === 2. Проверка на переполнение ibuf ===
                size_t totalPacketSize = sizeof(InfoHeader) + sizeof(msgSearch) + (size_t)extSize;
                if (totalPacketSize > MAX_INFO_BUFFER_SIZE)
                {
                    flog::error("PACKET_TYPE_SEARCH: totalSize {0} > MAX_INFO_BUFFER_SIZE {1}, drop extension",
                                totalPacketSize, MAX_INFO_BUFFER_SIZE);
                    msgSearch.UpdateLists          = false;
                    _this->ib_pkt_hdr->sizeOfExtension = 0;
                    extSize                         = 0;
                }

                flog::info("PACKET_TYPE_SEARCH. Send 2. msgSearch.idOfList_srch {0}, extSize {1}, button_srch {2}, currSrv {3}, autoLevel {4}",
                           msgSearch.idOfList_srch, extSize, msgSearch.button_srch, _this->currSrv, msgSearch.statAutoLevel);

                memcpy(&_this->ibuf[sizeof(InfoHeader)], (uint8_t *)&msgSearch, sizeof(msgSearch));
                _this->ib_pkt_hdr->type = PACKET_TYPE_SEARCH;
                _this->ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgSearch);

                if (msgSearch.UpdateLists && extSize > 0)
                {
                    _this->ib_pkt_hdr->sizeOfExtension = extSize;
                    memcpy(&_this->ibuf[_this->ib_pkt_hdr->size], (void *)gui::mainWindow.getbbuf_srch(), extSize);
                    _this->ib_pkt_hdr->size += extSize;
                }
                else
                {
                    _this->ib_pkt_hdr->sizeOfExtension = 0;
                }

                flog::info("PACKET_TYPE_SEARCH. Send. sizeOfExtension {0}, total_size {1}",
                           _this->ib_pkt_hdr->sizeOfExtension, _this->ib_pkt_hdr->size);

                gui::mainWindow.setUpdateMenuSnd5Srch(_this->currSrv, false);
            }
            else if (gui::mainWindow.getUpdateMenuSnd6Scan(_this->currSrv) && _thisNetOK)
            { // SCAN
                msgScan.idOfList_scan   = gui::mainWindow.getidOfList_scan(_this->currSrv);
                msgScan.button_scan     = gui::mainWindow.getbutton_scan(_this->currSrv);
                msgScan.maxRecWaitTime  = gui::mainWindow.getMaxRecWaitTime_scan(_this->currSrv);
                msgScan.maxRecDuration  = gui::mainWindow.getMaxRecDuration_scan(_this->currSrv);
                msgScan.flag_level      = true;
                msgScan.level           = gui::mainWindow.getLevelDbScan(_this->currSrv);
                msgScan.statAutoLevel   = gui::mainWindow.getAuto_levelScan(_this->currSrv);
                msgScan.UpdateModule    = gui::mainWindow.getUpdateModule_scan(_this->currSrv);
                msgScan.UpdateLists     = gui::mainWindow.getUpdateLists_scan();
                msgScan.UpdateMenu      = true;

                // === 1. Вычисляем размеры ===
                int extSize = 0;
                if (msgScan.UpdateLists)
                    extSize = gui::mainWindow.getsizeOfbbuf_scan();

                // === 2. Проверяем, влезет ли всё в ibuf ===
                size_t totalPacketSize = sizeof(InfoHeader) + sizeof(msgScan) + (size_t)extSize;
                if (totalPacketSize > MAX_INFO_BUFFER_SIZE)
                {
                    flog::error("PACKET_TYPE_SCAN: totalSize {0} > MAX_INFO_BUFFER_SIZE {1}, drop packet",
                                totalPacketSize, MAX_INFO_BUFFER_SIZE);
                    gui::mainWindow.setUpdateMenuSnd6Scan(_this->currSrv, false);
                }
                else
                {
                    // === 3. Формируем заголовок и тело ===
                    _this->ib_pkt_hdr->type = PACKET_TYPE_SCAN;

                    memcpy(&_this->ibuf[sizeof(InfoHeader)], (uint8_t *)&msgScan, sizeof(msgScan));
                    _this->ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgScan);

                    // === 4. Добавляем расширение (списки частот) ===
                    if (msgScan.UpdateLists && extSize > 0)
                    {
                        _this->ib_pkt_hdr->sizeOfExtension = extSize;
                        memcpy(&_this->ibuf[_this->ib_pkt_hdr->size], (void *)gui::mainWindow.getbbuf_scan(), extSize);
                        _this->ib_pkt_hdr->size += extSize;
                    }
                    else
                    {
                        _this->ib_pkt_hdr->sizeOfExtension = 0;
                    }

                    flog::info("PACKET_TYPE_SCAN. Send. idOfList {0}, size {1}, ext {2}",
                               msgScan.idOfList_scan, _this->ib_pkt_hdr->size, _this->ib_pkt_hdr->sizeOfExtension);

                    gui::mainWindow.setUpdateMenuSnd6Scan(_this->currSrv, false);
                }
            }                        
            else if (gui::mainWindow.getUpdateMenuSnd7Ctrl(_this->currSrv) && _thisNetOK) // CTRL
            {
                msgCTRL.idOfList_ctrl = gui::mainWindow.getidOfList_ctrl(_this->currSrv);
                msgCTRL.button_ctrl = gui::mainWindow.getbutton_ctrl(_this->currSrv);
                msgCTRL.flag_level = gui::mainWindow.getflag_level_ctrl(_this->currSrv);
                msgCTRL.level = gui::mainWindow.getLevelDbCtrl(_this->currSrv);
                msgCTRL.UpdateModule = gui::mainWindow.getUpdateModule_ctrl(_this->currSrv);
                msgCTRL.maxRecWaitTime = gui::mainWindow.getMaxRecWaitTime_ctrl(_this->currSrv);
                msgCTRL.statAutoLevel = gui::mainWindow.getAuto_levelCtrl(_this->currSrv);
                msgCTRL.status_AKF = gui::mainWindow.getAKFInd_ctrl(_this->currSrv);

                // ВАЖНО: решаем, есть ли списки, по реальному размеру буфера, а не по флагу
                int extSize = gui::mainWindow.getsizeOfbbuf_ctrl();
                msgCTRL.UpdateLists = (extSize > 0);

                msgCTRL.UpdateMenu = true;

                _this->ib_pkt_hdr->type = PACKET_TYPE_CTRL;
                _this->ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgCTRL);
                _this->ib_pkt_hdr->sizeOfExtension = 0;

                memcpy(&_this->ibuf[sizeof(InfoHeader)],
                       (uint8_t *)&msgCTRL,
                       sizeof(msgCTRL));

                if (msgCTRL.UpdateLists && extSize > 0)
                {
                    _this->ib_pkt_hdr->sizeOfExtension = extSize;
                    memcpy(&_this->ibuf[_this->ib_pkt_hdr->size],
                           (void *)gui::mainWindow.getbbuf_ctrl(),
                           extSize);
                    _this->ib_pkt_hdr->size += extSize;
                }

                flog::info("CTRL. Send. srv={0}, idOfList_ctrl={1}, size={2}, ext={3}, button_ctrl={4}",
                            _this->currSrv, msgCTRL.idOfList_ctrl, _this->ib_pkt_hdr->size, _this->ib_pkt_hdr->sizeOfExtension, msgCTRL.button_ctrl);

                // НЕ трогаем здесь UpdateListRcv7Ctrl — его использует модуль Спостереження для своей логики
                gui::mainWindow.setUpdateMenuSnd7Ctrl(_this->currSrv, false);
            }
            else if (statusServer > ARM_STATUS_NOT_CONTROL)
            {
                msgMain.status = _this->getStatusControl();
                msgMain.id_control = 0;
                msgMain.statusServer = statusServer;
                msgMain.freq = 0;
                if (gui::mainWindow.getUpdateMenuSnd0Main(_this->currSrv))
                { //
                    msgMain.playing = gui::mainWindow.isARMPlaying();
                    msgMain.isNotPlaying = true;
                    msgMain.recording = gui::mainWindow.getServerRecording(_this->currSrv);
                    msgMain.selectedLogicId = gui::mainWindow.getselectedLogicId(_this->currSrv);
                    msgMain.idOfList_srch = gui::mainWindow.getidOfList_srch(_this->currSrv);
                    msgMain.search = gui::mainWindow.getbutton_srch(_this->currSrv);
                    msgMain.scan = gui::mainWindow.getbutton_scan(_this->currSrv);
                    msgMain.idOfList_scan = gui::mainWindow.getidOfList_scan(_this->currSrv);
                    msgMain.control = gui::mainWindow.getbutton_ctrl(_this->currSrv);
                    msgMain.idOfList_control = gui::mainWindow.getidOfList_ctrl(_this->currSrv);
                    msgMain.setLevelDbSrch = gui::mainWindow.getLevelDbSrch(_this->currSrv);
                    msgMain.setLevelDbScan = gui::mainWindow.getLevelDbScan(_this->currSrv);
                    msgMain.setLevelDbCtrl = gui::mainWindow.getLevelDbCtrl(_this->currSrv);
                    msgMain.statAutoLevelSrch = gui::mainWindow.getAuto_levelSrch(_this->currSrv);
                    msgMain.statAutoLevelScan = gui::mainWindow.getAuto_levelScan(_this->currSrv);
                    msgMain.statAutoLevelCtrl = gui::mainWindow.getAuto_levelCtrl(_this->currSrv);
                    msgMain.SNRLevelDbSrch = gui::mainWindow.getSNRLevelDb(_this->currSrv);
                    msgMain.status_AKFSrch = gui::mainWindow.getAKFInd(_this->currSrv);
                    msgMain.status_AKFCtrl = gui::mainWindow.getAKFInd_ctrl(_this->currSrv);
                    msgMain.maxRecWaitTimeCtrl = gui::mainWindow.getMaxRecWaitTime_ctrl(_this->currSrv);

                    flog::info(" msgMain.idOfList_control={0}", msgMain.idOfList_control);
                    msgMain.tuningMode = gui::mainWindow.gettuningMode();
                    double t_freq = 0;
                    t_freq = gui::waterfall.getCenterFrequency();
                    if (msgMain.tuningMode == tuner::TUNER_MODE_NORMAL)
                    {
                        // flog::info("1 t_freq {0}", gui::waterfall.getCenterFrequency());
                        if (gui::waterfall.vfos.find("Канал приймання") != gui::waterfall.vfos.end())
                        {
                            _offset = gui::waterfall.vfos["Канал приймання"]->generalOffset;
                        }
                    }
                    else
                    {
                        // flog::info("2 t_freq {0}", gui::waterfall.getCenterFrequency());
                        _offset = 0;
                        // t_freq = _this->currentFreq;
                    }

                    msgMain.freq = t_freq;
                    msgMain.offset = _offset;
                    flog::info("\n      TRACE SND (msgMain) msgMain.tuningMode {0}, gui::mainWindow.gettuningMode() {1}, msgMain.freq {2}", msgMain.tuningMode, gui::mainWindow.gettuningMode(), msgMain.freq);

                    // flog::warn("ARM_STATUS_NOT_CONTROL  freq {0}, _offset = {1}, msgMain.tuningMode {2}", msgMain.freq, _offset, msgMain.tuningMode);

                    msgMain.UpdateMenu = true;
                    // gui::waterfall.VFOMoveSingleClick;
                    // std::string nameVFO = "Канал приймання"; // _streamName; // gui::waterfall.selectedVFO;

                    // flog::info("   gui::mainWindow.getUpdateMenuSnd0Main() == true. msgMain.status {0}, _this->currSrv {1}, msgMain.freq {2}, _offset {3}", msgMain.status, _this->currSrv, msgMain.freq, _offset);

                    // gui::waterfall.VFOMoveSingleClick = (bool)msgMain.tuningMode;

                    msgMain.level = _this->getVolLevel();
                    flog::info("PACKET_TYPE_MAIN SEND currSrv={0}, freq {1}, offset {2},  recording {3}, playing {4}, scan {5}, idOfList_srch {6}, idOfList_scan {7}, control {8}, idOfList_control {9}", _this->currSrv, msgMain.freq, msgMain.offset, msgMain.recording, msgMain.playing, msgMain.scan, msgMain.idOfList_srch, msgMain.idOfList_scan, msgMain.control, msgMain.idOfList_control);

                    send_main = true;
                }
                else
                {
                    msgMain.UpdateMenu = false;
                    // flog::info("PACKET_TYPE_MAIN send 2 (msgRadio.UpdateMenu) {0}, msgMain.statusServer {1}", msgMain.UpdateMenu, msgMain.statusServer);
                }

                _this->ib_pkt_hdr->size = sizeof(InfoHeader) + sizeof(msgMain); // sizeof(InfoHeader);
                _this->ib_pkt_hdr->type = PACKET_TYPE_MAIN;
                _this->ib_pkt_hdr->sizeOfExtension = 0;
                // flog::info("PACKET_TYPE_MAIN send, msgMain.freq {0}", msgMain.freq);
                memcpy(&_this->ibuf[sizeof(InfoHeader)], (uint8_t *)&msgMain, sizeof(msgMain));
            }
            //->write
            // if (_this->ib_pkt_hdr->type >= 0)
            // flog::info("SEND _this->ib_pkt_hdr->type {0}, statusServer {1}, _this->currSrv {2}, getUpdateMenuSnd0Main {3}", _this->ib_pkt_hdr->type, statusServer, _this->currSrv, gui::mainWindow.getUpdateMenuSnd0Main(_this->currSrv));
            // _this->sock->send(_this->ibuf, _this->ib_pkt_hdr->size);
            // Отправка готового пакета
            if (_this->sock->send(_this->ibuf, _this->ib_pkt_hdr->size) <= 0)
            {
                flog::error("WORKER_INFO: Failed to send response to srv {0}. Closing connection.", _this->currSrv);
                break;
            }
            if (rcv_stat)
                gui::mainWindow.setUpdateMenuRcv0Main(_this->currSrv, true);
            if (rcv_main)
                gui::mainWindow.setUpdateMenuRcv0Main(_this->currSrv, true);
            // _this->clntsending = false;
        }
        delete[] _this->ibuf;
        _this->ibuf = nullptr;
        // gui::mainWindow.setServerStatus(_this->currSrv, ARM_STATUS_NOT_CONTROL);
    }

    std::shared_ptr<TCPRemoveARM> connectARMData(dsp::stream<dsp::complex_t> *stream, std::string host, int port, uint8_t id)
    {
        auto sock = net::connect(host, port);
        // auto sockInfo = net::connect(host, port+2);
        flog::info("!connectARMData host {0}, port {1}, id {2}", host, port, id);
        return std::make_shared<TCPRemoveARM>(sock, stream, true, id);
    }

    std::shared_ptr<TCPRemoveARM> connectARMInfo(dsp::stream<dsp::complex_t> *stream, std::string host, int port, uint8_t id)
    {
        // auto sock = net::connect(host, port);
        auto sockInfo = net::connect(host, port + 2);
        flog::info("connectARMInfo host {0}, port {1}, id {2}", host, port + 2, id);
        return std::make_shared<TCPRemoveARM>(sockInfo, stream, false, id);
    }
}