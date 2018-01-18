// BERT.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "variable.pb.h"
#include "XLCALL.h"
#include "function_descriptor.h"
#include "bert.h"
#include "basic_functions.h"
#include "type_conversions.h"
#include "string_utilities.h"
#include "windows_api_functions.h"
#include "message_utilities.h"
#include "..\resource.h"

#include "excel_com_type_libraries.h"
#include <google\protobuf\util\json_util.h>

#include "language_service_r.h"
#include "language_service_julia.h"

// this is an excel api function
extern void RegisterFunctions();

// this is an excel api function
extern void UnregisterFunctions();

/** debug/util function */
void DumpJSON(const google::protobuf::Message &message, const char *path = 0) {
#ifdef _DEBUG
  std::string str;
  google::protobuf::util::JsonOptions opts;
  opts.add_whitespace = true;
  google::protobuf::util::MessageToJsonString(message, &str, opts);
  if (path) {
    FILE *f;
    fopen_s(&f, path, "w");
    if (f) {
      fwrite(str.c_str(), sizeof(char), str.length(), f);
      fflush(f);
    }
    fclose(f);
  }
  else DebugOut("%s\n", str.c_str());
#endif
}

BERT* BERT::instance_ = 0;

BERT* BERT::Instance() {
  if (!instance_) instance_ = new BERT;
  return instance_;
}

void BERT::FileWatcherCallback(void *argument, const std::vector<std::string> &files) {
  BERT *bert = reinterpret_cast<BERT*>(argument);
  bert->FileWatchUpdate(files);
}

bool BERT::LoadLanguageFile(const std::string &file) {

  // check available langauges
  for (auto language_service : language_services_) {
    if (language_service.second->ValidFile(file)) {
      DebugOut("File %s matches %s\n", file.c_str(), language_service.second->name().c_str());

      // FIXME: log to console (or have the internal routine do that)
      language_service.second->ReadSourceFile(file);

      // match
      return true;
    }
  }

  // no matching language
  return false;
}

void BERT::FileWatchUpdate(const std::vector<std::string> &files){

  bool updated = false;
  for (auto file : files) {
    updated = updated || LoadLanguageFile(file);
  }

  // any changes?
  if (updated) {

    DebugOut("Updating...\n");

    // NOTE: this has to get on the correct thread. use COM to switch contexts
    // (and use the marshaled pointer) 
    // (and don't forget to release reference)

    LPDISPATCH dispatch_pointer = 0;
    HRESULT hresult = AtlUnmarshalPtr(stream_pointer_, IID_IDispatch, (LPUNKNOWN*)&dispatch_pointer);
    if (SUCCEEDED(hresult) && dispatch_pointer) {
      CComQIPtr<Excel::_Application> application(dispatch_pointer);
      if (application) {
        CComVariant variant_macro = "BERT.UpdateFunctions";
        CComVariant variant_result = application->Run(variant_macro);
      }
      dispatch_pointer->Release();
    }

  }
}

int BERT::UpdateFunctions() {

  // FIXME: notify user via console

  // unregister uses IDs assigned in the registration process. so we need
  // to call unregister before map, which will change the function list.
  UnregisterFunctions();

  // scrub
  function_list_.clear();

  // now update
  MapFunctions();
  RegisterFunctions();

  return 0;

}

BERT::BERT()
  :  dev_flags_(0)
  , file_watcher_(BERT::FileWatcherCallback, this)
  , stream_pointer_(0)
{
  APIFunctions::GetRegistryDWORD(dev_flags_, "BERT2.DevOptions");
}

void BERT::OpenConsole() {

  if (console_process_id_) {
    // ... show ...
  }
  else {
    StartConsoleProcess();
  }

}

void BERT::SetPointers(ULONG_PTR excel_pointer, ULONG_PTR ribbon_pointer) {

  application_dispatch_ = reinterpret_cast<LPDISPATCH>(excel_pointer);

  // marshall pointer
  AtlMarshalPtrInProc(application_dispatch_, IID_IDispatch, &stream_pointer_);

  // set pointer in various language services
  for (const auto &language_service_pair : language_services_) {
    language_service_pair.second->SetApplicationPointer(application_dispatch_);
  }

}

