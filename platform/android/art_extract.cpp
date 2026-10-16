// Pulls still pictures out of the game's own cutscene files (already extracted from the user's disc) for the second-screen
// artwork. Nothing is bundled: the pictures are made on the device from the player's own copy of the game.
#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace
{
constexpr int kWidth = 640;

struct Picture
{
    int w = 0, h = 0;
    std::vector<uint32_t> argb;
    double luma = 0.0;
};

double MeanLuma(const std::vector<uint32_t>& px)
{
    if (px.empty())
        return 0.0;
    uint64_t sum = 0;
    const size_t step = 7;   // sample, this is only a "is it black" test
    size_t n = 0;
    for (size_t i = 0; i < px.size(); i += step, ++n)
    {
        const uint32_t p = px[i];
        sum += (((p >> 16) & 0xFF) * 3 + ((p >> 8) & 0xFF) * 6 + (p & 0xFF)) / 10;
    }
    return static_cast<double>(sum) / static_cast<double>(n);
}

// mode 0: the first picture at or after `seconds`.
// mode 1: the title card at the end of an episode intro: a little before the last picture that is not black.
bool Extract(const char* path, int mode, double seconds, Picture& out)
{
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(format, nullptr) < 0)
    {
        avformat_close_input(&format);
        return false;
    }

    int video = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { video = static_cast<int>(i); break; }
    if (video < 0)
    {
        avformat_close_input(&format);
        return false;
    }

    AVStream* stream = format->streams[video];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* decoder = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    if (decoder == nullptr || avcodec_parameters_to_context(decoder, stream->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0)
    {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return false;
    }

    const int outW = std::min(kWidth, decoder->width);
    const int outH = std::max(2, decoder->height * outW / std::max(1, decoder->width));
    SwsContext* scaler = sws_getContext(decoder->width, decoder->height, decoder->pix_fmt, outW, outH, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::deque<Picture> ring;
    constexpr size_t kRing = 48;
    int lastBright = -1;   // index (in decode order, counting ring pushes) of the last bright picture
    int total = 0;
    bool found = false;

    auto handle = [&](AVFrame* f) -> bool
    {
        Picture pic;
        pic.w = outW;
        pic.h = outH;
        pic.argb.resize(static_cast<size_t>(outW) * outH);
        uint8_t* dst[4] = { reinterpret_cast<uint8_t*>(pic.argb.data()), nullptr, nullptr, nullptr };
        int dstStride[4] = { outW * 4, 0, 0, 0 };
        sws_scale(scaler, f->data, f->linesize, 0, decoder->height, dst, dstStride);
        for (uint32_t& p : pic.argb)
            p |= 0xFF000000u;   // BGRA bytes on little endian == 0xAARRGGBB, force opaque
        pic.luma = MeanLuma(pic.argb);

        if (mode == 0)
        {
            // Raw elementary streams often carry no usable timestamps: fall back to counting pictures.
            const double fps = av_q2d(stream->avg_frame_rate) > 1.0 ? av_q2d(stream->avg_frame_rate) : 29.97;
            const double counted = static_cast<double>(total) / fps;
            ++total;
            const double pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? counted : f->best_effort_timestamp * av_q2d(stream->time_base);
            if (pts >= seconds)
            {
                out = std::move(pic);
                found = true;
                return true;
            }
            return false;
        }

        if (pic.luma > 9.0)
            lastBright = total;
        ring.push_back(std::move(pic));
        if (ring.size() > kRing)
            ring.pop_front();
        ++total;
        return false;
    };

    bool done = false;
    while (!done && av_read_frame(format, packet) >= 0)
    {
        if (packet->stream_index == video && avcodec_send_packet(decoder, packet) >= 0)
        {
            while (avcodec_receive_frame(decoder, frame) >= 0)
            {
                if (handle(frame)) { done = true; break; }
            }
        }
        av_packet_unref(packet);
    }
    if (!done)
    {
        avcodec_send_packet(decoder, nullptr);
        while (avcodec_receive_frame(decoder, frame) >= 0)
            if (handle(frame)) break;
    }

    if (mode == 1 && lastBright >= 0 && !ring.empty())
    {
        // ring holds pictures [total - ring.size(), total); take ~0.8 s (20 pictures) before the last bright one if still available
        int want = lastBright - 20;
        const int first = total - static_cast<int>(ring.size());
        want = std::max(want, first);
        out = std::move(ring[static_cast<size_t>(want - first)]);
        found = true;
    }

    sws_freeContext(scaler);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return found;
}
}   // namespace

// int[] { width, height, argb... } or null
extern "C" JNIEXPORT jintArray JNICALL
Java_org_projectcane_cane_SecondScreenActivity_nativeDecodeFrame(JNIEnv* env, jclass, jstring jpath, jint mode, jdouble seconds)
{
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    Picture pic;
    const bool ok = path != nullptr && Extract(path, mode, seconds, pic);
    __android_log_print(ANDROID_LOG_INFO, "cane", "[Art] %s mode=%d t=%.1f -> %s", path != nullptr ? path : "?", mode, seconds, ok ? "ok" : "no picture");
    if (path != nullptr)
        env->ReleaseStringUTFChars(jpath, path);
    if (!ok)
        return nullptr;

    const jsize n = static_cast<jsize>(pic.argb.size()) + 2;
    jintArray result = env->NewIntArray(n);
    if (result == nullptr)
        return nullptr;
    jint header[2] = { pic.w, pic.h };
    env->SetIntArrayRegion(result, 0, 2, header);
    env->SetIntArrayRegion(result, 2, n - 2, reinterpret_cast<const jint*>(pic.argb.data()));
    return result;
}
