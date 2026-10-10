// SPDX-License-Identifier: GPL-2.0-only
// AX ring layout and descriptor encodings adapted from OpenIPC/devourer
// f369aac553c9ce884901115ef9e66509f93c5683, src/PcieDmaAx.cpp (GPL-2.0).
// Windows port: kernel-owned KMDF common buffer, bounded copies and polling.
#include "radio.h"

typedef struct _TX_REGS { USHORT Num, Index, Bdram, Base; UCHAR Start, Max, Min; } TX_REGS;
static const TX_REGS Tx[13] = {
    {0x1024,0x1058,0x1300,0x1230,0,5,2}, {0x1026,0x105c,0x1304,0x1238,5,5,2},
    {0x1028,0x1060,0x1308,0x1240,10,5,2}, {0x102a,0x1064,0x130c,0x1248,15,5,2},
    {0x102c,0x1068,0x1310,0x1250,20,5,2}, {0x102e,0x106c,0x1314,0x1258,25,5,2},
    {0x1030,0x1070,0x1318,0x1260,30,5,2}, {0x1032,0x1074,0x131c,0x1268,35,5,2},
    {0x1034,0x1078,0x1320,0x1270,40,5,1}, {0x1036,0x107c,0x1324,0x1278,45,5,1},
    {0x1438,0x11d0,0x1420,0x1458,50,5,1}, {0x143a,0x11d4,0x1424,0x1460,55,5,1},
    {0x1038,0x1080,0x1328,0x1280,60,4,1}
};

ULONG RadioRead(DEVICE_CONTEXT* c, ULONG offset, ULONG width)
{
    if (width == 1) return READ_REGISTER_UCHAR(c->Registers + offset);
    if (width == 2) return READ_REGISTER_USHORT((volatile USHORT*)(c->Registers + offset));
    return READ_REGISTER_ULONG((volatile ULONG*)(c->Registers + offset));
}
VOID RadioWrite(DEVICE_CONTEXT* c, ULONG offset, ULONG width, ULONG value)
{
    if (width == 1) WRITE_REGISTER_UCHAR(c->Registers + offset, (UCHAR)value);
    else if (width == 2) WRITE_REGISTER_USHORT((volatile USHORT*)(c->Registers + offset), (USHORT)value);
    else WRITE_REGISTER_ULONG((volatile ULONG*)(c->Registers + offset), value);
}
static ULONG Le32(const UCHAR* p) { return p[0] | ((ULONG)p[1]<<8) | ((ULONG)p[2]<<16) | ((ULONG)p[3]<<24); }
static VOID Put16(UCHAR* p, ULONG v) { p[0]=(UCHAR)v; p[1]=(UCHAR)(v>>8); }
static VOID Put32(UCHAR* p, ULONG v) { Put16(p,v); Put16(p+2,v>>16); }
static VOID Bd(UCHAR* p, ULONG length, ULONG option, ULONG address)
{ Put16(p,length); Put16(p+2,option); Put32(p+4,address); }
static VOID Pause(VOID) { LARGE_INTEGER delay; delay.QuadPart=-1000; KeDelayExecutionThread(KernelMode,FALSE,&delay); }
static ULONG HardwareIndex(DEVICE_CONTEXT* c, USHORT reg)
{ ULONG v=RadioRead(c,reg,4); ULONG i=(v>>16)&0xfff; return v==MAXULONG || i>=RADIO_BDS ? RADIO_BDS : i; }
static VOID Arm(DEVICE_CONTEXT* c, ULONG bd, ULONG buffer, ULONG i)
{ Bd(c->Slab+bd+i*8,RADIO_RX_BYTES,0,c->DmaBase+buffer+i*RADIO_RX_BYTES); }

BOOLEAN RadioProtectedRegister(ULONG offset, ULONG width)
{
    ULONG i;
    // Ring base addresses belong to the driver; accepting one from user space
    // would let the NIC DMA into unrelated memory. Protect both low/high words.
    for(i=0;i<13;i++) if(offset < (ULONG)Tx[i].Base+8 && offset+width > Tx[i].Base) return TRUE;
    return (offset<0x1228 && offset+width>0x1220) || (offset<0x1230 && offset+width>0x1228);
}

NTSTATUS RadioStop(DEVICE_CONTEXT* c)
{
    ULONG i, v;
    if(c->Registers == NULL) return STATUS_SUCCESS;
    v=RadioRead(c,0x1000,4);
    if(v!=MAXULONG) {
        RadioWrite(c,0x1000,4,(v|(1u<<17))&~((1u<<7)|(1u<<15)));
        RadioWrite(c,0x10b0,4,0); RadioWrite(c,0x30b0,4,0);
        for(i=0;i<1000;i++) {
            if((RadioRead(c,0x101c,4)&0x7ff00)==0 && (RadioRead(c,0x11c8,4)&3)==0 && (RadioRead(c,0x1208,4)&3)==0) break;
            Pause();
        }
        if(i==1000) return STATUS_IO_TIMEOUT;
    }
    c->Rings=FALSE; c->AssemblyLength=0;
    return STATUS_SUCCESS;
}

