// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../WorkerProcess.h"
// The AWDL-specific address is configured through the independent IPv6 helper.
// No Wintun header or dynamic-library loader is linked into this executable.
class Tunnel {
    static std::wstring arguments(const std::array<uint8_t,6>& mac) {
        uint8_t address[16]={0xfe,0x80,0,0,0,0,0,0,static_cast<uint8_t>(mac[0]^2),mac[1],mac[2],0xff,0xfe,mac[3],mac[4],mac[5]};
        wchar_t ip[40]{};
        swprintf_s(ip,L"%x:%x:%x:%x:%x:%x:%x:%x",(address[0]<<8)|address[1],(address[2]<<8)|address[3],
            (address[4]<<8)|address[5],(address[6]<<8)|address[7],(address[8]<<8)|address[9],
            (address[10]<<8)|address[11],(address[12]<<8)|address[13],(address[14]<<8)|address[15]);
        return std::wstring(L"serve --name awdl0 --address ")+ip+L" --mtu 1450";
    }
    WorkerProcess worker_;
public:
    explicit Tunnel(const std::array<uint8_t,6>& mac) : worker_(L"windrop-tunnel.exe",arguments(mac)) {
        windrop::Record reply;
        uint64_t deadline=GetTickCount64()+120000;
        while(!worker_.next(reply)) {
            if(GetTickCount64()>deadline) throw std::runtime_error("IPv6 tunnel initialization timed out");
            Sleep(10);
        }
        if(reply.type!=windrop::Message::Hello || !reply.bytes.empty()) throw std::runtime_error("Invalid IPv6 tunnel initialization reply");
    }
    bool read(std::vector<uint8_t>& bytes) {
        windrop::Record reply;
        if(!worker_.next(reply)) return false;
        if(reply.type!=windrop::Message::Frame) throw std::runtime_error("Invalid IPv6 tunnel packet reply");
        bytes=std::move(reply.bytes); return true;
    }
    void write(const uint8_t* data,size_t size) {
        worker_.send(windrop::Message::Frame,std::vector<uint8_t>(data,data+size));
    }
};
