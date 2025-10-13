#pragma once
#include "../demod.h"
#include <dsp/demod/ssb.h>

namespace demod {
    class LSB : public Demodulator {
    public:
        LSB() {}

        LSB(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~LSB() {
            stop();
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _config = config;

            // Load config
            config->acquire();
            if (config->conf[name][getName()].contains("agcAttack")) {
                agcAttack = config->conf[name][getName()]["agcAttack"];
            }
            if (config->conf[name][getName()].contains("agcDecay")) {
                agcDecay = config->conf[name][getName()]["agcDecay"];
            }
            config->release();

            // Define structure
            demod.init(input, dsp::demod::SSB<dsp::stereo_t>::Mode::LSB, bandwidth, getIFSampleRate(), agcAttack / getIFSampleRate(), agcDecay / getIFSampleRate());
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            if (gui::mainWindow.getUpdateMODRadio()) {
                agcAttack = gui::mainWindow.getagcAttack();
                agcDecay = gui::mainWindow.getagcDecay();
                gui::mainWindow.setUpdateMODRadio(false);
            }            
            bool update_menu =false;
            float menuWidth = ImGui::GetContentRegionAvail().x;
            ImGui::LeftLabel("Наростання АРП ");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_lsb_agc_attack_" + name).c_str(), &agcAttack, 1.0f, 200.0f)) {
                demod.setAGCAttack(agcAttack / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcAttack"] = agcAttack;
                _config->release(true);
                update_menu = true;
            }
            ImGui::LeftLabel("Спад АРП              ");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_lsb_agc_decay_" + name).c_str(), &agcDecay, 1.0f, 20.0f)) {
                demod.setAGCDecay(agcDecay / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcDecay"] = agcDecay;
                _config->release(true);
                update_menu = true;
            }
            if (update_menu == true) {
                gui::mainWindow.setagcAttack(agcAttack);
                gui::mainWindow.setagcDecay(agcDecay);
                gui::mainWindow.setUpdateMenuSnd2Radio(true);
            }              
        }

        void setBandwidth(double bandwidth) { demod.setBandwidth(bandwidth); }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "LSB"; }
        double getIFSampleRate() { return 25000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 2700.0; }
        double getMinBandwidth() { return 500.0; }
        double getMaxBandwidth() { return getIFSampleRate() / 2.0; }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 100.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_UPPER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }
        dsp::stream<dsp::complex_t>* getIQOutput() { return nullptr; }

    private:
        dsp::demod::SSB<dsp::stereo_t> demod;
        // dsp::demod::pureIQ<dsp::complex_t> demodIQ;

        ConfigManager* _config;

        float agcAttack = 50.0f;
        float agcDecay = 5.0f;

        std::string name;
    };
}