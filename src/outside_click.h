#pragma once
#include <windows.h>
#include <atomic>
// Installs a mouse hook only while the panel is visible. No polling or input logging.
class OutsideClickDismiss {
    HANDLE thread_=nullptr;DWORD id_=0;HWND target_=nullptr;UINT notification_=0;
    std::atomic<bool> enabled_=false;
    std::atomic<ULONG_PTR> epoch_=0;
    inline static thread_local OutsideClickDismiss* self_=nullptr;
    static constexpr UINT Update=WM_APP+20;
    static LRESULT CALLBACK callback(int code,WPARAM message,LPARAM data){
        if(code==HC_ACTION && (message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN||message==WM_XBUTTONDOWN)){
            auto& s=*self_;auto token=s.epoch_.load();
            if(s.enabled_.load() && IsWindowVisible(s.target_)){
                const auto& input=*reinterpret_cast<MSLLHOOKSTRUCT*>(data);RECT rect{};
                if(GetWindowRect(s.target_,&rect) && !PtInRect(&rect,input.pt))
                    PostMessageW(s.target_,s.notification_,token,0);
            }
        }
        // The original click must still reach the desktop or other application.
        return CallNextHookEx(nullptr,code,message,data);
    }
    struct Start {OutsideClickDismiss* self;HANDLE ready;};
    static DWORD WINAPI run(void* context){
        auto start=*static_cast<Start*>(context);auto& s=*start.self;self_=&s;
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);SetEvent(start.ready);
        HHOOK hook=nullptr;
        while(GetMessageW(&message,nullptr,0,0)>0){
            if(message.message==Update){
                if(s.enabled_.load() && !hook)hook=SetWindowsHookExW(WH_MOUSE_LL,callback,GetModuleHandleW(nullptr),0);
                else if(!s.enabled_.load() && hook){UnhookWindowsHookEx(hook);hook=nullptr;}
            }else{TranslateMessage(&message);DispatchMessageW(&message);}
        }
        if(hook)UnhookWindowsHookEx(hook);return 0;
    }
public:
    OutsideClickDismiss()=default;
    OutsideClickDismiss(const OutsideClickDismiss&)=delete;
    OutsideClickDismiss& operator=(const OutsideClickDismiss&)=delete;
    bool start(HWND target,UINT notification){
        if(thread_)return true;target_=target;notification_=notification;
        HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ready)return false;
        Start start{this,ready};thread_=CreateThread(nullptr,64*1024,run,&start,0,&id_);
        if(thread_)WaitForSingleObject(ready,INFINITE);
        CloseHandle(ready);return thread_!=nullptr;
    }
    void visible(bool enabled){++epoch_;enabled_=enabled;if(thread_)PostThreadMessageW(id_,Update,0,0);}
    bool current(ULONG_PTR token)const{return enabled_.load() && epoch_.load()==token;}
    void stop(){
        visible(false);
        if(thread_){PostThreadMessageW(id_,WM_QUIT,0,0);WaitForSingleObject(thread_,INFINITE);CloseHandle(thread_);thread_=nullptr;}
    }
    ~OutsideClickDismiss(){stop();}
};
