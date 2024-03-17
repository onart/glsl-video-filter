/*
* LICENSE: GPL v3.0
*/
#ifndef __FMP_H__
#define __FMP_H__

#include <vector>
#include <thread>

namespace onart {

	class RingBuffer4Frame {
		friend class VideoDecoder;
		friend class Converter;
		friend class VideoEncoder;
	public:
		RingBuffer4Frame(size_t bufferLength = 2);
		~RingBuffer4Frame();
		size_t load();
	private:
		void* structure;
	};

	class RingBuffer4Texture {
		friend class Converter;
		friend class FrameFilter;
	public:
		RingBuffer4Texture(size_t bufferLength = 2);
		~RingBuffer4Texture();
		size_t load();
	private:
		void* structure;
	};

	class RingBuffer4RGBA {
		friend class FrameFilter;
		friend class Converter;
	public:
		RingBuffer4RGBA(size_t bufferLength = 2);
		~RingBuffer4RGBA();
		size_t load();
	private:
		void* structure;
	};

	class VideoFilter;
	class FileVideoEncoder;

	// in microseconds
	struct section { int32_t start, end; };

	class VideoEncoder {
		friend class VideoDecoder;
	public:
	private:
		void* structure;
	};

	class FrameFilter { // юс╫ц
	public:
		FrameFilter(int width, int height);
		~FrameFilter();
		void onLoop(RingBuffer4Texture* input);
	private:
		void* structure;
	};

	class FilterSet {
	public:
	private:
	};

	class Converter {
		friend class VideoDecoder;
	public:
		Converter(const Converter&) = delete;
		~Converter();
		void start(RingBuffer4Frame* input, RingBuffer4Texture* output, bool minmagLinear = true, bool extraWorker = true);
		void start(RingBuffer4Texture* input, RingBuffer4Frame* output, bool extraWorker = true);
	private:
		Converter() = default;
		void* structure;
	};

	class VideoDecoder {
	public:
		VideoDecoder();
		~VideoDecoder();
		std::unique_ptr<Converter> makeFormatConverter();
		std::unique_ptr<VideoEncoder> makeEncoder(int w, int h);
		bool open(const char* fileName);
		void start(RingBuffer4Frame* output, const std::vector<section>& sections = {}, bool extraWorker = true);
		void terminate();
		size_t load();
	public:
		size_t getDuration();
		int getWidth();
		int getHeight();
	private:
		bool isOpened();
		int64_t getTimeInMicro(int64_t);
		void* structure;
	};
}

#endif