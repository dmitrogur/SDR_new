#include "tcp_client.h"
#include <utils/flog.h>
#include <dsp/sink.h>

// #include <zstd.h>
// #include <server_protocol.h>

using namespace std::chrono_literals;

namespace server {
    bool REMOTE = true;
    bool _first = true;
    // ZSTD_DCtx* dctx;

    TCPClient::TCPClient(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* stream, bool datatype) {
        this->sock = sock;
        output = stream;
        this->datatype = datatype;
        if(REMOTE && this->datatype) {
            // Initialize decompressor
            // dctx = ZSTD_createDCtx();

            // Initialize DSP
            decompIn.setBufferSize((sizeof(dsp::complex_t) * STREAM_BUFFER_SIZE) + 8);
            decompIn.clearWriteStop();
            decomp.init(&decompIn);
            link.init(&decomp.out, output);
            decomp.start();
            link.start();
        } 
        // Start worker
        if(this->datatype) {
            flog::info("1 TCPClient {0}", datatype);
            this->stream = stream;
            workerThread = std::thread(&TCPClient::worker, this);
        } else {
            this->stream = NULL;
            flog::info("2 TCPClient {0}", datatype);
            workerThread = std::thread(&TCPClient::workerInfo, this);
        }
        _first = true;
        current = false;
        if(REMOTE) {
            currentSampleRate = 10000000;  
        } else {
            currentSampleRate = 192000;
        }            
    }

    TCPClient::~TCPClient() {
        close();
        delete[] rbuffer;        
    }

    bool TCPClient::isOpen() {
        return sock->isOpen();
    }

    void TCPClient::close() {
        flog::info("1 datatype {0}", datatype);
        if(REMOTE && this->datatype) decompIn.stopWriter();
        // flog::info("2");
        if(sock) sock->close();    
        // flog::info("3");
        if(REMOTE && this->datatype) decompIn.clearWriteStop();
        // flog::info("4");

        // sockInfo->close();
        if(!REMOTE && this->datatype) {
            stream->stopWriter();
            stream->clearWriteStop();
        }
        flog::info("5");
        if (workerThread.joinable()) {  workerThread.join(); }
        flog::info("6");
        if(REMOTE && this->datatype) {
            decomp.stop();
            link.stop();
        }
        // flog::info("7");
        _first = true;
    }

    void TCPClient::updateStream() {
        if(current) {
            // dsp::stream<dsp::complex_t>* tmpstream = stream;
            if(!REMOTE) {
                stream->stopWriter();
                stream->clearWriteStop();
            }
            //stream = dsp::stream<dsp::complex_t>* tmpstream;
        }
    }

    void TCPClient::setUpdate(bool curr) {
        current = curr;
    }

    bool TCPClient::getUpdate() {
        return current;
    }

    void TCPClient::setFrequency(double freq) {
        // sendCommand(1, freq);
        // if (!isOpen()) { return; }
        currentFreq = freq;
        flog::info("rcv setFrequency currentFreq {0}", currentFreq);   
    }

    void TCPClient::setSampleRate(double sr) {
        // if (!isOpen()) { return; }
        if(REMOTE) {
            currentSampleRate = 10000000;   
            bufferSize = 262152;
        } else {
            currentSampleRate = (unsigned int) sr;
            bufferSize = sr / 200.0;
        }
        flog::info("rcv setSampleRate currentSampleRate {0}, bufferSize {1}, getCurrSampleRate() {2}", currentSampleRate, bufferSize, getCurrSampleRate());   
    }

    double TCPClient::getCurrFrequency(){
        // flog::info("   get Frequency currentFreq {0}", currentFreq);   
        return currentFreq;
    }

    unsigned int TCPClient::getCurrSampleRate() {
        return currentSampleRate;
    }    
    
    #define PROTOCOL_TIMEOUT_MS             10000
    #define MAX_PACKET_SIZE  (STREAM_BUFFER_SIZE * sizeof(dsp::complex_t) * 2)

