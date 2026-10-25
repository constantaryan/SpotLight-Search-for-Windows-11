#include "core.h"
#include <shlobj.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
namespace sl {
std::wstring data_dir(){
    PWSTR path=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path)))return {};
    std::wstring dir=std::wstring(path)+L"\\Spotlight";CoTaskMemFree(path);
    return dir;
}
static std::string utf8(const std::wstring& s){
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr);return out;
}
static std::wstring wide(const std::string& s){
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n);return out;
}
static bool write_atomic(const wchar_t* file,const std::string& text){
    auto dir=data_dir();if(dir.empty())return false;
    if(!CreateDirectoryW(dir.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    auto dest=dir+L"\\"+file,tmp=dest+L".tmp";
    {std::ofstream stream(std::filesystem::path(tmp),std::ios::binary|std::ios::trunc);stream<<text;stream.flush();if(!stream)return false;}
    return MoveFileExW(tmp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
}
Settings load_settings(){
    Settings s;std::ifstream f(std::filesystem::path(data_dir()+L"\\settings.txt"));
    unsigned version=0,mod=0,key=0,start=0;
    if(f>>version>>mod>>key>>start && version==1 && key>=0x20 && key<=0xFE &&
       (mod & ~(MOD_ALT|MOD_CONTROL|MOD_SHIFT|MOD_WIN))==0 && mod!=0){s.modifiers=mod;s.key=key;s.startup=start!=0;}
    return s;
}
bool save_settings(const Settings&s){return write_atomic(L"settings.txt","1 "+std::to_string(s.modifiers)+" "+std::to_string(s.key)+" "+std::to_string(s.startup)+"\n");}
std::map<std::wstring,unsigned> load_history(){
    std::map<std::wstring,unsigned> out;
    std::ifstream f(std::filesystem::path(data_dir()+L"\\history.txt"),std::ios::binary);
    f.seekg(0,std::ios::end);auto bytes=f.tellg();if(bytes<0 || bytes>1024*1024)return out;f.seekg(0);
    std::string line;
    while(out.size()<MaxHistory && std::getline(f,line)){
        auto tab=line.find('\t');if(tab==line.npos || tab>10)continue;
        unsigned value=0;try{value=static_cast<unsigned>(std::stoul(line.substr(0,tab)));}catch(...){continue;}
        auto target=wide(line.substr(tab+1));if(!target.empty())out[target]=std::min(value,100u);
    }
    return out;
}
bool save_history(const std::map<std::wstring,unsigned>& history){
    std::string out;size_t count=0;
    for(const auto& [path,value]:history){
        if(++count>MaxHistory)break;
        if(path.find_first_of(L"\t\r\n")!=path.npos)continue;
        out+=std::to_string(value)+"\t"+utf8(path)+"\n";
    }
    return write_atomic(L"history.txt",out);
}
bool set_startup(bool enabled){
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    LONG result;
    if(enabled){
        wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,32768);
        if(!n || n==32768){RegCloseKey(key);return false;}
        std::wstring cmd=L"\""+std::wstring(path)+L"\" --background";
        result=RegSetValueExW(key,L"Spotlight",0,REG_SZ,reinterpret_cast<const BYTE*>(cmd.c_str()),static_cast<DWORD>((cmd.size()+1)*sizeof(wchar_t)));
    }else result=RegDeleteValueW(key,L"Spotlight");
    RegCloseKey(key);return result==ERROR_SUCCESS || (!enabled && result==ERROR_FILE_NOT_FOUND);
}
}
