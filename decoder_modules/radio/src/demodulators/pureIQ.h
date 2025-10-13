// File: /SDRPlusPlus/decoder_modules/radio/src/demodulators/pureiq.h

#pragma once
#include "../demod.h"
#include <dsp/demod/pureIQ.h>
#include <dsp/convert/complex_to_stereo.h>

namespace demod {
    class pureIQ : public Demodulator {
    public:
        pureIQ() = default;

        pureIQ(std::string name, dsp::stream<dsp::complex_t>* input, double bandwidth, double samplerate, double audioSR) {
            init(name, nullptr, input, bandwidth, audioSR);
        }

        ~pureIQ() { stop(); }

        void init(std::string name, ConfigManager* /*config*/, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) override {
            this->name = name;
            _outputSampleRate = audioSR;
            // demodIQ.init(name, input, bandwidth, audioSR, getAFSampleRate());
            double ifSampleRate = getIFSampleRate();
            // Инициализация внутреннего фильтра
            demodIQ.init(name, input, bandwidth, ifSampleRate, _outputSampleRate);
            // Подключаем преобразователь complex → stereo
            c2s.init(&demodIQ.out);            
        }

        void setBandwidth(double bandwidth) {
            demodIQ.setBandwidth(bandwidth);
        }

        void setInput(dsp::stream<dsp::complex_t>* input) override {
            demodIQ.setInput(input);
        }

        void AFSampRateChanged(double newSR) {
            _outputSampleRate = newSR;
        }

        void start() {
            fprintf(stderr, "[demod::pureIQ] 2 starting...\n");
            demodIQ.start();
            c2s.start();
        }

        void stop() {
            c2s.stop();
            demodIQ.stop();
        }

        void showMenu() {
            if (gui::mainWindow.getUpdateMODRadio()) {
                gui::mainWindow.setUpdateMODRadio(false);
            }            
            ImGui::Text("IQ narrowband recorder");
        }

        // ============= INFO =============
        const char* getName() override { return "pureIQ"; }
        double getIFSampleRate() override { return _inputSampleRate; }
        double getAFSampleRate() override { return _outputSampleRate; }
        double getDefaultBandwidth() override { return 12500.0; }
        double getMinBandwidth() override { return 4000.0; }
        double getMaxBandwidth() override { return getIFSampleRate(); }
        bool getBandwidthLocked() override { return true; }
        double getDefaultSnapInterval() override { return 1000.0; }
        int getVFOReference() override { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() override { return false; }
        bool getPostProcEnabled() override { return false; }
        int getDefaultDeemphasisMode() override { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() override { return false; }
        bool getNBAllowed() override { return false; }

        dsp::stream<dsp::stereo_t>* getOutput() { return &c2s.out; } // &c2s.out;
        dsp::stream<dsp::complex_t>* getIQOutput() { return &demodIQ.out; }
        // void* getDSP() override { return &demodIQ; }

    private:
        double _outputSampleRate = 96000.0;
        double _inputSampleRate = 10'000'000.0;        
        dsp::convert::ComplexToStereo c2s;
        dsp::demod::pureIQ<dsp::complex_t> demodIQ;
        std::string name;
    };
}
