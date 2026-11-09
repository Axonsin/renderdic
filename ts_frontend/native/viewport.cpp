// Small stable Node-API bridge. Resolve Node-API from Electron's executable so
// the standalone build needs only the Windows SDK, not Electron/V8 headers.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <stdint.h>
#include <stdio.h>
using napi_env=void*;using napi_value=void*;using napi_callback_info=void*;
using Callback=napi_value(*)(napi_env,napi_callback_info);
static int (*getInfo)(napi_env,napi_callback_info,size_t*,napi_value*,napi_value*,void**);
static int (*getString)(napi_env,napi_value,char*,size_t,size_t*);
static int (*makeString)(napi_env,const char*,size_t,napi_value*);
static int (*makeFunction)(napi_env,const char*,size_t,Callback,void*,napi_value*);
static int (*setProperty)(napi_env,napi_value,const char*,napi_value);
static int (*getUndefined)(napi_env,napi_value*);
static int (*throwError)(napi_env,const char*,const char*);
static HWND viewport=nullptr;
static HWND container=nullptr;
static HWND owner=nullptr;
static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
  if(msg==WM_PAINT){PAINTSTRUCT ps;BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);return 0;}
  if(msg==WM_ERASEBKGND)return 1;
  if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
  return DefWindowProcW(hwnd,msg,w,l);
}
static napi_value Destroy(napi_env env,napi_callback_info){if(container)DestroyWindow(container);viewport=nullptr;container=nullptr;owner=nullptr;napi_value result;getUndefined(env,&result);return result;}
static napi_value Create(napi_env env,napi_callback_info info){
  Destroy(env,nullptr);size_t argc=1;napi_value argument;getInfo(env,info,&argc,&argument,nullptr,nullptr);
  char handle[64]={};size_t length=0;
  if(argc!=1||getString(env,argument,handle,sizeof(handle),&length)){throwError(env,nullptr,"Expected parent HWND string");return nullptr;}
  HWND parent=reinterpret_cast<HWND>((uintptr_t)_strtoui64(handle,nullptr,10));
  WNDCLASSW cls={};cls.style=CS_OWNDC|CS_HREDRAW|CS_VREDRAW;cls.lpfnWndProc=Proc;cls.hInstance=GetModuleHandleW(nullptr);
  cls.lpszClassName=L"RenderDicTSElectronSurface";cls.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&cls);
  SetWindowLongPtrW(parent,GWL_STYLE,GetWindowLongPtrW(parent,GWL_STYLE)|WS_CLIPCHILDREN);
  owner=parent;
  // A non-activating, owned container establishes a separate DWM composition target.
  // Chromium otherwise paints its compositor surface over a foreign swapchain.
  // The actual replay HWND is a child of this container; neither has taskbar chrome.
  container=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"RenderDic preview container",WS_POPUP|WS_CLIPSIBLINGS|WS_CLIPCHILDREN,0,0,1,1,parent,nullptr,cls.hInstance,nullptr);
  viewport=CreateWindowExW(0,cls.lpszClassName,L"RenderDic replay",WS_CHILD|WS_CLIPSIBLINGS|WS_CLIPCHILDREN,0,0,1,1,container,nullptr,cls.hInstance,nullptr);
  if(!viewport){throwError(env,nullptr,"Cannot create replay HWND in Electron host");return nullptr;}
  snprintf(handle,sizeof(handle),"%llu",(unsigned long long)(uintptr_t)viewport);napi_value result;makeString(env,handle,(size_t)-1,&result);return result;
}
static napi_value Bounds(napi_env env,napi_callback_info info){
  size_t argc=1;napi_value argument;getInfo(env,info,&argc,&argument,nullptr,nullptr);char text[256]={};size_t size;
  if(argc!=1||getString(env,argument,text,sizeof(text),&size))return nullptr;
  double x=0,y=0,w=1,h=1,scale=1;int visible=0;
  if(sscanf_s(text,"[%lf,%lf,%lf,%lf,%lf,%d]",&x,&y,&w,&h,&scale,&visible)!=6){throwError(env,nullptr,"Invalid viewport coordinates");return nullptr;}
  if(container){POINT origin={(LONG)(x*scale+0.5),(LONG)(y*scale+0.5)};ClientToScreen(owner,&origin);
    SetWindowPos(container,nullptr,origin.x,origin.y,max(1,(int)(w*scale+0.5)),max(1,(int)(h*scale+0.5)),SWP_NOACTIVATE|SWP_NOZORDER);
    ShowWindow(container,visible&&w>0&&h>0?SW_SHOWNOACTIVATE:SW_HIDE);
  }
  napi_value result;getUndefined(env,&result);return result;
}
extern "C" __declspec(dllexport) int node_api_module_get_api_version_v1(){return 8;}
extern "C" __declspec(dllexport) napi_value napi_register_module_v1(napi_env env,napi_value exports){
  HMODULE node=GetModuleHandleW(nullptr);
#define LOAD(variable,name) variable=reinterpret_cast<decltype(variable)>(GetProcAddress(node,name));if(!variable)return nullptr;
  LOAD(getInfo,"napi_get_cb_info");LOAD(getString,"napi_get_value_string_utf8");LOAD(makeString,"napi_create_string_utf8");
  LOAD(makeFunction,"napi_create_function");LOAD(setProperty,"napi_set_named_property");LOAD(getUndefined,"napi_get_undefined");LOAD(throwError,"napi_throw_error");
  napi_value create,destroy,bounds;makeFunction(env,"create",(size_t)-1,Create,nullptr,&create);makeFunction(env,"destroy",(size_t)-1,Destroy,nullptr,&destroy);makeFunction(env,"bounds",(size_t)-1,Bounds,nullptr,&bounds);
  setProperty(env,exports,"create",create);setProperty(env,exports,"destroy",destroy);setProperty(env,exports,"bounds",bounds);return exports;
}
