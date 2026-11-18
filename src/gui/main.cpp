// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"XfswinManagementWindow";
constexpr int kRefresh = 1001;
constexpr int kStart = 1002;
constexpr int kEnable = 1003;
constexpr int kDisable = 1004;
constexpr int kMount = 1005;
constexpr int kUnmount = 1006;
constexpr int kOpen = 1007;
constexpr int kExport = 1008;
constexpr int kList = 1009;
constexpr int kEnableRw = 1010;
constexpr int kEject = 1011;
constexpr int kElevate = 1012;
constexpr int kExportJson = 1013;
constexpr int kMountRw = 1014;
constexpr int kDetails = 1015;
constexpr int kDriveChoice = 1016;
constexpr UINT kRefreshDone = WM_APP + 1;

struct DeviceRow { std::wstring label, uuid, drive, automount_mode, effective_mode, device_class, bus_type, capacity, device_size, serial, stable_id; bool automount_enabled = false, discovered = false, enrolled = false, mounted = false; };
struct State { HWND list{}, details{}, drive_choice{}; std::vector<DeviceRow> devices; bool busy = false; bool closing = false; };
State* g_state = nullptr;

std::wstring Quote(const std::wstring& value) {
  std::wstring result = L"\"";
  unsigned slashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'\"') result.append(slashes * 2 + 1, L'\\');
    else result.append(slashes, L'\\');
    slashes = 0;
    result.push_back(c);
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::wstring CliPath() {
  std::wstring path(32768, L'\0');
  DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) return {};
  path.resize(length);
  const size_t slash = path.find_last_of(L"\\/");
  path.resize(slash == std::wstring::npos ? 0 : slash + 1);
  path += L"xfswin.exe";
  return path;
}

std::wstring RunCli(const std::vector<std::wstring>& args, DWORD* exit_code = nullptr, DWORD timeout_ms = 60000) {
  const auto exe = CliPath();
  if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return L"xfswin.exe was not found beside xfswin-gui.exe.";
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE read_pipe = nullptr, write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return L"Cannot create a command output pipe.";
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  std::wstring command = Quote(exe);
  for (const auto& arg : args) command += L" " + Quote(arg);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr,
                                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                      &startup, &process);
  CloseHandle(write_pipe);
  if (!started) {
    CloseHandle(read_pipe);
    return GetLastError() == ERROR_ACCESS_DENIED
      ? L"Access denied. Reopen the application with the required Windows permissions."
      : L"Could not start xfswin.exe. Check that the adjacent CLI is available.";
  }
  std::string bytes;
  char buffer[4096];
  DWORD count = 0;
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  bool timed_out = false;
  for (;;) {
    DWORD available = 0;
    if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
      const DWORD want = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
      if (ReadFile(read_pipe, buffer, want, &count, nullptr) && count) bytes.append(buffer, count);
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 50);
    if (wait == WAIT_OBJECT_0) {
      while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
        const DWORD want = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(read_pipe, buffer, want, &count, nullptr) || !count) break;
        bytes.append(buffer, count);
      }
      break;
    }
    if (GetTickCount64() >= deadline) { timed_out = true; TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); break; }
  }
  CloseHandle(read_pipe);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exit_code) *exit_code = timed_out ? WAIT_TIMEOUT : code;
  if (timed_out) return L"The CLI command timed out after " + std::to_wstring(timeout_ms / 1000) + L" seconds and was stopped.";
  if (bytes.empty()) return code ? L"The command failed. Check permissions, WinFsp, service state, and drive-letter availability." : L"Command completed successfully.";
  const int chars = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
  std::wstring result(chars > 0 ? chars : 0, L'\0');
  if (chars) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), result.data(), chars);
  return result;
}

