#include <module.h>
#include <filesystem>
#include <utils/flog.h>

ModuleManager::Module_t ModuleManager::loadModule(std::string path) {
    Module_t mod;

    // On android, the path has to be relative, don't make it absolute
#ifndef __ANDROID__
    if (!std::filesystem::exists(path)) {
        flog::error("{0} does not exist", path);
        mod.handle = NULL;
        return mod;
    }
    if (!std::filesystem::is_regular_file(path)) {
        flog::error("{0} isn't a loadable module", path);
        mod.handle = NULL;
        return mod;
    }
#endif
#ifdef _WIN32
    mod.handle = LoadLibraryA(path.c_str());
    if (mod.handle == NULL) {
        flog::error("Couldn't load {0}. Error code: {1}", path, (int)GetLastError());
        mod.handle = NULL;
        return mod;
    }
    mod.info = (ModuleInfo_t*)GetProcAddress(mod.handle, "_INFO_");
    mod.init = (void (*)())GetProcAddress(mod.handle, "_INIT_");
    mod.createInstance = (Instance * (*)(std::string)) GetProcAddress(mod.handle, "_CREATE_INSTANCE_");
    mod.deleteInstance = (void (*)(Instance*))GetProcAddress(mod.handle, "_DELETE_INSTANCE_");
    mod.end = (void (*)())GetProcAddress(mod.handle, "_END_");
#else
    mod.handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (mod.handle == NULL) {
        flog::error("Couldn't load {0}: {1}", path, dlerror());
        mod.handle = NULL;
        return mod;
    }
    mod.info = (ModuleInfo_t*)dlsym(mod.handle, "_INFO_");
    mod.init = (void (*)())dlsym(mod.handle, "_INIT_");
    mod.createInstance = (Instance * (*)(std::string)) dlsym(mod.handle, "_CREATE_INSTANCE_");
    mod.deleteInstance = (void (*)(Instance*))dlsym(mod.handle, "_DELETE_INSTANCE_");
    mod.end = (void (*)())dlsym(mod.handle, "_END_");
#endif
    if (mod.info == NULL) {
        flog::error("{0} is missing _INFO_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.init == NULL) {
        flog::error("{0} is missing _INIT_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.createInstance == NULL) {
        flog::error("{0} is missing _CREATE_INSTANCE_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.deleteInstance == NULL) {
        flog::error("{0} is missing _DELETE_INSTANCE_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.end == NULL) {
        flog::error("{0} is missing _END_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (modules.find(mod.info->name) != modules.end()) {
        flog::error("{0} has the same name as an already loaded module", path);
        mod.handle = NULL;
        return mod;
    }
    for (auto const& [name, _mod] : modules) {
        if (mod.handle == _mod.handle) {
            return _mod;
        }
    }
    mod.init();
    modules[mod.info->name] = mod;
    return mod;
}

int ModuleManager::createInstance(std::string name, std::string module) {
    if (modules.find(module) == modules.end()) {
        flog::error("Module '{0}' doesn't exist", module);
        return -1;
    }
    if (instances.find(name) != instances.end()) {
        flog::error("A module instance with the name '{0}' already exists", name);
        return -1;
    }
    int maxCount = modules[module].info->maxInstances;
    if (countModuleInstances(module) >= maxCount && maxCount > 0) {
        flog::error("Maximum number of instances reached for '{0}'", module);
        return -1;
    }
    Instance_t inst;
    inst.module = modules[module];
    inst.instance = inst.module.createInstance(name);
    instances[name] = inst;
    onInstanceCreated.emit(name);
    return 0;
}

/*
int ModuleManager::deleteInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Tried to remove non-existent instance '{0}'", name);
        return -1;
    }
        flog::error("onInstanceDelete '{0}'", name);
    onInstanceDelete.emit(name);
    Instance_t inst = instances[name];
        flog::error("inst.module.deleteInstance '{0}'", name);
    inst.module.deleteInstance(inst.instance);

        flog::error("instances.erase '{0}'", name);
    instances.erase(name);

        flog::error("onInstanceDeleted '{0}'", name);
    onInstanceDeleted.emit(name);

    return 0;
}
*/
// core/src/module.cpp

int ModuleManager::deleteInstance(std::string name)
{
    flog::info("Deleting instance '{0}'...", name);

    // Используем мьютекс для защиты map, если этот метод могут вызывать из разных потоков
    std::lock_guard<std::recursive_mutex> lck(instanceMtx); 

    // 1. Находим итератор на элемент
    auto it = instances.find(name);
    if (it == instances.end()) {
        flog::error("Tried to remove non-existent instance '{0}'", name);
        return -1;
    }

    // 2. Получаем ссылку на структуру Instance_t, которая хранится в map. БЕЗ КОПИРОВАНИЯ.
    Instance_t& inst_to_delete = it->second;

    // 3. Вызываем событие "перед удалением"
    onInstanceDelete.emit(name);
    
    // 4. Вызываем delete для указателя на реальный объект (RecorderModule, RadioModule и т.д.)
    //    Это вызовет деструктор, который должен остановить и отсоединить потоки.
    delete inst_to_delete.instance;

    // 5. Удаляем элемент из map по итератору. Это эффективнее, чем по ключу.
    instances.erase(it);
    
    // 6. Вызываем событие "после удаления"
    onInstanceDeleted.emit(name);

    flog::info("Instance '{0}' deleted successfully.", name);

    return 0;
}

int ModuleManager::deleteInstance(ModuleManager::Instance* instance) {
    flog::error("Delete instance not implemented");
    return -1;
}

int ModuleManager::enableInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot enable '{0}', instance doesn't exist", name);
        return -1;
    }
    instances[name].instance->enable();
    return 0;
}

int ModuleManager::disableInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot disable '{0}', instance doesn't exist", name);
        return -1;
    }
    instances[name].instance->disable();
    return 0;
}

bool ModuleManager::instanceExist(std::string name) {
    if (instances.find(name) == instances.end()) {
        return false;
    }
    // flog::error("instanceExist {0}", name); 
    return true;
}

bool ModuleManager::instanceEnabled(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot check if '{0}' is enabled, instance doesn't exist", name);
        return false;
    }
    return instances[name].instance->isEnabled();
}

void ModuleManager::postInit(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot post-init '{0}', instance doesn't exist", name);
        return;
    }
    instances[name].instance->postInit();
}

std::string ModuleManager::getInstanceModuleName(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot get module name of'{0}', instance doesn't exist", name);
        return "";
    }
    return std::string(instances[name].module.info->name);
}

int ModuleManager::countModuleInstances(std::string module) {
    if (modules.find(module) == modules.end()) {
        flog::error("Cannot count instances of '{0}', Module doesn't exist", module);
        return -1;
    }
    ModuleManager::Module_t mod = modules[module];
    int count = 0;
    for (auto const& [name, instance] : instances) {
        if (instance.module == mod) { count++; }
    }
    return count;
}

void ModuleManager::doPostInitAll() {
    for (auto& [name, inst] : instances) {
        flog::info("Running post-init for {0}", name);
        inst.instance->postInit();
    }
}