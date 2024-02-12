#include "logger.hpp"
#include "yr_math.hpp"
#include "yr_sys.h"
#include "yr_graphics.h"

extern "C" {
    #include "../externals/ffmpeg/include/libavformat/avformat.h"
    #include "../externals/ffmpeg/include/libavcodec/avcodec.h"
    #include "../externals/ffmpeg/include/libswscale/swscale.h"
    #include "../externals/ffmpeg/include/libavutil/imgutils.h"
}

#include <filesystem>
#include <cstdlib>
#include <vector>

template<class T>
void freec(T*) {}

template<class T>
struct smp {
    T* ptr;
    inline smp(T* p) :ptr(p) {}
    T* operator->() { return ptr; }
    T& operator*() { return *ptr; }
    inline operator T* () { return ptr; }
    ~smp() {
        if(ptr) freec(ptr);
    }
};

template<> void freec<AVFormatContext>(AVFormatContext* ctx) { avformat_free_context(ctx); }
template<> void freec<AVCodecContext>(AVCodecContext* ctx) { avcodec_free_context(&ctx); }
template<> void freec<AVFrame>(AVFrame* ctx) { av_frame_free(&ctx); }
template<> void freec<AVDictionary>(AVDictionary* d) { av_dict_free(&d); }
template<> void freec<SwsContext>(SwsContext* ctx) { sws_freeContext(ctx); }

int main(int argc, char* argv[]){
#if BOOST_OS_WINDOWS
    system("chcp 65001");
#endif
    std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());
    if (argc < 4) {
        LOGRAW("usage:", argv[0], "input.mp4 filter.frag output.mp4");
        return 0;
    }
    auto fmt = smp(avformat_alloc_context());
    int errCode = 0;

    if ((errCode = avformat_open_input(&fmt.ptr, argv[1], nullptr, nullptr)) < 0) {
        LOGRAW("Failed to demux the file. Please check the file integrity:", errCode);
        return 1;
    }
    if ((errCode = avformat_find_stream_info(fmt, nullptr)) < 0) {
        LOGRAW("Failed to find stream info:", errCode);
        return 2;
    }
    int vidStream = -1;
    for (int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidStream = i;
            break;
        }
    }
    if (vidStream == -1) {
        LOGRAW("The file", argv[1], "does not seem to contain video stream");
        return 3;
    }
    
    AVStream* vid = fmt->streams[vidStream];
    const AVCodec* codec = avcodec_find_decoder(vid->codecpar->codec_id);
    if(!codec){
        LOGRAW("Could not find the codec", vid->codecpar->codec_id);
        return 4;
    }
    onart::Window::init();
    onart::YRGraphics* yg = new onart::YRGraphics;
    const double TB_STREAM = (double)vid->time_base.num / vid->time_base.den;
    const double INV_TB_STREAM = 1.0 / TB_STREAM;
    constexpr double TB_DOUBLE = AV_TIME_BASE;
    constexpr double INV_TB_DOUBLE = 1.0 / TB_DOUBLE;
    int width = vid->codecpar->width, height = vid->codecpar->height;
    LOGRAW("Resolution:", width, "x", height);
    LOGRAW("Length:", fmt->duration * INV_TB_DOUBLE, "seconds");
    auto cctx = smp(avcodec_alloc_context3(codec));
    
    avcodec_parameters_to_context(cctx, vid->codecpar);
    smp<AVDictionary> codecOpts = nullptr;
    LOGRAW(av_get_pix_fmt_name(AV_PIX_FMT_RGB32));
    std::vector<uint8_t> tempRGBA(av_image_get_buffer_size(AV_PIX_FMT_RGB32,width,height,1));
    av_dict_set(&codecOpts.ptr, "pix_fmt", av_get_pix_fmt_name(AV_PIX_FMT_RGB32),0);
    if((errCode = avcodec_open2(cctx, nullptr, &codecOpts.ptr)) < 0) {
        LOGRAW("Could not open the codec context:", errCode);
        return 5;
    }
    auto sctx1 = smp(sws_getContext(width, height, cctx->pix_fmt, width, height, AV_PIX_FMT_RGB32, SWS_POINT, nullptr, nullptr, nullptr));
    auto sctx2 = smp(sws_getContext(width, height, AV_PIX_FMT_RGB32, width, height, cctx->pix_fmt, SWS_POINT, nullptr, nullptr, nullptr));
    AVPacket tempPacket{};
    auto tempFrame = smp(av_frame_alloc());
    auto tempFrame2 = smp(av_frame_alloc());
    onart::YRGraphics::RenderPassCreationOptions opts;
    opts.autoclear.use = false;
    opts.canCopy = true;
    opts.width = width;
    opts.height = height;
    opts.linearSampled = false;
    auto rp = yg->createRenderPass(0, opts);
    auto st = yg->createStreamTexture(0, width, height, true);
    auto nm = yg->createNullMesh(0, 3);
    while(av_read_frame(fmt, &tempPacket) == 0){
        if (tempPacket.stream_index != vidStream) continue;
        int err2 = avcodec_send_packet(cctx, &tempPacket);
        if(err2 == 0){
            avcodec_receive_frame(cctx, tempFrame);
            err2 = sws_scale_frame(sctx1, tempFrame2, tempFrame);
            if(err2 < 0){
                LOGRAW("Failed to transcode pixel format:",err2);
                return 6;
            }
            rp->start();
            rp->invoke(nm);
            rp->execute();
            rp->wait();
            auto ptr = rp->readBack(0);
            tempFrame2->data;
        }
        av_packet_unref(&tempPacket);
    }
    delete yg;
    onart::Window::terminate();
}