std::wstring JsonField(const std::wstring& object, const wchar_t* name) {
  auto skip_space = [&](size_t* p) {
    while (*p < object.size() && iswspace(object[*p])) ++*p;
  };
  auto parse_string = [&](size_t* p, std::wstring* value) -> bool {
    if (*p >= object.size() || object[(*p)++] != L'"') return false;
    while (*p < object.size()) {
      wchar_t c = object[(*p)++];
      if (c == L'"') return true;
      if (c != L'\\') { value->push_back(c); continue; }
      if (*p >= object.size()) return false;
      c = object[(*p)++];
      switch (c) {
        case L'"': case L'\\': case L'/': value->push_back(c); break;
        case L'b': value->push_back(L'\b'); break;
        case L'f': value->push_back(L'\f'); break;
        case L'n': value->push_back(L'\n'); break;
        case L'r': value->push_back(L'\r'); break;
        case L't': value->push_back(L'\t'); break;
        case L'u': {
          if (*p + 4 > object.size()) return false;
          wchar_t code = 0;
          for (int i = 0; i < 4; ++i) {
            const wchar_t h = object[(*p)++];
            code = static_cast<wchar_t>(code << 4);
            if (h >= L'0' && h <= L'9') code += h - L'0';
            else if (h >= L'a' && h <= L'f') code += h - L'a' + 10;
            else if (h >= L'A' && h <= L'F') code += h - L'A' + 10;
            else return false;
          }
          value->push_back(code);
          break;
        }
        default: return false;
      }
    }
    return false;
  };
  size_t p = 0;
  skip_space(&p);
  if (p >= object.size() || object[p++] != L'{') return {};
  while (p < object.size()) {
    skip_space(&p);
    if (p < object.size() && object[p] == L'}') return {};
    std::wstring key;
    if (!parse_string(&p, &key)) return {};
    skip_space(&p);
    if (p >= object.size() || object[p++] != L':') return {};
    skip_space(&p);
    const size_t value_start = p;
    std::wstring value;
    if (p < object.size() && object[p] == L'"') {
      if (!parse_string(&p, &value)) return {};
    } else {
      int depth = 0;
      bool in_string = false, escaped = false;
      while (p < object.size()) {
        const wchar_t c = object[p];
        if (in_string) {
          if (escaped) escaped = false;
          else if (c == L'\\') escaped = true;
          else if (c == L'"') in_string = false;
        } else if (c == L'"') in_string = true;
        else if (c == L'{' || c == L'[') ++depth;
        else if (c == L'}' || c == L']') { if (depth == 0) break; --depth; }
        else if (c == L',' && depth == 0) break;
        ++p;
      }
      value = object.substr(value_start, p - value_start);
    }
    if (key == name) return value;
    skip_space(&p);
    if (p >= object.size()) return {};
    if (object[p] == L',') { ++p; continue; }
    if (object[p] == L'}') return {};
    return {};
  }
  return {};
}

void SkipJsonWhitespace(const std::wstring& json, size_t* position) {
  while (*position < json.size() &&
         (json[*position] == L' ' || json[*position] == L'\t' ||
          json[*position] == L'\r' || json[*position] == L'\n')) {
    ++*position;
  }
}

bool ParseJsonString(const std::wstring& json, size_t* position) {
  if (*position >= json.size() || json[(*position)++] != L'"') return false;
  while (*position < json.size()) {
    const wchar_t c = json[(*position)++];
    if (c == L'"') return true;
    if (c < 0x20) return false;
    if (c != L'\\') continue;
    if (*position >= json.size()) return false;
    const wchar_t escaped = json[(*position)++];
    if (escaped == L'"' || escaped == L'\\' || escaped == L'/' ||
        escaped == L'b' || escaped == L'f' || escaped == L'n' ||
        escaped == L'r' || escaped == L't') {
      continue;
    }
    if (escaped != L'u' || *position + 4 > json.size()) return false;
    for (int i = 0; i < 4; ++i) {
      const wchar_t h = json[(*position)++];
      if (!((h >= L'0' && h <= L'9') || (h >= L'a' && h <= L'f') ||
            (h >= L'A' && h <= L'F'))) {
        return false;
      }
    }
  }
  return false;
}

bool ParseJsonNumber(const std::wstring& json, size_t* position) {
  const size_t start = *position;
  if (*position < json.size() && json[*position] == L'-') ++*position;
  if (*position >= json.size()) return false;
  if (json[*position] == L'0') {
    ++*position;
  } else if (json[*position] >= L'1' && json[*position] <= L'9') {
    while (*position < json.size() && json[*position] >= L'0' &&
           json[*position] <= L'9') {
      ++*position;
    }
  } else {
    return false;
  }
  if (*position < json.size() && json[*position] == L'.') {
    ++*position;
    const size_t fraction = *position;
    while (*position < json.size() && json[*position] >= L'0' &&
           json[*position] <= L'9') {
      ++*position;
    }
    if (*position == fraction) return false;
  }
  if (*position < json.size() &&
      (json[*position] == L'e' || json[*position] == L'E')) {
    ++*position;
    if (*position < json.size() &&
        (json[*position] == L'+' || json[*position] == L'-')) {
      ++*position;
    }
    const size_t exponent = *position;
    while (*position < json.size() && json[*position] >= L'0' &&
           json[*position] <= L'9') {
      ++*position;
    }
    if (*position == exponent) return false;
  }
  return *position > start;
}

