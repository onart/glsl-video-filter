#include "fmp.h"
#include <list>

extern "C" {
	#include "YERM/externals/ffmpeg/include/libavformat/avformat.h"
	#include "YERM/externals/ffmpeg/include/libavcodec/avcodec.h"
	#include "YERM/externals/ffmpeg/include/libswscale/swscale.h"
	#include "YERM/externals/ffmpeg/include/libavutil/imgutils.h"
}

#include "YERM/YERM_PC/yr_graphics.h"
#include "YERM/YERM_PC/yr_input.h"

#include "YERM/YERM_PC/logger.hpp"

namespace onart {

	thread_local int errorCode;
	thread_local char errorString[128];	

#define FMCALL_VERBOSE(call) errorCode = call; av_make_error_string(errorString, sizeof(errorString), errorCode)
#define FMCALL_BASIC(call) errorCode = call
#define FMCALL FMCALL_VERBOSE

	inline auto errstr(const char* header) { return toString(header, "-", errorCode, errorString, '\n'); }

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
		inline operator T* () const { return ptr; }
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

	template<class T>
	struct _1v1rb {
		std::vector<T> buffer;
		T nullObject{};
		int size;
		int input = 0;
		int output = 0;
		bool done = false;
		std::mutex mtx;
		std::condition_variable rcv;
		std::condition_variable wcv;

		inline int getNext(int i) const { return i + 1 < size ? i + 1 : 0; }

		inline auto& get2Write() {
			int ni = getNext(input);
			if (ni == output) {
				std::unique_lock _(mtx);
				while (ni == output) {
					wcv.wait(_);
				}
			}
			return buffer[input];
		}
		inline void return2write() {
			std::unique_lock _(mtx);
			input = getNext(input);
			rcv.notify_one();
		}
		inline const T& get2Read() {
			if (input == output) {
				if (done) return nullObject;
				std::unique_lock _(mtx);
				while (input == output) {
					rcv.wait(_);
				}
			}
			return buffer[output];
		}
		inline void return2Read() {
			std::unique_lock _(mtx);
			output = getNext(output);
			wcv.notify_one();
		}
	};

#define _THIS reinterpret_cast<_rb4f*>(structure)

	struct _rb4f : public _1v1rb<AVFrame*> {
		inline void init(int format, int width, int height) {
			for (auto& fr : buffer) {
				fr = av_frame_alloc();
				fr->format = format;
				fr->width = width;
				fr->height = height;
				av_frame_get_buffer(fr, 0); // align 32?
			}
		}
	};

	RingBuffer4Frame::RingBuffer4Frame(size_t size) {
		structure = new _rb4f;
		if (size < 2) size = 2;
		_THIS->buffer.resize(size);
		_THIS->size = size;
	}

	RingBuffer4Frame::~RingBuffer4Frame() {
		for (AVFrame* f : _THIS->buffer) {
			av_frame_free(&f);
		}
		delete _THIS;
	}

	size_t RingBuffer4Frame::load() {
		int diff = _THIS->input - _THIS->output;
		return diff < 0 ? diff : diff + _THIS->size;
	}
#undef _THIS

#define _THIS reinterpret_cast<_rb4t*>(structure)

	struct _rb4t :public _1v1rb<YRGraphics::pStreamTexture> {
		inline void init(int width, int height, bool linear) {
			for (auto& tx : buffer) {
				tx = YRGraphics::createStreamTexture(INT32_MIN, width, height, linear);
			}
		}
	};

	RingBuffer4Texture::RingBuffer4Texture(size_t size) {
		structure = new _rb4t;
		if (size < 2) size = 2;
		_THIS->buffer.resize(size);
		_THIS->size = size;
	}

	RingBuffer4Texture::~RingBuffer4Texture() {
		delete _THIS;
		structure = nullptr;
	}

#undef _THIS

#define _THIS reinterpret_cast<_rb4r*>(structure)
	struct _rb4r :public _1v1rb<std::vector<uint32_t>> {
		inline void init(int width, int height) {
			for (auto& img : buffer) {
				img.resize(width * height);
			}
		}
	};

	RingBuffer4RGBA::RingBuffer4RGBA(size_t size) {
		structure = new _rb4r;
		if (size < 2) size = 2;
		_THIS->buffer.resize(size);
		_THIS->size = size;
	}
#undef _THIS

