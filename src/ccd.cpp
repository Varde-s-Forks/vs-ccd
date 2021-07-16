#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <VapourSynth.h>
#include <VSHelper.h>

// i dont know if there's a better way to do this :dek:
static const double DIVISORS[] = {1., 1. / 2, 1. / 3, 1. / 4, 1. / 5, 1. / 6, 1. / 7, 1. / 8, 1. / 9, 1. / 10, 1. / 11, 1. / 12, 1. / 13, 1. / 14, 1. / 15, 1. / 16, 1. / 17, 1. / 18, 1. / 19, 1. / 20};

typedef struct ccdData
{
    VSNodeRef *node;
    const VSVideoInfo *vi;
    float threshold;
} ccdData;

static void ccd_run(const VSFrameRef *src, VSFrameRef *dest, float threshold, const VSAPI *vsapi)
{
    int width = vsapi->getFrameWidth(src, 0);
    int height = vsapi->getFrameHeight(src, 0);

    auto *src_r_plane = reinterpret_cast<const uint8_t *>(vsapi->getReadPtr(src, 0));
    auto *src_g_plane = reinterpret_cast<const uint8_t *>(vsapi->getReadPtr(src, 1));
    auto *src_b_plane = reinterpret_cast<const uint8_t *>(vsapi->getReadPtr(src, 2));

    auto *dst_r_plane = reinterpret_cast<uint8_t *>(vsapi->getWritePtr(dest, 0));
    auto *dst_g_plane = reinterpret_cast<uint8_t *>(vsapi->getWritePtr(dest, 1));
    auto *dst_b_plane = reinterpret_cast<uint8_t *>(vsapi->getWritePtr(dest, 2));

    for (int y = 0; y < height; y++)
    {
        int y_start = y > 11 ? y - 12 : y;
        int y_end = y < height - 12 ? y + 12 : y;

        for (int x = 0; x < width; x++)
        {
            int i = y * width + x;

            auto r = src_r_plane[i];
            auto g = src_g_plane[i];
            auto b = src_b_plane[i];

            int total_r = r, total_g = g, total_b = b;
            int n = 0;

            int x_start = x > 11 ? x - 12 : x;
            int x_end = x < width - 12 ? x + 12 : x;

            for (int comp_y = y_start; comp_y < y_end; comp_y += 8)
            {
                int y_offset = comp_y * width;
                for (int comp_x = x_start; comp_x < x_end; comp_x += 8)
                {
                    auto comp_r = src_r_plane[y_offset + comp_x];
                    auto comp_g = src_g_plane[y_offset + comp_x];
                    auto comp_b = src_b_plane[y_offset + comp_x];

                    auto diff_r = comp_r - r;
                    auto diff_g = comp_g - r;
                    auto diff_b = comp_b - r;

#define SQUARE(x) (x * x)
                    if (threshold > (SQUARE(diff_r) + SQUARE(diff_g) + SQUARE(diff_b)))
                    {
                        total_r += comp_r;
                        total_b += comp_b;
                        total_g += comp_g;
                        n++;
                    }
#undef SQUARE
                }
            }

            auto calculated_r = total_r * DIVISORS[n];
            auto calculated_g = total_g * DIVISORS[n];
            auto calculated_b = total_b * DIVISORS[n];

            if (calculated_r < 0)
                calculated_r = 0;
            else if (calculated_r > 255)
                calculated_r = 255;

            if (calculated_g < 0)
                calculated_g = 0;
            else if (calculated_g > 255)
                calculated_g = 255;

            if (calculated_b < 0)
                calculated_b = 0;
            else if (calculated_b > 255)
                calculated_b = 255;

            dst_r_plane[i] = calculated_r + 0.5;
            dst_g_plane[i] = calculated_g + 0.5;
            dst_b_plane[i] = calculated_b + 0.5;
        }
    }
}

static void VS_CC ccdInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi)
{
    (void)in;
    (void)out;
    (void)core;

    auto *d = (ccdData *)*instanceData;

    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *
    VS_CC
    ccdGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx,
                VSCore *core, const VSAPI *vsapi)
{
    auto *d = static_cast<ccdData *>(*instanceData);

    if (activationReason == arInitial)
    {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    }
    else if (activationReason == arAllFramesReady)
    {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        const VSFormat *format = vsapi->getFrameFormat(src);

        int width = vsapi->getFrameWidth(src, 0);
        int height = vsapi->getFrameHeight(src, 0);

        const VSFrameRef *plane_src[3] = {src, src, src};
        int planes[3] = {0, 1, 2};

        VSFrameRef *dest = vsapi->newVideoFrame2(format, width, height, plane_src, planes, src, core);

        ccd_run(src, dest, d->threshold, vsapi);

        return dest;
    }

    return nullptr;
}

static void VS_CC ccdFree(void *instanceData, VSCore *core, const VSAPI *vsapi)
{
    auto *d = (ccdData *)instanceData;
    vsapi->freeNode(d->node);
    free(d);
}

static void VS_CC ccdCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi)
{
    ccdData d;
    ccdData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d.threshold = static_cast<float>(vsapi->propGetFloat(in, "threshold", 0, &err));
    if (err)
        d.threshold = 4;

    d.vi = vsapi->getVideoInfo(d.node);

    if (!d.vi->format)
    {
        vsapi->setError(out, "CCD: Variable format clips are not supported.");
        vsapi->freeNode(d.node);
    }

    if (d.vi->format->id != 2000010)
    { // ID of RGBS
        vsapi->setError(out, "CCD: Input clip must be RGB24");
        vsapi->freeNode(d.node);
    }

    if (d.threshold < 0)
    {
        vsapi->setError(out, "CCD: Threshold must be >= 0");
        vsapi->freeNode(d.node);
    }

    data = (ccdData *)malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "ccd", ccdInit, ccdGetframe, ccdFree, fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc,
                      VSPlugin *plugin)
{
    configFunc("com.eoe-scrad.ccd", "ccd", "chroma denoiser", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("CCD",
                 "clip:clip;"
                 "threshold:float:opt;",
                 ccdCreate, nullptr, plugin);
}