#include <signal_path/signal_path.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/tuner.h>
#include <string>
#include <gui/menus/source.h>

namespace tuner {

    void setVFOGeneralOffset(double old_getCenterFrequency) {
        if (gui::waterfall.selectedVFO != "Канал приймання")
            return;
        std::string tunesourceName = gui::waterfall.getSource();
        if (tunesourceName == "File" || tunesourceName == "Файл") {
        }
        else {
            for (auto const& [name, vfo] : gui::waterfall.vfos) {
                if (name == "Канал приймання"){
                    core::configManager.acquire();
                    core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
                    core::configManager.release(true);      
                } 
                /*
                else if(gui::mainWindow.getbutton_ctrl(gui::mainWindow.getCurrServer()) && gui::mainWindow.getupdateStart_ctrl(gui::mainWindow.getCurrServer())) {
                    flog::info("old_getCenterFrequency {0}, generalOffset {1}", old_getCenterFrequency, vfo->generalOffset);
                    gui::mainWindow.setupdateStart_ctrl(gui::mainWindow.getCurrServer(), false);

                    if (old_getCenterFrequency == 0) {
                        gui::waterfall.setPrevCenterFrequency(gui::waterfall.getCenterFrequency());
                        old_getCenterFrequency = gui::waterfall.getPrevCenterFrequency();
                    }
                    double CurrFreq = (old_getCenterFrequency + vfo->generalOffset);

                    flog::info("_. CurrFreq({0}) = old_getCenterFrequency() {1} + vfo->generalOffset {2}", CurrFreq, old_getCenterFrequency, vfo->generalOffset);
                    double generalOffset = CurrFreq - gui::waterfall.getCenterFrequency();
                    flog::info("_. NEW generalOffset({0}) = CurrFreq {1} - gui::waterfall.getCenterFrequency() {2}", generalOffset, CurrFreq,  gui::waterfall.getCenterFrequency());
                    flog::info("setVFOGeneralOffset. tuning ({0}), old generalOffset {1}, new generalOffset {2}", name,  vfo->generalOffset, generalOffset);
                    vfo->setOffset(generalOffset);
                    core::configManager.acquire();
                    core::configManager.conf["vfoOffsets"][name] = vfo->generalOffset;
                    flog::info("VFOGeneralOffset ({0}). generalOffset({1}), CFreq = {2}, old_CFreq {3}", name, vfo->generalOffset, gui::waterfall.getCenterFrequency(), old_getCenterFrequency);
                    core::configManager.release(true);
                }
                */
            }
        }
    }


    void centerTuning(std::string vfoName, double freq) {
        double old_getCenterFrequency = gui::waterfall.getCenterFrequency();
        double freq_offset = gui::freqSelect.old_freq - freq;
        // flog::info("centerTuning gui::waterfall.vfos[{0}] {1}, old_getCenterFrequency {2}, freq_offset {3}", vfoName, freq, old_getCenterFrequency, freq_offset);
        if (vfoName != "") {
            if (gui::waterfall.vfos.find(vfoName) == gui::waterfall.vfos.end()) { return; }
            sigpath::vfoManager.setOffset(vfoName, 0);
        }
        double BW = gui::waterfall.getBandwidth();
        double viewBW = gui::waterfall.getViewBandwidth();
        gui::waterfall.setViewOffset((BW / 2.0) - (viewBW / 2.0));
        gui::waterfall.setCenterFrequency(freq);
        gui::waterfall.setViewOffset(0);
        gui::freqSelect.setFrequency(freq);
        if (sourcemenu::getCurrSource() != SOURCE_ARM) {
            sigpath::sourceManager.tune(freq);
        }
        setVFOGeneralOffset(old_getCenterFrequency);
    }

    void normalTuning(std::string vfoName, double freq, bool remote) {
        // flog::info("normalTuning gui::waterfall.vfos[{0}] {1}, remote {2}", vfoName, freq, remote);
        if (vfoName == "") {
            centerTuning(vfoName, freq);
            return;
        }
        if (gui::waterfall.vfos.find(vfoName) == gui::waterfall.vfos.end()) { return; }
        // flog::info("normalTuning gui::waterfall.vfos[{0}] {1}, remote {2}", vfoName, freq, remote);
        // if (!remote)
        // gui::mainWindow.setUpdateMenuSnd0Main(gui::mainWindow.getCurrServer(), true);
        double freq_offset = gui::waterfall.getCenterFrequency() - freq;
        /// flog::info("gui::freqSelect.old_freq {0}, OLD gui::waterfall.getCenterFrequency() {1}", gui::freqSelect.old_freq, gui::waterfall.getCenterFrequency());

        double old_getCenterFrequency = gui::waterfall.getCenterFrequency();

        double viewBW = gui::waterfall.getViewBandwidth();
        double BW = gui::waterfall.getBandwidth();

        /// flog::info("void normalTuning(std::string vfoName, double freq) {0}, freq_offset {1}, viewBW {2}, BW {3}", freq, freq_offset, viewBW, BW);

        ImGui::WaterfallVFO* vfo = gui::waterfall.vfos[vfoName];

        double currentOff = vfo->centerOffset;
        double currentTune = gui::waterfall.getCenterFrequency() + vfo->generalOffset;
        double delta = freq - currentTune;

        double newVFO = currentOff + delta;
        double vfoBW = vfo->bandwidth;
        double vfoBottom = newVFO - (vfoBW / 2.0);
        double vfoTop = newVFO + (vfoBW / 2.0);

        double view = gui::waterfall.getViewOffset();
        double viewBottom = view - (viewBW / 2.0);
        double viewTop = view + (viewBW / 2.0);

        double bottom = -(BW / 2.0);
        double top = (BW / 2.0);

        // VFO still fints in the view
        if (vfoBottom > viewBottom && vfoTop < viewTop) {
            /// flog::info("__1. if vfoBottom {0} > viewBottom {1} && vfoTop {2} < viewTop {3} set setCenterOffset(newVFO={4})", vfoBottom, viewBottom, vfoTop, viewTop, newVFO);
            sigpath::vfoManager.setCenterOffset(vfoName, newVFO);
            setVFOGeneralOffset(old_getCenterFrequency);
            return;
        }

        // VFO too low for current SDR tuning
        if (vfoBottom < bottom) {
            gui::waterfall.setViewOffset((BW / 2.0) - (viewBW / 2.0));
            double newVFOOffset = (BW / 2.0) - (vfoBW / 2.0) - (viewBW / 10.0);
            sigpath::vfoManager.setOffset(vfoName, newVFOOffset);
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);
            // flog::info("__2. if vfoBottom {0} > bottom {1} set  setOffset({2}), setCenterFrequency({3})", vfoBottom, viewBottom, newVFOOffset, viewTop, freq - newVFOOffset);

            setVFOGeneralOffset(old_getCenterFrequency);
            return;
        }