    void TCPClient::worker(void* ctx) {
        TCPClient* _this = (TCPClient*) ctx;
        uint8_t* buffer = dsp::buffer::alloc<uint8_t>(STREAM_BUFFER_SIZE*2);
        if(REMOTE && _this->datatype) { 
            _this->rbuffer = new uint8_t[MAX_PACKET_SIZE];
            _this->r_pkt_hdr = (PacHeader*)_this->rbuffer;
            _this->r_pkt_data = &_this->rbuffer[sizeof(PacHeader)];
        } else {

        }   
        // r_cmd_hdr = (CommandHeader*)r_pkt_data;
        // r_cmd_data = &rbuffer[sizeof(PacHeader) + sizeof(CommandHeader)];        

        while (true) {
            if(!REMOTE) { 
                if(_first) continue;
            }

            // Read data
            int count = 0;
            if(REMOTE) {  
                // Receive header
                // count = sock->recv(buffer, bufferSize, false);

                _this->r_pkt_hdr->size = 0; 
                if (_this->sock->recv(_this->rbuffer, sizeof(PacHeader), true) <= 0) {
                    break;
                }
                
                // count = sock->recv(rbuffer, sizeof(PacHeader), true);
                // memcpy(&r_pkt_hdr, rbuffer, sizeof(PacHeader));

                // Receive remaining data
                /*
                if(_this->r_pkt_hdr->size>262160 || _this->r_pkt_hdr->size==0) {                    // flog::info("worker r_pkt_hdr->size {0}, size = {1}, count = {2}", r_pkt_hdr->size, r_pkt_hdr->size - sizeof(PacHeader), count);
                    _this->r_pkt_hdr->size = 262160;
                } else {
                    // flog::info("worker  r_pkt_hdr->size {0}, size = {1}, count = {2}", r_pkt_hdr->size, r_pkt_hdr->size - sizeof(PacHeader), count);
                }
                */
                if (_this->sock->recv(&_this->rbuffer[sizeof(PacHeader)], _this->r_pkt_hdr->size - sizeof(PacHeader), true, PROTOCOL_TIMEOUT_MS) <= 0) {
                    break;
                }
                
                //count = sock->recv(&rbuffer[sizeof(PacHeader)], r_pkt_hdr->size - sizeof(PacHeader), true);
                // flog::info("count {0}", count);
                count = _this->r_pkt_hdr->size - sizeof(PacHeader);
                // currentSampleRate = 10000000;   
                // bufferSize = 262152;                
                // count = sock->recv(buffer, bufferSize, false);
            } else {    
                count = _this->sock->recv(buffer, _this->bufferSize * 2, true);
            }            
            if (count <= 0) { break; }

            // flog::info("   sock->recv!!! bufferSize {0}, getCurrSampleRate {1},  count {2}, currentSampleRate {3}", _this->bufferSize , _this->getCurrSampleRate(), count, _this->currentSampleRate);

            int scount = count/ sizeof(int16_t);
            _this->bytes += count;
            if(REMOTE) {
                memcpy(_this->decompIn.writeBuf, &_this->rbuffer[sizeof(PacHeader)], _this->r_pkt_hdr->size - sizeof(PacHeader));                
                ///memcpy(decompIn.writeBuf, &rbuffer, count);
                if (!_this->decompIn.swap(count)) { break; }
                // memcpy(_this->decompIn.writeBuf, &buf[sizeof(PacketHeader)], _this->r_pkt_hdr->size - sizeof(PacketHeader));
                // _this->decompIn.swap(_this->r_pkt_hdr->size - sizeof(PacketHeader))
            } else {
                //     volk_16i_s32f_convert_32f((float*)stream->writeBuf, (int16_t*)inBuf, 32768.0, count/sizeof(int16_t));
                // Convert to complex float
            
                float del = 256.0;            
                for (int i = 0; i < scount; i++) {
                    _this->stream->writeBuf[i].re = ((double)buffer[i * 2] - del) / del;
                    _this->stream->writeBuf[i].im = ((double)buffer[(i * 2) + 1] - del) / del;
                }
                if (!_this->stream->swap(count)) { break; }
            }
        }

        dsp::buffer::free(buffer);
    }
    
    void TCPClient::start_server(bool val){
        _start_server = val;
        clntsending = true;
        // flog::info("_start_server={0}, val={1} !!!", _start_server, val); 
    };

    bool TCPClient::server_status(){
        return _start_server;
    }

