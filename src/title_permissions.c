/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Manual, metadata-only preparation. No transfer, deletion, chown or exit(). */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if YAKUMO_NATIVE
#include "klog.h"
#endif

#define BUILD "title-permissions-0.1.0"
#define PATH_BYTES 1024
#define OBJECT_LIMIT 65536
struct object { char *path; struct stat st; };
struct inventory { struct object *objects; size_t count, capacity; };
static char root[PATH_BYTES] = "/data/homebrew/PPSA99373";
static char problem[PATH_BYTES];
static char observed[192];
static const char *operation = "preflight";
static size_t changed;

static int reject(const char *path, int error) {
    snprintf(problem, sizeof(problem), "%s", path);
    errno = error; return -1;
}
static bool safe_name(const char *name) {
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
    return true;
}
static int join(char *out, const char *base, const char *leaf) {
    if (!safe_name(leaf)) return reject(base, EINVAL);
    int n = snprintf(out, PATH_BYTES, "%s/%s", base, leaf);
    if (n < 0 || n >= PATH_BYTES) return reject(base, ENAMETOOLONG);
    return 0;
}
/* Match the proven title adapter's no-follow traversal. No weaker fallback.
 * Keep Yakumo and all external writers closed: this is not a filesystem lock. */
static int checked_open(const char *path, int flags) {
    char prefix[PATH_BYTES]; size_t n = strlen(path);
    if (!n || n >= PATH_BYTES || path[0] != '/' || path[n-1] == '/' || strstr(path, "//"))
        return reject(path, EINVAL);
    for (size_t i = 1; i <= n; ++i) {
        if (i < n && path[i] != '/') continue;
        memcpy(prefix, path, i); prefix[i] = 0;
        if (!safe_name(strrchr(prefix, '/')+1)) return reject(path, EINVAL);
        int fd = open(prefix, (i == n ? flags : O_RDONLY|O_DIRECTORY)|O_NOFOLLOW|O_NONBLOCK);
        if (fd < 0) return reject(prefix, errno);
        struct stat st;
        if (fstat(fd, &st)) { int error = errno; close(fd); return reject(prefix, error); }
        if (i < n && !S_ISDIR(st.st_mode)) { close(fd); return reject(prefix, ENOTDIR); }
        if (i == n) return fd;
        if (close(fd)) return reject(prefix, errno);
    }
    return reject(path, EINVAL);
}
static void release(struct inventory *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->objects[i].path);
    free(list->objects); memset(list, 0, sizeof(*list));
}
static bool same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_mode == b->st_mode && a->st_nlink == b->st_nlink &&
        a->st_uid == b->st_uid && a->st_gid == b->st_gid && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}
