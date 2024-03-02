/*
* LICENSE: GPL v3.0
*/
#ifndef __FMP_H__
#define __FMP_H__

#include <vector>
#include <thread>

namespace onart {

	// 1:1 thread safe
	class PartiteLinkedList {
	public:
		struct Node {
			void* data;
			Node* next;
			inline void* operator new(size_t size) { return ::operator new(size); }
			inline void operator delete(void* p) { return ::operator delete(p); }
		};
		inline uint64_t load() const { return total - taken; }
		inline PartiteLinkedList() :taken(0), total(0) {
			tail = new Node{};
			head = new Node{ nullptr, tail };
		}
		inline ~PartiteLinkedList() {
			while (head) {
				Node* h = head;
				head = head->next;
				delete h;
			}
		}
		inline void pushEnd() { push(this); total++; }
		inline void push(void* elem) volatile {
			Node* oldTail = tail;
			Node* newNode = new Node{ elem, nullptr };
			tail = newNode;
			oldTail->next = newNode;
			total++;
		}
		inline void* pop1() volatile {
			if (total == taken) return nullptr;
			taken++;
			void* ret = head->data;
			Node* h = head;
			head = head->next;
			delete h;
			return ret;
		}
		inline void pushNode(Node* n) volatile {
			Node* oldTail = tail;
			Node* newNode = n;
			tail = newNode;
			oldTail->next = newNode;
			total++;
		}
		inline Node* pop1Node() volatile {
			if (total == taken) return nullptr;
			taken++;
			Node* h = head;
			head = head->next;
			return h;
		}
	private:
		Node* head;
		Node* tail;
		uint64_t taken, total;
		bool complete;
	};

	class Funnel {
	public:
		Funnel(size_t inputCount, size_t outputCount);
		~Funnel();
		// For writers
		inline void push(void* p, int idx) { if (p) inputs[idx].push(p); else inputs[idx].pushEnd(); }
	public:
		// For readers
		void* pick(int idx);
		inline bool isInputEnd() { return !b; }
	public:
		size_t inThreadCount() { return inputs.size(); }
		size_t outThreadCount() { return outputs.size(); }
	private:
		std::vector<PartiteLinkedList> inputs;
		std::vector<PartiteLinkedList> outputs;
		std::thread* transfer;
		bool b;
	};

	class VideoFilter;
	class FileVideoEncoder;

	struct section { int32_t start, end; };

	class VideoEncoder {
	public:
	private:
		void* structure;
	};

	class FilterSet {

	};

	class Converter {
		friend class VideoDecoder;
	public:
		~Converter();
		void start(Funnel* input, Funnel* output);
	private:
		Converter() = default;
		void* structure;
	};

	class VideoDecoder {
	public:
		VideoDecoder();
		~VideoDecoder();
		Converter makeFormatConverter();
		VideoEncoder makeEncoder(int w, int h);
		bool open(const char* fileName, size_t threadCount);
		void start(Funnel* output, const std::vector<section>& sections = {});
		void join();
		//bool startMem(void* videoInMem, Funnel* output);
	public:
		size_t getDuration();
		int getWidth();
		int getHeight();
	private:
		bool isOpened();
		void* structure;
	};
}

#endif