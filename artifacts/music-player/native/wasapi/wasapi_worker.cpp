// Splayer Windows WASAPI exclusive-mode raw PCM worker.
// Intentionally does not decode, resample, dither, apply EQ, or change sample bits.
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <avrt.h>
#include <ksmedia.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "avrt.lib")

namespace {
std::mutex g_outputMutex;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_pause{false};
std::atomic<double> g_seek{-1.0};
HANDLE g_controlEvent = nullptr;

void Emit(const std::string& json) {
  std::lock_guard<std::mutex> lock(g_outputMutex);
  std::cout << json << std::endl;
}

std::string JsonEscape(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) out += " ";
        else out += static_cast<char>(c);
    }
  }
  out += "\"";
  return out;
}

std::string Utf8(const wchar_t* value) {
  if (!value || !*value) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
  out.resize(static_cast<size_t>(size - 1));
  return out;
}

std::string HResultText(HRESULT hr) {
  char buffer[32]{};
  sprintf_s(buffer, "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

struct ComScope {
  HRESULT result;
  ComScope() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

template <typename T> void SafeRelease(T*& value) {
  if (value) { value->Release(); value = nullptr; }
}

struct WavInfo {
  WAVEFORMATEXTENSIBLE extensible{};
  DWORD dataOffset = 0;
  DWORD dataBytes = 0;
  DWORD blockAlign = 0;
  DWORD sampleRate = 0;
  WORD bits = 0;
  WORD channels = 0;
  bool isFloat = false;
};

bool ReadWav(const std::wstring& path, WavInfo& info, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) { error = "无法打开 WAV 文件"; return false; }

  char riff[4]{}, wave[4]{};
  uint32_t riffSize = 0;
  file.read(riff, 4);
  file.read(reinterpret_cast<char*>(&riffSize), 4);
  file.read(wave, 4);
  if (!file || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
    error = "仅支持标准 RIFF/WAVE 文件（RF64 暂不支持）";
    return false;
  }

  std::vector<uint8_t> fmt;
  bool gotData = false;
  while (file && (!fmt.size() || !gotData)) {
    char chunkId[4]{};
    uint32_t chunkSize = 0;
    file.read(chunkId, 4);
    if (!file) break;
    file.read(reinterpret_cast<char*>(&chunkSize), 4);
    if (!file) break;
    const auto payload = static_cast<uint64_t>(file.tellg());

    if (std::memcmp(chunkId, "fmt ", 4) == 0) {
      if (chunkSize < 16 || chunkSize > 4096) { error = "WAV fmt 块大小无效"; return false; }
      fmt.resize(chunkSize);
      file.read(reinterpret_cast<char*>(fmt.data()), chunkSize);
      if (!file) { error = "WAV fmt 块已截断"; return false; }
    } else if (std::memcmp(chunkId, "data", 4) == 0) {
      if (payload > 0xFFFFFFFFull) { error = "WAV 数据偏移超出支持范围"; return false; }
      info.dataOffset = static_cast<DWORD>(payload);
      info.dataBytes = chunkSize;
      gotData = true;
      file.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
    } else {
      file.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
    }
    if (chunkSize & 1) file.seekg(1, std::ios::cur);
  }

  if (fmt.size() < 16 || !gotData) { error = "WAV 缺少 fmt 或 data 数据块"; return false; }

  WORD formatTag = 0;
  std::memcpy(&formatTag, fmt.data(), 2);
  std::memcpy(&info.channels, fmt.data() + 2, 2);
  std::memcpy(&info.sampleRate, fmt.data() + 4, 4);
  std::memcpy(&info.blockAlign, fmt.data() + 12, 2);
  std::memcpy(&info.bits, fmt.data() + 14, 2);

  if (formatTag == WAVE_FORMAT_EXTENSIBLE) {
    if (fmt.size() < sizeof(WAVEFORMATEXTENSIBLE)) {
      error = "WAV WAVE_FORMAT_EXTENSIBLE 格式头不完整";
      return false;
    }
    std::memcpy(&info.extensible, fmt.data(), sizeof(WAVEFORMATEXTENSIBLE));
    const GUID& subtype = info.extensible.SubFormat;
    if (subtype == KSDATAFORMAT_SUBTYPE_PCM) info.isFloat = false;
    else if (subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) info.isFloat = true;
    else { error = "WAV 编码不是 PCM / IEEE Float"; return false; }
  } else if (formatTag == WAVE_FORMAT_PCM) {
    info.extensible.Format.wFormatTag = WAVE_FORMAT_PCM;
    info.extensible.Format.nChannels = info.channels;
    info.extensible.Format.nSamplesPerSec = info.sampleRate;
    info.extensible.Format.nAvgBytesPerSec = info.sampleRate * info.blockAlign;
    info.extensible.Format.nBlockAlign = static_cast<WORD>(info.blockAlign);
    info.extensible.Format.wBitsPerSample = info.bits;
    info.extensible.Format.cbSize = 0;
    info.isFloat = false;
  } else if (formatTag == WAVE_FORMAT_IEEE_FLOAT) {
    info.extensible.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    info.extensible.Format.nChannels = info.channels;
    info.extensible.Format.nSamplesPerSec = info.sampleRate;
    info.extensible.Format.nAvgBytesPerSec = info.sampleRate * info.blockAlign;
    info.extensible.Format.nBlockAlign = static_cast<WORD>(info.blockAlign);
    info.extensible.Format.wBitsPerSample = info.bits;
    info.extensible.Format.cbSize = 0;
    info.isFloat = true;
  } else {
    error = "WAV 不是未压缩 PCM；此格式将回退到共享播放";
    return false;
  }

  if (!info.channels || !info.sampleRate || !info.blockAlign ||
      (info.bits != 8 && info.bits != 16 && info.bits != 24 && info.bits != 32 && !(info.isFloat && info.bits == 64))) {
    error = "WAV 声道数、采样率或位深不受支持";
    return false;
  }
  if (info.isFloat && info.bits != 32 && info.bits != 64) {
    error = "IEEE Float WAV 只支持 32/64 位";
    return false;
  }
  if (info.dataBytes == 0 || info.dataBytes % info.blockAlign != 0) {
    error = "WAV PCM 数据长度与帧对齐不正确";
    return false;
  }
  return true;
}

WAVEFORMATEX* ExactFormat(WavInfo& info) {
  if (info.extensible.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    return &info.extensible.Format;
  }
  return &info.extensible.Format;
}

bool SupportsExclusive(IAudioClient* client, WAVEFORMATEX* format) {
  return client && client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format, nullptr) == S_OK;
}

std::string FormatLabel(UINT32 rate, WORD bits, WORD channels, bool isFloat) {
  return std::to_string(rate) + " Hz / " + std::to_string(bits) +
      (isFloat ? "-bit float" : "-bit PCM") + " / " + std::to_string(channels) + " ch";
}

void EmitFallback(const std::string& reason) {
  Emit("{\"type\":\"fallback\",\"reason\":" + JsonEscape(reason) + "}");
}

bool ProbeDevices() {
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDeviceCollection* devices = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr)) {
    Emit("{\"available\":false,\"reason\":" + JsonEscape("无法创建 Windows 音频设备枚举器 " + HResultText(hr)) + "}");
    return false;
  }
  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
  if (FAILED(hr)) {
    SafeRelease(enumerator);
    Emit("{\"available\":false,\"reason\":\"无法枚举音频输出设备\"}");
    return false;
  }

  std::string json = "{\"available\":true,\"devices\":[";
  UINT count = 0;
  devices->GetCount(&count);
  bool first = true;
  const UINT32 rates[] = {32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 352800, 384000};
  const WORD bits[] = {8, 16, 24, 32};
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IPropertyStore* props = nullptr;
    LPWSTR id = nullptr;
    PROPVARIANT name;
    PropVariantInit(&name);
    if (FAILED(devices->Item(i, &device))) continue;
    device->GetId(&id);
    std::wstring friendly = L"Audio output";
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
        SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
      friendly = name.pwszVal;
    }
    std::string label = Utf8(friendly.c_str());
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
      WAVEFORMATEX* mix = nullptr;
      client->GetMixFormat(&mix);
      std::vector<std::string> supported;
      for (UINT32 rate : rates) {
        for (WORD depth : bits) {
          for (WORD channels : {static_cast<WORD>(1), static_cast<WORD>(2)}) {
            WAVEFORMATEX candidate{};
            candidate.wFormatTag = WAVE_FORMAT_PCM;
            candidate.nChannels = channels;
            candidate.nSamplesPerSec = rate;
            candidate.wBitsPerSample = depth;
            candidate.nBlockAlign = static_cast<WORD>(channels * depth / 8);
            candidate.nAvgBytesPerSec = rate * candidate.nBlockAlign;
            if (SupportsExclusive(client, &candidate)) {
              supported.push_back(FormatLabel(rate, depth, channels, false));
            }

            // Probe IEEE Float separately; the source float payload can be passed
            // unchanged when the endpoint accepts this exact sample format.
            if (depth == 32 || depth == 64) {
              WAVEFORMATEX floating{};
              floating.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
              floating.nChannels = channels;
              floating.nSamplesPerSec = rate;
              floating.wBitsPerSample = depth;
              floating.nBlockAlign = static_cast<WORD>(channels * depth / 8);
              floating.nAvgBytesPerSec = rate * floating.nBlockAlign;
              if (SupportsExclusive(client, &floating)) {
                supported.push_back(FormatLabel(rate, depth, channels, true));
              }
            }
          }
        }
      }
      if (!first) json += ",";
      first = false;
      bool mixFloat = false;
      if (mix) {
        mixFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
          (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
      }
      json += "{\"name\":" + JsonEscape(label) + ",\"id\":" + JsonEscape(Utf8(id)) +
          ",\"mixFormat\":" + JsonEscape(mix ? FormatLabel(mix->nSamplesPerSec, mix->wBitsPerSample, mix->nChannels,
            mixFloat) : "unknown") + ",\"exclusiveFormats\":[";
      for (size_t n = 0; n < supported.size(); ++n) {
        if (n) json += ",";
        json += JsonEscape(supported[n]);
      }
      json += "]}";
      CoTaskMemFree(mix);
    }
    PropVariantClear(&name);
    CoTaskMemFree(id);
    SafeRelease(props);
    SafeRelease(client);
    SafeRelease(device);
  }
  json += "]}";
  Emit(json);
  SafeRelease(devices);
  SafeRelease(enumerator);
  return true;
}

