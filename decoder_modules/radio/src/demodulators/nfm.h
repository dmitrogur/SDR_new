#pragma once
#include "../demod.h"
#include <dsp/demod/fm.h>

namespace demod {
    class NFM : public Demodulator {
    public:
        NFM() {}

        NFM(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~NFM() { stop(); }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            this->_config = config;

            // Load config
            _config->acquire();
            if (config->conf[name][getName()].contains("lowPass")) {
                _lowPass = config->conf[name][getName()]["lowPass"];
            }
            if (config->conf[name][getName()].contains("highPass")) {
                _highPass = config->conf[name][getName()]["highPass"];
            }
            _config->release();


            // Define structure
            demod.init(input, getIFSampleRate(), bandwidth, _lowPass, _highPass);
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            if (gui::mainWindow.getUpdateMODRadio()) {
                _lowPass = gui::mainWindow.getlowPass();
                _highPass = gui::mainWindow.gethighPass();
                gui::mainWindow.setUpdateMODRadio(false);
            }            
            _lowPass = false;
            _highPass =false;
            /*
            bool update_menu = false;
            if (ImGui::Checkbox(("ФНЧ##_radio_wfm_lowpass_" + name).c_str(), &_lowPass)) {
                demod.setLowPass(_lowPass);
                _config->acquire();
                _config->conf[name][getName()]["lowPass"] = _lowPass;
                _config->release(true);
                update_menu = true;
            }
            if (ImGui::Checkbox(("ФВЧ##_radio_wfm_highpass_" + name).c_str(), &_highPass)) {
                demod.setHighPass(_highPass);
                _config->acquire();
                _config->conf[name][getName()]["highPass"] = _highPass;
                _config->release(true);
                update_menu = true;
            }
            if (update_menu == true) {
                gui::mainWindow.setlowPass(_lowPass);
                gui::mainWindow.sethighPass(_highPass);
                gui::mainWindow.setUpdateMenuSnd2Radio(true); //_this->update_menu
            }
            */
        }

        void setBandwidth(double bandwidth) {
            demod.setBandwidth(bandwidth);
        }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "FM"; }
        double getIFSampleRate() { return 250000.0; } // 50000.0
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 12500.0; }
        double getMinBandwidth() { return 1000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 2500.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return true; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return true; }
        bool getNBAllowed() { return false; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }
        dsp::stream<dsp::complex_t>* getIQOutput() { return nullptr; }

    private:
        dsp::demod::FM<dsp::stereo_t> demod;
        // dsp::demod::pureIQ<dsp::complex_t> demodIQ;

        ConfigManager* _config = NULL;

        bool _lowPass = true;
        bool _highPass = false;

        std::string name;
    };
}