	struct DecoderBase {
		DecoderBase() = default;
		smp<AVFormatContext> fmt{ nullptr };
		smp<AVCodecContext> codecCtx{ nullptr };
		std::vector<section> sections;
		const AVCodec* decoder{ nullptr };
		int videoStreamIndex = -1;
		int width = 0, height = 0;
		AVRational timeBase;
		size_t durationUS;
		AVPixelFormat pixelFormat;
		std::thread* worker;
		bool forcedStop = false;
	};

	struct ConverterBase {
		smp<SwsContext> preprocessor{ nullptr };
		int width, height;
		std::thread* worker;
		bool forcedStop = false;
	};

	struct EncoderBase {
		smp<SwsContext> preprocessor{ nullptr };
		smp<AVFormatContext> fmt{ nullptr };
		smp<AVCodecContext> codecCtx{ nullptr };
		AVStream* videoStream{ nullptr };
		smp<AVFrame> rgbaFrame{ nullptr };
		smp<AVFrame> preprocessedFrame{ nullptr };
		smp<AVPacket> compressedFrame{ nullptr };
		std::vector<section> sections;
		const AVCodec* encoder{ nullptr };
	};

#define _THIS reinterpret_cast<DecoderBase*>(structure)
	VideoDecoder::VideoDecoder() { structure = new DecoderBase; }
	VideoDecoder::~VideoDecoder() { 
		if (_THIS->worker) { 
			_THIS->worker->join();
			delete _THIS->worker;
		}
		delete _THIS;
	}

	bool VideoDecoder::open(const char* fileName) {
		if (isOpened()) return false;
		_THIS->fmt = avformat_alloc_context();
		FMCALL(avformat_open_input(&_THIS->fmt.ptr, fileName, nullptr, nullptr));
		if (errorCode < 0) {
			LOGRAW(errstr("open"));
			return false;
		}
		LOGRAW(fileName, "opened");

		FMCALL(avformat_find_stream_info(_THIS->fmt, nullptr));
		if (errorCode < 0) {
			LOGRAW(errstr("find stream info"));
			return false;
		}
		LOGRAW("stream found");
		int vidStream = -1;
		for (int i = 0; i < _THIS->fmt->nb_streams; i++) {
			if (_THIS->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				vidStream = i;
				break;
			}
		}
		if (vidStream == -1) {
			LOGRAW(fileName, "does not seem to contain video stream");
			return false;
		}		
		_THIS->videoStreamIndex = vidStream;
		_THIS->decoder = avcodec_find_decoder(_THIS->fmt->streams[vidStream]->codecpar->codec_id);
		if (!_THIS->decoder) {
			LOGRAW("Failed to find video stream decoder");
			return false;
		}
		_THIS->codecCtx = avcodec_alloc_context3(_THIS->decoder);
		FMCALL(avcodec_parameters_to_context(_THIS->codecCtx, _THIS->fmt->streams[vidStream]->codecpar));
		if (errorCode < 0) {
			LOGRAW(errstr("codec context"));
			return false;
		}
		_THIS->timeBase = _THIS->codecCtx->time_base;
		if (!_THIS->timeBase.den || !_THIS->timeBase.num) { _THIS->timeBase = _THIS->fmt->streams[vidStream]->time_base; }
		_THIS->width = _THIS->fmt->streams[vidStream]->codecpar->width;
		_THIS->height = _THIS->fmt->streams[vidStream]->codecpar->height;
		_THIS->durationUS = _THIS->fmt->duration;
		_THIS->pixelFormat = _THIS->codecCtx->pix_fmt;
		return true;
	}

	bool VideoDecoder::isOpened() {
		return _THIS->width;
	}

	void VideoDecoder::terminate() {
		_THIS->forcedStop = true;
	}

	int64_t VideoDecoder::getTimeInMicro(int64_t t) {
		return t * AV_TIME_BASE * _THIS->timeBase.num / _THIS->timeBase.den;
	}

