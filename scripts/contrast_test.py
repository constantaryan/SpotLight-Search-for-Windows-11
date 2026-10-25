"""Verify opaque foreground and 5% background visibility against black/white fixtures."""
import ctypes as c,tkinter as tk,pathlib,subprocess,json,time
from ctypes import wintypes as w
from PIL import ImageGrab
from ui_test import u,set_text
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
u.GetWindowRect.argtypes=[w.HWND,c.POINTER(w.RECT)];u.GetDpiForWindow.argtypes=[w.HWND];u.GetDpiForWindow.restype=w.UINT
assert not u.FindWindowW('Spotlight.Window.1',None),'Quit launcher first'
base=pathlib.Path(__file__).resolve().parents[1];proc=subprocess.Popen([str(base/'build/release/Spotlight.exe'),'--background'])
root=tk.Tk();root.geometry(f'{root.winfo_screenwidth()}x{root.winfo_screenheight()}+0+0');root.overrideredirect(True);root.attributes('-topmost',True);root.configure(bg='black');root.update();root.focus_force()
errors=[];h=None;dark=None

def guard(fn):
    try:fn()
    except BaseException as error:errors.append(error);root.destroy()
def shot():
    assert u.IsWindowVisible(h),'Panel lost focus during contrast check'
    rect=w.RECT();u.GetWindowRect(h,c.byref(rect))
    return ImageGrab.grab(bbox=(rect.left,rect.top,rect.right,rect.bottom),all_screens=True).convert('RGB')
def show():
    global h
    h=u.FindWindowW('Spotlight.Window.1',None);assert h
    u.AllowSetForegroundWindow(proc.pid);u.SendMessageW(h,0x8005,0,0)
    edit=u.GetDlgItem(h,100);set_text(edit,'vs code');u.SendMessageW(edit,0xB1,7,7)
    root.after(1800,lambda:guard(capture_dark))
def capture_dark():
    global dark
    dark=shot();dark.save(base/'build/monochrome-dark.png');root.configure(bg='white');root.update_idletasks()
    root.after(600,lambda:guard(capture_light))
def capture_light():
    light=shot();light.save(base/'build/monochrome-light.png');assert light.size==dark.size
    scale=u.GetDpiForWindow(h)/96
    box=(int(58*scale),int(22*scale),int(125*scale),int(48*scale))
    a=dark.crop(box);b=light.crop(box);solid=0;unchanged=0
    for first,second in zip(a.getdata(),b.getdata()):
        if min(first)>=242:
            solid+=1;unchanged+=max(abs(first[i]-second[i]) for i in range(3))<=2
    assert solid>=30,f'Insufficient bright text pixels: {solid}'
    assert unchanged/solid>=.97,f'Text changes with background: {unchanged}/{solid}'
    point=(dark.width//2,10);first=dark.getpixel(point);second=light.getpixel(point)
    delta=[second[i]-first[i] for i in range(3)]
    assert all(10<=change<=16 for change in delta),f'Expected about 5% backdrop visibility, got {delta}'
    result=dict(text_samples=solid,stable_text_samples=unchanged,stable_fraction=unchanged/solid,background_delta=delta,expected_delta=13,foreground='opaque',background='5% transparent')
    (base/'build/contrast-results.json').write_text(json.dumps(result,indent=2));print(json.dumps(result));root.destroy()
try:
    root.after(1000,lambda:guard(show));root.mainloop()
finally:
    if h:u.PostMessageW(h,0x111,19,0)
    proc.wait(timeout=5)
if errors:raise errors[0]
