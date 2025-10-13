#pragma once
#include <utils/networking.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <server_protocol.h>

namespace aster_server {    

    class AServer {
    public:
        AServer();
        ~AServer();

        void start();
        void stop();

        int bytes = 0;
        bool serverBusy = false;

    private:
        void worker();
        std::thread workerThread;
        bool _run;
    };


    void setInput(dsp::stream<dsp::complex_t>* stream);
    int main();

    void _clientHandler(net::Conn conn, void* ctx);
    void _packetHandler(int count, uint8_t* buf, void* ctx);
    void _testServerHandler(uint8_t* data, int count, void* ctx);

    void drawMenu();

    void commandHandler(aster_server::Command cmd, uint8_t* data, int len);
    void renderUI(SmGui::DrawList* dl, std::string diffId, SmGui::DrawListElem diffValue);
    void sendUI(aster_server::Command originCmd, std::string diffId, SmGui::DrawListElem diffValue);
    void sendError(Error err);
    void sendSampleRate(double sampleRate);
    void setInputSampleRate(double samplerate);

    void sendPacket(PacketType type, int len);
    void sendCommand(aster_server::Command cmd, int len);
    void sendCommandAck(aster_server::Command cmd, int len);
}
