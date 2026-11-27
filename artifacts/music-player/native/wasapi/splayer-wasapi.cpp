#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

static const GUID SUBTYPE_PCM_LOCAL = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID SUBTYPE_FLOAT_LOCAL = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

static std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), n, nullptr, nullptr);
    return out;
}
static std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), n);
    return out;
}
static std::string jsonEscape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default: if (c < 0x20) { char b[7]; sprintf_s(b, "\\u%04x", c); o << b; } else o << (char)c;
        }
    }
    return o.str();
}
static void emit(const std::string& json) {
    std::cout << json << std::endl;
    std::cout.flush();
}
static std::string hrString(HRESULT hr) {
    char b[16]; sprintf_s(b, "0x%08lx", (unsigned long)hr); return b;
}

struct WaveInfo {
    std::wstring path;
    std::ifstream file;
    WAVEFORMATEXTENSIBLE ext{};
    WAVEFORMATEX* format() { return &ext.Format; }
    uint64_t dataOffset = 0;
    uint64_t dataBytes = 0;
    uint64_t totalFrames = 0;
    bool isFloat = false;
    uint16_t sourceTag = 0;
    std::string error;
};

static bool readWave(const std::wstring& path, WaveInfo& w) {
    w.path = path;
    w.file.open(path, std::ios::binary);
    if (!w.file) { w.error = "无法打开 WAV 文件"; return false; }
    char riff[4]{}; uint32_t riffSize = 0; char wave[4]{};
    w.file.read(riff, 4); w.file.read((char*)&riffSize, 4); w.file.read(wave, 4);
    if (!w.file || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        w.error = "仅支持标准 RIFF/WAVE 文件"; return false;
    }
    bool haveFmt = false, haveData = false;
    while (w.file && (!haveFmt || !haveData)) {
        char id[4]{}; uint32_t size = 0;
        w.file.read(id, 4); w.file.read((char*)&size, 4);
        if (!w.file) break;
        const auto payload = (uint64_t)w.file.tellg();
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || size > 4096) { w.error = "WAV fmt 块长度无效"; return false; }
            std::vector<uint8_t> b(size);
            w.file.read((char*)b.data(), size);
            if (!w.file) { w.error = "WAV fmt 块不完整"; return false; }
            WAVEFORMATEX base{};
            std::memcpy(&base, b.data(), 16);
            w.sourceTag = base.wFormatTag;
            if (base.wFormatTag == WAVE_FORMAT_EXTENSIBLE && size >= 40) {
                std::memcpy(&w.ext, b.data(), sizeof(WAVEFORMATEXTENSIBLE));
                w.isFloat = IsEqualGUID(w.ext.SubFormat, SUBTYPE_FLOAT_LOCAL);
                if (!w.isFloat && !IsEqualGUID(w.ext.SubFormat, SUBTYPE_PCM_LOCAL)) {
                    w.error = "WAV 编码不是 PCM/IEEE Float"; return false;
                }
                if (w.ext.Samples.wValidBitsPerSample == 0)
                    w.ext.Samples.wValidBitsPerSample = base.wBitsPerSample;
            } else if (base.wFormatTag == WAVE_FORMAT_PCM || base.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                w.ext = {};
                w.ext.Format = base;
                w.ext.Format.cbSize = 0;
                w.isFloat = base.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
                w.ext.Samples.wValidBitsPerSample = base.wBitsPerSample;
                if (base.nChannels > 2) {
                    WAVEFORMATEX old = w.ext.Format;
                    w.ext = {};
                    w.ext.Format = old;
                    w.ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                    w.ext.Format.cbSize = 22;
                    w.ext.Samples.wValidBitsPerSample = old.wBitsPerSample;
                    w.ext.dwChannelMask = 0;
                    w.ext.SubFormat = w.isFloat ? SUBTYPE_FLOAT_LOCAL : SUBTYPE_PCM_LOCAL;
                }
            } else {
                w.error = "WAV 压缩格式不支持 WASAPI 原始直通"; return false;
            }
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            w.dataOffset = payload;
            w.dataBytes = size;
            haveData = true;
            w.file.seekg((std::streamoff)(payload + size + (size & 1)), std::ios::beg);
        } else {
            w.file.seekg((std::streamoff)(payload + size + (size & 1)), std::ios::beg);
        }
    }
    auto* f = w.format();
    if (!haveFmt || !haveData || f->nChannels == 0 || f->nSamplesPerSec == 0 ||
        f->nBlockAlign == 0 || f->wBitsPerSample == 0) {
        w.error = "WAV 缺少有效 fmt/data 块"; return false;
    }
    const uint16_t valid = w.sourceTag == WAVE_FORMAT_EXTENSIBLE
        ? w.ext.Samples.wValidBitsPerSample : f->wBitsPerSample;
    if (valid != f->wBitsPerSample ||
        (f->wBitsPerSample != 8 && f->wBitsPerSample != 16 &&
         f->wBitsPerSample != 24 && f->wBitsPerSample != 32 &&
         !(w.isFloat && f->wBitsPerSample == 64))) {
        w.error = "WAV 位深不是受支持的原生 PCM 容器格式"; return false;
    }
    if (w.isFloat && f->wBitsPerSample != 32 && f->wBitsPerSample != 64) {
        w.error = "IEEE Float WAV 仅支持 32/64 位"; return false;
    }
    w.totalFrames = w.dataBytes / f->nBlockAlign;
    if (w.totalFrames == 0) { w.error = "WAV 音频数据为空"; return false; }
    return true;
}

