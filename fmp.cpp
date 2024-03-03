#include "fmp.h"
#include <list>

extern "C" {
	#include "YERM/externals/ffmpeg/include/libavformat/avformat.h"
	#include "YERM/externals/ffmpeg/include/libavcodec/avcodec.h"
	#include "YERM/externals/ffmpeg/include/libswscale/swscale.h"
	#include "YERM/externals/ffmpeg/include/libavutil/imgutils.h"
}

#include "YERM/YERM_PC/yr_graphics.h"

#include "YERM/YERM_PC/logger.hpp"

namespace onart {

	Funnel::Funnel(size_t inputCount, size_t outputCount) :inputs(inputCount), outputs(outputCount) {
		transfer = new std::thread([this, inputCount, outputCount]() {
			std::vector<PartiteLinkedList::Node*> inter;
			int output = 0;
			int ic = inputCount;
			while (b) {
				// read
				for (auto& in : inputs) {
					while (auto nd = in.pop1Node()) {
						if (nd->data == &in) {
							ic--;
							break;
						}
						if (nd->data) {
							inter.push_back(nd);
						}
					}
				}
				// write
				for (auto n : inter) {
					auto& q = outputs[output++];
					q.pushNode(n);
					if (output >= outputCount) output = 0;
				}
				if (ic == 0) { 
					for (auto& o : outputs) { o.pushEnd(); }
					b = false;
					break;
				}
				inter.clear();
			}
		});
	}

	Funnel::~Funnel() {
		b = false;
		if (transfer) {
			transfer->join();
			delete transfer;
			transfer = 0;
		}
	}

	void* Funnel::pick(int idx) {
		return outputs[idx].pop1();
	}

	void Funnel::join() {
		transfer->join();
		delete transfer;
		transfer = 0;
	}

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

	struct FFThreadContext {
		smp<AVFormatContext> fmt{ nullptr };
		smp<AVCodecContext> codecCtx{ nullptr };
		std::vector<section> sections;
	};

	struct DecoderBase {
		DecoderBase() = default;
		std::vector<FFThreadContext> ctx;
		const AVCodec* decoder{ nullptr };
		int videoStreamIndex = -1;
		int width, height;
		AVRational timeBase;
		size_t durationUS;
		AVPixelFormat pixelFormat;
		std::vector<std::thread> workers;
	};

	struct ConverterBase {
		smp<SwsContext> preprocessor{ nullptr };
		int width, height;
		std::vector<std::thread> workers;
	};

	struct EncoderBase {
		smp<SwsContext> preprocessor{ nullptr };
		FFThreadContext ctx;
		const AVCodec* encoder{ nullptr };
	};

#define _THIS reinterpret_cast<DecoderBase*>(structure)
	VideoDecoder::VideoDecoder() { structure = new DecoderBase; }
	VideoDecoder::~VideoDecoder() { 
		for (auto& th : _THIS->workers) { th.join(); }
		_THIS->workers.clear();
		delete _THIS;
	}