static int metadata(const char *path, const struct stat *st) {
    snprintf(observed, sizeof(observed), "mode=%o nlink=%ju ino=%ju uid=%ju gid=%ju",
        (unsigned)st->st_mode, (uintmax_t)st->st_nlink, (uintmax_t)st->st_ino,
        (uintmax_t)st->st_uid, (uintmax_t)st->st_gid);
    if (!S_ISDIR(st->st_mode) && !S_ISREG(st->st_mode)) return reject(path, EPERM);
    /* Zero is NOT evidence of a single link. Never translate unknown metadata.
     * Historical title fstat nlink=0 must be tested separately in the ELF loader. */
    if (!st->st_nlink || !st->st_ino) {
        operation = "METADATA_UNVERIFIED"; return reject(path, EIO);
    }
    if (S_ISREG(st->st_mode) && st->st_nlink != 1) return reject(path, EMLINK);
    return 0;
}
static int scan(struct inventory *list, const char *path, unsigned depth) {
    if (depth > 16 || list->count >= OBJECT_LIMIT) return reject(path, EOVERFLOW);
    int fd = checked_open(path, O_RDONLY); if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { int error = errno; close(fd); return reject(path, error); }
    if (metadata(path, &st)) { int error = errno; close(fd); errno = error; return -1; }
    if (list->count && st.st_dev != list->objects[0].st.st_dev) {
        close(fd); return reject(path, EXDEV);
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity*2 : 64;
        void *next = realloc(list->objects, capacity*sizeof(*list->objects));
        if (!next) { close(fd); return reject(path, ENOMEM); }
        list->objects = next; list->capacity = capacity;
    }
    char *copy = strdup(path);
    if (!copy) { close(fd); return reject(path, ENOMEM); }
    list->objects[list->count++] = (struct object){copy, st};
    if (!S_ISDIR(st.st_mode)) return close(fd) ? reject(path, errno) : 0;
    DIR *dir = fdopendir(fd);
    if (!dir) { int error = errno; close(fd); return reject(path, error); }
    int rc = 0;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) rc = reject(path, errno); break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_BYTES];
        if (join(child, path, entry->d_name) || scan(list, child, depth+1)) { rc = -1; break; }
    }
    int error = errno;
    if (closedir(dir) && !rc) { rc = reject(path, errno); error = errno; }
    errno = error; return rc;
}
static int by_path(const void *a, const void *b) {
    return strcmp(((const struct object *)a)->path, ((const struct object *)b)->path);
}
static int by_inode(const void *a, const void *b) {
    const struct stat *x = &((const struct object *)a)->st, *y = &((const struct object *)b)->st;
    if (x->st_dev != y->st_dev) return x->st_dev < y->st_dev ? -1 : 1;
    return x->st_ino == y->st_ino ? 0 : (x->st_ino < y->st_ino ? -1 : 1);
}
static int inventory(struct inventory *list) {
    if (scan(list, root, 0)) return -1;
    qsort(list->objects, list->count, sizeof(*list->objects), by_inode);
    for (size_t i = 1; i < list->count; ++i)
        if (!by_inode(&list->objects[i-1], &list->objects[i]))
            return reject(list->objects[i].path, EMLINK);
    qsort(list->objects, list->count, sizeof(*list->objects), by_path);
    return 0;
}
static const struct object *find(const struct inventory *list, const char *relative) {
    char path[PATH_BYTES];
    if (snprintf(path, sizeof(path), "%s/%s", root, relative) >= (int)sizeof(path)) return NULL;
    for (size_t i = 0; i < list->count; ++i)
        if (!strcmp(path, list->objects[i].path)) return &list->objects[i];
    return NULL;
}
static int small_file(const struct object *obj, char *text, size_t capacity) {
    if (!obj || !S_ISREG(obj->st.st_mode) || obj->st.st_size <= 0 ||
        (uintmax_t)obj->st.st_size >= capacity) return reject(root, EINVAL);
    int fd = checked_open(obj->path, O_RDONLY); if (fd < 0) return -1;
    struct stat st; int rc = 0;
    if (fstat(fd, &st)) rc = reject(obj->path, errno);
    else if (!same(&st, &obj->st)) rc = reject(obj->path, ESTALE);
    size_t done = 0;
    while (!rc && done < (size_t)obj->st.st_size) {
        ssize_t n = read(fd, text+done, (size_t)obj->st.st_size-done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { rc = reject(obj->path, n < 0 ? errno : EIO); break; }
        done += (size_t)n;
    }
    if (!rc && memchr(text, 0, done)) rc = reject(obj->path, EINVAL);
    text[done] = 0;
    if (!rc && fstat(fd, &st)) rc = reject(obj->path, errno);
    if (!rc && !same(&st, &obj->st)) rc = reject(obj->path, ESTALE);
    int error = errno;
    if (close(fd) && !rc) { rc = reject(obj->path, errno); error = errno; }
    errno = error; return rc;
}
static int identity(const struct inventory *list) {
    char text[32769];
    if (small_file(find(list, "yakumo/title.id"), text, 32)) return -1;
    if (strcmp(text, "PPSA99373\n") && strcmp(text, "PPSA99373")) return reject(root, EPERM);
    /* This is an exact package identity check, not a general JSON parser.
     * Require a unique literal titleId and its complete value token. */
    if (small_file(find(list, "sce_sys/param.json"), text, sizeof(text))) return -1;
    char *key = strstr(text, "\"titleId\"");
    if (!key || strstr(key+9, "\"titleId\"")) return reject(root, EPERM);
    char *p = key+9; while (isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return reject(root, EPERM);
    while (isspace((unsigned char)*p)) ++p;
    if (strncmp(p, "\"PPSA99373\"", 11)) return reject(root, EPERM);
    p += 11; while (isspace((unsigned char)*p)) ++p;
    if (*p != ',' && *p != '}') return reject(root, EPERM);
    const char *required[] = {"eboot.bin", "sce_module/libc.prx", "yakumo/game/EBOOT.ELF"};
    for (size_t i = 0; i < sizeof(required)/sizeof(*required); ++i) {
        const struct object *obj = find(list, required[i]);
        if (!obj || !S_ISREG(obj->st.st_mode) || obj->st.st_size <= 0)
            return reject(required[i], ENOENT);
    }
    return 0;
}
static mode_t wanted(const struct object *obj) {
    if (S_ISDIR(obj->st.st_mode)) return 0777;
    const char *relative = obj->path+strlen(root)+1;
    return !strcmp(relative, "eboot.bin") || !strcmp(relative, "sce_module/libc.prx") ? 0777 : 0666;
}
static int equal_inventory(const struct inventory *before, const struct inventory *after) {
    if (before->count != after->count) return reject(root, ESTALE);
    for (size_t i = 0; i < before->count; ++i)
        if (strcmp(before->objects[i].path, after->objects[i].path) ||
            !same(&before->objects[i].st, &after->objects[i].st))
            return reject(before->objects[i].path, ESTALE);
    return 0;
}
static int repair(struct inventory *list) {
    for (size_t i = 0; i < list->count; ++i) {
        struct object *obj = &list->objects[i]; mode_t mode = wanted(obj);
        int fd = checked_open(obj->path, O_RDONLY); if (fd < 0) return -1;
        struct stat st; int rc = 0;
        if (fstat(fd, &st)) rc = reject(obj->path, errno);
        else if (!same(&st, &obj->st)) rc = reject(obj->path, ESTALE);
        if (!rc && (st.st_mode & 07777) != mode) {
            if (fchmod(fd, mode)) rc = reject(obj->path, errno);
            else {
                ++changed;
                obj->st.st_mode = (obj->st.st_mode & ~07777) | mode;
                if (fstat(fd, &st)) rc = reject(obj->path, errno);
                else if (!same(&st, &obj->st)) rc = reject(obj->path, ESTALE);
            }
        }
        int error = errno;
        if (close(fd) && !rc) { rc = reject(obj->path, errno); error = errno; }
        errno = error; if (rc) return -1;
    }
    return 0;
}
static void notify(const char *message) {
#if YAKUMO_NATIVE
    /* No dependency on a still-connected PC sender's stdout socket. */
    klog_printf("[%s] %s\n", BUILD, message);
    struct { char prefix[45]; char message[3075]; } request = {0};
    extern int sceKernelSendNotificationRequest(int, void *, size_t, int);
    snprintf(request.message, sizeof(request.message), "%s", message);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#else
    fprintf(stderr, "[%s] %s\n", BUILD, message);
#endif
}
int main(int argc, char **argv) {
#if !YAKUMO_NATIVE
    /* Linux /tmp fixtures only; the PS5 title path is never configurable. */
    if (argc != 2 || strlen(argv[1]) >= sizeof(root)) return 2;
    snprintf(root, sizeof(root), "%s", argv[1]);
#else
    (void)argc; (void)argv;
#endif
    struct inventory list = {0}, check = {0}; char message[2048]; int result = 1;
    if (inventory(&list) || identity(&list)) goto failure;
    /* Detect a foreign owner before changing ANY mode. No UID/capability patch. */
    operation = "owner preflight";
    for (size_t i = 0; i < list.count; ++i)
        if ((list.objects[i].st.st_mode & 07777) != wanted(&list.objects[i]) &&
            geteuid() != 0 && geteuid() != list.objects[i].st.st_uid) {
            reject(list.objects[i].path, EPERM); goto failure;
        }
    operation = "preflight revalidation";
    if (inventory(&check) || equal_inventory(&list, &check)) goto failure;
    release(&check);
    operation = "fchmod/readback";
    if (repair(&list)) goto failure;
    operation = "final verification";
    if (inventory(&check) || identity(&check) || equal_inventory(&list, &check)) goto failure;
    snprintf(message, sizeof(message), "Yakumo: PERMISOS LISTOS. %zu objetos verificados, %zu cambios. Ya puedes abrir el juego.", list.count, changed);
    notify(message); result = 0; goto done;
failure: {
    int error = errno;
    snprintf(message, sizeof(message), "Yakumo: ERROR %s; errno=%d (%s); euid=%ju; cambios=%zu; %s; last metadata: %s. No abras el juego con esta preparacion.",
        operation, error, strerror(error), (uintmax_t)geteuid(), changed, problem, observed);
    notify(message);
}
done:
    release(&list); release(&check); return result; /* CRT returns to the loader. */
}