static DWORD channelMask(uint16_t channels) {
    switch (channels) {
        case 1: return SPEAKER_FRONT_CENTER;
        case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 3: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
        case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 5: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 6: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        default: return 0;
    }
}
static WAVEFORMATEXTENSIBLE makeFormat(uint32_t rate, uint16_t channels, uint16_t bits, bool isFloat) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = channels;
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = bits;
    f.Format.nBlockAlign = (WORD)(channels * ((bits + 7) / 8));
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = 22;
    f.Samples.wValidBitsPerSample = bits;
    f.dwChannelMask = channelMask(channels);
    f.SubFormat = isFloat ? SUBTYPE_FLOAT_LOCAL : SUBTYPE_PCM_LOCAL;
    return f;
}
static bool supportsFormat(IMMDevice* device, const WAVEFORMATEX* format) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) return false;
    return client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format, nullptr) == S_OK;
}
static std::string formatJson(uint32_t rate, uint16_t channels, uint16_t bits, bool isFloat, bool supported) {
    std::ostringstream o;
    o << "{\"sampleRate\":" << rate << ",\"channels\":" << channels
      << ",\"bitsPerSample\":" << bits << ",\"sampleType\":\"" << (isFloat ? "float" : "pcm")
      << "\",\"supported\":" << (supported ? "true" : "false") << "}";
    return o.str();
}

static int listDevices(const std::wstring& probePath) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) { emit("{\"success\":false,\"error\":\"MMDevice 初始化失败\"}"); return 2; }
    ComPtr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
    }
    WaveInfo source;
    const bool hasSource = !probePath.empty();
    const bool sourceValid = !hasSource || readWave(probePath, source);
    ComPtr<IMMDeviceCollection> devices;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr)) { emit("{\"success\":false,\"error\":\"枚举输出设备失败\"}"); return 2; }
    UINT count = 0; devices->GetCount(&count);
    std::ostringstream out;
    out << "{\"success\":true,\"devices\":[";
    bool firstDevice = true;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev; if (FAILED(devices->Item(i, &dev))) continue;
        LPWSTR rawId = nullptr; if (FAILED(dev->GetId(&rawId)) || !rawId) continue;
        std::wstring id(rawId); CoTaskMemFree(rawId);
        ComPtr<IPropertyStore> props;
        std::wstring name = L"Audio Output";
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                name = v.pwszVal;
            PropVariantClear(&v);
        }
        ComPtr<IAudioClient> client;
        WAVEFORMATEX* mix = nullptr; uint32_t mixRate = 0; uint16_t mixBits = 0, mixChannels = 0;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) &&
            SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            mixRate = mix->nSamplesPerSec; mixBits = mix->wBitsPerSample; mixChannels = mix->nChannels;
            CoTaskMemFree(mix); mix = nullptr;
        }
        std::vector<std::string> formats;
        const uint32_t rates[] = {44100,48000,88200,96000,176400,192000,352800,384000};
        const uint16_t bitDepths[] = {16,24,32};
        for (uint32_t rate : rates) {
            for (uint16_t bits : bitDepths) {
                for (uint16_t channels : { (uint16_t)1, (uint16_t)2 }) {
                    auto candidate = makeFormat(rate, channels, bits, false);
                    bool ok = supportsFormat(dev.Get(), &candidate.Format);
                    formats.push_back(formatJson(rate, channels, bits, false, ok));
                }
            }
        }
        bool exact = false;
        if (hasSource && sourceValid) exact = supportsFormat(dev.Get(), source.format());
        if (!firstDevice) out << ",";
        firstDevice = false;
        out << "{\"id\":\"" << jsonEscape(utf8(id)) << "\",\"name\":\"" << jsonEscape(utf8(name))
            << "\",\"isDefault\":" << (id == defaultId ? "true" : "false")
            << ",\"mixFormat\":{\"sampleRate\":" << mixRate << ",\"channels\":" << mixChannels
            << ",\"bitsPerSample\":" << mixBits << "},\"supportsSourceFormat\":"
            << (exact ? "true" : "false") << ",\"formats\":[";
        for (size_t n=0;n<formats.size();++n) { if(n) out << ","; out << formats[n]; }
        out << "]}";
    }
    out << "]";
    if (hasSource) {
        if (!sourceValid) out << ",\"sourceError\":\"" << jsonEscape(source.error) << "\"";
        else out << ",\"sourceFormat\":{\"sampleRate\":" << source.format()->nSamplesPerSec
            << ",\"channels\":" << source.format()->nChannels << ",\"bitsPerSample\":"
            << source.format()->wBitsPerSample << ",\"sampleType\":\"" << (source.isFloat?"float":"pcm")
            << "\",\"duration\":" << (double)source.totalFrames/source.format()->nSamplesPerSec << "}";
    }
    out << "}";
    emit(out.str());
    return 0;
}