	std::unique_ptr<Converter> VideoDecoder::makeFormatConverter() {
		if (!isOpened()) return {};
		struct _cvt :Converter {};
		std::unique_ptr<Converter> ret = std::make_unique<_cvt>();
		auto base = new ConverterBase;
		base->preprocessor = sws_getContext(_THIS->width, _THIS->height, _THIS->pixelFormat, _THIS->width, _THIS->height, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
		base->width = _THIS->width;
		base->height = _THIS->height;
		ret->structure = base;
		return ret;
	}

	std::unique_ptr<VideoEncoder> VideoDecoder::makeEncoder(int w, int h) {
		if (!isOpened()) return {};
		struct _enc :VideoEncoder {};
		std::unique_ptr<VideoEncoder> ret = std::make_unique<_enc>();
		auto base = new EncoderBase;
		base->encoder = avcodec_find_encoder(_THIS->codecCtx->codec_id);
		base->codecCtx = avcodec_alloc_context3(base->encoder);
		base->codecCtx->bit_rate = _THIS->codecCtx->bit_rate * w * h / _THIS->width / _THIS->height;
		if (base->codecCtx->bit_rate == 0) {
			base->codecCtx->bit_rate = (int64_t)w * h * _THIS->codecCtx->framerate.num / _THIS->codecCtx->framerate.den;
		}
		base->codecCtx->width = w;
		base->codecCtx->height = h;
		base->codecCtx->time_base = _THIS->timeBase;
		base->codecCtx->framerate = _THIS->codecCtx->framerate;
		base->codecCtx->gop_size = 4;
		base->codecCtx->max_b_frames = 1;
		base->codecCtx->pix_fmt = _THIS->pixelFormat;

		base->rgbaFrame = av_frame_alloc();
		base->rgbaFrame->format = AV_PIX_FMT_RGBA;
		base->rgbaFrame->width = w;
		base->rgbaFrame->height = h;
		FMCALL(av_frame_get_buffer(base->rgbaFrame, 0));
		if (errorCode < 0) {
			LOGRAW(errstr("frame container allocation"));
		}

		if (_THIS->pixelFormat != AV_PIX_FMT_RGBA) {
			base->preprocessor = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, _THIS->pixelFormat, SWS_POINT, nullptr, nullptr, nullptr);
			base->preprocessedFrame = av_frame_alloc();
			base->preprocessedFrame->format = _THIS->pixelFormat;
			base->preprocessedFrame->width = w;
			base->preprocessedFrame->height = h;
			FMCALL(av_frame_get_buffer(base->preprocessedFrame, 0));
			if (errorCode < 0) {
				LOGRAW(errstr("frame container allocation"));
			}
		}

		base->compressedFrame = av_packet_alloc();
		
		ret->structure = base;
		return ret;
	}

	size_t VideoDecoder::getDuration() {
		if (!isOpened()) return 0;
		return _THIS->durationUS;
	}

	int VideoDecoder::getWidth() { return _THIS->width; }
	int VideoDecoder::getHeight() { return _THIS->height; }

	void VideoDecoder::start(RingBuffer4Frame* output, const std::vector<section>& sections, bool extraWorker) {
		if (!isOpened()) return;

		// validate sections (no overlapping / no start > end case)
		_THIS->sections = sections;
		if (_THIS->sections.empty()) {
			_THIS->sections.push_back(section{ 0,(int)_THIS->durationUS });
		}
		
		auto work = [this, output]() {
			auto outputRing = reinterpret_cast<_rb4f*>(output->structure);
			outputRing->init(_THIS->pixelFormat, _THIS->width, _THIS->height);
			FMCALL(avcodec_open2(_THIS->codecCtx.ptr, _THIS->decoder, nullptr));
			if (errorCode < 0) {
				LOGRAW(errstr("codec open"));
				return;
			}
			// decode time section
			smp<AVPacket> packet = av_packet_alloc();
			smp<AVFrame> frame = av_frame_alloc();
			for (section s : _THIS->sections) {
				int64_t sectionTick = s.start;
				avcodec_flush_buffers(_THIS->codecCtx);
				int err = av_seek_frame(_THIS->fmt, -1, s.start, AVSEEK_FLAG_BACKWARD);
				while (av_read_frame(_THIS->fmt, packet) == 0) {
					if (_THIS->forcedStop) return;
					if (packet->stream_index != _THIS->videoStreamIndex) {
						continue;
					}
					int err = avcodec_send_packet(_THIS->codecCtx, packet);
					if (err == AVERROR(EAGAIN)) {}
					else if (err == AVERROR_EOF) {
						LOGRAW("EOF detected", s.start, s.end);
						break;
					}
					else if (err) {
						av_make_error_string(errorString, sizeof(errorString), err);
						LOGRAW(errorString);
						break;
					}
					err = avcodec_receive_frame(_THIS->codecCtx, frame);
					if (err == AVERROR(EAGAIN)) { continue; }
					else if (err == AVERROR_EOF) {
						avcodec_flush_buffers(_THIS->codecCtx);
						break;
					}
					int64_t lowEnd = frame->pts;
					int64_t highEnd = lowEnd + frame->duration;
					if (getTimeInMicro(highEnd) < s.start) { continue; }
					else if (getTimeInMicro(lowEnd) > s.end) { break; }
					AVFrame* cloned = outputRing->get2Write();
					av_frame_copy(cloned, frame);
					av_frame_copy_props(cloned, frame);
					cloned->pts = getTimeInMicro(lowEnd);
					cloned->duration = getTimeInMicro(highEnd);
					outputRing->return2write();
					av_packet_unref(packet);
				}
			}
			outputRing->done = true;
		};

		if (extraWorker) { 
			_THIS->worker = new std::thread(work);
		}
		else { 
			work();
		}
	}

#undef _THIS

#define _THIS reinterpret_cast<ConverterBase*>(structure)
	Converter::~Converter() { 
		if (_THIS->worker) {
			_THIS->worker->join();
			delete _THIS->worker;
		}
		delete _THIS;
	}
	void Converter::start(RingBuffer4Frame* input, RingBuffer4Texture* output, bool linear, bool extraWorker) {
		auto work = [this, input, output, linear]() {
			auto irb = reinterpret_cast<_rb4f*>(input->structure);
			auto orb = reinterpret_cast<_rb4t*>(output->structure);
			orb->init(_THIS->width, _THIS->height, linear);
			int pitch = _THIS->width * 4;
			while (AVFrame* fr = irb->get2Read()) {
				YRGraphics::pStreamTexture& tex = orb->get2Write();
				tex->updateBy([this, pitch, fr](void* data, uint32_t) {
					uint8_t* castedData = (uint8_t*)data;
					sws_scale(_THIS->preprocessor, fr->data, fr->linesize, 0, fr->height, &castedData, &pitch);
				});
				irb->return2Read();
				orb->return2write();
			}
			orb->done = true;
		};
		if (extraWorker) {
			_THIS->worker = new std::thread(work);
		}
		else {
			work();
		}
	}

#undef _THIS

#define _THIS reinterpret_cast<FilterBase*>(structure)

