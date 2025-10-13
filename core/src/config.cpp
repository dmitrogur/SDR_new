#include <config.h>
#include <utils/flog.h>
#include <fstream>

#include <filesystem>

ConfigManager::ConfigManager() {
}

ConfigManager::~ConfigManager() {
    disableAutoSave();
    if (std::filesystem::path(path).filename() == "config.json") {
        if (!std::filesystem::remove(pathCpy)) {
            flog::warn("std::filesystem::remove({0})", pathCpy);
        }
        const auto copyOptions = std::filesystem::copy_options::update_existing | std::filesystem::copy_options::recursive;
        std::error_code ec;
        std::filesystem::copy(path, pathCpy, copyOptions, ec);
        if (ec) {
            flog::warn("Error copy config file copy({0}, {1})", path, pathCpy);
        }
        else {
            flog::info("copy to {0}", pathCpy);
        }
    }
}

void ConfigManager::setPath(std::string file) {
    path = std::filesystem::absolute(file).string();
}

void ConfigManager::setPathCpy(std::string file) {
    pathCpy = std::filesystem::absolute(file).string();
}

std::string ConfigManager::getPath() {
    std::filesystem::path p = path;
    return p.parent_path();
}

void ConfigManager::load(json def, bool lock) {
    // Используем lock_guard для автоматического и безопасного управления мьютексом.
    // Он автоматически вызовет unlock() при выходе из функции, даже в случае ошибки.
    std::unique_lock<std::mutex> lck(mtx, std::defer_lock);
    if (lock) {
        lck.lock();
    }

    if (path.empty()) {
        flog::error("ConfigManager tried to load file with no path specified.");
        return;
    }

    // 1. Устанавливаем в память рабочую конфигурацию по умолчанию.
    // Это гарантирует, что у программы всегда есть корректный конфиг для работы.
    conf = def;

    // 2. Пытаемся прочитать файл с диска.
    if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
        try {
            std::ifstream file(path);
            json file_conf;
            file >> file_conf; // Эта строка может бросить исключение, если JSON "битый"
            file.close();

            // 3. Если чтение прошло успешно, СЛИВАЕМ конфиг из файла с дефолтным.
            // Ключи из file_conf перезапишут дефолтные.
            // Новые ключи из def, которых нет в file_conf, останутся.
            conf.merge_patch(file_conf);
            
            // (Опционально) Создаем резервную копию ПОСЛЕ успешного чтения.
            // Это гарантирует, что pathCpy всегда будет рабочей версией.
            std::string pathCpy = path + ".bak";
            std::ofstream backup_file(pathCpy, std::ios::trunc);
            backup_file << conf.dump(4);
            backup_file.close();

        } catch (const std::exception& e) {
            // 4. Блок САМОИСЦЕЛЕНИЯ. Срабатывает, если файл на диске поврежден.
            flog::error("!!! CONFIG FILE CORRUPTED: '{0}' !!!", path);
            flog::error("    REASON: {0}", e.what());
            
            // Попытка восстановления из резервной копии
            std::string pathCpy = path + ".bak";
            if (std::filesystem::exists(pathCpy)) {
                 flog::warn("    Attempting to restore from backup: '{0}'", pathCpy);
                 try {
                     std::ifstream backup_file(pathCpy);
                     json backup_conf;
                     backup_file >> backup_conf;
                     backup_file.close();
                     
                     conf = def; // Снова берем дефолт
                     conf.merge_patch(backup_conf); // Накладываем бэкап
                     
                     flog::info("    SUCCESSFULLY RESTORED from backup file.");
                     save(false); // Сразу сохраняем восстановленный конфиг, чтобы исправить основной файл
                     return; // Выходим, так как все успешно восстановлено
                 } catch (const std::exception& be) {
                     flog::error("    BACKUP FILE IS ALSO CORRUPTED: {0}", be.what());
                 }
            }

            // Если восстановление из бэкапа не удалось или бэкапа нет:
            flog::warn("    RESETTING TO DEFAULT CONFIGURATION and overwriting the corrupted file.");
            // В 'conf' уже лежат значения по умолчанию из шага 1.
            // Просто сохраняем их поверх "битого" файла.
            save(false);
        }
    } else {
        // 5. Если файла конфигурации на диске нет, создаем его из дефолтных значений.
        flog::info("Config file '{0}' not found. Creating default config.", path);
        save(false);
    }
}
/*
void ConfigManager::load_old(json def, bool lock) {
    if (lock) { mtx.lock(); }
    if (path == "") {
        flog::error("Config manager tried to load file with no path specified");
        return;
    }

    if (!std::filesystem::exists(path)) {
        flog::warn("Config file '{0}' does not exist, creating it", path);
        conf = def;
        save(false);
    }
    if (!std::filesystem::is_regular_file(path)) {
        flog::error("Config file '{0}' isn't a file", path);
        return;
    }

    bool corrupted = false;
    try {
        std::ifstream file(path.c_str());
        file >> conf;
        file.close();
    }
    catch (std::exception e) {
        // flog::error("Config file '{0}' is corrupted, resetting it", path);
        flog::error("Config file '{0}' is corrupted, resetting it: {1}", path, e.what());
        corrupted = true;
    }
    if (corrupted) {
        if (std::filesystem::path(path).filename() == "config.json") {
            flog::info("correcting...");
            bool correct = true;
            if (!std::filesystem::exists(pathCpy)) {
                flog::warn("Config file '{0}' does not exist", pathCpy);
                correct = false;
            }

            if (correct && !std::filesystem::is_regular_file(pathCpy)) {
                flog::warn("Config file '{0}' isn't a file", pathCpy);
                correct = false;
            }
            if (correct) {
                try {
                    std::ifstream file(pathCpy.c_str());
                    file >> conf;
                    file.close();
                    corrupted = false;
                    flog::info("Config file was corrected from {0}!", pathCpy);
                }
                catch (std::exception e) {
                    flog::error("Config file '{}' is corrupted, resetting it: {}", pathCpy, e.what());
                    corrupted = true;
                }
            }
        }
    }
    if (corrupted) {
        conf = def;
        save(false);
    }
    if (lock) { mtx.unlock(); }
}
*/
void ConfigManager::save(bool lock) {
    if (lock) { mtx.lock(); }
    std::ofstream file(path.c_str());
    file << conf.dump(4);
    file.close();
    if (lock) { mtx.unlock(); }
}

void ConfigManager::enableAutoSave() {
    if (autoSaveEnabled) { return; }
    autoSaveEnabled = true;
    termFlag = false;
    autoSaveThread = std::thread(&ConfigManager::autoSaveWorker, this);
}

void ConfigManager::disableAutoSave() {
    if (!autoSaveEnabled) { return; }
    {
        std::lock_guard<std::mutex> lock(termMtx);
        autoSaveEnabled = false;
        termFlag = true;
    }
    termCond.notify_one();
    if (autoSaveThread.joinable()) { autoSaveThread.join(); }
}

void ConfigManager::acquire() {
    mtx.lock();
}

void ConfigManager::release(bool modified) {
    changed |= modified;
    mtx.unlock();
}

void ConfigManager::autoSaveWorker() {
    while (autoSaveEnabled) {
        if (!mtx.try_lock()) {
            flog::warn("ConfigManager locked, waiting...");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        if (changed) {
            changed = false;
            save(false);
        }
        mtx.unlock();

        // Sleep but listen for wakeup call
        {
            std::unique_lock<std::mutex> lock(termMtx);
            termCond.wait_for(lock, std::chrono::milliseconds(1000), [this]() { return termFlag; });
        }
    }
}