// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_mods.hpp"
#include "game_files.hpp"
#include "game_web_protocol.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mod_ini.hpp"
#include <algorithm>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <sstream>
#include <unistd.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 1024
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace yakumo::game_mods {
namespace {
namespace mods = mhp3rd::mods;
using game_web::quote;
using Fields = std::map<std::string,std::string>;
using Path = game_files::path;
[[noreturn]] void fail(const char* why) { throw std::runtime_error(why); }
struct Fd {
    int value;
    ~Fd() { if(value>=0) ::close(value); }
    void close() { const int fd=value; value=-1; if(::close(fd)) fail("Cannot close mod preferences"); }
};
Path folder() { return game_files::root()/"yakumo/mods"; }
Path choices() { return game_files::root()/"yakumo/config/mods.ini"; }
bool safe_id(const std::string& id) {
    return !id.empty() && id.size()<=128 && id.front()!='.' && mods::trim(id)==id &&
        std::none_of(id.begin(),id.end(),[](unsigned char c){return c<32 || c==127 || c=='[' || c==']' || c=='/' || c=='\\';});
}
std::string read(const Path& path,std::size_t limit,bool optional=false) {
    Fd fd{game_files::open_file(path,O_RDONLY)};
    if(fd.value<0) { if(optional && errno==ENOENT) return {}; fail("Cannot read mod file safely"); }
    struct stat st{};
    // PS5 reports zero links for normal title files (recorded console evidence).
    // Match the validated file adapter: reject reported multiple links, not zero.
    if(::fstat(fd.value,&st) || !S_ISREG(st.st_mode) || st.st_nlink>1 || st.st_size<0 ||
       static_cast<std::uint64_t>(st.st_size)>limit) fail("Unsafe or oversized mod file");
    std::string out; char bytes[8192];
    for(;;) {
        const auto n=::read(fd.value,bytes,sizeof(bytes));
        if(n<0 && errno==EINTR) continue;
        if(n<0) fail("Cannot read mod file");
        if(!n) break;
        out.append(bytes,n); if(out.size()>limit) fail("Mod file exceeds limit");
    }
    fd.close(); return out;
}
std::string png(const Path& path) {
    const auto bytes=read(path,1024*1024);
    constexpr unsigned char signature[]={137,80,78,71,13,10,26,10};
    if(bytes.size()<24 || !std::equal(std::begin(signature),std::end(signature),
        reinterpret_cast<const unsigned char*>(bytes.data()))) fail("Invalid PNG preview");
    int w{},h{},channels{};
    const auto* data=reinterpret_cast<const unsigned char*>(bytes.data());
    if(!stbi_info_from_memory(data,bytes.size(),&w,&h,&channels) || w<1 || h<1 || w>1024 || h>1024)
        fail("Preview dimensions exceed limit");
    auto* decoded=stbi_load_from_memory(data,bytes.size(),&w,&h,&channels,4);
    if(!decoded) fail("Invalid PNG preview");
    stbi_image_free(decoded); return bytes;
}
// Extend display metadata and reject unsafe inputs; interpretation stays in the engine.
class Format final: public mods::ModFormat {
    mods::p3rd::ModFolderFormat fallback_;
    const mods::ModFormat& engine_;
public:
    Format(): engine_(mods::session()?mods::session()->library().format():fallback_) {}
    std::string file_name(mods::FileId f) const override { return engine_.file_name(f); }
    std::optional<mods::FileId> parse_file(const std::string& s) const override { return engine_.parse_file(s); }
    std::optional<mods::Mod> read(const Path& path) const override {
        const auto id=mods::to_utf8(path.filename());
        if(!safe_id(id)) return {};
        try {
            // Find case-insensitively, just like the engine. Never traverse links.
            std::string metadata;
            std::error_code ec;
            if(game_files::is_regular_file(path/"mod.ini",ec)) metadata=game_mods::read(path/"mod.ini",65536);
            else for(const auto& entry:game_files::directory_iterator(path,ec))
                if(mods::iequals(mods::to_utf8(entry.path().filename()),"mod.ini")) metadata=game_mods::read(entry.path(),65536);
            auto mod=engine_.read(path); if(!mod) return {};
            const auto ini=mods::IniFile::parse(metadata);
            if(mod->author.empty()) mod->author=ini.get("Author","Name");
            for(const auto& change:mod->changes) {
                struct stat st{}; std::error_code ec;
                if(!game_files::inspect(change.source,st,ec) || !S_ISREG(st.st_mode) || st.st_nlink>1)
                    mod->unusable="Unsafe or missing replacement file";
            }
            if(ini.find("MOD INFO","Script")) mod->unusable="PSP manager scripts are not supported";
            return mod;
        } catch(const std::exception& error) {
            std::cerr<<"[ps5-mods] reader=mods-r1 folder="<<quote(id)<<" error="<<quote(error.what())<<'\n';
            mods::Mod mod; mod.name=id; mod.unusable="Unsafe, invalid or oversized mod metadata"; return mod;
        }
    }
};
struct Snapshot {
    Format format;
    mods::ModLibrary library{format};
    std::string disk;
    Fields reasons;
    Snapshot(): disk(read(choices(),1024*1024,true)) {
        library.load_choices(choices());
        library.scan(folder());
        if(library.mods().size()>128) fail("More than 128 mods; reduce the folder count");
        for(const auto& mod:library.mods()) {
            auto reason=mod.unusable;
            const auto c=library.choice(mod.id);
            if(reason.empty() && !mod.slots.empty() &&
               (c.slots.size()!=mod.slots.size() || std::any_of(c.slots.begin(),c.slots.end(),[](auto s){return !s;})))
                reason="Equipment targets must already be configured";
            for(const auto& dependency:mod.depends)
                if(reason.empty() && !mods::iequals(dependency,"file_replacer") && !library.find(dependency))
                    reason="Missing dependency: "+dependency;
            for(const auto& member:mod.members)
                if(reason.empty() && !library.find(member)) reason="Missing pack member: "+member;
            reasons[mod.id]=reason;
        }
        // A dependency on an unusable mod is unusable too, including chains.
        for(std::size_t n=0;n<library.mods().size();++n) for(const auto& mod:library.mods()) {
            for(const auto& dep:mod.depends) if(!mods::iequals(dep,"file_replacer") && reasons[mod.id].empty() && reasons.count(dep) && !reasons[dep].empty())
                reasons[mod.id]="Incompatible dependency: "+dep;
            for(const auto& member:mod.members) if(reasons[mod.id].empty() && reasons.count(member) && !reasons[member].empty())
                reasons[mod.id]="Incompatible pack member: "+member;
        }
        if(read(choices(),1024*1024,true)!=disk) fail("Mod preferences changed during detection; retry");
    }
    std::string revision() const {
        std::uint64_t h=14695981039346656037ull;
        const auto add=[&](const std::string& s){for(unsigned char c:s){h^=c;h*=1099511628211ull;} h^=0xff;h*=1099511628211ull;};
        add(disk);
        for(const auto& mod:library.mods()) {
            add(mod.id); add(mod.name); add(mod.author); add(reasons.at(mod.id));
            std::error_code listing_error;
            const auto entries=game_files::directory_iterator(mod.folder,listing_error);
            add(std::to_string(listing_error.value()));
            for(const auto& entry:entries) {
                // Covers metadata edits and FTP replacement without hashing large assets.
                struct stat st{};std::error_code ec;
                if(game_files::inspect(entry.path(),st,ec)) {
                    add(entry.path().string());add(std::to_string(st.st_size));add(std::to_string(st.st_mtime));
                    if(mods::iequals(entry.path().filename().string(),"mod.ini")) {
                        try{add(read(entry.path(),65536));}catch(...){add("unreadable metadata");}
                    }
                }
            }
            for(const auto& c:mod.changes) {add(c.source.string());add(std::to_string(c.file));}
        }
        return std::to_string(h);
    }
    std::string json() const {
        std::string out="{\"ok\":true,\"revision\":"+quote(revision())+",\"master\":"+(library.master()?"true":"false")+",\"mods\":[";
        bool first=true;
        for(const auto& mod:library.mods()) {
            if(!first) out+=',';first=false;
            bool image=false;try{if(!mod.preview.empty()){png(mod.preview);image=true;}}catch(...){}
            out+="{\"id\":"+quote(mod.id)+",\"name\":"+quote(mod.name)+",\"author\":"+quote(mod.author)+
                ",\"enabled\":"+(library.enabled(mod.id)&&reasons.at(mod.id).empty()?"true":"false")+",\"reason\":"+quote(reasons.at(mod.id))+
                ",\"preview\":"+(image?"true":"false")+",\"changes\":[";
            bool next=false;
            for(const auto& c:mod.changes) {if(next)out+=',';next=true;
                out+="{\"target\":"+quote(format.file_name(c.file))+",\"kind\":"+quote(c.kind==mods::FileChange::Kind::Replace?"replace":"patch")+"}";}
            for(const auto& slot:library.choice(mod.id).slots) if(slot) {if(next)out+=',';next=true;
                out+="{\"target\":"+quote(format.file_name(*slot))+",\"kind\":\"replace\"}";}
            out+="],\"depends\":[";next=false;
            for(const auto& dep:mod.depends) {if(next)out+=',';next=true;out+=quote(dep);}
            out+="],\"members\":[";next=false;
            for(const auto& member:mod.members) {if(next)out+=',';next=true;out+=quote(member);}
            out+="]}";
        }
        return out+"]}";
    }
};
// Preserve unknown entries and choices for absent mods. Only owned keys change.
std::string merged(const Snapshot& s,const Fields& fields,const std::vector<std::string>& order) {
    // Retain original spelling, quotes, comments and unknown/absent sections.
    std::istringstream input(s.disk);std::string line,section,out;std::set<std::string> emitted;
    const auto additions=[&](const std::string& current) {
        if(mods::iequals(current,"general") && emitted.insert("general").second) return std::string("enabled=1\n");
        for(std::size_t n=0;n<order.size();++n) if(mods::iequals(current,"mod "+order[n]) && emitted.insert("mod "+order[n]).second)
            return "enabled="+fields.at("enabled."+order[n])+"\nrank="+std::to_string(order.size()-n)+"\n";
        return std::string{};
    };
    while(std::getline(input,line)) {
        auto trimmed=mods::trim(line);
        if(trimmed.starts_with("\xEF\xBB\xBF")) trimmed.erase(0,3);
        if(trimmed.starts_with('[')) {
            section=mods::trim(trimmed.substr(1,trimmed.find(']')-1));
            out+=line+'\n'+additions(section);continue;
        } else if(!trimmed.empty() && trimmed.front()!=';' && trimmed.front()!='#') {
            const auto key=mods::trim(trimmed.substr(0,trimmed.find('=')));
            const bool owned=std::any_of(order.begin(),order.end(),[&](const auto& id){return mods::iequals(section,"mod "+id);});
            if((owned && (mods::iequals(key,"enabled") || mods::iequals(key,"rank"))) ||
               (mods::iequals(section,"general") && mods::iequals(key,"enabled"))) continue;
        }
        out+=line+'\n';
    }
    if(!emitted.count("general")) out+="\n[general]\n"+additions("general");
    for(const auto& id:order) if(!emitted.count("mod "+id)) out+="\n[mod "+id+"]\n"+additions("mod "+id);
    if(out.size()>1024*1024) fail("Mod preferences exceed 1 MiB");
    return out;
}
}
std::string catalog() {
    static bool announced=false;
    if(!announced){std::cout<<"[ps5-mods] reader=mods-r1 link-policy=reject-multiple apply=restart\n";announced=true;}
    return Snapshot{}.json();
}
std::string preview(const std::string& id) {
    if(!safe_id(id)) fail("Invalid mod id");
    Snapshot s;const auto* mod=s.library.find(id);
    if(!mod || mod->preview.empty()) fail("Preview unavailable");
    return png(mod->preview);
}
std::string save(const Fields& fields) {
    Snapshot s;
    const auto rev=fields.find("_revision");
    if(rev==fields.end() || rev->second!=s.revision()) fail("Mods changed on disk. Reopen Mods before saving.");
    const auto count=s.library.mods().size();
    if(fields.size()!=1+2*count) fail("Invalid mod preferences");
    std::vector<std::string> order(count); std::set<std::string> seen;
    for(std::size_t n=0;n<count;++n) {
        const auto it=fields.find("order."+std::to_string(n));
        if(it==fields.end() || !s.library.find(it->second) || !seen.insert(it->second).second) fail("Invalid mod order");
        order[n]=it->second;
        const auto on=fields.find("enabled."+it->second);
        if(on==fields.end() || (on->second!="0" && on->second!="1")) fail("Invalid mod switch");
        if(on->second=="1" && !s.reasons.at(it->second).empty()) fail("An incompatible mod cannot be enabled");
    }
    for(const auto& mod:s.library.mods()) if(fields.at("enabled."+mod.id)=="1") {
        for(const auto& dep:mod.depends) if(!mods::iequals(dep,"file_replacer") && fields.at("enabled."+dep)!="1")
            fail("Enable the required dependency too");
        for(const auto& member:mod.members) if(fields.at("enabled."+member)!="1") fail("Enable all pack members too");
    }
    const auto output=merged(s,fields,order);
    game_files::create_directories(choices().parent_path());
    Fd destination{game_files::open_file(choices(),O_RDWR)};
    if(destination.value<0 && errno!=ENOENT) fail("Mod preferences are not writable");
    if(destination.value>=0) destination.close();
    const auto partial=choices().parent_path()/"mods.ini.part";
    Fd fd{game_files::open_file(partial,O_WRONLY|O_CREAT|O_EXCL)};
    if(fd.value<0) fail("Cannot create mods.ini.part; existing files were kept");
    try {
        std::size_t at=0;
        while(at<output.size()) {
            const auto n=::write(fd.value,output.data()+at,output.size()-at);
            if(n<0 && errno==EINTR) continue;
            if(n<=0) fail("Cannot write mod preferences");
            at+=n;
        }
        if(::fsync(fd.value)) fail("Cannot synchronize mod preferences");
        fd.close();
        if(Snapshot{}.revision()!=rev->second) fail("Mods changed during save; reopen Mods");
        game_files::rename(partial,choices());
    } catch(...) {std::error_code ec;game_files::remove(partial,ec);throw;}
    return catalog();
}
}
