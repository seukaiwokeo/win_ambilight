#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <array>
#include <mutex>
#include <span>
#include <cmath>
#include <thread>
#include <algorithm>
#include <condition_variable>
#include <libusb-1.0/libusb.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "libusb-1.0.lib")

#undef max
#undef min

constexpr int ENDPOINT_OUT = 1;
constexpr int INTERFACE_NUMBER = 0;
constexpr int VENDOR_ID = 0x1A86;
constexpr int PRODUCT_ID = 0xFE07;

constexpr int NUM_LEDS = 12;
constexpr int NUM_GROUPS = 3;
constexpr int TOTAL_LEDS = NUM_LEDS * NUM_GROUPS;

using ColorArray = std::array<std::array<float, NUM_GROUPS>, NUM_LEDS>;

UINT screenWidth = 0, screenWidth4 = 0;
UINT screenHeight = 0, screenHeight4 = 0;

using Microsoft::WRL::ComPtr;

struct alignas(4) RGBData {
    uint8_t r, g, b, _pad;
    constexpr RGBData(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0) : r(r), g(g), b(b), _pad(0) {}
    constexpr RGBData(float r, float g, float b) : r(static_cast<uint8_t>(r)), g(static_cast<uint8_t>(g)), b(static_cast<uint8_t>(b)), _pad(0) {}
};

static constexpr std::array<uint8_t, 7> HEADER = { 0x53, 0x43, 0x00, 0xB1, 0xBF, 0x80, 0x01 };
static std::array<uint8_t, 192> ledBuffer;

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<ID3D11ComputeShader> computeShader;
ComPtr<ID3D11Texture2D> smallTexture1, smallTexture2, smallTexture3;
ComPtr<ID3D11ShaderResourceView> srv1, srv2, srv3;
ComPtr<ID3D11UnorderedAccessView> uav1, uav2, uav3;
ComPtr<ID3D11Buffer> resultBuffer;
ComPtr<ID3D11Texture2D> stagingTexture;
ComPtr<ID3D11UnorderedAccessView> resultUAV;
D3D11_BUFFER_DESC bufferDesc = {};
ComPtr<IDXGIOutputDuplication> outputDuplication;
ComPtr<ID3D11Buffer> stagingBuffer;

static ComPtr<ID3DBlob> CompileShader(const wchar_t* filename, const char* entryPoint, const char* target)
{
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(filename, nullptr, nullptr, entryPoint, target, 0, 0, &shaderBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << (char*)errorBlob->GetBufferPointer() << "\n";
        }
        throw std::runtime_error("Shader compilation failed");
    }
    return shaderBlob;
}

static bool InitD3D()
{
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
        &device, &featureLevel, &context);
    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDevice failed\n";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> dxgiOutput;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC outputDesc;
    hr = dxgiOutput->GetDesc(&outputDesc);
    if (FAILED(hr)) return false;

    screenWidth = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    screenHeight = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    screenWidth4 = screenWidth / 8;
    screenHeight4 = screenHeight / 8;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = screenWidth4;
    texDesc.Height = screenHeight4;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    device->CreateTexture2D(&texDesc, nullptr, &smallTexture1);
    device->CreateTexture2D(&texDesc, nullptr, &smallTexture2);
    device->CreateTexture2D(&texDesc, nullptr, &smallTexture3);

    device->CreateShaderResourceView(smallTexture1.Get(), nullptr, &srv1);
    device->CreateShaderResourceView(smallTexture2.Get(), nullptr, &srv2);
    device->CreateShaderResourceView(smallTexture3.Get(), nullptr, &srv3);

    device->CreateUnorderedAccessView(smallTexture1.Get(), nullptr, &uav1);
    device->CreateUnorderedAccessView(smallTexture2.Get(), nullptr, &uav2);
    device->CreateUnorderedAccessView(smallTexture3.Get(), nullptr, &uav3);

    bufferDesc.ByteWidth = sizeof(float) * 4;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufferDesc.CPUAccessFlags = 0;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = sizeof(float) * 4;

    hr = device->CreateBuffer(&bufferDesc, nullptr, &resultBuffer);
    if (FAILED(hr)) {
        std::cerr << "CreateBuffer failed\n";
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 1;

    hr = device->CreateUnorderedAccessView(resultBuffer.Get(), &uavDesc, &resultUAV);
    if (FAILED(hr)) {
        std::cerr << "CreateUnorderedAccessView for resultBuffer failed\n";
        return false;
    }

    ComPtr<ID3DBlob> csBlob = CompileShader(L"ambilight.hlsl", "CSMain", "cs_5_0");
    hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &computeShader);
    if (FAILED(hr)) {
        std::cerr << "CreateComputeShader failed\n";
        return false;
    }

    std::cout << "Shader compiled." << std::endl;

    ComPtr<IDXGIOutput1> dxgiOutput1;
    hr = dxgiOutput.As(&dxgiOutput1);
    if (FAILED(hr)) return false;

    hr = dxgiOutput1->DuplicateOutput(device.Get(), &outputDuplication);
    if (FAILED(hr)) {
        std::cerr << "DuplicateOutput failed\n";
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = screenWidth;
    stagingDesc.Height = screenHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;

    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        std::cerr << "Create staging texture failed\n";
        return false;
    }

    D3D11_BUFFER_DESC desc = bufferDesc;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    device->CreateBuffer(&desc, nullptr, &stagingBuffer);

    return true;
}

