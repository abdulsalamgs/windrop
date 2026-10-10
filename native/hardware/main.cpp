// SPDX-License-Identifier: GPL-2.0-only
#include "WindowsTransport.h"
#include "../ipc.h"
#include "kestrel/RtlKestrelDevice.h"
#include "RxPacket.h"
#include <atomic>
#include <mutex>
#include <thread>

int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--help") {
        puts("windrop-hardware doctor | serve\nRTL8852CE native radio worker. Requires a signed Windrop driver. serve uses bounded binary stdio IPC."); return 0;
    }
    try {
        if(argc!=2 || (std::string(argv[1])!="doctor" && std::string(argv[1])!="serve")) throw std::runtime_error("Use doctor or serve");
        auto transport=std::make_shared<WindowsTransport>();
        if(std::string(argv[1])=="doctor") { puts("Driver ABI v2: register and frame DMA transport available. AWDL readiness requires the running AWDL worker."); return 0; }
        windrop::binary_stdio();
        auto logger=std::make_shared<Logger>();
        RtlKestrelDevice radio(RtlAdapter(transport,logger),logger,kestrel::ChipVariant::C8852C);
        if(!radio.BringUpMonitor({6,0,CHANNEL_WIDTH_20})) throw std::runtime_error("RTL8852CE firmware/monitor initialization failed");
        radio.EnableTxScheduler();
        radio.SetCcaMode(false);
        uint8_t mac[6]; if(!radio.GetPermanentMacAddress(mac)) throw std::runtime_error("RTL8852CE has no valid permanent MAC");
        windrop::write_record(stdout,windrop::Message::Hello,mac,6);
        std::mutex output_mutex;
        std::atomic<bool> rx_failed{false};
        std::thread rx([&] {
            try { radio.StartRxLoop([&](const Packet& packet) {
                if(packet.Data.size()>4096 || packet.RxAtrib.crc_err || packet.RxAtrib.icv_err) return;
                // Bare MPDU -> radiotap. Include FLAGS and DBM_ANTSIGNAL so OWL
                // never guesses FCS presence or reads uninitialized metadata.
                std::vector<uint8_t> frame(10+packet.Data.size());
                uint8_t header[10]={0,0,10,0,0x22,0,0,0,packet.RxAtrib.fcs_present?uint8_t(0x10):uint8_t(0),0};
                header[9]=static_cast<uint8_t>(static_cast<int>(packet.RxAtrib.rssi[0])-110);
                memcpy(frame.data(),header,10); memcpy(frame.data()+10,packet.Data.data(),packet.Data.size());
                std::lock_guard<std::mutex> guard(output_mutex);
                windrop::write_record(stdout,windrop::Message::Frame,frame.data(),frame.size());
            }); } catch(const std::exception& error) {
                rx_failed=true;
                std::lock_guard<std::mutex> guard(output_mutex);
                try { windrop::write_record(stdout,windrop::Message::Fault,error.what(),strlen(error.what())); } catch(...) {}
            }
        });
        try {
            windrop::Record request;
            while(!rx_failed && windrop::read_record(stdin,request)) {
                if(request.type==windrop::Message::Stop && request.bytes.empty()) break;
                if(request.type==windrop::Message::Channel && request.bytes.size()==4) {
                    uint32_t channel; memcpy(&channel,request.bytes.data(),4);
                    // AWDL social channels only. No DFS or 6 GHz operation.
                    if(channel!=6 && channel!=36 && channel!=44 && channel!=149) throw std::runtime_error("Unsupported AWDL social channel");
                    radio.FastRetune(static_cast<uint8_t>(channel));
                    std::lock_guard<std::mutex> guard(output_mutex);
                    windrop::write_record(stdout,windrop::Message::ChannelAck,&channel,4);
                } else if(request.type==windrop::Message::Transmit && request.bytes.size()>=16 && request.bytes.size()<=4104) {
                    uint64_t deadline; memcpy(&deadline,request.bytes.data(),8);
                    if(GetTickCount64()>deadline) continue; // a retune must not delay a packet into the wrong AW
                    if(!radio.send_packet(request.bytes.data()+8,request.bytes.size()-8)) throw std::runtime_error("Radio transmission failed");
                } else throw std::runtime_error("Invalid radio command");
            }
        } catch(...) { radio.StopRxLoop(); rx.join(); radio.Stop(); transport->stop(); throw; }
        radio.StopRxLoop(); rx.join(); radio.Stop(); transport->stop(); return rx_failed?3:0;
    } catch(const std::exception& error) { fprintf(stderr,"%s\n",error.what()); return 3; }
}