NTSTATUS RadioSetup(DEVICE_CONTEXT* c)
{
    ULONG i;
    // HAL calls this after stopping HAXI and clearing its indices. Verify idle
    // ourselves before replacing any descriptor or resetting page ownership.
    if((RadioRead(c,0x1000,4)&(1u<<17))==0 || (RadioRead(c,0x101c,4)&0x7ff00)!=0 ||
        (RadioRead(c,0x11c8,4)&3)!=0 || (RadioRead(c,0x1208,4)&3)!=0) return STATUS_DEVICE_BUSY;
    RtlZeroMemory(c->Slab,RADIO_DMA_BYTES);
    RtlZeroMemory(c->Busy,sizeof(c->Busy));
    RtlZeroMemory(c->TxWrite,sizeof(c->TxWrite));
    c->RxWrite=0; c->ReportWrite=0; c->AssemblyLength=0;
    for(i=0;i<13;i++) {
        RadioWrite(c,Tx[i].Num,2,RADIO_BDS);
        RadioWrite(c,Tx[i].Bdram,4,Tx[i].Start|((ULONG)Tx[i].Max<<8)|((ULONG)Tx[i].Min<<16));
        RadioWrite(c,Tx[i].Base,4,c->DmaBase+i*RADIO_BDS*8);
        RadioWrite(c,Tx[i].Base+4,4,0);
        RadioWrite(c,Tx[i].Index,2,0);
    }
    for(i=0;i<RADIO_BDS;i++) { Arm(c,RADIO_RXQ_BD,RADIO_RXQ,i); Arm(c,RADIO_RPQ_BD,RADIO_RPQ,i); }
    RadioWrite(c,0x1210,2,RADIO_BDS); RadioWrite(c,0x1212,2,RADIO_BDS);
    RadioWrite(c,0x1220,4,c->DmaBase+RADIO_RXQ_BD); RadioWrite(c,0x1224,4,0);
    RadioWrite(c,0x1228,4,c->DmaBase+RADIO_RPQ_BD); RadioWrite(c,0x122c,4,0);
    RadioWrite(c,0x1218,2,0); RadioWrite(c,0x121c,2,0);
    KeMemoryBarrier(); c->Rings=TRUE;
    return STATUS_SUCCESS;
}

static VOID ReapReports(DEVICE_CONTEXT* c)
{
    ULONG hw=HardwareIndex(c,0x121c), count=0;
    KeMemoryBarrier();
    while(hw<RADIO_BDS && c->ReportWrite!=hw && count++<RADIO_BDS) {
        const UCHAR* p=c->Slab+RADIO_RPQ+c->ReportWrite*RADIO_RX_BYTES;
        ULONG info=Le32(p), bytes=info&0x3fff;
        if((info&0xc000)==0xc000 && bytes>=20 && bytes<=RADIO_RX_BYTES) {
            ULONG d=Le32(p+4), payload=d&0x3fff, type=(d>>24)&15;
            ULONG start=4+((d>>31)?32u:16u)+((d>>28)&7)*16+((d>>14)&3)*2;
            if((d&(1u<<23)) && type!=1) start+=4;
            if(start<=bytes && payload<=bytes-start && type==7) {
                ULONG pos;
                for(pos=0;pos+4<=payload;pos+=4) {
                    ULONG seq=(Le32(p+start+pos)>>16)&0x7fff;
                    if(seq<RADIO_PAGES) c->Busy[seq]=FALSE;
                }
            }
        }
        Arm(c,RADIO_RPQ_BD,RADIO_RPQ,c->ReportWrite);
        c->ReportWrite=(c->ReportWrite+1)%RADIO_BDS;
    }
    KeMemoryBarrier(); RadioWrite(c,0x121c,2,c->ReportWrite);
}

