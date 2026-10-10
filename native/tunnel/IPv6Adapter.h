// SPDX-License-Identifier: MIT
// Original generic IPv6 adapter implementation using the MIT Wintun API header.
#pragma once
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wintun.h>
#include "../Paths.h"
#include <stdexcept>
#include <array>
#include <vector>
#include <string>

class IPv6Adapter {
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
    uint32_t mtu_=0;
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
    IPv6Adapter(const std::wstring& name,const IN6_ADDR& ip,uint32_t mtu) : mtu_(mtu) {
        try {
            NET_LUID existing{};
            if(ConvertInterfaceAliasToLuid(name.c_str(),&existing)==NO_ERROR)
                throw std::runtime_error("IPv6 tunnel already exists; stop its owner before starting Windrop");
            auto dll=executable_directory()/L"wintun.dll";
            library_=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!library_) throw std::runtime_error("Cannot load packaged Wintun DLL");
            load(create_,"WintunCreateAdapter"); load(close_,"WintunCloseAdapter"); load(luid_,"WintunGetAdapterLUID");
            load(start_,"WintunStartSession"); load(end_,"WintunEndSession"); load(receive_,"WintunReceivePacket");
            load(release_,"WintunReleaseReceivePacket"); load(allocate_,"WintunAllocateSendPacket"); load(send_,"WintunSendPacket");
            // Do not take ownership of an existing user adapter. Wintun removes
            // this newly created adapter when the owning handle closes.
            adapter_=create_(name.c_str(),L"Windrop IPv6",nullptr);
            if(!adapter_) throw std::runtime_error("Cannot create IPv6 tunnel adapter: "+std::to_string(GetLastError()));
            luid_(adapter_,&interface_);
            std::array<wchar_t,IF_MAX_STRING_SIZE+1> alias{};
            check(ConvertInterfaceLuidToAlias(&interface_,alias.data(),alias.size()),"Read IPv6 adapter name");
            if(std::wstring(alias.data())!=name) throw std::runtime_error("IPv6 tunnel name was taken during startup");
            session_=start_(adapter_,0x400000);
            if(!session_) throw std::runtime_error("Cannot start Wintun session");
            MIB_IPINTERFACE_ROW properties{}; InitializeIpInterfaceEntry(&properties);
            properties.Family=AF_INET6; properties.InterfaceLuid=interface_;
            check(GetIpInterfaceEntry(&properties),"Read IPv6 tunnel IPv6 settings");
            properties.NlMtu=mtu; properties.RouterDiscoveryBehavior=RouterDiscoveryDisabled;
            properties.DisableDefaultRoutes=TRUE; properties.LinkLocalAddressBehavior=LinkLocalUnchanged;
            properties.DadTransmits=0;
            check(SetIpInterfaceEntry(&properties),"Configure IPv6 tunnel IPv6");
            InitializeUnicastIpAddressEntry(&address_);
            address_.InterfaceLuid=interface_; address_.Address.Ipv6.sin6_family=AF_INET6;
            address_.Address.Ipv6.sin6_addr=ip;
            address_.OnLinkPrefixLength=64; address_.PrefixOrigin=IpPrefixOriginManual;
            address_.SuffixOrigin=IpSuffixOriginManual; address_.ValidLifetime=0xffffffff;
            address_.PreferredLifetime=0xffffffff; address_.DadState=IpDadStatePreferred;
            check(CreateUnicastIpAddressEntry(&address_),"Assign IPv6 link-local address"); address_created_=true;
            // Windows rejects LinkLocalAlwaysOff for this IPv6 interface. Keep
            // its supported behavior, then remove only other link-local entries
            // on this newly created, exclusively owned adapter. Discovery must
            // select the explicitly configured address, not Windows' random one.
            MIB_UNICASTIPADDRESS_TABLE* addresses=nullptr;
            check(GetUnicastIpAddressTable(AF_INET6,&addresses),"Read owned IPv6 addresses");
            struct FreeAddresses { MIB_UNICASTIPADDRESS_TABLE* table; ~FreeAddresses(){FreeMibTable(table);} } free_addresses{addresses};
            for(ULONG i=0;i<addresses->NumEntries;i++) {
                auto& row=addresses->Table[i]; const auto& other=row.Address.Ipv6.sin6_addr;
                if(row.InterfaceLuid.Value!=interface_.Value || other.u.Byte[0]!=0xfe || (other.u.Byte[1]&0xc0)!=0x80 ||
                    memcmp(&other,&ip,sizeof(ip))==0) continue;
                DWORD error=DeleteUnicastIpAddressEntry(&row);
                if(error!=ERROR_NOT_FOUND) check(error,"Remove automatically created IPv6 address");
            }
        } catch(...) { cleanup(); throw; }
    }
    ~IPv6Adapter() { cleanup(); }
    IPv6Adapter(const IPv6Adapter&)=delete;
    IPv6Adapter& operator=(const IPv6Adapter&)=delete;
    bool read(std::vector<uint8_t>& bytes) {
        DWORD size=0; uint8_t* packet=receive_(session_,&size);
        if(!packet) {
            DWORD error=GetLastError(); if(error==ERROR_NO_MORE_ITEMS) return false;
            throw std::runtime_error("Wintun receive failed: "+std::to_string(error));
        }
        try { if(size<=mtu_) bytes.assign(packet,packet+size); else bytes.clear(); }
        catch(...) { release_(session_,packet); throw; }
        release_(session_,packet); return true;
    }
    void write(const uint8_t* data,size_t size) {
        if(size<40 || size>mtu_ || (data[0]>>4)!=6 || 40+((static_cast<size_t>(data[4])<<8)|data[5])!=size)
            throw std::runtime_error("Invalid IPv6 packet");
        uint8_t* packet=allocate_(session_,static_cast<DWORD>(size));
        if(!packet) {
            if(GetLastError()==ERROR_BUFFER_OVERFLOW) return; // bounded ring drops under pressure
            throw std::runtime_error("Wintun send failed");
        }
        memcpy(packet,data,size); send_(session_,packet);
    }
};
