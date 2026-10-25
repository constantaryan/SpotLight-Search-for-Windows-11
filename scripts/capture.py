"""Capture the actual composited launcher (including acrylic), never the full desktop."""
import ctypes as c
from ctypes import wintypes as w
import pathlib,time,argparse
from PIL import ImageGrab
a=argparse.ArgumentParser();a.add_argument("--query",default="");a.add_argument("--output",default="launcher-preview.png");args=a.parse_args()
u=c.WinDLL("user32");d=c.WinDLL("dwmapi")
u.FindWindowW.argtypes=[w.LPCWSTR,w.LPCWSTR];u.FindWindowW.restype=w.HWND
u.GetDlgItem.argtypes=[w.HWND,c.c_int];u.GetDlgItem.restype=w.HWND
u.SendMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM];u.SendMessageW.restype=c.c_ssize_t
u.GetWindowRect.argtypes=[w.HWND,c.POINTER(w.RECT)]
d.DwmGetWindowAttribute.argtypes=[w.HWND,w.DWORD,c.c_void_p,w.DWORD]
window=u.FindWindowW("Spotlight.Window.1",None);assert window
u.SendMessageW(window,0x8005,0,0)
edit=u.GetDlgItem(window,100);text=c.create_unicode_buffer(args.query)
u.SendMessageW(edit,0xC,0,c.cast(text,c.c_void_p).value);u.SendMessageW(edit,0xB1,len(args.query),len(args.query))
time.sleep(1)
rect=w.RECT()
if d.DwmGetWindowAttribute(window,9,c.byref(rect),c.sizeof(rect))!=0:u.GetWindowRect(window,c.byref(rect))
root=pathlib.Path(__file__).resolve().parents[1]
out=root/"build"/args.output
ImageGrab.grab(bbox=(rect.left,rect.top,rect.right,rect.bottom),all_screens=True).save(out)
print(out)
