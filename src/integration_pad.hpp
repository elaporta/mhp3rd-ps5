// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "integration.hpp"
#include <cmath>
#include <cstring>
namespace yakumo::ps5 {
// Layout facts from OpenOrbis include/orbis/_types/pad.h, not proof of PS5 ABI.
struct IntegrationPadInformation {
    float density;std::uint16_t resolution_x,resolution_y;
    std::uint8_t deadzone_left,deadzone_right,connection_type,count;
    std::int32_t connected,device_class;std::uint8_t unknown[8];
};
static_assert(sizeof(IntegrationPadInformation)==28 && offsetof(IntegrationPadInformation,connected)==12 &&
    offsetof(IntegrationPadInformation,device_class)==16);
inline IntegrationInput integration_pad_information(const void* bytes,int rc,bool guard) {
    IntegrationPadInformation fields{};std::memcpy(&fields,bytes,sizeof(fields));
    IntegrationInput result;
    result.api_result=rc;result.reported_connected=fields.connected;result.device_class=fields.device_class;
    result.count=fields.count;result.resolution_x=fields.resolution_x;result.resolution_y=fields.resolution_y;result.density=fields.density;
    const bool plausible=fields.connected==0 || (std::isfinite(fields.density)&&fields.density>0&&fields.density<100 &&
        fields.resolution_x>0 && fields.resolution_x<=32768 && fields.resolution_y>0 && fields.resolution_y<=32768 &&
        fields.deadzone_left<=127 && fields.deadzone_right<=127 && fields.device_class==0);
    result.valid=rc==0 && guard && plausible && (fields.connected==0||fields.connected==1);
    result.connected=result.valid&&fields.connected==1;
    return result;
}
}
