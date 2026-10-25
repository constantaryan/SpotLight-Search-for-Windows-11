#include <windows.h>
#include <cwchar>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int){
    wchar_t path[32768]{};
    if(!GetModuleFileNameW(nullptr,path,32700))return 1;
    wcscat_s(path,L".launched");
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return 2;
    DWORD written=0;BOOL ok=WriteFile(file,"ok",2,&written,nullptr);
    CloseHandle(file);return ok && written==2?0:3;
}
