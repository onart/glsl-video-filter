#include "logger.hpp"
#include "yr_sys.h"
#include "yr_graphics.h"

#include "../../fmp.h"

#include <filesystem>
#include <cstdlib>
#include <vector>

// for check decoded frame
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../stb_image_write.h"

int main(int argc, char* argv[]){
#if BOOST_OS_WINDOWS
    system("chcp 65001");
#endif
    std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());
    /*
    if (argc < 4) {
        LOGRAW("usage:", argv[0], "input.mp4 filter.frag output.mp4");
        return 0;
    }
    */

    onart::VideoDecoder decoder;
    decoder.open("dbg.mp4", 1);
    int width = decoder.getWidth();
    int height = decoder.getHeight();
    auto duration = decoder.getDuration();
    auto converter = decoder.makeFormatConverter();
    onart::Funnel funnel1(1, 1);
    onart::Funnel funnel2(1, 1);
    decoder.start(&funnel1, { onart::section{0, 1000000} });
    converter->start(&funnel1, &funnel2);
    funnel2.join();
    funnel2.pick(0);
    funnel2.pick(0);
    uint8_t* image = (uint8_t*)funnel2.pick(0);
    if (image) {
        stbi_write_png("aa.png", width, height, 4, image, 0);
    }

    onart::Window::init();
    onart::YRGraphics* yg = new onart::YRGraphics;
    
    delete yg;
    onart::Window::terminate();
}