	struct FilterBase {
		YRGraphics::RenderPass2Screen* scr;
		YRGraphics::Pipeline* pp;
		YRGraphics::pMesh mesh;
	};

	FrameFilter::FrameFilter(int w, int h) {
		structure = new FilterBase;
		{
			YRGraphics::RenderPassCreationOptions opts;
			_THIS->scr = YRGraphics::createRenderPass2Screen(0, 0, opts);
		}
		{
			YRGraphics::ShaderModuleCreationOptions opts;
			const uint32_t NULL3_VERT[276] = { 119734787,65536,851979,51,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,524303,0,4,1852399981,0,13,26,41,327752,11,0,11,0,327752,11,1,11,1,327752,11,2,11,3,327752,11,3,11,4,196679,11,2,262215,26,11,42,262215,41,30,0,131091,2,196641,3,2,196630,6,32,262167,7,6,4,262165,8,32,0,262187,8,9,1,262172,10,6,9,393246,11,7,6,10,10,262176,12,3,11,262203,12,13,3,262165,14,32,1,262187,14,15,0,262167,16,6,2,262187,8,17,3,262172,18,16,17,262187,6,19,3212836864,327724,16,20,19,19,262187,6,21,1077936128,327724,16,22,19,21,327724,16,23,21,19,393260,18,24,20,22,23,262176,25,1,14,262203,25,26,1,262176,28,7,18,262176,30,7,16,262187,6,33,0,262187,6,34,1065353216,262176,38,3,7,262176,40,3,16,262203,40,41,3,327724,16,42,33,33,262187,6,43,1073741824,327724,16,44,33,43,327724,16,45,43,33,393260,18,46,42,44,45,327734,2,4,0,3,131320,5,262203,28,29,7,262203,28,48,7,262205,14,27,26,196670,29,24,327745,30,31,29,27,262205,16,32,31,327761,6,35,32,0,327761,6,36,32,1,458832,7,37,35,36,33,34,327745,38,39,13,15,196670,39,37,196670,48,46,327745,30,49,48,27,262205,16,50,49,196670,41,50,65789,65592 };
			opts.size = sizeof(NULL3_VERT);
			opts.stage = YRGraphics::ShaderStage::VERTEX;
			opts.source = NULL3_VERT;
			auto vs = YRGraphics::createShader(0, opts);
			const uint32_t COPY_FRAG[119] = { 119734787,65536,851979,20,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,458767,4,4,1852399981,0,9,17,196624,4,7,262215,9,30,0,262215,13,34,0,262215,13,33,0,262215,17,30,0,131091,2,196641,3,2,196630,6,32,262167,7,6,4,262176,8,3,7,262203,8,9,3,589849,10,6,1,0,0,0,1,0,196635,11,10,262176,12,0,11,262203,12,13,0,262167,15,6,2,262176,16,1,15,262203,16,17,1,327734,2,4,0,3,131320,5,262205,11,14,13,262205,15,18,17,327767,7,19,14,18,196670,9,19,65789,65592 };
			opts.size = sizeof(COPY_FRAG);
			opts.stage = YRGraphics::ShaderStage::FRAGMENT;
			opts.source = COPY_FRAG;
			auto fs = YRGraphics::createShader(1, opts);
			YRGraphics::PipelineCreationOptions pco;
			pco.vertexShader = vs;
			pco.fragmentShader = fs;
			pco.pass2screen = _THIS->scr;
			pco.shaderResources.usePush = false;
			pco.shaderResources.pos0 = YRGraphics::ShaderResourceType::TEXTURE_1;
			_THIS->pp = YRGraphics::createPipeline(0, pco);
		}
		{
			_THIS->mesh = YRGraphics::createNullMesh(INT32_MIN, 3);
		}
	}

