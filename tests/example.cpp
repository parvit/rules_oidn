/*
 *  SPDX-FileCopyrightText: Copyright 2023 Julian Amann <dev@vertexwahn.de>
 *  SPDX-License-Identifier: Apache-2.0
 */

#include "OpenImageDenoise/oidn.hpp"

#include "src/lib/OpenEXR/ImfArray.h"
#include "src/lib/OpenEXR/ImfChannelList.h"
#include "src/lib/OpenEXR/ImfIO.h"
#include "src/lib/OpenEXR/ImfInputFile.h"
#include "src/lib/OpenEXR/ImfOutputFile.h"
#include "src/lib/OpenEXR/ImfRgbaFile.h"
#include "src/lib/OpenEXR/ImfStringAttribute.h"

using namespace Imf;
using namespace Imath;

#include <cassert>
#include <cfloat>
#include <iostream>

using namespace std;

class Image3f {
public:
    Image3f(const int width, const int height) : width_(width), height_(height) {
        data_ = new float[width_ * height_ * 3];

        for (int x = 0; x < width_; ++x) {
            for (int y = 0; y < height_; ++y) {
                auto r = 0.f;
                auto g = 0.f;
                auto b = 0.f;

                data_[(width_ * y + x) * 3] = r;
                data_[(width_ * y + x) * 3 + 1] = g;
                data_[(width_ * y + x) * 3 + 2] = b;
            }
        }
    }

    void set_pixel(int x, int y, float red, float green, float blue) {
        assert(x >= 0);
        assert(y >= 0);
        assert(x < width_);
        assert(y < height_);

        data_[(width_ * y + x) * 3] = red;
        data_[(width_ * y + x) * 3 + 1] = green;
        data_[(width_ * y + x) * 3 + 2] = blue;
    }

    float *data() {
        return data_;
    }

    int width() const {
        return width_;
    }

    int height() const {
        return height_;
    }

private:
    int width_ = 0;
    int height_ = 0;
    float *data_;
};

Image3f load_image_openexr(std::string_view filename) {
    // see https://www.openexr.com/documentation/ReadingAndWritingImageFiles.pdf
    // Heading Reading an Image File
    InputFile file(filename.data());
    const Header &header = file.header();

    Box2i dw = header.dataWindow();
    int width = dw.max.x - dw.min.x + 1;
    int height = dw.max.y - dw.min.y + 1;

    /*
    bool hasRed = false;
    bool hasGreen = false;
    bool hasBlue = false;

    for (ChannelList::ConstIterator it = header.channels().begin(), ite = header.channels().end(); it != ite; it++) {
        if ((strcmp(it.name(), "R") == 0)) { hasRed = true; }
        if ((strcmp(it.name(), "G") == 0)) { hasGreen = true; }
        if ((strcmp(it.name(), "B") == 0)) { hasBlue = true; }
        if (it.channel().type != HALF) {
            //HDR_LOG("Unable to open EXR file \"%s\" (unsupported data type %s)", filename, it.channel().type);
            //return (IEFileCantOpen);
        }
    }
    */

    Imf::Array2D<float> rPixels;
    Imf::Array2D<float> gPixels;
    Imf::Array2D<float> bPixels;

    rPixels.resizeErase(height, width);
    gPixels.resizeErase(height, width);
    bPixels.resizeErase(height, width);

    FrameBuffer frameBuffer;

    frameBuffer.insert("R", // name
                       Slice(FLOAT, // type
                             (char *) (&rPixels[0][0] - // base
                                       dw.min.x -
                                       dw.min.y * width),
                             sizeof(rPixels[0][0]) * 1, // xStride
                             sizeof(rPixels[0][0]) * width,// yStride
                             1, 1, // x/y sampling
                             FLT_MAX)); // fillValue

    frameBuffer.insert("G", // name
                       Slice(FLOAT, // type
                             (char *) (&gPixels[0][0] - // base
                                       dw.min.x -
                                       dw.min.y * width),
                             sizeof(gPixels[0][0]) * 1, // xStride
                             sizeof(gPixels[0][0]) * width,// yStride
                             1, 1, // x/y sampling
                             FLT_MAX)); // fillValue

    frameBuffer.insert("B", // name
                       Slice(FLOAT, // type
                             (char *) (&bPixels[0][0] - // base
                                       dw.min.x -
                                       dw.min.y * width),
                             sizeof(bPixels[0][0]) * 1, // xStride
                             sizeof(bPixels[0][0]) * width,// yStride
                             1, 1, // x/y sampling
                             FLT_MAX)); // fillValue

    file.setFrameBuffer(frameBuffer);
    file.readPixels(dw.min.y, dw.max.y);

    Image3f img{width, height};

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            img.set_pixel(x, y, rPixels[y][x], gPixels[y][x], bPixels[y][x]);
        }
    }
    return img;
}

int main() {
    cout << "Simple denoising example" << endl;

    Image3f noisy = load_image_openexr("data/noisy_10spp.exr");
    Image3f normal = load_image_openexr("data/normal_10spp.exr");
    Image3f albedo = load_image_openexr("data/albedo_10spp.exr");
    Image3f out{noisy.width(), noisy.height()};

    float* colorPtr = noisy.data();
    float* albedoPtr = albedo.data();
    float* normalPtr = normal.data();
    float* outputPtr = out.data();
    int width = out.width();
    int height = out.height();

    oidn::DeviceRef device = oidn::newDevice();
    device.commit();

    // Create a filter for denoising a beauty (color) image using optional auxiliary images too
    oidn::FilterRef filter = device.newFilter("RT"); // generic ray tracing filter
    filter.setImage("color",  colorPtr,  oidn::Format::Float3, width, height); // beauty
    filter.setImage("albedo", albedoPtr, oidn::Format::Float3, width, height); // auxiliary
    filter.setImage("normal", normalPtr, oidn::Format::Float3, width, height); // auxiliary
    filter.setImage("output", outputPtr, oidn::Format::Float3, width, height); // denoised beauty
    filter.set("hdr", true); // beauty image is HDR
    filter.commit();

    // Filter the image
    filter.execute();

    // Check for errors
    const char* errorMessage;
    if (device.getError(errorMessage) != oidn::Error::None) {
        std::cout << "Error: " << errorMessage << std::endl;
    }

    return 0;
}
