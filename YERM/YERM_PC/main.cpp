#include "logger.hpp"
#include "yr_sys.h"
#include "yr_graphics.h"
#include "yr_game.h"
#include "yr_constants.hpp"

#include "../../fmp.h"
extern "C" {
    #include "../externals/ffmpeg/include/libavformat/avformat.h"
    #include "../externals/ffmpeg/include/libavcodec/avcodec.h"
    #include "../externals/ffmpeg/include/libswscale/swscale.h"
    #include "../externals/ffmpeg/include/libavutil/imgutils.h"
    #include "../externals/ffmpeg/include/libavutil/opt.h"
}

#ifdef YR_USE_VULKAN
#include "../externals/shaderc/shaderc.hpp"
#ifndef DEBUG
#pragma comment(lib, "shaderc_combined.lib")
#endif
#endif

#include <filesystem>
#include <cstdlib>
#include <vector>

template<class T>
void freec(T*) {}

template<class T>
struct smp {
    T* ptr;
    inline smp(T* p) :ptr(p) {}
    inline smp& operator=(T* p) {
        if (ptr) freec(ptr);
        ptr = p;
        return *this;
    }
    T* operator->() const { return ptr; }
    T& operator*() const { return *ptr; }
    T** operator&() const { return &ptr; }
    inline operator T* () const { return ptr; }
    T** operator&() { return &ptr; }
    ~smp() {
        if (ptr) freec(ptr);
    }
};

template<> void freec<AVFormatContext>(AVFormatContext* ctx) { avformat_free_context(ctx); }
template<> void freec<AVCodecContext>(AVCodecContext* ctx) { avcodec_free_context(&ctx); }
template<> void freec<AVFrame>(AVFrame* ctx) { av_frame_free(&ctx); }
template<> void freec<AVDictionary>(AVDictionary* d) { av_dict_free(&d); }
template<> void freec<SwsContext>(SwsContext* ctx) { sws_freeContext(ctx); }
template<> void freec<AVPacket>(AVPacket* pkt) { av_packet_free(&pkt); }

thread_local char errorString[128];

int main(int argc, char* argv[]){
#if BOOST_OS_WINDOWS
    system("chcp 65001");
#endif
    std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());
    
    
    if (argc < 4) {
        LOGRAW("usage:", argv[0], "input.mp4 filter.frag output.mp4 [new width] [new height]");
        return 0;
    }
    std::filesystem::path video(argv[1]);
    std::filesystem::path fs(argv[2]);
    std::filesystem::path output(argv[3]);
    if (!std::filesystem::exists(video)) {
        LOGRAW("video file", video.u8string(), "does not exist");
        return 1;
    }
    if (!std::filesystem::exists(fs)) {
        LOGRAW("Fragment shader code file", fs.u8string(), "does not exist");
        return 1;
    }
    int w = 0, h = 0;
    if (argc >= 5) {
        w = std::atoi(argv[4]);
    }
    if (argc >= 6) {
        h = std::atoi(argv[5]);
    }

    LOGRAW("Compiling shader..");
    onart::Window::init();
    onart::YRGraphics* _gr(new onart::YRGraphics);
    onart::Window::CreationOptions wopts{};
    wopts.height = 720;
    wopts.width = 1280;
    onart::Window* window = new onart::Window(nullptr, &wopts);
    _gr->addWindow(0, window);

    onart::YRGraphics::ShaderModuleCreationOptions shaderOpts{};
    onart::shader_t vertShader{}, fragShader{};
    
