// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_probe.hpp"
#include "psprecomp/runtime.hpp"
#include "hle/hle_common.hpp"
#include "gpu/ge_state.hpp"
#include "gpu/texture_decode.hpp"
#include "save_data/param_sfo.hpp"
#include "save_data/savedata_crypto.hpp"
#include "kernel/iso_image.hpp"
#include "platform/utf8_path.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace yakumo::ps5 {
namespace {
using namespace mhp3rd::gpu;
using namespace mhp3rd::savedata;
constexpr std::uint32_t vertices=0x08010000,list=0x08008000,pixels=0x08020000,palette=0x08024000;
void check(bool ok,const char* contract){if(!ok)throw std::runtime_error(contract);}
std::uint32_t command(unsigned op,std::uint32_t data=0){return (op<<24)|(data&0xffffff);}
void word(std::vector<std::uint8_t>& v,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)v.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));}
std::vector<std::uint8_t> payload(){std::vector<std::uint8_t> v(513);for(unsigned i=0;i<v.size();++i)v[i]=static_cast<std::uint8_t>(i*19+7);return v;}
Block hex(const char* s){Block v{};for(unsigned i=0;i<16;++i){auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};v[i]=static_cast<std::uint8_t>(digit(s[2*i])*16+digit(s[2*i+1]));}return v;}
struct Files {
    std::string directory;std::vector<std::string> owned;bool cleaned{};unsigned sequence{};
    explicit Files(const TitlePaths& paths){
        char name[path_capacity];check(join_path(name,sizeof(name),paths.cache,"profile-probe"),"private cache spelling");directory=name;
        if(mkdir(name,0755)!=0)check(errno==EEXIST,"private cache mkdir");
        const int fd=open(name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);struct stat s{};
        const bool ok=fd>=0&&fstat(fd,&s)==0&&S_ISDIR(s.st_mode);const bool closed=fd<0||close(fd)==0;
        check(ok&&closed,"private cache nofollow directory identity");
    }
    std::string create(const std::vector<std::uint8_t>& bytes){
        const auto name=directory+"/own-"+std::to_string(getpid())+"-"+std::to_string(sequence++)+".synthetic";
        const int fd=open(name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);check(fd>=0,"exclusive owned file create");owned.push_back(name);
        struct stat s{};bool ok=fstat(fd,&s)==0&&S_ISREG(s.st_mode);
        std::size_t at=0;while(ok&&at<bytes.size()){const auto n=write(fd,bytes.data()+at,bytes.size()-at);if(n<=0)ok=false;else at+=static_cast<std::size_t>(n);}
        ok=fsync(fd)==0&&ok;const bool closed=close(fd)==0;check(ok&&closed,"owned file write/fsync/close");return name;
    }
    std::vector<std::uint8_t> read_file(const std::string& name){
        check(std::find(owned.begin(),owned.end(),name)!=owned.end(),"only own files reopened");
        const int fd=open(name.c_str(),O_RDONLY|O_NOFOLLOW);check(fd>=0,"owned nofollow reopen");struct stat s{};
        if(fstat(fd,&s)!=0||!S_ISREG(s.st_mode)||s.st_size<0||s.st_size>65536){close(fd);check(false,"owned size/identity");}
        std::vector<std::uint8_t> data(static_cast<std::size_t>(s.st_size));std::size_t at=0;bool ok=true;
        while(at<data.size()){const auto n=read(fd,data.data()+at,data.size()-at);if(n<=0){ok=false;break;}at+=static_cast<std::size_t>(n);}
        const bool closed=close(fd)==0;check(ok&&closed,"owned file exact read/close");return data;
    }
    bool cleanup(){bool ok=true;for(const auto& name:owned)if(unlink(name.c_str())!=0)ok=false;owned.clear();cleaned=true;return ok;}
    ~Files(){if(!cleaned)cleanup();}
};
void hle(psprecomp::Runtime& r,bool bad){
    r.nids().add("ProbeProfile",0x01900001,"guest_data");mhp3rd::HleRegistrar registry(r);
    registry.add("ProbeProfile","guest_data",[bad](auto& runtime,auto& ctx){ctx.set_gpr(3,ctx.gpr[3]+1);mhp3rd::store64(runtime.memory(),pixels,0x0123456789abcdefull);ctx.set_gpr(2,bad?43:42);});
    check(registry.count()==1&&registry.bound("ProbeProfile",0x01900001),"real profile registrar identity/count");
    r.invoke_import("ProbeProfile",0x01900001,r.cpu());check(r.cpu().gpr[3]==1&&r.cpu().gpr[2]==42&&r.memory().load32(pixels)==0x89abcdef&&r.memory().load32(pixels+4)==0x01234567,"HLE dispatch/guest 64-bit store oracle");
    check(!registry.try_add("ProbeProfile","absent",[](auto&,auto&){}),"unknown HLE try_add rejection");
    bool rejected=false;try{registry.add("ProbeProfile","absent",[](auto&,auto&){});}catch(const std::exception&){rejected=true;}check(rejected,"unknown HLE add exception");
    mhp3rd::write_cstring(r.memory(),pixels,"public",4);check(mhp3rd::read_cstring(r.memory(),pixels)=="pub"&&r.memory().load8(pixels+3)==0,"guest EABI string capacity and terminator");
    rejected=false;try{(void)mhp3rd::read_cstring(r.memory(),0x0a000000,1);}catch(const std::exception&){rejected=true;}check(rejected,"profile guest memory exception");
}
void geometry(psprecomp::GuestMemory& memory,ge::Scene& scene,bool bad){
    const ge::Scene expected;scene=expected;
    GeState state;unsigned draw=0;
    state.set_draw_sink([&](const DrawCall& call){
        check(draw<expected.draw_count&&call.vertices.size()==6&&call.primitive==PrimitiveType::Triangles,"GE display list draw/primitive count");
        const auto& d=expected.draws[draw];check(call.through==d.through,"GE through bit decode");
        for(unsigned i=0;i<6;++i){const auto& v=call.vertices[i];const auto& want=expected.vertices[d.first+i];
            auto& out=scene.vertices[d.first+i];out.x=v.position[0];out.y=v.position[1];out.z=v.position[2];out.u=v.texcoord[0];out.v=v.texcoord[1];out.color=v.color;
            if(bad&&draw==0&&i==0)out.x+=1;
            check(out.x==want.x&&out.y==want.y&&out.z==want.z&&out.u==want.u&&out.v==want.v&&out.color==want.color,"independent GE decoded geometry oracle");
        }++draw;
    });
    for(unsigned d=0;d<expected.draw_count;++d){
        constexpr std::uint32_t type=3u|(7u<<2)|(3u<<7);const auto& descriptor=expected.draws[d];
        for(unsigned i=0;i<6;++i){const auto& v=expected.vertices[descriptor.first+i];const auto address=vertices+i*24;
            memory.store32(address,std::bit_cast<std::uint32_t>(v.u));memory.store32(address+4,std::bit_cast<std::uint32_t>(v.v));memory.store32(address+8,v.color);
            memory.store32(address+12,std::bit_cast<std::uint32_t>(v.x));memory.store32(address+16,std::bit_cast<std::uint32_t>(v.y));memory.store32(address+20,std::bit_cast<std::uint32_t>(v.z));
        }
        const std::uint32_t words[]{command(0x10,0x080000),command(0x12,type|(descriptor.through?(1u<<23):0)),command(1,vertices),command(4,(3u<<16)|6),command(0x0c)};
        for(unsigned i=0;i<5;++i)memory.store32(list+4*i,words[i]);bool finished=false;
        const auto stalled=state.execute(memory,list,list+12,finished);check(!finished&&stalled==list+12,"GE stall before primitive");
        check(state.execute(memory,stalled,0,finished)==list+20&&finished,"GE resume/END position");
    }
    check(draw==11&&scene.vertex_count==66,"GE eleven panels/draws and 66 decoded vertices");
    std::vector<Vertex> decoded;memory.store32(vertices,std::bit_cast<std::uint32_t>(1.25f));memory.store32(vertices+4,std::bit_cast<std::uint32_t>(-2.5f));memory.store32(vertices+8,std::bit_cast<std::uint32_t>(3.75f));
    check(decode_vertices(memory,vertices,3u<<7,1,decoded)==12&&decoded.size()==1&&decoded[0].position==std::array<float,4>{1.25f,-2.5f,3.75f,1},"raw float vertex constants");
    memory.store8(vertices,12);memory.store8(vertices+1,34);memory.store8(vertices+2,56);
    check(decode_vertices(memory,vertices,(1u<<7)|(1u<<23),1,decoded)==3&&decoded[0].position[0]==12&&decoded[0].position[1]==34,"through byte vertex format");
}
void textures(psprecomp::GuestMemory& memory,bool bad){
    for(unsigned format=0;format<=10;++format){
        TextureState t{};t.address=pixels;t.width=format>=8?4:2;t.height=format>=8?4:2;t.buffer_width=t.width;t.format=static_cast<TextureFormat>(format);t.clut_address=palette;t.clut_format=3;t.clut_mask=255;
        for(unsigned i=0;i<64;++i)memory.store8(pixels+i,0);
        memory.store32(palette,0xff0000ff);memory.store32(palette+4,0xff00ff00);
        std::vector<std::uint32_t> expected;
        if(format<3){for(unsigned i=0;i<4;++i)memory.store16(pixels+2*i,format==0?0x001f:format==1?0x801f:0xf00f);expected.assign(4,0xff0000ff);}
        else if(format==3){for(unsigned i=0;i<4;++i)memory.store32(pixels+4*i,ge::texels[i]);expected.assign(std::begin(ge::texels),std::end(ge::texels));}
        else if(format<8){for(unsigned i=0;i<4;++i){if(format==4)memory.store8(pixels+i/2,static_cast<std::uint8_t>(i%2?0x10:0));else if(format==5)memory.store8(pixels+i,static_cast<std::uint8_t>(i%2));else if(format==6)memory.store16(pixels+2*i,static_cast<std::uint16_t>(i%2));else memory.store32(pixels+4*i,i%2);}expected={0xff0000ff,0xff00ff00,0xff0000ff,0xff00ff00};}
        else {memory.store16(pixels+4,0xf800);memory.store16(pixels+6,0x07e0);if(format==9){for(unsigned i=0;i<8;++i)memory.store8(pixels+8+i,255);}if(format==10){memory.store8(pixels+14,255);memory.store8(pixels+15,0);}expected.assign(16,0xff0000ff);}
        std::vector<std::uint32_t> actual;check(decode_texture(memory,t,actual),"original decoder format accepted");if(bad&&format==0)actual[0]^=1;if(actual!=expected)throw std::runtime_error("texture format="+std::to_string(format)+" actual first="+std::to_string(actual.at(0))+" expected first="+std::to_string(expected.at(0)));
        if(format<8){TextureSnapshot snapshot;check(snapshot_texture(memory,t,snapshot),"owned texture/palette snapshot");memory.store32(pixels,0);memory.store32(palette,0);check(decode_snapshot(snapshot,actual)&&actual==expected,"snapshot independent of subsequent guest changes");}
    }
    TextureState t{};t.address=pixels;t.width=4;t.height=1;t.buffer_width=4;t.format=TextureFormat::Rgba8888;t.swizzled=true;
    for(unsigned i=0;i<4;++i)memory.store32(pixels+4*i,0xff010203u+i);std::vector<std::uint32_t> out;
    check(decode_texture(memory,t,out)&&out==std::vector<std::uint32_t>{0xff010203,0xff010204,0xff010205,0xff010206},"swizzled first block oracle");
    t.swizzled=false;t.address=0x041ffffe;t.width=2;t.buffer_width=2;memory.store32(t.address,0xff112233);memory.store32(t.address+4,0xff445566);
    check(decode_texture(memory,t,out)&&out==std::vector<std::uint32_t>{0xff112233,0xff445566},"EDRAM physical wrap oracle");TextureSnapshot snapshot;check(!snapshot_texture(memory,t,snapshot),"noncontiguous snapshot rejection");
    t.address=0;check(!decode_texture(memory,t,out),"unmapped texture rejection");t.address=pixels;t.width=0;check(!decode_texture(memory,t,out),"zero texture rejection");t.width=1025;check(!decode_texture(memory,t,out),"oversized texture rejection");t.width=2;t.format=static_cast<TextureFormat>(11);check(!decode_texture(memory,t,out),"unknown texture format rejection");
}
void crypto(bool bad){
    const auto key=hex("000102030405060708090a0b0c0d0e0f");const Aes128 cipher(key);
    const auto plain=hex("00112233445566778899aabbccddeeff");auto actual=cipher.encrypt(plain);if(bad)actual[0]^=1;
    check(actual==hex("69c4e0d86a7b0430d8cdb78070b4c55a")&&cipher.decrypt(actual)==plain,"FIPS-197 independent AES vector");
    const Aes128 cmac_cipher(hex("2b7e151628aed2a6abf7158809cf4f3c"));
    check(cmac(cmac_cipher,{})==hex("bb1d6929e95937287fa37d129b756746"),"RFC4493 empty CMAC vector");
    for(const auto mode:{CryptMode::Mode1,CryptMode::Mode3,CryptMode::Mode5}){auto data=payload();const auto encrypted=encrypt_data(data,mode,&key,plain);auto decrypted=decrypt_data(encrypted,mode,&key);
        check(encrypted.size()==544&&decrypted&&decrypted->size()==528&&std::equal(data.begin(),data.end(),decrypted->begin())&&std::all_of(decrypted->begin()+513,decrypted->end(),[](auto b){return b==0;}),"three save modes length/plaintext/padding oracle");
        auto changed=encrypted;changed[20]^=1;check(data_file_hash(changed,mode,&key)!=data_file_hash(encrypted,mode,&key),"save hash observes corruption");check(!decrypt_data(std::span(encrypted).first(15),mode,&key),"short encrypted header rejection");
    }
}
std::vector<std::uint8_t> sfo(){
    ParamSfo value;value.set_string("TITLE","Public synthetic probe",64);value.set_integer("COUNT",42);value.set_binary("SAVEDATA_PARAMS",std::vector<std::uint8_t>(128),128);
    auto data=value.serialize();check(data[0]==0&&data[1]=='P'&&data[2]=='S'&&data[3]=='F',"SFO fixed magic oracle");auto parsed=ParamSfo::parse(data);
    check(parsed&&parsed->string("TITLE")=="Public synthetic probe"&&parsed->integer("COUNT")==42&&parsed->binary("SAVEDATA_PARAMS")->size()==128,"SFO typed field oracle");
    const auto offset=value.data_offset("SAVEDATA_PARAMS");check(offset.has_value(),"SFO signature field location");
    for(const auto mode:{CryptMode::Mode1,CryptMode::Mode3,CryptMode::Mode5}){auto signed_data=data;sign_param_sfo(signed_data,*offset,mode);check(verify_param_sfo(signed_data,*offset),"signed SFO verifies");signed_data[0]^=1;check(!verify_param_sfo(signed_data,*offset),"signed SFO corruption rejected");}
    auto invalid=data;invalid[0]=1;check(!ParamSfo::parse(invalid)&&!ParamSfo::parse(std::span(data).first(19)),"malformed/truncated SFO rejection");return data;
}
std::vector<std::uint8_t> iso_bytes(){
    std::vector<std::uint8_t> image(19*2048);const unsigned pvd=16*2048,root=17*2048;
    image[pvd]=1;std::memcpy(image.data()+pvd+1,"CD001",5);image[pvd+6]=1;word(image,pvd+158,17);word(image,pvd+166,2048);
    image[root]=44;word(image,root+2,18);word(image,root+10,513);image[root+32]=10;std::memcpy(image.data()+root+33,"READ.BIN;1",10);
    const auto data=payload();std::copy(data.begin(),data.end(),image.begin()+18*2048);return image;
}
void paths(){
    char out[path_capacity];check(join_path(out,sizeof(out),"/title/cache","profile-probe/public.bin"),"contained lexical path");
    for(const auto* invalid:{"../other","a/../b","/other","a\\b","a//b","./a","a/"})check(!join_path(out,sizeof(out),"/title/cache",invalid),"traversal/absolute/ambiguous path rejected");
    const std::string utf8="public-\xc3\xb1-\xe6\xbc\xa2.bin";check(mhp3rd::path_to_utf8(mhp3rd::path_from_utf8(utf8))==utf8,"original UTF8 path roundtrip");
}
}
bool profile_graphics_scene(ge::Scene& scene,std::array<std::uint32_t,4>& texels,Log& log){
    try{psprecomp::GuestMemory memory;geometry(memory,scene,false);TextureState texture{};texture.address=pixels;texture.width=texture.height=texture.buffer_width=2;texture.format=TextureFormat::Rgba8888;
        for(unsigned i=0;i<4;++i)memory.store32(pixels+4*i,ge::texels[i]);std::vector<std::uint32_t> decoded;check(decode_texture(memory,texture,decoded)&&decoded.size()==4,"profile GPU texture input");std::copy(decoded.begin(),decoded.end(),texels.begin());
        check(std::equal(texels.begin(),texels.end(),std::begin(ge::texels)),"profile GPU texel oracle");log.write("[profile-graphics] PASS original GeState 11 draws/66 vertices and original RGBA decoder feed native buffers; decoded shader path");return true;
    }catch(const std::exception& e){log.write("[profile-graphics] FAIL %s",e.what());return false;}
}
bool profile_probe(const TitlePaths& paths_value,Log& log,ProfileFault fault){
    unsigned passed=0;bool all=true;
    auto run=[&](const char* name,auto function){try{function();++passed;log.write("[profile] case=%s PASS original code and independent oracle",name);}catch(const std::exception& e){all=false;log.write("[profile] case=%s FAIL %s",name,e.what());}};
    psprecomp::Runtime runtime;run("hle",[&]{hle(runtime,fault==ProfileFault::Hle);});
    run("geometry",[&]{ge::Scene scene;geometry(runtime.memory(),scene,fault==ProfileFault::Geometry);});
    run("textures",[&]{textures(runtime.memory(),fault==ProfileFault::Texture);});
    run("crypto",[&]{crypto(fault==ProfileFault::Crypto);});
    std::vector<std::uint8_t> metadata;run("sfo",[&]{metadata=sfo();});run("paths",[]{paths();});
    try{Files files(paths_value);
        run("iso",[&]{const auto name=files.create(iso_bytes());mhp3rd::IsoImage image(mhp3rd::path_from_utf8(name));const auto entry=image.find("read.bin");check(entry&&entry->lba==18&&entry->size==513&&!entry->directory&&image.list("")==std::vector<std::string>{"READ.BIN"}&&!image.find("absent"),"original ISO directory oracle");std::vector<std::uint8_t> data(513);check(image.read(18*2048,data)==513&&data==payload()&&image.read(image.size_bytes(),data)==0,"original ISO exact payload/EOF");
            for(unsigned i=0;i<data.size();++i)runtime.memory().store8(pixels+i,data[i]);
            runtime.nids().add("ProbeProfile",0x01900002,"decoded_texel");mhp3rd::HleRegistrar imports(runtime);
            imports.add("ProbeProfile","decoded_texel",[](auto& r,auto& ctx){TextureState texture{};texture.address=pixels;texture.width=texture.height=texture.buffer_width=1;texture.format=TextureFormat::Rgba8888;std::vector<std::uint32_t> result;check(decode_texture(r.memory(),texture,result)&&result.size()==1,"ISO->guest->HLE->texture decode");ctx.set_gpr(2,result[0]);});
            runtime.invoke_import("ProbeProfile",0x01900002,runtime.cpu());check(runtime.cpu().gpr[2]==0x402d1a07,"file-to-guest-to-original-HLE/texture independent packed-word oracle");
            const auto bad=files.create(std::vector<std::uint8_t>(32));bool rejected=false;try{mhp3rd::IsoImage invalid(mhp3rd::path_from_utf8(bad));}catch(const std::exception&){rejected=true;}check(rejected,"malformed synthetic ISO rejection");});
        if(fault!=ProfileFault::Omit)run("checkpoint",[&]{const auto key=hex("000102030405060708090a0b0c0d0e0f");const auto data=payload();auto encrypted=encrypt_data(data,CryptMode::Mode5,&key,Block{});const auto source=files.create(encrypted),destination=files.create({});check(rename(source.c_str(),destination.c_str())==0,"owned reserved rename");files.owned.erase(std::find(files.owned.begin(),files.owned.end(),source));auto returned=files.read_file(destination);if(fault==ProfileFault::Files)returned[19]^=1;check(returned==encrypted,"whole encrypted checkpoint oracle");const auto sfo_name=files.create(metadata);check(files.read_file(sfo_name)==metadata&&ParamSfo::parse(metadata).has_value(),"SFO checkpoint bytes/parser");auto restored=decrypt_data(returned,CryptMode::Mode5,&key);check(restored&&std::equal(data.begin(),data.end(),restored->begin()),"owned checkpoint crypto->file->decrypt");});
        const bool cleaned=files.cleanup()&&fault!=ProfileFault::Cleanup;all=all&&cleaned;log.write("[profile] cleanup=%s only execution-owned cache files removed",cleaned?"PASS":"FAIL");
    }catch(const std::exception& e){all=false;log.write("[profile] FAIL file boundary %s",e.what());}
    all=all&&passed==8;log.write("[profile-summary] cases=%u expected=8 profile=%s full_profile=TODO full_game=TODO",passed,all?"PASS":"FAIL");return all;
}
}
