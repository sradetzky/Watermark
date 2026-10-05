#include "cli/common.h"

int wmain(int argc, wchar_t** argv) {
    return watermark::cli::run_embed(argc, argv);
}
