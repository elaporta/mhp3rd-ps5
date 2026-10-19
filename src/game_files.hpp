// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <fstream>
#include <streambuf>
#include <vector>
#include <memory>
#include <sys/stat.h>

// Native-only source projection uses this backend. Desktop std::filesystem
// and streams are unchanged. No process-wide libc interposition is installed.
namespace yakumo::game_files {
using std::filesystem::path;
using std::filesystem::filesystem_error;
using std::filesystem::directory_options;
using std::filesystem::file_status;
using std::filesystem::file_type;
using std::filesystem::space_info;
using std::filesystem::file_time_type;
using std::filesystem::copy_options;
inline bool exists(file_status s) { return std::filesystem::exists(s); }
inline bool is_symlink(file_status s) { return std::filesystem::is_symlink(s); }
inline bool is_directory(file_status s) { return std::filesystem::is_directory(s); }
inline bool is_regular_file(file_status s) { return std::filesystem::is_regular_file(s); }
void set_root(const path& root);
const path& root();
bool permitted(const path&, bool write, std::error_code&);
int open_file(const path&, int flags, unsigned mode = 0666);
// Repair generated writable trees only, through validated no-follow handles.
// Also covers private RADV cache modes after the renderer has shut down.
using PermissionProgress = void(*)();
void repair_generated_permissions(PermissionProgress progress = nullptr);
// Metadata-only FTP management repair for the validated native title root.
// Does not permit game/assets content writes through the runtime backend.
void repair_title_permissions(PermissionProgress progress = nullptr);
bool inspect(const path&, struct stat&, std::error_code&);
path weakly_canonical(const path&);
path weakly_canonical(const path&, std::error_code&);
path canonical(const path& p);
path absolute(const path& p);
path absolute(const path& p, std::error_code&);
bool copy_file(const path&, const path&, copy_options, std::error_code&);
void copy(const path&, const path&, copy_options, std::error_code&);
bool exists(const path&, std::error_code&);
bool exists(const path&);
bool is_directory(const path&, std::error_code&);
bool is_directory(const path&);
bool is_regular_file(const path&, std::error_code&);
bool is_regular_file(const path&);
std::uintmax_t file_size(const path&, std::error_code&);
std::uintmax_t file_size(const path&);
bool create_directories(const path&, std::error_code&);
bool create_directories(const path&);
bool create_directory(const path&, std::error_code&);
void rename(const path&, const path&, std::error_code&);
void rename(const path&, const path&);
bool remove(const path&, std::error_code&);
bool remove(const path&);
std::uintmax_t remove_all(const path&, std::error_code&);
path relative(const path&, const path&, std::error_code&);
path relative(const path&, const path&);
path read_symlink(const path&, std::error_code&);
file_status symlink_status(const path&, std::error_code&);
bool equivalent(const path&, const path&, std::error_code&);
space_info space(const path&, std::error_code&);
file_time_type last_write_time(const path&, std::error_code&);
void last_write_time(const path&, file_time_type, std::error_code&);

class directory_entry {
    std::filesystem::path name_;
public:
    directory_entry() = default;
    explicit directory_entry(std::filesystem::path name): name_(std::move(name)) {}
    const std::filesystem::path& path() const { return name_; }
    bool is_regular_file(std::error_code& ec) const { return game_files::is_regular_file(name_, ec); }
    bool is_directory(std::error_code& ec) const { return game_files::is_directory(name_, ec); }
    bool is_regular_file() const { return game_files::is_regular_file(name_); }
    bool is_directory() const { return game_files::is_directory(name_); }
    bool is_symlink(std::error_code& ec) const { struct stat st{}; game_files::inspect(name_, st, ec); return false; }
    bool is_symlink() const { std::error_code ec; struct stat st{}; if(!game_files::inspect(name_,st,ec))throw filesystem_error("inspect entry",name_,ec); return false; }
    std::uintmax_t file_size(std::error_code& ec) const { return game_files::file_size(name_, ec); }
    std::uintmax_t file_size() const { return game_files::file_size(name_); }
    file_time_type last_write_time(std::error_code& ec) const { return game_files::last_write_time(name_, ec); }
};
// Snapshot names only. Metadata and subsequent opens always revalidate; no
// symlink entry is traversed. Depth and entry count are bounded.
class directory_iterator {
protected:
    std::shared_ptr<std::vector<directory_entry>> entries_;
    std::size_t index_{};
    bool skip_children_{};
    directory_iterator(const path&, bool recursive, std::error_code&);
public:
    directory_iterator() = default;
    directory_iterator(const path& p, std::error_code& ec): directory_iterator(p, false, ec) {}
    directory_iterator(const path& p, directory_options, std::error_code& ec): directory_iterator(p, false, ec) {}
    explicit directory_iterator(const path&);
    const directory_entry& operator*() const { return (*entries_)[index_]; }
    const directory_entry* operator->() const { return &**this; }
    directory_iterator& increment(std::error_code& ec) { ec.clear(); return ++*this; }
    directory_iterator& operator++();
    bool operator==(const directory_iterator& b) const { return entries_ == b.entries_ && index_ == b.index_; }
    bool operator!=(const directory_iterator& b) const { return !(*this == b); }
    directory_iterator begin() const { return *this; }
    directory_iterator end() const { return {}; }
    void disable_recursion_pending() { skip_children_ = true; }
};
class recursive_directory_iterator: public directory_iterator {
public:
    recursive_directory_iterator() = default;
    explicit recursive_directory_iterator(const path&);
    recursive_directory_iterator(const path& p, std::error_code& ec): directory_iterator(p, true, ec) {}
    recursive_directory_iterator(const path& p, directory_options, std::error_code& ec): directory_iterator(p, true, ec) {}
};

class fd_buffer final: public std::streambuf {
    int fd_{-1};
    char input_[8192]{};
    bool rewind_input();
protected:
    int_type underflow() override;
    std::streamsize xsgetn(char*, std::streamsize) override;
    std::streamsize xsputn(const char*, std::streamsize) override;
    int_type overflow(int_type) override;
    pos_type seekoff(off_type, std::ios::seekdir, std::ios::openmode) override;
    pos_type seekpos(pos_type, std::ios::openmode) override;
    int sync() override;
public:
    ~fd_buffer() override;
    bool open(const path&, std::ios::openmode);
    bool close();
    bool is_open() const { return fd_ >= 0; }
};
class fstream: public std::iostream {
    fd_buffer buffer_;
public:
    fstream(): std::iostream(nullptr) { rdbuf(&buffer_); clear(); }
    explicit fstream(const path& p, std::ios::openmode m = std::ios::in | std::ios::out): fstream() { open(p, m); }
    void open(const path& p, std::ios::openmode m = std::ios::in | std::ios::out) { clear(); if (!buffer_.open(p, m)) setstate(std::ios::failbit); }
    void close() { if (!buffer_.close()) setstate(std::ios::failbit); }
    bool is_open() const { return buffer_.is_open(); }
};
class ifstream: public fstream {
public:
    ifstream() = default;
    explicit ifstream(const path& p, std::ios::openmode m = std::ios::in): fstream(p, m | std::ios::in) {}
    void open(const path& p, std::ios::openmode m = std::ios::in) { fstream::open(p, m | std::ios::in); }
};
class ofstream: public fstream {
public:
    ofstream() = default;
    explicit ofstream(const path& p, std::ios::openmode m = std::ios::out): fstream(p, m | std::ios::out) {}
    void open(const path& p, std::ios::openmode m = std::ios::out) { fstream::open(p, m | std::ios::out); }
};
FILE* fopen(const path&, const char* mode);
}