    float TCPClient::getVolLevel(){
        return _squelchLevel;
    }
    void  TCPClient::setVolLevel(float val, bool updt){
        flog::info("_squelchLevel = {0}, val = {1}");
        _squelchLevel=val;
        if(updt)
            clntsending = true;
    }

    int TCPClient::getGainMode(){
        return _gainMode;
    }
    int TCPClient::getLinearGain(){
        return _linearGain;
    }
    void  TCPClient::setLinearGain(int val, bool updt){
        _linearGain=val;
        if(updt)
            clntsending = true;
    }
        
    //====================================
    void TCPClient::workerInfo(void* ctx) {
        TCPClient* _this = (TCPClient*) ctx;        

        struct pktmsg
        {
            uint8_t id;
            uint32_t freq;
            uint32_t sampleRate;
            bool clntsending;
            bool playing;
            float level; 
            int gainMode;
            int linearGain;
            bool Airspy;
            //std::string sources;
            int sourceId;                        
        };
        pktmsg msg;

        while (true) {
            int count = _this->sock->recv((uint8_t*) &msg, sizeof(msg), true);
            flog::info("   msg.sampleRate {0}, getCurrSampleRate {1} !!!", msg.sampleRate, _this->getCurrSampleRate());
            _first = false;
            if(count==sizeof(msg)) {
                bool _update = false;
                if(_this->getCurrFrequency()!=msg.freq) {
                    _this->setFrequency(msg.freq);
                    _update = true;
                }
                if(_this->getCurrSampleRate()!=msg.sampleRate) {
                    _this->setSampleRate(msg.sampleRate);
                    _update = true;
                }
                if(!_this->clntsending && _this->getVolLevel()!=msg.level) {
                    _this->setVolLevel(msg.level, false);
                    _update = true;
                }              
                if(!_this->clntsending && _this->getGainMode()!=msg.gainMode) {
                    _this->_gainMode = msg.gainMode;
                    _update = true;
                } 
                if(!_this->clntsending && _this->getLinearGain()!=msg.linearGain) {
                    // flog::info("setLinearGain msg.linearGain = {0}, getLinearGain() {1} !!!", msg.linearGain, getLinearGain());
                    _this->setLinearGain(msg.linearGain, false);
                    _update = true;
                } 
                if(!_this->clntsending){
                    //flog::info("   clntsending {0}, _start_server {1}=msg.playing {2} !!!", clntsending, _start_server, msg.playing);
                    _this->_start_server = msg.playing;    
                } else {
                    flog::info("   clntsending {0}, _start_server {1} !!!", _this->clntsending, _this->_start_server);
                }
                // flog::info("msg.gainMode {0}", msg.gainMode);
                if(_update) {
                    flog::info("   sockInfo->recv !!! count {0}, freq {1}, sizeof(msg) {2}, sampleRate {3}, msg.gainMode {4}, msg.linearGain {5}", count, msg.freq, sizeof(msg), msg.sampleRate,msg.gainMode, msg.linearGain);
                   _this->setUpdate(true);
                }
                // Command cmd = { command, htonl(param) };
                // sock->send((uint8_t*)&cmd, sizeof(Command));     
                
                msg.clntsending = _this->clntsending;
                msg.playing = _this->_start_server;
                msg.level = _this->_squelchLevel;
                msg.gainMode = _this->_gainMode;
                msg.linearGain = _this->getLinearGain();

                _this->sock->send((uint8_t*) &msg, sizeof(msg));                
                _this->clntsending = false;
            } else {
                flog::info("   sockInfo->recv Info!!! break;");
                break;
            }
        }
    }

    std::shared_ptr<TCPClient> connect(dsp::stream<dsp::complex_t>* stream, std::string host, int port) {
        auto sock = net::connect(host, port);
        // auto sockInfo = net::connect(host, port+2);
        return std::make_shared<TCPClient>(sock, stream, true);
    }

    std::shared_ptr<TCPClient> connectInfo(dsp::stream<dsp::complex_t>* stream, std::string host, int port) {
        // auto sock = net::connect(host, port);
        auto sockInfo = net::connect(host, port+2);
        return std::make_shared<TCPClient>(sockInfo, stream, false);
    }

}