#pragma once
#include "core.h"
namespace sl {
struct IconPixels {
    int width=0,height=0;
    // Top-down premultiplied BGRA. Immutable once published by the worker.
    std::vector<RGBQUAD> pixels;
};
class IconCache {
    struct State;
    std::shared_ptr<State> state_;
public:
    IconCache()=default;
    IconCache(const IconCache&)=delete;
    IconCache& operator=(const IconCache&)=delete;
    ~IconCache();
    bool start(HWND window,UINT notification);
    void request(const std::vector<Item>& visible,int pixelSize);
    std::shared_ptr<const IconPixels> get(std::wstring_view target,int pixelSize);
    void clear();
    void stop();
};
}