bool ParseJsonValue(const std::wstring& json, size_t* position,
                    unsigned depth) {
  if (depth > 128) return false;
  SkipJsonWhitespace(json, position);
  if (*position >= json.size()) return false;
  if (json[*position] == L'"') return ParseJsonString(json, position);
  if (json[*position] == L'-' ||
      (json[*position] >= L'0' && json[*position] <= L'9')) {
    return ParseJsonNumber(json, position);
  }
  if (json.compare(*position, 4, L"true") == 0) {
    *position += 4;
    return true;
  }
  if (json.compare(*position, 5, L"false") == 0) {
    *position += 5;
    return true;
  }
  if (json.compare(*position, 4, L"null") == 0) {
    *position += 4;
    return true;
  }
  if (json[*position] == L'[') {
    ++*position;
    SkipJsonWhitespace(json, position);
    if (*position < json.size() && json[*position] == L']') {
      ++*position;
      return true;
    }
    for (;;) {
      if (!ParseJsonValue(json, position, depth + 1)) return false;
      SkipJsonWhitespace(json, position);
      if (*position >= json.size()) return false;
      if (json[(*position)++] == L']') return true;
      if (json[*position - 1] != L',') return false;
    }
  }
  if (json[*position] == L'{') {
    ++*position;
    SkipJsonWhitespace(json, position);
    if (*position < json.size() && json[*position] == L'}') {
      ++*position;
      return true;
    }
    for (;;) {
      SkipJsonWhitespace(json, position);
      if (!ParseJsonString(json, position)) return false;
      SkipJsonWhitespace(json, position);
      if (*position >= json.size() || json[(*position)++] != L':') return false;
      if (!ParseJsonValue(json, position, depth + 1)) return false;
      SkipJsonWhitespace(json, position);
      if (*position >= json.size()) return false;
      if (json[(*position)++] == L'}') return true;
      if (json[*position - 1] != L',') return false;
    }
  }
  return false;
}

bool IsValidJsonDocument(const std::wstring& json) {
  size_t position = 0;
  if (!ParseJsonValue(json, &position, 0)) return false;
  SkipJsonWhitespace(json, &position);
  return position == json.size();
}

bool IsJsonArray(const std::wstring& value) {
  size_t position = 0;
  SkipJsonWhitespace(value, &position);
  if (position >= value.size() || value[position++] != L'[') return false;
  size_t end = value.size();
  while (end > position &&
         (value[end - 1] == L' ' || value[end - 1] == L'\t' ||
          value[end - 1] == L'\r' || value[end - 1] == L'\n')) {
    --end;
  }
  return end > position && value[end - 1] == L']';
}

bool JsonArrayHasElements(const std::wstring& value) {
  size_t position = 0;
  SkipJsonWhitespace(value, &position);
  if (position >= value.size() || value[position++] != L'[') return false;
  SkipJsonWhitespace(value, &position);
  return position < value.size() && value[position] != L']';
}

bool IsDiagnosticsJsonReport(const std::wstring& report) {
  if (!IsValidJsonDocument(report)) return false;
  size_t position = 0;
  SkipJsonWhitespace(report, &position);
  if (position >= report.size() || report[position] != L'{') return false;
  const auto schema = JsonField(report, L"schema_version");
  if (schema.empty() ||
      !std::all_of(schema.begin(), schema.end(), [](wchar_t c) {
        return c >= L'0' && c <= L'9';
      })) {
    return false;
  }
  return IsJsonArray(JsonField(report, L"devices")) &&
         IsJsonArray(JsonField(report, L"partial_failures"));
}

bool DiagnosticsJsonHasPartialFailures(const std::wstring& report) {
  return JsonArrayHasElements(JsonField(report, L"partial_failures"));
}

bool IsDiagnosticsTextReport(const std::wstring& report) {
  constexpr wchar_t header[] = L"XFS for Windows diagnostics (schema ";
  return report.rfind(header, 0) == 0 &&
         report.find(L"\nVersion: ") != std::wstring::npos &&
         report.find(L"\nWinFsp: ") != std::wstring::npos &&
         report.find(L"\nAutomount service: ") != std::wstring::npos &&
         report.find(L"\nDiscovery: ") != std::wstring::npos &&
         report.find(L"\nDevices: ") != std::wstring::npos;
}