enum Command { CMD_NONE=0, CMD_PAUSE=1, CMD_RESUME=2, CMD_STOP=3, CMD_SEEK=4 };
static std::atomic<int> gCommand{CMD_NONE};
static std::atomic<double> gSeekSeconds{0.0};
static HANDLE gCommandEvent = nullptr;
static std::atomic<bool> gInputEnded{false};

static void commandReader() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "pause") gCommand.store(CMD_PAUSE);
        else if (line == "resume") gCommand.store(CMD_RESUME);
        else if (line == "stop") gCommand.store(CMD_STOP);
        else if (line.rfind("seek=", 0) == 0) {
            try { gSeekSeconds.store(std::max(0.0, std::stod(line.substr(5)))); gCommand.store(CMD_SEEK); }
            catch (...) {}
        }
        if (gCommandEvent) SetEvent(gCommandEvent);
        if (gInputEnded.load()) break;
    }
}

static int playFile(const std::wstring& path, const std::wstring& requestedDevice) {
    WaveInfo wave;
    if (!readWave(path, wave)) {
        emit("{\"type\":\"error\",\"code\":\"invalid_wav\",\"error\":\"" + jsonEscape(wave.error) + "\"}");
        return 3;
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(hr);
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        emit("{\"type\":\"error\",\"code\":\"device_init\",\"error\":\"WASAPI 设备服务初始化失败\"}");
        if (ownsCom) CoUninitialize(); return 4;
    }
    ComPtr<IMMDevice> device;
    if (requestedDevice.empty()) hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    else hr = enumerator->GetDevice(requestedDevice.c_str(), &device);
    if (FAILED(hr) || !device) {
        emit("{\"type\":\"error\",\"code\":\"device_missing\",\"error\":\"所选输出设备不可用\"}");
        if (ownsCom) CoUninitialize(); return 5;
    }
    if (!supportsFormat(device.Get(), wave.format())) {
        emit("{\"type\":\"error\",\"code\":\"unsupported_format\",\"error\":\"设备不支持 WAV 原始采样率/声道/位深的 WASAPI 独占格式\"}");
        if (ownsCom) CoUninitialize(); return 6;
    }
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) {
        emit("{\"type\":\"error\",\"code\":\"activate_failed\",\"error\":\"打开 WASAPI 音频客户端失败\"}");
        if (ownsCom) CoUninitialize(); return 7;
    }
    REFERENCE_TIME defaultPeriod=0, minPeriod=0;
    client->GetDevicePeriod(&defaultPeriod, &minPeriod);
    REFERENCE_TIME period = minPeriod > 0 ? minPeriod : (defaultPeriod > 0 ? defaultPeriod : 100000);
    HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gCommandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent || !gCommandEvent) {
        emit("{\"type\":\"error\",\"code\":\"event_failed\",\"error\":\"创建音频事件失败\"}");
        if (audioEvent) CloseHandle(audioEvent); if(gCommandEvent) CloseHandle(gCommandEvent);
        if (ownsCom) CoUninitialize(); return 8;
    }
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, period, period, wave.format(), nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 alignedFrames=0;
        client->GetBufferSize(&alignedFrames);
        client.Reset();
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr) && alignedFrames > 0) {
            period = (REFERENCE_TIME)((10000000.0 * alignedFrames) / wave.format()->nSamplesPerSec);
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, period, period, wave.format(), nullptr);
        }
    }
    if (FAILED(hr)) {
        std::ostringstream e; e << "{\"type\":\"error\",\"code\":\"exclusive_open_failed\",\"hresult\":\"" << hrString(hr)
          << "\",\"error\":\"WASAPI 独占模式打开失败，设备可能被占用或不接受该格式\"}";
        emit(e.str());
        CloseHandle(audioEvent); CloseHandle(gCommandEvent); gCommandEvent=nullptr;
        if (ownsCom) CoUninitialize(); return 9;
    }
    hr = client->SetEventHandle(audioEvent);
    ComPtr<IAudioRenderClient> render;
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    UINT32 bufferFrames=0;
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || !render || bufferFrames == 0) {
        emit("{\"type\":\"error\",\"code\":\"render_init_failed\",\"error\":\"WASAPI 渲染缓冲区初始化失败\"}");
        client->Stop(); client->Reset();
        CloseHandle(audioEvent); CloseHandle(gCommandEvent); gCommandEvent=nullptr;
        if (ownsCom) CoUninitialize(); return 10;
    }

    std::wstring deviceName=L"Audio Output";
    ComPtr<IPropertyStore> props; if (SUCCEEDED(device->OpenPropertyStore(STGM_READ,&props))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&v)) && v.vt==VT_LPWSTR && v.pwszVal) deviceName=v.pwszVal;
        PropVariantClear(&v);
    }
    auto* fmt=wave.format();
    std::ostringstream ready;
    ready << "{\"type\":\"ready\",\"mode\":\"exclusive\",\"device\":\"" << jsonEscape(utf8(deviceName))
          << "\",\"sampleRate\":" << fmt->nSamplesPerSec << ",\"channels\":" << fmt->nChannels
          << ",\"bitsPerSample\":" << fmt->wBitsPerSample << ",\"sampleType\":\"" << (wave.isFloat?"float":"pcm")
          << "\",\"duration\":" << (double)wave.totalFrames/fmt->nSamplesPerSec
          << ",\"bufferFrames\":" << bufferFrames << "}";
    emit(ready.str());

    std::thread inputThread(commandReader);
    DWORD taskIndex=0;
    HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&taskIndex);
    const uint64_t blockAlign=fmt->nBlockAlign;
    uint64_t submittedFrames=0;
    bool paused=false, pendingEnd=false, done=false;
    auto lastStatus=std::chrono::steady_clock::now();

    auto fillBuffer = [&]() -> bool {
        UINT32 padding=0;
        HRESULT p=client->GetCurrentPadding(&padding);
        if (FAILED(p) || padding > bufferFrames) return false;
        UINT32 available=bufferFrames-padding;
        if (available==0) return true;
        BYTE* dst=nullptr;
        HRESULT b=render->GetBuffer(available,&dst);
        if (FAILED(b) || !dst) return false;
        uint64_t remainingFrames = wave.totalFrames > submittedFrames ? wave.totalFrames-submittedFrames : 0;
        UINT32 framesToRead=(UINT32)std::min<uint64_t>(available,remainingFrames);
        uint64_t bytesToRead=(uint64_t)framesToRead*blockAlign;
        wave.file.clear();
        wave.file.seekg((std::streamoff)(wave.dataOffset + submittedFrames*blockAlign),std::ios::beg);
        if (bytesToRead) wave.file.read((char*)dst,(std::streamsize)bytesToRead);
        uint64_t bytesRead=(uint64_t)wave.file.gcount();
        bytesRead -= bytesRead % blockAlign;
        uint32_t actualFrames=(uint32_t)(bytesRead/blockAlign);
        if (actualFrames < available)
            std::memset(dst + (uint64_t)actualFrames*blockAlign, 0,
                        (size_t)((uint64_t)(available-actualFrames)*blockAlign));
        HRESULT rel=render->ReleaseBuffer(available,0);
        if (FAILED(rel)) return false;
        submittedFrames += actualFrames;
        if (actualFrames < available || submittedFrames >= wave.totalFrames) pendingEnd=true;
        return true;
    };

    if (!fillBuffer() || FAILED(client->Start())) {
        emit("{\"type\":\"error\",\"code\":\"start_failed\",\"error\":\"WASAPI 启动音频流失败\"}");
        done=true;
    }
    while (!done) {
        HANDLE handles[2]={audioEvent,gCommandEvent};
        DWORD wr=WaitForMultipleObjects(2,handles,FALSE,500);
        if (wr == WAIT_OBJECT_0+1) {
            ResetEvent(gCommandEvent);
            int cmd=gCommand.exchange(CMD_NONE);
            if (cmd==CMD_STOP) { done=true; break; }
            if (cmd==CMD_PAUSE && !paused) { client->Stop(); paused=true; emit("{\"type\":\"paused\"}"); }
            else if (cmd==CMD_RESUME && paused) {
                if (FAILED(client->Start())) { emit("{\"type\":\"error\",\"code\":\"resume_failed\",\"error\":\"恢复 WASAPI 播放失败\"}"); done=true; }
                else { paused=false; emit("{\"type\":\"playing\"}"); }
            } else if (cmd==CMD_SEEK) {
                double seconds=gSeekSeconds.load();
                uint64_t frame=(uint64_t)std::min<double>(wave.totalFrames,std::max(0.0,seconds*fmt->nSamplesPerSec));
                client->Stop(); client->Reset();
                submittedFrames=frame; pendingEnd=false;
                if (!fillBuffer()) { emit("{\"type\":\"error\",\"code\":\"seek_failed\",\"error\":\"WAV 定位失败\"}"); done=true; }
                else if (!paused && FAILED(client->Start())) { emit("{\"type\":\"error\",\"code\":\"seek_start_failed\",\"error\":\"定位后重新启动失败\"}"); done=true; }
                emit("{\"type\":\"seeked\",\"position\":" + std::to_string((double)frame/fmt->nSamplesPerSec) + "}");
            }
        } else if (wr == WAIT_OBJECT_0 && !paused) {
            UINT32 padding=0;
            if (FAILED(client->GetCurrentPadding(&padding))) { emit("{\"type\":\"error\",\"code\":\"padding_failed\",\"error\":\"读取 WASAPI 缓冲状态失败\"}"); done=true; }
            else if (pendingEnd) {
                done=true;
                emit("{\"type\":\"ended\"}");
            } else if (!fillBuffer()) {
                emit("{\"type\":\"error\",\"code\":\"buffer_failed\",\"error\":\"写入 WASAPI PCM 缓冲区失败\"}"); done=true;
            }
            auto now=std::chrono::steady_clock::now();
            if (now-lastStatus >= std::chrono::milliseconds(100)) {
                UINT32 pad=0; client->GetCurrentPadding(&pad);
                uint64_t played=submittedFrames>pad?submittedFrames-pad:0;
                double pos=(double)played/fmt->nSamplesPerSec;
                if (pos > (double)wave.totalFrames/fmt->nSamplesPerSec) pos=(double)wave.totalFrames/fmt->nSamplesPerSec;
                emit("{\"type\":\"progress\",\"position\":" + std::to_string(pos) + "}");
                lastStatus=now;
            }
        } else if (wr == WAIT_TIMEOUT) {
            if (gCommand.load()!=CMD_NONE) SetEvent(gCommandEvent);
        } else if (wr == WAIT_FAILED) {
            emit("{\"type\":\"error\",\"code\":\"wait_failed\",\"error\":\"WASAPI 等待音频事件失败\"}"); done=true;
        }
    }
    client->Stop(); client->Reset();
    gInputEnded.store(true);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (inputThread.joinable()) { if (gCommandEvent) SetEvent(gCommandEvent); inputThread.detach(); }
    render.Reset(); client.Reset(); device.Reset(); enumerator.Reset();
    CloseHandle(audioEvent); CloseHandle(gCommandEvent); gCommandEvent=nullptr;
    if (ownsCom) CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring(argv[1]) == L"--list-devices") {
        std::wstring path;
        if (argc >= 4 && std::wstring(argv[2]) == L"--probe-file") path = argv[3];
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int result = listDevices(path);
        if (SUCCEEDED(hr)) CoUninitialize();
        return result;
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--play") {
        std::wstring deviceId;
        for (int i=3;i+1<argc;i++) if (std::wstring(argv[i]) == L"--device-id") deviceId=argv[i+1];
        return playFile(argv[2],deviceId);
    }
    emit("{\"type\":\"error\",\"code\":\"usage\",\"error\":\"用法: splayer-wasapi.exe --list-devices [--probe-file file.wav] 或 --play file.wav [--device-id id]\"}");
    return 1;
}
