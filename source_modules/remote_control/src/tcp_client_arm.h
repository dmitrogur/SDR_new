#pragma once
#include <utils/net.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <thread>
#include <dsp/routing/stream_link.h>
#include <dsp/compression/sample_stream_decompressor.h>

namespace server {
#pragma pack(push, 1)
    struct Command {
        uint8_t cmd;
        uint32_t param;
    };
#pragma pack(pop)
    enum {
        ARM_STATUS_NOT_CONTROL,
        ARM_STATUS_STAT_CONTROL,
        ARM_STATUS_FULL_CONTROL
    };

    class TCPRemoveARM {
    public:
        TCPRemoveARM(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* stream, bool datatype, uint8_t id);
        ~TCPRemoveARM();

        bool isOpen();
        // bool isOpenInfo();
        void close();
        void setStatusControl(int val);
        int getStatusControl();
        uint8_t getCurrSrv() {
            return currSrv;
        }
        void setFrequency(double freq);
        void setSampleRate(double sr);
        double getCurrFrequency();
        unsigned int getCurrSampleRate();
        void setUpdate(bool curr);
        bool getUpdate();
        void updateStream();

        int bytes = 0;
        void start_server(bool val);
        bool server_status();

        float getVolLevel();
        void setVolLevel(float val, bool updt = false);

        void reconnect(std::shared_ptr<net::Socket> new_sock, uint8_t id);

    private:
        // void sendCommand(uint8_t command, uint32_t param);
        static void worker(void* ctx);
        static void workerInfo(void* ctx);

        std::shared_ptr<net::Socket> sock;

        std::thread workerThread;
        int SERVERS_Count = 8;
        double currentFreq = 0;
        // int bufferSize = currentSampleRate / 200;

        bool datatype = false;
        bool current = false;
        bool first = false;
        bool _start_server;
        bool clntsending = false;
        float _squelchLevel = 0.2;
        // int _gainMode = 1;
        // int _linearGain = 0;

        dsp::stream<dsp::complex_t> decompIn;
        dsp::routing::StreamLink<dsp::complex_t> link;

        struct InfoHeader {
            uint32_t type;
            uint32_t size;
            uint32_t sizeOfExtension;
        };
        uint8_t* bbuf = NULL;
        InfoHeader* bb_pkt_hdr = NULL;
        uint8_t* bb_pkt_data = NULL;

        uint8_t* ibuf = NULL;
        InfoHeader* ib_pkt_hdr = NULL;
        uint8_t* ib_pkt_data = NULL;

        uint8_t currSrv = 0;
        int statusControl = 0;
    };

    std::shared_ptr<TCPRemoveARM> connectARMData(dsp::stream<dsp::complex_t>* stream, std::string host, int port = 1234, uint8_t id = 0);
    std::shared_ptr<TCPRemoveARM> connectARMInfo(dsp::stream<dsp::complex_t>* stream, std::string host, int port = 1236, uint8_t id = 0);
    
}
