#include "logger.hpp"
#include "yr_sys.h"
#include "yr_graphics.h"
#include "yr_game.h"

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

    onart::Game game;
    game.start();
}