	bool VideoDecoder::open(const char* fileName, size_t threadCount) {
		if (isOpened()) return false;
		if (threadCount == 0) threadCount = 1;
		_THIS->ctx.resize(threadCount);
		for (size_t i = 0; i < threadCount; i++) {
			auto& fmt = _THIS->ctx[i].fmt = avformat_alloc_context();
			FMCALL(avformat_open_input(&fmt.ptr, fileName, nullptr, nullptr));
			if (errorCode < 0) {
				LOGRAW(errstr("open"));
				return false;
			}
		}
		LOGRAW(fileName, "opened");
		for (size_t i = 0; i < threadCount; i++) {
			auto& fmt = _THIS->ctx[i].fmt;
			FMCALL(avformat_find_stream_info(fmt, nullptr));
			if (errorCode < 0) {
				LOGRAW(errstr("find stream info"));
				return false;
			}
		}
		LOGRAW("stream found");
		int vidStream = -1;
		for (int i = 0; i < _THIS->ctx[0].fmt->nb_streams; i++) {
			if (_THIS->ctx[0].fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				vidStream = i;
				break;
			}
		}
		if (vidStream == -1) {
			LOGRAW(fileName, "does not seem to contain video stream");
			return false;
		}		
		_THIS->videoStreamIndex = vidStream;
		_THIS->decoder = avcodec_find_decoder(_THIS->ctx[0].fmt->streams[vidStream]->codecpar->codec_id);
		if (!_THIS->decoder) {
			LOGRAW("Failed to find video stream decoder");
			return false;
		}
		for (size_t i = 0; i < threadCount; i++) {
			auto& cdc = _THIS->ctx[i].codecCtx = avcodec_alloc_context3(_THIS->decoder);
			if (!cdc) {
				LOGRAW("Failed to allocate context");
				return false;
			}
			FMCALL(avcodec_parameters_to_context(cdc, _THIS->ctx[0].fmt->streams[vidStream]->codecpar));
			if (errorCode < 0) {
				LOGRAW(errstr("codec context"));
				return false;
			}
		}
		_THIS->timeBase = _THIS->ctx[0].codecCtx->time_base;
		if (!_THIS->timeBase.den || !_THIS->timeBase.num) { _THIS->timeBase = _THIS->ctx[0].fmt->streams[vidStream]->time_base; }
		_THIS->width = _THIS->ctx[0].fmt->streams[vidStream]->codecpar->width;
		_THIS->height = _THIS->ctx[0].fmt->streams[vidStream]->codecpar->height;
		_THIS->durationUS = _THIS->ctx[0].fmt->duration;
		_THIS->pixelFormat = _THIS->ctx[0].codecCtx->pix_fmt;
		return true;
	}

	bool VideoDecoder::isOpened() {
		return _THIS->ctx.size();
	}

	int64_t VideoDecoder::getTimeInMicro(int64_t t) {
		return t * AV_TIME_BASE * _THIS->timeBase.num / _THIS->timeBase.den;
	}

