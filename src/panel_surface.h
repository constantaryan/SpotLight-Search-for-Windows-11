#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
// One reusable premultiplied BGRA surface: translucent backdrop, opaque content.
class PanelSurface {
    HDC dc_=nullptr;HBITMAP bitmap_=nullptr;HGDIOBJ previous_=nullptr;
    RGBQUAD* pixels_=nullptr;int capacityWidth_=0,capacityHeight_=0;
public:
    int width=0,height=0;
    PanelSurface()=default;
    PanelSurface(const PanelSurface&)=delete;
    PanelSurface& operator=(const PanelSurface&)=delete;
    ~PanelSurface(){release();}
    void release(){
        if(dc_ && previous_)SelectObject(dc_,previous_);
        if(bitmap_)DeleteObject(bitmap_);if(dc_)DeleteDC(dc_);
        dc_=nullptr;bitmap_=nullptr;previous_=nullptr;pixels_=nullptr;capacityWidth_=capacityHeight_=width=height=0;
    }
    bool ensure(int w,int h){
        if(w<=0||h<=0||w>8192||h>8192||static_cast<size_t>(w)*h>16*1024*1024)return false;
        if(w>capacityWidth_||h>capacityHeight_){
            int cw=std::max(w,capacityWidth_),ch=std::max(h,capacityHeight_);release();
            dc_=CreateCompatibleDC(nullptr);if(!dc_)return false;
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=cw;
            info.bmiHeader.biHeight=-ch;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels_),nullptr,0);
            if(!bitmap_){release();return false;}previous_=SelectObject(dc_,bitmap_);capacityWidth_=cw;capacityHeight_=ch;
        }
        width=w;height=h;std::memset(pixels_,0,static_cast<size_t>(capacityWidth_)*h*sizeof(RGBQUAD));return true;
    }
    HDC dc()const{return dc_;}
    void background(COLORREF color,BYTE opacity,int radius){
        GdiFlush();RGBQUAD base{static_cast<BYTE>(GetBValue(color)*opacity/255),static_cast<BYTE>(GetGValue(color)*opacity/255),static_cast<BYTE>(GetRValue(color)*opacity/255),opacity};
        // Most pixels are untouched background. Avoid alpha math and geometry there.
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            auto& p=pixels_[y*capacityWidth_+x];
            if(p.rgbReserved==0){
                if(!(p.rgbBlue|p.rgbGreen|p.rgbRed)){p=base;continue;}
                p.rgbReserved=255;continue; // legacy GDI ink without alpha
            }
            if(p.rgbReserved==255)continue;
            unsigned inverse=255-p.rgbReserved;
            p.rgbBlue=static_cast<BYTE>(std::min(255u,p.rgbBlue+(base.rgbBlue*inverse+127)/255));
            p.rgbGreen=static_cast<BYTE>(std::min(255u,p.rgbGreen+(base.rgbGreen*inverse+127)/255));
            p.rgbRed=static_cast<BYTE>(std::min(255u,p.rgbRed+(base.rgbRed*inverse+127)/255));
            p.rgbReserved=static_cast<BYTE>(p.rgbReserved+(opacity*inverse+127)/255);
        }
        auto rim=[](RGBQUAD& p,unsigned a){unsigned inverse=255-a;
            p.rgbBlue=static_cast<BYTE>((116*a+p.rgbBlue*inverse+127)/255);
            p.rgbGreen=static_cast<BYTE>((112*a+p.rgbGreen*inverse+127)/255);
            p.rgbRed=static_cast<BYTE>((110*a+p.rgbRed*inverse+127)/255);
            p.rgbReserved=static_cast<BYTE>(a+(p.rgbReserved*inverse+127)/255);
        };
        radius=std::min({radius,width/2,height/2});
        for(int x=radius;x<width-radius;++x){rim(pixels_[x],100);rim(pixels_[(height-1)*capacityWidth_+x],100);}
        for(int y=radius;y<height-radius;++y){rim(pixels_[y*capacityWidth_],100);rim(pixels_[y*capacityWidth_+width-1],100);}
        // Curved-edge geometry is needed only in the four small corner squares.
        for(int corner=0;corner<4;++corner)for(int y=0;y<radius;++y)for(int x=0;x<radius;++x){
            int px=(corner&1)?width-1-x:x,py=(corner&2)?height-1-y:y;
            auto& p=pixels_[py*capacityWidth_+px];double dx=radius-(x+.5),dy=radius-(y+.5);
            double distance=std::sqrt(dx*dx+dy*dy);
            double coverage=std::clamp(radius+.5-distance,0.0,1.0);
            unsigned edge=static_cast<unsigned>(std::clamp(distance-(radius-1.0),0.0,1.0)*100);
            if(edge)rim(p,edge);
            if(coverage<1){p.rgbBlue=static_cast<BYTE>(p.rgbBlue*coverage);p.rgbGreen=static_cast<BYTE>(p.rgbGreen*coverage);p.rgbRed=static_cast<BYTE>(p.rgbRed*coverage);p.rgbReserved=static_cast<BYTE>(p.rgbReserved*coverage);}
        }
    }
    bool present(HWND window){
        POINT source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        return UpdateLayeredWindow(window,nullptr,nullptr,&size,dc_,&source,0,&blend,ULW_ALPHA)!=FALSE;
    }
};
