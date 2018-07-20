/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <algorithm>
#include <set>
extern "C" {
#include <solv/poolarch.h>
}

#include "ModulePackageContainer.hpp"
#include "ModulePackageMaker.hpp"

#include "libdnf/utils/utils.hpp"
#include "libdnf/utils/File.hpp"
#include <functional>
#include <../sack/query.hpp>

ModulePackageContainer::ModulePackageContainer(const std::shared_ptr<Pool> &pool, const char *arch)
        : pool(pool)
{
    if (arch) {
        pool_setarch(pool.get(), arch);
    }
}

ModulePackageContainer::~ModulePackageContainer()
{
    if (activatedModules) {
        map_free(activatedModules);
        delete activatedModules;
    }
}


void ModulePackageContainer::add(const std::shared_ptr<ModulePackage> &package)
{
    modules.insert(std::make_pair(package->getId(), package));
}

void ModulePackageContainer::add(const std::vector<std::shared_ptr<ModulePackage>> &packages)
{
    for (const auto &package : packages) {
        add(package);
    }
}

void ModulePackageContainer::add(const std::map<Id, std::shared_ptr<ModulePackage>> &packages)
{
    modules.insert(std::begin(packages), std::end(packages));
}

std::shared_ptr<ModulePackage> ModulePackageContainer::getModulePackage(Id id)
{
    return modules[id];
}

std::vector<std::shared_ptr<ModulePackage>>
ModulePackageContainer::requiresModuleEnablement(const libdnf::PackageSet & packages)
{
    std::vector<std::shared_ptr<ModulePackage>> output;
    libdnf::Query baseQuery(packages.getSack());
    baseQuery.addFilter(HY_PKG, HY_EQ, &packages);
    baseQuery.apply();
    libdnf::Query testQuery(baseQuery);
    auto moduleMapSize = pool->nsolvables;
    for (Id id = 0; id < moduleMapSize; ++id) {
        if (!MAPTST(activatedModules, id)) {
            continue;
        }
        auto module = modules[id];
        if (module->isEnabled()) {
            continue;
        }
        auto includeNEVRAs = module->getArtifacts();
        std::vector<const char *> includeNEVRAsCString(includeNEVRAs.size() + 1);
        transform(includeNEVRAs.begin(), includeNEVRAs.end(), includeNEVRAsCString.begin(), std::mem_fn(&std::string::c_str));
        testQuery.queryUnion(baseQuery);
        testQuery.addFilter(HY_PKG_NEVRA_STRICT, HY_EQ, includeNEVRAsCString.data());
        if (testQuery.empty()) {
            continue;
        }
        output.push_back(module);
    }
    return output;
}

void ModulePackageContainer::enable(const std::string &name, const std::string &stream)
{
    for (const auto &iter : modules) {
        auto modulePackage = iter.second;
        if (modulePackage->getName() == name && modulePackage->getStream() == stream) {
            modulePackage->enable();
        }
    }
}

void ModulePackageContainer::resolveActiveModulePackages(const std::map<std::string, std::string> &defaultStreams)
{
    std::vector<std::shared_ptr<ModulePackage>> packages;

    // Use only Enabled or Default modules for transaction
    for (const auto &iter : modules) {
        auto module = iter.second;

        bool hasDefaultStream;
        try {
            hasDefaultStream = defaultStreams.at(module->getName()) == module->getStream();
        } catch (std::out_of_range &exception) {
            hasDefaultStream = false;
            // TODO logger.debug(exception.what())
        }
        if (module->isEnabled()) {
            packages.push_back(module);
        } else if (hasDefaultStream) {
            module->setState(ModulePackage::ModuleState::DEFAULT);
            packages.push_back(module);
        }
    }
    auto ids = moduleSolve(packages);
    if (activatedModules) {
        map_free(activatedModules);
    } else {
        activatedModules = new Map;
    }
    map_init(activatedModules, pool->nsolvables);
    for (int i = 0; i < ids->size(); ++i) {
        Id id = (*ids)[i];
        auto solvable = pool_id2solvable(pool.get(), id);
        // TODO use Goal::listInstalls() to not requires filtering out Platform
        if (strcmp(solvable->repo->name, HY_SYSTEM_REPO_NAME) == 0)
            continue;
        MAPSET(activatedModules, id);
    }
}

bool ModulePackageContainer::isModuleActive(Id id)
{
    if (activatedModules) {
        return MAPTST(activatedModules, id);
    }
    return false;
}


std::vector<std::shared_ptr<ModulePackage>> ModulePackageContainer::getModulePackages()
{
    std::vector<std::shared_ptr<ModulePackage>> values;

    std::transform(std::begin(modules), std::end(modules), std::back_inserter(values),
                   [](const std::map<Id, std::shared_ptr<ModulePackage>>::value_type &pair){ return pair.second; });

    return values;
}