	std::unique_ptr<Converter> VideoDecoder::makeFormatConverter() {
		if (!isOpened()) return {};
		struct _cvt :Converter {};
		std::unique_ptr<Converter> ret = std::make_unique<_cvt>();
		auto base = new ConverterBase;
		base->preprocessor = sws_getContext(_THIS->width, _THIS->height, _THIS->pixelFormat, _THIS->width, _THIS->height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
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
		base->encoder = avcodec_find_encoder(_THIS->ctx[0].codecCtx->codec_id);
		base->preprocessor = sws_getContext(w, h, AV_PIX_FMT_RGB32, w, h, _THIS->pixelFormat, SWS_POINT, nullptr, nullptr, nullptr);
		ret->structure = base;
		return ret;
	}

	size_t VideoDecoder::getDuration() {
		if (!isOpened()) return 0;
		return _THIS->durationUS;
	}

	int VideoDecoder::getWidth() { return _THIS->width; }
	int VideoDecoder::getHeight() { return _THIS->height; }

	void VideoDecoder::start(Funnel* output, const std::vector<section>& sections) {
		if (!isOpened()) return;
		if (output->inThreadCount() != _THIS->ctx.size()) {
			LOGRAW("Invalid thread count. Match it with funnel input");
			return;
		}

		// validate sections (no overlapping / no start > end case)


		// distribute time sections
		int32_t totalTime = 0;
		for (const section& s : sections) { totalTime += s.end - s.start; }
		if (totalTime == 0) totalTime = getDuration();
		int32_t slice = totalTime / _THIS->ctx.size();
		section wholeRange{ 0, totalTime };
		
		int i = 0;
		int32_t inputSectionIndex = 0;
		int32_t startTime = sections.size() ? sections[0].start : 0;
		for (auto& ctx : _THIS->ctx) {
			int32_t restSlice = slice;
			if (i != _THIS->ctx.size() - 1) {
				while (restSlice > 0) {
					section& next = ctx.sections.emplace_back();
					next.start = startTime;
					while (true) {
						const section& inputNext = sections.size() ? sections[inputSectionIndex] : wholeRange;
						if (restSlice > inputNext.end - startTime) {
							next.end = inputNext.end;
							restSlice -= inputNext.end - startTime;
							inputSectionIndex++;
							startTime = sections[inputSectionIndex].start;
						}
						else {
							startTime += restSlice;
							restSlice = 0;
							break;
						}
					}
				}
			}
			else {
				if (sections.size() == 0) { ctx.sections.emplace_back(section{ startTime, wholeRange.end }); }
				else {
					ctx.sections.emplace_back(section{ startTime, sections[inputSectionIndex++].end });
					for (; inputSectionIndex < sections.size(); inputSectionIndex++) { ctx.sections.push_back(sections[inputSectionIndex]); }
				}
			}
			_THIS->workers.emplace_back([this, output, i]() {
				auto& ctx = _THIS->ctx[i];
				FMCALL(avcodec_open2(ctx.codecCtx.ptr, _THIS->decoder, nullptr));
				if (errorCode < 0) {
					LOGRAW(errstr("codec open"));
					return;
				}
				// decode time section
				smp<AVPacket> packet = av_packet_alloc();
				smp<AVFrame> frame = av_frame_alloc();
				for (section s : ctx.sections) {
					int64_t sectionTick = s.start;
					avcodec_flush_buffers(ctx.codecCtx);
					int err = av_seek_frame(ctx.fmt, -1, s.start, AVSEEK_FLAG_BACKWARD);
					while (av_read_frame(ctx.fmt, packet) == 0) {
						if (packet->stream_index != _THIS->videoStreamIndex) continue;
						int err = avcodec_send_packet(ctx.codecCtx, packet);
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
						err = avcodec_receive_frame(ctx.codecCtx, frame);
						if (err == AVERROR(EAGAIN)) { continue; }
						else if (err == AVERROR_EOF) {
							avcodec_flush_buffers(ctx.codecCtx);
							break;
						}
						int64_t lowEnd = frame->pts;
						int64_t highEnd = lowEnd + frame->duration;
						if (getTimeInMicro(highEnd) < s.start) { continue; }
						else if (getTimeInMicro(lowEnd) > s.end) { break; }
						AVFrame* cloned = av_frame_alloc();
						cloned->format = frame->format;
						cloned->width = frame->width;
						cloned->height = frame->height;
						cloned->channels = frame->channels;
						cloned->channel_layout = frame->channel_layout;
						cloned->nb_samples = frame->nb_samples;
						av_frame_get_buffer(cloned, 0); // align 32?
						av_frame_copy(cloned, frame);
						av_frame_copy_props(cloned, frame);
						cloned->pts = getTimeInMicro(lowEnd);
						cloned->duration = getTimeInMicro(highEnd);
						output->push(cloned, i);
						av_packet_unref(packet);
					}
				}
				output->push(nullptr, i);
			});
			i++;
		}
	}

#undef _THIS

#define _THIS reinterpret_cast<ConverterBase*>(structure)
	Converter::~Converter() { 
		for (auto& th : _THIS->workers) th.join();
		delete _THIS;
	}
	void Converter::start(Funnel* input, Funnel* output) {
		for (size_t i = 0; i < input->outThreadCount(); i++) {
			_THIS->workers.emplace_back([this, input, output, i]() {
				while (!input->isInputEnd()) {
					AVFrame* frame = (AVFrame*)input->pick(i);
					if (!frame) continue;
					int pitch = _THIS->width * 4;
					uint8_t* data = new uint8_t[_THIS->height * pitch];
					sws_scale(_THIS->preprocessor, frame->data, frame->linesize, 0, frame->height, &data, &pitch);
					output->push(data, i);
				}
				output->push(nullptr, i);
			});
		}
	}

#undef _THIS

}