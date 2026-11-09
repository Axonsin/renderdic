#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Python.h>
#include <fcntl.h>
#include <io.h>
#include <string>

// This module belongs exclusively to the TS frontend. No RenderDoc headers or ABI wrappers.
static HWND surface = nullptr;
static bool dirty = false;
static PyObject *Fail(const char *what)
{
  return PyErr_Format(PyExc_RuntimeError, "%s (Win32 error %lu)", what, GetLastError());
}
static PyObject *Bounds(PyObject *, PyObject *args)
{
  double x,y,w,h,scale; int visible;
  if(!PyArg_ParseTuple(args,"dddddp",&x,&y,&w,&h,&scale,&visible)) return nullptr;
  if(surface) {
    auto px=[scale](double v){return static_cast<int>(v*scale+0.5);};
    SetWindowPos(surface,HWND_TOP,px(x),px(y),max(1,px(w)),max(1,px(h)),SWP_NOACTIVATE);
    ShowWindow(surface,visible&&w>0&&h>0?SW_SHOWNOACTIVATE:SW_HIDE); dirty=true;
  }
  Py_RETURN_NONE;
}
static PyObject *Attach(PyObject *,PyObject *args){const char *id;if(!PyArg_ParseTuple(args,"s",&id))return nullptr;surface=(HWND)(uintptr_t)_strtoui64(id,nullptr,10);if(!IsWindow(surface))return Fail("Invalid external HWND");if(!SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(surface)))return Fail("Cannot match replay HWND DPI context");dirty=true;Py_RETURN_NONE;}
static PyObject *Destroy(PyObject *,PyObject *) { surface=nullptr;dirty=false;Py_RETURN_NONE; }
static PyObject *Pump(PyObject *,PyObject *)
{
  MSG msg;
  unsigned count=0;
  while(count++<64 && PeekMessageW(&msg,surface,0,0,PM_REMOVE|PM_NOYIELD)) {
    TranslateMessage(&msg);DispatchMessageW(&msg);
  }
  bool redraw=dirty;dirty=false;return PyBool_FromLong(redraw);
}
static PyObject *Inspect(PyObject *,PyObject *)
{
  RECT r={}; if(surface)GetWindowRect(surface,&r);
  POINT center={(r.left+r.right)/2,(r.top+r.bottom)/2};HWND top=WindowFromPoint(center);wchar_t cls[256]={};GetClassNameW(top,cls,256);
  char hwndText[32],parentText[32],ownerText[32];
  _snprintf_s(hwndText,sizeof(hwndText),_TRUNCATE,"%llu",(unsigned long long)(uintptr_t)surface);
  _snprintf_s(parentText,sizeof(parentText),_TRUNCATE,"%llu",(unsigned long long)(uintptr_t)GetParent(surface));
  _snprintf_s(ownerText,sizeof(ownerText),_TRUNCATE,"%llu",(unsigned long long)(uintptr_t)GetWindow(GetParent(surface),GW_OWNER));
  PyObject *result=Py_BuildValue("{s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:i,s:i}","hwnd",hwndText,
    "parent",parentText,"owner",ownerText,"x",r.left,"y",r.top,
    "width",r.right-r.left,"height",r.bottom-r.top,"visible",surface?IsWindowVisible(surface):0,
    "dpi",surface?GetDpiForWindow(surface):0);
  PyObject *topClass=PyUnicode_FromWideChar(cls,-1);PyDict_SetItemString(result,"topClass",topClass);Py_DECREF(topClass);
  PyObject *unobscured=PyBool_FromLong(top==surface);PyDict_SetItemString(result,"unobscured",unobscured);Py_DECREF(unobscured);
  HDC screen=GetDC(nullptr);COLORREF color=GetPixel(screen,center.x,center.y);ReleaseDC(nullptr,screen);
  PyObject *pixel=PyLong_FromUnsignedLong(color);PyDict_SetItemString(result,"screenPixel",pixel);Py_DECREF(pixel);
  return result;
}
static PyMethodDef methods[]={ {"bounds",Bounds,METH_VARARGS,nullptr},
  {"attach",Attach,METH_VARARGS,nullptr},
  {"destroy",Destroy,METH_NOARGS,nullptr},{"pump",Pump,METH_NOARGS,nullptr},{"inspect",Inspect,METH_NOARGS,nullptr},{nullptr,nullptr,0,nullptr} };
static PyModuleDef module={PyModuleDef_HEAD_INIT,"_frontend_native",nullptr,-1,methods};
static PyObject *InitModule(){return PyModule_Create(&module);}
static DWORD WINAPI ParentWatch(void *p) {WaitForSingleObject(static_cast<HANDLE>(p),INFINITE);TerminateProcess(GetCurrentProcess(),0);return 0;}
int wmain(int argc,wchar_t **argv)
{
  if(argc>2 && wcscmp(argv[1],L"--parent-pid")==0) {
    HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,wcstoul(argv[2],nullptr,10));
    if(!parent)return 3;
    HANDLE thread=CreateThread(nullptr,0,ParentWatch,parent,0,nullptr);if(thread)CloseHandle(thread);
  }
  wchar_t exe[32768]; GetModuleFileNameW(nullptr,exe,32768);
  std::wstring directory(exe);directory.resize(directory.find_last_of(L"\\/"));
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  AddDllDirectory(directory.c_str());
  _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
  PyImport_AppendInittab("_frontend_native",InitModule);
  std::wstring search=directory+L"\\python36.zip;"+directory+L";"+directory+L"\\bridge";
  Py_SetProgramName(exe);Py_SetPythonHome(const_cast<wchar_t*>(directory.c_str()));
  Py_SetPath(search.c_str());Py_SetStandardStreamEncoding("utf-8","replace");
  Py_Initialize();PySys_SetArgvEx(argc,argv,0);
  std::wstring script=directory+L"\\bridge\\worker.py";
  FILE *file=nullptr;_wfopen_s(&file,script.c_str(),L"rb");
  if(!file){fprintf(stderr,"Missing worker script\n");Py_Finalize();return 2;}
  int result=PyRun_SimpleFileEx(file,"worker.py",1);
  Py_Finalize();return result?1:0;
}
