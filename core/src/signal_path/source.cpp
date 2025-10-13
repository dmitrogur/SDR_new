#include <server.h>
#include <aster_server.h>
#include <signal_path/source.h>
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/main_window.h>

SourceManager::SourceManager() {
}

void SourceManager::registerSource(std::string name, SourceHandler* handler) {
    if (sources.find(name) != sources.end()) {
        flog::error("Tried to register new source with existing name: {0}", name);
        return;
    }
    sources[name] = handler;
    onSourceRegistered.emit(name);
    isServer = gui::mainWindow.getIsServer();
}

void SourceManager::unregisterSource(std::string name) {
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to unregister non existent source: {0}", name);
        return;
    }
    onSourceUnregister.emit(name);
    if (name == selectedName) {
        if (selectedHandler != NULL) {
            sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
        }
        sigpath::iqFrontEnd.setInput(&nullSource);
        sigpath::remoteRadio.setInputStream(&nullSource);
        selectedHandler = NULL;
    }
    sources.erase(name);
    onSourceUnregistered.emit(name);
}

std::vector<std::string> SourceManager::getSourceNames() {
    std::vector<std::string> names;
    for (auto const& [name, src] : sources) { names.push_back(name); }
    return names;
}

void SourceManager::selectSource(std::string name) {
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to select non existent source: {0}", name);
        return;
    }
    if (selectedHandler != NULL) {
        sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
    }
    selectedHandler = sources[name];
    selectedHandler->selectHandler(selectedHandler->ctx);
    selectedName = name;
    if (core::args["server"].b()) {
        server::setInput(selectedHandler->stream);
    }
    else {
        flog::info("selectSource setInputStream");
        sigpath::iqFrontEnd.setInput(selectedHandler->stream);
        if (isServer) {
            sigpath::remoteRadio.setInputStream(selectedHandler->stream);
        }
    }
    // Set server input here
}

void SourceManager::showSelectedMenu() {
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->menuHandler(selectedHandler->ctx);
}

void SourceManager::start() {
    if (selectedHandler == NULL) {
        return;
    }
flog::info("selectSource setInputStream start(), isServer {0}", isServer);
    // usleep(1000);
    // selectedHandler->selectHandler(selectedHandler->ctx);
    if (core::args["server"].b()) {
    }
    else {
        sigpath::iqFrontEnd.setInput(selectedHandler->stream);
        if (isServer) {
            flog::info("selectedName {0}", selectedName);
            if (selectedHandler != NULL) {
                // SourceHandler* tmp_handler = sources[selectedName];
                // sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
                //std::string tmp_selectedName = selectedName;
                // unregisterSource(tmp_selectedName);
                // flog::info("tmp_selectedName {0}", tmp_selectedName);
                // registerSource(tmp_selectedName, tmp_handler);
                // selectSource(selectedName);
                //selectedHandler->selectHandler(selectedHandler->ctx);
            }
            // sigpath::remoteRadio.setInputStream(&nullSource);

            sigpath::remoteRadio.setInputStream(selectedHandler->stream);
        }
    }
    selectedHandler->startHandler(selectedHandler->ctx);
}

void SourceManager::stop() {
    if (selectedHandler == NULL) {
        return;
    }
    sigpath::iqFrontEnd.setInput(&nullSource);
    sigpath::remoteRadio.setInputStream(&nullSource);

    selectedHandler->stopHandler(selectedHandler->ctx);
}

void SourceManager::tune(double freq) {
    if (selectedHandler == NULL) {
        return;
    }
    // TODO: No need to always retune the hardware in panadpter mode
    // selectedHandler->tuneHandler(((tuneMode == TuningMode::NORMAL) ? freq : ifFreq) + tuneOffset, selectedHandler->ctx);
    selectedHandler->tuneHandler(abs(((tuneMode == TuningMode::NORMAL) ? freq : ifFreq) + tuneOffset), selectedHandler->ctx);
    onRetune.emit(freq);
    currentFreq = freq;
}

void SourceManager::setTuningOffset(double offset) {
    tuneOffset = offset;
    tune(currentFreq);
}

void SourceManager::setTuningMode(TuningMode mode) {
    tuneMode = mode;
    tune(currentFreq);
}

void SourceManager::setPanadapterIF(double freq) {
    ifFreq = freq;
    tune(currentFreq);
}