void InputThread() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "pause") g_pause.store(true);
    else if (line == "resume") g_pause.store(false);
    else if (line == "stop") g_stop.store(true);
    else if (line.rfind("seek:", 0) == 0) {
      try { g_seek.store(std::max(0.0, std::stod(line.substr(5)))); } catch (...) {}
    }
    if (g_controlEvent) SetEvent(g_controlEvent);
    if (g_stop.load()) break;
  }
}

int Play(const std::wstring& path) {
  WavInfo wav;
  std::string error;
  if (!ReadWav(path, wav, error)) { EmitFallback(error); return 2; }

  ComScope com;
  if (FAILED(com.result)) { EmitFallback("无法初始化 COM 音频线程：" + HResultText(com.result)); return 2; }

  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  HANDLE audioEvent = nullptr;
  HANDLE mmcss = nullptr;

  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
  if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  WAVEFORMATEX* format = ExactFormat(wav);
  if (SUCCEEDED(hr) && !SupportsExclusive(client, format)) {
    EmitFallback("当前默认输出设备不支持 WAV 原采样率、声道数和位深的 WASAPI 独占格式；已回退共享播放");
    SafeRelease(client); SafeRelease(device); SafeRelease(enumerator);
    return 2;
  }
  if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 200000, format, nullptr);
  if (SUCCEEDED(hr)) {
    audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent) hr = HRESULT_FROM_WIN32(GetLastError());
  }
  if (SUCCEEDED(hr)) hr = client->SetEventHandle(audioEvent);
  UINT32 bufferFrames = 0;
  if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
  if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
  if (FAILED(hr)) {
    EmitFallback("WASAPI 独占模式打开失败 " + HResultText(hr) + "；已回退共享播放");
    if (audioEvent) CloseHandle(audioEvent);
    SafeRelease(render); SafeRelease(client); SafeRelease(device); SafeRelease(enumerator);
    return 2;
  }

  std::ifstream file(path, std::ios::binary);
  file.seekg(wav.dataOffset);
  const uint64_t totalFrames = wav.dataBytes / wav.blockAlign;
  uint64_t positionFrames = 0;
  g_stop.store(false); g_pause.store(false); g_seek.store(-1.0);
  g_controlEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g_controlEvent) {
    EmitFallback("无法创建 WASAPI 控制事件");
    if (audioEvent) CloseHandle(audioEvent);
    SafeRelease(render); SafeRelease(client); SafeRelease(device); SafeRelease(enumerator);
    return 2;
  }
  DWORD taskIndex = 0;
  mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  hr = client->Start();
  if (FAILED(hr)) {
    EmitFallback("WASAPI 启动失败 " + HResultText(hr));
  } else {
    std::thread input(InputThread);
    Emit("{\"type\":\"ready\",\"exclusive\":true,\"format\":" +
        JsonEscape(FormatLabel(wav.sampleRate, wav.bits, wav.channels, wav.isFloat)) +
        ",\"duration\":" + std::to_string(static_cast<double>(totalFrames) / wav.sampleRate) + "}");
    HANDLE waits[] = {audioEvent, g_controlEvent};
    auto lastProgress = std::chrono::steady_clock::now();
    bool paused = false;
    while (!g_stop.load()) {
      DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
      if (wait == WAIT_OBJECT_0 + 1) {
        const double seek = g_seek.exchange(-1.0);
        if (seek >= 0.0) {
          client->Stop(); client->Reset();
          positionFrames = std::min<uint64_t>(totalFrames,
              static_cast<uint64_t>(seek * wav.sampleRate));
          file.clear();
          file.seekg(static_cast<std::streamoff>(wav.dataOffset + positionFrames * wav.blockAlign));
          if (!paused) client->Start();
          Emit("{\"type\":\"progress\",\"position\":" +
              std::to_string(static_cast<double>(positionFrames) / wav.sampleRate) + "}");
        }
        const bool shouldPause = g_pause.load();
        if (shouldPause != paused) {
          paused = shouldPause;
          if (paused) client->Stop(); else client->Start();
          Emit(paused ? "{\"type\":\"paused\"}" : "{\"type\":\"resumed\"}");
        }
        continue;
      }
      if (wait != WAIT_OBJECT_0) continue;
      if (paused || g_stop.load()) continue;

      // Do not signal natural end until the last submitted PCM buffer has
      // actually drained to the endpoint. Stopping immediately after queueing
      // the final block can truncate its tail.
      if (positionFrames >= totalFrames) {
        UINT32 paddingFrames = 0;
        hr = client->GetCurrentPadding(&paddingFrames);
        if (SUCCEEDED(hr) && paddingFrames == 0) {
          Emit("{\"type\":\"ended\"}");
          g_stop.store(true);
          break;
        }
        continue;
      }

      BYTE* buffer = nullptr;
      const UINT32 frames = bufferFrames;
      hr = render->GetBuffer(frames, &buffer);
      if (FAILED(hr)) { EmitFallback("WASAPI 获取输出缓冲区失败 " + HResultText(hr)); break; }

      const uint64_t remaining = totalFrames - std::min(positionFrames, totalFrames);
      const UINT32 copyFrames = static_cast<UINT32>(std::min<uint64_t>(remaining, frames));
      const UINT32 copyBytes = copyFrames * wav.blockAlign;
      if (copyBytes) {
        file.read(reinterpret_cast<char*>(buffer), copyBytes);
        const UINT32 actuallyRead = static_cast<UINT32>(file.gcount());
        if (actuallyRead < copyBytes) {
          std::memset(buffer + actuallyRead, 0, frames * wav.blockAlign - actuallyRead);
          render->ReleaseBuffer(frames, 0);
          EmitFallback("WAV 文件读取中断，已停止独占输出");
          break;
        }
      }
      if (copyFrames < frames) {
        std::memset(buffer + copyBytes, 0, (frames - copyFrames) * wav.blockAlign);
      }
      hr = render->ReleaseBuffer(frames, 0);
      if (FAILED(hr)) { EmitFallback("WASAPI 提交 PCM 缓冲区失败 " + HResultText(hr)); break; }
      positionFrames += copyFrames;

      const auto now = std::chrono::steady_clock::now();
      if (now - lastProgress >= std::chrono::milliseconds(100)) {
        Emit("{\"type\":\"progress\",\"position\":" +
            std::to_string(static_cast<double>(positionFrames) / wav.sampleRate) + "}");
        lastProgress = now;
      }
      // End notification is emitted after WASAPI reports that padding drained.
    }
    client->Stop();
    g_stop.store(true);
    if (g_controlEvent) SetEvent(g_controlEvent);
    // stdin remains open in Electron while playback runs. Do not block audio teardown
    // waiting for EOF; the child process is exiting immediately after this function.
    if (input.joinable()) input.detach();
  }

  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  // Keep the control event alive until process exit so the detached stdin reader
  // cannot signal a closed/recycled HANDLE during shutdown.
  if (audioEvent) CloseHandle(audioEvent);
  SafeRelease(render); SafeRelease(client); SafeRelease(device); SafeRelease(enumerator);
  return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 2 && std::wstring(argv[1]) == L"--probe") {
    ComScope com;
    if (FAILED(com.result)) {
      Emit("{\"available\":false,\"reason\":\"COM 初始化失败\"}");
      return 2;
    }
    return ProbeDevices() ? 0 : 2;
  }
  if (argc >= 3 && std::wstring(argv[1]) == L"--play") {
    return Play(argv[2]);
  }
  Emit("{\"available\":false,\"reason\":\"参数无效\"}");
  return 2;
}
