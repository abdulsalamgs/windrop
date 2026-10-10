// SPDX-License-Identifier: MIT
#include "IPv6Adapter.h"
#include "../ipc.h"
#include <atomic>
#include <thread>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>

// Output backpressure never blocks the adapter loop or its shutdown. Packet
// overflow drops at the bounded queue; Stop/EOF cancels an outstanding write.
class OutputPipe {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<windrop::Record> queue_;
    bool stopped_=false;
    std::string fault_;
    std::thread writer_;
public:
    OutputPipe() : writer_([this] {
        try {
            for(;;) {
                windrop::Record record;
                { std::unique_lock<std::mutex> lock(mutex_);
                  changed_.wait(lock,[this]{return stopped_ || !queue_.empty();});
                  if(stopped_) return; record=std::move(queue_.front()); queue_.pop_front(); }
                windrop::write_record(stdout,record.type,record.bytes.data(),record.bytes.size());
            }
        } catch(const std::exception& error) { std::lock_guard<std::mutex> lock(mutex_); fault_=error.what(); }
    }) {}
    ~OutputPipe() {
        { std::lock_guard<std::mutex> lock(mutex_); stopped_=true; changed_.notify_all(); }
        while(WaitForSingleObject(writer_.native_handle(),10)==WAIT_TIMEOUT) CancelSynchronousIo(writer_.native_handle());
        writer_.join();
    }
    void check() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!fault_.empty()) throw std::runtime_error(fault_);
    }
    void send(windrop::Message type,std::vector<uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!fault_.empty()) throw std::runtime_error(fault_);
        if(queue_.size()<32) { queue_.push_back({type,std::move(bytes)}); changed_.notify_one(); }
    }
};

// This is a standalone IPv6 packet adapter, with no OWL/AWDL dependency.
// stdout contains only bounded binary records; diagnostics go to stderr.
static int serve(const std::wstring& name,const IN6_ADDR& address,uint32_t mtu) {
    IPv6Adapter adapter(name,address,mtu);
    windrop::binary_stdio();
    OutputPipe output;
    output.send(windrop::Message::Hello,{});
    std::atomic<bool> stopped=false;
    std::string fault;
    std::thread input([&] {
        try {
            windrop::Record record;
            while(!stopped.load() && windrop::read_record(stdin,record)) {
                if(record.type==windrop::Message::Stop && record.bytes.empty()) break;
                if(record.type!=windrop::Message::Frame) throw std::runtime_error("Invalid IPv6 tunnel record");
                adapter.write(record.bytes.data(),record.bytes.size());
            }
        } catch(const std::exception& error) { fault=error.what(); }
        stopped.store(true);
    });
    struct JoinInput {
        std::thread& thread; std::atomic<bool>& stop;
        ~JoinInput() {
            stop.store(true);
            // Repeat cancellation to cover the race between the stop check and
            // entering fread. The adapter remains alive until this thread exits.
            while(WaitForSingleObject(thread.native_handle(),10)==WAIT_TIMEOUT) CancelSynchronousIo(thread.native_handle());
            thread.join();
        }
    } join{input,stopped};
    std::vector<uint8_t> packet;
    while(!stopped.load()) {
        output.check();
        if(adapter.read(packet)) {
            if(packet.size()>=40 && (packet[0]>>4)==6 && 40+((static_cast<size_t>(packet[4])<<8)|packet[5])==packet.size())
                output.send(windrop::Message::Frame,std::move(packet));
        } else Sleep(1);
    }
    // The release/acquire stop flag publishes the input thread's diagnostic.
    if(!fault.empty()) throw std::runtime_error(fault);
    return 0;
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--help") {
            puts("windrop-tunnel serve --name NAME --address IPV6 --mtu MTU\nStandalone administrator IPv6 adapter. Binary stdio: Hello(1,empty), Frame(2,IPv6 packet), Stop(5,empty).\nThe supplied address must be unicast link-local; MTU is 1280..32768. EOF stops and removes its adapter.");
            return 0;
        }
        if(argc!=8 || std::string(argv[1])!="serve" || std::string(argv[2])!="--name" ||
            std::string(argv[4])!="--address" || std::string(argv[6])!="--mtu") throw std::runtime_error("Use --help for IPv6 tunnel commands");
        std::string name=argv[3];
        if(name.empty() || name.size()>128 || !std::all_of(name.begin(),name.end(),[](unsigned char c){
            return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_';
        })) throw std::runtime_error("Invalid IPv6 adapter name");
        IN6_ADDR address{};
        if(InetPtonA(AF_INET6,argv[5],&address)!=1 || address.u.Byte[0]!=0xfe || (address.u.Byte[1]&0xc0)!=0x80)
            throw std::runtime_error("Invalid IPv6 address: supply a unicast link-local address");
        std::string text=argv[7]; size_t parsed=0;
        unsigned long mtu=std::stoul(text,&parsed);
        if(parsed!=text.size() || mtu<1280 || mtu>windrop::max_record) throw std::runtime_error("Invalid IPv6 MTU");
        return serve(std::wstring(name.begin(),name.end()),address,static_cast<uint32_t>(mtu));
    } catch(const std::exception& error) { fprintf(stderr,"%s\n",error.what()); return 3; }
}