	void FrameFilter::onLoop(RingBuffer4Texture* input) {
		auto irb = reinterpret_cast<_rb4t*>(input->structure);
		if (!Input::isKeyDown(Input::KeyCode::space)) return;
		YRGraphics::pStreamTexture tx = irb->get2Read();
		if (!tx) {
			return;
		}
		_THIS->scr->start();
		_THIS->scr->bind(0, tx);
		_THIS->scr->invoke(_THIS->mesh);
		_THIS->scr->execute();
		_THIS->scr->wait();
		irb->return2Read();
	}

	FrameFilter::~FrameFilter() {
		delete _THIS;
	}

#undef _THIS

#define _THIS reinterpret_cast<EncoderBase*>(structure)
	
	void VideoEncoder::start(const char* fileName) {
		FMCALL(avformat_alloc_output_context2(&_THIS->fmt.ptr, nullptr, nullptr, fileName));
		if (errorCode < 0) {
			LOGRAW(errstr("video encoder start"));
			return;
		}
		_THIS->videoStream = avformat_new_stream(_THIS->fmt, _THIS->encoder);
		if (!_THIS->videoStream) {
			LOGRAW("Failed to add new stream");
			return;
		}
		FMCALL(avcodec_open2(_THIS->codecCtx, _THIS->encoder, nullptr));
		if (errorCode < 0) {
			LOGRAW(errstr("codec open"));
			return;
		}
		FMCALL(avformat_write_header(_THIS->fmt, nullptr));
		if (errorCode < 0) {
			LOGRAW(errstr("file header"));
			return;
		}
	}

	void VideoEncoder::push(const uint8_t* rgba, size_t duration) {
		if (!_THIS->fmt) {
			LOGRAW("You must start the encoder before pushing frame data");
			return;
		}
		AVFrame* pFrame{};
		std::memcpy(_THIS->rgbaFrame->data, rgba, _THIS->codecCtx->width * _THIS->codecCtx->height * 4);
		if (_THIS->preprocessor) {
			sws_scale(_THIS->preprocessor, _THIS->rgbaFrame->data, _THIS->rgbaFrame->linesize, 0, _THIS->rgbaFrame->height, _THIS->preprocessedFrame->data, _THIS->preprocessedFrame->linesize);
			pFrame = _THIS->preprocessedFrame;
		}
		else {
			pFrame = _THIS->rgbaFrame;
		}
		int err = avcodec_send_frame(_THIS->codecCtx, pFrame);
		if (err < 0) {
			LOGRAW("Failed to send frame");
			return;
		}
		err = avcodec_receive_packet(_THIS->codecCtx, _THIS->compressedFrame);
		if (err < 0) {
			LOGRAW("Failed to recieve packet");
			return;
		}
		_THIS->compressedFrame->stream_index = 0; // video index == 0(which I've just added)
		_THIS->compressedFrame->duration = duration;
		av_interleaved_write_frame(_THIS->fmt, _THIS->compressedFrame);
	}

	void VideoEncoder::end() {
		if (!_THIS->fmt) {
			LOGRAW("You must start the encoder before pushing frame data");
			return;
		}
		FMCALL(av_write_trailer(_THIS->fmt));
		if (errorCode < 0) {
			LOGRAW(errstr("file trailer"));
			return;
		}
		_THIS->fmt = {};
	}

#undef _THIS

}