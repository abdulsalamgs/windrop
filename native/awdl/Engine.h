// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>
extern "C" {
#include "state.h"
#include "rx.h"
#include "tx.h"
#include "schedule.h"
#include "election.h"
}
static_assert(sizeof(ieee80211_hdr)==24 && sizeof(awdl_action)==16 && sizeof(awdl_data)==8,"Wire packing must match AWDL");

class Engine {
public:
    awdl_state state{};
    ieee80211_state ieee{};
    explicit Engine(const std::array<uint8_t,6>& mac) {
        ether_addr self{}; memcpy(&self,mac.data(),6);
        awdl_chan channel{}; channel.opclass.chan_num=6; channel.opclass.opclass=0x51;
        awdl_init_state(&state,"Windrop",&self,channel,clock_time_us());
        ieee80211_init_state(&ieee);
    }
    ~Engine() { awdl_peers_free(state.peers.peers); }
    Engine(const Engine&)=delete;
    Engine& operator=(const Engine&)=delete;
    void add_replay_peer(const std::array<uint8_t,6>& mac) {
        ether_addr address{}; memcpy(&address,mac.data(),6);
        if(awdl_peer_add(state.peers.peers,&address,clock_time_us(),nullptr,nullptr)<0) throw std::runtime_error("Cannot add replay peer");
    }
    static bool ipv6(const uint8_t* p,size_t size) {
        return size>=40 && size<=1450 && (p[0]>>4)==6 && (static_cast<size_t>(p[4])*256+p[5]+40)==size;
    }
    void receive(const std::vector<uint8_t>& frame,const std::function<void(const uint8_t*,size_t)>& deliver) {
        if(frame.size()<8 || frame.size()>4096) return;
        size_t rt=frame[2]+static_cast<size_t>(frame[3])*256;
        static const uint8_t bssid[]={0,0x25,0,0xff,0x94,0x73};
        if(rt<8 || rt>frame.size() || frame.size()-rt<24 || memcmp(frame.data()+rt+16,bssid,6)) return;
        const buf* input=buf_new_const(frame.data(),static_cast<int>(frame.size()));
        if(!input) throw std::bad_alloc();
        std::array<buf*,16> output{}; buf** end=output.data();
        int result=awdl_rx(input,&end,output.data()+output.size(),&state);
        buf_free(input);
        for(buf** p=output.data();p<end;p++) {
            const uint8_t* bytes=buf_data(*p); int size=buf_len(*p);
            if(result==RX_OK && size>=14 && bytes[12]==0x86 && bytes[13]==0xdd && ipv6(bytes+14,static_cast<size_t>(size-14)))
                deliver(bytes+14,static_cast<size_t>(size-14));
            buf_free(*p);
        }
    }
    std::vector<uint8_t> action(awdl_action_type type) {
        std::vector<uint8_t> bytes(4096);
        int size=awdl_init_full_action_frame(bytes.data(),&state,&ieee,type);
        if(size<0 || size>4096) throw std::runtime_error("Cannot serialize AWDL action");
        bytes.resize(size); return bytes;
    }
    bool destination(const std::vector<uint8_t>& ip,ether_addr& address) {
        if(!ipv6(ip.data(),ip.size())) return false;
        if(ip[24]==0xff) { address.ether_addr_octet[0]=0x33; address.ether_addr_octet[1]=0x33; memcpy(address.ether_addr_octet+2,ip.data()+36,4); return true; }
        // RFC4291 link-local EUI-64. Do not forward ordinary LAN/global traffic.
        if(ip[24]!=0xfe || ip[25]!=0x80 || ip[35]!=0xff || ip[36]!=0xfe) return false;
        address.ether_addr_octet[0]=ip[32]^2; address.ether_addr_octet[1]=ip[33]; address.ether_addr_octet[2]=ip[34];
        memcpy(address.ether_addr_octet+3,ip.data()+37,3);
        return awdl_peer_get(state.peers.peers,&address,nullptr)==PEERS_OK;
    }
    bool can_send(const ether_addr& destination,uint64_t now) {
        if(destination.ether_addr_octet[0]&1)
            return awdl_is_multicast_eaw(&state,now) && awdl_can_send_in(&state,now,AWDL_MULTICAST_GUARD_TU)==0;
        awdl_peer* peer=nullptr;
        return awdl_peer_get(state.peers.peers,&destination,&peer)==PEERS_OK &&
            awdl_can_send_unicast_in(&state,peer,now,AWDL_UNICAST_GUARD_TU)==0;
    }
    std::vector<uint8_t> data(const std::vector<uint8_t>& ip,const ether_addr& destination) {
        std::vector<uint8_t> bytes(4096);
        int n=awdl_init_full_data_frame(bytes.data(),&state.self_address,&destination,ip.data(),static_cast<unsigned>(ip.size()),&state,&ieee);
        if(n<0 || n>4096) throw std::runtime_error("Cannot serialize AWDL data");
        bytes.resize(n); return bytes;
    }
    void clean(uint64_t now) {
        awdl_peers_remove(state.peers.peers,now>state.peers.timeout?now-state.peers.timeout:0,nullptr,nullptr);
        awdl_election_run(&state.election,&state.peers);
        awdl_peer* master=nullptr;
        if(!awdl_election_is_sync_master(&state.election,&state.self_address) &&
            awdl_peer_get(state.peers.peers,&state.election.sync_addr,&master)==PEERS_OK && master->has_sequence) {
            memcpy(state.channel.sequence,master->sequence,sizeof(state.channel.sequence));
            state.channel.enc=master->sequence_enc; state.channel.sequence_from_peer=1;
        } else if(state.channel.sequence_from_peer) {
            awdl_chanseq_init_static(state.channel.sequence,&state.channel.master);
            state.channel.enc=AWDL_CHAN_ENC_OPCLASS; state.channel.sequence_from_peer=0;
        }
        state.psf_interval=awdl_election_is_sync_master(&state.election,&state.self_address)?110:440;
    }
};
