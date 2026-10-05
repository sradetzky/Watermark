#include "core/watermark.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <stdexcept>
#include <sstream>

namespace watermark {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ").";
        throw std::runtime_error(message.str());
    }
}
class ComInitialization {
public:
    ComInitialization() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result != RPC_E_CHANGED_MODE) { check(result, "COM initialization"); initialized_ = true; }
    }
    ~ComInitialization() { if (initialized_) { CoUninitialize(); } }
private:
    bool initialized_ = false;
};
class Imaging {
    ComInitialization com_;
public:
    Imaging() {
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&factory)), "Create WIC factory");
    }
    ComPtr<IWICImagingFactory> factory;
};
}

Image load_image(const std::filesystem::path& path) {
    Imaging imaging;
    ComPtr<IWICBitmapDecoder> decoder;
    check(imaging.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnLoad, &decoder), "Open image");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "Read image frame");
    UINT width = 0, height = 0;
    check(frame->GetSize(&width, &height), "Read dimensions");
    if (width > INT_MAX || height > INT_MAX) { throw std::runtime_error("Image dimensions exceed supported range."); }
    Image image(static_cast<int>(width), static_cast<int>(height));
    ComPtr<IWICFormatConverter> converter;
    check(imaging.factory->CreateFormatConverter(&converter), "Create pixel converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
          WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Convert pixels to RGBA");
    static_assert(sizeof(Pixel) == 4);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.pixels.size() * 4),
          reinterpret_cast<BYTE*>(image.pixels.data())), "Read pixels");
    return image;
}

void save_image(const Image& image, const std::filesystem::path& path, ImageFormat format, int jpeg_quality) {
    image.validate();
    if (jpeg_quality < 1 || jpeg_quality > 100) { throw std::invalid_argument("JPEG quality must be 1..100."); }
    Imaging imaging;
    ComPtr<IWICStream> stream;
    check(imaging.factory->CreateStream(&stream), "Create image stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open output image");
    ComPtr<IWICBitmapEncoder> encoder;
    check(imaging.factory->CreateEncoder(format == ImageFormat::png ? GUID_ContainerFormatPng :
          GUID_ContainerFormatJpeg, nullptr, &encoder), "Create image encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(&frame, &properties), "Create output frame");
    if (format == ImageFormat::jpeg) {
        PROPBAG2 option{};
        option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = jpeg_quality / 100.0f;
        check(properties->Write(1, &option, &value), "Set JPEG quality");
    }
    check(frame->Initialize(properties.Get()), "Initialize output frame");
    check(frame->SetSize(image.width, image.height), "Set output dimensions");
    WICPixelFormatGUID pixel_format = format == ImageFormat::png ?
        GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&pixel_format), "Set output pixel format");
    Image flattened;
    const Image* source = &image;
    if (format == ImageFormat::jpeg) {
        flattened = image;
        for (auto& pixel : flattened.pixels) {
            const auto composite = [&](std::uint8_t channel) {
                return static_cast<std::uint8_t>((channel * pixel.a + 255 * (255 - pixel.a) + 127) / 255);
            };
            pixel.r = composite(pixel.r); pixel.g = composite(pixel.g); pixel.b = composite(pixel.b);
            pixel.a = 255;
        }
        source = &flattened;
    }
    ComPtr<IWICBitmap> bitmap;
    check(imaging.factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppRGBA,
          image.width * 4, static_cast<UINT>(source->pixels.size() * 4),
          reinterpret_cast<BYTE*>(const_cast<Pixel*>(source->pixels.data())), &bitmap), "Create output bitmap");
    ComPtr<IWICFormatConverter> converter;
    check(imaging.factory->CreateFormatConverter(&converter), "Create output converter");
    check(converter->Initialize(bitmap.Get(), pixel_format, WICBitmapDitherTypeNone, nullptr,
          0, WICBitmapPaletteTypeCustom), "Convert output pixels");
    check(frame->WriteSource(converter.Get(), nullptr), "Encode pixels");
    check(frame->Commit(), "Commit image frame");
    check(encoder->Commit(), "Commit image");
}
} // namespace watermark
