// НаомиФон — буфер Android (gralloc, блок памяти ION) из своего кода: его видит видеочип (текстура и цель
// отрисовки через EGLImage), видеокодер (номер fd) и процессор (mmap). Через расширение libhybris
// EGL_HYBRIS_native_buffer2 (docs/STREAM.md, «Видеочип»).
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cstddef>

struct GrallocBuffer {
    EGLClientBuffer buffer = nullptr;   // ANativeWindowBuffer
    int fd = -1;                        // dma-buf; принадлежит буферу — не закрывать
    int stride = 0;                     // в пикселях
    size_t size = 0;                    // байт в dma-buf
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    GLuint fbo = 0;
    void *map = nullptr;                // вид для процессора (mapForCpu)
};

// форматы и назначение — как в Android (hybris_nativebufferext.h)
enum : int {
    kGrallocRgba8888 = 1,
    kGrallocBlob = 0x21,
    kGrallocSwReadOften = 0x3,
    kGrallocSwReadRarely = 0x2,
    kGrallocSwWriteOften = 0x30,
    kGrallocHwTexture = 0x100,
    kGrallocHwRender = 0x200,
    kGrallocHwVideoEncoder = 0x10000,
};

bool grallocAllocate(GrallocBuffer &b, int width, int height, int format, int usage);
// Буфер RGBA, который видеочип заполняет целиком, — не меньше bytes байт: ширина равна строке (gralloc строку
// иногда выравнивает: 512 → 768 пикселей, и тогда хвост каждой строки текстуре не виден). widthHint — с какой
// ширины начать; не совпала со строкой — заново шириной строки.
bool grallocAllocateLinear(GrallocBuffer &b, size_t bytes, int widthHint, int usage);
bool grallocAllocateExtra(GrallocBuffer &b, size_t bytes);
// EGLImage, текстура и кадровый буфер — в текущем контексте GL
bool grallocBindGl(GrallocBuffer &b, EGLDisplay dpy);
bool grallocMapForCpu(GrallocBuffer &b);
void grallocFree(GrallocBuffer &b, EGLDisplay dpy);
