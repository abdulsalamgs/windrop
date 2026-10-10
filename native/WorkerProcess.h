// SPDX-License-Identifier: MIT
#pragma once
#include "Paths.h"
#include <winsock2.h>
#include <windows.h>
#include "ipc.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Anonymous pipes are inherited only by our child process. No network listener,
// shared-memory DMA mapping, or unauthenticated system-wide pipe is exposed.
class WorkerProcess {
    HANDLE job_=nullptr;
    PROCESS_INFORMATION child_{};
    FILE* input_=nullptr;
    FILE* output_=nullptr;
    std::thread reader_,writer_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<windrop::Record> received_,sending_;
    bool stopped_=false;
    std::string fault_;
    void fault(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_); if(fault_.empty()) fault_=message; stopped_=true; changed_.notify_all();
    }
    static FILE* stream(HANDLE handle,int flags,const char* mode) {
        int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),flags);
        if(fd<0) { CloseHandle(handle); throw std::runtime_error("Cannot open worker pipe"); }
        FILE* file=_fdopen(fd,mode);
        if(!file) { _close(fd); throw std::runtime_error("Cannot buffer worker pipe"); }
        return file;
    }
public:
    WorkerProcess(const std::wstring& filename,const std::wstring& arguments) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        HANDLE child_in=nullptr,parent_in=nullptr,parent_out=nullptr,child_out=nullptr;
        HANDLE errors=nullptr; LPPROC_THREAD_ATTRIBUTE_LIST attributes=nullptr; bool attributes_ready=false;
        try {
            if(!CreatePipe(&child_in,&parent_in,&security,65536) || !CreatePipe(&parent_out,&child_out,&security,65536) ||
                !SetHandleInformation(parent_in,HANDLE_FLAG_INHERIT,0) || !SetHandleInformation(parent_out,HANDLE_FLAG_INHERIT,0))
                throw std::runtime_error("Cannot create worker pipes");
            if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(STD_ERROR_HANDLE),GetCurrentProcess(),&errors,0,TRUE,DUPLICATE_SAME_ACCESS))
                throw std::runtime_error("Cannot inherit worker diagnostics stream");
            SIZE_T size=0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
            attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(),0,size));
            if(!attributes || !InitializeProcThreadAttributeList(attributes,1,0,&size)) throw std::runtime_error("Cannot configure worker process");
            attributes_ready=true;
            HANDLE inherited[]={child_in,child_out,errors};
            if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))
                throw std::runtime_error("Cannot restrict inherited worker handles");
            job_=CreateJobObjectW(nullptr,nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if(!job_ || !SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error("Cannot contain worker worker");
            auto exe=executable_directory()/filename;
            std::wstring command=L"\""+exe.wstring()+L"\" "+arguments;
            STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.lpAttributeList=attributes;
            startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput=child_in; startup.StartupInfo.hStdOutput=child_out; startup.StartupInfo.hStdError=errors;
            if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,
                nullptr,exe.parent_path().c_str(),&startup.StartupInfo,&child_)) throw std::runtime_error("Cannot start worker worker: "+std::to_string(GetLastError()));
            if(!AssignProcessToJobObject(job_,child_.hProcess)) { TerminateProcess(child_.hProcess,3); throw std::runtime_error("Cannot contain worker worker"); }
            if(ResumeThread(child_.hThread)==static_cast<DWORD>(-1)) throw std::runtime_error("Cannot resume worker worker");
            DeleteProcThreadAttributeList(attributes); HeapFree(GetProcessHeap(),0,attributes); attributes=nullptr;
            CloseHandle(child_in); child_in=nullptr; CloseHandle(child_out); child_out=nullptr; CloseHandle(errors); errors=nullptr;
            HANDLE input_handle=parent_in; parent_in=nullptr;
            input_=stream(input_handle,_O_BINARY,"wb");
            HANDLE output_handle=parent_out; parent_out=nullptr;
            output_=stream(output_handle,_O_BINARY,"rb");
            reader_=std::thread([this] {
                try {
                    windrop::Record message;
                    while(windrop::read_record(output_,message)) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if(received_.size()<128) received_.push_back(std::move(message));
                        else if(message.type!=windrop::Message::Frame) throw std::runtime_error("Worker control queue overflow");
                        changed_.notify_all();
                    }
                    fault("Worker worker stopped");
                } catch(const std::exception& e) { fault(e.what()); }
            });
            writer_=std::thread([this] {
                try {
                    for(;;) {
                        windrop::Record message;
                        { std::unique_lock<std::mutex> lock(mutex_); changed_.wait(lock,[this]{return stopped_ || !sending_.empty();});
                          if(stopped_) return; message=std::move(sending_.front()); sending_.pop_front(); }
                        windrop::write_record(input_,message.type,message.bytes.data(),message.bytes.size());
                    }
                } catch(const std::exception& e) { fault(e.what()); }
            });
        } catch(...) {
            if(attributes) { if(attributes_ready) DeleteProcThreadAttributeList(attributes); HeapFree(GetProcessHeap(),0,attributes); }
            if(child_in) CloseHandle(child_in); if(child_out) CloseHandle(child_out); if(errors) CloseHandle(errors);
            if(parent_in) CloseHandle(parent_in); if(parent_out) CloseHandle(parent_out);
            shutdown(); throw;
        }
    }
    ~WorkerProcess() { shutdown(); }
    WorkerProcess(const WorkerProcess&)=delete;
    WorkerProcess& operator=(const WorkerProcess&)=delete;
    void shutdown() noexcept {
        if(child_.hProcess && writer_.joinable()) {
            try { send(windrop::Message::Stop,{}); WaitForSingleObject(child_.hProcess,2000); } catch(...) {}
        }
        { std::lock_guard<std::mutex> lock(mutex_); stopped_=true; changed_.notify_all(); }
        if(child_.hProcess) TerminateProcess(child_.hProcess,0); // release all resources owned by the child
        if(writer_.joinable()) { CancelSynchronousIo(writer_.native_handle()); writer_.join(); }
        if(reader_.joinable()) { CancelSynchronousIo(reader_.native_handle()); reader_.join(); }
        if(input_) { fclose(input_); input_=nullptr; } if(output_) { fclose(output_); output_=nullptr; }
        if(child_.hThread) { CloseHandle(child_.hThread); child_.hThread=nullptr; }
        if(child_.hProcess) { CloseHandle(child_.hProcess); child_.hProcess=nullptr; }
        if(job_) { CloseHandle(job_); job_=nullptr; }
    }
    bool next(windrop::Record& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!fault_.empty()) throw std::runtime_error(fault_);
        if(received_.empty()) return false;
        message=std::move(received_.front()); received_.pop_front(); return true;
    }
    bool send(windrop::Message type,std::vector<uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!fault_.empty()) throw std::runtime_error(fault_);
        if(sending_.size()>=32) return false;
        sending_.push_back({type,std::move(bytes)}); changed_.notify_all(); return true;
    }
};
