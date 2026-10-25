"""Exercise real inside/outside clicks on a harmless nonactivating test window."""
import ctypes as c
from ctypes import wintypes as w
import pathlib,subprocess,time,json
from ui_test import u,k
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
u.DefWindowProcW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM];u.DefWindowProcW.restype=c.c_ssize_t
u.GetWindowRect.argtypes=[w.HWND,c.POINTER(w.RECT)];u.GetCursorPos.argtypes=[c.POINTER(w.POINT)]
u.SetCursorPos.argtypes=[c.c_int,c.c_int];u.mouse_event.argtypes=[w.DWORD,w.DWORD,w.DWORD,w.DWORD,c.c_size_t]
u.PeekMessageW.argtypes=[c.POINTER(w.MSG),w.HWND,w.UINT,w.UINT,w.UINT]
u.DispatchMessageW.argtypes=[c.POINTER(w.MSG)];u.DispatchMessageW.restype=c.c_ssize_t
k.GetModuleHandleW.argtypes=[w.LPCWSTR];k.GetModuleHandleW.restype=w.HMODULE
WNDPROC=c.WINFUNCTYPE(c.c_ssize_t,w.HWND,w.UINT,w.WPARAM,w.LPARAM)
clicks={}
@WNDPROC
def wndproc(hwnd,msg,wp,lp):
    if msg==0x21:return 3 # MA_NOACTIVATE: clicking here cannot trigger app deactivation.
    if msg in [0x201,0x204,0x207,0x20b]:clicks[msg]=clicks.get(msg,0)+1;return 1 if msg==0x20b else 0
    return u.DefWindowProcW(hwnd,msg,wp,lp)
class WindowClass(c.Structure):
    _fields_=[('style',w.UINT),('proc',WNDPROC),('clsExtra',c.c_int),('wndExtra',c.c_int),('instance',w.HINSTANCE),('icon',w.HICON),('cursor',w.HANDLE),('brush',w.HBRUSH),('menu',w.LPCWSTR),('name',w.LPCWSTR)]
u.RegisterClassW.argtypes=[c.POINTER(WindowClass)]
u.CreateWindowExW.argtypes=[w.DWORD,w.LPCWSTR,w.LPCWSTR,w.DWORD,c.c_int,c.c_int,c.c_int,c.c_int,w.HWND,w.HMENU,w.HINSTANCE,c.c_void_p];u.CreateWindowExW.restype=w.HWND
u.DestroyWindow.argtypes=[w.HWND]
def pump(seconds=.2):
    end=time.monotonic()+seconds;msg=w.MSG()
    while time.monotonic()<end:
        while u.PeekMessageW(c.byref(msg),None,0,0,1):u.TranslateMessage(c.byref(msg));u.DispatchMessageW(c.byref(msg))
        time.sleep(.005)
def click(x,y,down=2,up=4,data=0):
    u.SetCursorPos(x,y);pump(.04);u.mouse_event(down,0,0,data,0);u.mouse_event(up,0,0,data,0);pump(.25)
root=pathlib.Path(__file__).resolve().parents[1]
assert not u.FindWindowW('Spotlight.Window.1',None),'Quit launcher first'
instance=k.GetModuleHandleW(None);cls=WindowClass();cls.proc=wndproc;cls.instance=instance;cls.brush=6;cls.name='Spotlight.DismissFixture'
assert u.RegisterClassW(c.byref(cls))
fixture=u.CreateWindowExW(0x08000088,cls.name,'Dismiss test',0x90000000,0,0,u.GetSystemMetrics(0),u.GetSystemMetrics(1),None,None,instance,None);assert fixture
cursor=w.POINT();u.GetCursorPos(c.byref(cursor));proc=None;h=None
try:
    proc=subprocess.Popen([str(root/'build/release/Spotlight.exe'),'--background']);pump(3)
    h=u.FindWindowW('Spotlight.Window.1',None);assert h
    edit=u.GetDlgItem(h,100);listing=u.GetDlgItem(h,101)
    for label,down,up,data,msg in [('left',2,4,0,0x201),('right',8,16,0,0x204),('middle',32,64,0,0x207),('X1',128,256,1,0x20b)]:
        u.SendMessageW(h,0x8005,0,0);pump(.25);assert u.IsWindowVisible(h)
        r=w.RECT();u.GetWindowRect(h,c.byref(r));click(r.left+80,r.top+35)
        assert u.IsWindowVisible(h),'Inside click dismissed the panel'
        before=clicks.get(msg,0);click(20,20,down,up,data)
        assert not u.IsWindowVisible(h),label+' outside click failed'
        assert clicks.get(msg,0)>before,label+' click was swallowed'
    for control,name in [(edit,'input'),(listing,'list')]:
        u.SendMessageW(h,0x8005,0,0);pump(.2)
        if control==listing:u.SendMessageW(edit,0x100,9,0)
        u.SendMessageW(control,0x100,27,0);pump(.15)
        assert not u.IsWindowVisible(h),'Escape failed from '+name
    result=dict(outside_buttons=['left','right','middle','X1'],inside_click='keeps open',click_delivery='passed',escape_input='passed',escape_list='passed',nonactivating_target='passed')
    (root/'build/dismiss-results.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
finally:
    u.SetCursorPos(cursor.x,cursor.y)
    if h:u.PostMessageW(h,0x111,19,0)
    if proc:proc.wait(timeout=5)
    u.DestroyWindow(fixture)