#ifdef YR_USE_VULKAN
    {
#ifndef DEBUG
        FILE* fsFile = fopen(fs.string().c_str(), "rb");
        fseek(fsFile, 0, SEEK_END);
        auto fsSize = ftell(fsFile);
        fseek(fsFile, 0, SEEK_SET);
        std::vector<char> content(fsSize);
        fread(content.data(), 1, fsSize, fsFile);
        fclose(fsFile);
        shaderc::Compiler compiler;
        shaderc::CompileOptions compileOptions;
        compileOptions.SetOptimizationLevel(shaderc_optimization_level_performance);
        auto result = compiler.CompileGlslToSpv(content.data(), fsSize, shaderc_shader_kind::shaderc_glsl_fragment_shader, u8"main", compileOptions);
        if (result.GetCompilationStatus() != shaderc_compilation_status::shaderc_compilation_status_success) {
            LOGRAW("Shader Compile:", result.GetErrorMessage());
            return 2;
        }
        std::vector<uint32_t> spv{ result.cbegin(),result.cend() };
        shaderOpts.size = spv.size() * sizeof(spv[0]);
        shaderOpts.source = spv.data();
        shaderOpts.stage = onart::YRGraphics::ShaderStage::FRAGMENT;
        fragShader = onart::YRGraphics::createShader(0, shaderOpts);
        shaderOpts.size = sizeof(TEST_TX_VERT);
        shaderOpts.source = TEST_TX_VERT;
        shaderOpts.stage = onart::YRGraphics::ShaderStage::VERTEX;
        vertShader = onart::YRGraphics::createShader(1, shaderOpts);
#else
        shaderOpts.size = sizeof(TEST_TX_FRAG);
        shaderOpts.source = TEST_TX_FRAG;
        shaderOpts.stage = onart::YRGraphics::ShaderStage::FRAGMENT;
        fragShader = onart::YRGraphics::createShader(0, shaderOpts);
        shaderOpts.size = sizeof(TEST_TX_VERT);
        shaderOpts.source = TEST_TX_VERT;
        shaderOpts.stage = onart::YRGraphics::ShaderStage::VERTEX;
        vertShader = onart::YRGraphics::createShader(1, shaderOpts);
#endif
    }
#elif defined(YR_USE_D3D11)
    {
        FILE* fsFile = fopen(fs.string().c_str(), "rb");
        fseek(fsFile, 0, SEEK_END);
        auto fsSize = ftell(fsFile);
        fseek(fsFile, 0, SEEK_SET);
        std::vector<char> content(fsSize);
        fread(content.data(), 1, fsSize, fsFile);
        fclose(fsFile);
        auto blob = onart::compileShader(content.data(), onart::YRGraphics::ShaderStage::FRAGMENT);
        if (!blob) {
            LOGRAW("Shader Compile Error");
            return 2;
        }
        shaderOpts.size = blob->GetBufferSize();
        shaderOpts.source = blob->GetBufferPointer();
        shaderOpts.stage = onart::YRGraphics::ShaderStage::FRAGMENT;
        fragShader = onart::YRGraphics::createShader(0, shaderOpts);
        blob->Release();
        if (!fragShader) {
            LOGRAW("Shader Compile Error");
            return 2;
        }
    }
#elif defined(YR_USE_OPENGL)
    FILE* fsFile = fopen(fs.string().c_str(), "rb");
    fseek(fsFile, 0, SEEK_END);
    auto fsSize = ftell(fsFile);
    fseek(fsFile, 0, SEEK_SET);
    std::vector<char> content(fsSize);
    fread(content.data(), 1, fsSize, fsFile);
    fclose(fsFile);
    shaderOpts.size = fsSize;
    shaderOpts.source = content.data();
    shaderOpts.stage = onart::YRGraphics::ShaderStage::FRAGMENT;
    fragShader = onart::YRGraphics::createShader(0, shaderOpts);
    if (!fragShader) {
        LOGRAW("Shader Compile Error");
        return 2;
    }
#endif
    LOGRAW("Compile done\n\nMuxing input video..");
    smp<AVFormatContext> inputFmt = avformat_alloc_context();