bool DiagnosticsTextHasPartialFailures(const std::wstring& report) {
  constexpr wchar_t prefix[] = L"Partial failures: ";
  size_t line_start = 0;
  while (line_start < report.size()) {
    const size_t line_end = report.find(L'\n', line_start);
    const size_t end = line_end == std::wstring::npos ? report.size() : line_end;
    if (report.compare(line_start, std::size(prefix) - 1, prefix) == 0) {
      size_t digit = line_start + sizeof(prefix) / sizeof(prefix[0]) - 1;
      while (digit < end && report[digit] >= L'0' && report[digit] <= L'9') {
        if (report[digit] != L'0') return true;
        ++digit;
      }
    }
    if (line_end == std::wstring::npos) break;
    line_start = line_end + 1;
  }
  return false;
}

void SetDetails(const std::wstring& text) { if (g_state && g_state->details) SetWindowTextW(g_state->details, text.c_str()); }

void SetBusy(HWND window, bool busy) {
  if (!g_state || g_state->closing) return;
  g_state->busy = busy;
  const int ids[] = {kRefresh, kStart, kEnable, kEnableRw, kDisable, kMount,
                     kUnmount, kEject, kOpen, kExport, kExportJson, kMountRw,
                     kElevate};
  for (int id : ids) EnableWindow(GetDlgItem(window, id), busy ? FALSE : TRUE);
  if (g_state->list) EnableWindow(g_state->list, busy ? FALSE : TRUE);
  if (g_state->drive_choice) EnableWindow(g_state->drive_choice, busy ? FALSE : TRUE);
}

void Refresh(HWND window) {
  if (!g_state || g_state->busy) return;
  SetBusy(window, true);
  HANDLE thread = CreateThread(nullptr, 0, [](void* arg) -> DWORD {
    HWND owner = static_cast<HWND>(arg);
    auto* text = new std::wstring(RunCli({L"diagnostics", L"--json", L"--include-identifiers"}));
    if (!PostMessageW(owner, kRefreshDone, 0, reinterpret_cast<LPARAM>(text))) delete text;
    return 0;
  }, window, 0, nullptr);
  if (thread) CloseHandle(thread);
  else SetBusy(window, false);
}

size_t FindJsonObjectEnd(const std::wstring& json, size_t begin) {
  int depth = 0;
  bool in_string = false, escaped = false;
  for (size_t i = begin; i < json.size(); ++i) {
    const wchar_t c = json[i];
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == L'\\') escaped = true;
      else if (c == L'"') in_string = false;
    } else if (c == L'"') in_string = true;
    else if (c == L'{') ++depth;
    else if (c == L'}' && --depth == 0) return i;
  }
  return std::wstring::npos;
}

std::wstring FindDeviceJsonObject(const std::wstring& json,
                                  const std::wstring& uuid) {
  const auto devices = JsonField(json, L"devices");
  if (!IsJsonArray(devices)) return {};
  size_t position = 0;
  SkipJsonWhitespace(devices, &position);
  ++position;
  std::wstring match;
  for (;;) {
    SkipJsonWhitespace(devices, &position);
    if (position >= devices.size() || devices[position] == L']') return match;
    if (devices[position] != L'{') return {};
    const size_t end = FindJsonObjectEnd(devices, position);
    if (end == std::wstring::npos) return {};
    const auto object = devices.substr(position, end - position + 1);
    if (JsonField(object, L"uuid") == uuid) {
      if (!match.empty()) return {};
      match = object;
    }
    position = end + 1;
    SkipJsonWhitespace(devices, &position);
    if (position >= devices.size()) return {};
    if (devices[position] == L',') ++position;
    else if (devices[position] != L']') return {};
  }
}

std::wstring MountAnnouncementDrive(const std::string& output) {
  constexpr char prefix[] = "Mounting ";
  constexpr char read_only[] = " (read-only)";
  constexpr char read_write[] = " (read-write)";
  size_t line_start = 0;
  while (line_start < output.size()) {
    const size_t line_end = output.find('\n', line_start);
    const size_t end = line_end == std::string::npos ? output.size() : line_end;
    if (output.compare(line_start, sizeof(prefix) - 1, prefix) == 0) {
      size_t mode = output.find(read_only, line_start);
      if (mode == std::string::npos || mode >= end) {
        mode = output.find(read_write, line_start);
      }
      if (mode != std::string::npos && mode < end && mode >= line_start + 6) {
        const size_t drive_start = mode - 2;
        if (drive_start >= line_start + sizeof(prefix) - 1 &&
            (output[drive_start] >= 'A' && output[drive_start] <= 'Z' ||
             output[drive_start] >= 'a' && output[drive_start] <= 'z') &&
            output[drive_start + 1] == ':' && drive_start >= 4 &&
            output.compare(drive_start - 4, 4, " at ") == 0) {
          wchar_t drive[] = {static_cast<wchar_t>(output[drive_start]), L':', L'\0'};
          if (drive[0] >= L'a' && drive[0] <= L'z') drive[0] -= L'a' - L'A';
          return drive;
        }
      }
    }
    if (line_end == std::string::npos) break;
    line_start = line_end + 1;
  }
  return {};
}

