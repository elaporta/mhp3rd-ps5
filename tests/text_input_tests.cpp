// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_input_ps5.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace yakumo::text_input;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
ImeParam* parameter{};
int init_rc{},status_value{1},result_rc{},term_rc{},abort_rc{},end_value{},term_calls{},abort_calls{};
int module_load{};
int init(ImeParam* p,void* extended){check(!extended&&p->user==4&&p->max_length<=512,"IME ABI and persistent parameter");parameter=p;return init_rc;}
int status(){return status_value;}
int result(ImeResult* p){p->end=end_value;return result_rc;}
int term(){++term_calls;return term_rc;}
int abort_ime(){++abort_calls;status_value=2;end_value=2;return abort_rc;}
void reset(){init_rc=result_rc=term_rc=abort_rc=end_value=term_calls=abort_calls=0;status_value=1;parameter=nullptr;}
std::optional<Failure> resolve(ImeApi& api){api={init,status,result,term,abort_ime};return {};}
void answer(std::u16string_view text){check(parameter,"persistent parameter still available");std::copy(text.begin(),text.end(),parameter->buffer);parameter->buffer[text.size()]=0;status_value=2;}
Request request(){Request r;r.title="Character";r.prompt="Name";r.max_characters=12;r.max_utf16_units=12;r.max_utf8_bytes=48;r.allow_empty=false;r.allowed=hunter_name_character;r.keyboard=KeyboardKind::BasicLatin;return r;}
void release(Service& s){check(s.suppress_sample(true),"release frame suppressed");check(!s.blocks_input(),"release restores input");}
void conversion(){
    std::u16string units=u"unchanged";std::string bytes="unchanged";
    const std::string text="A\xc3\xa9\xe6\x97\xa5\xf0\x9f\x98\x80";
    check(decode_utf8(text,units)&&units==u"A\u00e9\u65e5\U0001f600","all UTF-8 widths");
    check(decode_utf16(units,bytes)&&bytes==text,"Unicode round trip");
    for(const auto& invalid: {std::string("\x80"),std::string("\xc0\xaf"),std::string("\xe0\x80\x80"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xf5\x80\x80\x80"),std::string("\xe2\x82"),std::string("\xe2X\xa1"),std::string("A\0B",3)}){
        units=u"sentinel";check(!decode_utf8(invalid,units)&&units==u"sentinel","invalid UTF-8 is atomic");
    }
    for(const auto& invalid:{std::u16string(1,0xd800),std::u16string(1,0xdc00),std::u16string{0xd800,u'A'},std::u16string{u'A',0,u'B'}}){
        bytes="sentinel";check(!decode_utf16(invalid,bytes)&&bytes=="sentinel","invalid UTF-16 is atomic");
    }
    Request r;r.max_characters=1;r.max_utf16_units=2;r.max_utf8_bytes=4;
    check(validate(r,"\xf0\x9f\x98\x80"),"one scalar is two UTF-16 units/four bytes");
    r.max_utf16_units=1;check(!validate(r,"\xf0\x9f\x98\x80"),"unit limit independent");
    r.max_utf16_units=2;r.max_utf8_bytes=3;check(!validate(r,"\xf0\x9f\x98\x80"),"byte limit independent");
    r.max_utf8_bytes=8;check(!validate(r,"AB"),"scalar limit independent");
    check(hunter_name_character('Z')&&hunter_name_character('9')&&hunter_name_character(' ')&&!hunter_name_character('%')&&!hunter_name_character(0xe9)&&!hunter_name_character(0),"original name alphabet");
}
void lifecycle(){
    reset();Ps5Backend backend(resolve);backend.user(4);Service s(backend);
    auto id=s.submit(request()).id;check(id&&s.blocks_input(),"empty initial can open required field");
    check(parameter->type==1&&std::u16string(parameter->title)==u"Character"&&std::u16string(parameter->placeholder)==u"Name","native request metadata and kind");
    check(parameter->options==0x22&&s.active(),"single-line options and active request");
    check(s.submit(request()).failure->operation=="busy","second request does not replace active");
    check(!s.set_backend(backend),"backend cannot change during request");s.tick();check(!s.take(id),"pending is not confirmed");
    answer(u"Yakumo27");s.tick();check(!s.take(id+1),"wrong ID cannot consume answer");
    auto r=s.take(id);check(r&&r->outcome==Outcome::Confirmed&&r->text=="Yakumo27"&&term_calls==1,"result after native close");
    check(!s.active()&&s.blocks_input(),"native close separate from physical release");
    check(!s.take(id),"result once");check(s.suppress_sample(false)&&s.blocks_input(),"held buttons stay captured");release(s);
    check(s.set_backend(backend),"idle backend can change");
    reset();id=s.submit(request()).id;s.cancel(id+10);check(!abort_calls,"stale cancellation ignored");
    answer(u"Partial");end_value=1;s.tick();r=s.take(id);check(r->outcome==Outcome::Cancelled&&r->text.empty(),"cancel never publishes partial");release(s);
    reset();id=s.submit(request()).id;s.cancel(id);s.tick();r=s.take(id);check(r->outcome==Outcome::Cancelled&&abort_calls==1,"program cancellation");release(s);
    reset();id=s.submit(request()).id;answer(u"Late");s.cancel(id);end_value=0;s.tick();check(s.take(id)->outcome==Outcome::Cancelled,"confirm racing cancel cannot commit");release(s);
    reset();id=s.submit(request()).id;s.shutdown();check(s.take(id)->outcome==Outcome::Cancelled,"shutdown cancels");release(s);
    s.shutdown();check(!s.suppress_sample(true),"idle shutdown and input");
    reset();auto multiple=request();multiple.multiline=true;multiple.allowed={};
    id=s.submit(multiple).id;check(parameter->options==0x23,"native multiline option preserves original options");
    answer(u"First\nSecond");s.tick();check(s.take(id)->text=="First\nSecond","native multiline result");release(s);
    for(const auto& text:{u"",u"tooLongName123",u"Percent%"}){
        reset();id=s.submit(request()).id;answer(text);s.tick();r=s.take(id);check(r->outcome==Outcome::Error&&r->text.empty(),"invalid consumer result never truncated");release(s);
    }
    reset();id=s.submit(request()).id;answer(u"TwelveChars!");s.tick();check(s.take(id)->text=="TwelveChars!","exact consumer limit");release(s);
    Request bad=request();bad.initial="%";check(s.submit(bad).failure.has_value(),"invalid initial rejected");
    bad=request();bad.max_utf16_units=0;check(s.submit(bad).failure.has_value(),"zero capacity rejected");
    bad=request();bad.title="\xff";check(s.submit(bad).failure.has_value(),"invalid metadata encoding");
    reset();Request generic;generic.initial="\xe6\x97\xa5";id=s.submit(generic).id;
    check(parameter->type==0&&parameter->buffer[0]==0x65e5,"generic Unicode consumer uses default native kind");
    answer(u"\u65e5\U0001f600");s.tick();check(s.take(id)->text=="\xe6\x97\xa5\xf0\x9f\x98\x80","generic Unicode result independent of hunter rules");release(s);
}
void faults(){
    const auto run=[](auto inject,const char* operation){
        reset();Ps5Backend backend(resolve);backend.user(4);Service s(backend);inject(false);
        const auto id=s.submit(request()).id;inject(true);s.tick();auto r=s.take(id);
        check(r&&r->outcome==Outcome::Error&&r->text.empty()&&r->failure.operation==operation,"operation-specific error");release(s);
    };
    run([](bool){init_rc=-7;},"sceImeDialogInit");
    run([](bool after){if(after){status_value=2;result_rc=-8;}},"sceImeDialogGetResult");
    run([](bool after){if(after)status_value=-9;},"sceImeDialogGetStatus");
    run([](bool after){if(after)status_value=0;},"sceImeDialogGetStatus");
    run([](bool after){if(after){status_value=2;end_value=7;}},"ime-end-status");
    run([](bool after){if(after){answer(u"A");parameter->buffer[0]=0xd800;}},"ime-invalid-utf16");
    run([](bool after){if(after){std::fill_n(parameter->buffer,13,u'A');status_value=2;}},"ime-missing-terminator");
    reset();Ps5Backend backend(resolve);backend.user(4);Service s(backend);
    auto id=s.submit(request()).id;answer(u"Valid");term_rc=-10;s.tick();check(!s.take(id)&&backend.active()&&s.blocks_input(),"close failure retains buffers/capture");
    check(parameter->buffer[0]==u'V',"no free or clearing while close failed");term_rc=0;s.tick();check(s.take(id)->failure.operation=="sceImeDialogTerm","close recovery retains error");release(s);
    reset();id=s.submit(request()).id;abort_rc=-11;s.cancel(id);s.tick();check(s.take(id)->failure.operation=="sceImeDialogAbort","abort error distinct");release(s);
    reset();id=s.submit(request()).id;s.cancel(id);status_value=1;
    for(unsigned i=0;i<120;++i)s.tick();
    check(s.take(id)->failure.operation=="ime-abort-timeout"&&!backend.active(),"acknowledged abort cannot leave an endless running request");release(s);
    reset();id=s.submit(request()).id;answer(u"Valid");term_rc=-12;
    bool threw=false;try{for(unsigned i=0;i<120;++i)s.tick();}catch(const std::exception&){threw=true;}
    check(threw&&backend.active(),"bounded unrecoverable cleanup retains native memory");term_rc=0;s.tick();check(s.take(id)->outcome==Outcome::Error,"cleanup can still recover");release(s);
    Ps5Backend no_user(resolve);check(no_user.open(request())->operation=="ime-user","invalid user");
    Ps5Backend missing([](ImeApi&)->std::optional<Failure>{return Failure{"resolve-missing",-2};});missing.user(4);check(missing.open(request())->operation=="resolve-missing","missing capability");
    Ps5Backend incomplete([](ImeApi& api)->std::optional<Failure>{api.init=init;return {};});incomplete.user(4);check(incomplete.open(request())->operation=="ime-incomplete-api","partial exports rejected");
    auto big=request();big.max_utf16_units=513;check(backend.open(big)->operation=="ime-request-bounds","backend capacity");big=request();big.title=std::string(129,'x');check(backend.open(big).has_value(),"title capacity");
    reset();check(!backend.open(request()),"direct open");check(backend.open(request())->operation=="ime-busy","native busy");backend.cancel();backend.poll();check(!backend.poll(),"idle poll");backend.cancel();
}
}
extern "C" {
int sceSysmoduleLoadModule(std::uint16_t id){check(id==0x96,"public IME sysmodule ID");return module_load;}
int sceImeDialogInit(ImeParam* p,void* e){return init(p,e);}
int sceImeDialogGetStatus(){return status();}
int sceImeDialogGetResult(ImeResult* p){return result(p);}
int sceImeDialogTerm(){return term();}
int sceImeDialogAbort(){return abort_ime();}
// Match the hardware refusal. The r3 path must never depend on these calls.
int sceKernelDlsym(int,const char*,void**){throw std::runtime_error("native title dlsym must not be called");}
int sceKernelLoadStartModule(const char*,std::size_t,const void*,unsigned,void*,int*){throw std::runtime_error("payload-style module loading must not be called");}
}
int main(int argc,char** argv){try{
    if(argc>1){
        if(std::string_view(argv[1])=="module-hint-failed")module_load=-63;
        initialize_ps5(4);auto& s=service();
        for(int attempt=0;attempt<2;++attempt){
            reset();const auto id=s.submit(request()).id;answer(u"Yakumo27");s.tick();
            check(s.take(id)->outcome==Outcome::Confirmed,"checked imports work despite unavailable dynamic lookup");release(s);
        }
    }else{
        conversion();lifecycle();faults();
        ImeApi sentinel{init,status,result,term,abort_ime};
        for(int missing=0;missing<5;++missing){
            ImeApi table=sentinel,output{};
            if(missing==0)table.init=nullptr;if(missing==1)table.status=nullptr;if(missing==2)table.result=nullptr;
            if(missing==3)table.term=nullptr;if(missing==4)table.abort=nullptr;
            check(checked_ime_imports(output,table).has_value()&&!output.init&&!output.status&&!output.result&&!output.term&&!output.abort,"partial imports rejected atomically");
        }
        ImeApi output{};check(!checked_ime_imports(output,sentinel)&&output.init==init,"complete imported capability accepted");
    }
    std::cout<<"text input contracts PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
