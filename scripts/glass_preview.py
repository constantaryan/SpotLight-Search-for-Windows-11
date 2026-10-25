"""Capture the launcher over a temporary controlled color backdrop."""
import tkinter as tk,ctypes as c,pathlib
from ctypes import wintypes as w
from PIL import ImageGrab
u=c.WinDLL('user32');u.SetProcessDpiAwarenessContext(c.c_void_p(-4));u.FindWindowW.restype=w.HWND;u.FindWindowW.argtypes=[w.LPCWSTR,w.LPCWSTR]
u.GetWindowRect.argtypes=[w.HWND,c.POINTER(w.RECT)];u.SendMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM]
u.GetDlgItem.argtypes=[w.HWND,c.c_int];u.GetDlgItem.restype=w.HWND
u.GetWindowThreadProcessId.argtypes=[w.HWND,c.POINTER(w.DWORD)];u.IsWindowVisible.argtypes=[w.HWND]
h=u.FindWindowW('Spotlight.Window.1',None);assert h
r=w.RECT();u.GetWindowRect(h,c.byref(r))
root=tk.Tk();root.geometry(f'{root.winfo_screenwidth()}x{root.winfo_screenheight()}+0+0');root.overrideredirect(True)
canvas=tk.Canvas(root,highlightthickness=0);canvas.pack(fill='both',expand=True)
for i,color in enumerate(['#a73c63','#347ca0','#4e9175','#c09555']):canvas.create_rectangle(i*root.winfo_screenwidth()/4,0,(i+1)*root.winfo_screenwidth()/4,root.winfo_screenheight(),fill=color,outline='')
root.attributes('-topmost',True);root.update();root.focus_force()
def show():
    owner=w.DWORD();u.GetWindowThreadProcessId(h,c.byref(owner));u.AllowSetForegroundWindow(owner.value)
    u.SendMessageW(h,0x8005,0,0)
    text=c.create_unicode_buffer('code');u.SendMessageW(u.GetDlgItem(h,100),0xC,0,c.cast(text,c.c_void_p).value)
    root.after(1500,capture)
def capture():
    try:
        assert u.IsWindowVisible(h),'Launcher lost focus; no screenshot saved'
        u.GetWindowRect(h,c.byref(r));out=pathlib.Path(__file__).resolve().parents[1]/'build/glass-controlled.png'
        ImageGrab.grab(bbox=(r.left,r.top,r.right,r.bottom),all_screens=True).save(out);print(out)
    finally:root.destroy()
root.after(600,show);root.mainloop()

