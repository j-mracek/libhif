# Copyright (C) 2020 Red Hat, Inc.
#
# This file is part of libdnf: https://github.com/rpm-software-management/libdnf/
#
# Libdnf is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# Libdnf is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with libdnf.  If not, see <https://www.gnu.org/licenses/>.

require 'test/unit'
include Test::Unit::Assertions

require 'libdnf/base'

class TestRepo < Test::Unit::TestCase
    def test_repo()
        base = Base::Base.new()

        # Sets path to cache directory.
        cwd = Dir.getwd()
        base.get_config().cachedir().set(Conf::Option::Priority_RUNTIME, cwd)

        repo_sack = Rpm::RepoSack.new(base)
        solv_sack = Rpm::SolvSack.new(base)

        # Creates system repository and loads it into rpm::SolvSack.
        solv_sack.create_system_repo(false)

        # Creates new repositories in the repo_sack
        repo = repo_sack.new_repo("dnf-ci-fedora")

        # Tunes repositotory configuration (baseurl is mandatory)
        repo_path = File.join(cwd, '../../../test/libdnf/rpm/repos-data/dnf-ci-fedora/')
        baseurl = 'file://' + repo_path
        repo_cfg = repo.get_config()
        repo_cfg.baseurl().set(Conf::Option::Priority_RUNTIME, baseurl)

        # Loads repository into rpm::Repo.
        repo.load()

        # Loads rpm::Repo into rpm::SolvSack
        solv_sack.load_repo(repo.get(), Rpm::SolvSack::LoadRepoFlags_USE_PRESTO |
                            Rpm::SolvSack::LoadRepoFlags_USE_UPDATEINFO | Rpm::SolvSack::LoadRepoFlags_USE_OTHER)
    end
end
