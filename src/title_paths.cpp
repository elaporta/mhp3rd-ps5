// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace yakumo::ps5 {
#ifndef YAKUMO_NATIVE_PATHS
#define YAKUMO_NATIVE_PATHS YAKUMO_NATIVE
#endif
bool join_path(char* out, std::size_t capacity, const char* root, const char* relative) {
    if (!root || root[0] != '/' || !relative || relative[0] == '/' || std::strchr(relative, '\\')) return false;
    const char* part = relative;
    for (const char* p = relative;; ++p) {
        if (*p == '/' || *p == '\0') {
            const auto n = p - part;
            if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!*p) break;
            part = p + 1;
        }
    }
    const int n = std::snprintf(out, capacity, "%s/%s", root, relative);
    return n > 0 && static_cast<std::size_t>(n) < capacity;
}

bool TitlePaths::fail(const char* operation, const char* path, int error, Log* log) {
    const auto used = std::strlen(diagnostics);
    std::snprintf(diagnostics + used, sizeof(diagnostics) - used, "%s%s %s errno=%d",
                  used ? "\n" : "", operation, path, error);
    if (log) log->write("[paths] FAIL operation=%s path=%s errno=%d", operation, path, error);
    errno = error;
    return false;
}

#if YAKUMO_NATIVE_PATHS
// realpath() and lstat() are refused on the user's console, even for /app0.
// Open directory components with O_NOFOLLOW and inspect the resulting handles.
// This deliberately rejects symlink aliases instead of guessing their targets.
static bool native_directory(TitlePaths& paths, const char* path, bool create_leaf, Log* log) {
    if (!path || path[0] != '/' || !path[1]) return paths.fail("invalid-directory", path ? path : "", EINVAL, log);
    if (std::strlen(path) >= path_capacity) return paths.fail("directory-too-long", path, ENAMETOOLONG, log);
    char prefix[path_capacity]{};
    std::snprintf(prefix, sizeof(prefix), "%s", path);
    const char* part = path + 1;
    // Validate the whole spelling before attempting any filesystem mutation.
    for (const char* p = part;; ++p) {
        if (*p != '/' && *p != '\0') continue;
        const auto length = p - part;
        if (!length || (length == 1 && *part == '.') ||
            (length == 2 && part[0] == '.' && part[1] == '.'))
            return paths.fail("invalid-component", path, EINVAL, log);
        if (!*p) break;
        part = p + 1;
    }
    for (char* p = prefix + 1;; ++p) {
        if (*p != '/' && *p != '\0') continue;
        const bool leaf = *p == '\0';
        *p = '\0';
        constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
        int fd = open(prefix, flags);
        if (fd < 0 && errno == ENOENT && leaf && create_leaf) {
            if (mkdir(prefix, 0755) && errno != EEXIST) return paths.fail("mkdir", prefix, errno, log);
            fd = open(prefix, flags);
        }
        if (fd < 0) return paths.fail("open-directory-nofollow", prefix, errno, log);
        struct stat st{};
        const int rc = fstat(fd, &st);
        const int saved_error = errno;
        const int close_rc = close(fd);
        if (rc) return paths.fail("fstat-directory", prefix, saved_error, log);
        if (close_rc) return paths.fail("close-directory", prefix, errno, log);
        if (!S_ISDIR(st.st_mode)) return paths.fail("not-directory", prefix, ENOTDIR, log);
        if (leaf) return true;
        *p = '/';
    }
}
#endif

static bool title_matches(TitlePaths& paths, const char* root, const char* title, Log* log) {
    char path[path_capacity], marker[64]{};
    const auto open_identity = [](const char* name) -> FILE* {
#if YAKUMO_NATIVE_PATHS
        const int fd = open(name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) return nullptr;
        struct stat st{};
        const int rc = fstat(fd, &st);
        if (rc || !S_ISREG(st.st_mode)) {
            const int saved = rc ? errno : EINVAL;
            close(fd); errno = saved; return nullptr;
        }
        FILE* file = fdopen(fd, "rb");
        if (!file) { const int saved = errno; close(fd); errno = saved; }
        return file;
#else
        return std::fopen(name, "rb");
#endif
    };
#if YAKUMO_NATIVE_PATHS
    const char* identity_directories[] = {"yakumo", "sce_sys"};
    for (const char* relative : identity_directories) {
        if (!join_path(path, sizeof(path), root, relative)) return paths.fail("join-identity-directory", root, ENAMETOOLONG, log);
        if (!native_directory(paths, path, false, log)) return false;
    }
#endif
    if (!join_path(path, sizeof(path), root, "yakumo/title.id")) return paths.fail("join-marker", root, ENAMETOOLONG, log);
    FILE* f = open_identity(path);
    if (!f) return paths.fail("open-marker", path, errno, log);
    const auto n = std::fread(marker, 1, sizeof(marker) - 1, f);
    const int marker_error = std::ferror(f) ? (errno ? errno : EIO) : 0;
    std::fclose(f);
    if (marker_error) return paths.fail("read-marker", path, marker_error, log);
    if (n == sizeof(marker) - 1) return paths.fail("marker-too-long", path, EINVAL, log);
    marker[std::strcspn(marker, "\r\n")] = '\0';
    if (std::strcmp(marker, title)) return paths.fail("marker-title-mismatch", path, EINVAL, log);
    if (!join_path(path, sizeof(path), root, "sce_sys/param.json")) return paths.fail("join-metadata", root, ENAMETOOLONG, log);
    char metadata[8192]{};
    f = open_identity(path);
    if (!f) return paths.fail("open-metadata", path, errno, log);
    const auto size = std::fread(metadata, 1, sizeof(metadata) - 1, f);
    const int metadata_error = std::ferror(f) ? (errno ? errno : EIO) : 0;
    std::fclose(f);
    if (metadata_error) return paths.fail("read-metadata", path, metadata_error, log);
    if (size == sizeof(metadata) - 1) return paths.fail("metadata-too-long", path, EINVAL, log);
    // Read the generated package's single string-valued titleId, not general user JSON.
    const char* key = std::strstr(metadata, "\"titleId\"");
    if (!key || std::strstr(key + 9, "\"titleId\"")) return paths.fail("metadata-title-key", path, EINVAL, log);
    key += 9;
    key += std::strspn(key, " \t\r\n");
    if (*key++ != ':') return paths.fail("metadata-title-colon", path, EINVAL, log);
    key += std::strspn(key, " \t\r\n");
    if (*key++ != '"') return paths.fail("metadata-title-string", path, EINVAL, log);
    const auto length = std::strlen(title);
    if (std::strlen(key) <= length || std::strncmp(key, title, length) || key[length] != '"')
        return paths.fail("metadata-title-mismatch", path, EINVAL, log);
    return true;
}

