#pragma once
#include <windows.h>
// Dedicated message thread: the hook never waits for search or painting.
class WinSpaceShortcut {
    HANDLE thread_=nullptr,ready_=nullptr;DWORD id_=0;HWND target_=nullptr;bool installed_=false;
    inline static thread_local WinSpaceShortcut* self_=nullptr;
    bool keys_[256]{};bool captured_=false;
    static LRESULT CALLBACK callback(int code,WPARAM message,LPARAM data){
        if(code<0)return CallNextHookEx(nullptr,code,message,data);
        auto& s=*self_;auto& key=*reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        constexpr ULONG_PTR MaskTag=0x53504C54;
        if(key.dwExtraInfo==MaskTag)return CallNextHookEx(nullptr,code,message,data);
        bool down=message==WM_KEYDOWN||message==WM_SYSKEYDOWN;
        if(key.vkCode<256)s.keys_[key.vkCode]=down;
        bool win=s.keys_[VK_LWIN]||s.keys_[VK_RWIN];
        bool other=s.keys_[VK_LCONTROL]||s.keys_[VK_RCONTROL]||s.keys_[VK_LMENU]||s.keys_[VK_RMENU]||s.keys_[VK_LSHIFT]||s.keys_[VK_RSHIFT];
        if(key.vkCode==VK_SPACE){
            if(!down && s.captured_){s.captured_=false;return 1;}
            if(down && (s.captured_ || (win&&!other))){
                if(!s.captured_){
                    s.captured_=true;
                    // A neutral key masks the bare-Windows Start-menu action.
                    INPUT mask[2]{};for(auto& i:mask){i.type=INPUT_KEYBOARD;i.ki.wVk=0xE8;i.ki.dwExtraInfo=MaskTag;}
                    mask[1].ki.dwFlags=KEYEVENTF_KEYUP;SendInput(2,mask,sizeof(INPUT));
                    PostMessageW(s.target_,WM_HOTKEY,1,0);
                }
                return 1;
            }
        }
        return CallNextHookEx(nullptr,code,message,data);
    }
    static DWORD WINAPI run(void* context){
        auto& s=*static_cast<WinSpaceShortcut*>(context);self_=&s;
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
        HHOOK hook=SetWindowsHookExW(WH_KEYBOARD_LL,callback,GetModuleHandleW(nullptr),0);
        s.installed_=hook!=nullptr;SetEvent(s.ready_);
        if(hook){while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}UnhookWindowsHookEx(hook);}
        return 0;
    }
public:
    bool start(HWND target){
        if(thread_)return installed_;target_=target;ready_=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ready_)return false;
        thread_=CreateThread(nullptr,64*1024,run,this,0,&id_);
        if(thread_)WaitForSingleObject(ready_,INFINITE);
        CloseHandle(ready_);ready_=nullptr;
        if(!installed_)stop();return installed_;
    }
    void stop(){if(thread_){PostThreadMessageW(id_,WM_QUIT,0,0);WaitForSingleObject(thread_,INFINITE);CloseHandle(thread_);thread_=nullptr;}installed_=false;captured_=false;for(auto& k:keys_)k=false;}
    ~WinSpaceShortcut(){stop();}
};