#define ON_ERROR_RETURN(type, val)  if(errcode < 0){ LOGRAW(toString(type, av_make_error_string(errorString, sizeof(errorString), errcode))); return val; }
    int errcode = avformat_open_input(&inputFmt, video.string().c_str(), nullptr, nullptr);
    ON_ERROR_RETURN("Open input", 3);
    errcode = avformat_find_stream_info(inputFmt, nullptr);
    ON_ERROR_RETURN("Find stream info", 3);
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;
    for (int i = 0; i < inputFmt->nb_streams; i++) {
        switch (inputFmt->streams[i]->codecpar->codec_type)
        {
        case AVMEDIA_TYPE_AUDIO: audioStreamIndex = i; break;
        case AVMEDIA_TYPE_VIDEO: videoStreamIndex = i; break;
        default: break;
        }
    }
    if (videoStreamIndex == -1) {
        LOGRAW("Video stream not found from", video);
        return 3;
    }
    AVStream* videoStream = inputFmt->streams[videoStreamIndex];
    const AVCodec* decoder = avcodec_find_decoder(videoStream->codecpar->codec_id);
    const AVCodec* encoder = avcodec_find_encoder(videoStream->codecpar->codec_id);
    smp<AVCodecContext> decContext = avcodec_alloc_context3(decoder);
    smp<AVCodecContext> encContext = avcodec_alloc_context3(encoder);
    avcodec_parameters_to_context(decContext, videoStream->codecpar);
    errcode = avcodec_open2(decContext, decoder, nullptr);
    ON_ERROR_RETURN("Decoder open", 6);
    double duration = inputFmt->duration / 1'000'000.0;
    LOGRAW("Mux done: Duration", duration, "s | Resolution", videoStream->codecpar->width, videoStream->codecpar->height);
    if (w <= 0) {
        if (h > 0) {
            double realW = (double)h * videoStream->codecpar->width / videoStream->codecpar->height;
            w = std::lround(realW);
            w += w & 1;
        }
        else {
            w = videoStream->codecpar->width;
        }
    }
    if (h <= 0) {
        double realH = (double)w * videoStream->codecpar->height / videoStream->codecpar->width;
        h = std::lround(realH);
        h += h & 1;
    }
    LOGRAW("->", w, h);
    onart::YRGraphics::RenderPassCreationOptions rpOpts{};
    rpOpts.autoclear.use = true;
    rpOpts.canCopy = true;
    rpOpts.depthInput = false;
    rpOpts.width = w;
    rpOpts.height = h;
    rpOpts.subpassCount = 1;
    auto renderPass = onart::YRGraphics::createRenderPass(0, rpOpts);
    auto wd = onart::YRGraphics::createRenderPass2Screen(0, 0, rpOpts);
    auto quad = onart::YRGraphics::createNullMesh(0, 3);
    {
        onart::YRGraphics::PipelineCreationOptions pipeOpts;
        pipeOpts.vertexShader = vertShader;
        pipeOpts.fragmentShader = fragShader;
        pipeOpts.vertexSize = 0;
        pipeOpts.vertexAttributeCount = 0;
        pipeOpts.pass = renderPass;
        pipeOpts.shaderResources.pos0 = onart::YRGraphics::ShaderResourceType::TEXTURE_1;

        auto pp = onart::YRGraphics::createPipeline(0, pipeOpts);
        wd->usePipeline(pp, 0);
    }

    onart::YRGraphics::pStreamTexture tex = onart::YRGraphics::createStreamTexture(0, videoStream->codecpar->width, videoStream->codecpar->height, !(w % videoStream->codecpar->width == 0 && h % videoStream->codecpar->height == 0));
    if (!tex) {
        LOGRAW("Failed to create stream texture");
        return 5;
    }

    AVRational timeBase = decContext->time_base;
    if (timeBase.den * timeBase.num == 0) { timeBase = videoStream->time_base; }

    LOGRAW("Preparing encoder..");
    smp<AVFormatContext> outputFmt(nullptr);
    errcode = avformat_alloc_output_context2(&outputFmt, nullptr, nullptr, output.string().c_str());
    ON_ERROR_RETURN("Allocate output context", 4);
    for (int i = 0; i < inputFmt->nb_streams; i++) {
        avformat_new_stream(outputFmt, NULL);
        avcodec_parameters_copy(outputFmt->streams[i]->codecpar, inputFmt->streams[i]->codecpar);
    }
    AVStream* outputVideoStream = outputFmt->streams[videoStreamIndex];

    outputVideoStream->time_base = videoStream->time_base;
    LOGWITH(decContext->framerate.num, decContext->framerate.den);
    LOGWITH(videoStream->time_base.num, videoStream->time_base.den);
    double totalFrame = 1.0 / outputVideoStream->nb_frames;
    encContext->bit_rate = decContext->bit_rate * w * h / (videoStream->codecpar->width * videoStream->codecpar->height);
    if (encContext->bit_rate == 0) {
        encContext->bit_rate = (int64_t)w * h * decContext->framerate.num / decContext->framerate.den;
    }
    encContext->width = w;
    encContext->height = h;
    encContext->time_base = outputVideoStream->time_base;
    encContext->framerate = decContext->framerate;
    encContext->gop_size = 4;
    encContext->max_b_frames = 1;
    encContext->pix_fmt = decContext->pix_fmt;
    avcodec_parameters_from_context(outputVideoStream->codecpar, encContext);
    //av_opt_set(encContext->priv_data, "crf", "1", 0);
    
    errcode = avcodec_open2(encContext, encoder, nullptr);
    ON_ERROR_RETURN("Encoder codec context open", 4);

    if (outputFmt->oformat->flags & AVFMT_GLOBALHEADER)
        outputFmt->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (!(outputFmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outputFmt->pb, output.string().c_str(), AVIO_FLAG_WRITE) < 0) {
            LOGRAW("Output file open fail");
            return 4;
        }
    }
    avformat_write_header(outputFmt, nullptr);

    LOGRAW("Decoder ready:", decoder->long_name);
    LOGRAW("Encoder ready:", encoder->long_name);
    
    smp<SwsContext> preproc1{ nullptr }, preproc2{ nullptr };
    if (decContext->pix_fmt != AV_PIX_FMT_BGRA) {
        preproc1 = sws_getContext(videoStream->codecpar->width, videoStream->codecpar->height, decContext->pix_fmt, videoStream->codecpar->width, videoStream->codecpar->height, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
    }
    if (encContext->pix_fmt != AV_PIX_FMT_RGBA) {
        preproc2 = sws_getContext(videoStream->codecpar->width, videoStream->codecpar->height, AV_PIX_FMT_RGBA, videoStream->codecpar->width, videoStream->codecpar->height, encContext->pix_fmt, SWS_POINT, nullptr, nullptr, nullptr);
    }
    smp<AVPacket> decPacket = av_packet_alloc(), encPacket = av_packet_alloc();
    smp<AVFrame> 
        decFrame = av_frame_alloc(), 
        procFrame = av_frame_alloc(), 
        encFrame = av_frame_alloc();

    procFrame->format = decContext->pix_fmt;
    procFrame->width = videoStream->codecpar->width;
    procFrame->height = videoStream->codecpar->height;
    av_frame_get_buffer(procFrame, 0);

    encFrame->format = decContext->pix_fmt;
    encFrame->width = w;
    encFrame->height = h;
    av_frame_get_buffer(encFrame, 0);
    
    printf("0%%"); fflush(stdout);

    bool invoked = false;
    int64_t pts = 0;
    int64_t frameDuration = 0;

    while (av_read_frame(inputFmt, decPacket) == 0) {
        if (decPacket->stream_index == videoStreamIndex) {
            errcode = avcodec_send_packet(decContext, decPacket);
            if (errcode == AVERROR(EAGAIN)) {}
            else if (errcode == AVERROR_EOF) { break; }
            else if (errcode) {
                av_make_error_string(errorString, sizeof(errorString), errcode);
                LOGRAW(errorString);
                break;
            }
            errcode = avcodec_receive_frame(decContext, decFrame);
            if (errcode == AVERROR(EAGAIN)) { continue; }
            else if (errcode == AVERROR_EOF) {
                avcodec_flush_buffers(decContext);
                break;
            }
            av_frame_copy(procFrame, decFrame);
            av_frame_copy_props(procFrame, decFrame);
            //av_packet_unref(decPacket); // this was incorrect..
            if (invoked) {
                // encode
                renderPass->wait();
                auto pix = renderPass->readBack(0);
                auto src = pix.get();
                if (preproc2) {
                    int srcPitch = w * 4;
                    sws_scale(preproc2, &src, &srcPitch, 0, h, encFrame->data, encFrame->linesize);
                }
                else {
                    for (int i = 0; i < procFrame->height; i++) {
                        std::memcpy(encFrame->data[0], src, w * 4);
                        src += w * 4;
                    }
                }

                encFrame->pts = pts;
                encFrame->duration = frameDuration;
                errcode = avcodec_send_frame(encContext, encFrame);
                if (errcode < 0) {
                    LOGRAW("Failed to send frame", encFrame->pts);
                    // todo: appropriate process on failure
                }
                errcode = avcodec_receive_packet(encContext, encPacket);
                if (errcode == AVERROR(EAGAIN)) {

                }
                else if (errcode < 0) {
                    LOGRAW("Failed to receive packet", encFrame->pts);
                    LOGRAW(toString(av_make_error_string(errorString, sizeof(errorString), errcode)));
                    // todo: appropriate process on failure
                }
                else {
                    encPacket->stream_index = videoStreamIndex;
                    av_packet_rescale_ts(encPacket, videoStream->time_base, outputVideoStream->time_base);
                    av_interleaved_write_frame(outputFmt, encPacket);
                }
                printf("\r%.2f%%", encFrame->pts * 100 * timeBase.num / timeBase.den / duration);
            }
            else { invoked = true; }
            pts = decFrame->pts;
            frameDuration = decFrame->duration;
            tex->updateBy([&preproc1, &procFrame](void* data, uint32_t) {
                if (preproc1) {
                    uint8_t* castedData = (uint8_t*)data;
                    int dstPitch = procFrame->width * 4;
                    sws_scale(preproc1, procFrame->data, procFrame->linesize, 0, procFrame->height, &castedData, &dstPitch);
                }
                else {
                    uint8_t* castedData = (uint8_t*)data;
                    uint8_t* src = procFrame->data[0];
                    for (int i = 0; i < procFrame->height; i++) {
                        std::memcpy(castedData, src, procFrame->width * 4);
                        src += procFrame->linesize[0];
                    }
                }
            });
            renderPass->start();
            renderPass->bind(0, tex);
            renderPass->invoke(quad);
            renderPass->execute();
            wd->start();
            wd->bind(0, renderPass);
            wd->invoke(quad);
            wd->execute(renderPass);
        }
        else {
            av_packet_rescale_ts(encPacket, videoStream->time_base, outputVideoStream->time_base);
            av_interleaved_write_frame(outputFmt, decPacket);
        }
    }

    // encode last one
    {
        renderPass->wait();
        auto pix = renderPass->readBack(0);
        auto src = pix.get();
        if (preproc2) {
            int srcPitch = w * 4;
            sws_scale(preproc2, &src, &srcPitch, 0, h, encFrame->data, encFrame->linesize);
        }
        else {
            for (int i = 0; i < procFrame->height; i++) {
                std::memcpy(encFrame->data[0], src, w * 4);
                src += w * 4;
            }
        }

        encFrame->pts = pts;
        encFrame->duration = frameDuration;
        errcode = avcodec_send_frame(encContext, encFrame);
        if (errcode < 0) {
            LOGRAW("Failed to send frame", encFrame->pts);
            // todo: appropriate process on failure
        }
        errcode = avcodec_receive_packet(encContext, encPacket);
        if (errcode < 0) {
            LOGRAW("Failed to receive packet", encFrame->pts);
            // todo: appropriate process on failure
            LOGRAW(toString(av_make_error_string(errorString, sizeof(errorString), errcode)));
        }
        encPacket->stream_index = videoStreamIndex;
        av_packet_rescale_ts(encPacket, videoStream->time_base, outputVideoStream->time_base);
        av_interleaved_write_frame(outputFmt, encPacket);
    }
    av_write_trailer(outputFmt);

    avformat_close_input(&inputFmt);
    avio_close(outputFmt->pb);
    quad.reset();
    tex.reset();
    delete _gr;
    onart::Window::terminate();
}