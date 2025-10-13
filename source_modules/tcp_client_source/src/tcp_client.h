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

    class TCPClient {
    public:
        TCPClient(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* stream, bool datatype);
        ~TCPClient();

        bool isOpen();
        // bool isOpenInfo();
        void close();

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

        int getGainMode();
        void setLinearGain(int linearGain, bool updt = false);
        int getLinearGain();

    private:
        // void sendCommand(uint8_t command, uint32_t param);
        static void worker(void* ctx);
        static void workerInfo(void* ctx);

        std::shared_ptr<net::Socket> sock;

        std::thread workerThread;
        dsp::stream<dsp::complex_t>* stream;
        
        double currentFreq = 0;
        unsigned int currentSampleRate = 192000;
        int bufferSize = currentSampleRate / 200;

        bool datatype = false;
        bool current  = false;
        bool _start_server;
        bool clntsending=false;
        float _squelchLevel;
        int _gainMode = 1;
        int _linearGain = 0;

    dsp::stream<uint8_t> decompIn;
    dsp::compression::SampleStreamDecompressor decomp;
    dsp::routing::StreamLink<dsp::complex_t> link;
    dsp::stream<dsp::complex_t>* output;
    struct PacHeader {
        uint32_t type;
        uint32_t size;
    };
    PacHeader* r_pkt_hdr = NULL;
    uint8_t* r_pkt_data = NULL;
    uint8_t* rbuffer = NULL;

    };

    std::shared_ptr<TCPClient> connect(dsp::stream<dsp::complex_t>* stream, std::string host, int port = 1234);
    std::shared_ptr<TCPClient> connectInfo(dsp::stream<dsp::complex_t>* stream, std::string host, int port = 1236);
}