int BERT::StartConsoleProcess() {

  std::string console_command;
  std::string console_arguments;

  APIFunctions::GetRegistryString(console_arguments, "BERT2.ConsoleArguments");
  APIFunctions::GetRegistryString(console_command, "BERT2.ConsoleCommand");

  // FIXME: need to pass pipes for different languages...

  std::string pipe_name = language_services_[0]->pipe_name();

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;

  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);

  ZeroMemory(&pi, sizeof(pi));

  int rslt = 0;
  char *args = new char[1024];

  // really? A: yes, it needs a non-const buffer
  sprintf_s(args, 1024, "\"%s\" %s -p %s -d %d", console_command.c_str(), console_arguments.c_str(), pipe_name.c_str(), dev_flags_);

  if (!CreateProcessA(0, args, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
    DebugOut("CreateProcess failed (%d).\n", GetLastError());
    rslt = GetLastError();
  }
  else {
    console_process_id_ = pi.dwProcessId;
    if (job_handle_) {
      if (!AssignProcessToJobObject(job_handle_, pi.hProcess))
      {
        DebugOut("Could not AssignProcessToObject\n");
      }
    }
  }

  delete[] args;

  return rslt;
}

void BERT::HandleCallback() {

  // this function gets called from a thread (i.e. not the main excel thread),
  // so we cannot call the excel API but we can call excel via COM using the 
  // marshalled pointer.

  // if we want to support the Excel API (and I suppose we do) we'll need to 
  // get back on the main thread. we could do that with COM, and a call-through,
  // but I'm not sure if that will preserve context (from a function call).

  // if COM doesn't work in this situation we'll need to use an event we can 
  // signal the blocked thread, but we will need to know if we are in fact 
  // blocking that thread. we'll also need a way to pass data back and forth.

  // SO: COM doesn't work in that case. we need to signal. 

  // there are two possible cases. either we are being called from a spreadsheet
  // function or we're being called from a shell function. the semantics are different
  // because the main thread may or may not be blocked.

  DWORD wait_result = WaitForSingleObject(callback_info_.default_signaled_event_, 0);
  if (wait_result == WAIT_OBJECT_0) {
    // DebugOut("event 2 is already signaled; this is a shell function\n");

    BERTBuffers::CallResponse &call = callback_info_.callback_call_;
    BERTBuffers::CallResponse &response = callback_info_.callback_response_;

    if (stream_pointer_) {
      LPDISPATCH dispatch_pointer = 0;
      CComVariant variant_result;
      HRESULT hresult = AtlUnmarshalPtr(stream_pointer_, IID_IDispatch, (LPUNKNOWN*)&dispatch_pointer);

      if (SUCCEEDED(hresult)) {
        CComQIPtr<Excel::_Application> application(dispatch_pointer);
        if (application) {
          CComVariant variant_macro = "BERT.ContextSwitch";
          CComVariant variant_result = application->Run(variant_macro);
        }
        else response.set_err("qi failed");
        dispatch_pointer->Release();
      }
      else response.set_err("unmarshal failed");
    }
    else {
      response.set_err("invalid stream pointer");
    }

  }
  else {
    DebugOut("event 2 is not signaled; this is a spreadsheet function\n");
    DebugOut("callback waiting for signal\n");

    // let main thread handle
    SetEvent(callback_info_.default_unsignaled_event_);
    WaitForSingleObject(callback_info_.default_signaled_event_, INFINITE);
    DebugOut("callback signaled\n");
  }
}

int BERT::HandleCallbackOnThread(const BERTBuffers::CallResponse *call, BERTBuffers::CallResponse *response) {

  if (!call) call = &(callback_info_.callback_call_);
  if (!response) response = &(callback_info_.callback_response_);

  int return_value = 0;

  // DumpJSON(call);
  response->set_id(call->id());

  if (call->operation_case() == BERTBuffers::CallResponse::OperationCase::kFunctionCall) {

    auto callback = call->function_call();
    if (callback.target() == BERTBuffers::CallTarget::language) {

      auto function = callback.function();
      if (!function.compare("excel")) {
        return_value = ExcelCallback(*call, *response);
      }
      else if (!function.compare("release-pointer")) {
        if (callback.arguments_size() > 0) {
          DebugOut("release pointer 0x%lx\n", callback.arguments(0).external_pointer());
          object_map_.RemoveCOMPointer(static_cast<ULONG_PTR>(callback.arguments(0).external_pointer()));
        }
      }
      else {
        response->mutable_result()->set_boolean(false);
      }
    }
    else if (callback.target() == BERTBuffers::CallTarget::COM) {
      object_map_.InvokeCOMFunction(callback, *response);
    }
    else {
      response->mutable_result()->set_boolean(false);
    }
  }
  else {
    response->mutable_result()->set_boolean(false);
  }

  // response.mutable_value()->set_boolean(true);
  return return_value;

}

