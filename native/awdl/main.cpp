// SPDX-License-Identifier: GPL-3.0-or-later
#include <winsock2.h>
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <string>
#include "compat/owl-platform.h"
#include "Engine.h"
#include "../WorkerProcess.h"
#include "Tunnel.h"
#include <mmsystem.h>

static volatile LONG stopping=0;
static BOOL WINAPI stop_console(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) { InterlockedExchange(&stopping,1); return TRUE; }
    return FALSE;
}
static int run() {
    WorkerProcess radio(L"windrop-hardware.exe",L"serve");
    windrop::Record response;
    uint64_t deadline=GetTickCount64()+120000;
    std::array<uint8_t,6> mac{};
    while(!radio.next(response)) {
        if(GetTickCount64()>deadline) throw std::runtime_error("Radio initialization timed out");
        Sleep(10);
    }
    if(response.type!=windrop::Message::Hello || response.bytes.size()!=6) throw std::runtime_error("Invalid radio initialization reply");
    memcpy(mac.data(),response.bytes.data(),6);
    Engine engine(mac); Tunnel tunnel(mac);
    uint32_t actual_channel=6,pending_channel=0;
    engine.state.channel.current=engine.state.channel.master;
    uint64_t next_psf=0,next_mif=0,next_clean=0,pending_since=0;
    struct Pending { std::vector<uint8_t> bytes; uint64_t expires; };
    std::deque<Pending> unicast,multicast;
    SetConsoleCtrlHandler(stop_console,TRUE);
    struct TimerResolution { TimerResolution(){timeBeginPeriod(1);} ~TimerResolution(){timeEndPeriod(1);} } timing;
    puts("AWDL worker running on awdl0 (experimental; hardware interoperability is unqualified). Press Ctrl+C to stop."); fflush(stdout);
    auto transmit=[&](std::vector<uint8_t> frame) {
        uint64_t expires=GetTickCount64()+4;
        std::vector<uint8_t> bytes(8+frame.size()); memcpy(bytes.data(),&expires,8); memcpy(bytes.data()+8,frame.data(),frame.size());
        return radio.send(windrop::Message::Transmit,std::move(bytes));
    };
    while(!InterlockedCompareExchange(&stopping,0,0)) {
        uint64_t now=clock_time_us();
        for(unsigned i=0;i<128 && radio.next(response);i++) {
            if(response.type==windrop::Message::Frame) engine.receive(response.bytes,[&](const uint8_t* p,size_t n){tunnel.write(p,n);});
            else if(response.type==windrop::Message::ChannelAck && response.bytes.size()==4) {
                uint32_t channel; memcpy(&channel,response.bytes.data(),4);
                if(channel!=pending_channel) throw std::runtime_error("Unexpected radio channel acknowledgement");
                actual_channel=channel; pending_channel=0;
                awdl_chan current{};
                if(engine.state.channel.enc==AWDL_CHAN_ENC_LEGACY) current.legacy.chan_num=static_cast<uint8_t>(channel);
                else { current.opclass.chan_num=static_cast<uint8_t>(channel); current.opclass.opclass=channel>14?0x80:0x51; }
                engine.state.channel.current=current;
            } else if(response.type==windrop::Message::Fault) throw std::runtime_error("Radio worker failed");
            else throw std::runtime_error("Invalid live radio response");
        }
        if(now>=next_clean) { engine.clean(now); next_clean=now+engine.state.peers.clean_interval; }
        unsigned slot=awdl_sync_current_eaw(now,&engine.state.sync)%AWDL_CHANSEQ_LENGTH;
        uint32_t desired=awdl_chan_num(engine.state.channel.sequence[slot],engine.state.channel.enc);
        if(desired && desired!=actual_channel && !pending_channel) {
            std::vector<uint8_t> bytes(4); memcpy(bytes.data(),&desired,4);
            if(radio.send(windrop::Message::Channel,std::move(bytes))) { pending_channel=desired; pending_since=now; }
        }
        if(pending_channel && now-pending_since>2000000) throw std::runtime_error("Radio retune timed out");
        bool on_channel=desired && desired==actual_channel && !pending_channel;
        if(on_channel && now>=next_psf) {
            transmit(engine.action(AWDL_ACTION_PSF)); next_psf=now+static_cast<uint64_t>(engine.state.psf_interval)*1024;
        }
        if(on_channel && now>=next_mif) {
            transmit(engine.action(AWDL_ACTION_MIF));
            next_mif=now+awdl_sync_next_aw_us(now,&engine.state.sync)+
                static_cast<uint64_t>(engine.state.sync.presence_mode)*engine.state.sync.aw_period*1024/2;
        }
        // Separate bounded queues prevent mDNS from starving a file transfer.
        std::vector<uint8_t> packet;
        for(unsigned i=0;i<32 && tunnel.read(packet);i++) {
            ether_addr destination{};
            if(!engine.destination(packet,destination)) continue;
            auto& queue=(destination.ether_addr_octet[0]&1)?multicast:unicast;
            if(queue.size()<16) queue.push_back({std::move(packet),now+2000000});
        }
        for(auto* queue : {&multicast,&unicast}) {
            while(!queue->empty() && queue->front().expires<now) queue->pop_front();
            if(queue->empty() || !on_channel) continue;
            ether_addr destination{};
            if(!engine.destination(queue->front().bytes,destination)) { queue->pop_front(); continue; }
            if(engine.can_send(destination,now) && transmit(engine.data(queue->front().bytes,destination))) queue->pop_front();
        }
        Sleep(1);
    }
    SetConsoleCtrlHandler(stop_console,FALSE);
    return 0;
}

