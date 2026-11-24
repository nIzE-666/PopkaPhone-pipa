#include "gralloc_buffer.h"

#include <GLES2/gl2ext.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <climits>
#include <cstring>
#include <vector>

namespace {

using CreateFn = EGLBoolean (*)(EGLint, EGLint, EGLint, EGLint, EGLint *, EGLClientBuffer *);
using ReleaseFn = EGLBoolean (*)(EGLClientBuffer);
using InfoFn = EGLBoolean (*)(EGLClientBuffer, int *, int *);
using SerializeFn = EGLBoolean (*)(EGLClientBuffer, int *, int *);

template <typename T> T proc(const char *name)
{
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

constexpr EGLenum kNativeBufferHybris = 0x3140;   // = EGL_NATIVE_BUFFER_ANDROID

} // namespace

bool grallocAllocate(GrallocBuffer &b, int width, int height, int format, int usage)
{
    static const auto create = proc<CreateFn>("eglHybrisCreateNativeBuffer");
    static const auto info = proc<InfoFn>("eglHybrisGetNativeBufferInfo");
    static const auto serialize = proc<SerializeFn>("eglHybrisSerializeNativeBuffer");
    if (!create || !info || !serialize) {
        fprintf(stderr, "gralloc: нет EGL_HYBRIS_native_buffer2\n");
        return false;
    }
    EGLint stride = 0;
    if (!create(width, height, usage, format, &stride, &b.buffer) || !b.buffer) {
        fprintf(stderr, "gralloc: буфер %dx%d (формат 0x%x) не выделен\n", width, height, format);
        return false;
    }
    int numInts = 0, numFds = 0;
    info(b.buffer, &numInts, &numFds);
    std::vector<int> ints(numInts), fds(numFds);
    serialize(b.buffer, ints.data(), fds.data());
    if (numFds < 1) {
        fprintf(stderr, "gralloc: у буфера нет fd\n");
        return false;
    }
    b.fd = fds[0];
    b.stride = stride;
    const off_t end = lseek(b.fd, 0, SEEK_END);
    lseek(b.fd, 0, SEEK_SET);
    b.size = end > 0 ? size_t(end) : 0;
    return b.size > 0;
}

bool grallocAllocateLinear(GrallocBuffer &b, size_t bytes, int widthHint, int usage)
{
    int w = widthHint;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int h = int((bytes + size_t(w) * 4 - 1) / (size_t(w) * 4));
        if (!grallocAllocate(b, w, h, kGrallocRgba8888, usage))
            return false;
        if (b.stride == w)
            return true;
        w = b.stride;
        grallocFree(b, EGL_NO_DISPLAY);
    }
    fprintf(stderr, "gralloc: строка буфера так и не совпала с шириной\n");
    return false;
}

bool grallocAllocateExtra(GrallocBuffer &b, size_t bytes)
{
    if (bytes > INT_MAX || !grallocAllocate(b, int(bytes), 1, kGrallocBlob, kGrallocSwWriteOften | kGrallocHwVideoEncoder)
        || b.size < bytes)
        return false;
    void *map = mmap(nullptr, bytes, PROT_WRITE, MAP_SHARED, b.fd, 0);
    if (map == MAP_FAILED) {
        perror("venus: mmap extradata");
        return false;
    }
    dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    ioctl(b.fd, DMA_BUF_IOCTL_SYNC, &sync);
    memset(map, 0, bytes);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    ioctl(b.fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(map, bytes);
    return true;
}

bool grallocBindGl(GrallocBuffer &b, EGLDisplay dpy)
{
    static const auto createImage = proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    static const auto targetTexture = proc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>("glEGLImageTargetTexture2DOES");
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    b.image = createImage(dpy, EGL_NO_CONTEXT, kNativeBufferHybris, b.buffer, attrs);
    if (b.image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "gralloc: EGLImage не создан (0x%x)\n", eglGetError());
        return false;
    }
    glGenTextures(1, &b.texture);
    glBindTexture(GL_TEXTURE_2D, b.texture);
    targetTexture(GL_TEXTURE_2D, b.image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &b.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gralloc: кадровый буфер не готов (0x%x)\n", status);
        return false;
    }
    return true;
}

bool grallocMapForCpu(GrallocBuffer &b)
{
    void *p = mmap(nullptr, b.size, PROT_READ, MAP_SHARED, b.fd, 0);
    if (p == MAP_FAILED) {
        perror("gralloc: mmap");
        return false;
    }
    b.map = p;
    return true;
}

void grallocFree(GrallocBuffer &b, EGLDisplay dpy)
{
    static const auto destroyImage = proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    static const auto release = proc<ReleaseFn>("eglHybrisReleaseNativeBuffer");
    if (b.map)
        munmap(b.map, b.size);
    if (b.fbo)
        glDeleteFramebuffers(1, &b.fbo);
    if (b.texture)
        glDeleteTextures(1, &b.texture);
    if (b.image != EGL_NO_IMAGE_KHR)
        destroyImage(dpy, b.image);
    if (b.buffer && release)
        release(b.buffer);
    b = GrallocBuffer{};
}