        // VFO too high for current SDR tuning
        if (vfoTop > top) {
            gui::waterfall.setViewOffset((viewBW / 2.0) - (BW / 2.0));
            double newVFOOffset = (vfoBW / 2.0) - (BW / 2.0) + (viewBW / 10.0);
            sigpath::vfoManager.setOffset(vfoName, newVFOOffset);
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);

            // flog::info("__3. if vfoTop {0} > top {1} set  setOffset({2}), setCenterFrequency({3})", vfoTop, top, newVFOOffset, viewTop, freq - newVFOOffset);
            setVFOGeneralOffset(old_getCenterFrequency);
            return;
        }

        // VFO is still without the SDR's bandwidth
        if (delta < 0) {

            double newViewOff = vfoTop - (viewBW / 2.0) + (viewBW / 10.0);
            double newViewBottom = newViewOff - (viewBW / 2.0);

            /// flog::info("__4. delta < 0. newViewOff {0}, newViewBottom {1}, bottom {2}, viewBW {3}", newViewOff, newViewBottom, bottom, viewBW);

            if (newViewBottom > bottom) {
                gui::waterfall.setViewOffset(newViewOff);
                sigpath::vfoManager.setCenterOffset(vfoName, newVFO);
                setVFOGeneralOffset(old_getCenterFrequency);
                return;
            }

            gui::waterfall.setViewOffset((BW / 2.0) - (viewBW / 2.0));
            double newVFOOffset = (BW / 2.0) - (vfoBW / 2.0) - (viewBW / 10.0);
            sigpath::vfoManager.setCenterOffset(vfoName, newVFOOffset);
            // flog::info("__4");
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);
            setVFOGeneralOffset(old_getCenterFrequency);
        }
        else {
            double newViewOff = vfoBottom + (viewBW / 2.0) - (viewBW / 10.0);
            double newViewTop = newViewOff + (viewBW / 2.0);
            // flog::info("__5. delta >= 0. newViewOff {0}, newViewTop {1}, bottom {2}, viewBW {3}", newViewOff, newViewTop, bottom, viewBW);

            if (newViewTop < top) {
                gui::waterfall.setViewOffset(newViewOff);
                sigpath::vfoManager.setCenterOffset(vfoName, newVFO);
                setVFOGeneralOffset(old_getCenterFrequency);
                return;
            }

            gui::waterfall.setViewOffset((viewBW / 2.0) - (BW / 2.0));
            double newVFOOffset = (vfoBW / 2.0) - (BW / 2.0) + (viewBW / 10.0);
            sigpath::vfoManager.setCenterOffset(vfoName, newVFOOffset);
            // flog::info("__5");
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);
            setVFOGeneralOffset(old_getCenterFrequency);
        }
    }

    void iqTuning(double freq) {
        flog::info("draw iqTuning");
        // double old_getCenterFrequency = gui::waterfall.getCenterFrequency();
        // setVFOGeneralOffset(old_getCenterFrequency);

        if (gui::waterfall.vfos.find("Канал приймання") == gui::waterfall.vfos.end()) { return; }
        ImGui::WaterfallVFO* vfo = gui::waterfall.vfos["Канал приймання"];
        vfo->generalOffset = 0;
        // flog::info("__6");
        gui::waterfall.setCenterFrequency(freq);
        gui::waterfall.centerFreqMoved = true;
        sigpath::sourceManager.tune(freq);
    }

    void tune(int mode, std::string vfoName, double freq) {
        // flog::info("tune...");
        switch (mode) {
        case TUNER_MODE_CENTER:
            centerTuning(vfoName, freq);
            break;
        case TUNER_MODE_NORMAL:
            normalTuning(vfoName, freq);
            break;
        case TUNER_MODE_LOWER_HALF:
            normalTuning(vfoName, freq);
            break;
        case TUNER_MODE_UPPER_HALF:
            normalTuning(vfoName, freq);
            break;
        case TUNER_MODE_IQ_ONLY:
            iqTuning(freq);
            break;
        }
    }
}