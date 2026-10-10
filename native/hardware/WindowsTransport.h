// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <winioctl.h>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
#include "Transport.h"
#include "../../drivers/Windrop.Radio/protocol.h"

class WindowsTransport final : public devourer::ITransport {
    HANDLE device_=INVALID_HANDLE_VALUE;
    void call(DWORD code, void* input=nullptr, DWORD size=0, void* output=nullptr, DWORD output_size=0, DWORD* read=nullptr) {
        DWORD returned=0;
        if(!DeviceIoControl(device_,code,input,size,output,output_size,&returned,nullptr))
            throw std::runtime_error("Windows radio IOCTL failed: "+std::to_string(GetLastError()));
        if(read) *read=returned;
    }
    uint32_t reg(uint32_t offset, uint32_t width, bool write, uint32_t value=0) {
        WINDROP_REGISTER operation={offset,width,write?1u:0u,value}; DWORD n;
        call(WINDROP_IOCTL_REGISTER,&operation,sizeof(operation),&operation,sizeof(operation),&n);
        if(n!=sizeof(operation)) throw std::runtime_error("Invalid register reply");
        return operation.Value;
    }
public:
    WindowsTransport() {
        HDEVINFO list=SetupDiGetClassDevsW(&GUID_DEVINTERFACE_WINDROP_RADIO,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        if(list==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate native radio");
        SP_DEVICE_INTERFACE_DATA data{}; data.cbSize=sizeof(data);
        DWORD needed=0;
        if(SetupDiEnumDeviceInterfaces(list,nullptr,&GUID_DEVINTERFACE_WINDROP_RADIO,0,&data)) {
            SetupDiGetDeviceInterfaceDetailW(list,&data,nullptr,0,&needed,nullptr);
            if(needed>=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) && needed<=65536) {
                std::vector<uint8_t> storage(needed);
                auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
                detail->cbSize=sizeof(*detail);
                if(SetupDiGetDeviceInterfaceDetailW(list,&data,detail,needed,nullptr,nullptr))
                    device_=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            }
        }
        SetupDiDestroyDeviceInfoList(list);
        if(device_==INVALID_HANDLE_VALUE) throw std::runtime_error("Windrop RTL8852CE driver unavailable; a signed, installed driver and administrator access are required");
        try {
            WINDROP_RADIO_INFO info{}; DWORD n;
            call(WINDROP_IOCTL_GET_INFO,nullptr,0,&info,sizeof(info),&n);
            if(n!=sizeof(info) || info.Size!=sizeof(info) || info.Version!=2 || info.VendorId!=0x10ec || info.DeviceId!=0xc852 ||
                (info.Capabilities&3)!=3) throw std::runtime_error("Incompatible Windrop radio driver");
        } catch(...) { CloseHandle(device_); device_=INVALID_HANDLE_VALUE; throw; }
    }
    ~WindowsTransport() override { if(device_!=INVALID_HANDLE_VALUE) CloseHandle(device_); }
    bool is_usb() const override { return false; }
    // Conservatively silence LTR. The Windows bus has not granted us an LTR contract.
    bool ltr_allowed() override { return false; }
    uint8_t read8(uint16_t r) override { return static_cast<uint8_t>(reg(r,1,false)); }
    uint16_t read16(uint16_t r) override { return static_cast<uint16_t>(reg(r,2,false)); }
    uint32_t read32(uint16_t r) override { return reg(r,4,false); }
    uint32_t read32_wide(uint32_t r) override { return reg(r,4,false); }
    bool write8(uint16_t r,uint8_t v) override { reg(r,1,true,v); return true; }
    bool write16(uint16_t r,uint16_t v) override { reg(r,2,true,v); return true; }
    bool write32(uint16_t r,uint32_t v) override { reg(r,4,true,v); return true; }
    bool write32_wide(uint32_t r,uint32_t v) override { reg(r,4,true,v); return true; }
    bool write_bytes(uint16_t r,const uint8_t* p,size_t n) override {
        if(n>65536u-r) throw std::runtime_error("Register block overflow");
        for(size_t i=0;i<n;i++) write8(static_cast<uint16_t>(r+i),p[i]); return true;
    }
    void hci_setup() override { call(WINDROP_IOCTL_SETUP); }
    void stop() { call(WINDROP_IOCTL_STOP); }
    int tx_sync(uint8_t channel,uint8_t* p,size_t n,int) override {
        if(n>WINDROP_TX_MAX) throw std::runtime_error("TX buffer too large");
        std::vector<uint8_t> bytes(sizeof(WINDROP_TX_HEADER)+n);
        WINDROP_TX_HEADER header={channel,static_cast<ULONG>(n)};
        memcpy(bytes.data(),&header,sizeof(header)); memcpy(bytes.data()+sizeof(header),p,n);
        call(WINDROP_IOCTL_TRANSMIT,bytes.data(),static_cast<DWORD>(bytes.size())); return static_cast<int>(n);
    }
    bool tx_async(uint8_t q,uint8_t* p,size_t n,unsigned timeout) override { return tx_sync(q,p,n,static_cast<int>(timeout))>=0; }
    void rx_loop(int,int,const std::function<void(const uint8_t*,int)>& on_data,const std::function<bool()>& stop_requested) override {
        std::vector<uint8_t> bytes(WINDROP_FRAME_MAX);
        while(!stop_requested()) {
            DWORD n; call(WINDROP_IOCTL_RECEIVE,nullptr,0,bytes.data(),static_cast<DWORD>(bytes.size()),&n);
            if(n) on_data(bytes.data(),static_cast<int>(n)); else Sleep(1);
        }
    }
};
