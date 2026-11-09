/* Native headless GPU rendering for tests: with RT_HEADLESS=1 RT_DUMP=N the
 * Glide frames are rendered offscreen (EGL, surfaceless Mesa) on the game
 * thread and every Nth one is written to build/frames/gNNNNN.ppm. */
#if !defined(__EMSCRIPTEN__) && !defined(RT_NO_SDL)
#define _GNU_SOURCE
#include "rt.h"
#include "gpu.h"
#include <time.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

extern GLuint gpu_target_fbo;
extern volatile double gpu_renderer_seen_reset;
static EGLDisplay dpy; static int egl_ok = -1; static GLuint fb, rb;
static int egl_init(void) {
    if (egl_ok >= 0) return egl_ok;
    egl_ok = 0;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!dpy || !eglInitialize(dpy, NULL, NULL)) { rt_log("gpu headless: no EGL display"); return 0; }
    eglBindAPI(EGL_OPENGL_ES_API);
    rt_log("gpu headless: EGL %s (%s)", eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
    EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE }; EGLConfig cfg = NULL; EGLint nc = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &nc) || !nc) cfg = NULL;   /* EGL_KHR_no_config_context */
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { rt_log("gpu headless: no EGL context"); return 0; }
    glGenFramebuffers(1, &fb); glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 640, 480);
    glBindFramebuffer(GL_FRAMEBUFFER, fb); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    return egl_ok = 1;
}
/* gpu.c: an LFB lock wants the shown picture. Headless, the frames are drawn here on the game
 * thread, so draw now (gpu_render serves the readback); -1 when not headless. */
int gpu_headless_sync(void) {
    if (!getenv("RT_HEADLESS")) return -1;
    if (!egl_init()) return 0;
    gpu_target_fbo = fb;
    gpu_render(640, 480);
    gpu_renderer_seen_reset = 1;
    return 1;
}
void gpu_headless_frame(GFrame *unused) {
    (void)unused;
    static int every = -1, n;
    if (every < 0) {
        every = 0;
        if (getenv("RT_HEADLESS") && getenv("RT_DUMP")) every = atoi(getenv("RT_DUMP"));
        if (every <= 0) every = 0;
    }
    if (!every) return;
    static double from = -1, t0;                      /* RT_DUMP_FROM=s: dump nothing before then */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (from < 0) { from = getenv("RT_DUMP_FROM") ? atof(getenv("RT_DUMP_FROM")) : 0; t0 = now; }
    if (now - t0 < from) { n++; return; }
    if (++n % every) return;
    if (!egl_init()) return;
    gpu_target_fbo = fb;
    gpu_render(640, 480);
    gpu_renderer_seen_reset = 1;
    static uint8_t px[640 * 480 * 4];
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const char *dir = getenv("RT_DUMP_DIR") ? getenv("RT_DUMP_DIR") : "build/frames";
    char path[512]; snprintf(path, sizeof path, "%s/g%06d.ppm", dir, n);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n640 480\n255\n");
    for (int y = 479; y >= 0; y--) for (int x = 0; x < 640; x++) fwrite(px + (y * 640 + x) * 4, 1, 3, f);
    fclose(f);
}
#endif