static std::array<uint8_t,6> parse_mac(const std::string& text) {
    if(text.size()!=12) throw std::runtime_error("Replay peer must be 12 hex digits");
    std::array<uint8_t,6> address{};
    for(size_t i=0;i<6;i++) {
        size_t count=0; unsigned long value=std::stoul(text.substr(i*2,2),&count,16);
        if(count!=2) throw std::runtime_error("Invalid replay peer MAC");
        address[i]=static_cast<uint8_t>(value);
    }
    return address;
}
static uint32_t le32(const uint8_t* p) { return p[0]|static_cast<uint32_t>(p[1])<<8|static_cast<uint32_t>(p[2])<<16|static_cast<uint32_t>(p[3])<<24; }
static void read(std::ifstream& input,uint8_t* p,size_t n) {
    if(!input.read(reinterpret_cast<char*>(p),static_cast<std::streamsize>(n))) throw std::runtime_error("Truncated PCAP");
}
static int replay(const char* input_name,const char* output_name,const std::array<uint8_t,6>& peer) {
    std::ifstream input(input_name,std::ios::binary);
    if(!input) throw std::runtime_error("Cannot open PCAP");
    uint8_t header[24]; read(input,header,24);
    if(le32(header)!=0xa1b2c3d4 || header[4]!=2 || header[6]!=4 || le32(header+20)!=127)
        throw std::runtime_error("Expected little-endian radiotap PCAP v2.4");
    Engine engine({2,0,0,0,0,1}); engine.add_replay_peer(peer);
    std::vector<uint8_t> packets;
    unsigned frames=0,delivered=0;
    while(input.peek()!=EOF) {
        uint8_t record[16]; read(input,record,16); uint32_t size=le32(record+8);
        if(!size || size>4096 || size>le32(record+12) || ++frames>100000) throw std::runtime_error("Invalid PCAP record size");
        std::vector<uint8_t> frame(size); read(input,frame.data(),frame.size());
        engine.receive(frame,[&](const uint8_t* p,size_t n) {
            if(packets.size()+n>64*1024*1024) throw std::runtime_error("Replay output exceeds 64 MiB");
            packets.insert(packets.end(),p,p+n); delivered++;
        });
    }
    std::ofstream output(output_name,std::ios::binary|std::ios::trunc);
    if(!output || !output.write(reinterpret_cast<const char*>(packets.data()),static_cast<std::streamsize>(packets.size())))
        throw std::runtime_error("Cannot write replay output");
    engine.clean(clock_time_us());
    printf("frames=%u ipv6_packets=%u bytes=%zu channels=",frames,delivered,packets.size());
    for(unsigned i=0;i<AWDL_CHANSEQ_LENGTH;i++) printf("%s%u",i?",":"",awdl_chan_num(engine.state.channel.sequence[i],engine.state.channel.enc));
    puts(""); return 0;
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--help") { puts("windrop-awdl run\nwindrop-awdl replay input.pcap output.bin --peer HEXMAC\nrun requires administrator access and a signed RTL8852CE driver. replay does not initialize hardware."); return 0; }
        if(argc==2 && std::string(argv[1])=="run") return run();
        if(argc==6 && std::string(argv[1])=="replay" && std::string(argv[4])=="--peer") return replay(argv[2],argv[3],parse_mac(argv[5]));
        throw std::runtime_error("Use --help for AWDL worker commands");
    } catch(const std::exception& error) { fprintf(stderr,"%s\n",error.what()); return 3; }
}