bool TitlePaths::resolve(const char* const* candidates, std::size_t count, const char* title_id, Log* log) {
    *this = {};
    if (!title_id || std::strlen(title_id) != 9 || std::strncmp(title_id, "PPSA", 4) ||
        std::strspn(title_id + 4, "0123456789") != 5) return fail("invalid-title-id", "", EINVAL, log);
    for (std::size_t i = 0; i < count; ++i) {
        if (!candidates[i] || candidates[i][0] != '/') continue;
        if (log) log->write("[paths] candidate=%s", candidates[i]);
#if YAKUMO_NATIVE_PATHS
        if (!native_directory(*this, candidates[i], false, log)) continue;
        const char* canonical = candidates[i];
#else
        char* canonical = realpath(candidates[i], nullptr);
        if (!canonical) { fail("realpath", candidates[i], errno, log); continue; }
#endif
        const bool valid = std::strlen(canonical) < sizeof(root) ? title_matches(*this, canonical, title_id, log)
            : fail("root-too-long", candidates[i], ENAMETOOLONG, log);
        if (valid) std::snprintf(root, sizeof(root), "%s", canonical);
#if !YAKUMO_NATIVE_PATHS
        std::free(canonical);
#endif
        if (valid) { diagnostics[0] = '\0'; return true; }
    }
    return false;
}

static bool directory_inside(TitlePaths& paths, const char* path, Log* log) {
#if YAKUMO_NATIVE_PATHS
    const auto size = std::strlen(paths.root);
    if (std::strncmp(path, paths.root, size) || path[size] != '/')
        return paths.fail("outside-title", path, EPERM, log);
    return native_directory(paths, path, true, log);
#else
    if (mkdir(path, 0755) && errno != EEXIST) return paths.fail("mkdir", path, errno, log);
    struct stat st{};
    if (stat(path, &st)) return paths.fail("stat", path, errno, log);
    if (!S_ISDIR(st.st_mode)) return paths.fail("not-directory", path, ENOTDIR, log);
    char* actual = realpath(path, nullptr);
    if (!actual) return paths.fail("realpath", path, errno, log);
    const auto size = std::strlen(paths.root);
    const bool contained = !std::strncmp(actual, paths.root, size) && actual[size] == '/';
    std::free(actual);
    return contained || paths.fail("outside-title", path, EPERM, log);
#endif
}

bool TitlePaths::prepare_logs(Log* log) {
    if (!root[0]) return fail("unresolved-root", "", EINVAL, log);
    char base[path_capacity];
    if (!join_path(base, sizeof(base), root, "yakumo") || !join_path(logs, sizeof(logs), base, "logs"))
        return fail("join-log-directory", root, ENAMETOOLONG, log);
    return directory_inside(*this, base, log) && directory_inside(*this, logs, log);
}

bool TitlePaths::prepare(Log* log) {
    if (!prepare_logs(log)) return false;
    char base[path_capacity];
    if (!join_path(base, sizeof(base), root, "yakumo")) return fail("join-yakumo", root, ENAMETOOLONG, log);
    struct Field { const char* relative; char* output; } fields[] = {
        {"game", game}, {"textures", textures}, {"assets", assets}, {"config", config},
        {"saves", saves}, {"logs", logs}, {"cache", cache}};
    for (const auto& field : fields) {
        if (!join_path(field.output, path_capacity, base, field.relative))
            return fail("join-directory", base, ENAMETOOLONG, log);
        if (!directory_inside(*this, field.output, log)) return false;
    }
    char textures_id[path_capacity];
    if (!join_path(textures_id, sizeof(textures_id), textures, "NPJB40001"))
        return fail("join-texture-directory", textures, ENAMETOOLONG, log);
    return directory_inside(*this, textures_id, log);
}
}
