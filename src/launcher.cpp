// OptiCaptureHost launcher. Original project code Copyright (C) MK-JH.
// Modified development fork, 2026-11-24.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>
#include "settings.h"
#include "version.h"
namespace gp = Gdiplus;
namespace
{
constexpr wchar_t kClassName[] = L"OptiCaptureLauncher";
constexpr wchar_t kOverlayClass[] = L"OptiCaptureHostOverlay";
constexpr wchar_t kHostExe[] = L"OptiCaptureHost.exe";
constexpr wchar_t kAppTitle[] = L"OptiCaptureHost";
constexpr UINT_PTR kTimerList = 1, kTimerHost = 2;
constexpr int kHotkeyShow = 1, kIdRefresh = 1, kIdScale = 2, kIdStart = 3;
constexpr int kIdShortcutBase = 100, kIdRowBase = 1000;
constexpr float kWidth = 720.0f, kHeight = 720.0f;
using std::max;
using std::min;
namespace col
{
const gp::Color bg(255,13,15,20), card(255,21,24,32), row(255,27,31,42), rowHot(255,34,39,53), rowSelected(255,38,45,66);
const gp::Color line(255,40,45,58), text(255,233,236,243), muted(255,135,143,163), accent(255,108,140,255);
const gp::Color cyan(255,56,214,220), green(255,61,220,151), red(255,255,92,112);
}
struct Hit { gp::RectF rect; int id = 0; };
struct GameItem { HWND hwnd = nullptr; std::wstring title, exe; int width = 0, height = 0; bool minimized = false; HICON icon = nullptr; };
struct EnumContext { std::vector<GameItem>* games = nullptr; DWORD ownPid = 0, hostPid = 0; };
std::wstring KeyName(UINT vk)
{
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= L'0' && vk <= L'9') || (vk >= L'A' && vk <= L'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    switch (vk) { case VK_INSERT: return L"Insert"; case VK_DELETE: return L"Delete"; case VK_HOME: return L"Home";
    case VK_END: return L"End"; case VK_PRIOR: return L"Page Up"; case VK_NEXT: return L"Page Down"; case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right"; case VK_UP: return L"Up"; case VK_DOWN: return L"Down"; case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space"; default: break; }
    wchar_t buffer[64] = {};
    if (GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16), buffer, 64) > 0) return buffer;
    return L"Key " + std::to_wstring(vk);
}
std::wstring HotkeyText(const oc::Hotkey& h)
{
    std::wstring s;
    if (h.mods & MOD_CONTROL) s += L"Ctrl + "; if (h.mods & MOD_ALT) s += L"Alt + ";
    if (h.mods & MOD_SHIFT) s += L"Shift + "; if (h.mods & MOD_WIN) s += L"Win + ";
    return s + KeyName(h.vk);
}
bool IsModifier(UINT vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}
HICON WindowIcon(HWND w)
{
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(w, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 40, &result) && result) return reinterpret_cast<HICON>(result);
    if (SendMessageTimeoutW(w, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 40, &result) && result) return reinterpret_cast<HICON>(result);
    HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(w, GCLP_HICON));
    return icon ? icon : reinterpret_cast<HICON>(GetClassLongPtrW(w, GCLP_HICONSM));
}
BOOL CALLBACK EnumWindowsProc(HWND w, LPARAM parameter)
{
    auto* c = reinterpret_cast<EnumContext*>(parameter);
    if (!IsWindowVisible(w)) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t title[512] = {};
    if (GetWindowTextW(w, title, 512) <= 0) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid == c->ownPid || (c->hostPid && pid == c->hostPid) || (GetWindow(w, GW_OWNER) && !(ex & WS_EX_APPWINDOW))) return TRUE;
    wchar_t cls[80] = {}; GetClassNameW(w, cls, 80);
    if (!wcscmp(cls,L"Progman") || !wcscmp(cls,L"WorkerW") || !wcscmp(cls,L"Shell_TrayWnd") || !wcscmp(cls,kOverlayClass)) return TRUE;
    RECT rc{}; if (!GetClientRect(w,&rc)) return TRUE;
    const int width = rc.right-rc.left, height = rc.bottom-rc.top;
    if (width < 200 || height < 120) return TRUE;
    GameItem it; it.hwnd=w; it.title=title; it.width=width; it.height=height; it.minimized=IsIconic(w)!=FALSE; it.icon=WindowIcon(w);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (p) { wchar_t path[MAX_PATH]={}; DWORD n=MAX_PATH; if (QueryFullProcessImageNameW(p,0,path,&n)) {
        std::wstring full=path; size_t cut=full.find_last_of(L"\\/"); it.exe=cut==std::wstring::npos?full:full.substr(cut+1); } CloseHandle(p); }
    c->games->push_back(std::move(it)); return TRUE;
}
BOOL CALLBACK CloseOverlayProc(HWND w, LPARAM parameter)
{
    DWORD pid=0; GetWindowThreadProcessId(w,&pid); if (pid!=static_cast<DWORD>(parameter)) return TRUE;
    wchar_t cls[80]={}; GetClassNameW(w,cls,80); if (!wcscmp(cls,kOverlayClass)) PostMessageW(w,WM_CLOSE,0,0); return TRUE;
}
UINT ReadOptiScalerMenuKey()
{
    wchar_t value[32]=L"0x2D"; const std::wstring path=oc::ExeDir()+L"OptiScaler.ini";
    GetPrivateProfileStringW(L"Menu",L"ShortcutKey",L"0x2D",value,32,path.c_str());
    wchar_t* end=nullptr; const unsigned long key=wcstoul(value,&end,0);
    return end==value || key==0 || key>254 ? VK_INSERT : static_cast<UINT>(key);
}
void DrawRound(gp::Graphics& g,const gp::RectF& r,float radius,const gp::Color& fill,const gp::Color* stroke=nullptr,float sw=1.0f)
{
    gp::GraphicsPath path; const float d=min(radius*2.0f,min(r.Width,r.Height));
    path.AddArc(r.X,r.Y,d,d,180,90); path.AddArc(r.X+r.Width-d,r.Y,d,d,270,90);
    path.AddArc(r.X+r.Width-d,r.Y+r.Height-d,d,d,0,90); path.AddArc(r.X,r.Y+r.Height-d,d,d,90,90); path.CloseFigure();
    gp::SolidBrush b(fill); g.FillPath(&b,&path); if (stroke) { gp::Pen p(*stroke,sw); g.DrawPath(&p,&path); }
}
}
class App
{
public: bool Create(HINSTANCE); int Loop();
private:
    struct Fonts { std::unique_ptr<gp::Font> title,heading,body,caption,button; };
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM); LRESULT OnMessage(UINT,WPARAM,LPARAM);
    void MakeFonts(); void ApplyDpi(UINT,const RECT*); float S(float v) const { return v*scale_; }
    gp::RectF R(float x,float y,float w,float h) const { return gp::RectF(S(x),S(y),S(w),S(h)); }
    void Paint(HDC,int,int); void Text(gp::Graphics&,const std::wstring&,const gp::Font&,const gp::RectF&,const gp::Color&,int=0);
    void Card(gp::Graphics&,const gp::RectF&); void AddHit(const gp::RectF& r,int id) { hits_.push_back({r,id}); }
    int HitAt(int,int) const; void Refresh(); void ClampScroll(); void SetNote(const std::wstring&,bool=false); void Save(); void OnClick(int);
    void BeginHotkeyCapture(int); void SetCapturedHotkey(UINT); oc::Hotkey* HotkeyAt(int); std::wstring HotkeyLabel(int) const;
    bool RegisterShowHotkey(); void Start(); void Stop(bool); void PollHost(); void RestoreTargetWindow(); void ResizeTargetWindow(const GameItem&);
    void ShowOnTop(); void PaintLive(gp::Graphics&,const gp::RectF&);
    HWND hwnd_=nullptr; HINSTANCE instance_=nullptr; float scale_=1.0f; Fonts fonts_; oc::Settings settings_;
    std::vector<GameItem> games_; int selected_=-1; float scroll_=0; std::vector<Hit> hits_; int hover_=0,pressed_=0;
    bool trackingMouse_=false; int capturingShortcut_=-1; std::wstring note_; bool noteError_=false;
    HANDLE host_=nullptr; DWORD hostPid_=0; HWND target_=nullptr; bool stopping_=false; ULONGLONG stopAt_=0;
    HWND resizedWindow_=nullptr; RECT originalRect_{}; WINDOWPLACEMENT originalPlacement_{};
    bool originallyMinimized_=false, originallyMaximized_=false; oc::LiveStats live_; bool liveValid_=false;
};
void App::Text(gp::Graphics& g,const std::wstring& value,const gp::Font& font,const gp::RectF& rect,const gp::Color& color,int align)
{
    gp::SolidBrush brush(color); gp::StringFormat format;
    format.SetAlignment(align==1?gp::StringAlignmentCenter:(align==2?gp::StringAlignmentFar:gp::StringAlignmentNear));
    format.SetLineAlignment(gp::StringAlignmentCenter); format.SetFormatFlags(gp::StringFormatFlagsNoWrap|gp::StringFormatFlagsLineLimit);
    g.DrawString(value.c_str(),static_cast<INT>(value.size()),&font,rect,&format,&brush);
}
void App::Card(gp::Graphics& g,const gp::RectF& r) { DrawRound(g,r,S(12),col::card,&col::line,max(1.0f,S(1))); }
void App::MakeFonts()
{
    auto make=[&](const wchar_t* family,float size,int style) { return std::make_unique<gp::Font>(family,S(size),style,gp::UnitPixel); };
    fonts_.title=make(L"Segoe UI Semibold",25,gp::FontStyleRegular); fonts_.heading=make(L"Segoe UI Semibold",16,gp::FontStyleRegular);
    fonts_.body=make(L"Segoe UI",14,gp::FontStyleRegular); fonts_.caption=make(L"Segoe UI",12,gp::FontStyleRegular);
    fonts_.button=make(L"Segoe UI Semibold",13,gp::FontStyleRegular);
}
bool App::Create(HINSTANCE inst)
{
    instance_=inst; oc::LoadSettings(settings_); WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=Proc; wc.hInstance=inst;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.lpszClassName=kClassName;
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    const DWORD style=WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX; const UINT dpi=GetDpiForSystem(); scale_=static_cast<float>(dpi)/96.0f;
    RECT rc{0,0,static_cast<LONG>(S(kWidth)),static_cast<LONG>(S(kHeight))}; AdjustWindowRectExForDpi(&rc,style,FALSE,0,dpi);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const int x=work.left+((work.right-work.left)-(rc.right-rc.left))/2, y=work.top+((work.bottom-work.top)-(rc.bottom-rc.top))/2;
    hwnd_=CreateWindowExW(0,kClassName,kAppTitle,style,x,y,rc.right-rc.left,rc.bottom-rc.top,nullptr,nullptr,inst,this);
    if (!hwnd_) return false;
    UINT actual=GetDpiForWindow(hwnd_); if (actual && actual!=dpi) ApplyDpi(actual,nullptr); else MakeFonts();
    BOOL dark=TRUE; DwmSetWindowAttribute(hwnd_,20,&dark,sizeof(dark)); const COLORREF caption=RGB(13,15,20); DwmSetWindowAttribute(hwnd_,35,&caption,sizeof(caption));
    SendMessageW(hwnd_,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(LoadIconW(nullptr,IDI_APPLICATION)));
    DeleteFileW(oc::StatusPath().c_str()); Refresh(); RegisterShowHotkey(); SetTimer(hwnd_,kTimerList,3000,nullptr); SetTimer(hwnd_,kTimerHost,300,nullptr);
    ShowWindow(hwnd_,SW_SHOW); UpdateWindow(hwnd_); return true;
}
int App::Loop()
{
    MSG msg{}; while (GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); } return static_cast<int>(msg.wParam);
}
LRESULT CALLBACK App::Proc(HWND w,UINT m,WPARAM wp,LPARAM lp)
{
    App* app=reinterpret_cast<App*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if (m==WM_NCCREATE) { auto* cs=reinterpret_cast<CREATESTRUCTW*>(lp); app=static_cast<App*>(cs->lpCreateParams); app->hwnd_=w;
        SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
    return app?app->OnMessage(m,wp,lp):DefWindowProcW(w,m,wp,lp);
}
void App::ApplyDpi(UINT dpi,const RECT* suggested)
{
    scale_=static_cast<float>(dpi)/96.0f; MakeFonts();
    if (suggested) SetWindowPos(hwnd_,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
    else { RECT rc{0,0,static_cast<LONG>(S(kWidth)),static_cast<LONG>(S(kHeight))};
        AdjustWindowRectExForDpi(&rc,static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_STYLE)),FALSE,0,dpi);
        SetWindowPos(hwnd_,nullptr,0,0,rc.right-rc.left,rc.bottom-rc.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE); }
    ClampScroll(); InvalidateRect(hwnd_,nullptr,FALSE);
}
void App::Refresh()
{
    const HWND keep=selected_>=0&&selected_<static_cast<int>(games_.size())?games_[selected_].hwnd:nullptr;
    std::vector<GameItem> found; EnumContext c{&found,GetCurrentProcessId(),hostPid_}; EnumWindows(EnumWindowsProc,reinterpret_cast<LPARAM>(&c));
    games_=std::move(found); selected_=-1;
    if (keep) for (size_t i=0;i<games_.size();++i) if (games_[i].hwnd==keep) { selected_=static_cast<int>(i); break; }
    ClampScroll(); InvalidateRect(hwnd_,nullptr,FALSE);
}
void App::ClampScroll()
{
    const float content=static_cast<float>(games_.size())*S(52); const float view=R(40,151,636,258).Height;
    scroll_=min(max(0.0f,scroll_),max(0.0f,content-view));
}
void App::SetNote(const std::wstring& value,bool error) { note_=value; noteError_=error; InvalidateRect(hwnd_,nullptr,FALSE); }
void App::Save() { oc::SaveSettings(settings_); }
oc::Hotkey* App::HotkeyAt(int i)
{
    switch(i) { case 0:return &settings_.stop; case 1:return &settings_.focus; case 2:return &settings_.menu; case 3:return &settings_.launcher; default:return nullptr; }
}
std::wstring App::HotkeyLabel(int i) const
{
    const oc::Hotkey* h=nullptr; switch(i) { case 0:h=&settings_.stop;break; case 1:h=&settings_.focus;break; case 2:h=&settings_.menu;break; case 3:h=&settings_.launcher;break; }
    if (!h) return {}; return capturingShortcut_==i?L"Press keys...":HotkeyText(*h);
}
bool App::RegisterShowHotkey()
{
    UnregisterHotKey(hwnd_,kHotkeyShow);
    if (!RegisterHotKey(hwnd_,kHotkeyShow,settings_.launcher.mods|MOD_NOREPEAT,settings_.launcher.vk))
    { SetNote(L"The launcher shortcut is already used by another program.",true); return false; } return true;
}
void App::BeginHotkeyCapture(int index)
{
    if (host_) return; capturingShortcut_=index; SetNote(L"Press the shortcut keys together. Press Esc to cancel.");
}
void App::SetCapturedHotkey(UINT key)
{
    if (capturingShortcut_<0||capturingShortcut_>=4||IsModifier(key)) return;
    if (key==VK_ESCAPE) { capturingShortcut_=-1; SetNote(L"Shortcut change cancelled."); return; }
    UINT mods=0; if (GetAsyncKeyState(VK_CONTROL)&0x8000) mods|=MOD_CONTROL; if (GetAsyncKeyState(VK_SHIFT)&0x8000) mods|=MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU)&0x8000) mods|=MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN)&0x8000)||(GetAsyncKeyState(VK_RWIN)&0x8000)) mods|=MOD_WIN;
    if (!mods) { SetNote(L"Use at least one modifier key (Ctrl, Alt, Shift, or Win).",true); return; }
    const int changed=capturingShortcut_; if (oc::Hotkey* h=HotkeyAt(changed)) { h->mods=mods; h->vk=key; }
    capturingShortcut_=-1; Save(); if (changed==3) RegisterShowHotkey(); SetNote(L"Shortcut saved. It applies to the next capture.");
}
void App::Start()
{
    if (host_) { Stop(false); return; }
    if (selected_<0||selected_>=static_cast<int>(games_.size())) { SetNote(L"Select a window first.",true); return; }
    const GameItem game=games_[selected_]; if (!IsWindow(game.hwnd)) { Refresh(); SetNote(L"That window has closed. Refresh the list.",true); return; }
    const std::wstring dir=oc::ExeDir(), hostPath=dir+kHostExe;
    if (GetFileAttributesW(hostPath.c_str())==INVALID_FILE_ATTRIBUTES) { SetNote(L"OptiCaptureHost.exe was not found next to the launcher.",true); return; }
    target_=game.hwnd; if (settings_.targetScale) ResizeTargetWindow(game);
    const UINT menuKey=ReadOptiScalerMenuKey(); wchar_t key[8]={}; swprintf_s(key,L"%02X",menuKey);
    const std::wstring cmd=L"\""+hostPath+L"\" --hwnd "+std::to_wstring(reinterpret_cast<ULONG_PTR>(game.hwnd))+L" --parent "+
        std::to_wstring(GetCurrentProcessId())+L" --menu-key 0x"+key;
    std::vector<wchar_t> command(cmd.begin(),cmd.end()); command.push_back(L'\0'); STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(hostPath.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,dir.c_str(),&si,&pi)) {
        RestoreTargetWindow(); target_=nullptr; SetNote(L"Could not start the capture host (error "+std::to_wstring(GetLastError())+L").",true); return; }
    CloseHandle(pi.hThread); host_=pi.hProcess; hostPid_=pi.dwProcessId; stopping_=false; liveValid_=false;
    SetNote(L"Capture starting. The target window will be restored when capture stops."); ShowWindow(hwnd_,SW_MINIMIZE);
}
void App::ResizeTargetWindow(const GameItem& game)
{
    HWND w=game.hwnd; if (!settings_.targetScale||!IsWindow(w)) return;
    originalPlacement_=WINDOWPLACEMENT{}; originalPlacement_.length=sizeof(originalPlacement_); GetWindowPlacement(w,&originalPlacement_);
    originallyMinimized_=IsIconic(w)!=FALSE; originallyMaximized_=IsZoomed(w)!=FALSE; originalRect_=originalPlacement_.rcNormalPosition;
    if (!originallyMinimized_&&!originallyMaximized_) GetWindowRect(w,&originalRect_); resizedWindow_=w;
    if (originallyMinimized_||originallyMaximized_) ShowWindow(w,SW_RESTORE);
    MONITORINFO mi{}; mi.cbSize=sizeof(mi); if (!GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi)) return;
    static const double factors[]={1.0,1.5,1.7,2.0}; const double f=factors[settings_.targetScale];
    const int cw=(static_cast<int>((mi.rcMonitor.right-mi.rcMonitor.left)/f+0.5))&~1;
    const int ch=(static_cast<int>((mi.rcMonitor.bottom-mi.rcMonitor.top)/f+0.5))&~1;
    RECT frame{0,0,cw,ch}; AdjustWindowRectEx(&frame,static_cast<DWORD>(GetWindowLongPtrW(w,GWL_STYLE)),FALSE,
                                               static_cast<DWORD>(GetWindowLongPtrW(w,GWL_EXSTYLE)));
    SetWindowPos(w,nullptr,mi.rcMonitor.left,mi.rcMonitor.top,frame.right-frame.left,frame.bottom-frame.top,SWP_NOZORDER|SWP_NOACTIVATE);
    RECT client{}; GetClientRect(w,&client); SetNote(L"Target window set to "+std::to_wstring(client.right-client.left)+L"x"+
        std::to_wstring(client.bottom-client.top)+L". The host uses a neutral blit to fill the monitor.");
}
void App::RestoreTargetWindow()
{
    if (!resizedWindow_) return;
    if (IsWindow(resizedWindow_)) {
        if (originallyMinimized_) { SetWindowPlacement(resizedWindow_,&originalPlacement_); ShowWindow(resizedWindow_,SW_MINIMIZE); }
        else if (originallyMaximized_) { SetWindowPlacement(resizedWindow_,&originalPlacement_); ShowWindow(resizedWindow_,SW_MAXIMIZE); }
        else SetWindowPos(resizedWindow_,nullptr,originalRect_.left,originalRect_.top,originalRect_.right-originalRect_.left,
                          originalRect_.bottom-originalRect_.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    }
    resizedWindow_=nullptr;
}
void App::Stop(bool wait)
{
    if (!host_) return; EnumWindows(CloseOverlayProc,static_cast<LPARAM>(hostPid_)); stopping_=true; stopAt_=GetTickCount64()+3000;
    if (wait) { if (WaitForSingleObject(host_,2000)!=WAIT_OBJECT_0) TerminateProcess(host_,1); PollHost(); }
}
void App::PollHost()
{
    if (!host_) return; oc::LiveStats stats;
    liveValid_=oc::ReadLiveStats(stats)&&GetTickCount64()-stats.stamp<3000; if (liveValid_) live_=stats;
    if (WaitForSingleObject(host_,0)==WAIT_OBJECT_0) {
        DWORD code=1; GetExitCodeProcess(host_,&code); CloseHandle(host_); host_=nullptr; hostPid_=0; RestoreTargetWindow(); target_=nullptr;
        stopping_=false; liveValid_=false; SetNote(code==0?L"Capture stopped.":L"Capture host stopped with code "+std::to_wstring(code)+L". See OptiCaptureHost.log.",code!=0);
        Refresh(); ShowOnTop(); return;
    }
    if (stopping_&&GetTickCount64()>=stopAt_) { TerminateProcess(host_,1); stopAt_=GetTickCount64()+3000; }
    InvalidateRect(hwnd_,nullptr,FALSE);
}
void App::ShowOnTop()
{
    ShowWindow(hwnd_,IsIconic(hwnd_)?SW_RESTORE:SW_SHOW); SetWindowPos(hwnd_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetForegroundWindow(hwnd_); SetWindowPos(hwnd_,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
int App::HitAt(int x,int y) const
{
    for (auto it=hits_.rbegin();it!=hits_.rend();++it) if (it->rect.Contains(static_cast<float>(x),static_cast<float>(y))) return it->id;
    return 0;
}
void App::OnClick(int id)
{
    if (id==kIdRefresh) { Refresh(); SetNote(L"Window list refreshed."); }
    else if (id==kIdScale) {
        if (host_) return; static const wchar_t* choices[]={L"Native window size",L"67% of monitor size",L"58% of monitor size",L"50% of monitor size"};
        HMENU menu=CreatePopupMenu(); if (!menu) return;
        for (int i=0;i<4;++i) AppendMenuW(menu,MF_STRING|(settings_.targetScale==i?MF_CHECKED:0),10+i,choices[i]);
        POINT p{}; GetCursorPos(&p); UINT cmd=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd_,nullptr); DestroyMenu(menu);
        if (cmd>=10&&cmd<=13) { settings_.targetScale=static_cast<int>(cmd-10); Save();
            SetNote(settings_.targetScale==0?L"Native mode leaves the target window size unchanged.":L"Scaled mode restores the original window size after capture."); }
    } else if (id==kIdStart) Start();
    else if (id>=kIdShortcutBase&&id<kIdShortcutBase+4) BeginHotkeyCapture(id-kIdShortcutBase);
    else if (id>=kIdRowBase) { int index=id-kIdRowBase; if (!host_&&index>=0&&index<static_cast<int>(games_.size())) {
        selected_=index; SetNote(L"Selected "+games_[index].title+L"."); } }
}
void App::PaintLive(gp::Graphics& g,const gp::RectF& r)
{
    if (!liveValid_) { Text(g,L"Waiting for capture diagnostics...",*fonts_.body,r,col::muted); return; }
    wchar_t line[256]={}; swprintf_s(line,L"Capture %.1f fps  ·  Host %.1f fps  ·  Present %.2f ms",live_.captureFps,live_.presentFps,live_.presentMs);
    Text(g,line,*fonts_.body,r,col::green);
}
void App::Paint(HDC dc,int width,int height)
{
    gp::Graphics g(dc); g.SetSmoothingMode(gp::SmoothingModeAntiAlias); g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
    gp::SolidBrush bg(col::bg); g.FillRectangle(&bg,0,0,width,height); hits_.clear();
    DrawRound(g,R(26,24,42,42),S(10),col::accent); gp::Pen mark(col::text,max(1.5f,S(2.5f))); mark.SetStartCap(gp::LineCapRound); mark.SetEndCap(gp::LineCapRound);
    g.DrawLine(&mark,S(36),S(36),S(36),S(54)); g.DrawLine(&mark,S(36),S(36),S(44),S(36)); g.DrawLine(&mark,S(58),S(36),S(58),S(44));
    g.DrawLine(&mark,S(50),S(36),S(58),S(36)); g.DrawLine(&mark,S(58),S(54),S(58),S(46)); g.DrawLine(&mark,S(50),S(54),S(58),S(54));
    g.DrawLine(&mark,S(36),S(54),S(36),S(46)); g.DrawLine(&mark,S(36),S(54),S(44),S(54));
    Text(g,L"OptiCaptureHost",*fonts_.title,R(80,20,390,34),col::text); Text(g,L"Window capture frontend  ·  OptiScaler owns graphics settings",*fonts_.caption,R(82,55,570,20),col::muted);
    const bool running=host_!=nullptr; Card(g,R(24,102,672,324)); Text(g,L"Capture target",*fonts_.heading,R(42,112,310,24),col::text);
    Text(g,L"Choose a visible window; exclusive fullscreen is not supported.",*fonts_.caption,R(42,136,430,18),col::muted);
    DrawRound(g,R(596,112,82,28),S(7),hover_==kIdRefresh?col::rowHot:col::row,&col::line,max(1.0f,S(1)));
    Text(g,L"Refresh",*fonts_.button,R(596,112,82,28),running?col::muted:col::text,1); if (!running) AddHit(R(596,112,82,28),kIdRefresh);
    const gp::RectF view=R(40,151,636,258); g.SetClip(view); float y=view.Y-scroll_;
    for (size_t i=0;i<games_.size();++i,y+=S(52)) {
        gp::RectF row(view.X,y,view.Width-S(8),S(46)); if (row.Y+row.Height<view.Y||row.Y>view.Y+view.Height) continue;
        const int id=kIdRowBase+static_cast<int>(i); const bool selected=static_cast<int>(i)==selected_;
        DrawRound(g,row,S(8),selected?col::rowSelected:(hover_==id?col::rowHot:col::row),selected?&col::accent:nullptr,max(1.0f,S(1)));
        const float iconSize=S(28); HDC iconDc=g.GetHDC(); DrawIconEx(iconDc,static_cast<int>(row.X+S(10)),static_cast<int>(row.Y+(row.Height-iconSize)/2),
            games_[i].icon?games_[i].icon:LoadIconW(nullptr,IDI_APPLICATION),static_cast<int>(iconSize),static_cast<int>(iconSize),0,nullptr,DI_NORMAL); g.ReleaseHDC(iconDc);
        Text(g,games_[i].title,*fonts_.body,gp::RectF(row.X+S(50),row.Y+S(4),row.Width-S(62),S(20)),col::text);
        std::wstring detail=games_[i].exe.empty()?L"Application":games_[i].exe; detail+=L"  ·  "+std::to_wstring(games_[i].width)+L"×"+std::to_wstring(games_[i].height);
        if (games_[i].minimized) detail+=L"  ·  minimized"; Text(g,detail,*fonts_.caption,gp::RectF(row.X+S(50),row.Y+S(24),row.Width-S(62),S(17)),col::muted);
        if (!running) AddHit(row,id);
    }
    if (games_.empty()) Text(g,L"No windows found. Start the game, then press Refresh.",*fonts_.body,view,col::muted,1); g.ResetClip();
    Card(g,R(24,440,322,108)); Text(g,L"Target window size",*fonts_.heading,R(42,450,270,24),col::text);
    static const wchar_t* scales[]={L"Native",L"67% of monitor",L"58% of monitor",L"50% of monitor"};
    DrawRound(g,R(42,480,286,34),S(8),hover_==kIdScale?col::rowHot:col::row,&col::line,max(1.0f,S(1)));
    Text(g,scales[settings_.targetScale],*fonts_.button,R(54,480,260,34),running?col::muted:col::text);
    if (!running) AddHit(R(42,480,286,34),kIdScale); Text(g,L"Native leaves the selected window unchanged.",*fonts_.caption,R(42,518,286,18),col::muted);
    Card(g,R(366,440,330,188)); Text(g,L"Host shortcuts",*fonts_.heading,R(384,450,280,24),col::text);
    static const wchar_t* shortcutNames[]={L"Stop capture",L"Toggle overlay input",L"Open OptiScaler menu",L"Show launcher"};
    for (int i=0;i<4;++i) { const float sy=480.0f+i*32.0f; Text(g,shortcutNames[i],*fonts_.caption,R(384,sy,150,24),col::muted);
        DrawRound(g,R(530,sy,148,24),S(6),hover_==kIdShortcutBase+i?col::rowHot:col::row,&col::line,max(1.0f,S(1)));
        Text(g,HotkeyLabel(i),*fonts_.caption,R(538,sy,132,24),capturingShortcut_==i?col::cyan:col::text,1);
        if (!running) AddHit(R(530,sy,148,24),kIdShortcutBase+i); }
    Text(g,L"Menu key is read from OptiScaler.ini without changing it.",*fonts_.caption,R(384,610,296,14),col::muted);
    Card(g,R(24,642,446,52)); if (running) PaintLive(g,R(42,648,410,24));
    else Text(g,note_.empty()?L"Ready. Source-game frame limits are set in the game.":note_,*fonts_.caption,R(42,648,410,24),noteError_?col::red:col::muted);
    Text(g,L"Diagnostics: OptiCaptureHost.log",*fonts_.caption,R(42,672,410,14),col::muted);
    DrawRound(g,R(492,642,204,52),S(10),hover_==kIdStart?col::cyan:(running?col::red:col::accent));
    Text(g,running?(stopping_?L"Stopping...":L"STOP CAPTURE"):L"START CAPTURE",*fonts_.button,R(492,642,204,52),col::text,1); AddHit(R(492,642,204,52),kIdStart);
}
LRESULT App::OnMessage(UINT m,WPARAM w,LPARAM l)
{
    switch(m) {
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd_,&ps); RECT rc{}; GetClientRect(hwnd_,&rc); Paint(dc,rc.right,rc.bottom); EndPaint(hwnd_,&ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: ApplyDpi(HIWORD(w),reinterpret_cast<const RECT*>(l)); return 0;
    case WM_TIMER: if (w==kTimerList&&!host_) Refresh(); else if (w==kTimerHost) PollHost(); return 0;
    case WM_HOTKEY: if (w==kHotkeyShow) ShowOnTop(); return 0;
    case WM_MOUSEMOVE: { if (!trackingMouse_) { TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd_,0}; TrackMouseEvent(&t); trackingMouse_=true; }
        int id=HitAt(GET_X_LPARAM(l),GET_Y_LPARAM(l)); if (id!=hover_) { hover_=id; InvalidateRect(hwnd_,nullptr,FALSE); } return 0; }
    case WM_MOUSELEAVE: trackingMouse_=false; hover_=0; InvalidateRect(hwnd_,nullptr,FALSE); return 0;
    case WM_LBUTTONDOWN: pressed_=HitAt(GET_X_LPARAM(l),GET_Y_LPARAM(l)); SetCapture(hwnd_); return 0;
    case WM_LBUTTONUP: { int up=HitAt(GET_X_LPARAM(l),GET_Y_LPARAM(l)); if (GetCapture()==hwnd_) ReleaseCapture(); int down=pressed_; pressed_=0;
        if (down&&down==up) OnClick(down); InvalidateRect(hwnd_,nullptr,FALSE); return 0; }
    case WM_MOUSEWHEEL: scroll_-=static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA)*S(52); ClampScroll(); InvalidateRect(hwnd_,nullptr,FALSE); return 0;
    case WM_KEYDOWN: if (capturingShortcut_>=0) { SetCapturedHotkey(static_cast<UINT>(w)); InvalidateRect(hwnd_,nullptr,FALSE); return 0; }
        if (w==VK_ESCAPE) { capturingShortcut_=-1; SetNote(L""); return 0; } return 0;
    case WM_CLOSE: if (host_) Stop(true); DestroyWindow(hwnd_); return 0;
    case WM_DESTROY: KillTimer(hwnd_,kTimerList); KillTimer(hwnd_,kTimerHost); UnregisterHotKey(hwnd_,kHotkeyShow);
        if (host_) Stop(true); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd_,m,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE once=CreateMutexW(nullptr,TRUE,L"Local\\OptiCaptureLauncher.SingleInstance");
    if (once&&GetLastError()==ERROR_ALREADY_EXISTS) { if (HWND other=FindWindowW(kClassName,nullptr)) {
        if (IsIconic(other)) ShowWindow(other,SW_RESTORE); SetForegroundWindow(other); } CloseHandle(once); return 0; }
    gp::GdiplusStartupInput input; ULONG_PTR token=0;
    if (gp::GdiplusStartup(&token,&input,nullptr)!=gp::Ok) { MessageBoxW(nullptr,L"Could not start GDI+.",kAppTitle,MB_OK|MB_ICONERROR);
        if (once) CloseHandle(once); return 1; }
    int result=1; { App app; if (app.Create(instance)) result=app.Loop();
        else MessageBoxW(nullptr,L"Could not create the launcher window.",kAppTitle,MB_OK|MB_ICONERROR); }
    gp::GdiplusShutdown(token); if (once) CloseHandle(once); return result;
}
