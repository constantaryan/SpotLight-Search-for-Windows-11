#pragma once
#include <windows.h>
#include <objbase.h>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <memory>
#include <map>
namespace sl {
constexpr size_t MaxApps=5000, MaxResults=100, VisibleResults=8, MaxHistory=500;
enum class Kind { App, File, Path, Web };
struct Item { std::wstring name, target, folded; Kind kind=Kind::App; };
struct Result { Item item; int score=0; };
std::wstring fold(std::wstring_view s);
int rank(std::wstring_view name, std::wstring_view query);
std::wstring file_query(std::wstring_view s);
bool is_absolute_path(std::wstring_view s);
// Pure construction: no network activity occurs until the user launches the Web item.
std::wstring web_search_url(std::wstring_view query);
void append_web_result(std::vector<Result>& results,std::wstring_view query);
std::vector<Result> match(const std::vector<Item>& items, std::wstring_view query,
                         const std::map<std::wstring,unsigned>& history);
std::vector<Item> catalogue(bool& capped);
struct Files { std::vector<Item> items; std::wstring status; HRESULT error=S_OK; double ms=0; bool skipped=false; };
Files search_files(std::wstring_view query, const std::atomic<unsigned>& generation, unsigned expected);
HRESULT launch(const Item& item);
struct Settings { UINT modifiers=MOD_WIN, key=VK_SPACE; bool startup=false; };
std::wstring data_dir();
Settings load_settings();
bool save_settings(const Settings&);
std::map<std::wstring,unsigned> load_history();
bool save_history(const std::map<std::wstring,unsigned>&);
bool set_startup(bool);
double clock_ms();
template<class T> struct Com {
    T* p=nullptr;
    ~Com(){ if(p) p->Release(); }
    Com()=default; Com(const Com&)=delete; Com& operator=(const Com&)=delete;
    T** put(){ if(p){p->Release();p=nullptr;} return &p; }
    T* operator->()const{return p;}
    explicit operator bool()const{return p!=nullptr;}
};
struct Apartment {
    HRESULT result;
    explicit Apartment(DWORD mode=COINIT_APARTMENTTHREADED):result(CoInitializeEx(nullptr,mode)){}
    ~Apartment(){if(SUCCEEDED(result)) CoUninitialize();}
};
}
