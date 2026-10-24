// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_mods.hpp"
#include "game_files.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mod_session.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
namespace fs=std::filesystem;
namespace menu=yakumo::game_mods;
namespace mods=mhp3rd::mods;
bool fail_sync=false,fail_rename=false;
bool ps5_zero_links=false;
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_fstat(int fd,struct stat* st){
 const int rc=__real_fstat(fd,st);
 if(!rc && ps5_zero_links && S_ISREG(st->st_mode) && st->st_nlink==1)st->st_nlink=0;
 return rc;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
extern "C" int __real_rename(const char*,const char*);
extern "C" int __wrap_rename(const char* a,const char* b){if(fail_rename){errno=EIO;return -1;}return __real_rename(a,b);}
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void put(const fs::path& p,const std::string& s){fs::create_directories(p.parent_path());std::ofstream(p,std::ios::binary)<<s;}
std::string get(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::string revision(const std::string& json){auto at=json.find("\"revision\":\"")+12;return json.substr(at,json.find('"',at)-at);}
std::map<std::string,std::string> fields(const std::string& rev){return {{"_revision",rev},{"order.0","alpha"},{"order.1","beta"},{"enabled.alpha","1"},{"enabled.beta","1"}};}
template<class F> void rejected(F fn){bool threw=false;try{fn();}catch(...){threw=true;}check(threw,"unsafe or stale operation must fail");}
int main(int argc,char** argv){try{
 ps5_zero_links=argc>1 && std::string(argv[argc-1])=="--ps5-zero-links";
 const bool supplied=argc>1 && std::string(argv[1])!="--ps5-zero-links";
 const auto root=supplied?fs::current_path()/"out/data/mods-0.12.0/private-fixture":fs::temp_directory_path()/("yakumo-mods-"+std::to_string(getpid()));
 check(!fs::exists(root),"existing fixture preserved");
 const auto directory=root/"yakumo/mods",choices=root/"yakumo/config/mods.ini";
 fs::create_directories(root/"yakumo/config");
 yakumo::game_files::set_root(root);
 if(supplied){
  fs::create_directories(directory);
  for(const auto& entry:fs::directory_iterator(argv[1]))fs::copy(entry.path(),directory/entry.path().filename(),fs::copy_options::recursive);
  const auto json=menu::catalog();
  check(json.find("Lucho")!=json.npos && json.find("0FFD")!=json.npos && json.find("0FEA")!=json.npos,"supplied HD targets and author fallback");
  check(json.find("\"author\":\"\"")!=json.npos,"Buttons has no author");
  check(menu::preview("guild_master_npc").size()==50701 && menu::preview("buttons_nintendo").size()==1449,"supplied valid previews");
  if(fs::exists(directory/"buttons_xbox")) {
   check(json.find("Buttons: Xbox")!=json.npos,"Xbox display name comes from mod.ini");
   check(!menu::preview("buttons_xbox").empty(),"Xbox preview decodes");
  }
  check(!fs::exists(choices),"private detection must not write preferences");
  fs::remove_all(root);std::cout<<"Private supplied metadata, HD variants, unknown author and 166x166 previews PASS\n";return 0;
 }
 check(menu::catalog().find("\"mods\":[]")!=std::string::npos && !fs::exists(choices),"missing folder detection is read-only");
 for(const auto* id:{"alpha","beta"}){
  put(directory/id/"mod.ini","[MOD INFO]\nName="+std::string(id)+"\nType=File\nVersion=BOTH\nFiles=psp.bin\nTarget=0001\nFilesHD=hd.bin\nTargetHD=0002\nDepends=file_replacer\n[Author]\nName=Fallback\n");
  put(directory/id/"psp.bin","psp");put(directory/id/"hd.bin",id);
 }
 auto json=menu::catalog();check(!fs::exists(choices) && json.find("\"enabled\":true")==json.npos,"new mods default off, detection does not save");
 check(json.find("0002")!=json.npos && json.find("0001")==json.npos && json.find("Fallback")!=json.npos,"HD selection and fallback author");
 const auto alpha=get(directory/"alpha/mod.ini");
 put(directory/"alpha/mod.ini",alpha+"[MOD INFO]\nAuthor=Primary\n");
 check(menu::catalog().find("Primary")!=std::string::npos,"primary author takes precedence");
 put(directory/"alpha/mod.ini",alpha);
 put(choices,"[general]\nenabled=0\nunknown=keep\n[mod absent]\nenabled=1\nrank=99\nslot1=0007\n[future]\nvalue=retained\n");
 const auto before=get(choices);
 auto patch=fields(revision(menu::catalog()));
 mods::p3rd::ModFolderFormat format;
 int activations=0;mods::ModSession live(format,{directory,choices,nullptr},{[](const auto&){return true;},[&](const auto&){++activations;}});
 live.start();const auto active=live.active();
 for(auto* flag:{&fail_sync,&fail_rename}){*flag=true;rejected([&]{menu::save(patch);});*flag=false;check(get(choices)==before && !fs::exists(choices.string()+".part"),"failed transaction retains destination and removes only owned temporary");}
 put(choices.string()+".part","foreign");rejected([&]{menu::save(patch);});check(get(choices.string()+".part")=="foreign","exclusive temporary preserves existing file");fs::remove(choices.string()+".part");
 const auto saved=menu::save(patch);check(saved.find("\"enabled\":true")!=saved.npos,"switches persist");
 check(activations==1 && live.active().same_files(active) && live.library().master()==false,"web save never activates or modifies running session");
 const auto ini=mods::IniFile::parse(get(choices));check(ini.get("mod absent","rank")=="99" && ini.get("future","value")=="retained","absent choices and unknown keys preserved");
 mods::ModLibrary restarted(format);restarted.load_choices(choices);restarted.scan(directory);
 check(restarted.resolve().replacements.at(2).mod=="alpha" && restarted.resolve().conflicts.size()==1,"top priority wins after restart");
 rejected([&]{menu::save(patch);});
 patch=fields(revision(saved));patch["order.0"]="beta";patch["order.1"]="alpha";menu::save(patch);
 restarted.load_choices(choices);restarted.scan(directory);check(restarted.resolve().replacements.at(2).mod=="beta","reordered conflict winner persists");
 rejected([&]{menu::preview("../alpha");});
 put(directory/"alpha/preview.png","invalid");rejected([&]{menu::preview("alpha");});
 put(directory/"alpha/preview.png",std::string(1048577,'x'));rejected([&]{menu::preview("alpha");});
 // A valid 1x1 PNG followed by dimension corruption exercises the bounded decoder.
 const unsigned char one[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,100,248,15,0,1,5,1,1,39,24,227,102,0,0,0,0,73,69,78,68,174,66,96,130};
 std::string image(reinterpret_cast<const char*>(one),sizeof(one));put(directory/"alpha/preview.png",image);check(menu::preview("alpha")==image,"valid PNG preview");
 image[18]=8;put(directory/"alpha/preview.png",image);rejected([&]{menu::preview("alpha");});
 fs::remove(directory/"alpha/preview.png");
 fs::create_hard_link(directory/"alpha/hd.bin",directory/"beta/linked");
 check(menu::catalog().find("Unsafe or missing replacement")!=std::string::npos,"hardlinked replacements incompatible");fs::remove(directory/"beta/linked");
 const auto stored=get(choices);fs::create_hard_link(choices,root/"yakumo/config/alias");rejected([&]{menu::catalog();});fs::remove(root/"yakumo/config/alias");check(get(choices)==stored,"hardlinked preferences unchanged");
 put(directory/"beta/mod.ini","[MOD INFO]\nName=Code\nType=Code\n");check(menu::catalog().find("cannot load")!=std::string::npos,"unsupported PSP code reason");
 check(get(directory/"alpha/mod.ini")==alpha && get(directory/"alpha/psp.bin")=="psp" && get(directory/"alpha/hd.bin")=="alpha","mod contents conserved");
 fs::remove_all(root);std::cout<<"Mod metadata, HD variants, draft persistence, conflicts, read-only detection, previews, restart-only and atomic failure contracts PASS\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
