// SPDX-License-Identifier: GPL-3.0-or-later
// Actual error/recovery evaluator with only the SDK boundary controlled on host.
#include "profile_probe.hpp"
#include <set>
using namespace yakumo::ps5;
namespace {unsigned fault{},opens{};std::set<int> live;}
extern "C" {
int sceAudioOutOpen(int,int,int,unsigned grain,unsigned,unsigned){if(grain==0)return fault==1?99:-22;const int handle=8+static_cast<int>(opens++);if(fault==2)return -33;live.insert(handle);return handle;}
int sceAudioOutOutput(int handle,const void*){if(handle==-1)return fault==3?256:-44;if(!live.contains(handle))return fault==4?256:-55;return fault==5?-66:256;}
int sceAudioOutClose(int handle){live.erase(handle);return fault==6?-77:0;}
int scePadGetControllerInformation(int,void* data){if(fault==7)static_cast<unsigned char*>(data)[512]=0;return fault==8?0:-88;}
}
int main(){Log log;for(fault=0;fault<=8;++fault){live.clear();opens=0;const bool result=profile_native_errors(log);if(result!=(fault==0)||!live.empty())return 1;}return 0;}