NTSTATUS RadioTransmit(DEVICE_CONTEXT* c, ULONG channel, const UCHAR* data, ULONG length)
{
    ULONG hw, wp, address, pageLength, i;
    UCHAR* target;
    if(!c->Rings) return STATUS_DEVICE_NOT_READY;
    if(channel>=13) return STATUS_INVALID_PARAMETER;
    ReapReports(c);
    hw=HardwareIndex(c,Tx[channel].Index); wp=c->TxWrite[channel];
    if(hw>=RADIO_BDS) return STATUS_DEVICE_NOT_CONNECTED;
    if((wp+1)%RADIO_BDS==hw) return STATUS_DEVICE_BUSY;
    if(channel==12) {
        if(length<16 || length>RADIO_FW_BYTES) return STATUS_INVALID_BUFFER_SIZE;
        target=c->Slab+RADIO_FW+wp*RADIO_FW_BYTES;
        RtlCopyMemory(target,data,length);
        address=c->DmaBase+RADIO_FW+wp*RADIO_FW_BYTES; pageLength=length;
    } else {
        ULONG dw0, desc, payload, page, n=0, remain, dma;
        UCHAR* ai;
        if(length<32) return STATUS_INVALID_BUFFER_SIZE;
        dw0=Le32(data); desc=32+((dw0&(1u<<22))?24u:0u);
        if(length<=desc || length-desc>RADIO_PAYLOAD_BYTES) return STATUS_INVALID_BUFFER_SIZE;
        payload=length-desc;
        for(page=0;page<RADIO_PAGES && c->Busy[page];page++) { }
        if(page==RADIO_PAGES) return STATUS_DEVICE_BUSY;
        c->Busy[page]=TRUE;
        target=c->Slab+RADIO_WD+page*RADIO_WD_BYTES;
        RtlZeroMemory(target,RADIO_WD_BYTES); RtlCopyMemory(target,data,desc);
        RtlCopyMemory(c->Slab+RADIO_PAYLOAD+page*RADIO_PAYLOAD_BYTES,data+desc,payload);
        Put16(target+desc,page|0x8000);
        ai=target+desc+8; remain=payload; dma=c->DmaBase+RADIO_PAYLOAD+page*RADIO_PAYLOAD_BYTES;
        while(remain) {
            ULONG chunk=min(remain,2044u); remain-=chunk;
            Put16(ai,chunk|(remain==0?0x8000u:0u)); Put32(ai+2,dma);
            ai+=6; n++; dma+=chunk;
        }
        Put32(target,(dw0|(1u<<7))&~(1u<<10));
        Put32(target+4,(Le32(target+4)&~(0x3fu<<26))|(n<<26));
        address=c->DmaBase+RADIO_WD+page*RADIO_WD_BYTES; pageLength=desc+8+n*6;
    }
    Bd(c->Slab+channel*RADIO_BDS*8+wp*8,pageLength,1u<<14,address);
    KeMemoryBarrier(); c->TxWrite[channel]=(USHORT)((wp+1)%RADIO_BDS);
    RadioWrite(c,Tx[channel].Index,2,c->TxWrite[channel]);
    // A timeout does not release/reuse a WD page. Only an RPQ release does.
    for(i=0;i<200;i++) {
        hw=HardwareIndex(c,Tx[channel].Index);
        if(hw==c->TxWrite[channel]) return STATUS_SUCCESS;
        if(hw>=RADIO_BDS) return STATUS_DEVICE_NOT_CONNECTED;
        Pause();
    }
    return STATUS_IO_TIMEOUT;
}

NTSTATUS RadioReceive(DEVICE_CONTEXT* c, UCHAR* output, ULONG* length)
{
    ULONG hw, count=0;
    *length=0;
    if(!c->Rings) return STATUS_DEVICE_NOT_READY;
    ReapReports(c); hw=HardwareIndex(c,0x1218);
    if(hw>=RADIO_BDS) return STATUS_DEVICE_NOT_CONNECTED;
    KeMemoryBarrier();
    while(c->RxWrite!=hw && count++<RADIO_BDS) {
        const UCHAR* p=c->Slab+RADIO_RXQ+c->RxWrite*RADIO_RX_BYTES;
        ULONG info=Le32(p), bytes=info&0x3fff;
        BOOLEAN first=(info&0x8000)!=0, last=(info&0x4000)!=0;
        if(first) c->AssemblyLength=0;
        if(bytes>4 && bytes<=RADIO_RX_BYTES && (first || c->AssemblyLength)) {
            ULONG part=bytes-4;
            if(part<=WINDROP_FRAME_MAX-c->AssemblyLength) {
                RtlCopyMemory(c->Assembly+c->AssemblyLength,p+4,part); c->AssemblyLength+=part;
                if(last) { *length=c->AssemblyLength; RtlCopyMemory(output,c->Assembly,*length); c->AssemblyLength=0; }
            } else c->AssemblyLength=0;
        } else c->AssemblyLength=0;
        Arm(c,RADIO_RXQ_BD,RADIO_RXQ,c->RxWrite); c->RxWrite=(c->RxWrite+1)%RADIO_BDS;
        if(*length) break;
    }
    KeMemoryBarrier(); RadioWrite(c,0x1218,2,c->RxWrite);
    return STATUS_SUCCESS;
}
