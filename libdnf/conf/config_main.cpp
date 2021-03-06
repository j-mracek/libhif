/*
Copyright (C) 2018-2020 Red Hat, Inc.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "libdnf/conf/config_main.hpp"

#include "config_utils.hpp"

#include "libdnf/conf/config_parser.hpp"
#include "libdnf/conf/const.hpp"

#include <dirent.h>
#include <fmt/format.h>
#include <glob.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <utility>

#define ASCII_LOWERCASE "abcdefghijklmnopqrstuvwxyz"
#define ASCII_UPPERCASE "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define ASCII_LETTERS   ASCII_LOWERCASE ASCII_UPPERCASE
#define DIGITS          "0123456789"
#define REPOID_CHARS    ASCII_LETTERS DIGITS "-_.:"

extern char ** environ;

namespace libdnf {

/// @brief Converts a friendly bandwidth option to bytes
///
/// Function converts a friendly bandwidth option to bytes.  The input
/// should be a string containing a (possibly floating point)
/// number followed by an optional single character unit. Valid
/// units are 'k', 'M', 'G'. Case is ignored. The convention that
/// 1k = 1024 bytes is used.
///
/// @param str Bandwidth as user friendly string
/// @return int Number of bytes
static int str_to_bytes(const std::string & str) {
    if (str.empty()) {
        throw Option::InvalidValue("no value specified");
    }

    std::size_t idx;
    auto res = std::stod(str, &idx);
    if (res < 0) {
        throw Option::InvalidValue(fmt::format("seconds value '{}' must not be negative", str));
    }

    if (idx < str.length()) {
        if (idx < str.length() - 1) {
            throw Option::InvalidValue(fmt::format("could not convert '{}' to bytes", str));
        }
        switch (str.back()) {
            case 'k':
            case 'K':
                res *= 1024;
                break;
            case 'm':
            case 'M':
                res *= 1024 * 1024;
                break;
            case 'g':
            case 'G':
                res *= 1024 * 1024 * 1024;
                break;
            default:
                throw Option::InvalidValue(fmt::format("unknown unit '{}'", str.back()));
        }
    }

    return static_cast<int>(res);
}

static void add_from_file(std::ostream & out, const std::string & file_path) {
    std::ifstream ifs(file_path);
    if (!ifs) {
        throw std::runtime_error("add_from_file(): Can't open file");
    }
    ifs.exceptions(std::ifstream::badbit);

    std::string line;
    while (!ifs.eof()) {
        std::getline(ifs, line);
        auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) {
            continue;
        }
        if (line[start] == '#') {
            continue;
        }
        auto end = line.find_last_not_of(" \t\r");

        out.write(line.c_str() + start, static_cast<int>(end - start + 1));
        out.put(' ');
    }
}

static void add_from_files(std::ostream & out, const std::string & glob_path) {
    glob_t glob_buf;
    glob(glob_path.c_str(), GLOB_MARK | GLOB_NOSORT, nullptr, &glob_buf);
    for (size_t i = 0; i < glob_buf.gl_pathc; ++i) {
        auto path = glob_buf.gl_pathv[i];
        if (path[strlen(path) - 1] != '/') {
            add_from_file(out, path);
        }
    }
    globfree(&glob_buf);
}

/// @brief Replaces globs (like /etc/foo.d/\\*.foo) by content of matching files.
///
/// Ignores comment lines (start with '#') and blank lines in files.
/// Result:
/// Words delimited by spaces. Characters ',' and '\n' are replaced by spaces.
/// Extra spaces are removed.
/// @param strWithGlobs Input string with globs
/// @return Words delimited by space
static std::string resolve_globs(const std::string & str_with_globs) {
    std::ostringstream res;
    std::string::size_type start{0};
    while (start < str_with_globs.length()) {
        auto end = str_with_globs.find_first_of(" ,\n", start);
        if (str_with_globs.compare(start, 5, "glob:") == 0) {
            start += 5;
            if (start >= str_with_globs.length()) {
                break;
            }
            if (end == std::string::npos) {
                add_from_files(res, str_with_globs.substr(start));
                break;
            }
            if ((end - start) != 0) {
                add_from_files(res, str_with_globs.substr(start, end - start));
            }
        } else {
            if (end == std::string::npos) {
                res << str_with_globs.substr(start);
                break;
            }
            if ((end - start) != 0) {
                res << str_with_globs.substr(start, end - start) << " ";
            }
        }
        start = end + 1;
    }
    return res.str();
}

class ConfigMain::Impl {
    friend class ConfigMain;

    explicit Impl(Config & owner);

    Config & owner;

    OptionNumber<std::int32_t> debuglevel{2, 0, 10};
    OptionNumber<std::int32_t> errorlevel{3, 0, 10};
    OptionPath installroot{"/", false, true};
    OptionPath config_file_path{CONF_FILENAME};
    OptionBool plugins{true};
    OptionStringList pluginpath{std::vector<std::string>{}};
    OptionStringList pluginconfpath{std::vector<std::string>{}};
    OptionPath persistdir{PERSISTDIR};
    OptionBool transformdb{true};
    OptionNumber<std::int32_t> recent{7, 0};
    OptionBool reset_nice{true};
    OptionPath system_cachedir{SYSTEM_CACHEDIR};
    OptionBool cacheonly{false};
    OptionBool keepcache{false};
    OptionString logdir{"/var/log"};
    OptionNumber<std::int32_t> log_size{1024 * 1024, str_to_bytes};
    OptionNumber<std::int32_t> log_rotate{4, 0};
    OptionStringList varsdir{VARS_DIRS};
    OptionStringList reposdir{{"/etc/yum.repos.d", "/etc/yum/repos.d", "/etc/distro.repos.d"}};
    OptionBool debug_solver{false};
    OptionStringList installonlypkgs{INSTALLONLYPKGS};
    OptionStringList group_package_types{GROUP_PACKAGE_TYPES};

    OptionNumber<std::uint32_t> installonly_limit{3, 0, [](const std::string & value) -> std::uint32_t {
                                                      if (value == "<off>") {
                                                          return 0;
                                                      }
                                                      try {
                                                          return static_cast<std::uint32_t>(std::stoul(value));
                                                      } catch (...) {
                                                          return 0;
                                                      }
                                                  }};

    OptionStringList tsflags{std::vector<std::string>{}};
    OptionBool assumeyes{false};
    OptionBool assumeno{false};
    OptionBool check_config_file_age{true};
    OptionBool defaultyes{false};
    OptionBool diskspacecheck{true};
    OptionBool localpkg_gpgcheck{false};
    OptionBool gpgkey_dns_verification{false};
    OptionBool obsoletes{true};
    OptionBool showdupesfromrepos{false};
    OptionBool exit_on_lock{false};
    OptionBool allow_vendor_change{true};
    OptionSeconds metadata_timer_sync{60 * 60 * 3};  // 3 hours
    OptionStringList disable_excludes{std::vector<std::string>{}};
    OptionEnum<std::string> multilib_policy{"best", {"best", "all"}};  // :api
    OptionBool best{false};                                            // :api
    OptionBool install_weak_deps{true};
    OptionString bugtracker_url{BUGTRACKER};
    OptionBool zchunk{true};

    OptionEnum<std::string> color{"auto", {"auto", "never", "always"}, [](const std::string & value) {
                                      const std::array<const char *, 4> always{{"on", "yes", "1", "true"}};
                                      const std::array<const char *, 4> never{{"off", "no", "0", "false"}};
                                      const std::array<const char *, 2> aut{{"tty", "if-tty"}};
                                      std::string tmp;
                                      if (std::find(always.begin(), always.end(), value) != always.end()) {
                                          tmp = "always";
                                      } else if (std::find(never.begin(), never.end(), value) != never.end()) {
                                          tmp = "never";
                                      } else if (std::find(aut.begin(), aut.end(), value) != aut.end()) {
                                          tmp = "auto";
                                      } else {
                                          tmp = value;
                                      }
                                      return tmp;
                                  }};

    OptionString color_list_installed_older{"yellow"};
    OptionString color_list_installed_newer{"bold,yellow"};
    OptionString color_list_installed_reinstall{"dim,cyan"};
    OptionString color_list_installed_extra{"bold,red"};
    OptionString color_list_available_upgrade{"bold,blue"};
    OptionString color_list_available_downgrade{"dim,magenta"};
    OptionString color_list_available_reinstall{"bold,underline,green"};
    OptionString color_list_available_install{"bold,cyan"};
    OptionString color_update_installed{"dim,red"};
    OptionString color_update_local{"dim,green"};
    OptionString color_update_remote{"bold,green"};
    OptionString color_search_match{"bold,magenta"};
    OptionBool history_record{true};
    OptionStringList history_record_packages{std::vector<std::string>{"dnf", "rpm"}};
    OptionString rpmverbosity{"info"};
    OptionBool strict{true};                    // :api
    OptionBool skip_broken{false};              // :yum-compatibility
    OptionBool autocheck_running_kernel{true};  // :yum-compatibility
    OptionBool clean_requirements_on_remove{true};

    OptionEnum<std::string> history_list_view{
        "commands", {"single-user-commands", "users", "commands"}, [](const std::string & value) {
            if (value == "cmds" || value == "default") {
                return std::string("commands");
            }
            return value;
        }};

    OptionBool upgrade_group_objects_upgrade{true};  // :api
    OptionPath destdir{nullptr};
    OptionString comment{nullptr};
    OptionBool downloadonly{false};  // runtime only option
    OptionBool ignorearch{false};
    OptionString module_platform_id{nullptr};

    OptionString user_agent{"libdnf"};  // TODO(jrohel): getUserAgent()
    OptionBool countme{false};

    // Repo main config

    OptionNumber<std::uint32_t> retries{10};
    OptionString cachedir{nullptr};
    OptionBool fastestmirror{false};
    OptionStringList excludepkgs{std::vector<std::string>{}};
    OptionStringList includepkgs{std::vector<std::string>{}};
    OptionString proxy{""};
    OptionString proxy_username{nullptr};
    OptionString proxy_password{nullptr};

    OptionEnum<std::string> proxy_auth_method{
        "any",
        {"any", "none", "basic", "digest", "negotiate", "ntlm", "digest_ie", "ntlm_wb"},
        [](const std::string & value) {
            auto tmp = value;
            std::transform(tmp.begin(), tmp.end(), tmp.begin(), ::tolower);
            return tmp;
        }};

    OptionStringList protected_packages{
        resolve_globs("dnf glob:/etc/yum/protected.d/*.conf "
                      "glob:/etc/dnf/protected.d/*.conf")};
    OptionString username{""};
    OptionString password{""};
    OptionBool gpgcheck{false};
    OptionBool repo_gpgcheck{false};
    OptionBool enabled{true};
    OptionBool enablegroups{true};
    OptionNumber<std::uint32_t> bandwidth{0, str_to_bytes};
    OptionNumber<std::uint32_t> minrate{1000, str_to_bytes};

    OptionEnum<std::string> ip_resolve{"whatever", {"ipv4", "ipv6", "whatever"}, [](const std::string & value) {
                                           auto tmp = value;
                                           if (value == "4") {
                                               tmp = "ipv4";
                                           } else if (value == "6") {
                                               tmp = "ipv6";
                                           } else {
                                               std::transform(tmp.begin(), tmp.end(), tmp.begin(), ::tolower);
                                           }
                                           return tmp;
                                       }};

    OptionNumber<float> throttle{
        0, 0, [](const std::string & value) {
            if (!value.empty() && value.back() == '%') {
                std::size_t idx;
                auto res = std::stof(value, &idx);
                if (res < 0 || res > 100) {
                    // TODO(jrohel): Better exception info?
                    // throw Option::InvalidValue(tfm::format(_("percentage '%s' is out of range"), value));
                    throw Option::InvalidValue(value);
                }
                return res / 100;
            }
            return static_cast<float>(str_to_bytes(value));
        }};

    OptionSeconds timeout{30};
    OptionNumber<std::uint32_t> max_parallel_downloads{3, 1};
    OptionSeconds metadata_expire{60 * 60 * 48};
    OptionString sslcacert{""};
    OptionBool sslverify{true};
    OptionString sslclientcert{""};
    OptionString sslclientkey{""};
    OptionBool deltarpm{true};
    OptionNumber<std::uint32_t> deltarpm_percentage{75};
    OptionBool skip_if_unavailable{false};
};

ConfigMain::Impl::Impl(Config & owner) : owner(owner) {
    owner.opt_binds().add("debuglevel", debuglevel);
    owner.opt_binds().add("errorlevel", errorlevel);
    owner.opt_binds().add("installroot", installroot);
    owner.opt_binds().add("config_file_path", config_file_path);
    owner.opt_binds().add("plugins", plugins);
    owner.opt_binds().add("pluginpath", pluginpath);
    owner.opt_binds().add("pluginconfpath", pluginconfpath);
    owner.opt_binds().add("persistdir", persistdir);
    owner.opt_binds().add("transformdb", transformdb);
    owner.opt_binds().add("recent", recent);
    owner.opt_binds().add("reset_nice", reset_nice);
    owner.opt_binds().add("system_cachedir", system_cachedir);
    owner.opt_binds().add("cacheonly", cacheonly);
    owner.opt_binds().add("keepcache", keepcache);
    owner.opt_binds().add("logdir", logdir);
    owner.opt_binds().add("log_size", log_size);
    owner.opt_binds().add("log_rotate", log_rotate);
    owner.opt_binds().add("varsdir", varsdir);
    owner.opt_binds().add("reposdir", reposdir);
    owner.opt_binds().add("debug_solver", debug_solver);

    owner.opt_binds().add(
        "installonlypkgs",
        installonlypkgs,
        [&](Option::Priority priority, const std::string & value) {
            option_T_list_append(installonlypkgs, priority, value);
        },
        nullptr,
        true);

    owner.opt_binds().add("group_package_types", group_package_types);
    owner.opt_binds().add("installonly_limit", installonly_limit);

    owner.opt_binds().add(
        "tsflags",
        tsflags,
        [&](Option::Priority priority, const std::string & value) { option_T_list_append(tsflags, priority, value); },
        nullptr,
        true);

    owner.opt_binds().add("assumeyes", assumeyes);
    owner.opt_binds().add("assumeno", assumeno);
    owner.opt_binds().add("check_config_file_age", check_config_file_age);
    owner.opt_binds().add("defaultyes", defaultyes);
    owner.opt_binds().add("diskspacecheck", diskspacecheck);
    owner.opt_binds().add("localpkg_gpgcheck", localpkg_gpgcheck);
    owner.opt_binds().add("gpgkey_dns_verification", gpgkey_dns_verification);
    owner.opt_binds().add("obsoletes", obsoletes);
    owner.opt_binds().add("showdupesfromrepos", showdupesfromrepos);
    owner.opt_binds().add("exit_on_lock", exit_on_lock);
    owner.opt_binds().add("allow_vendor_change", allow_vendor_change);
    owner.opt_binds().add("metadata_timer_sync", metadata_timer_sync);
    owner.opt_binds().add("disable_excludes", disable_excludes);
    owner.opt_binds().add("multilib_policy", multilib_policy);
    owner.opt_binds().add("best", best);
    owner.opt_binds().add("install_weak_deps", install_weak_deps);
    owner.opt_binds().add("bugtracker_url", bugtracker_url);
    owner.opt_binds().add("zchunk", zchunk);
    owner.opt_binds().add("color", color);
    owner.opt_binds().add("color_list_installed_older", color_list_installed_older);
    owner.opt_binds().add("color_list_installed_newer", color_list_installed_newer);
    owner.opt_binds().add("color_list_installed_reinstall", color_list_installed_reinstall);
    owner.opt_binds().add("color_list_installed_extra", color_list_installed_extra);
    owner.opt_binds().add("color_list_available_upgrade", color_list_available_upgrade);
    owner.opt_binds().add("color_list_available_downgrade", color_list_available_downgrade);
    owner.opt_binds().add("color_list_available_reinstall", color_list_available_reinstall);
    owner.opt_binds().add("color_list_available_install", color_list_available_install);
    owner.opt_binds().add("color_update_installed", color_update_installed);
    owner.opt_binds().add("color_update_local", color_update_local);
    owner.opt_binds().add("color_update_remote", color_update_remote);
    owner.opt_binds().add("color_search_match", color_search_match);
    owner.opt_binds().add("history_record", history_record);
    owner.opt_binds().add("history_record_packages", history_record_packages);
    owner.opt_binds().add("rpmverbosity", rpmverbosity);
    owner.opt_binds().add("strict", strict);
    owner.opt_binds().add("skip_broken", skip_broken);
    owner.opt_binds().add("autocheck_running_kernel", autocheck_running_kernel);
    owner.opt_binds().add("clean_requirements_on_remove", clean_requirements_on_remove);
    owner.opt_binds().add("history_list_view", history_list_view);
    owner.opt_binds().add("upgrade_group_objects_upgrade", upgrade_group_objects_upgrade);
    owner.opt_binds().add("destdir", destdir);
    owner.opt_binds().add("comment", comment);
    owner.opt_binds().add("ignorearch", ignorearch);
    owner.opt_binds().add("module_platform_id", module_platform_id);
    owner.opt_binds().add("user_agent", user_agent);
    owner.opt_binds().add("countme", countme);

    // Repo main config

    owner.opt_binds().add("retries", retries);
    owner.opt_binds().add("cachedir", cachedir);
    owner.opt_binds().add("fastestmirror", fastestmirror);

    owner.opt_binds().add(
        "excludepkgs",
        excludepkgs,
        [&](Option::Priority priority, const std::string & value) {
            option_T_list_append(excludepkgs, priority, value);
        },
        nullptr,
        true);
    //compatibility with yum
    owner.opt_binds().add(
        "exclude",
        excludepkgs,
        [&](Option::Priority priority, const std::string & value) {
            option_T_list_append(excludepkgs, priority, value);
        },
        nullptr,
        true);

    owner.opt_binds().add(
        "includepkgs",
        includepkgs,
        [&](Option::Priority priority, const std::string & value) {
            option_T_list_append(includepkgs, priority, value);
        },
        nullptr,
        true);

    owner.opt_binds().add("proxy", proxy);
    owner.opt_binds().add("proxy_username", proxy_username);
    owner.opt_binds().add("proxy_password", proxy_password);
    owner.opt_binds().add("proxy_auth_method", proxy_auth_method);
    owner.opt_binds().add(
        "protected_packages",
        protected_packages,
        [&](Option::Priority priority, const std::string & value) {
            if (priority >= protected_packages.get_priority()) {
                protected_packages.set(priority, resolve_globs(value));
            }
        },
        nullptr,
        false);

    owner.opt_binds().add("username", username);
    owner.opt_binds().add("password", password);
    owner.opt_binds().add("gpgcheck", gpgcheck);
    owner.opt_binds().add("repo_gpgcheck", repo_gpgcheck);
    owner.opt_binds().add("enabled", enabled);
    owner.opt_binds().add("enablegroups", enablegroups);
    owner.opt_binds().add("bandwidth", bandwidth);
    owner.opt_binds().add("minrate", minrate);
    owner.opt_binds().add("ip_resolve", ip_resolve);
    owner.opt_binds().add("throttle", throttle);
    owner.opt_binds().add("timeout", timeout);
    owner.opt_binds().add("max_parallel_downloads", max_parallel_downloads);
    owner.opt_binds().add("metadata_expire", metadata_expire);
    owner.opt_binds().add("sslcacert", sslcacert);
    owner.opt_binds().add("sslverify", sslverify);
    owner.opt_binds().add("sslclientcert", sslclientcert);
    owner.opt_binds().add("sslclientkey", sslclientkey);
    owner.opt_binds().add("deltarpm", deltarpm);
    owner.opt_binds().add("deltarpm_percentage", deltarpm_percentage);
    owner.opt_binds().add("skip_if_unavailable", skip_if_unavailable);
}

ConfigMain::ConfigMain() {
    p_impl = std::unique_ptr<Impl>(new Impl(*this));
}
ConfigMain::~ConfigMain() = default;

OptionNumber<std::int32_t> & ConfigMain::debuglevel() {
    return p_impl->debuglevel;
}
OptionNumber<std::int32_t> & ConfigMain::errorlevel() {
    return p_impl->errorlevel;
}
OptionString & ConfigMain::installroot() {
    return p_impl->installroot;
}
OptionString & ConfigMain::config_file_path() {
    return p_impl->config_file_path;
}
OptionBool & ConfigMain::plugins() {
    return p_impl->plugins;
}
OptionStringList & ConfigMain::pluginpath() {
    return p_impl->pluginpath;
}
OptionStringList & ConfigMain::pluginconfpath() {
    return p_impl->pluginconfpath;
}
OptionString & ConfigMain::persistdir() {
    return p_impl->persistdir;
}
OptionBool & ConfigMain::transformdb() {
    return p_impl->transformdb;
}
OptionNumber<std::int32_t> & ConfigMain::recent() {
    return p_impl->recent;
}
OptionBool & ConfigMain::reset_nice() {
    return p_impl->reset_nice;
}
OptionString & ConfigMain::system_cachedir() {
    return p_impl->system_cachedir;
}
OptionBool & ConfigMain::cacheonly() {
    return p_impl->cacheonly;
}
OptionBool & ConfigMain::keepcache() {
    return p_impl->keepcache;
}
OptionString & ConfigMain::logdir() {
    return p_impl->logdir;
}
OptionNumber<std::int32_t> & ConfigMain::log_size() {
    return p_impl->log_size;
}
OptionNumber<std::int32_t> & ConfigMain::log_rotate() {
    return p_impl->log_rotate;
}
OptionStringList & ConfigMain::varsdir() {
    return p_impl->varsdir;
}
OptionStringList & ConfigMain::reposdir() {
    return p_impl->reposdir;
}
OptionBool & ConfigMain::debug_solver() {
    return p_impl->debug_solver;
}
OptionStringList & ConfigMain::installonlypkgs() {
    return p_impl->installonlypkgs;
}
OptionStringList & ConfigMain::group_package_types() {
    return p_impl->group_package_types;
}
OptionNumber<std::uint32_t> & ConfigMain::installonly_limit() {
    return p_impl->installonly_limit;
}
OptionStringList & ConfigMain::tsflags() {
    return p_impl->tsflags;
}
OptionBool & ConfigMain::assumeyes() {
    return p_impl->assumeyes;
}
OptionBool & ConfigMain::assumeno() {
    return p_impl->assumeno;
}
OptionBool & ConfigMain::check_config_file_age() {
    return p_impl->check_config_file_age;
}
OptionBool & ConfigMain::defaultyes() {
    return p_impl->defaultyes;
}
OptionBool & ConfigMain::diskspacecheck() {
    return p_impl->diskspacecheck;
}
OptionBool & ConfigMain::localpkg_gpgcheck() {
    return p_impl->localpkg_gpgcheck;
}
OptionBool & ConfigMain::gpgkey_dns_verification() {
    return p_impl->gpgkey_dns_verification;
}
OptionBool & ConfigMain::obsoletes() {
    return p_impl->obsoletes;
}
OptionBool & ConfigMain::showdupesfromrepos() {
    return p_impl->showdupesfromrepos;
}
OptionBool & ConfigMain::exit_on_lock() {
    return p_impl->exit_on_lock;
}
OptionBool & ConfigMain::allow_vendor_change() {
    return p_impl->allow_vendor_change;
}
OptionSeconds & ConfigMain::metadata_timer_sync() {
    return p_impl->metadata_timer_sync;
}
OptionStringList & ConfigMain::disable_excludes() {
    return p_impl->disable_excludes;
}
OptionEnum<std::string> & ConfigMain::multilib_policy() {
    return p_impl->multilib_policy;
}
OptionBool & ConfigMain::best() {
    return p_impl->best;
}
OptionBool & ConfigMain::install_weak_deps() {
    return p_impl->install_weak_deps;
}
OptionString & ConfigMain::bugtracker_url() {
    return p_impl->bugtracker_url;
}
OptionBool & ConfigMain::zchunk() {
    return p_impl->zchunk;
}
OptionEnum<std::string> & ConfigMain::color() {
    return p_impl->color;
}
OptionString & ConfigMain::color_list_installed_older() {
    return p_impl->color_list_installed_older;
}
OptionString & ConfigMain::color_list_installed_newer() {
    return p_impl->color_list_installed_newer;
}
OptionString & ConfigMain::color_list_installed_reinstall() {
    return p_impl->color_list_installed_reinstall;
}
OptionString & ConfigMain::color_list_installed_extra() {
    return p_impl->color_list_installed_extra;
}
OptionString & ConfigMain::color_list_available_upgrade() {
    return p_impl->color_list_available_upgrade;
}
OptionString & ConfigMain::color_list_available_downgrade() {
    return p_impl->color_list_available_downgrade;
}
OptionString & ConfigMain::color_list_available_reinstall() {
    return p_impl->color_list_available_reinstall;
}
OptionString & ConfigMain::color_list_available_install() {
    return p_impl->color_list_available_install;
}
OptionString & ConfigMain::color_update_installed() {
    return p_impl->color_update_installed;
}
OptionString & ConfigMain::color_update_local() {
    return p_impl->color_update_local;
}
OptionString & ConfigMain::color_update_remote() {
    return p_impl->color_update_remote;
}
OptionString & ConfigMain::color_search_match() {
    return p_impl->color_search_match;
}
OptionBool & ConfigMain::history_record() {
    return p_impl->history_record;
}
OptionStringList & ConfigMain::history_record_packages() {
    return p_impl->history_record_packages;
}
OptionString & ConfigMain::rpmverbosity() {
    return p_impl->rpmverbosity;
}
OptionBool & ConfigMain::strict() {
    return p_impl->strict;
}
OptionBool & ConfigMain::skip_broken() {
    return p_impl->skip_broken;
}
OptionBool & ConfigMain::autocheck_running_kernel() {
    return p_impl->autocheck_running_kernel;
}
OptionBool & ConfigMain::clean_requirements_on_remove() {
    return p_impl->clean_requirements_on_remove;
}
OptionEnum<std::string> & ConfigMain::history_list_view() {
    return p_impl->history_list_view;
}
OptionBool & ConfigMain::upgrade_group_objects_upgrade() {
    return p_impl->upgrade_group_objects_upgrade;
}
OptionPath & ConfigMain::destdir() {
    return p_impl->destdir;
}
OptionString & ConfigMain::comment() {
    return p_impl->comment;
}
OptionBool & ConfigMain::downloadonly() {
    return p_impl->downloadonly;
}
OptionBool & ConfigMain::ignorearch() {
    return p_impl->ignorearch;
}

OptionString & ConfigMain::module_platform_id() {
    return p_impl->module_platform_id;
}
OptionString & ConfigMain::user_agent() {
    return p_impl->user_agent;
}
OptionBool & ConfigMain::countme() {
    return p_impl->countme;
}

// Repo main config
OptionNumber<std::uint32_t> & ConfigMain::retries() {
    return p_impl->retries;
}
OptionString & ConfigMain::cachedir() {
    return p_impl->cachedir;
}
OptionBool & ConfigMain::fastestmirror() {
    return p_impl->fastestmirror;
}
OptionStringList & ConfigMain::excludepkgs() {
    return p_impl->excludepkgs;
}
OptionStringList & ConfigMain::includepkgs() {
    return p_impl->includepkgs;
}
OptionString & ConfigMain::proxy() {
    return p_impl->proxy;
}
OptionString & ConfigMain::proxy_username() {
    return p_impl->proxy_username;
}
OptionString & ConfigMain::proxy_password() {
    return p_impl->proxy_password;
}
OptionEnum<std::string> & ConfigMain::proxy_auth_method() {
    return p_impl->proxy_auth_method;
}
OptionStringList & ConfigMain::protected_packages() {
    return p_impl->protected_packages;
}
OptionString & ConfigMain::username() {
    return p_impl->username;
}
OptionString & ConfigMain::password() {
    return p_impl->password;
}
OptionBool & ConfigMain::gpgcheck() {
    return p_impl->gpgcheck;
}
OptionBool & ConfigMain::repo_gpgcheck() {
    return p_impl->repo_gpgcheck;
}
OptionBool & ConfigMain::enabled() {
    return p_impl->enabled;
}
OptionBool & ConfigMain::enablegroups() {
    return p_impl->enablegroups;
}
OptionNumber<std::uint32_t> & ConfigMain::bandwidth() {
    return p_impl->bandwidth;
}
OptionNumber<std::uint32_t> & ConfigMain::minrate() {
    return p_impl->minrate;
}
OptionEnum<std::string> & ConfigMain::ip_resolve() {
    return p_impl->ip_resolve;
}
OptionNumber<float> & ConfigMain::throttle() {
    return p_impl->throttle;
}
OptionSeconds & ConfigMain::timeout() {
    return p_impl->timeout;
}
OptionNumber<std::uint32_t> & ConfigMain::max_parallel_downloads() {
    return p_impl->max_parallel_downloads;
}
OptionSeconds & ConfigMain::metadata_expire() {
    return p_impl->metadata_expire;
}
OptionString & ConfigMain::sslcacert() {
    return p_impl->sslcacert;
}
OptionBool & ConfigMain::sslverify() {
    return p_impl->sslverify;
}
OptionString & ConfigMain::sslclientcert() {
    return p_impl->sslclientcert;
}
OptionString & ConfigMain::sslclientkey() {
    return p_impl->sslclientkey;
}
OptionBool & ConfigMain::deltarpm() {
    return p_impl->deltarpm;
}
OptionNumber<std::uint32_t> & ConfigMain::deltarpm_percentage() {
    return p_impl->deltarpm_percentage;
}
OptionBool & ConfigMain::skip_if_unavailable() {
    return p_impl->skip_if_unavailable;
}

static void dir_close(DIR * d) {
    closedir(d);
}

void ConfigMain::add_vars_from_dir(std::map<std::string, std::string> & vars_map, const std::string & dir_path) {
    if (DIR * dir = opendir(dir_path.c_str())) {
        std::unique_ptr<DIR, decltype(&dir_close)> dir_guard(dir, &dir_close);
        while (auto ent = readdir(dir)) {
            auto dname = ent->d_name;
            if (dname[0] == '.' && (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0')))
                continue;

            auto full_path = dir_path;
            if (full_path.back() != '/') {
                full_path += "/";
            }
            full_path += dname;
            std::ifstream in_stream(full_path);
            if (in_stream.fail()) {
                // log.warning()
                continue;
            }
            std::string line;
            std::getline(in_stream, line);
            if (in_stream.fail()) {
                // log.warning()
                continue;
            }
            vars_map[dname] = std::move(line);
        }
    }
}

void ConfigMain::add_vars_from_env(std::map<std::string, std::string> & vars_map) {
    if (!environ) {
        return;
    }

    for (const char * const * var_ptr = environ; *var_ptr; ++var_ptr) {
        auto var = *var_ptr;
        if (auto eql_ptr = strchr(var, '=')) {
            auto eql_idx = eql_ptr - var;
            if (eql_idx == 4 && strncmp("DNF", var, 3) == 0 && isdigit(var[3]) != 0) {
                // DNF[0-9]
                vars_map[std::string(var, static_cast<size_t>(eql_idx))] = eql_ptr + 1;
            } else if (
                // DNF_VAR_[A-Za-z0-9_]+ , DNF_VAR_ prefix is cut off
                eql_idx > 8 && strncmp("DNF_VAR_", var, 8) == 0 &&
                static_cast<int>(strspn(var + 8, ASCII_LETTERS DIGITS "_")) == eql_idx - 8) {
                vars_map[std::string(var + 8, static_cast<size_t>(eql_idx - 8))] = eql_ptr + 1;
            }
        }
    }
}

}  // namespace libdnf
