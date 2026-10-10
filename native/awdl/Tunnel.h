// SPDX-License-Identifier: GPL-3.0-or-later
// Uses Wintun's API under its MIT option. Loader and adapter logic are original.
#pragma once
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wintun.h>
#include <filesystem>
#include <stdexcept>
#include <array>
#include <vector>
#include <string>

inline std::filesystem::path executable_directory() {
    std::array<wchar_t,32768> path{};
    DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n || n==path.size()) throw std::runtime_error("Cannot locate native worker");
    return std::filesystem::path(path.data()).parent_path();
}
class Tunnel {
    HMODULE library_=nullptr;
    WINTUN_ADAPTER_HANDLE adapter_=nullptr;
    WINTUN_SESSION_HANDLE session_=nullptr;
    WINTUN_CREATE_ADAPTER_FUNC* create_=nullptr;
    WINTUN_CLOSE_ADAPTER_FUNC* close_=nullptr;
    WINTUN_GET_ADAPTER_LUID_FUNC* luid_=nullptr;
    WINTUN_START_SESSION_FUNC* start_=nullptr;
    WINTUN_END_SESSION_FUNC* end_=nullptr;
    WINTUN_RECEIVE_PACKET_FUNC* receive_=nullptr;
    WINTUN_RELEASE_RECEIVE_PACKET_FUNC* release_=nullptr;
    WINTUN_ALLOCATE_SEND_PACKET_FUNC* allocate_=nullptr;
    WINTUN_SEND_PACKET_FUNC* send_=nullptr;
    NET_LUID interface_{};
    MIB_UNICASTIPADDRESS_ROW address_{};
    bool address_created_=false;
    template<class T> void load(T& pointer,const char* name) {
        pointer=reinterpret_cast<T>(GetProcAddress(library_,name));
        if(!pointer) throw std::runtime_error("Incompatible Wintun DLL");
    }
    void cleanup() noexcept {
        if(address_created_) DeleteUnicastIpAddressEntry(&address_);
        if(session_ && end_) end_(session_);
        if(adapter_ && close_) close_(adapter_);
        if(library_) FreeLibrary(library_);
    }
    static void check(DWORD error,const char* operation) {
        if(error!=NO_ERROR) throw std::runtime_error(std::string(operation)+": Windows error "+std::to_string(error));
    }
public:
    explicit Tunnel(const std::array<uint8_t,6>& mac) {
        try {
            NET_LUID existing{};
            if(ConvertInterfaceAliasToLuid(L"awdl0",&existing)==NO_ERROR)
                throw std::runtime_error("awdl0 already exists; stop its owner before starting Windrop");
            auto dll=executable_directory()/L"wintun.dll";
            library_=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!library_) throw std::runtime_error("Cannot load packaged Wintun DLL");
            load(create_,"WintunCreateAdapter"); load(close_,"WintunCloseAdapter"); load(luid_,"WintunGetAdapterLUID");
            load(start_,"WintunStartSession"); load(end_,"WintunEndSession"); load(receive_,"WintunReceivePacket");
            load(release_,"WintunReleaseReceivePacket"); load(allocate_,"WintunAllocateSendPacket"); load(send_,"WintunSendPacket");
            // Do not take ownership of an existing user adapter. Wintun removes
            // this newly created adapter when the owning handle closes.
            adapter_=create_(L"awdl0",L"Windrop",nullptr);
            if(!adapter_) throw std::runtime_error("Cannot create awdl0 adapter: "+std::to_string(GetLastError()));
            luid_(adapter_,&interface_);
            std::array<wchar_t,IF_MAX_STRING_SIZE+1> alias{};
            check(ConvertInterfaceLuidToAlias(&interface_,alias.data(),alias.size()),"Read AWDL adapter name");
            if(std::wstring(alias.data())!=L"awdl0") throw std::runtime_error("awdl0 name was taken during startup");
            session_=start_(adapter_,0x400000);
            if(!session_) throw std::runtime_error("Cannot start Wintun session");
            MIB_IPINTERFACE_ROW properties{}; InitializeIpInterfaceEntry(&properties);
            properties.Family=AF_INET6; properties.InterfaceLuid=interface_;
            check(GetIpInterfaceEntry(&properties),"Read awdl0 IPv6 settings");
            properties.NlMtu=1450; properties.RouterDiscoveryBehavior=RouterDiscoveryDisabled;
            properties.DisableDefaultRoutes=TRUE; properties.LinkLocalAddressBehavior=LinkLocalAlwaysOff;
            properties.DadTransmits=0;
            check(SetIpInterfaceEntry(&properties),"Configure awdl0 IPv6");
            InitializeUnicastIpAddressEntry(&address_);
            address_.InterfaceLuid=interface_; address_.Address.Ipv6.sin6_family=AF_INET6;
            auto a=address_.Address.Ipv6.sin6_addr.u.Byte;
            a[0]=0xfe; a[1]=0x80; a[8]=mac[0]^2; a[9]=mac[1]; a[10]=mac[2];
            a[11]=0xff; a[12]=0xfe; a[13]=mac[3]; a[14]=mac[4]; a[15]=mac[5];
            address_.OnLinkPrefixLength=64; address_.PrefixOrigin=IpPrefixOriginManual;
            address_.SuffixOrigin=IpSuffixOriginManual; address_.ValidLifetime=0xffffffff;
            address_.PreferredLifetime=0xffffffff; address_.DadState=IpDadStatePreferred;
            check(CreateUnicastIpAddressEntry(&address_),"Assign AWDL link-local address"); address_created_=true;
        } catch(...) { cleanup(); throw; }
    }
    ~Tunnel() { cleanup(); }
    Tunnel(const Tunnel&)=delete;
    Tunnel& operator=(const Tunnel&)=delete;
    bool read(std::vector<uint8_t>& bytes) {
        DWORD size=0; uint8_t* packet=receive_(session_,&size);
        if(!packet) {
            DWORD error=GetLastError(); if(error==ERROR_NO_MORE_ITEMS) return false;
            throw std::runtime_error("Wintun receive failed: "+std::to_string(error));
        }
        try { if(size<=1450) bytes.assign(packet,packet+size); else bytes.clear(); }
        catch(...) { release_(session_,packet); throw; }
        release_(session_,packet); return true;
    }
    void write(const uint8_t* data,size_t size) {
        uint8_t* packet=allocate_(session_,static_cast<DWORD>(size));
        if(!packet) {
            if(GetLastError()==ERROR_BUFFER_OVERFLOW) return; // bounded ring drops under pressure
            throw std::runtime_error("Wintun send failed");
        }
        memcpy(packet,data,size); send_(session_,packet);
    }
};
