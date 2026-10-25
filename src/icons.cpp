#include "icons.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
namespace sl {
namespace {
constexpr size_t MaxIconEntries=32;
struct BitmapOwner {HBITMAP value=nullptr;~BitmapOwner(){if(value)DeleteObject(value);}};
// The dual-background rasterization also handles legacy mask-only icons.
std::shared_ptr<const IconPixels> legacy_icon(const std::wstring& target,int size){
    PIDLIST_ABSOLUTE pidl=nullptr;
    if(FAILED(SHParseDisplayName(target.c_str(),nullptr,&pidl,0,nullptr)))return {};
    SHFILEINFOW info{};
    auto success=SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl),0,&info,sizeof(info),SHGFI_PIDL|SHGFI_ICON|SHGFI_LARGEICON);
    CoTaskMemFree(pidl);
    if(!success || !info.hIcon)return {};
    struct IconOwner {HICON icon;~IconOwner(){DestroyIcon(icon);}} owner{info.hIcon};
    HDC dc=CreateCompatibleDC(nullptr);if(!dc)return {};
    struct DcOwner {HDC dc;~DcOwner(){DeleteDC(dc);}} dcOwner{dc};
    BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bmi.bmiHeader.biWidth=size;
    bmi.bmiHeader.biHeight=-size;bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
    RGBQUAD* pixels=nullptr;BitmapOwner bitmap;
    bitmap.value=CreateDIBSection(dc,&bmi,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
    if(!bitmap.value)return {};
    auto old=SelectObject(dc,bitmap.value);
    struct Selection {HDC dc;HGDIOBJ old;~Selection(){SelectObject(dc,old);}} selection{dc,old};
    auto out=std::make_shared<IconPixels>();out->width=out->height=size;
    out->pixels.resize(static_cast<size_t>(size)*size);
    std::fill_n(pixels,out->pixels.size(),RGBQUAD{});
    if(!DrawIconEx(dc,0,0,info.hIcon,size,size,0,nullptr,DI_NORMAL))return {};
    GdiFlush();std::copy_n(pixels,out->pixels.size(),out->pixels.begin());
    std::fill_n(pixels,out->pixels.size(),RGBQUAD{255,255,255,255});
    if(!DrawIconEx(dc,0,0,info.hIcon,size,size,0,nullptr,DI_NORMAL))return {};
    GdiFlush();
    for(size_t i=0;i<out->pixels.size();++i){
        auto& black=out->pixels[i];const auto& white=pixels[i];
        int difference=std::max({int(white.rgbRed)-black.rgbRed,int(white.rgbGreen)-black.rgbGreen,int(white.rgbBlue)-black.rgbBlue,0});
        black.rgbReserved=static_cast<BYTE>(255-std::min(difference,255));
        black.rgbRed=std::min(black.rgbRed,black.rgbReserved);black.rgbGreen=std::min(black.rgbGreen,black.rgbReserved);black.rgbBlue=std::min(black.rgbBlue,black.rgbReserved);
    }
    return out;
}
std::shared_ptr<const IconPixels> extract_icon(const std::wstring& target,int size){
    Com<IShellItemImageFactory> factory;
    if(FAILED(SHCreateItemFromParsingName(target.c_str(),nullptr,IID_PPV_ARGS(factory.put()))))return legacy_icon(target,size);
    BitmapOwner bitmap;
    if(FAILED(factory->GetImage(SIZE{size,size},static_cast<SIIGBF>(SIIGBF_ICONONLY|SIIGBF_SCALEUP),&bitmap.value)) || !bitmap.value)return legacy_icon(target,size);
    BITMAP description{};
    if(!GetObjectW(bitmap.value,sizeof(description),&description) || description.bmWidth<=0 || description.bmHeight<=0 || description.bmWidth>64 || description.bmHeight>64)return {};
    auto out=std::make_shared<IconPixels>();out->width=description.bmWidth;out->height=description.bmHeight;
    out->pixels.resize(static_cast<size_t>(out->width)*out->height);
    BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bmi.bmiHeader.biWidth=out->width;
    bmi.bmiHeader.biHeight=-out->height;bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);if(!dc)return {};
    int lines=GetDIBits(dc,bitmap.value,0,static_cast<UINT>(out->height),out->pixels.data(),&bmi,DIB_RGB_COLORS);DeleteDC(dc);
    if(lines!=out->height)return {};
    if(std::none_of(out->pixels.begin(),out->pixels.end(),[](const RGBQUAD& p){return p.rgbReserved!=0;}))return legacy_icon(target,size);
    // Shell image-factory bitmaps are composited as premultiplied BGRA.
    for(auto& p:out->pixels){p.rgbBlue=std::min(p.rgbBlue,p.rgbReserved);p.rgbGreen=std::min(p.rgbGreen,p.rgbReserved);p.rgbRed=std::min(p.rgbRed,p.rgbReserved);}
    return out;
}
}
struct IconCache::State {
    struct Key {std::wstring target;int size=0;unsigned epoch=0;};
    struct Entry {Key key;std::shared_ptr<const IconPixels> image;unsigned long long touched=0;};
    std::mutex mutex;std::condition_variable wake;
    bool stopped=false;HWND window=nullptr;UINT notification=0;unsigned epoch=0;
    unsigned long long clock=0;std::vector<Entry> cache;std::vector<Key> pending;std::optional<Key> active;
    static bool same(const Key& key,std::wstring_view target,int size,unsigned epoch){return key.size==size && key.epoch==epoch && key.target==target;}
    static void run(std::shared_ptr<State> state){
        Apartment apartment;
        for(;;){
            Key key;
            {std::unique_lock lock(state->mutex);state->wake.wait(lock,[&]{return state->stopped||!state->pending.empty();});
             if(state->stopped)return;
             key=std::move(state->pending.front());state->pending.erase(state->pending.begin());state->active=key;}
            std::shared_ptr<const IconPixels> image;
            try{if(SUCCEEDED(apartment.result))image=extract_icon(key.target,key.size);}catch(...){}
            {std::lock_guard lock(state->mutex);state->active.reset();
             if(state->stopped)return;
             if(key.epoch!=state->epoch)continue;
             if(state->cache.size()==MaxIconEntries){auto oldest=std::min_element(state->cache.begin(),state->cache.end(),[](const Entry& a,const Entry& b){return a.touched<b.touched;});state->cache.erase(oldest);}
             state->cache.push_back({std::move(key),std::move(image),++state->clock});
             // Posting while locked prevents shutdown from reusing this HWND first.
             PostMessageW(state->window,state->notification,0,0);}
        }
    }
};
IconCache::~IconCache(){stop();}
bool IconCache::start(HWND window,UINT notification){
    if(state_)return true;
    try{auto state=std::make_shared<State>();state->window=window;state->notification=notification;state->cache.reserve(MaxIconEntries);
        std::thread worker(State::run,state);worker.detach();state_=std::move(state);return true;
    }catch(...){return false;}
}
void IconCache::request(const std::vector<Item>& visible,int pixelSize){
    if(!state_)return;auto& state=*state_;int size=std::clamp(pixelSize,16,64);
    std::lock_guard lock(state.mutex);if(state.stopped)return;
    state.pending.clear();
    for(const auto& item:visible){
        if(state.pending.size()==VisibleResults)break;
        // Web actions have no Shell item and must not perform any icon network lookup.
        if(item.target.starts_with(L"https://") || item.target.starts_with(L"http://"))continue;
        if(std::any_of(state.cache.begin(),state.cache.end(),[&](const State::Entry& entry){return State::same(entry.key,item.target,size,state.epoch);}))continue;
        if(state.active && State::same(*state.active,item.target,size,state.epoch))continue;
        if(std::any_of(state.pending.begin(),state.pending.end(),[&](const State::Key& key){return State::same(key,item.target,size,state.epoch);}))continue;
        state.pending.push_back({item.target,size,state.epoch});
    }
    if(!state.pending.empty())state.wake.notify_one();
}
std::shared_ptr<const IconPixels> IconCache::get(std::wstring_view target,int pixelSize){
    if(!state_)return {};auto& state=*state_;int size=std::clamp(pixelSize,16,64);
    std::lock_guard lock(state.mutex);
    for(auto& entry:state.cache)if(State::same(entry.key,target,size,state.epoch)){entry.touched=++state.clock;return entry.image;}
    return {};
}
void IconCache::clear(){if(!state_)return;std::lock_guard lock(state_->mutex);++state_->epoch;state_->cache.clear();state_->pending.clear();}
void IconCache::stop(){
    if(!state_)return;
    {std::lock_guard lock(state_->mutex);state_->stopped=true;state_->window=nullptr;state_->pending.clear();state_->cache.clear();}
    state_->wake.notify_one();state_.reset();
    // A Shell extension can block: detached worker owns only shared state, never
    // App/UI resources. Shutdown does not wait indefinitely on external code.
}
}