static void readColorData(int index, int side, ID3D11ShaderResourceView* srv,
    ComPtr<ID3D11Texture2D>& smallTexture, std::array<float, 3>& output)
{
    D3D11_BOX srcBox{};
    srcBox.front = 0;
    srcBox.back = 1;

    if (side == 0) {
        srcBox.left = 0;
        srcBox.top = (screenHeight * index) / NUM_LEDS;
        srcBox.right = screenWidth4;
        srcBox.bottom = (screenHeight * (index + 1)) / NUM_LEDS;
    }
    else if (side == 1) {
        srcBox.left = (screenWidth * index) / NUM_LEDS;
        srcBox.top = 0;
        srcBox.right = (screenWidth * (index + 1)) / NUM_LEDS;
        srcBox.bottom = screenHeight4;
    }
    else {
        srcBox.left = screenWidth - screenWidth4;
        srcBox.top = (screenHeight * index) / NUM_LEDS;
        srcBox.right = screenWidth;
        srcBox.bottom = (screenHeight * (index + 1)) / NUM_LEDS;
    }

    context->CopySubresourceRegion(smallTexture.Get(), 0, 0, 0, 0, stagingTexture.Get(), 0, &srcBox);
    context->CSSetShader(computeShader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, resultUAV.GetAddressOf(), nullptr);

    const UINT dispatchX = ((srcBox.right - srcBox.left) + 31) / 32;
    const UINT dispatchY = ((srcBox.bottom - srcBox.top) + 31) / 32;
    context->Dispatch(dispatchX, dispatchY, 1);

    context->CopyResource(stagingBuffer.Get(), resultBuffer.Get());
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    context->Map(stagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    std::copy_n(static_cast<const float*>(mapped.pData), 3, output.begin());
    context->Unmap(stagingBuffer.Get(), 0);
}

static bool ProcessFrame(ColorArray& led1, ColorArray& led2, ColorArray& led3)
{
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> desktopResource;
    HRESULT hr = outputDuplication->AcquireNextFrame(5000, &frameInfo, &desktopResource);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<ID3D11Texture2D> acquiredTexture;
    hr = desktopResource.As(&acquiredTexture);
    if (FAILED(hr)) {
        outputDuplication->ReleaseFrame();
        return false;
    }

    context->CopyResource(stagingTexture.Get(), acquiredTexture.Get());

    for (int i = 0; i < TOTAL_LEDS; ++i) {
        if (i < NUM_LEDS) {
            readColorData(i, 0, srv1.Get(), smallTexture1, led1[i]);
        }
        else if (i < NUM_LEDS * 2) {
            readColorData(i - NUM_LEDS, 1, srv2.Get(), smallTexture2, led2[i - NUM_LEDS]);
        }
        else {
            readColorData(i - NUM_LEDS * 2, 2, srv3.Get(), smallTexture3, led3[i - NUM_LEDS * 2]);
        }
    }

    outputDuplication->ReleaseFrame();
    return true;
}

static __forceinline void fast_memcpy(void* dst, const void* src, size_t size) {
    __movsb(static_cast<unsigned char*>(dst), static_cast<const unsigned char*>(src), size);
}

static void BuildLedBuffer(const RGBData* topLeft, const RGBData* top, const RGBData* topRight) {
    uint8_t* __restrict ptr = ledBuffer.data();

    fast_memcpy(ptr, HEADER.data(), HEADER.size());
    ptr += HEADER.size();

    // LED 1
    *ptr++ = topLeft[0].r;
    *ptr++ = topLeft[0].g;
    *ptr++ = topLeft[0].b;

    uint8_t c = 0x1;
    for (size_t i = 1; i < 12; ++i) {
        *ptr++ = c++;
        *ptr++ = c++;
        *ptr++ = topLeft[i].r;
        *ptr++ = topLeft[i].g;
        *ptr++ = topLeft[i].b;
    }

    // LED 2
    for (size_t i = 0; i < 12; ++i) {
        *ptr++ = c++;
        *ptr++ = c++;
        *ptr++ = top[i].r;
        *ptr++ = top[i].g;
        *ptr++ = top[i].b;
    }

    // LED 3
    for (size_t i = 0; i < 12; ++i) {
        *ptr++ = c++;
        *ptr++ = c++;
        *ptr++ = topRight[i].r;
        *ptr++ = topRight[i].g;
        *ptr++ = topRight[i].b;
    }

    __stosb(ptr, 0, ledBuffer.data() + ledBuffer.size() - ptr);
}

static __forceinline void convert_and_fill(std::span<RGBData, 12> dest, const ColorArray& src, bool reverse = false) {
    if (reverse) {
        for (int i = 0; i < 12; ++i) {
            dest[i] = {
                static_cast<uint8_t>(src[11 - i][0] * 255.0f),
                static_cast<uint8_t>(src[11 - i][1] * 255.0f),
                static_cast<uint8_t>(src[11 - i][2] * 255.0f)
            };
        }
    }
    else {
        for (int i = 0; i < 12; ++i) {
            dest[i] = {
                static_cast<uint8_t>(src[i][0] * 255.0f),
                static_cast<uint8_t>(src[i][1] * 255.0f),
                static_cast<uint8_t>(src[i][2] * 255.0f)
            };
        }
    }
}

static void update_leds(libusb_device_handle* handle, const ColorArray& currentColor1, const ColorArray& currentColor2, const ColorArray& currentColor3, uint8_t& counter) {
    alignas(16) std::array<RGBData, 12> dataLed1, dataLed2, dataLed3;

    convert_and_fill(dataLed1, currentColor1, true);
    convert_and_fill(dataLed2, currentColor2);
    convert_and_fill(dataLed3, currentColor3);

    BuildLedBuffer(dataLed1.data(), dataLed2.data(), dataLed3.data());
    ledBuffer[4] = counter++;

    int actual_length;
    constexpr int TIMEOUT = 100;
    libusb_interrupt_transfer(handle, ENDPOINT_OUT, ledBuffer.data(), 64, &actual_length, TIMEOUT);
    libusb_interrupt_transfer(handle, ENDPOINT_OUT, ledBuffer.data() + 64, 64, &actual_length, TIMEOUT);
    libusb_interrupt_transfer(handle, ENDPOINT_OUT, ledBuffer.data() + 128, 64, &actual_length, TIMEOUT);
}

int main() {
    if (!InitD3D())
        return 0;

    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;

    if (libusb_init(&ctx) < 0) {
        std::cerr << "libusb init failed\n";
        return 0;
    }

    handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!handle) {
        std::cerr << "Device not found\n";
        libusb_exit(ctx);
        return 0;
    }

    if (libusb_kernel_driver_active(handle, INTERFACE_NUMBER))
        libusb_detach_kernel_driver(handle, INTERFACE_NUMBER);

    if (libusb_claim_interface(handle, INTERFACE_NUMBER) != 0) {
        std::cerr << "Cannot claim interface\n";
        libusb_close(handle);
        libusb_exit(ctx);
        return 0;
    }

    static ColorArray ledColors[NUM_GROUPS]{};
    static ColorArray prevColors[NUM_GROUPS]{};
    static ColorArray targetColors[NUM_GROUPS]{};
    static ColorArray currentColors[NUM_GROUPS]{};

    const float COLOR_THRESHOLD = 0.3f;
    const float TRANSITION_TIME = 0.2f;

    std::mutex frameMutex;
    std::condition_variable frameCV;
    bool frameReady = false;
    bool shouldStop = false;

    std::thread processThread([&]() {
        while (!shouldStop) {
            static thread_local ColorArray newLed[NUM_GROUPS]{};
            bool processed = ProcessFrame(newLed[0], newLed[1], newLed[2]);
            if (processed) {
                std::lock_guard lock(frameMutex);
                for (auto i = 0; i < NUM_GROUPS; ++i)
                    ledColors[i] = newLed[i];
                frameReady = true;
                frameCV.notify_one();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / 30));
        }
        });

    auto lastTime = std::chrono::high_resolution_clock::now();
    uint8_t counter = 0;

    constexpr int STABLE_FRAME_COUNT = 20;
    int changeCounter[NUM_GROUPS] = {};

    while (!shouldStop) {
        {
            std::unique_lock lock(frameMutex);
            frameCV.wait_for(lock, std::chrono::milliseconds(1), [&] { return frameReady; });
            frameReady = false;
        }

        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::min(std::chrono::duration<float>(now - lastTime).count(), 0.1f);
        lastTime = now;

        for (int group = 0; group < NUM_GROUPS; ++group) {
            auto& ledColor = ledColors[group];
            auto& prevColor = prevColors[group];
            auto& targetColor = targetColors[group];
            auto& currentColor = currentColors[group];

            float maxDiff = 0.0f;
            for (int i = 0; i < NUM_LEDS; ++i) {
                float diff = std::sqrt(
                    std::pow(ledColor[i][0] - prevColor[i][0], 2) +
                    std::pow(ledColor[i][1] - prevColor[i][1], 2) +
                    std::pow(ledColor[i][2] - prevColor[i][2], 2)
                );
                maxDiff = std::max(maxDiff, diff);
            }
            if (maxDiff >= COLOR_THRESHOLD) {
                prevColor = ledColor;
                targetColor = ledColor;
            }

            for (int i = 0; i < NUM_LEDS; ++i) {
                bool transitionComplete = true;
                for (int c = 0; c < NUM_GROUPS; ++c) {
                    float diff = targetColor[i][c] - currentColor[i][c];
                    if (std::abs(diff) > 0.001f) {
                        float step = diff * deltaTime / TRANSITION_TIME;
                        step = std::clamp(step, -std::abs(diff), std::abs(diff));
                        currentColor[i][c] += step;
                        transitionComplete = false;
                    }
                    else {
                        currentColor[i][c] = targetColor[i][c];
                    }
                }
                if (transitionComplete)
                    currentColor[i] = targetColor[i];
            }
        }

        update_leds(handle, currentColors[0], currentColors[1], currentColors[2], counter);
    }

    shouldStop = true;
    processThread.join();

    libusb_release_interface(handle, INTERFACE_NUMBER);
    libusb_close(handle);
    libusb_exit(ctx);

    device.Reset();
    context.Reset();
    computeShader.Reset();
    smallTexture1.Reset();
    smallTexture2.Reset();
    smallTexture3.Reset();
    srv1.Reset();
    srv2.Reset();
    srv3.Reset();
    uav1.Reset();
    uav2.Reset();
    uav3.Reset();
    resultBuffer.Reset();
    resultUAV.Reset();
    outputDuplication.Reset();
    stagingTexture.Reset();

    return 0;
}