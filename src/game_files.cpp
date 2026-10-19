// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_files.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace yakumo::game_files {
namespace {
path title_root;
void error(std::error_code& ec, int code) { ec = std::error_code(code, std::generic_category()); errno = code; }
void mode_metadata(const path& name,int fd,mode_t requested,const struct stat& st,const char* action){
#if !YAKUMO_NATIVE
    // DrvFS may expose virtual 0777 for every file even after chmod. Preserve
    // exact diagnostics in the checked/native adapter without flooding host runs.
    if(name!=title_root)return;
#endif
    std::cerr<<"[game-permissions] action="<<action<<" path="<<name.string()<<" fd="<<fd
        <<" requested="<<std::oct<<requested<<" mode="<<st.st_mode<<std::dec
        <<" type="<<(S_ISDIR(st.st_mode)?"directory":S_ISREG(st.st_mode)?"regular":"other")
        <<" uid="<<st.st_uid<<" gid="<<st.st_gid<<" euid="<<::geteuid()<<" egid="<<::getegid()
        <<" nlink="<<st.st_nlink<<" dev="<<st.st_dev<<" ino="<<st.st_ino<<'\n';
}
int set_mode(const path& name,int fd,mode_t mode){
    struct stat before{};
    if(::fstat(fd,&before)){
        const int saved=errno;std::cerr<<"[game-permissions] FAIL operation=fstat-before path="<<name.string()<<" fd="<<fd<<" errno="<<saved<<'\n';errno=saved;return -1;
    }
    // Validate even on the no-change path. Special bits are part of the mode
    // contract: an otherwise writable sticky directory is not exactly 0777.
    const bool program = name==title_root/"eboot.bin" || name==title_root/"sce_module/libc.prx";
    if((program&&!S_ISREG(before.st_mode))||(!S_ISDIR(before.st_mode)&&!S_ISREG(before.st_mode))||
       (mode!=(S_ISDIR(before.st_mode)||program?0777:0666))||
       (S_ISREG(before.st_mode)&&before.st_nlink>1)){
        mode_metadata(name,fd,mode,before,"reject");errno=EPERM;return -1;
    }
    const bool exact=(before.st_mode&07777)==mode;
    if(exact){
        // Log root evidence, but avoid per-open noise for normal game writes.
        if(name==title_root||program)mode_metadata(name,fd,mode,before,"already-correct");
        return 0;
    }
    mode_metadata(name,fd,mode,before,"change-required");
    const int changed=::fchmod(fd,mode);
    if(changed){
        const int saved=errno;
        std::cerr<<"[game-permissions] FAIL operation=fchmod path="<<name.string()<<" fd="<<fd
            <<" requested="<<std::oct<<mode<<std::dec<<" rc="<<changed<<" errno="<<saved<<'\n';
        if(saved==EPERM&&before.st_uid!=::geteuid())
            std::cerr<<"[game-permissions] prerequisite=owner-side-metadata-repair owner_uid="<<before.st_uid
                <<" app_euid="<<::geteuid()<<" tool=payload.elf"
                <<" preserve_contents=1 reinstall_required=0\n";
        errno=saved;return -1;
    }
#if YAKUMO_NATIVE
    struct stat st{};
    if(::fstat(fd,&st)){const int saved=errno;std::cerr<<"[game-permissions] FAIL operation=fstat fd="<<fd<<" errno="<<saved<<'\n';errno=saved;return -1;}
    if(st.st_dev!=before.st_dev||st.st_ino!=before.st_ino||
       (st.st_mode&S_IFMT)!=(before.st_mode&S_IFMT)||(st.st_mode&07777)!=mode||
       (S_ISREG(st.st_mode)&&st.st_nlink>1)){
        mode_metadata(name,fd,mode,st,"readback-rejected");errno=EACCES;return -1;
    }
#endif
    return 0;
}
bool spelling(const path& p, std::error_code& ec) {
    const auto text = p.string();
    if (text.empty() || text.size() >= 1024 || text.front() != '/' || text.find('\\') != text.npos ||
        text.find('\0') != text.npos || text.find("//") != text.npos || (text.size() > 1 && text.back() == '/')) {
        error(ec, EINVAL); return false;
    }
    for (const auto& component: p) if (component == "." || component == "..") { error(ec, EPERM); return false; }
    return true;
}
bool directories(const path& p, bool create, std::error_code& ec) {
    path prefix;
    for (const auto& component: p) {
        prefix /= component;
        if (prefix == "/") continue;
        int fd = ::open(prefix.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0 && errno == ENOENT && create) {
            if (::mkdir(prefix.c_str(), 0777) != 0 && errno != EEXIST) { error(ec, errno); return false; }
            fd = ::open(prefix.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        }
        if (fd < 0) { error(ec, errno); return false; }
        struct stat st{};
        const int rc = ::fstat(fd, &st), saved = errno;
        std::error_code allowed;
        if (!rc && S_ISDIR(st.st_mode) && create && permitted(prefix,true,allowed) && set_mode(prefix,fd,0777)) {
            const int failure=errno; ::close(fd); error(ec,failure); return false;
        }
        const int closed = ::close(fd);
        if (rc != 0 || !S_ISDIR(st.st_mode) || closed != 0) { error(ec, rc ? saved : closed ? errno : ENOTDIR); return false; }
    }
    ec.clear(); return true;
}
void raise(const char* operation, const path& p, const std::error_code& ec) {
    if (ec) throw filesystem_error(operation, p, ec);
}
}
const path& root() { return title_root; }
void set_root(const path& p) {
    std::error_code ec;
    if (!spelling(p, ec) || !directories(p, false, ec)) raise("title-root", p, ec);
    if (!title_root.empty() && title_root != p) throw std::logic_error("Title root already fixed");
    title_root = p;
}
bool permitted(const path& p, bool write, std::error_code& ec) {
    ec.clear();
    if (!spelling(p, ec)) return false;
    const auto base = title_root.string(), text = p.string();
    if (base.empty() || (text != base && !text.starts_with(base + '/'))) { error(ec, EPERM); return false; }
    if (write) {
        const char* writable[] = {"config", "saves", "cache", "logs"};
        bool allowed = false;
        for (const auto* folder: writable) {
            const auto prefix = base + "/yakumo/" + folder;
            allowed |= text == prefix || text.starts_with(prefix + '/');
        }
        if (!allowed) { error(ec, EROFS); return false; }
    }
    return true;
}
int open_file(const path& p, int flags, unsigned mode) {
    std::error_code ec;
    const bool write = (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND));
    if (!permitted(p, write, ec) || !directories(p.parent_path(), false, ec)) return -1;
    // Delay truncation until the opened handle has been checked.
    const int fd = ::open(p.c_str(), (flags & ~O_TRUNC) | O_NOFOLLOW | O_NONBLOCK, mode);
    if (fd < 0) return -1;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || (!S_ISREG(st.st_mode) && !(flags & O_DIRECTORY)) ||
        ((flags & O_DIRECTORY) && !S_ISDIR(st.st_mode))) {
        const int saved = errno ? errno : EINVAL; ::close(fd); errno = saved; return -1;
    }
    if (write && S_ISREG(st.st_mode) && st.st_nlink>1) { ::close(fd); errno=EPERM; return -1; }
    if (write && set_mode(p,fd,(flags & O_DIRECTORY)?0777:0666)) { const int saved=errno; ::close(fd); errno=saved; return -1; }
    if ((flags & O_TRUNC) && ::ftruncate(fd, 0) != 0) { const int saved=errno; ::close(fd); errno=saved; return -1; }
    return fd;
}
bool inspect(const path& p, struct stat& st, std::error_code& ec) {
    if (!permitted(p, false, ec) || !directories(p.parent_path(), false, ec)) return false;
    const int fd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { error(ec, errno); return false; }
    const int rc = ::fstat(fd, &st), saved = errno;
    const int closed = ::close(fd);
    if (rc || closed) { error(ec, rc ? saved : errno); return false; }
    ec.clear(); return true;
}
path weakly_canonical(const path& p, std::error_code& ec) {
    if (!permitted(p, false, ec)) return {};
    path prefix;
    for (const auto& component: p) {
        prefix /= component;
        if (prefix == "/") continue;
        int fd = ::open(prefix.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            if (errno == ENOENT) { ec.clear(); return p; }
            error(ec, errno); return {};
        }
        struct stat st{};
        const int rc = ::fstat(fd, &st), saved = errno;
        ::close(fd);
        if (rc || (prefix != p && !S_ISDIR(st.st_mode))) { error(ec, rc ? saved : ENOTDIR); return {}; }
    }
    ec.clear(); return p;
}
path weakly_canonical(const path& p) { std::error_code ec; auto result=game_files::weakly_canonical(p,ec); raise("canonical",p,ec); return result; }
path canonical(const path& p) { std::error_code ec; struct stat st{}; inspect(p,st,ec); raise("canonical",p,ec); return p; }
path absolute(const path& p) { std::error_code ec; if (!permitted(p,false,ec)) raise("absolute",p,ec); return p; }
path absolute(const path& p,std::error_code& ec) { return permitted(p,false,ec)?p:path{}; }
bool copy_file(const path& a,const path& b,copy_options options,std::error_code& ec) {
    struct stat source{},destination{};
    if(!inspect(a,source,ec)||!permitted(b,true,ec))return false;
    std::error_code other;
    if(inspect(b,destination,other)) {
        if(source.st_dev==destination.st_dev&&source.st_ino==destination.st_ino){error(ec,EINVAL);return false;}
        if((options&copy_options::skip_existing)!=copy_options::none){ec.clear();return false;}
        if((options&copy_options::overwrite_existing)==copy_options::none){error(ec,EEXIST);return false;}
    } else if(other.value()!=ENOENT){ec=other;return false;}
    int input=open_file(a,O_RDONLY);if(input<0){error(ec,errno);return false;}
    int output=open_file(b,O_WRONLY|O_CREAT|((options&copy_options::overwrite_existing)!=copy_options::none?O_TRUNC:O_EXCL));
    if(output<0){int saved=errno;::close(input);error(ec,saved);return false;}
    char buffer[8192];bool ok=true;
    while(ok){ssize_t n=::read(input,buffer,sizeof(buffer));if(n<0&&errno==EINTR)continue;if(n<0){error(ec,errno);ok=false;break;}if(!n)break;ssize_t done=0;while(done<n){ssize_t m=::write(output,buffer+done,n-done);if(m<0&&errno==EINTR)continue;if(m<=0){error(ec,errno?errno:EIO);ok=false;break;}done+=m;}}
    if(ok&&::fsync(output)!=0){error(ec,errno);ok=false;}::close(input);if(::close(output)!=0&&ok){error(ec,errno);ok=false;}return ok;
}
void copy(const path& a,const path& b,copy_options options,std::error_code& ec){
    struct stat st{};if(!inspect(a,st,ec)||!permitted(b,true,ec))return;
    if(S_ISREG(st.st_mode)){game_files::copy_file(a,b,options,ec);return;}
    if(!S_ISDIR(st.st_mode)){error(ec,EINVAL);return;}
    game_files::create_directories(b,ec);if(ec)return;
    if((options&copy_options::recursive)==copy_options::none)return;
    recursive_directory_iterator it(a,ec);if(ec)return;
    for(const auto& item:it){const auto dest=b/item.path().lexically_relative(a);if(item.is_directory(ec))game_files::create_directories(dest,ec);else if(!ec)game_files::copy_file(item.path(),dest,options,ec);if(ec)return;}
}
bool exists(const path& p, std::error_code& ec) { struct stat st{}; bool ok=inspect(p,st,ec); if (ec.value()==ENOENT) ec.clear(); return ok; }
bool exists(const path& p) { std::error_code ec; bool ok=game_files::exists(p,ec); raise("exists",p,ec); return ok; }
bool is_directory(const path& p, std::error_code& ec) { struct stat st{}; bool ok=inspect(p,st,ec); if(ec.value()==ENOENT)ec.clear(); return ok&&S_ISDIR(st.st_mode); }
bool is_directory(const path& p) { std::error_code ec; bool ok=game_files::is_directory(p,ec); raise("is-directory",p,ec); return ok; }
bool is_regular_file(const path& p, std::error_code& ec) { struct stat st{}; bool ok=inspect(p,st,ec); if(ec.value()==ENOENT)ec.clear(); return ok&&S_ISREG(st.st_mode); }
bool is_regular_file(const path& p) { std::error_code ec; bool ok=game_files::is_regular_file(p,ec); raise("is-file",p,ec); return ok; }
std::uintmax_t file_size(const path& p, std::error_code& ec) { struct stat st{}; if(!inspect(p,st,ec))return UINTMAX_MAX; if(!S_ISREG(st.st_mode)){error(ec,EINVAL);return UINTMAX_MAX;}return st.st_size; }
std::uintmax_t file_size(const path& p) { std::error_code ec; auto n=game_files::file_size(p,ec);raise("file-size",p,ec);return n; }
bool create_directories(const path& p, std::error_code& ec) { if(!permitted(p,true,ec))return false; const bool was=game_files::is_directory(p,ec); if(ec)return false; return directories(p,true,ec)&&!was; }
bool create_directories(const path& p) { std::error_code ec; bool n=game_files::create_directories(p,ec);raise("mkdir",p,ec);return n; }
bool create_directory(const path& p, std::error_code& ec) { if(!permitted(p,true,ec)||!directories(p.parent_path(),false,ec))return false;const bool was=game_files::is_directory(p,ec);if(ec)return false;return directories(p,true,ec)&&!was; }
void rename(const path& a, const path& b, std::error_code& ec) {
    struct stat st{};
    if(!permitted(a,true,ec)||!permitted(b,true,ec)||!inspect(a,st,ec)||!directories(b.parent_path(),false,ec))return;
    std::error_code destination_error;
    inspect(b,st,destination_error);
    if(destination_error && destination_error.value()!=ENOENT){ec=destination_error;return;}
    if(::rename(a.c_str(),b.c_str())!=0)error(ec,errno);else ec.clear();
}
void rename(const path& a,const path& b){std::error_code ec;game_files::rename(a,b,ec);raise("rename",a,ec);}
bool remove(const path& p,std::error_code& ec){struct stat st{};if(!permitted(p,true,ec)||!inspect(p,st,ec)){if(ec.value()==ENOENT)ec.clear();return false;}if((S_ISDIR(st.st_mode) ? ::rmdir(p.c_str()) : ::unlink(p.c_str()))!=0){error(ec,errno);return false;}ec.clear();return true;}
bool remove(const path& p){std::error_code ec;bool n=game_files::remove(p,ec);raise("remove",p,ec);return n;}
path relative(const path& p,const path& base,std::error_code& ec){if(!permitted(p,false,ec)||!permitted(base,false,ec))return {};ec.clear();return p.lexically_relative(base);}
path relative(const path& p,const path& base){std::error_code ec;auto result=game_files::relative(p,base,ec);raise("relative",p,ec);return result;}
path read_symlink(const path& p,std::error_code& ec){(void)p;error(ec,ENOTSUP);return {};}
file_status symlink_status(const path& p,std::error_code& ec){struct stat st{};if(!inspect(p,st,ec))return file_status(file_type::none);return file_status(S_ISDIR(st.st_mode)?file_type::directory:S_ISREG(st.st_mode)?file_type::regular:file_type::unknown);}
bool equivalent(const path& a,const path& b,std::error_code& ec){struct stat x{},y{};return inspect(a,x,ec)&&inspect(b,y,ec)&&x.st_dev==y.st_dev&&x.st_ino==y.st_ino;}
space_info space(const path& p,std::error_code& ec){(void)p;error(ec,ENOTSUP);return {UINTMAX_MAX,UINTMAX_MAX,UINTMAX_MAX};}
file_time_type last_write_time(const path& p,std::error_code& ec){struct stat st{};if(!inspect(p,st,ec))return file_time_type::min();return file_time_type::clock::from_sys(std::chrono::system_clock::from_time_t(st.st_mtime));}
void last_write_time(const path& p,file_time_type,std::error_code& ec){if(permitted(p,true,ec))error(ec,ENOTSUP);}
namespace {
void repair_tree(const path& p,unsigned depth,bool whole_title,PermissionProgress progress,std::size_t& visited){
    std::error_code ec;
    if(depth>16)throw filesystem_error("permission repair depth",p,std::error_code(EOVERFLOW,std::generic_category()));
    if(++visited>65536)throw filesystem_error("permission repair count",p,std::error_code(EOVERFLOW,std::generic_category()));
    if(!permitted(p,!whole_title,ec)||!directories(p.parent_path(),false,ec))raise("permission repair",p,ec);
    const int fd=::open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){if(errno==ENOENT)return;error(ec,errno);raise("permission repair open",p,ec);}
    struct stat st{};
    if(::fstat(fd,&st)||(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode))){const int saved=errno?errno:EINVAL;::close(fd);error(ec,saved);raise("permission repair type",p,ec);}
    if(S_ISREG(st.st_mode)&&st.st_nlink>1){::close(fd);error(ec,EPERM);raise("permission repair hardlink",p,ec);}
    if(set_mode(p,fd,(S_ISDIR(st.st_mode)||p==title_root/"eboot.bin"||p==title_root/"sce_module/libc.prx")?0777:0666)){const int saved=errno;::close(fd);error(ec,saved);raise("permission repair chmod",p,ec);}
    if(progress)progress(); // Only after this real object passed validation/repair.
    if(!S_ISDIR(st.st_mode)){::close(fd);return;}
    DIR* d=::fdopendir(fd);if(!d){const int saved=errno;::close(fd);error(ec,saved);raise("permission repair list",p,ec);}
    std::vector<path> children;errno=0;
    while(auto* entry=::readdir(d)){if(std::strcmp(entry->d_name,".")&&std::strcmp(entry->d_name,".."))children.push_back(p/entry->d_name);}
    const int saved=errno;::closedir(d);if(saved){error(ec,saved);raise("permission repair enumerate",p,ec);}
    std::sort(children.begin(),children.end());
    for(const auto& child:children)repair_tree(child,depth+1,whole_title,progress,visited);
}
bool collect(const path& p,std::vector<directory_entry>& out,bool recursive,unsigned depth,std::error_code& ec){
    if(depth>16||out.size()>65536){error(ec,EOVERFLOW);return false;}
    if(!permitted(p,false,ec)||!directories(p,false,ec))return false;
    const int fd=::open(p.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0){error(ec,errno);return false;}
    DIR* d=::fdopendir(fd);if(!d){int saved=errno;::close(fd);error(ec,saved);return false;}
    std::vector<path> names;errno=0;
    while(auto* entry=::readdir(d)){if(std::strcmp(entry->d_name,".")&&std::strcmp(entry->d_name,".."))names.push_back(p/entry->d_name);}
    const int saved=errno;::closedir(d);if(saved){error(ec,saved);return false;}
    std::sort(names.begin(),names.end());
    for(const auto& name:names){struct stat st{};if(!inspect(name,st,ec))return false;out.emplace_back(name);if(recursive&&S_ISDIR(st.st_mode)&&!collect(name,out,true,depth+1,ec))return false;}
    return true;
}
}
void repair_generated_permissions(PermissionProgress progress){
    std::size_t visited=0;
    for(const char* folder:{"config","saves","cache","logs"})repair_tree(title_root/"yakumo"/folder,0,false,progress,visited);
}
void repair_title_permissions(PermissionProgress progress){std::size_t visited=0;repair_tree(title_root,0,true,progress,visited);}
directory_iterator::directory_iterator(const path& p,bool recursive,std::error_code& ec){auto data=std::make_shared<std::vector<directory_entry>>();if(collect(p,*data,recursive,0,ec)&&!data->empty())entries_=std::move(data);}
directory_iterator::directory_iterator(const path& p){std::error_code ec;*this=directory_iterator(p,ec);raise("list",p,ec);}
recursive_directory_iterator::recursive_directory_iterator(const path& p){std::error_code ec;auto data=std::make_shared<std::vector<directory_entry>>();if(collect(p,*data,true,0,ec)&&!data->empty())entries_=std::move(data);raise("recursive-list",p,ec);}
directory_iterator& directory_iterator::operator++(){
    if(entries_){
        const auto prefix=(*entries_)[index_].path().string()+'/';
        ++index_;
        if(skip_children_){while(index_<entries_->size()&&(*entries_)[index_].path().string().starts_with(prefix))++index_;skip_children_=false;}
        if(index_>=entries_->size()){entries_.reset();index_=0;}
    }
    return *this;
}
std::uintmax_t remove_all(const path& p,std::error_code& ec){
    struct stat st{};if(!permitted(p,true,ec)||!inspect(p,st,ec)){if(ec.value()==ENOENT)ec.clear();return ec?UINTMAX_MAX:0;}
    std::vector<directory_entry> entries;if(S_ISDIR(st.st_mode)&&!collect(p,entries,true,0,ec))return UINTMAX_MAX;
    std::uintmax_t count=0;for(auto it=entries.rbegin();it!=entries.rend();++it){if(game_files::remove(it->path(),ec))++count;if(ec)return UINTMAX_MAX;}
    if(game_files::remove(p,ec))++count;return ec?UINTMAX_MAX:count;
}
fd_buffer::~fd_buffer(){close();}
bool fd_buffer::open(const path& p,std::ios::openmode mode){
    if(fd_>=0){errno=EBUSY;return false;}
    const bool input=mode&std::ios::in,output=mode&std::ios::out;
    int flags=input&&output?O_RDWR:output?O_WRONLY:O_RDONLY;
    if(output&&(!input||(mode&(std::ios::trunc|std::ios::app))))flags|=O_CREAT;
    if((mode&std::ios::trunc)||(output&&!input&&!(mode&std::ios::app)))flags|=O_TRUNC;
    if(mode&std::ios::app)flags|=O_APPEND;
    fd_=open_file(p,flags);setg(nullptr,nullptr,nullptr);
    if(fd_<0)return false;
    if((mode&std::ios::ate)&&::lseek(fd_,0,SEEK_END)<0){close();return false;}
    return true;
}
bool fd_buffer::close(){if(fd_<0)return true;const int fd=fd_;fd_=-1;setg(nullptr,nullptr,nullptr);return ::close(fd)==0;}
bool fd_buffer::rewind_input(){if(gptr()&&gptr()<egptr()&&::lseek(fd_,gptr()-egptr(),SEEK_CUR)<0)return false;setg(nullptr,nullptr,nullptr);return true;}
fd_buffer::int_type fd_buffer::underflow(){if(gptr()&&gptr()<egptr())return traits_type::to_int_type(*gptr());ssize_t n;do{n=::read(fd_,input_,sizeof(input_));}while(n<0&&errno==EINTR);if(n<=0)return traits_type::eof();setg(input_,input_,input_+n);return traits_type::to_int_type(*gptr());}
std::streamsize fd_buffer::xsgetn(char* out,std::streamsize size){std::streamsize done=0;while(done<size){if(!gptr()||gptr()==egptr()){if(traits_type::eq_int_type(underflow(),traits_type::eof()))break;}const auto n=std::min<std::streamsize>(egptr()-gptr(),size-done);std::memcpy(out+done,gptr(),n);gbump(static_cast<int>(n));done+=n;}return done;}
std::streamsize fd_buffer::xsputn(const char* data,std::streamsize size){if(!rewind_input())return 0;std::streamsize done=0;while(done<size){auto n=::write(fd_,data+done,size-done);if(n<0&&errno==EINTR)continue;if(n<=0)break;done+=n;}return done;}
fd_buffer::int_type fd_buffer::overflow(int_type c){if(traits_type::eq_int_type(c,traits_type::eof()))return traits_type::not_eof(c);const char value=traits_type::to_char_type(c);return xsputn(&value,1)==1?c:traits_type::eof();}
fd_buffer::pos_type fd_buffer::seekoff(off_type off,std::ios::seekdir direction,std::ios::openmode){if(!rewind_input())return pos_type(off_type(-1));const auto n=::lseek(fd_,off,direction==std::ios::beg?SEEK_SET:direction==std::ios::end?SEEK_END:SEEK_CUR);return pos_type(n);}
fd_buffer::pos_type fd_buffer::seekpos(pos_type p,std::ios::openmode m){return seekoff(off_type(p),std::ios::beg,m);}
int fd_buffer::sync(){return rewind_input()?0:-1;}
FILE* fopen(const path& p,const char* mode){
    int flags=mode[0]=='r'?O_RDONLY:O_WRONLY|O_CREAT;
    if(std::strchr(mode,'+'))flags=(flags&~O_ACCMODE)|O_RDWR;
    if(mode[0]=='w')flags|=O_TRUNC;if(mode[0]=='a')flags|=O_APPEND;
    const int fd=open_file(p,flags);if(fd<0)return nullptr;FILE* file=::fdopen(fd,mode);if(!file){const int saved=errno;::close(fd);errno=saved;}return file;
}
}
