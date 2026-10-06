// SPDX-License-Identifier: MPL-2.0
#include "ShellStart.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <exdisp.h>
#include <servprov.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <format>

namespace urnw::updater {
namespace {

using Microsoft::WRL::ComPtr;

class Bstr {
 public:
  explicit Bstr(const wchar_t* text) : value_(::SysAllocString(text)) {}
  ~Bstr() { ::SysFreeString(value_); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  BSTR get() const { return value_; }

 private:
  BSTR value_;
};

// COM on this thread, for as long as the object lives.
class ComApartment {
 public:
  ComApartment()
      : result_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
  ~ComApartment() {
    if (SUCCEEDED(result_)) ::CoUninitialize();
  }
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;
  HRESULT result() const { return result_; }

 private:
  HRESULT result_;
};

bool Failed(std::string& error, const char* step, HRESULT hr) {
  error = std::format("{} failed: 0x{:08x}", step, static_cast<unsigned>(hr));
  return false;
}

}  // namespace

bool StartThroughShell(const std::filesystem::path& program, std::string& error) {
  const ComApartment com;
  if (FAILED(com.result())) return Failed(error, "CoInitializeEx", com.result());

  // The desktop window's shell browser, its view, and the Shell.Application
  // object behind that view, all in Explorer: what that object starts, the
  // shell starts, as the user it runs as.
  ComPtr<IShellWindows> windows;
  HRESULT hr = ::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                  __uuidof(IShellWindows),
                                  reinterpret_cast<void**>(windows.GetAddressOf()));
  if (FAILED(hr)) return Failed(error, "CoCreateInstance(ShellWindows)", hr);
  VARIANT empty{};
  long desktop = 0;
  ComPtr<IDispatch> desktopDispatch;
  hr = windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH,
                             desktopDispatch.GetAddressOf());
  if (hr != S_OK || !desktopDispatch) {
    return Failed(error, "FindWindowSW(the desktop)", hr == S_OK || hr == S_FALSE ? E_FAIL : hr);
  }
  ComPtr<IServiceProvider> services;
  hr = desktopDispatch.As(&services);
  if (FAILED(hr)) return Failed(error, "IServiceProvider", hr);
  ComPtr<IShellBrowser> browser;
  hr = services->QueryService(SID_STopLevelBrowser, __uuidof(IShellBrowser),
                              reinterpret_cast<void**>(browser.GetAddressOf()));
  if (FAILED(hr)) return Failed(error, "QueryService(SID_STopLevelBrowser)", hr);
  ComPtr<IShellView> view;
  hr = browser->QueryActiveShellView(view.GetAddressOf());
  if (FAILED(hr)) return Failed(error, "QueryActiveShellView", hr);
  ComPtr<IDispatch> background;
  hr = view->GetItemObject(SVGIO_BACKGROUND, __uuidof(IDispatch),
                           reinterpret_cast<void**>(background.GetAddressOf()));
  if (FAILED(hr)) return Failed(error, "GetItemObject(SVGIO_BACKGROUND)", hr);
  ComPtr<IShellFolderViewDual> folderView;
  hr = background.As(&folderView);
  if (FAILED(hr)) return Failed(error, "IShellFolderViewDual", hr);
  ComPtr<IDispatch> application;
  hr = folderView->get_Application(application.GetAddressOf());
  if (FAILED(hr)) return Failed(error, "get_Application", hr);
  ComPtr<IShellDispatch2> shell;
  hr = application.As(&shell);
  if (FAILED(hr)) return Failed(error, "IShellDispatch2", hr);

  const Bstr file(program.c_str());
  const Bstr folder(program.parent_path().c_str());
  const Bstr verb(L"open");
  if (!file.get() || !folder.get() || !verb.get()) return Failed(error, "SysAllocString", E_OUTOFMEMORY);
  VARIANT arguments{};  // VT_EMPTY: none
  VARIANT directory{};
  directory.vt = VT_BSTR;
  directory.bstrVal = folder.get();
  VARIANT operation{};
  operation.vt = VT_BSTR;
  operation.bstrVal = verb.get();
  VARIANT show{};
  show.vt = VT_INT;
  show.intVal = SW_SHOWNORMAL;
  hr = shell->ShellExecute(file.get(), arguments, directory, operation, show);
  if (hr != S_OK) return Failed(error, "IShellDispatch2::ShellExecute", hr == S_FALSE ? E_FAIL : hr);
  return true;
}

}  // namespace urnw::updater
