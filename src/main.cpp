#include "core.h"
#include "shortcut.h"
#include "icons.h"
#include "outside_click.h"
#include "panel_surface.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
using namespace sl;
namespace {
constexpr wchar_t ClassName[]=L"Spotlight.Window.1";
constexpr UINT WM_READY=WM_APP+1, WM_FILES=WM_APP+2, WM_LAUNCHED=WM_APP+3, WM_TRAY=WM_APP+4, WM_SHOW=WM_APP+5, WM_ICONS=WM_APP+6, WM_OUTSIDE=WM_APP+7, WM_RENDER=WM_APP+8;
constexpr UINT TimerSearch=1, TimerStatus=2, TimerEntrance=3, TimerCaret=4;
// Premultiplied text is required on DWM acrylic; ordinary GDI text glows.
struct GlassPaint {
    HPAINTBUFFER buffer=nullptr;HDC dc=nullptr;
    GlassPaint(HDC target,const RECT& r){
        BP_PAINTPARAMS params{sizeof(params)};params.dwFlags=BPPF_ERASE;
        buffer=BeginBufferedPaint(target,&r,BPBF_TOPDOWNDIB,&params,&dc);
        if(!buffer)dc=target;
    }
    void opaque_ink(){
        RGBQUAD* bits=nullptr;int stride=0;RECT r{};
        if(!buffer || FAILED(GetBufferedPaintBits(buffer,&bits,&stride)) || FAILED(GetBufferedPaintTargetRect(buffer,&r)))return;
        for(int y=0;y<r.bottom-r.top;++y)for(int x=0;x<r.right-r.left;++x){
            auto& pixel=bits[y*stride+x];if(pixel.rgbRed||pixel.rgbGreen||pixel.rgbBlue)pixel.rgbReserved=255;
        }
    }
    void smooth_corners(RECT shape,int radius,COLORREF color){
        RGBQUAD* bits=nullptr;int stride=0;RECT bounds{};
        if(!buffer || FAILED(GetBufferedPaintBits(buffer,&bits,&stride)) || FAILED(GetBufferedPaintTargetRect(buffer,&bounds)))return;
        for(int corner=0;corner<4;++corner){
            bool right=corner&1,bottom=corner&2;
            int cx=right?shape.right-radius:shape.left+radius;
            int cy=bottom?shape.bottom-radius:shape.top+radius;
            for(int y=0;y<radius;++y)for(int x=0;x<radius;++x){
                int px=right?shape.right-1-x:shape.left+x,py=bottom?shape.bottom-1-y:shape.top+y;
                if(px<bounds.left||px>=bounds.right||py<bounds.top||py>=bounds.bottom)continue;
                int inside=0;
                for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
                    double dx=px+(sx+.5)/4-cx,dy=py+(sy+.5)/4-cy;
                    inside+=dx*dx+dy*dy<=radius*radius;
                }
                BYTE alpha=static_cast<BYTE>((inside*255+8)/16);
                bits[(py-bounds.top)*stride+px-bounds.left]={
                    static_cast<BYTE>(GetBValue(color)*alpha/255),static_cast<BYTE>(GetGValue(color)*alpha/255),
                    static_cast<BYTE>(GetRValue(color)*alpha/255),alpha};
            }
        }
    }
    void icon(const IconPixels& source,int left,int top,int size){
        RGBQUAD* bits=nullptr;int stride=0;RECT bounds{};
        if(!buffer || source.width<=0 || source.height<=0 || FAILED(GetBufferedPaintBits(buffer,&bits,&stride)) || FAILED(GetBufferedPaintTargetRect(buffer,&bounds)))return;
        for(int y=0;y<size;++y)for(int x=0;x<size;++x){
            int dx=left+x,dy=top+y;if(dx<bounds.left||dx>=bounds.right||dy<bounds.top||dy>=bounds.bottom)continue;
            const auto& src=source.pixels[static_cast<size_t>(y*source.height/size)*source.width+x*source.width/size];
            auto& dst=bits[(dy-bounds.top)*stride+dx-bounds.left];unsigned inverse=255-src.rgbReserved;
            dst.rgbBlue=static_cast<BYTE>(std::min(255u,src.rgbBlue+(dst.rgbBlue*inverse+127)/255));
            dst.rgbGreen=static_cast<BYTE>(std::min(255u,src.rgbGreen+(dst.rgbGreen*inverse+127)/255));
            dst.rgbRed=static_cast<BYTE>(std::min(255u,src.rgbRed+(dst.rgbRed*inverse+127)/255));
            dst.rgbReserved=static_cast<BYTE>(std::min(255u,src.rgbReserved+(dst.rgbReserved*inverse+127)/255));
        }
    }
    ~GlassPaint(){if(buffer)EndBufferedPaint(buffer,TRUE);}
};
void text_on_glass(HTHEME theme,HDC dc,const wchar_t* text,int length,RECT rect,DWORD flags,COLORREF color){
    DTTOPTS options{sizeof(options)};options.dwFlags=DTT_COMPOSITED|DTT_TEXTCOLOR;options.crText=color;
    if(!theme || FAILED(DrawThemeTextEx(theme,dc,0,0,text,length,flags,&rect,&options))){
        SetTextColor(dc,color);DrawTextW(dc,text,length,&rect,flags);
    }
}
struct Reply {unsigned generation;Files files;};
struct Shared {
    std::mutex mutex;
    std::condition_variable fileWake,shellWake;
    std::atomic<bool> stop=false;
    std::atomic<unsigned> generation=0;
    std::optional<std::pair<unsigned,std::wstring>> request;
    std::optional<Reply> reply;
    std::shared_ptr<std::vector<Item>> readyCatalogue;
    std::optional<Item> launchRequest;
    std::optional<std::map<std::wstring,unsigned>> historyWrite;
    bool refresh=true,capped=false;
    HRESULT launchResult=S_OK;
    HWND window=nullptr;
};
void file_worker(std::shared_ptr<Shared> s){
    Apartment apartment(COINIT_MULTITHREADED);
    while(!s->stop){
        std::pair<unsigned,std::wstring> request;
        {std::unique_lock lock(s->mutex);
         s->fileWake.wait(lock,[&]{return s->stop||s->request.has_value();});
         if(s->stop)break;
         request=std::move(*s->request);s->request.reset();}
        Files files;
        try {files=search_files(request.second,s->generation,request.first);}
        catch(...){files.status=L"File search failed. Apps still work.";files.error=E_FAIL;}
        {std::lock_guard lock(s->mutex);
         if(s->stop || request.first!=s->generation.load())continue;
         s->reply=Reply{request.first,std::move(files)};}
        PostMessageW(s->window,WM_FILES,0,0);
    }
}
void shell_worker(std::shared_ptr<Shared> s){
    Apartment apartment;
    while(!s->stop){
        bool refresh=false;std::optional<Item> launchItem;
        std::optional<std::map<std::wstring,unsigned>> history;
        {std::unique_lock lock(s->mutex);
         s->shellWake.wait(lock,[&]{return s->stop||s->refresh||s->launchRequest||s->historyWrite;});
         if(s->stop)break;
         refresh=std::exchange(s->refresh,false);
         launchItem=std::move(s->launchRequest);s->launchRequest.reset();
         history=std::move(s->historyWrite);s->historyWrite.reset();}
        if(launchItem){
            HRESULT hr=E_FAIL;try{hr=launch(*launchItem);}catch(...){}
            {std::lock_guard lock(s->mutex);s->launchResult=hr;}
            if(!s->stop)PostMessageW(s->window,WM_LAUNCHED,0,0);
        }
        if(history)save_history(*history);
        if(refresh){
            bool capped=false;auto apps=std::make_shared<std::vector<Item>>();
            try{*apps=catalogue(capped);}catch(...){capped=true;}
            {std::lock_guard lock(s->mutex);s->readyCatalogue=std::move(apps);s->capped=capped;}
            if(!s->stop)PostMessageW(s->window,WM_READY,0,0);
        }
    }
}
struct App {
    HWND window=nullptr,edit=nullptr,list=nullptr;
    HBRUSH background=nullptr;
    PanelSurface surface;
    bool frameQueued=false,rendering=false,caretVisible=true;
    std::wstring launchError;
#ifdef SL_PROFILE_PAINT
    double paintMs[6]{};unsigned paintCount=0;
#endif
    HIMAGELIST rowSpacing=nullptr;
    HTHEME textTheme=nullptr;
    HFONT inputFont=nullptr,listFont=nullptr,smallFont=nullptr;
    Settings settings;
    std::shared_ptr<Shared> shared=std::make_shared<Shared>();
    std::shared_ptr<std::vector<Item>> apps=std::make_shared<std::vector<Item>>();
    std::map<std::wstring,unsigned> history;
    std::vector<Result> results;
    std::wstring query,status=L"Loading applications...";
    std::optional<Item> launching;
    UINT dpi=96,taskbarCreated=0;
    WinSpaceShortcut winSpace;
    OutsideClickDismiss outsideClick;
    IconCache icons;
    bool composing=false,hotkey=false,capped=false,menu=false;
    bool highContrast=false, glass=false, reducedMotion=false, animating=false;
    double animationStart=0;
    int restingX=0,restingY=0;
    int panelHeight()const{return query.empty()?68:108+static_cast<int>(std::min(results.size(),VisibleResults))*42+32;}
    void material(){
        HIGHCONTRASTW contrast{sizeof(contrast)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        highContrast=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
        BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        reducedMotion=!animations || highContrast;
        DWORD transparent=1,bytes=sizeof(transparent);
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"EnableTransparency",RRF_RT_REG_DWORD,nullptr,&transparent,&bytes);
        glass=transparent && !highContrast;
        // Per-pixel composition replaces uniform opacity, preserving opaque text/icons.
        DWM_SYSTEMBACKDROP_TYPE backdrop=DWMSBT_NONE;
        DwmSetWindowAttribute(window,DWMWA_SYSTEMBACKDROP_TYPE,&backdrop,sizeof(backdrop));
        MARGINS margins{};DwmExtendFrameIntoClientArea(window,&margins);
        SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)|WS_EX_LAYERED);
        bg=RGB(0,0,0);fg=highContrast?GetSysColor(COLOR_WINDOWTEXT):RGB(245,246,249);
        if(background)DeleteObject(background);background=CreateSolidBrush(bg);
        DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(window,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
        COLORREF border=DWMWA_COLOR_NONE;DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&border,sizeof(border));
    }
    void resize_panel(){
        if(!IsWindowVisible(window))return;
        RECT r{};GetWindowRect(window,&r);
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        int height=std::min<LONG>(px(panelHeight()),monitor.rcWork.bottom-r.top-px(14));
        SetWindowPos(window,nullptr,0,0,r.right-r.left,std::max(px(68),height),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    COLORREF bg=RGB(29,30,34),fg=RGB(240,240,244);
    int px(int value)const{return MulDiv(value,dpi,96);}
    void set_status(std::wstring text){status=std::move(text);}
    void fonts(){
        for(auto font:{inputFont,listFont,smallFont})if(font)DeleteObject(font);
        inputFont=CreateFontW(-px(26),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI Variable Text");
        listFont=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI Variable Text");
        smallFont=CreateFontW(-px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI Variable Text");
        if(rowSpacing){ListView_SetImageList(list,nullptr,LVSIL_SMALL);ImageList_Destroy(rowSpacing);}
        rowSpacing=ImageList_Create(1,px(40),ILC_COLOR32,1,0);
        ListView_SetImageList(list,rowSpacing,LVSIL_SMALL);
        SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(inputFont),TRUE);
        SendMessageW(list,WM_SETFONT,reinterpret_cast<WPARAM>(listFont),TRUE);
    }
    void layout(){
        RECT r{};GetClientRect(window,&r);
        MoveWindow(edit,px(56),px(18),r.right-px(118),px(36),TRUE);
        ShowWindow(list,query.empty()?SW_HIDE:SW_SHOWNA);
        MoveWindow(list,px(10),px(100),r.right-px(20),std::max(1L,r.bottom-px(138)),TRUE);
        ListView_SetColumnWidth(list,0,r.right-px(24));
        ListView_SetColumnWidth(list,1,0);
        ShowScrollBar(list,SB_BOTH,FALSE);
    }
    void tray(bool add){
        NOTIFYICONDATAW n{sizeof(n)};n.hWnd=window;n.uID=1;
        n.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;n.uCallbackMessage=WM_TRAY;
        n.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(n.szTip,L"Spotlight - search apps and files");
        Shell_NotifyIconW(add?NIM_ADD:NIM_DELETE,&n);
    }
    bool register_key(const Settings& wanted){
        if(wanted.modifiers==MOD_WIN && wanted.key==VK_SPACE){
            if(!winSpace.start(window))return false;
            if(hotkey)UnregisterHotKey(window,1);hotkey=false;settings=wanted;return true;
        }
        // Register a temporary ID first so a conflict preserves the current shortcut.
        if(!RegisterHotKey(window,2,wanted.modifiers|MOD_NOREPEAT,wanted.key))return false;
        if(hotkey)UnregisterHotKey(window,1);
        UnregisterHotKey(window,2);
        hotkey=RegisterHotKey(window,1,wanted.modifiers|MOD_NOREPEAT,wanted.key)!=0;
        if(hotkey){winSpace.stop();settings=wanted;}
        return hotkey;
    }
    std::wstring selected_target()const{
        int previous=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        return previous>=0 && static_cast<size_t>(previous)<results.size()?results[previous].item.target:std::wstring{};
    }
    void request_icons(){
        std::vector<Item> visible;
        if(IsWindowVisible(window) && !query.empty()){
            visible.reserve(results.size());for(const auto& r:results)if(r.item.kind!=Kind::Web)visible.push_back(r.item);
        }
        icons.request(visible,px(22));
    }
    void display(std::wstring selectedTarget={}){
        SendMessageW(list,WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(list);
        for(size_t i=0;i<results.size();++i){
                    auto& item=results[i].item;
            LVITEMW v{};v.mask=LVIF_TEXT;v.iItem=static_cast<int>(i);v.pszText=item.name.data();
            ListView_InsertItem(list,&v);
            auto detail=item.kind==Kind::App?std::wstring(L"Application"):item.kind==Kind::Web?std::wstring(L"Search the web in your browser"):item.target;
            ListView_SetItemText(list,static_cast<int>(i),1,detail.data());
        }
        if(!results.empty()){
            int selected=0;
            for(size_t i=0;i<results.size();++i)if(results[i].item.target==selectedTarget){selected=static_cast<int>(i);break;}
            ListView_SetItemState(list,selected,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        }
        SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,nullptr,TRUE);
        resize_panel();layout();request_icons();queue_frame();
    }
    void local_results(){
        results=match(*apps,query,history);
        if(is_absolute_path(query)){
            results.insert(results.begin(),{{L"Open "+query,query,{},Kind::Path},11000});
            if(results.size()>VisibleResults)results.resize(VisibleResults);
        }
        append_web_result(results,query);
        display();
    }
    void changed(){
        launchError.clear();caretVisible=true;
        wchar_t text[513]{};GetWindowTextW(edit,text,513);query=text;
        ++shared->generation;
        {std::lock_guard lock(shared->mutex);shared->request.reset();shared->reply.reset();}
        KillTimer(window,TimerSearch);KillTimer(window,TimerStatus);
        local_results();
        if(query.empty())set_status(capped?L"Application limit reached. Refresh from the tray menu.":L"Type to search. Enter opens; Esc hides.");
        else if(query.size()<2)set_status(L"Type at least 2 characters to search files.");
        else if(is_absolute_path(query))set_status(L"Press Enter to open this path.");
        else {
            set_status(L"Apps ready. Searching indexed filenames...");
            if(!composing){SetTimer(window,TimerSearch,40,nullptr);SetTimer(window,TimerStatus,2500,nullptr);}
        }
    }
    void hide(){
        outsideClick.visible(false);KillTimer(window,TimerCaret);
        ShowWindow(window,SW_HIDE);KillTimer(window,TimerSearch);KillTimer(window,TimerStatus);KillTimer(window,TimerEntrance);animating=false;
        ++shared->generation;
        {std::lock_guard lock(shared->mutex);shared->request.reset();shared->reply.reset();}
        results.clear();ListView_DeleteAllItems(list);request_icons();
    }
    void show(){
        bool opening=!IsWindowVisible(window);
        HWND previous=GetForegroundWindow();MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(previous?previous:window,MONITOR_DEFAULTTONEAREST),&info);
        int width=std::min<LONG>(px(680),info.rcWork.right-info.rcWork.left-px(32));
        restingX=info.rcWork.left+(info.rcWork.right-info.rcWork.left-width)/2;
        restingY=info.rcWork.top+(info.rcWork.bottom-info.rcWork.top)/5;
        int height=std::min<LONG>(px(panelHeight()),info.rcWork.bottom-restingY-px(16));
        SetWindowPos(window,HWND_TOPMOST,restingX,restingY,width,height,SWP_SHOWWINDOW);
        SetForegroundWindow(window);SetFocus(edit);SendMessageW(edit,EM_SETSEL,0,-1);
        changed();
        outsideClick.visible(true);
        caretVisible=true;UINT blink=GetCaretBlinkTime();
        if(blink && blink!=INFINITE)SetTimer(window,TimerCaret,std::max(250u,blink),nullptr);
        present();
        if(opening && !reducedMotion){
            animationStart=clock_ms();animating=true;
            SetWindowPos(window,nullptr,restingX,restingY+px(5),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            SetTimer(window,TimerEntrance,15,nullptr);
        }
    }
    void open_selected(){
        if(launching)return;
        int at=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        if(at<0 || static_cast<size_t>(at)>=results.size())return;
        launching=results[at].item;
        {std::lock_guard lock(shared->mutex);shared->launchRequest=launching;}
        shared->shellWake.notify_one();hide();
    }
    void move_selection(int delta){
        if(results.empty())return;
        int at=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        at=std::clamp(at+delta,0,static_cast<int>(results.size())-1);
        ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(list,at,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_EnsureVisible(list,at,FALSE);
    }
    LRESULT draw_result(NMLVCUSTOMDRAW* draw){
        if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
        if(draw->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT)return CDRF_DODEFAULT;
        size_t i=draw->nmcd.dwItemSpec;if(i>=results.size())return CDRF_SKIPDEFAULT;
        RECT row=draw->nmcd.rc;GlassPaint paint(draw->nmcd.hdc,row);HDC dc=paint.dc;
        FillRect(dc,&row,background);
        bool selected=(ListView_GetItemState(list,static_cast<int>(i),LVIS_SELECTED)&LVIS_SELECTED)!=0;
        if(selected){
            HBRUSH brush=CreateSolidBrush(highContrast?GetSysColor(COLOR_HIGHLIGHT):RGB(53,55,61));
            auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
            RoundRect(dc,row.left+px(2),row.top+px(2),row.right-px(2),row.bottom-px(2),px(20),px(20));
            SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
        }
        if(selected && !highContrast){
            HPEN outline=CreatePen(PS_SOLID,1,RGB(194,197,204));auto oldPen=SelectObject(dc,outline);auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
            RoundRect(dc,row.left+px(2),row.top+px(2),row.right-px(2),row.bottom-px(2),px(20),px(20));
            SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(outline);
        }
        auto& item=results[i].item;
        auto artwork=item.kind==Kind::Web?std::shared_ptr<const IconPixels>{}:icons.get(item.target,px(22));
        int x=row.left+px(12),y=row.top+px(9);
        if(!artwork){
            if(item.kind==Kind::Web){
                HPEN pen=CreatePen(PS_SOLID,px(1),RGB(224,228,236));auto oldPen=SelectObject(dc,pen);auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
                Ellipse(dc,x+px(1),y+px(1),x+px(21),y+px(21));
                Ellipse(dc,x+px(7),y+px(1),x+px(15),y+px(21));
                MoveToEx(dc,x+px(2),y+px(11),nullptr);LineTo(dc,x+px(20),y+px(11));
                SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
            }else{
                HBRUSH brush=CreateSolidBrush(RGB(109,123,143));auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
                RoundRect(dc,x,y,x+px(22),y+px(22),px(6),px(6));SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
                HPEN pen=CreatePen(PS_SOLID,px(1),RGB(235,240,248));oldPen=SelectObject(dc,pen);oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
                Rectangle(dc,x+px(5),y+px(4),x+px(17),y+px(18));
                SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
            }
        }
        if(selected){
            int middle=(row.top+row.bottom)/2;HPEN pen=CreatePen(PS_SOLID,std::max(1,px(2)),highContrast?GetSysColor(COLOR_HIGHLIGHTTEXT):RGB(50,166,248));
            auto oldPen=SelectObject(dc,pen);
            MoveToEx(dc,row.right-px(14),middle-px(5),nullptr);LineTo(dc,row.right-px(14),middle+px(2));LineTo(dc,row.right-px(28),middle+px(2));
            MoveToEx(dc,row.right-px(24),middle-px(2),nullptr);LineTo(dc,row.right-px(28),middle+px(2));LineTo(dc,row.right-px(24),middle+px(6));
            SelectObject(dc,oldPen);DeleteObject(pen);
        }
        paint.opaque_ink();
        if(artwork)paint.icon(*artwork,x,y,px(22));
        SetBkMode(dc,TRANSPARENT);
        auto font=SelectObject(dc,listFont);
        RECT label{row.left+px(46),row.top,row.right-px(122),row.bottom};
        text_on_glass(textTheme,dc,item.name.c_str(),-1,label,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX,selected?RGB(255,255,255):fg);
        SelectObject(dc,smallFont);
        RECT detail{row.right-px(144),row.top,row.right-px(selected?43:15),row.bottom};
        const wchar_t* kind=item.kind==Kind::App?L"Application":item.kind==Kind::Path?L"Open path":item.kind==Kind::Web?L"Web search":L"File";
        text_on_glass(textTheme,dc,kind,-1,detail,DT_RIGHT|DT_VCENTER|DT_SINGLELINE,selected?RGB(219,221,228):RGB(183,187,196));
        SelectObject(dc,font);return CDRF_SKIPDEFAULT;
    }
    void queue_frame(){
        if(!window || !IsWindowVisible(window) || rendering || frameQueued)return;
        frameQueued=true;PostMessageW(window,WM_RENDER,0,0);
    }
    void draw_chrome(HDC target){
        RECT r{};GetClientRect(window,&r);
        // Chrome occupies the header/footer only; leave the result region untouched.
        RECT header{0,0,r.right,std::min(r.bottom,static_cast<LONG>(px(100)))};
        {
            GlassPaint paint(target,header);HDC dc=paint.dc;FillRect(dc,&header,background);SetBkMode(dc,TRANSPARENT);
            HPEN pen=CreatePen(PS_SOLID,px(2),RGB(174,179,191));auto oldPen=SelectObject(dc,pen);auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
            Ellipse(dc,px(22),px(24),px(38),px(40));MoveToEx(dc,px(36),px(38),nullptr);LineTo(dc,px(43),px(45));
            SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
            if(!query.empty()){
                pen=CreatePen(PS_SOLID,1,RGB(68,71,80));oldPen=SelectObject(dc,pen);
                MoveToEx(dc,px(15),px(68),nullptr);LineTo(dc,r.right-px(15),px(68));SelectObject(dc,oldPen);DeleteObject(pen);
            }
            paint.opaque_ink();auto font=SelectObject(dc,smallFont);
            RECT escape{r.right-px(51),px(22),r.right-px(18),px(47)};
            text_on_glass(textTheme,dc,L"esc",3,escape,DT_CENTER|DT_VCENTER|DT_SINGLELINE,RGB(184,188,199));
            if(!query.empty()){
                RECT title{px(23),px(77),r.right-px(20),px(99)};
                text_on_glass(textTheme,dc,results.empty()?L"No Results":L"Top Hit",-1,title,DT_SINGLELINE|DT_VCENTER,RGB(184,188,199));
            }
            SelectObject(dc,font);
        }
        if(!query.empty()){
            RECT footer{0,r.bottom-px(34),r.right,r.bottom};GlassPaint paint(target,footer);HDC dc=paint.dc;
            FillRect(dc,&footer,background);SetBkMode(dc,TRANSPARENT);
            HPEN pen=CreatePen(PS_SOLID,1,RGB(54,57,65));auto oldPen=SelectObject(dc,pen);
            MoveToEx(dc,px(15),footer.top,nullptr);LineTo(dc,r.right-px(15),footer.top);SelectObject(dc,oldPen);DeleteObject(pen);
            paint.opaque_ink();auto font=SelectObject(dc,smallFont);
            RECT label{px(23),footer.top+px(5),r.right-px(22),r.bottom-px(6)};
            text_on_glass(textTheme,dc,launchError.empty()?L"Enter to open":launchError.c_str(),-1,label,
                DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX|(launchError.empty()?DT_RIGHT:DT_LEFT),RGB(186,189,198));
            SelectObject(dc,font);
        }
    }
    void draw_input(HDC target){
        RECT area{};GetWindowRect(edit,&area);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&area),2);
        GlassPaint paint(target,area);HDC dc=paint.dc;FillRect(dc,&area,background);
        auto font=SelectObject(dc,inputFont);SetBkMode(dc,TRANSPARENT);
        wchar_t input[513]{};int count=GetWindowTextW(edit,input,513);
        DWORD first=0,last=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        int offset=0;if(count){LRESULT position=SendMessageW(edit,EM_POSFROMCHAR,0,0);if(position!=-1)offset=static_cast<short>(LOWORD(position));}
        if(first!=last && GetFocus()==edit){
            SIZE left{},right{};GetTextExtentPoint32W(dc,input,static_cast<int>(std::min<DWORD>(first,count)),&left);
            GetTextExtentPoint32W(dc,input,static_cast<int>(std::min<DWORD>(last,count)),&right);
            RECT selection{area.left+offset+left.cx,area.top,area.left+offset+right.cx,area.top+px(34)};
            HBRUSH brush=CreateSolidBrush(highContrast?GetSysColor(COLOR_HIGHLIGHT):RGB(65,69,80));FillRect(dc,&selection,brush);DeleteObject(brush);
        }
        if(caretVisible && GetFocus()==edit && first==last){
            POINT caret{};if(GetCaretPos(&caret)){
                RECT line{area.left+caret.x,area.top+px(3),area.left+caret.x+std::max(1,px(1)),area.top+px(30)};
                HBRUSH brush=CreateSolidBrush(fg);FillRect(dc,&line,brush);DeleteObject(brush);
            }
        }
        paint.opaque_ink();RECT text{area.left+offset,area.top,area.right,area.bottom};
        text_on_glass(textTheme,dc,count?input:L"Spotlight Search",-1,text,DT_SINGLELINE|DT_NOPREFIX,count?fg:RGB(174,178,190));
        SelectObject(dc,font);
    }
    void present(){
        frameQueued=false;if(rendering || !window || !IsWindowVisible(window) || !edit || !list)return;
#ifdef SL_PROFILE_PAINT
        double stamp[7]{};stamp[0]=clock_ms();
#endif
        RECT area{};GetClientRect(window,&area);if(!surface.ensure(area.right,area.bottom))return;
#ifdef SL_PROFILE_PAINT
        stamp[1]=clock_ms();
#endif
        rendering=true;HDC dc=surface.dc();draw_chrome(dc);
#ifdef SL_PROFILE_PAINT
        stamp[2]=clock_ms();
#endif
        draw_input(dc);
#ifdef SL_PROFILE_PAINT
        stamp[3]=clock_ms();
#endif
        if(!query.empty()){
            RECT position{};GetWindowRect(list,&position);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&position),2);
            int saved=SaveDC(dc);SetViewportOrgEx(dc,position.left,position.top,nullptr);
            for(size_t i=0;i<results.size();++i){
                NMLVCUSTOMDRAW draw{};draw.nmcd.dwDrawStage=CDDS_ITEMPREPAINT;draw.nmcd.hdc=dc;draw.nmcd.dwItemSpec=i;
                if(ListView_GetItemRect(list,static_cast<int>(i),&draw.nmcd.rc,LVIR_BOUNDS))draw_result(&draw);
            }
            RestoreDC(dc,saved);
        }
#ifdef SL_PROFILE_PAINT
        stamp[4]=clock_ms();
#endif
        surface.background(highContrast?GetSysColor(COLOR_WINDOW):RGB(25,27,32),glass?242:255,px(16));
#ifdef SL_PROFILE_PAINT
        stamp[5]=clock_ms();
#endif
        surface.present(window);rendering=false;
#ifdef SL_PROFILE_PAINT
        stamp[6]=clock_ms();++paintCount;for(int i=0;i<6;++i)paintMs[i]+=stamp[i+1]-stamp[i];
#endif
    }
    void popup(){
        menu=true;
        HMENU m=CreatePopupMenu();
        AppendMenuW(m,MF_STRING,10,L"Open Spotlight");
        AppendMenuW(m,MF_STRING,11,L"Refresh applications");
        AppendMenuW(m,MF_STRING,12,L"Shortcut: Windows+Space");
        AppendMenuW(m,MF_STRING,13,L"Shortcut: Ctrl+Alt+Space");
        AppendMenuW(m,MF_STRING|(settings.startup?MF_CHECKED:0),14,L"Start at sign-in");
        AppendMenuW(m,MF_STRING,15,L"Open settings folder");
        AppendMenuW(m,MF_STRING,16,L"Reload settings");
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);AppendMenuW(m,MF_STRING,19,L"Quit");
        POINT p{};GetCursorPos(&p);SetForegroundWindow(window);
        int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);
        DestroyMenu(m);menu=false;PostMessageW(window,WM_NULL,0,0);
        if(cmd==10)show();
        if(cmd==11){icons.clear();request_icons();std::lock_guard lock(shared->mutex);shared->refresh=true;shared->shellWake.notify_one();}
        if(cmd==12||cmd==13||cmd==16){
            Settings wanted=cmd==16?load_settings():settings;
            if(cmd!=16){wanted.modifiers=cmd==12?MOD_WIN:MOD_CONTROL|MOD_ALT;wanted.key=VK_SPACE;}
            if(wanted.modifiers==settings.modifiers && wanted.key==settings.key)return;
            if(!register_key(wanted))MessageBoxW(window,L"That shortcut is already in use. The previous shortcut is retained.",L"Spotlight",MB_OK|MB_ICONINFORMATION);
            else save_settings(settings);
        }
        if(cmd==14){
            bool value=!settings.startup;
            if(set_startup(value)){settings.startup=value;save_settings(settings);}
            else MessageBoxW(window,L"Could not change the sign-in setting.",L"Spotlight",MB_OK|MB_ICONERROR);
        }
        if(cmd==15){
            auto dir=data_dir();CreateDirectoryW(dir.c_str(),nullptr);save_settings(settings);
            ShellExecuteW(window,L"open",dir.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        }
        if(cmd==19)DestroyWindow(window);
    }
};
LRESULT CALLBACK input_proc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto& a=*reinterpret_cast<App*>(data);
    if(msg==WM_PAINT){PAINTSTRUCT paint{};BeginPaint(h,&paint);EndPaint(h,&paint);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_IME_STARTCOMPOSITION)a.composing=true;
    if(msg==WM_IME_ENDCOMPOSITION){a.composing=false;PostMessageW(a.window,WM_COMMAND,MAKEWPARAM(100,EN_CHANGE),reinterpret_cast<LPARAM>(h));}
    if(msg==WM_KEYDOWN && !a.composing){
        if(w==VK_ESCAPE){a.hide();return 0;}
        if(w==VK_RETURN){a.open_selected();return 0;}
        if(w==VK_DOWN||w==VK_UP){a.move_selection(w==VK_DOWN?1:-1);return 0;}
        if(w=='A' && GetKeyState(VK_CONTROL)<0){SendMessageW(h,EM_SETSEL,0,-1);return 0;}
        if(w==VK_TAB){SetFocus(a.list);return 0;}
    }
    if(msg==WM_CHAR && !a.composing && (w==VK_RETURN||w==VK_ESCAPE||w==VK_TAB))return 0;
    LRESULT result=DefSubclassProc(h,msg,w,l);
    if(msg==EM_SETSEL || msg==WM_KEYUP || msg==WM_LBUTTONDOWN || msg==WM_LBUTTONUP || msg==WM_SETFOCUS || msg==WM_KILLFOCUS || msg==WM_IME_COMPOSITION || (msg==WM_MOUSEMOVE && (w&MK_LBUTTON))){a.caretVisible=true;a.queue_frame();}
    return result;
}
LRESULT CALLBACK list_proc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto& a=*reinterpret_cast<App*>(data);
    if(msg==WM_PAINT){PAINTSTRUCT paint{};BeginPaint(h,&paint);EndPaint(h,&paint);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    LRESULT result=DefSubclassProc(h,msg,w,l);
    if(msg==WM_LBUTTONUP || msg==WM_KEYUP || msg==WM_SETFOCUS || msg==WM_KILLFOCUS)a.queue_frame();
    return result;
}
LRESULT CALLBACK window_proc(HWND h,UINT msg,WPARAM w,LPARAM l){
    auto* ptr=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){ptr=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);ptr->window=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(ptr));}
    if(!ptr)return DefWindowProcW(h,msg,w,l);
    auto& a=*ptr;
    if(msg==a.taskbarCreated && a.taskbarCreated){a.tray(true);return 0;}
    switch(msg){
    case WM_CREATE:{
        a.dpi=GetDpiForWindow(h);
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        a.highContrast=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
        if(a.highContrast){a.bg=GetSysColor(COLOR_WINDOW);a.fg=GetSysColor(COLOR_WINDOWTEXT);}
        a.material();
        a.textTheme=OpenThemeData(h,L"WINDOW");
        a.edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,1,1,h,reinterpret_cast<HMENU>(100),nullptr,nullptr);
        SendMessageW(a.edit,EM_SETLIMITTEXT,512,0);SendMessageW(a.edit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Spotlight Search"));
        SetWindowSubclass(a.edit,input_proc,1,reinterpret_cast<DWORD_PTR>(&a));
        a.list=CreateWindowExW(0,WC_LISTVIEWW,L"Search results",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_NOCOLUMNHEADER|LVS_SHOWSELALWAYS|LVS_NOSCROLL,0,0,1,1,h,reinterpret_cast<HMENU>(101),nullptr,nullptr);
        SetWindowSubclass(a.list,list_proc,1,reinterpret_cast<DWORD_PTR>(&a));
        ListView_SetExtendedListViewStyle(a.list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(a.list,a.bg);ListView_SetTextBkColor(a.list,a.bg);ListView_SetTextColor(a.list,a.fg);
        LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=260;col.pszText=const_cast<wchar_t*>(L"Name");ListView_InsertColumn(a.list,0,&col);
        col.pszText=const_cast<wchar_t*>(L"Location");ListView_InsertColumn(a.list,1,&col);
        a.fonts();a.layout();

        a.taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");a.tray(true);
        return 0;}
    case WM_NCCALCSIZE:return 0;
    case WM_NCPAINT:return 0;
    case WM_NCACTIVATE:return TRUE;
    case WM_NCHITTEST:return HTCLIENT;
    case WM_SETTINGCHANGE:
        a.material();ListView_SetBkColor(a.list,a.bg);ListView_SetTextBkColor(a.list,a.bg);
        a.queue_frame();return 0;
    case WM_SIZE:a.layout();a.queue_frame();return 0;
    case WM_DPICHANGED:{
        a.dpi=HIWORD(w);a.fonts();auto* r=reinterpret_cast<RECT*>(l);
        SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);a.layout();a.request_icons();a.surface.release();a.queue_frame();return 0;}
    case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(w),a.fg);SetBkColor(reinterpret_cast<HDC>(w),a.bg);return reinterpret_cast<LRESULT>(a.background);
    case WM_ERASEBKGND:{RECT r{};GetClientRect(h,&r);FillRect(reinterpret_cast<HDC>(w),&r,a.background);return 1;}
    case WM_PAINT:{PAINTSTRUCT paint{};BeginPaint(h,&paint);EndPaint(h,&paint);return 0;}
    case WM_COMMAND:if(LOWORD(w)==100 && HIWORD(w)==EN_UPDATE)a.queue_frame();if(LOWORD(w)==19){DestroyWindow(h);return 0;} if(LOWORD(w)==100 && HIWORD(w)==EN_CHANGE)a.changed();return 0;
    case WM_TIMER:
        if(w==TimerCaret){if(GetFocus()==a.edit && !a.menu){a.caretVisible=!a.caretVisible;a.queue_frame();}return 0;}
        if(w==TimerEntrance){
            double t=std::min(1.0,(clock_ms()-a.animationStart)/120.0);
            int offset=static_cast<int>(a.px(5)*(1-t)*(1-t)*(1-t));
            SetWindowPos(h,nullptr,a.restingX,a.restingY+offset,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            if(t>=1){KillTimer(h,TimerEntrance);a.animating=false;}
            return 0;
        }
        if(w==TimerSearch){
            KillTimer(h,TimerSearch);
            {std::lock_guard lock(a.shared->mutex);a.shared->request=std::make_pair(a.shared->generation.load(),a.query);}
            a.shared->fileWake.notify_one();
        }else if(w==TimerStatus){KillTimer(h,TimerStatus);a.set_status(L"File search is taking longer. Application results are available.");}
        return 0;
    case WM_READY:{
        {std::lock_guard lock(a.shared->mutex);if(a.shared->readyCatalogue)a.apps=std::move(a.shared->readyCatalogue);a.capped=a.shared->capped;}
        if(IsWindowVisible(h))a.changed();return 0;}
    case WM_FILES:{
        std::optional<Reply> reply;
        {std::lock_guard lock(a.shared->mutex);reply=std::move(a.shared->reply);a.shared->reply.reset();}
        if(!reply || reply->generation!=a.shared->generation.load() || !IsWindowVisible(h))return 0;
        KillTimer(h,TimerStatus);
        auto selected=a.selected_target();
        const bool hasWeb=std::any_of(a.results.begin(),a.results.end(),[](const Result& r){return r.item.kind==Kind::Web;});
        std::erase_if(a.results,[](const Result& r){return r.item.kind==Kind::Web;});
        for(auto& item:reply->files.items){
            if(a.results.size()>=(hasWeb?VisibleResults-1:VisibleResults))break;
            if(std::none_of(a.results.begin(),a.results.end(),[&](const Result& r){return _wcsicmp(r.item.target.c_str(),item.target.c_str())==0;}))
                a.results.push_back({std::move(item),0});
        }
        append_web_result(a.results,a.query);
        a.display(std::move(selected));a.set_status(reply->files.status);return 0;}
    case WM_LAUNCHED:{
        HRESULT hr;{std::lock_guard lock(a.shared->mutex);hr=a.shared->launchResult;}
        if(SUCCEEDED(hr) && a.launching && a.launching->kind!=Kind::Web){
            auto& count=a.history[a.launching->target];count=std::min(count+1,100u);
            if(a.history.size()>MaxHistory){
                auto it=std::min_element(a.history.begin(),a.history.end(),[](auto&x,auto&y){return x.second<y.second;});
                a.history.erase(it);
            }
            {std::lock_guard lock(a.shared->mutex);a.shared->historyWrite=a.history;}
            a.shared->shellWake.notify_one();
        }else if(FAILED(hr)){a.show();a.launchError=a.launching && a.launching->kind==Kind::Web?L"Could not open your browser. Check the default browser setting.":L"Could not open this result. It may have moved or been removed.";a.queue_frame();}
        a.launching.reset();return 0;}
    case WM_NOTIFY:{
        auto* n=reinterpret_cast<NMHDR*>(l);
        if(n->hwndFrom==a.list && n->code==LVN_ITEMCHANGED)a.queue_frame();
        if(n->hwndFrom==a.list && n->code==NM_CUSTOMDRAW)return a.draw_result(reinterpret_cast<NMLVCUSTOMDRAW*>(l));
        if(n->hwndFrom==a.list && n->code==NM_DBLCLK)a.open_selected();
        if(n->hwndFrom==a.list && n->code==LVN_KEYDOWN){
            auto key=reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey;
            if(key==VK_RETURN)a.open_selected();else if(key==VK_ESCAPE)a.hide();else if(key==VK_TAB)SetFocus(a.edit);
        }return 0;}
    case WM_OUTSIDE:if(!a.menu && a.outsideClick.current(w))a.hide();return 0;
#ifdef SL_PROFILE_PAINT
    case WM_APP+40:return w==0?a.paintCount:(w<=6 && a.paintCount?static_cast<LRESULT>(1000*a.paintMs[w-1]/a.paintCount):0);
    case WM_APP+41:for(auto& t:a.paintMs)t=0;a.paintCount=0;return 0;
    case WM_APP+42:a.present();return 0;
#endif
    case WM_RENDER:if(a.frameQueued)a.present();return 0;
    case WM_ICONS:a.queue_frame();return 0;
    case WM_SHOW:a.show();return 0;
    case WM_HOTKEY:if(IsWindowVisible(h))a.hide();else a.show();return 0;
    case WM_TRAY:if(l==WM_LBUTTONUP)a.show();else if(l==WM_RBUTTONUP)a.popup();return 0;
    case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE && !a.menu)a.hide();return 0;
    case WM_CLOSE:a.hide();return 0;
    case WM_DESTROY:
        a.outsideClick.stop();
        a.icons.stop();
        a.winSpace.stop();
        a.shared->stop=true;++a.shared->generation;a.shared->fileWake.notify_all();a.shared->shellWake.notify_all();
        if(a.hotkey)UnregisterHotKey(h,1);
        a.tray(false);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR command,int){
    Apartment apartment;
    BufferedPaintInit();
    HANDLE singleton=CreateMutexW(nullptr,FALSE,L"Local\\Spotlight.Singleton.1");
    if(!singleton)return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS){
        HWND existing=FindWindowW(ClassName,nullptr);
        if(existing){DWORD owner=0;GetWindowThreadProcessId(existing,&owner);AllowSetForegroundWindow(owner);PostMessageW(existing,WM_SHOW,0,0);}
        CloseHandle(singleton);return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    App app;app.settings=load_settings();app.history=load_history();
    WNDCLASSW cls{};cls.style=CS_DROPSHADOW;cls.lpfnWndProc=window_proc;cls.hInstance=instance;cls.lpszClassName=ClassName;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&cls);
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,ClassName,L"Spotlight",WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,0,0,700,435,nullptr,nullptr,instance,&app);
    if(!window){CloseHandle(singleton);return 2;}
    app.shared->window=window;
    app.icons.start(window,WM_ICONS);
    app.outsideClick.start(window,WM_OUTSIDE);
    if(!app.register_key(app.settings)){
        Settings fallback=app.settings;fallback.modifiers=MOD_CONTROL|MOD_ALT;fallback.key=VK_SPACE;
        if(app.register_key(fallback))app.set_status(L"Preferred shortcut unavailable. Using Ctrl+Alt+Space.");
        else MessageBoxW(window,L"Global shortcuts are in use. Open Spotlight from its tray icon and choose another shortcut.",L"Spotlight",MB_OK|MB_ICONINFORMATION);
    }
    // Shared ownership keeps worker state alive through shutdown. A blocked external
    // provider cannot keep Quit waiting: returning from wWinMain ends this process.
    std::thread(file_worker,app.shared).detach();std::thread(shell_worker,app.shared).detach();
    if(!wcsstr(command,L"--background"))app.show();
    MSG msg{};BOOL result;
    while((result=GetMessageW(&msg,nullptr,0,0))>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    app.shared->stop=true;app.shared->fileWake.notify_all();app.shared->shellWake.notify_all();
    for(auto font:{app.inputFont,app.listFont,app.smallFont})if(font)DeleteObject(font);
    if(app.textTheme)CloseThemeData(app.textTheme);
    BufferedPaintUnInit();
    if(app.rowSpacing)ImageList_Destroy(app.rowSpacing);
    if(app.background)DeleteObject(app.background);
    CloseHandle(singleton);return result==-1?3:0;
}


