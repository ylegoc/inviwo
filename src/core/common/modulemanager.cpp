/*********************************************************************************
 *
 * Inviwo - Interactive Visualization Workshop
 *
 * Copyright (c) 2017-2023 Inviwo Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************************/

#include <inviwo/core/common/modulemanager.h>
#include <inviwo/core/common/inviwomodule.h>
#include <inviwo/core/common/version.h>
#include <inviwo/core/common/inviwoapplication.h>
#include <inviwo/core/util/filesystem.h>
#include <inviwo/core/util/settings/systemsettings.h>
#include <inviwo/core/util/sharedlibrary.h>
#include <inviwo/core/util/vectoroperations.h>
#include <inviwo/core/util/utilities.h>
#include <inviwo/core/util/capabilities.h>
#include <inviwo/core/network/processornetwork.h>
#include <inviwo/core/inviwocommondefines.h>

#include <string>
#include <functional>

#include <fmt/std.h>

#if WIN32
#include <windows.h>
#endif

namespace inviwo {

ModuleManager::ModuleManager(InviwoApplication* app)
    : app_{app}
    , protected_{}
    , onModulesDidRegister_{}
    , onModulesWillUnregister_{}
    , libraryObserver_{app}
    , sharedLibraries_{}
    , clearLibs_([&]() {
        util::reverse_erase_if(sharedLibraries_, [this](const auto& module) {
            // Figure out module identifier from file name
            auto moduleName = util::stripModuleFileNameDecoration(module->getFilePath());
            return !this->isProtected(moduleName);
        });
        // Leak the protected dlls here to avoid openglqt crash
        for (auto& lib : sharedLibraries_) lib->release();
    })
    , factoryObjects_{}
    , modules_{}
    , clearModules_([&]() {
        // Need to clear the modules in reverse order since the might depend on each other.
        // The destruction order of vector is undefined.
        util::reverse_erase(modules_);
    }) {}

ModuleManager::~ModuleManager() = default;

bool ModuleManager::isRuntimeModuleReloadingEnabled() {
    return app_->getSystemSettings().runtimeModuleReloading_;
}

void ModuleManager::registerModules(std::vector<std::unique_ptr<InviwoModuleFactoryObject>> mfo) {
    factoryObjects_.insert(factoryObjects_.end(), std::make_move_iterator(mfo.begin()),
                           std::make_move_iterator(mfo.end()));

    // Topological sort to make sure that we load modules in correct order
    topologicalModuleFactoryObjectSort(std::begin(factoryObjects_), std::end(factoryObjects_));

    for (auto& obj : factoryObjects_) {
        app_->postProgress("Loading module: " + obj->name);
        if (getModuleByIdentifier(obj->name)) continue;  // already loaded
        if (!checkDependencies(*obj)) continue;
        try {
            registerModule(obj->create(app_));
        } catch (const ModuleInitException& e) {
            auto dereg = deregisterDependetModules(e.getModulesToDeregister());
            auto err = (!dereg.empty() ? "\nUnregistered dependent modules: " +
                                             joinString(dereg.begin(), dereg.end(), ", ")
                                       : "");
            LogError("Failed to register module: " << obj->name << ". Reason:\n"
                                                   << e.getMessage() << err);
        } catch (const Exception& e) {
            LogError("Failed to register module: " << obj->name << ". Reason:\n" << e.getMessage());
        } catch (const std::exception& e) {
            LogError("Failed to register module: " << obj->name << ". Reason:\n" << e.what());
        }
    }

    app_->postProgress("Loading Capabilities");
    for (auto& module : modules_) {
        for (auto& elem : module->getCapabilities()) {
            elem->retrieveStaticInfo();
            elem->printInfo();
        }
    }

    onModulesDidRegister_.invoke();
}

std::function<bool(std::string_view)> ModuleManager::getEnabledFilter() {
    // Load enabled modules if file "application_name-enabled-modules.txt" exists,
    // otherwise load all modules

    const auto exeName = filesystem::getExecutablePath().stem();
    const auto exepath = filesystem::getExecutablePath().parent_path();

    const auto enabledModuleFileName = exeName.string() + "-enabled-modules.txt";

#ifdef __APPLE__
    // Executable path is inviwo.app/Content/MacOs
    std::filesystem::path enabledModulesFilePath(exepath / "../../.." / enabledModuleFileName);
#else
    std::filesystem::path enabledModulesFilePath(exepath / enabledModuleFileName);
#endif
    if (!std::filesystem::is_regular_file(enabledModulesFilePath)) {
        return [](std::string_view) { return true; };
    }

    std::ifstream enabledModulesFile{enabledModulesFilePath};
    std::vector<std::string> enabledModules;
    std::copy(std::istream_iterator<std::string>(enabledModulesFile),
              std::istream_iterator<std::string>(), std::back_inserter(enabledModules));
    std::for_each(std::begin(enabledModules), std::end(enabledModules), toLower);

    return [enabledModules](std::string_view name) { return util::contains(enabledModules, name); };
}

void ModuleManager::reloadModules() {
    if (isRuntimeModuleReloadingEnabled()) libraryObserver_.reloadModules();
}

void ModuleManager::registerModules(RuntimeModuleLoading,
                                    std::function<bool(std::string_view)> isEnabled) {
    // Perform the following steps
    // 1. Recursively get all library files and the folders they are in
    // 2. Filter out files with correct extension, named inviwo-module
    //    and listed in application_name-enabled-modules.txt (if it exist).
    // 3. Load libraries and see if createModule function exist.
    // 4. Start observing file if reloadLibrariesWhenChanged
    // 5. Pass module factories to registerModules

    // Find unique files and directories in specified search paths
    auto librarySearchPaths = util::getLibrarySearchPaths();
    std::set<std::filesystem::path> libraryFiles;
    for (auto path : librarySearchPaths) {
        // Make sure that we have an absolute path to avoid duplicates
        path = std::filesystem::weakly_canonical(path);
        try {
            auto files =
                filesystem::getDirectoryContentsRecursively(path, filesystem::ListMode::Files);
            libraryFiles.insert(std::make_move_iterator(files.begin()),
                                std::make_move_iterator(files.end()));
        } catch (FileException&) {  // Invalid path, ignore it
        }
    }
    // Determines if a library is already loaded into the application
    auto isModuleLibraryLoaded = [&](const std::filesystem::path& path) {
        return util::contains_if(sharedLibraries_, [&](const auto& lib) {
            return std::filesystem::equivalent(lib->getFilePath(), path);
        });
    };

    auto libraryTypes = SharedLibrary::libraryFileExtensions();
    // Remove unsupported files and files belonging to already loaded modules.
    std::erase_if(libraryFiles, [&](const std::filesystem::path& file) {
        return !libraryTypes.contains(file.extension()) ||
               (file.string().find("inviwo-module") == std::string::npos &&
                file.string().find("inviwo-core") == std::string::npos) ||
               isModuleLibraryLoaded(file) || !isEnabled(util::stripModuleFileNameDecoration(file));
    });

    const auto tmpDir = [&]() -> std::filesystem::path {
        if (isRuntimeModuleReloadingEnabled()) {
            const auto tmp = filesystem::getInviwoUserSettingsPath() / "temporary-module-libraries";
            std::filesystem::create_directories(tmp);
            return tmp;
        } else {
            return {};
        }
    }();

    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> tmpLibraryFiles;
    std::transform(libraryFiles.begin(), libraryFiles.end(), std::back_inserter(tmpLibraryFiles),
                   [&](const std::filesystem::path& filePath)
                       -> std::pair<std::filesystem::path, std::filesystem::path> {
                       if (isRuntimeModuleReloadingEnabled()) {
                           auto dstPath = tmpDir / filePath.stem();
                           if (std::filesystem::last_write_time(filePath) !=
                               std::filesystem::last_write_time(dstPath)) {
                               // Load a copy of the file to make sure that we can overwrite the
                               // file.
                               std::error_code ec;
                               if (!std::filesystem::copy_file(filePath, dstPath, ec)) {
                                   LogWarn("Unable to write temporary file " << dstPath);
                                   return {filePath, filePath};
                               }
                           }
                           return {filePath, dstPath};
                       } else {
                           return {filePath, filePath};
                       }
                   });

    filesystem::setWorkingDirectory(filesystem::getInviwoBinDir());

    // Load libraries from temporary directory but observe the original file
    auto isLoaded = [loaded = filesystem::getLoadedLibraries()](const auto& path) {
        return util::contains_if(
            loaded, [&](const auto& lib) { return std::filesystem::equivalent(path, lib); });
    };
    std::vector<std::unique_ptr<InviwoModuleFactoryObject>> modules;
    for (const auto& [filePath, tmpPath] : tmpLibraryFiles) {
        try {
            const bool loaded = isLoaded(filePath);

            // Load library. Will throw exception if failed to load
            auto sharedLib = std::make_unique<SharedLibrary>(loaded ? filePath : tmpPath);
            // Only consider libraries with Inviwo module creation function
            if (auto moduleFunc = sharedLib->findSymbolTyped<f_getModule>("createModule")) {
                // Add module factory object
                modules.emplace_back(moduleFunc());
                auto moduleName = toLower(modules.back()->name);
                if (modules.back()->protectedModule == ProtectedModule::on || loaded) {
                    protected_.insert(modules.back()->name);
                }
                sharedLibraries_.emplace_back(std::move(sharedLib));
                if (isRuntimeModuleReloadingEnabled()) {
                    libraryObserver_.observe(filePath);
                }
            } else {
                util::logWarn(
                    IVW_CONTEXT,
                    "Could not find 'createModule' function needed for creating the module in {}. "
                    "Make sure that you have compiled the library and exported the function.",
                    filePath);
            }
        } catch (const Exception& e) {
            // Library dependency is probably missing. We silently skip this library.
            util::logWarn(IVW_CONTEXT, "Could not load library: {}", filePath);
            util::log(e.getContext(), e.getMessage(), LogLevel::Warn);
        }
    }

    auto dependencies = getProtectedDependencies(protected_, modules);
    protected_.insert(dependencies.begin(), dependencies.end());

    registerModules(std::move(modules));
}

void ModuleManager::unregisterModules() {
    onModulesWillUnregister_.invoke();
    app_->getProcessorNetwork()->clear();
    // Need to clear the modules in reverse order since the might depend on each other.
    // The destruction order of vector is undefined.
    util::reverse_erase_if(
        modules_, [this](const auto& m) { return !this->isProtected(m->getIdentifier()); });

    // Remove module factories
    util::reverse_erase_if(factoryObjects_,
                           [this](const auto& mfo) { return !this->isProtected(mfo->name); });

    // Modules should now have removed all allocated resources and it should be safe to unload
    // shared libraries.
    util::reverse_erase_if(sharedLibraries_, [this](const auto& module) {
        // Figure out module identifier from file name
        auto moduleName = util::stripModuleFileNameDecoration(module->getFilePath());
        return !this->isProtected(moduleName);
    });
}

void ModuleManager::registerModule(std::unique_ptr<InviwoModule> module) {
    modules_.push_back(std::move(module));
}

const std::vector<std::unique_ptr<InviwoModule>>& ModuleManager::getModules() const {
    return modules_;
}

const std::vector<std::unique_ptr<InviwoModuleFactoryObject>>&
ModuleManager::getModuleFactoryObjects() const {
    return factoryObjects_;
}

InviwoModule* ModuleManager::getModuleByIdentifier(std::string_view identifier) const {
    const auto it =
        std::find_if(modules_.begin(), modules_.end(), [&](const std::unique_ptr<InviwoModule>& m) {
            return iCaseCmp(m->getIdentifier(), identifier);
        });
    if (it != modules_.end()) {
        return it->get();
    } else {
        return nullptr;
    }
}

std::vector<InviwoModule*> ModuleManager::getModulesByAlias(std::string_view alias) const {
    std::vector<InviwoModule*> res;
    for (const auto& mfo : factoryObjects_) {
        if (util::contains(mfo->aliases, alias)) {
            if (auto m = getModuleByIdentifier(mfo->name)) {
                res.push_back(m);
            }
        }
    }
    return res;
}

InviwoModuleFactoryObject* ModuleManager::getFactoryObject(std::string_view identifier) const {
    auto it = util::find_if(factoryObjects_,
                            [&](const auto& module) { return iCaseCmp(module->name, identifier); });
    // Check if dependent module is of correct version
    if (it != factoryObjects_.end()) {
        return it->get();
    } else {
        return nullptr;
    }
}

std::vector<std::string> ModuleManager::findDependentModules(std::string_view module) const {
    std::vector<std::string> dependencies;
    for (const auto& item : factoryObjects_) {
        if (util::contains_if(item->dependencies, [&](auto& dep) { return dep.first == module; })) {
            auto name = toLower(item->name);
            auto deps = findDependentModules(name);
            util::append(dependencies, deps);
            dependencies.push_back(name);
        }
    }
    std::vector<std::string> unique;
    for (const auto& item : dependencies) {
        util::push_back_unique(unique, item);
    }
    return unique;
}

std::shared_ptr<std::function<void()>> ModuleManager::onModulesDidRegister(
    std::function<void()> callback) {
    return onModulesDidRegister_.add(callback);
}

std::shared_ptr<std::function<void()>> ModuleManager::onModulesWillUnregister(
    std::function<void()> callback) {
    return onModulesWillUnregister_.add(callback);
}

const std::set<std::string, CaseInsensitiveCompare>& ModuleManager::getProtectedModuleIdentifiers()
    const {
    return protected_;
}

bool ModuleManager::isProtected(std::string_view module) const {
    return protected_.count(module) != 0;
}

void ModuleManager::addProtectedIdentifier(std::string_view id) { protected_.emplace(id); }

bool ModuleManager::checkDependencies(const InviwoModuleFactoryObject& obj) const {
    std::stringstream err;

    // Make sure that the module supports the current inviwo core version
    if (!build::version.semanticVersionEqual(obj.inviwoCoreVersion)) {
        err << "\nModule was built for Inviwo version " << obj.inviwoCoreVersion
            << ", current version is " << build::version;
    }

    // Check if dependency modules have correct versions.
    // Note that the module version only need to be increased
    // when changing and the inviwo core version has not changed
    // since we are ensuring the they must be built for the
    // same core version.
    for (const auto& dep : obj.dependencies) {
        const auto& name = dep.first;
        const auto& version = dep.second;

        if (auto depObj = getFactoryObject(name)) {
            if (!getModuleByIdentifier(depObj->name)) {
                err << "\nModule dependency: " + depObj->name + " failed to register";
            } else if (!depObj->version.semanticVersionEqual(obj.version)) {
                err << "\nModule depends on " << depObj->name << " version " << version
                    << " but version " << depObj->version << " was loaded";
            }
        } else {
            err << "\nModule depends on " << name << " version " << version
                << " but no such module was found";
        }
    }
    if (err.str().size() > 0) {
        LogError("Failed to register module: " << obj.name << ". Reason: " << err.str());
        return false;
    } else {
        return true;
    }
}

std::vector<std::string> ModuleManager::deregisterDependetModules(
    const std::vector<std::string>& toDeregister) {
    IdSet deregister;
    for (const auto& m : toDeregister) {
        deregister.insert(m);
        auto dependents = findDependentModules(m);
        deregister.insert(dependents.begin(), dependents.end());
    }
    std::vector<std::string> deregistered;
    util::reverse_erase_if(modules_, [&](const auto& m) {
        if (deregister.count(m->getIdentifier()) != 0) {
            deregistered.push_back(m->getIdentifier());
            return true;
        } else {
            return false;
        }
    });
    return deregistered;
}

auto ModuleManager::getProtectedDependencies(
    const IdSet& ptotectedIds,
    const std::vector<std::unique_ptr<InviwoModuleFactoryObject>>& modules) -> IdSet {
    IdSet dependencies;
    std::function<void(std::string_view)> getDeps = [&](std::string_view module) {
        auto it = util::find_if(modules, [&](const auto& m) { return iCaseCmp(m->name, module); });
        if (it != modules.end()) {
            for (const auto& dep : (*it)->dependencies) {
                dependencies.insert(dep.first);
                getDeps(dep.first);
            }
        }
    };
    for (auto& module : ptotectedIds) {
        getDeps(module);
    }
    return dependencies;
}

}  // namespace inviwo