int BERT::ExcelCallback(const BERTBuffers::CallResponse &call, BERTBuffers::CallResponse &response) {

  auto callback = call.function_call();
  auto function = callback.function();

  int32_t command = 0;
  int32_t success = -1;

  if (callback.arguments_size() > 0) {
    auto arguments_array = callback.arguments(0).arr();

    int count = arguments_array.data().size();
    if (count > 0) {
      command = (int32_t)arguments_array.data(0).num();
    }
    if (command) {
      XLOPER12 excel_result;
      std::vector<LPXLOPER12> excel_arguments;
      for (int i = 1; i < count; i++) {
        LPXLOPER12 argument = new XLOPER12;
        excel_arguments.push_back(Convert::VariableToXLOPER(argument, arguments_array.data(i)));
      }
      if (excel_arguments.size()) success = Excel12v(command, &excel_result, (int32_t)excel_arguments.size(), &(excel_arguments[0]));
      else success = Excel12(command, &excel_result, 0, 0);
      Convert::XLOPERToVariable(response.mutable_result(), &excel_result);
      Excel12(xlFree, 0, 1, &excel_result);
    }
  }

  return success;
}

void BERT::Init() {

  // the job object created here is used to kill child processes
  // in the event of an excel exit (for any reason).

  job_handle_ = CreateJobObject(0, 0);

  if (job_handle_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    memset(&jeli, 0, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_handle_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
    {
      DebugOut("Could not SetInformationJobObject\n");
    }
  }
  else {
    DebugOut("Create job object failed\n");
  }

  // init R
  {
    std::string r_home;
    std::string child_path;
    std::string pipe_name;

    APIFunctions::GetRegistryString(r_home, "BERT2.RHome");
    APIFunctions::GetRegistryString(child_path, "BERT2.ControlRCommand");

    APIFunctions::GetRegistryString(pipe_name, "BERT2.OverrideRPipeName");
    if (!pipe_name.length()) {
      std::stringstream ss;
      ss << "BERT2-PIPE-R-" << _getpid();
      pipe_name = ss.str();
    }

    auto r_service = std::make_shared<LanguageServiceR>(callback_info_, object_map_, dev_flags_, pipe_name, child_path, r_home);
    language_services_.insert_or_assign(r_service->key(), r_service);
  }

  // julia
  {
    std::string julia_home;
    std::string child_path;
    std::string pipe_name;

    APIFunctions::GetRegistryString(julia_home, "BERT2.JuliaHome");
    APIFunctions::GetRegistryString(child_path, "BERT2.ControlJuliaCommand");

    APIFunctions::GetRegistryString(pipe_name, "BERT2.OverrideJuliaPipeName");
    if (!pipe_name.length()) {
      std::stringstream ss;
      ss << "BERT2-PIPE-JL-" << _getpid();
      pipe_name = ss.str();
    }

    auto julia_service = std::make_shared<LanguageServiceJulia>(callback_info_, object_map_, dev_flags_, pipe_name, child_path, julia_home);
    language_services_.insert_or_assign(julia_service->key(), julia_service);
  }

  // connect all first; then initialize
  for (const auto &language_service_pair : language_services_) {
    language_service_pair.second->Connect(job_handle_);
  }

  // ... insert callback thread ...

  for (const auto &language_service_pair : language_services_) {
    language_service_pair.second->Initialize();
  }

  if (dev_flags_) StartConsoleProcess();

  // load code from starup folder(s). then start watching folders.

  {
    std::string functions_directory;
    APIFunctions::GetRegistryString(functions_directory, "BERT2.FunctionsDirectory");
    if (functions_directory.length()) {

      // list...
      std::vector< std::pair< std::string, FILETIME >> directory_entries = APIFunctions::ListDirectory(functions_directory);

      // load...
      for (auto file_info : directory_entries) {
        LoadLanguageFile(file_info.first);
      }

      // now watch
      file_watcher_.WatchDirectory(functions_directory);
    }
    file_watcher_.StartWatch();
  }
}

void BERT::MapFunctions() {
  function_list_.clear();
  for (const auto &language_service_pair : language_services_) {
    FUNCTION_LIST functions = language_service_pair.second->MapLanguageFunctions();
    function_list_.insert(function_list_.end(), functions.begin(), functions.end());
  }
}

std::shared_ptr<LanguageService> BERT::GetLanguageService(uint32_t key) {
  auto iterator = language_services_.find(key);
  if (iterator == language_services_.end()) return 0;
  return iterator->second;
}

void BERT::CallLanguage(uint32_t language_key, BERTBuffers::CallResponse &response, BERTBuffers::CallResponse &call) {
  auto language_service = GetLanguageService(language_key);
  if (language_service) language_service->Call(response, call);
}

void BERT::Close() {

  // file watch thread
  file_watcher_.Shutdown();

  // shutdown services
  for (const auto &language_service_pair : language_services_) {
    language_service_pair.second->Shutdown();
  }

  // free marshalled pointer
  if (stream_pointer_) AtlFreeMarshalStream(stream_pointer_);
  
}
