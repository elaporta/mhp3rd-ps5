// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#if YAKUMO_NATIVE_PATHS
extern int realpath_calls;
extern int lstat_calls;
extern bool deny_stat;
constexpr const char* containment_error = "open-directory-nofollow";
#else
constexpr const char* containment_error = "outside-title";
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    namespace fs = std::filesystem;
    using namespace yakumo::ps5;
    char temp[] = "/tmp/yakumo-probe-test-XXXXXX";
    CHECK(mkdtemp(temp));
    fs::path root(temp);
    struct Cleanup { fs::path p; ~Cleanup() { fs::remove_all(p); } } cleanup{root};
    fs::create_directories(root / "title/yakumo");
    fs::create_directories(root / "title/sce_sys");
    std::ofstream(root / "title/yakumo/title.id") << "PPSA99373\n";
    std::ofstream(root / "title/sce_sys/param.json") << "{\"titleId\": \"PPSA99373\"}";
    const auto candidate = (root / "title").string();
    const char* candidates[] = {"relative", "/nonexistent-yakumo-probe", candidate.c_str()};
    TitlePaths paths;
    CHECK(!paths.resolve(candidates, 3, "PPSA00000"));
    CHECK(std::string(paths.diagnostics).find("marker-title-mismatch") != std::string::npos);
    CHECK(!paths.resolve(candidates, 3, "../../bad"));
    CHECK(paths.resolve(candidates, 3, "PPSA99373"));
    CHECK(paths.diagnostics[0] == '\0');
    CHECK(paths.prepare());
#if YAKUMO_NATIVE_PATHS
    {
        Log api_log;
        char summary[256];
        deny_stat = true;
        // The matrix still opens and reads the identity despite stat EPERM.
        filesystem_diagnostics(candidate.c_str(), "TEST", summary, sizeof(summary), api_log);
        CHECK(std::string(summary) == "TEST S/O/D/N/F/M/R=1/0/0/0/0/0/0");
        CHECK(paths.resolve(candidates, 3, "PPSA99373"));
        CHECK(paths.prepare());
        deny_stat = false;
    }
#endif
    char out[128];
    CHECK(!join_path(out, sizeof(out), "/tmp", "../bad"));
    CHECK(!join_path(out, sizeof(out), "/tmp", "/bad"));
    CHECK(!join_path(out, sizeof(out), "/tmp", "x/../bad"));
    CHECK(!join_path(out, 3, "/tmp", "too-long"));
    {
        Log log;
        log.write("early-log-test");
        CHECK(log.attach(paths));
        CHECK(storage_probe(paths, log));
        CHECK(storage_probe(paths, log));
        const auto pending = fs::path(paths.cache) / "probe-persistence.tmp";
        std::ofstream(pending) << "unexpected existing temporary file";
        CHECK(!storage_probe(paths, log));
        CHECK(fs::file_size(pending) > 0);
        fs::remove(pending);
        std::ofstream(fs::path(paths.cache) / "probe-persistence.txt") << "preserve corrupt marker";
        CHECK(!storage_probe(paths, log));
    }
    std::ifstream log_file(fs::path(paths.logs) / "probe.log");
    const std::string log((std::istreambuf_iterator<char>(log_file)), {});
    CHECK(log.find("early-log-test") != std::string::npos);
    CHECK(log.find("previous-run sentinel verified") != std::string::npos);
    if (geteuid() != 0) {
        const auto log_path = fs::path(paths.logs) / "probe.log";
        CHECK(chmod(log_path.c_str(), 0400) == 0);
        Log denied;
        CHECK(!denied.attach(paths));
        CHECK(chmod(log_path.c_str(), 0600) == 0);
    }
    fs::remove_all(root / "title/yakumo/saves");
    fs::create_directory(root / "outside");
    fs::create_directory_symlink(root / "outside", root / "title/yakumo/saves");
    {
        Log early;
        CHECK(paths.prepare_logs(&early));
        CHECK(early.attach(paths));
        CHECK(!paths.prepare(&early));
        CHECK(std::string(paths.diagnostics).find(containment_error) != std::string::npos);
    }
    std::ifstream failed_log(fs::path(paths.logs) / "probe.log");
    CHECK(std::string((std::istreambuf_iterator<char>(failed_log)), {}).find(containment_error) != std::string::npos);
#if YAKUMO_NATIVE_PATHS
    // Reject an alias to an otherwise valid title and an alias in an ancestor.
    fs::create_directory_symlink(root / "title", root / "alias");
    const auto alias = (root / "alias").string();
    const char* aliases[] = {alias.c_str()};
    CHECK(!paths.resolve(aliases, 1, "PPSA99373"));
    CHECK(std::string(paths.diagnostics).find(containment_error) != std::string::npos);
    fs::create_directory_symlink(root, root / "ancestor");
    const auto ancestor = (root / "ancestor/title").string();
    const char* ancestor_paths[] = {ancestor.c_str()};
    CHECK(!paths.resolve(ancestor_paths, 1, "PPSA99373"));
    CHECK(std::string(paths.diagnostics).find(containment_error) != std::string::npos);
    const auto traversal = candidate + "/../title";
    const char* traversal_paths[] = {traversal.c_str()};
    CHECK(!paths.resolve(traversal_paths, 1, "PPSA99373"));
    CHECK(std::string(paths.diagnostics).find("invalid-component") != std::string::npos);
    // A replaced yakumo parent must not create a logs folder outside this title.
    fs::rename(root / "title/yakumo", root / "title/yakumo-held");
    fs::create_directory_symlink(root / "outside", root / "title/yakumo");
    std::snprintf(paths.root, sizeof(paths.root), "%s", candidate.c_str());
    CHECK(!paths.prepare_logs());
    CHECK(!fs::exists(root / "outside/logs"));
    fs::remove(root / "title/yakumo");
    fs::rename(root / "title/yakumo-held", root / "title/yakumo");
    CHECK(realpath_calls == 0);
    CHECK(lstat_calls == 0);
    // Identity leaves must not follow a link even to an otherwise valid marker.
    fs::rename(root / "title/yakumo/title.id", root / "title/yakumo/title-held.id");
    fs::create_symlink(root / "title/yakumo/title-held.id", root / "title/yakumo/title.id");
    CHECK(!paths.resolve(aliases, 1, "PPSA99373"));
    const char* marker_candidate[] = {candidate.c_str()};
    CHECK(!paths.resolve(marker_candidate, 1, "PPSA99373"));
    CHECK(std::string(paths.diagnostics).find("open-marker") != std::string::npos);
    fs::remove(root / "title/yakumo/title.id");
    fs::rename(root / "title/yakumo/title-held.id", root / "title/yakumo/title.id");
#endif
    std::ofstream(root / "title/sce_sys/param.json") << "{\"titleId\":\"PPSA00000\"}";
    CHECK(!paths.resolve(candidates, 3, "PPSA99373"));
    CHECK(std::string(paths.diagnostics).find("metadata-title-mismatch") != std::string::npos);
    fs::remove(root / "title/yakumo/title.id");
    CHECK(!paths.resolve(candidates, 3, "PPSA99373"));
    CHECK(std::string(paths.diagnostics).find("open-marker") != std::string::npos);
    {
        Log api_log;
        char summary[256];
        filesystem_diagnostics(candidate.c_str(), "MISSING", summary, sizeof(summary), api_log);
        CHECK(std::string(summary) == "MISSING S/O/D/N/F/M/R=0/0/0/0/0/2/-1");
        CHECK(!fs::exists(root / "title/yakumo/title.id"));
    }
    std::puts("PASS title identity, paths, containment, persistence and early logging");
}