void Populate(const std::wstring& json) {
  g_state->devices.clear();
  SendMessageW(g_state->list, LB_RESETCONTENT, 0, 0);
  size_t pos = json.find(L"\"devices\":[");
  while (pos != std::wstring::npos && (pos = json.find(L'{', pos)) != std::wstring::npos) {
    const size_t end = FindJsonObjectEnd(json, pos);
    if (end == std::wstring::npos) break;
    const auto object = json.substr(pos, end - pos + 1);
    if (object.find(L"\"label\"") == std::wstring::npos) break;
    DeviceRow row;
    row.label = JsonField(object, L"label");
    row.uuid = JsonField(object, L"uuid");
    row.drive = JsonField(object, L"drive");
    row.automount_mode = JsonField(object, L"automount_mode");
    row.effective_mode = JsonField(object, L"effective_mode");
    row.device_class = JsonField(object, L"device_class");
    row.bus_type = JsonField(object, L"bus_type");
    row.capacity = JsonField(object, L"capacity_bytes");
    row.device_size = JsonField(object, L"device_size_bytes");
    row.serial = JsonField(object, L"serial_number");
    row.stable_id = JsonField(object, L"stable_id");
    row.automount_enabled = object.find(L"\"automount_enabled\":true") != std::wstring::npos;
    row.discovered = object.find(L"\"discovered\":true") != std::wstring::npos;
    row.enrolled = object.find(L"\"enrolled\":true") != std::wstring::npos;
    row.mounted = object.find(L"\"mounted\":true") != std::wstring::npos;
    std::wstring display = row.label.empty() ? L"XFS filesystem" : row.label;
    display += L"  " + row.capacity + L" bytes  " + row.device_class + L"/" + row.bus_type;
    display += L"  " + row.drive + L"  " + row.automount_mode;
    display += row.discovered ? L"  discovered" : L"  offline/not discovered";
    if (row.enrolled) display += L"  enrolled";
    if (row.mounted) display += L"  mounted";
    if (row.automount_enabled) display += L" enabled";
    if (!row.effective_mode.empty()) display += L"  mounted " + row.effective_mode;
    display += L"  " + row.uuid;
    SendMessageW(g_state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
    g_state->devices.push_back(std::move(row));
    pos = end + 1;
    if (pos < json.size() && json[pos] == L']') break;
  }
  SetDetails(json);
}

struct ActionRequest { HWND window; int id; DeviceRow device; std::wstring output_path; std::wstring drive_selection = L"auto"; bool json = false; };

std::wstring SavePath(HWND window, bool json) {
  std::wstring path(32768, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = L"Text diagnostics (*.txt)\0*.txt\0JSON diagnostics (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
  dialog.nFilterIndex = json ? 2 : 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = json ? L"json" : L"txt";
  dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
  if (!GetSaveFileNameW(&dialog)) return {};
  path.resize(wcslen(path.c_str()));
  return path;
}

bool WriteUtf8(const std::wstring& path, const std::wstring& text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return false;
  std::string utf8(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
  std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
  file.close();
  return static_cast<bool>(file);
}

void Action(HWND window, int id) {
  if (!g_state || g_state->busy) return;
  const LRESULT selected = SendMessageW(g_state->list, LB_GETCURSEL, 0, 0);
  const bool needs_device = id == kEnable || id == kEnableRw || id == kEject || id == kDisable || id == kMount || id == kMountRw || id == kUnmount || id == kOpen;
  if (needs_device && (selected == LB_ERR || static_cast<size_t>(selected) >= g_state->devices.size())) {
    SetDetails(L"Select one discovered XFS filesystem first.");
    return;
  }
  DeviceRow device;
  if (needs_device) device = g_state->devices[static_cast<size_t>(selected)];
  if (id == kMountRw &&
      (!device.discovered ||
       (device.device_class != L"virtual-disk" && device.serial.empty()) ||
       device.stable_id.empty() || device.device_size.empty())) {
    SetDetails(L"Read-write mount requires a discovered device with stable ID and device size; physical devices also need a serial number.");
    return;
  }
  if (id == kEnableRw || id == kMountRw) {
    std::wstring consent = L"Read-write access can modify this filesystem. The CLI will recheck its full policy and identity.\n\nLabel: " + device.label + L"\nUUID: " + device.uuid + L"\nDevice: " + device.device_class + L" / " + device.bus_type + L"\nSerial: " + (device.serial.empty() ? L"unavailable" : device.serial) + L"\nStable ID: " + (device.stable_id.empty() ? L"unavailable" : device.stable_id) + L"\nDevice size: " + (device.device_size.empty() ? L"unknown" : device.device_size + L" bytes") + L"\n\nProceed?";
    if (MessageBoxW(window, consent.c_str(), L"Confirm read-write operation", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
  }
  std::wstring output_path;
  if (id == kExport || id == kExportJson) {
    output_path = SavePath(window, id == kExportJson);
    if (output_path.empty()) return;
  }
  if ((id == kOpen || id == kUnmount) && (!device.mounted || device.drive.empty())) {
    SetDetails(L"This filesystem has no active drive letter. Refresh to check its mount state.");
    return;
  }
  if (id == kUnmount && device.automount_enabled) {
    SetDetails(L"Automatic mounting is enabled for this filesystem and may remount it after unmount. Use Eject for a safe release, or disable enrollment and refresh first. Filesystem unmount is not device eject.");
    return;
  }
  if (id == kOpen) {
    ShellExecuteW(window, L"open", (device.drive + L"\\").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  std::wstring drive_selection = L"auto";
  if (id == kMount || id == kMountRw) {
    const LRESULT choice = SendMessageW(g_state->drive_choice, CB_GETCURSEL, 0, 0);
    if (choice != CB_ERR) {
      wchar_t selected_text[16]{};
      SendMessageW(g_state->drive_choice, CB_GETLBTEXT, static_cast<WPARAM>(choice),
                   reinterpret_cast<LPARAM>(selected_text));
      drive_selection = wcscmp(selected_text, L"Auto") == 0 ? L"auto" : selected_text;
    }
  }
  auto* request = new ActionRequest{window, id, std::move(device), std::move(output_path), std::move(drive_selection)};
  SetBusy(window, true);
  HANDLE thread = CreateThread(nullptr, 0, [](void* raw) -> DWORD {
    std::unique_ptr<ActionRequest> request(static_cast<ActionRequest*>(raw));
    DWORD code = 0;
    std::wstring output;
    const auto& d = request->device;
    if (request->id == kStart) output = RunCli({L"automount", L"start"}, &code);
    else if (request->id == kEnable) output = RunCli({L"automount", L"enable", d.uuid}, &code);
    else if (request->id == kEnableRw) output = RunCli({L"automount", L"enable", d.uuid, L"--rw", L"--yes-i-really-mean-it"}, &code);
    else if (request->id == kEject) output = RunCli({L"automount", L"eject", d.uuid}, &code);
    else if (request->id == kDisable) output = RunCli({L"automount", L"disable", d.uuid}, &code);
    else if (request->id == kUnmount) output = RunCli({L"unmount", d.drive}, &code);
    else if (request->id == kExport || request->id == kExportJson) {
      const bool json = request->id == kExportJson;
      output = RunCli(json ? std::vector<std::wstring>{L"diagnostics", L"--json"}
                           : std::vector<std::wstring>{L"diagnostics"}, &code);
      const bool report_available = json ? IsDiagnosticsJsonReport(output)
                                         : IsDiagnosticsTextReport(output);
      const bool partial_report = code != 0 ||
          (json ? DiagnosticsJsonHasPartialFailures(output)
                : DiagnosticsTextHasPartialFailures(output));
      if (report_available && WriteUtf8(request->output_path, output))
        output = (partial_report ? L"Partial redacted diagnostics saved to "
                                 : L"Redacted diagnostics saved to ") + request->output_path;
      else output = L"Diagnostics could not be exported. Check the selected path and permissions.";
    }
    else if (request->id == kMount || request->id == kMountRw) {
      const auto exe = CliPath();
      const bool writable = request->id == kMountRw;
      std::wstring command = Quote(exe) + L" " + Quote(L"mount") + L" " + Quote(d.uuid) + L" " + Quote(L"--drive") + L" " + Quote(request->drive_selection);
      if (writable) {
        command += L" " + Quote(L"--rw") + L" " + Quote(L"--device-write-qualification");
        if (!d.serial.empty()) command += L" " + Quote(L"--expect-serial=" + d.serial);
        command += L" " + Quote(L"--expect-stable-id=" + d.stable_id);
        command += L" " + Quote(L"--expect-size=" + d.device_size);
        if (d.device_class == L"removable-disk") command += L" " + Quote(L"--allow-removable-disk-write");
        if (d.device_class == L"fixed-disk") command += L" " + Quote(L"--allow-fixed-disk-write");
      }
      SECURITY_ATTRIBUTES pipe_sa{sizeof(pipe_sa), nullptr, TRUE};
      HANDLE output_read = nullptr, output_write = nullptr;
      if (!CreatePipe(&output_read, &output_write, &pipe_sa, 0)) {
        output = L"Cannot capture mount output.";
      } else {
        SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
        HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &pipe_sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        startup.wShowWindow = SW_HIDE; startup.hStdOutput = output_write;
        startup.hStdError = output_write; startup.hStdInput = null_input;
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(output_write);
        if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
        if (!started) {
          CloseHandle(output_read);
          output = GetLastError() == ERROR_ACCESS_DENIED ? L"Access denied. Reopen with the required Windows permissions." : L"Could not start the mount process; check WinFsp and drive-letter availability.";
        } else {
          CloseHandle(process.hThread);
          std::string mount_output;
          char buffer[2048]; DWORD count = 0, available = 0;
          output = L"Mount startup is still pending; the mount process will be left running. Refresh after this operation and use Unmount if it appears.";
          bool mounted = false, process_exited = false;
          const ULONGLONG mount_deadline = GetTickCount64() + 30000;
          for (int attempt = 0; attempt < 30 && GetTickCount64() < mount_deadline; ++attempt) {
            if (PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr) && available) {
              const DWORD want = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
              if (ReadFile(output_read, buffer, want, &count, nullptr) && count) mount_output.append(buffer, count);
            }
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) { process_exited = true; break; }
            const ULONGLONG remaining = mount_deadline - GetTickCount64();
            if (remaining == 0) break;
            const DWORD probe_timeout = static_cast<DWORD>((std::min)(remaining, ULONGLONG{5000}));
            const std::wstring status = RunCli({L"diagnostics", L"--json", L"--include-identifiers"}, nullptr, probe_timeout);
            if (IsDiagnosticsJsonReport(status)) {
              const auto device_status = FindDeviceJsonObject(status, d.uuid);
              if (!device_status.empty()) {
                const std::wstring drive = JsonField(device_status, L"drive");
                const std::wstring announced_drive = MountAnnouncementDrive(mount_output);
                if (!drive.empty() && !announced_drive.empty() &&
                    JsonField(device_status, L"mounted") == L"true" &&
                    CompareStringOrdinal(drive.c_str(), -1, announced_drive.c_str(), -1, TRUE) == CSTR_EQUAL) {
                  output = L"Mount is ready at " + drive + L"\\. The mount process continues independently.";
                  mounted = true;
                  break;
                }
              }
            }
            Sleep(1000);
          }
          if (!mounted && !process_exited && WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            process_exited = true;
          while (PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr) && available) {
            const DWORD want = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
            if (!ReadFile(output_read, buffer, want, &count, nullptr) || !count) break;
            mount_output.append(buffer, count);
          }
          if (!mounted && !mount_output.empty()) {
            const int chars = MultiByteToWideChar(CP_UTF8, 0, mount_output.data(), static_cast<int>(mount_output.size()), nullptr, 0);
            std::wstring captured(chars > 0 ? chars : 0, L'\0');
            if (chars) MultiByteToWideChar(CP_UTF8, 0, mount_output.data(), static_cast<int>(mount_output.size()), captured.data(), chars);
            output = process_exited ? L"Mount process exited before readiness. CLI output: " + captured
                                    : L"Mount remains pending and its process was left running. Refresh to check its state. CLI output: " + captured;
          } else if (process_exited && !mounted) {
            output = L"Mount process exited before the filesystem became ready.";
          }
          CloseHandle(output_read);
          CloseHandle(process.hProcess);
        }
      }
    }
    auto* result = new std::wstring(std::move(output));
    if (!PostMessageW(request->window, kRefreshDone + 1, 0, reinterpret_cast<LPARAM>(result))) delete result;
    return 0;
  }, request, 0, nullptr);
  if (thread) CloseHandle(thread);
  else { delete request; SetBusy(window, false); SetDetails(L"Could not start a background operation."); }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CREATE) {
    g_state = new State;
    g_state->list = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
                                  12, 12, 700, 190, window, reinterpret_cast<HMENU>(kList), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Mount drive", WS_CHILD | WS_VISIBLE,
                  720, 12, 70, 18, window, nullptr, nullptr, nullptr);
    g_state->drive_choice = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
        720, 32, 64, 260, window, reinterpret_cast<HMENU>(kDriveChoice), nullptr, nullptr);
    SendMessageW(g_state->drive_choice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
    for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
      wchar_t choice[] = {letter, L':', L'\0'};
      SendMessageW(g_state->drive_choice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
    }
    SendMessageW(g_state->drive_choice, CB_SETCURSEL, 0, 0);
    g_state->details = CreateWindowW(L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                     12, 210, 700, 240, window, reinterpret_cast<HMENU>(kDetails), nullptr, nullptr);
    const struct { int id; const wchar_t* text; int x; int y; int width; } buttons[] = {
      {kRefresh,L"Refresh",12,460,76},{kStart,L"Start service",94,460,100},
      {kEnable,L"Enroll RO",200,460,82},{kEnableRw,L"Enroll RW",288,460,82},
      {kDisable,L"Disable",374,460,76},{kMount,L"Mount RO",454,460,84},
      {kUnmount,L"Unmount",542,460,82},{kOpen,L"Open",628,460,64},
      {kEject,L"Eject",12,494,76},{kExport,L"Export text",94,494,94},
      {kExportJson,L"Export JSON",194,494,100},{kElevate,L"Run elevated",300,494,110},
      {kMountRw,L"Mount RW",416,494,90}};
    for (const auto& b : buttons) CreateWindowW(L"BUTTON", b.text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                b.x, b.y, b.width, 28, window, reinterpret_cast<HMENU>(b.id), nullptr, nullptr);
    Refresh(window);
    return 0;
  }
  if (message == WM_COMMAND) {
    const int id = LOWORD(wparam);
    if (HIWORD(wparam) != BN_CLICKED || !lparam ||
        reinterpret_cast<HWND>(lparam) != GetDlgItem(window, id)) {
      return 0;
    }
    if (id == kRefresh) Refresh(window);
    else if (id == kElevate) {
      const auto exe = CliPath();
      const size_t slash = exe.find_last_of(L"\\/");
      std::wstring gui = slash == std::wstring::npos ? L"xfswin-gui.exe" : exe.substr(0, slash + 1) + L"xfswin-gui.exe";
      SHELLEXECUTEINFOW info{};
      info.cbSize = sizeof(info);
      info.lpVerb = L"runas"; info.lpFile = gui.c_str(); info.nShow = SW_SHOWNORMAL;
      if (!ShellExecuteExW(&info)) SetDetails(L"Elevation was cancelled or failed. You can still use read-only scans.");
    } else if (id == kStart || id == kEnable || id == kEnableRw ||
               id == kEject || id == kDisable || id == kMount ||
               id == kMountRw || id == kUnmount || id == kOpen ||
               id == kExport || id == kExportJson) {
      Action(window, id);
    }
    return 0;
  }
  if (message == kRefreshDone) {
    std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lparam));
    if (g_state && !g_state->closing) Populate(*result);
    SetBusy(window, false);
    return 0;
  }
  if (message == kRefreshDone + 1) {
    std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lparam));
    if (g_state && !g_state->closing) SetDetails(*result);
    SetBusy(window, false);
    return 0;
  }
  if (message == WM_CLOSE) {
    if (MessageBoxW(window, L"Closing this window does not stop the automount service or mounted filesystems.",
                    L"XFS for Windows", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) DestroyWindow(window);
    return 0;
  }
  if (message == WM_DESTROY) {
    if (g_state) g_state->closing = true;
    MSG pending{};
    while (PeekMessageW(&pending, window, kRefreshDone, kRefreshDone + 1, PM_REMOVE))
      delete reinterpret_cast<std::wstring*>(pending.lParam);
    delete g_state; g_state = nullptr; PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassW(&wc)) return 1;
  HWND window = CreateWindowW(kWindowClass, L"XFS for Windows", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 800, 580, nullptr, nullptr, instance, nullptr);
  if (!window) return 1;
  ShowWindow(window, show);
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  return static_cast<int>(message.wParam);
}
