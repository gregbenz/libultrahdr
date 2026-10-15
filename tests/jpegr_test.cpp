/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
 * https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
 * <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
 * option. This file may not be copied, modified, or distributed
 * except according to those terms.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ultrahdr_api.h"

#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/jpegr.h"
#include "ultrahdr/jpegrutils.h"
#include "ultrahdr/gainmapmetadata.h"
#include "ultrahdr/gainmapmath.h"

//#define DUMP_OUTPUT

namespace ultrahdr {

// resources used by unit tests
#ifdef __ANDROID__
const char* kYCbCrP010FileName = "/data/local/tmp/raw_p010_image.p010";
const char* kYCbCr420FileName = "/data/local/tmp/raw_yuv420_image.yuv420";
const char* kSdrJpgFileName = "/data/local/tmp/jpeg_image.jpg";
const char* kOldAppleFileName = "/data/local/tmp/apple_gainmap_old.jpg";
const char* kNewAppleFileName = "/data/local/tmp/apple_gainmap_new.jpg";
#else
const char* kYCbCrP010FileName = "raw_p010_image.p010";
const char* kYCbCr420FileName = "raw_yuv420_image.yuv420";
const char* kSdrJpgFileName = "jpeg_image.jpg";
const char* kOldAppleFileName = "apple_gainmap_old.jpg";
const char* kNewAppleFileName = "apple_gainmap_new.jpg";
#endif
const size_t kImageWidth = 1280;
const size_t kImageHeight = 720;
const int kQuality = 90;

// Wrapper to describe the input type
typedef enum {
  YCbCr_p010 = 0,
  YCbCr_420 = 1,
} UhdrInputFormat;

/**
 * Wrapper class for raw resource
 * Sample usage:
 *   UhdrUnCompressedStructWrapper rawImg(width, height, YCbCr_p010);
 *   rawImg.setImageColorGamut(colorGamut));
 *   rawImg.setImageStride(strideLuma, strideChroma); // optional
 *   rawImg.setChromaMode(false); // optional
 *   rawImg.allocateMemory();
 *   rawImg.loadRawResource(kYCbCrP010FileName);
 */
class UhdrUnCompressedStructWrapper {
 public:
  UhdrUnCompressedStructWrapper(unsigned int width, unsigned int height, UhdrInputFormat format);
  ~UhdrUnCompressedStructWrapper() = default;

  bool setChromaMode(bool isChromaContiguous);
  bool setImageStride(unsigned int lumaStride, unsigned int chromaStride);
  bool setImageColorGamut(ultrahdr_color_gamut colorGamut);
  bool allocateMemory();
  bool loadRawResource(const char* fileName);
  jr_uncompressed_ptr getImageHandle();

 private:
  std::unique_ptr<uint8_t[]> mLumaData;
  std::unique_ptr<uint8_t[]> mChromaData;
  jpegr_uncompressed_struct mImg;
  UhdrInputFormat mFormat;
  bool mIsChromaContiguous;
};

/**
 * Wrapper class for compressed resource
 * Sample usage:
 *   UhdrCompressedStructWrapper jpgImg(width, height);
 *   rawImg.allocateMemory();
 */
class UhdrCompressedStructWrapper {
 public:
  UhdrCompressedStructWrapper(unsigned int width, unsigned int height);
  ~UhdrCompressedStructWrapper() = default;

  bool allocateMemory();
  jr_compressed_ptr getImageHandle();

 private:
  std::unique_ptr<uint8_t[]> mData;
  jpegr_compressed_struct mImg{};
  unsigned int mWidth;
  unsigned int mHeight;
};

UhdrUnCompressedStructWrapper::UhdrUnCompressedStructWrapper(unsigned int width,
                                                             unsigned int height,
                                                             UhdrInputFormat format) {
  mImg.data = nullptr;
  mImg.width = width;
  mImg.height = height;
  mImg.colorGamut = ULTRAHDR_COLORGAMUT_UNSPECIFIED;
  mImg.chroma_data = nullptr;
  mImg.luma_stride = 0;
  mImg.chroma_stride = 0;
  mFormat = format;
  mIsChromaContiguous = true;
}

bool UhdrUnCompressedStructWrapper::setChromaMode(bool isChromaContiguous) {
  if (mLumaData.get() != nullptr) {
    std::cerr << "Object has sailed, no further modifications are allowed" << std::endl;
    return false;
  }
  mIsChromaContiguous = isChromaContiguous;
  return true;
}

bool UhdrUnCompressedStructWrapper::setImageStride(unsigned int lumaStride,
                                                   unsigned int chromaStride) {
  if (mLumaData.get() != nullptr) {
    std::cerr << "Object has sailed, no further modifications are allowed" << std::endl;
    return false;
  }
  if (lumaStride != 0) {
    if (lumaStride < mImg.width) {
      std::cerr << "Bad luma stride received" << std::endl;
      return false;
    }
    mImg.luma_stride = lumaStride;
  }
  if (chromaStride != 0) {
    if (mFormat == YCbCr_p010 && chromaStride < mImg.width) {
      std::cerr << "Bad chroma stride received for format YCbCrP010" << std::endl;
      return false;
    }
    if (mFormat == YCbCr_420 && chromaStride < (mImg.width >> 1)) {
      std::cerr << "Bad chroma stride received for format YCbCr420" << std::endl;
      return false;
    }
    mImg.chroma_stride = chromaStride;
  }
  return true;
}

bool UhdrUnCompressedStructWrapper::setImageColorGamut(ultrahdr_color_gamut colorGamut) {
  if (mLumaData.get() != nullptr) {
    std::cerr << "Object has sailed, no further modifications are allowed" << std::endl;
    return false;
  }
  mImg.colorGamut = colorGamut;
  return true;
}

bool UhdrUnCompressedStructWrapper::allocateMemory() {
  if (mImg.width == 0 || (mImg.width % 2 != 0) || mImg.height == 0 || (mImg.height % 2 != 0) ||
      (mFormat != YCbCr_p010 && mFormat != YCbCr_420)) {
    std::cerr << "Object in bad state, mem alloc failed" << std::endl;
    return false;
  }
  int lumaStride = mImg.luma_stride == 0 ? mImg.width : mImg.luma_stride;
  int lumaSize = lumaStride * mImg.height * (mFormat == YCbCr_p010 ? 2 : 1);
  int chromaSize = (mImg.height >> 1) * (mFormat == YCbCr_p010 ? 2 : 1);
  if (mIsChromaContiguous) {
    chromaSize *= lumaStride;
  } else {
    if (mImg.chroma_stride == 0) {
      std::cerr << "Object in bad state, mem alloc failed" << std::endl;
      return false;
    }
    if (mFormat == YCbCr_p010) {
      chromaSize *= mImg.chroma_stride;
    } else {
      chromaSize *= (mImg.chroma_stride * 2);
    }
  }
  if (mIsChromaContiguous) {
    mLumaData = std::make_unique<uint8_t[]>(lumaSize + chromaSize);
    mImg.data = mLumaData.get();
    mImg.chroma_data = nullptr;
  } else {
    mLumaData = std::make_unique<uint8_t[]>(lumaSize);
    mImg.data = mLumaData.get();
    mChromaData = std::make_unique<uint8_t[]>(chromaSize);
    mImg.chroma_data = mChromaData.get();
  }
  return true;
}

static bool openFileHelper(const char* filename, std::ifstream& ifd,
                           std::ios_base::openmode mode = std::ios::binary | std::ios::ate) {
  std::vector<std::string> candidates = {
      filename,
      std::string("tests/data/") + filename,
      std::string("./data/") + filename,
      std::string("data/") + filename,
      std::string("../tests/data/") + filename,
      std::string("third_party/libultrahdr/tests/data/") + filename,
  };
  for (const auto& path : candidates) {
    ifd.open(path, mode);
    if (ifd.good()) return true;
    ifd.clear();
  }
  return false;
}

bool UhdrUnCompressedStructWrapper::loadRawResource(const char* fileName) {
  if (!mImg.data) {
    std::cerr << "memory is not allocated, read not possible" << std::endl;
    return false;
  }
  std::ifstream ifd;
  if (openFileHelper(fileName, ifd, std::ios::binary | std::ios::ate)) {
    int bpp = mFormat == YCbCr_p010 ? 2 : 1;
    int size = ifd.tellg();
    int length = mImg.width * mImg.height * bpp * 3 / 2;  // 2x2 subsampling
    if (size < length) {
      std::cerr << "requested to read " << length << " bytes from file : " << fileName
                << ", file contains only " << length << " bytes" << std::endl;
      return false;
    }
    ifd.seekg(0, std::ios::beg);
    size_t lumaStride = mImg.luma_stride == 0 ? mImg.width : mImg.luma_stride;
    char* mem = static_cast<char*>(mImg.data);
    for (size_t i = 0; i < mImg.height; i++) {
      ifd.read(mem, mImg.width * bpp);
      mem += lumaStride * bpp;
    }
    if (!mIsChromaContiguous) {
      mem = static_cast<char*>(mImg.chroma_data);
    }
    size_t chromaStride;
    if (mIsChromaContiguous) {
      chromaStride = mFormat == YCbCr_p010 ? lumaStride : lumaStride / 2;
    } else {
      if (mFormat == YCbCr_p010) {
        chromaStride = mImg.chroma_stride == 0 ? lumaStride : mImg.chroma_stride;
      } else {
        chromaStride = mImg.chroma_stride == 0 ? (lumaStride / 2) : mImg.chroma_stride;
      }
    }
    if (mFormat == YCbCr_p010) {
      for (size_t i = 0; i < mImg.height / 2; i++) {
        ifd.read(mem, mImg.width * 2);
        mem += chromaStride * 2;
      }
    } else {
      for (size_t i = 0; i < mImg.height / 2; i++) {
        ifd.read(mem, (mImg.width / 2));
        mem += chromaStride;
      }
      for (size_t i = 0; i < mImg.height / 2; i++) {
        ifd.read(mem, (mImg.width / 2));
        mem += chromaStride;
      }
    }
    return true;
  }
  std::cerr << "unable to open file : " << fileName << std::endl;
  return false;
}

jr_uncompressed_ptr UhdrUnCompressedStructWrapper::getImageHandle() { return &mImg; }

UhdrCompressedStructWrapper::UhdrCompressedStructWrapper(unsigned int width, unsigned int height) {
  mWidth = width;
  mHeight = height;
}

bool UhdrCompressedStructWrapper::allocateMemory() {
  if (mWidth == 0 || (mWidth % 2 != 0) || mHeight == 0 || (mHeight % 2 != 0)) {
    std::cerr << "Object in bad state, mem alloc failed" << std::endl;
    return false;
  }
  int maxLength = (std::max)(8 * 1024 /* min size 8kb */, (int)(mWidth * mHeight * 3 * 2));
  mData = std::make_unique<uint8_t[]>(maxLength);
  mImg.data = mData.get();
  mImg.length = 0;
  mImg.maxLength = maxLength;
  return true;
}

jr_compressed_ptr UhdrCompressedStructWrapper::getImageHandle() { return &mImg; }

#ifdef DUMP_OUTPUT
static bool writeFile(const char* filename, void*& result, int length) {
  std::ofstream ofd(filename, std::ios::binary);
  if (ofd.is_open()) {
    ofd.write(static_cast<char*>(result), length);
    return true;
  }
  std::cerr << "unable to write to file : " << filename << std::endl;
  return false;
}
#endif

static bool readFile(const char* fileName, void*& result, size_t maxLength, size_t& length) {
  std::ifstream ifd;
  if (openFileHelper(fileName, ifd, std::ios::binary | std::ios::ate)) {
    length = ifd.tellg();
    if (length > maxLength) {
      std::cerr << "not enough space to read file" << std::endl;
      return false;
    }
    ifd.seekg(0, std::ios::beg);
    ifd.read(static_cast<char*>(result), length);
    return true;
  }
  std::cerr << "unable to read file : " << fileName << std::endl;
  return false;
}

static std::vector<std::vector<uint8_t>> getApp13Payloads(const void* image, size_t length) {
  const auto* data = static_cast<const uint8_t*>(image);
  std::vector<std::vector<uint8_t>> payloads;
  size_t pos = 2;  // skip SOI
  while (pos + 1 < length && data[pos] == 0xff) {
    const uint8_t marker = data[pos + 1];
    if (marker == 0xda || marker == 0xd9) break;
    if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      pos += 2;
      continue;
    }
    if (pos + 4 > length) break;
    const size_t segment_length = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
    if (segment_length < 2 || segment_length > length - pos - 2) break;
    if (marker == 0xed) {
      payloads.emplace_back(data + pos + 4, data + pos + 2 + segment_length);
    }
    pos += 2 + segment_length;
  }
  return payloads;
}

static std::vector<uint8_t> insertApp13Payloads(const std::vector<uint8_t>& image,
                                                const std::vector<std::vector<uint8_t>>& payloads,
                                                bool before_exif) {
  if (image.size() < 4 || image[0] != 0xff || image[1] != 0xd8) return {};
  size_t insert_pos = 2;
  while (insert_pos + 4 <= image.size() && image[insert_pos] == 0xff &&
         image[insert_pos + 1] >= 0xe0 && image[insert_pos + 1] <= 0xef) {
    const size_t segment_length = (static_cast<size_t>(image[insert_pos + 2]) << 8) |
                                  image[insert_pos + 3];
    if (segment_length < 2 || segment_length > image.size() - insert_pos - 2) return {};
    insert_pos += 2 + segment_length;
    if (before_exif) break;
  }

  std::vector<uint8_t> result;
  result.reserve(image.size());
  result.insert(result.end(), image.begin(), image.begin() + insert_pos);
  for (const auto& payload : payloads) {
    const size_t segment_length = payload.size() + 2;
    if (segment_length > 0xffff) return {};
    result.push_back(0xff);
    result.push_back(0xed);  // APP13
    result.push_back(static_cast<uint8_t>(segment_length >> 8));
    result.push_back(static_cast<uint8_t>(segment_length));
    result.insert(result.end(), payload.begin(), payload.end());
  }
  result.insert(result.end(), image.begin() + insert_pos, image.end());
  return result;
}

uhdr_color_gamut_t map_internal_cg_to_cg(ultrahdr::ultrahdr_color_gamut cg) {
  switch (cg) {
    case ultrahdr::ULTRAHDR_COLORGAMUT_BT2100:
      return UHDR_CG_BT_2100;
    case ultrahdr::ULTRAHDR_COLORGAMUT_BT709:
      return UHDR_CG_BT_709;
    case ultrahdr::ULTRAHDR_COLORGAMUT_P3:
      return UHDR_CG_DISPLAY_P3;
    default:
      return UHDR_CG_UNSPECIFIED;
  }
}

uhdr_color_transfer_t map_internal_ct_to_ct(ultrahdr::ultrahdr_transfer_function ct) {
  switch (ct) {
    case ultrahdr::ULTRAHDR_TF_HLG:
      return UHDR_CT_HLG;
    case ultrahdr::ULTRAHDR_TF_PQ:
      return UHDR_CT_PQ;
    case ultrahdr::ULTRAHDR_TF_LINEAR:
      return UHDR_CT_LINEAR;
    case ultrahdr::ULTRAHDR_TF_SRGB:
      return UHDR_CT_SRGB;
    default:
      return UHDR_CT_UNSPECIFIED;
  }
}

void decodeJpegRImg(jr_compressed_ptr img, [[maybe_unused]] const char* outFileName) {
  jpegr_info_struct info{};
  JpegR jpegHdr;
  ASSERT_EQ(JPEGR_NO_ERROR, jpegHdr.getJPEGRInfo(img, &info));
  ASSERT_EQ(kImageWidth, info.width);
  ASSERT_EQ(kImageHeight, info.height);
  size_t outSize = info.width * info.height * 8;
  std::unique_ptr<uint8_t[]> data = std::make_unique<uint8_t[]>(outSize);
  jpegr_uncompressed_struct destImage{};
  destImage.data = data.get();
  ASSERT_EQ(JPEGR_NO_ERROR, jpegHdr.decodeJPEGR(img, &destImage));
  ASSERT_EQ(kImageWidth, destImage.width);
  ASSERT_EQ(kImageHeight, destImage.height);
#ifdef DUMP_OUTPUT
  if (!writeFile(outFileName, destImage.data, outSize)) {
    std::cerr << "unable to write output file" << std::endl;
  }
#endif
  uhdr_codec_private_t* obj = uhdr_create_decoder();
  uhdr_compressed_image_t uhdr_image{};
  uhdr_image.data = img->data;
  uhdr_image.data_sz = img->length;
  uhdr_image.capacity = img->length;
  uhdr_image.cg = UHDR_CG_UNSPECIFIED;
  uhdr_image.ct = UHDR_CT_UNSPECIFIED;
  uhdr_image.range = UHDR_CR_UNSPECIFIED;
  uhdr_error_info_t status = uhdr_dec_set_image(obj, &uhdr_image);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  status = uhdr_decode(obj);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  uhdr_raw_image_t* raw_image = uhdr_get_decoded_image(obj);
  ASSERT_NE(nullptr, raw_image);
  ASSERT_EQ(map_internal_cg_to_cg(destImage.colorGamut), raw_image->cg);
  ASSERT_EQ(destImage.width, raw_image->w);
  ASSERT_EQ(destImage.height, raw_image->h);
  char* testData = static_cast<char*>(raw_image->planes[UHDR_PLANE_PACKED]);
  char* refData = static_cast<char*>(destImage.data);
  int bpp = (raw_image->fmt == UHDR_IMG_FMT_64bppRGBAHalfFloat) ? 8 : 4;
  const size_t testStride = raw_image->stride[UHDR_PLANE_PACKED] * bpp;
  const size_t refStride = destImage.width * bpp;
  const size_t length = destImage.width * bpp;
  for (unsigned i = 0; i < destImage.height; i++, testData += testStride, refData += refStride) {
    ASSERT_EQ(0, memcmp(testData, refData, length));
  }
  uhdr_release_decoder(obj);
}

// ============================================================================
// Unit Tests
// ============================================================================

// Test Encode API-0 invalid arguments
TEST(JpegRTest, EncodeAPI0WithInvalidArgs) {
  JpegR uHdrLib;

  UhdrCompressedStructWrapper jpgImg(16, 16);
  ASSERT_TRUE(jpgImg.allocateMemory());

  // test quality factor and transfer function
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());

    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), -1, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad jpeg quality factor";
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), 101, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad jpeg quality factor";

    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_UNSPECIFIED,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(),
                                  static_cast<ultrahdr_transfer_function>(
                                      ultrahdr_transfer_function::ULTRAHDR_TF_MAX + 1),
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), static_cast<ultrahdr_transfer_function>(-10),
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
  }

  // test dest
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());

    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            nullptr, kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
    UhdrCompressedStructWrapper jpgImg2(16, 16);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg2.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
  }

  // test p010 input
  {
    ASSERT_NE(uHdrLib.encodeJPEGR(nullptr, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";

    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";
  }

  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_UNSPECIFIED));
    ASSERT_TRUE(rawImg.allocateMemory());
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";

    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg2.setImageColorGamut(
        static_cast<ultrahdr_color_gamut>(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_MAX + 1)));
    ASSERT_TRUE(rawImg2.allocateMemory());
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg2.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";
  }

  {
    const int kWidth = 32, kHeight = 32;
    UhdrUnCompressedStructWrapper rawImg(kWidth, kHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    auto rawImgP010 = rawImg.getImageHandle();

    rawImgP010->width = kWidth - 1;
    rawImgP010->height = kHeight;
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImgP010, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight - 1;
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImgP010, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = 0;
    rawImgP010->height = kHeight;
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImgP010, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = 0;
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImgP010, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth - 2;
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImgP010, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad luma stride";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth + 64;
    rawImgP010->chroma_data = rawImgP010->data;
    rawImgP010->chroma_stride = kWidth - 2;
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImgP010, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad chroma stride";
  }
}

/* Test Encode API-1 invalid arguments */
TEST(JpegRTest, EncodeAPI1WithInvalidArgs) {
  JpegR uHdrLib;

  UhdrCompressedStructWrapper jpgImg(16, 16);
  ASSERT_TRUE(jpgImg.allocateMemory());

  // test quality factor and transfer function
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());

    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), -1, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad jpeg quality factor";
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), 101, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad jpeg quality factor";

    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_UNSPECIFIED,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  static_cast<ultrahdr_transfer_function>(
                                      ultrahdr_transfer_function::ULTRAHDR_TF_MAX + 1),
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  static_cast<ultrahdr_transfer_function>(-10),
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
  }

  // test dest
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());

    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG, nullptr, kQuality,
                                  nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
    UhdrCompressedStructWrapper jpgImg2(16, 16);
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
  }

  // test p010 input
  {
    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());
    ASSERT_NE(uHdrLib.encodeJPEGR(nullptr, rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";

    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";
  }

  {
    const int kWidth = 32, kHeight = 32;
    UhdrUnCompressedStructWrapper rawImg(kWidth, kHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    auto rawImgP010 = rawImg.getImageHandle();
    UhdrUnCompressedStructWrapper rawImg2(kWidth, kHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());
    auto rawImg420 = rawImg2.getImageHandle();

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_UNSPECIFIED;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut =
        static_cast<ultrahdr_color_gamut>(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_MAX + 1);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";

    rawImgP010->width = kWidth - 1;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight - 1;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = 0;
    rawImgP010->height = kHeight;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = 0;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad luma stride";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth + 64;
    rawImgP010->chroma_data = rawImgP010->data;
    rawImgP010->chroma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad chroma stride";
  }

  // test 420 input
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), nullptr,
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr 420 image";

    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr 420 image";
  }
  {
    const int kWidth = 32, kHeight = 32;
    UhdrUnCompressedStructWrapper rawImg(kWidth, kHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    auto rawImgP010 = rawImg.getImageHandle();
    UhdrUnCompressedStructWrapper rawImg2(kWidth, kHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());
    auto rawImg420 = rawImg2.getImageHandle();

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_UNSPECIFIED;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad 420 color gamut";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->colorGamut =
        static_cast<ultrahdr_color_gamut>(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_MAX + 1);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad 420 color gamut";

    rawImg420->width = kWidth - 1;
    rawImg420->height = kHeight;
    rawImg420->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width for 420";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight - 1;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height for 420";

    rawImg420->width = 0;
    rawImg420->height = kHeight;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width for 420";

    rawImg420->width = kWidth;
    rawImg420->height = 0;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height for 420";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->luma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad luma stride for 420";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->luma_stride = 0;
    rawImg420->chroma_data = rawImgP010->data;
    rawImg420->chroma_stride = kWidth / 2 - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR)
        << "fail, API allows bad chroma stride for 420";
  }
}

/* Test Encode API-2 invalid arguments */
TEST(JpegRTest, EncodeAPI2WithInvalidArgs) {
  JpegR uHdrLib;

  UhdrCompressedStructWrapper jpgImg(16, 16);
  ASSERT_TRUE(jpgImg.allocateMemory());

  // test quality factor and transfer function
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());

    ASSERT_NE(uHdrLib.encodeJPEGR(
                  rawImg.getImageHandle(), rawImg2.getImageHandle(), jpgImg.getImageHandle(),
                  ultrahdr_transfer_function::ULTRAHDR_TF_UNSPECIFIED, jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  jpgImg.getImageHandle(),
                                  static_cast<ultrahdr_transfer_function>(
                                      ultrahdr_transfer_function::ULTRAHDR_TF_MAX + 1),
                                  jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(uHdrLib.encodeJPEGR(
                  rawImg.getImageHandle(), rawImg2.getImageHandle(), jpgImg.getImageHandle(),
                  static_cast<ultrahdr_transfer_function>(-10), jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
  }

  // test dest
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());

    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(),
                                  jpgImg.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
    UhdrCompressedStructWrapper jpgImg2(16, 16);
    ASSERT_NE(uHdrLib.encodeJPEGR(
                  rawImg.getImageHandle(), rawImg2.getImageHandle(), jpgImg.getImageHandle(),
                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
  }

  // test compressed image
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());

    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), rawImg2.getImageHandle(), nullptr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr for compressed image";
    UhdrCompressedStructWrapper jpgImg2(16, 16);
    ASSERT_NE(uHdrLib.encodeJPEGR(
                  rawImg.getImageHandle(), rawImg2.getImageHandle(), jpgImg2.getImageHandle(),
                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr for compressed image";
  }

  // test p010 input
  {
    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());
    ASSERT_NE(
        uHdrLib.encodeJPEGR(nullptr, rawImg2.getImageHandle(), jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";

    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_NE(uHdrLib.encodeJPEGR(
                  rawImg.getImageHandle(), rawImg2.getImageHandle(), jpgImg.getImageHandle(),
                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";
  }

  {
    const int kWidth = 32, kHeight = 32;
    UhdrUnCompressedStructWrapper rawImg(kWidth, kHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    auto rawImgP010 = rawImg.getImageHandle();
    UhdrUnCompressedStructWrapper rawImg2(kWidth, kHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());
    auto rawImg420 = rawImg2.getImageHandle();

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_UNSPECIFIED;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut =
        static_cast<ultrahdr_color_gamut>(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_MAX + 1);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";

    rawImgP010->width = kWidth - 1;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight - 1;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = 0;
    rawImgP010->height = kHeight;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = 0;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad luma stride";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth + 64;
    rawImgP010->chroma_data = rawImgP010->data;
    rawImgP010->chroma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad chroma stride";
  }

  // test 420 input
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), nullptr, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr 420 image";

    UhdrUnCompressedStructWrapper rawImg2(16, 16, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_NE(uHdrLib.encodeJPEGR(
                  rawImg.getImageHandle(), rawImg2.getImageHandle(), jpgImg.getImageHandle(),
                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr 420 image";
  }
  {
    const int kWidth = 32, kHeight = 32;
    UhdrUnCompressedStructWrapper rawImg(kWidth, kHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    auto rawImgP010 = rawImg.getImageHandle();
    UhdrUnCompressedStructWrapper rawImg2(kWidth, kHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
    ASSERT_TRUE(rawImg2.allocateMemory());
    auto rawImg420 = rawImg2.getImageHandle();

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_UNSPECIFIED;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad 420 color gamut";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->colorGamut =
        static_cast<ultrahdr_color_gamut>(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_MAX + 1);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad 420 color gamut";

    rawImg420->width = kWidth - 1;
    rawImg420->height = kHeight;
    rawImg420->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width for 420";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight - 1;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height for 420";

    rawImg420->width = 0;
    rawImg420->height = kHeight;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width for 420";

    rawImg420->width = kWidth;
    rawImg420->height = 0;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height for 420";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->luma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad luma stride for 420";

    rawImg420->width = kWidth;
    rawImg420->height = kHeight;
    rawImg420->luma_stride = 0;
    rawImg420->chroma_data = rawImgP010->data;
    rawImg420->chroma_stride = kWidth / 2 - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, rawImg420, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad chroma stride for 420";
  }
}

/* Test Encode API-3 invalid arguments */
TEST(JpegRTest, EncodeAPI3WithInvalidArgs) {
  JpegR uHdrLib;

  UhdrCompressedStructWrapper jpgImg(16, 16);
  ASSERT_TRUE(jpgImg.allocateMemory());

  // test quality factor and transfer function
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());

    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), jpgImg.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_UNSPECIFIED,
                                  jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), jpgImg.getImageHandle(),
                                  static_cast<ultrahdr_transfer_function>(
                                      ultrahdr_transfer_function::ULTRAHDR_TF_MAX + 1),
                                  jpgImg.getImageHandle()),
              JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), jpgImg.getImageHandle(),
                            static_cast<ultrahdr_transfer_function>(-10), jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad hdr transfer function";
  }

  // test dest
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());

    ASSERT_NE(uHdrLib.encodeJPEGR(rawImg.getImageHandle(), jpgImg.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG, nullptr),
              JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
    UhdrCompressedStructWrapper jpgImg2(16, 16);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr dest";
  }

  // test compressed image
  {
    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());

    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), nullptr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr for compressed image";
    UhdrCompressedStructWrapper jpgImg2(16, 16);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), jpgImg2.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr for compressed image";
  }

  // test p010 input
  {
    ASSERT_NE(
        uHdrLib.encodeJPEGR(nullptr, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";

    UhdrUnCompressedStructWrapper rawImg(16, 16, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImg.getImageHandle(), jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows nullptr p010 image";
  }

  {
    const int kWidth = 32, kHeight = 32;
    UhdrUnCompressedStructWrapper rawImg(kWidth, kHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
    ASSERT_TRUE(rawImg.allocateMemory());
    auto rawImgP010 = rawImg.getImageHandle();

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_UNSPECIFIED;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut =
        static_cast<ultrahdr_color_gamut>(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_MAX + 1);
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad p010 color gamut";

    rawImgP010->width = kWidth - 1;
    rawImgP010->height = kHeight;
    rawImgP010->colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight - 1;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = 0;
    rawImgP010->height = kHeight;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image width";

    rawImgP010->width = kWidth;
    rawImgP010->height = 0;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad image height";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad luma stride";

    rawImgP010->width = kWidth;
    rawImgP010->height = kHeight;
    rawImgP010->luma_stride = kWidth + 64;
    rawImgP010->chroma_data = rawImgP010->data;
    rawImgP010->chroma_stride = kWidth - 2;
    ASSERT_NE(
        uHdrLib.encodeJPEGR(rawImgP010, jpgImg.getImageHandle(),
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
        JPEGR_NO_ERROR)
        << "fail, API allows bad chroma stride";
  }
}

/* Test Encode API-4 invalid arguments */
TEST(JpegRTest, EncodeAPI4WithInvalidArgs) {
  UhdrCompressedStructWrapper jpgImg(16, 16);
  ASSERT_TRUE(jpgImg.allocateMemory());
  UhdrCompressedStructWrapper jpgImg2(16, 16);
  JpegR uHdrLib;

  // test dest
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), nullptr, nullptr),
            JPEGR_NO_ERROR)
      << "fail, API allows nullptr dest";
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), nullptr,
                                jpgImg2.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows nullptr dest";

  // test primary image
  ASSERT_NE(uHdrLib.encodeJPEGR(nullptr, jpgImg.getImageHandle(), nullptr, jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows nullptr primary image";
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg2.getImageHandle(), jpgImg.getImageHandle(), nullptr,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows nullptr primary image";

  // test gain map
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), nullptr, nullptr, jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows nullptr gain map image";
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg2.getImageHandle(), nullptr,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows nullptr gain map image";

  // test metadata
  ultrahdr_metadata_struct good_metadata;
  good_metadata.version = "1.0";
  good_metadata.minContentBoost = 1.0f;
  good_metadata.maxContentBoost = 2.0f;
  good_metadata.gamma = 1.0f;
  good_metadata.offsetSdr = 0.0f;
  good_metadata.offsetHdr = 0.0f;
  good_metadata.hdrCapacityMin = 1.0f;
  good_metadata.hdrCapacityMax = 2.0f;

  ultrahdr_metadata_struct metadata = good_metadata;
  metadata.version = "1.1";
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), &metadata,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows bad metadata version";

  metadata = good_metadata;
  metadata.minContentBoost = 3.0f;
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), &metadata,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows bad metadata content boost";

  metadata = good_metadata;
  metadata.gamma = -0.1f;
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), &metadata,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows bad metadata gamma";

  metadata = good_metadata;
  metadata.offsetSdr = -0.1f;
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), &metadata,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows bad metadata offset sdr";

  metadata = good_metadata;
  metadata.offsetHdr = -0.1f;
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), &metadata,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows bad metadata offset hdr";

  metadata = good_metadata;
  metadata.hdrCapacityMax = 0.5f;
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), &metadata,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows bad metadata hdr capacity max";

  metadata = good_metadata;
  metadata.hdrCapacityMin = 0.5f;
  ASSERT_NE(uHdrLib.encodeJPEGR(jpgImg.getImageHandle(), jpgImg.getImageHandle(), &metadata,
                                jpgImg.getImageHandle()),
            JPEGR_NO_ERROR)
      << "fail, API allows bad metadata hdr capacity min";
}

TEST(JpegRTest, EncodeAPI4PreservesApp13Payloads) {
  std::ifstream ifd;
  ASSERT_TRUE(openFileHelper(kSdrJpgFileName, ifd, std::ios::binary | std::ios::ate));
  const size_t source_size = static_cast<size_t>(ifd.tellg());
  ASSERT_GT(source_size, 0u);
  ifd.seekg(0, std::ios::beg);
  std::vector<uint8_t> source(source_size);
  ASSERT_TRUE(ifd.read(reinterpret_cast<char*>(source.data()), source_size));

  // Photoshop IPTC resource: "APP13 caption" in dataset 0x0278.
  const std::vector<uint8_t> iptc_payload = {
      'P', 'h', 'o', 't', 'o', 's', 'h', 'o', 'p', ' ', '3', '.', '0', 0,
      '8', 'B', 'I', 'M', 0x04, 0x04, 0, 0, 0, 0, 0, 18, 0x1c, 0x02, 0x78, 0, 13,
      'A', 'P', 'P', '1', '3', ' ', 'c', 'a', 'p', 't', 'i', 'o', 'n'};

  std::vector<uint8_t> opaque_payload(4097);
  for (size_t i = 0; i < opaque_payload.size(); ++i) {
    opaque_payload[i] = static_cast<uint8_t>((i * 17 + 3) % 253);
  }
  const std::vector<std::vector<uint8_t>> expected_payloads = {iptc_payload, opaque_payload};
  ASSERT_TRUE(getApp13Payloads(source.data(), source.size()).empty());
  JpegDecoderHelper source_decoder;
  ASSERT_EQ(source_decoder.parseImage(source.data(), source.size()).error_code, UHDR_CODEC_OK);
  ASSERT_GT(source_decoder.getEXIFSize(), 0u);
  const auto* source_exif = static_cast<const uint8_t*>(source_decoder.getEXIFPtr());
  const size_t source_exif_size = source_decoder.getEXIFSize();

  UhdrCompressedStructWrapper dest(kImageWidth, kImageHeight);
  ASSERT_TRUE(dest.allocateMemory());
  auto* dest_image = dest.getImageHandle();
  uhdr_compressed_image_t gainmap_input{
      source.data(), source.size(), source.size(), UHDR_CG_BT_709, UHDR_CT_UNSPECIFIED,
      UHDR_CR_UNSPECIFIED};

  uhdr_compressed_image_t output{dest_image->data, 0, dest_image->maxLength, UHDR_CG_UNSPECIFIED,
                                 UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
  uhdr_gainmap_metadata_ext_t metadata("1.0");
  std::fill_n(metadata.max_content_boost, 3, 2.0f);
  std::fill_n(metadata.min_content_boost, 3, 1.0f);
  std::fill_n(metadata.gamma, 3, 1.0f);
  std::fill_n(metadata.offset_sdr, 3, 0.0f);
  std::fill_n(metadata.offset_hdr, 3, 0.0f);
  metadata.hdr_capacity_min = 1.0f;
  metadata.hdr_capacity_max = 2.0f;
  metadata.use_base_cg = true;
  JpegR uHdrLib;

  for (const bool before_exif : {false, true}) {
    std::vector<uint8_t> source_with_app13 =
        insertApp13Payloads(source, expected_payloads, before_exif);
    ASSERT_FALSE(source_with_app13.empty());
    ASSERT_EQ(getApp13Payloads(source_with_app13.data(), source_with_app13.size()),
              expected_payloads);
    uhdr_compressed_image_t base_input{
        source_with_app13.data(), source_with_app13.size(), source_with_app13.size(),
        UHDR_CG_BT_709, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
    output.data_sz = 0;
    const uhdr_error_info_t status =
        uHdrLib.encodeJPEGR(&base_input, &gainmap_input, &metadata, &output);
    ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << status.detail;
    dest_image->length = output.data_sz;
    EXPECT_EQ(getApp13Payloads(dest_image->data, dest_image->length), expected_payloads);
    JpegDecoderHelper output_decoder;
    ASSERT_EQ(output_decoder.parseImage(dest_image->data, dest_image->length).error_code,
              UHDR_CODEC_OK);
    ASSERT_GT(output_decoder.getEXIFSize(), 0u);
    ASSERT_EQ(output_decoder.getEXIFSize(), source_exif_size);
    ASSERT_EQ(memcmp(output_decoder.getEXIFPtr(), source_exif, source_exif_size), 0);
    ASSERT_NO_FATAL_FAILURE(decodeJpegRImg(dest_image, "decode_api4_app13_output.rgb"));
  }
}

/* Test Decode API invalid arguments */
TEST(JpegRTest, DecodeAPIWithInvalidArgs) {
  JpegR uHdrLib;

  UhdrCompressedStructWrapper jpgImg(16, 16);
  jpegr_uncompressed_struct destImage{};
  size_t outSize = 16 * 16 * 8;
  std::unique_ptr<uint8_t[]> data = std::make_unique<uint8_t[]>(outSize);
  destImage.data = data.get();

  // test jpegr image
  ASSERT_NE(uHdrLib.decodeJPEGR(nullptr, &destImage), JPEGR_NO_ERROR)
      << "fail, API allows nullptr for jpegr img";
  ASSERT_NE(uHdrLib.decodeJPEGR(jpgImg.getImageHandle(), &destImage), JPEGR_NO_ERROR)
      << "fail, API allows nullptr for jpegr img";
  ASSERT_TRUE(jpgImg.allocateMemory());

  // test dest image
  ASSERT_NE(uHdrLib.decodeJPEGR(jpgImg.getImageHandle(), nullptr), JPEGR_NO_ERROR)
      << "fail, API allows nullptr for dest";
  destImage.data = nullptr;
  ASSERT_NE(uHdrLib.decodeJPEGR(jpgImg.getImageHandle(), &destImage), JPEGR_NO_ERROR)
      << "fail, API allows nullptr for dest";
  destImage.data = data.get();

  // test max display boost
  ASSERT_NE(uHdrLib.decodeJPEGR(jpgImg.getImageHandle(), &destImage, 0.5), JPEGR_NO_ERROR)
      << "fail, API allows invalid max display boost";

  // test output format
  ASSERT_NE(uHdrLib.decodeJPEGR(jpgImg.getImageHandle(), &destImage, FLT_MAX, nullptr,
                                static_cast<ultrahdr_output_format>(-1)),
            JPEGR_NO_ERROR)
      << "fail, API allows invalid output format";
  ASSERT_NE(uHdrLib.decodeJPEGR(jpgImg.getImageHandle(), &destImage, FLT_MAX, nullptr,
                                static_cast<ultrahdr_output_format>(ULTRAHDR_OUTPUT_MAX + 1)),
            JPEGR_NO_ERROR)
      << "fail, API allows invalid output format";
}

TEST(JpegRTest, DecodeLegacyApiWithGainMapOutputKeepsPrimaryDescriptor) {
  UhdrUnCompressedStructWrapper rawImg(kImageWidth, kImageHeight, YCbCr_p010);
  ASSERT_TRUE(rawImg.setImageColorGamut(ULTRAHDR_COLORGAMUT_BT2100));
  ASSERT_TRUE(rawImg.allocateMemory());
  ASSERT_TRUE(rawImg.loadRawResource(kYCbCrP010FileName));

  UhdrCompressedStructWrapper jpgImg(kImageWidth, kImageHeight);
  ASSERT_TRUE(jpgImg.allocateMemory());

  JpegR uHdrLib;
  ASSERT_EQ(JPEGR_NO_ERROR,
            uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ULTRAHDR_TF_HLG,
                                jpgImg.getImageHandle(), kQuality, nullptr));

  std::vector<uint8_t> primaryData(kImageWidth * kImageHeight * 8);
  jpegr_uncompressed_struct primaryImage{};
  primaryImage.data = primaryData.data();

  std::vector<uint8_t> gainmapData(kImageWidth * kImageHeight * 3);
  jpegr_uncompressed_struct gainmapImage{};
  gainmapImage.data = gainmapData.data();

  ultrahdr_metadata_struct metadata{};
  ASSERT_EQ(JPEGR_NO_ERROR,
            uHdrLib.decodeJPEGR(jpgImg.getImageHandle(), &primaryImage, FLT_MAX, nullptr,
                                ULTRAHDR_OUTPUT_HDR_LINEAR, &gainmapImage, &metadata));

  EXPECT_EQ(kImageWidth, primaryImage.width);
  EXPECT_EQ(kImageHeight, primaryImage.height);
  EXPECT_EQ(UHDR_IMG_FMT_64bppRGBAHalfFloat, primaryImage.pixelFormat);
  EXPECT_GT(gainmapImage.width, 0u);
  EXPECT_GT(gainmapImage.height, 0u);
  EXPECT_LE(gainmapImage.width, kImageWidth);
  EXPECT_LE(gainmapImage.height, kImageHeight);
  EXPECT_GT(metadata.hdrCapacityMax, 1.0f);
}

class TestJpegR : public JpegR {
 public:
  using JpegR::applyGainMap;
};

TEST(JpegRTest, ApplyGainMapInvalidArgs) {
  TestJpegR uHdrLib;
  uhdr_raw_image_t sdr_intent{};
  sdr_intent.fmt = UHDR_IMG_FMT_32bppRGBA8888;
  sdr_intent.w = 16;
  sdr_intent.h = 16;
  sdr_intent.stride[0] = 16;
  std::vector<uint8_t> sdr_buf(16 * 16 * 4, 128);
  sdr_intent.planes[0] = sdr_buf.data();

  uhdr_raw_image_t gainmap_img{};
  gainmap_img.fmt = UHDR_IMG_FMT_8bppYCbCr400;
  gainmap_img.w = 16;
  gainmap_img.h = 16;
  gainmap_img.stride[0] = 16;
  std::vector<uint8_t> gm_buf(16 * 16, 128);
  gainmap_img.planes[0] = gm_buf.data();

  uhdr_gainmap_metadata_ext_t metadata("1.0");
  std::fill_n(metadata.max_content_boost, 3, 2.0f);
  std::fill_n(metadata.min_content_boost, 3, 1.0f);
  std::fill_n(metadata.gamma, 3, 1.0f);
  std::fill_n(metadata.offset_sdr, 3, 0.0f);
  std::fill_n(metadata.offset_hdr, 3, 0.0f);
  metadata.hdr_capacity_min = 1.0f;
  metadata.hdr_capacity_max = 2.0f;

  uhdr_raw_image_t dest{};
  dest.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
  dest.w = 16;
  dest.h = 16;
  dest.stride[0] = 16;
  std::vector<uint8_t> dest_buf(32 * 16 * 8);
  dest.planes[0] = dest_buf.data();

  // Test nullptr dest or plane pointer
  EXPECT_NE(uHdrLib.applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, nullptr).error_code, UHDR_CODEC_OK);
  dest.planes[0] = nullptr;
  EXPECT_EQ(uHdrLib.applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, &dest).error_code, UHDR_CODEC_INVALID_PARAM);

  dest.planes[0] = dest_buf.data();

  // Test stride < width
  dest.stride[0] = 8;
  EXPECT_EQ(uHdrLib.applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, &dest).error_code, UHDR_CODEC_INVALID_PARAM);
  dest.stride[0] = 16;

  // Test destination dimensions that do not match the output dimensions
  dest.w = 8;
  EXPECT_EQ(
      uHdrLib
          .applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, &dest)
          .error_code,
      UHDR_CODEC_INVALID_PARAM);
  dest.w = 16;

  dest.h = 8;
  EXPECT_EQ(
      uHdrLib
          .applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, &dest)
          .error_code,
      UHDR_CODEC_INVALID_PARAM);
  dest.h = 16;

  dest.w = 32;
  dest.stride[0] = 32;
  EXPECT_EQ(
      uHdrLib
          .applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, &dest)
          .error_code,
      UHDR_CODEC_INVALID_PARAM);
  dest.w = 16;

  // Test valid row padding
  EXPECT_EQ(
      uHdrLib
          .applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, &dest)
          .error_code,
      UHDR_CODEC_OK);
  dest.stride[0] = 16;

  // Test invalid output_ct
  EXPECT_EQ(uHdrLib.applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_UNSPECIFIED, dest.fmt, 2.0f, &dest).error_code, UHDR_CODEC_INVALID_PARAM);

  // Test format mismatch (LINEAR ct with non-64bpp format)
  dest.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
  EXPECT_EQ(uHdrLib.applyGainMap(&sdr_intent, &gainmap_img, &metadata, UHDR_CT_LINEAR, dest.fmt, 2.0f, &dest).error_code, UHDR_CODEC_INVALID_PARAM);
}

TEST(JpegRTest, AppleApplyGainMapMatchesCenteredScalarOracle) {
  struct MapPattern {
    std::string name;
    unsigned int width;
    unsigned int height;
    std::vector<uint8_t> values;
  };
  std::vector<MapPattern> patterns;
  std::vector<uint8_t> ramp(3 * 5);
  for (unsigned int y = 0; y < 5; ++y) {
    for (unsigned int x = 0; x < 3; ++x) {
      ramp[y * 3 + x] = static_cast<uint8_t>((x * 64 + y * 47) & 0xff);
    }
  }
  patterns.push_back({"ramp", 3, 5, ramp});
  std::vector<uint8_t> impulse(3 * 5, 0);
  impulse[2 * 3 + 1] = 255;
  patterns.push_back({"impulse", 3, 5, impulse});
  std::vector<uint8_t> edge(3 * 5);
  for (unsigned int y = 0; y < 5; ++y) {
    for (unsigned int x = 0; x < 3; ++x) edge[y * 3 + x] = x == 0 ? 0 : 255;
  }
  patterns.push_back({"vertical-edge", 3, 5, edge});
  std::vector<uint8_t> corners(3 * 5, 0);
  corners[0] = corners[2] = corners[12] = corners[14] = 255;
  patterns.push_back({"corners", 3, 5, corners});
  std::vector<uint8_t> checkerboard(3 * 5);
  for (unsigned int y = 0; y < 5; ++y) {
    for (unsigned int x = 0; x < 3; ++x) {
      checkerboard[y * 3 + x] = ((x + y) & 1) ? 255 : 0;
    }
  }
  patterns.push_back({"checkerboard", 3, 5, checkerboard});
  patterns.push_back({"constant-127", 1, 1, {127}});

  const std::vector<float> headrooms = {1.0f,
                                        std::nextafter(1.0f, 2.0f),
                                        2.0f,
                                        8.0f,
                                        8.5f,
                                        9.0f,
                                        16.0f,
                                        32.0f,
                                        1.0e6f,
                                        std::ldexp(1.0f, 127)};
  const std::vector<float> requested_boosts = {1.0f, 2.0f, 4.0f, 8.0f};
  constexpr double kLinearOutputMax = 10000.0 / 203.0;
  constexpr float kTablePowerError = 8.0e-7f;
  constexpr double kTableGainStopBudget = 9.0e-6;
  constexpr double kFinalGainArithmeticReserve = 1.0e-6;
  constexpr double kTotalGainStopBudget = 1.0e-5;
  constexpr float kAppleMapGamma = 2.2f;
  constexpr uint64_t kPaddingSentinel = 0xdeadbeefcafef00dULL;
  static_assert(kTableGainStopBudget + kFinalGainArithmeticReserve <= kTotalGainStopBudget);

  std::array<float, 1024> power_table{};
  double max_power_entry_error = 0.0;
  for (size_t i = 0; i < power_table.size(); ++i) {
    const float value = static_cast<float>(i) / static_cast<float>(power_table.size() - 1);
    power_table[i] = std::pow(value, kAppleMapGamma);
    const double exact_power =
        std::pow(static_cast<double>(value), static_cast<double>(kAppleMapGamma));
    const float lower = std::nextafter(power_table[i], -std::numeric_limits<float>::infinity());
    const float upper = std::nextafter(power_table[i], std::numeric_limits<float>::infinity());
    const double down_ulp = static_cast<double>(power_table[i]) - lower;
    const double up_ulp = static_cast<double>(upper) - power_table[i];
    const double adjacent_float_ulp = std::max(down_ulp, up_ulp);
    const double entry_error = std::abs(static_cast<double>(power_table[i]) - exact_power);
    max_power_entry_error = std::max(max_power_entry_error, entry_error);
    EXPECT_LE(entry_error, adjacent_float_ulp) << "table entry " << i;
  }
  std::cout << std::setprecision(10) << "APPLE_POWER_TABLE entries=" << power_table.size()
            << " max_double_error=" << max_power_entry_error << " one_float_ulp=verified\n";
  const auto evaluatePowerTable = [&power_table](float value) {
    const float position = value * static_cast<float>(power_table.size() - 1);
    const size_t lower = static_cast<size_t>(position);
    if (lower >= power_table.size() - 1) return power_table.back();
    const float fraction = position - static_cast<float>(lower);
    return power_table[lower] + fraction * (power_table[lower + 1] - power_table[lower]);
  };

  const double base_encoded = 128.0 / 255.0;
  const double base_linear_scalar =
      std::pow((base_encoded + 0.055) / 1.055, 2.4);
  const double base_lut_error =
      std::abs(static_cast<double>(srgbInvOetfExtendedLUT(static_cast<float>(base_encoded))) -
               base_linear_scalar);
  EXPECT_LE(base_lut_error, 1.0e-6);

  TestJpegR decoder;
  for (const MapPattern& pattern : patterns) {
    const size_t map_stride = pattern.width + 3;
    std::vector<uint8_t> map_storage(map_stride * pattern.height, 0xee);
    for (unsigned int y = 0; y < pattern.height; ++y) {
      std::copy_n(pattern.values.data() + static_cast<size_t>(y) * pattern.width, pattern.width,
                  map_storage.data() + static_cast<size_t>(y) * map_stride);
    }
    uhdr_raw_image_t map{};
    map.fmt = UHDR_IMG_FMT_8bppYCbCr400;
    map.cg = UHDR_CG_UNSPECIFIED;
    map.ct = UHDR_CT_SRGB;
    map.range = UHDR_CR_FULL_RANGE;
    map.w = pattern.width;
    map.h = pattern.height;
    map.planes[UHDR_PLANE_Y] = map_storage.data();
    map.stride[UHDR_PLANE_Y] = map_stride;

    for (const auto& geometry : {std::pair<unsigned int, unsigned int>{7, 9}, {31, 33}}) {
      const unsigned int width = geometry.first;
      const unsigned int height = geometry.second;
      const size_t base_stride = width + 2;
      std::vector<uint8_t> y_storage(base_stride * height, 0xe1);
      std::vector<uint8_t> u_storage(base_stride * height, 0xe2);
      std::vector<uint8_t> v_storage(base_stride * height, 0xe3);
      for (unsigned int y = 0; y < height; ++y) {
        std::fill_n(y_storage.data() + static_cast<size_t>(y) * base_stride, width, 128);
        std::fill_n(u_storage.data() + static_cast<size_t>(y) * base_stride, width, 128);
        std::fill_n(v_storage.data() + static_cast<size_t>(y) * base_stride, width, 128);
      }
      uhdr_raw_image_t base{};
      base.fmt = UHDR_IMG_FMT_24bppYCbCr444;
      base.cg = UHDR_CG_DISPLAY_P3;
      base.ct = UHDR_CT_SRGB;
      base.range = UHDR_CR_FULL_RANGE;
      base.w = width;
      base.h = height;
      base.planes[UHDR_PLANE_Y] = y_storage.data();
      base.planes[UHDR_PLANE_U] = u_storage.data();
      base.planes[UHDR_PLANE_V] = v_storage.data();
      base.stride[UHDR_PLANE_Y] = base_stride;
      base.stride[UHDR_PLANE_U] = base_stride;
      base.stride[UHDR_PLANE_V] = base_stride;

      for (const float headroom : headrooms) {
        std::vector<float> display_boosts;
        for (const float requested : requested_boosts) {
          const float boost = std::min(requested, headroom);
          if (display_boosts.empty() || display_boosts.back() != boost) {
            display_boosts.push_back(boost);
          }
        }
        if (display_boosts.back() != headroom) display_boosts.push_back(headroom);

        const double table_stop_bound =
            std::log2(1.0 + (static_cast<double>(headroom) - 1.0) * kTablePowerError);
        const bool table_is_used =
            std::isfinite(table_stop_bound) && table_stop_bound <= kTableGainStopBudget;
        if (headroom == 8.0f || headroom == 8.5f) EXPECT_TRUE(table_is_used);
        if (headroom == 9.0f || headroom == 16.0f) EXPECT_FALSE(table_is_used);
        uhdr_gainmap_metadata_ext_t metadata("1.0");
        metadata.is_apple_gainmap = true;
        metadata.apple_gainmap_version = 131072;
        std::fill_n(metadata.min_content_boost, 3, 1.0f);
        std::fill_n(metadata.max_content_boost, 3, headroom);
        std::fill_n(metadata.gamma, 3, 1.0f);
        std::fill_n(metadata.offset_sdr, 3, 0.0f);
        std::fill_n(metadata.offset_hdr, 3, 0.0f);
        metadata.hdr_capacity_min = 1.0f;
        metadata.hdr_capacity_max = headroom;
        metadata.use_base_cg = true;

        for (const float display_boost : display_boosts) {
          const size_t destination_stride = width + 1;
          std::vector<uint64_t> output(destination_stride * height, kPaddingSentinel);
          uhdr_raw_image_t dest{};
          dest.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
          dest.cg = UHDR_CG_DISPLAY_P3;
          dest.ct = UHDR_CT_LINEAR;
          dest.range = UHDR_CR_FULL_RANGE;
          dest.w = width;
          dest.h = height;
          dest.planes[UHDR_PLANE_PACKED] = output.data();
          dest.stride[UHDR_PLANE_PACKED] = destination_stride;

          const uhdr_error_info_t status = decoder.applyGainMap(
              &base, &map, &metadata, UHDR_CT_LINEAR, UHDR_IMG_FMT_64bppRGBAHalfFloat,
              display_boost, &dest, true, 1, 1);
          ASSERT_EQ(status.error_code, UHDR_CODEC_OK)
              << pattern.name << " " << width << "x" << height << " H=" << headroom
              << " D=" << display_boost << " " << status.detail;

          double max_power_abs_error = 0.0;
          double max_gain_stop_error = 0.0;
          unsigned int max_half_ulp = 0;
          size_t saturated_pixels = 0;
          size_t nonsaturated_pixels = 0;
          for (unsigned int y = 0; y < height; ++y) {
            for (unsigned int x = 0; x < width; ++x) {
              const double source_x = std::clamp(
                  (static_cast<double>(x) + 0.5) * pattern.width / width - 0.5, 0.0,
                  static_cast<double>(pattern.width - 1));
              const double source_y = std::clamp(
                  (static_cast<double>(y) + 0.5) * pattern.height / height - 0.5, 0.0,
                  static_cast<double>(pattern.height - 1));
              const unsigned int x0 = static_cast<unsigned int>(std::floor(source_x));
              const unsigned int y0 = static_cast<unsigned int>(std::floor(source_y));
              const unsigned int x1 = std::min(x0 + 1, pattern.width - 1);
              const unsigned int y1 = std::min(y0 + 1, pattern.height - 1);
              const double fx = source_x - x0;
              const double fy = source_y - y0;
              const auto mapAt = [&pattern](unsigned int mx, unsigned int my) {
                return static_cast<double>(pattern.values[my * pattern.width + mx]) / 255.0;
              };
              const double top = mapAt(x0, y0) * (1.0 - fx) + mapAt(x1, y0) * fx;
              const double bottom = mapAt(x0, y1) * (1.0 - fx) + mapAt(x1, y1) * fx;
              const double sampled_map = top * (1.0 - fy) + bottom * fy;
              const float sampled_map_float = static_cast<float>(sampled_map);
              const double exact_power = std::pow(sampled_map, 2.2);
              const float modeled_power = table_is_used
                                              ? evaluatePowerTable(sampled_map_float)
                                              : std::pow(sampled_map_float, 2.2f);
              max_power_abs_error = std::max(
                  max_power_abs_error,
                  std::abs(static_cast<double>(modeled_power) - exact_power));
              const double expected_gain =
                  1.0 + (static_cast<double>(display_boost) - 1.0) * exact_power;
              const double modeled_gain =
                  1.0 + (static_cast<double>(display_boost) - 1.0) * modeled_power;
              max_gain_stop_error = std::max(
                  max_gain_stop_error, std::abs(std::log2(modeled_gain / expected_gain)));

              const double expected_linear = base_linear_scalar * expected_gain;
              const bool saturated = expected_linear >= kLinearOutputMax;
              const float bounded_linear =
                  static_cast<float>(std::clamp(expected_linear, 0.0, kLinearOutputMax));
              const uint16_t expected_half = floatToHalf(bounded_linear);
              const size_t output_index = static_cast<size_t>(y) * destination_stride + x;
              const uint64_t packed = output[output_index];
              const std::array<uint16_t, 4> actual_half = {
                  static_cast<uint16_t>(packed & 0xffff),
                  static_cast<uint16_t>((packed >> 16) & 0xffff),
                  static_cast<uint16_t>((packed >> 32) & 0xffff),
                  static_cast<uint16_t>((packed >> 48) & 0xffff)};
              EXPECT_EQ(halfToFloat(actual_half[3]), 1.0f)
                  << pattern.name << " " << width << "x" << height << " H=" << headroom
                  << " D=" << display_boost << " at " << x << "," << y;
              for (size_t channel = 0; channel < 3; ++channel) {
                const float actual_linear = halfToFloat(actual_half[channel]);
                EXPECT_TRUE(std::isfinite(actual_linear))
                    << pattern.name << " " << width << "x" << height << " H=" << headroom
                    << " D=" << display_boost << " at " << x << "," << y;
                EXPECT_GE(actual_linear, 0.0f);
                EXPECT_LE(actual_linear, static_cast<float>(kLinearOutputMax));
                const unsigned int ulp_distance = static_cast<unsigned int>(std::abs(
                    static_cast<int>(actual_half[channel]) - static_cast<int>(expected_half)));
                if (saturated) {
                  EXPECT_EQ(actual_half[channel], expected_half)
                      << pattern.name << " " << width << "x" << height << " H=" << headroom
                      << " D=" << display_boost << " at " << x << "," << y;
                } else {
                  ++nonsaturated_pixels;
                  max_half_ulp = std::max(max_half_ulp, ulp_distance);
                  EXPECT_LE(ulp_distance, 1u)
                      << pattern.name << " " << width << "x" << height << " H=" << headroom
                      << " D=" << display_boost << " at " << x << "," << y;
                }
              }
              if (saturated) ++saturated_pixels;
            }
          }
          for (unsigned int y = 0; y < height; ++y) {
            EXPECT_EQ(output[static_cast<size_t>(y) * destination_stride + width],
                      kPaddingSentinel);
            for (size_t x = width; x < base_stride; ++x) {
              EXPECT_EQ(y_storage[static_cast<size_t>(y) * base_stride + x], 0xe1);
              EXPECT_EQ(u_storage[static_cast<size_t>(y) * base_stride + x], 0xe2);
              EXPECT_EQ(v_storage[static_cast<size_t>(y) * base_stride + x], 0xe3);
            }
          }
          for (unsigned int y = 0; y < pattern.height; ++y) {
            for (size_t x = pattern.width; x < map_stride; ++x) {
              EXPECT_EQ(map_storage[static_cast<size_t>(y) * map_stride + x], 0xee);
            }
          }

          EXPECT_LE(max_power_abs_error, static_cast<double>(kTablePowerError));
          EXPECT_LE(max_gain_stop_error, kTableGainStopBudget);
          std::cout << std::setprecision(10) << "APPLE_APPLY_MAP pattern=" << pattern.name
                    << " map=" << pattern.width << "x" << pattern.height << " base=" << width
                    << "x" << height << " H=" << headroom << " D=" << display_boost
                    << " path=" << (table_is_used ? "table" : "scalar")
                    << " power_abs_error=" << max_power_abs_error
                    << " gain_stop_error=" << max_gain_stop_error
                    << " base_lut_abs_error=" << base_lut_error << " max_half_ulp="
                    << max_half_ulp << " nonsaturated=" << nonsaturated_pixels
                    << " saturated=" << saturated_pixels
                    << " alpha=1 finite=1 padded=preserved\n";
        }
      }
    }
  }
}

TEST(JpegRTest, writeXmpThenRead) {
  uhdr_gainmap_metadata_ext_t metadata_expected("1.0");
  std::fill_n(metadata_expected.max_content_boost, 3, 1.25f);
  std::fill_n(metadata_expected.min_content_boost, 3, 0.75f);
  std::fill_n(metadata_expected.gamma, 3, 1.0f);
  std::fill_n(metadata_expected.offset_sdr, 3, 0.0f);
  std::fill_n(metadata_expected.offset_hdr, 3, 0.0f);
  metadata_expected.hdr_capacity_min = 1.0f;
  metadata_expected.hdr_capacity_max = metadata_expected.max_content_boost[0];
  metadata_expected.use_base_cg = true;

  const std::string nameSpace = "http://ns.adobe.com/xap/1.0/\0";
  const size_t nameSpaceLength = nameSpace.size() + 1;  // need to count the null terminator

  std::string xmp = generateXmpForSecondaryImage(metadata_expected);

  std::vector<uint8_t> xmpData;
  xmpData.reserve(nameSpaceLength + xmp.size());
  xmpData.insert(xmpData.end(), reinterpret_cast<const uint8_t*>(nameSpace.c_str()),
                 reinterpret_cast<const uint8_t*>(nameSpace.c_str()) + nameSpaceLength);
  xmpData.insert(xmpData.end(), reinterpret_cast<const uint8_t*>(xmp.c_str()),
                 reinterpret_cast<const uint8_t*>(xmp.c_str()) + xmp.size());

  uhdr_gainmap_metadata_ext_t metadata_read;
  EXPECT_EQ(getMetadataFromXMP(xmpData.data(), xmpData.size(), /*exif_data=*/nullptr,
                               /*exif_size=*/0, &metadata_read)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_FLOAT_EQ(metadata_expected.max_content_boost[0], metadata_read.max_content_boost[0]);
  EXPECT_FLOAT_EQ(metadata_expected.min_content_boost[0], metadata_read.min_content_boost[0]);
  EXPECT_FLOAT_EQ(metadata_expected.gamma[0], metadata_read.gamma[0]);
  EXPECT_FLOAT_EQ(metadata_expected.offset_sdr[0], metadata_read.offset_sdr[0]);
  EXPECT_FLOAT_EQ(metadata_expected.offset_hdr[0], metadata_read.offset_hdr[0]);
  EXPECT_FLOAT_EQ(metadata_expected.hdr_capacity_min, metadata_read.hdr_capacity_min);
  EXPECT_FLOAT_EQ(metadata_expected.hdr_capacity_max, metadata_read.hdr_capacity_max);
  EXPECT_TRUE(metadata_read.use_base_cg);
}

// Apple XMP without HDRGainMapHeadroom, so that the headroom is read from EXIF.
static std::vector<uint8_t> appleXmpWithoutHeadroom() {
  const std::string nameSpace = "http://ns.adobe.com/xap/1.0/\0";
  const size_t nameSpaceLength = nameSpace.size() + 1;
  const std::string xmp =
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
      "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
      "xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">"
      "<HDRGainMap:HDRGainMapVersion>65536</HDRGainMap:HDRGainMapVersion>"
      "</rdf:Description></rdf:RDF></x:xmpmeta>";
  std::vector<uint8_t> xmpData;
  xmpData.insert(xmpData.end(), reinterpret_cast<const uint8_t*>(nameSpace.c_str()),
                 reinterpret_cast<const uint8_t*>(nameSpace.c_str()) + nameSpaceLength);
  xmpData.insert(xmpData.end(), xmp.begin(), xmp.end());
  return xmpData;
}

static std::vector<uint8_t> appleXmp(const std::string& version, const std::string& headroom,
                                     const std::string& nativeFormat = "",
                                     const std::string& storedFormat = "") {
  const std::string nameSpace = "http://ns.adobe.com/xap/1.0/\0";
  const size_t nameSpaceLength = nameSpace.size() + 1;
  std::string xmp =
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
      "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
      "xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\" "
      "xmlns:apdi=\"http://apple.com/appledigitalcamera/1.0/\">"
      "<HDRGainMap:HDRGainMapVersion>" + version + "</HDRGainMap:HDRGainMapVersion>";
  if (!headroom.empty()) {
    xmp += "<HDRGainMap:HDRGainMapHeadroom>" + headroom + "</HDRGainMap:HDRGainMapHeadroom>";
  }
  if (!nativeFormat.empty()) {
    xmp += "<apdi:NativeFormat>" + nativeFormat + "</apdi:NativeFormat>";
  }
  if (!storedFormat.empty()) {
    xmp += "<apdi:StoredFormat>" + storedFormat + "</apdi:StoredFormat>";
  }
  xmp += "</rdf:Description></rdf:RDF></x:xmpmeta>";
  std::vector<uint8_t> data;
  data.insert(data.end(), reinterpret_cast<const uint8_t*>(nameSpace.c_str()),
              reinterpret_cast<const uint8_t*>(nameSpace.c_str()) + nameSpaceLength);
  data.insert(data.end(), xmp.begin(), xmp.end());
  return data;
}

static void putU16(std::vector<uint8_t>& v, size_t pos, uint16_t x, bool bigEndian) {
  v[pos + (bigEndian ? 0 : 1)] = x >> 8;
  v[pos + (bigEndian ? 1 : 0)] = x & 0xff;
}

static void putU32(std::vector<uint8_t>& v, size_t pos, uint32_t x, bool bigEndian) {
  putU16(v, pos + (bigEndian ? 0 : 2), x >> 16, bigEndian);
  putU16(v, pos + (bigEndian ? 2 : 0), x & 0xffff, bigEndian);
}

// EXIF with little endian TIFF data: IFD0 holds an Exif sub-IFD pointer, the sub-IFD
// holds the Maker Notes, which are Apple Maker Notes with tags 33 and 48 (rationals).
// Offsets of the IFDs and rationals are parameters, relative to the TIFF header and the
// Maker Notes respectively.
struct AppleExifOffsets {
  uint32_t ifd0 = 8;
  uint32_t exifIfd = 26;
  uint32_t makerNotes = 44;
  uint32_t tag33 = 46;
  uint32_t tag48 = 54;
};

static std::vector<uint8_t> appleExif(const AppleExifOffsets& o) {
  const size_t kTiff = 6;
  std::vector<uint8_t> exif(kTiff + 44 + 62, 0);
  memcpy(exif.data(), "Exif\0\0II*\0", 10);
  putU32(exif, kTiff + 4, o.ifd0, false);
  // IFD0 at TIFF + 8: one entry, the Exif sub-IFD pointer.
  putU16(exif, kTiff + 8, 1, false);
  putU16(exif, kTiff + 10, 0x8769, false);
  putU16(exif, kTiff + 12, 4, false);
  putU32(exif, kTiff + 14, 1, false);
  putU32(exif, kTiff + 18, o.exifIfd, false);
  // Exif sub-IFD at TIFF + 26: one entry, the Maker Notes.
  putU16(exif, kTiff + 26, 1, false);
  putU16(exif, kTiff + 28, 0x927c, false);
  putU16(exif, kTiff + 30, 7, false);
  putU32(exif, kTiff + 32, 62, false);
  putU32(exif, kTiff + 36, o.makerNotes, false);
  // Apple Maker Notes at TIFF + 44, big endian, offsets relative to their start.
  const size_t kMaker = kTiff + 44;
  const uint8_t kHeader[] = {'A', 'p', 'p', 'l', 'e', ' ', 'i', 'O', 'S', 0, 0, 1, 'M', 'M'};
  memcpy(&exif[kMaker], kHeader, sizeof(kHeader));
  putU16(exif, kMaker + 14, 2, true);
  putU16(exif, kMaker + 16, 33, true);
  putU16(exif, kMaker + 18, 10, true);
  putU32(exif, kMaker + 20, 1, true);
  putU32(exif, kMaker + 24, o.tag33, true);
  putU16(exif, kMaker + 28, 48, true);
  putU16(exif, kMaker + 30, 10, true);
  putU32(exif, kMaker + 32, 1, true);
  putU32(exif, kMaker + 36, o.tag48, true);
  // Rationals: tag 33 = 1/2, tag 48 = 5/1000.
  putU32(exif, kMaker + 46, 1, true);
  putU32(exif, kMaker + 50, 2, true);
  putU32(exif, kMaker + 54, 5, true);
  putU32(exif, kMaker + 58, 1000, true);
  return exif;
}

TEST(JpegRTest, readAppleHeadroomFromExif) {
  std::vector<uint8_t> xmpData = appleXmpWithoutHeadroom();
  std::vector<uint8_t> exif = appleExif(AppleExifOffsets());
  uhdr_gainmap_metadata_ext_t metadata;
  ASSERT_EQ(getMetadataFromXMP(xmpData.data(), xmpData.size(), exif.data(), exif.size(), &metadata)
                .error_code,
            UHDR_CODEC_OK);
  // maker33 < 1 and maker48 <= 0.01: stops = -20 * maker48 + 1.8.
  EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, (float)pow(2.0, 1.7));
  EXPECT_FLOAT_EQ(metadata.max_content_boost[0], (float)pow(2.0, 1.7));
}

TEST(JpegRTest, readAppleHeadroomNearIsoFloatBoundary) {
  const size_t maker = 6 + 44;
  std::vector<uint8_t> xmp_data = appleXmpWithoutHeadroom();
  for (const auto& boundary : {std::pair<int32_t, uint32_t>{-62, 35},
                               std::pair<int32_t, uint32_t>{-1785714284, 1000000000}}) {
    std::vector<uint8_t> exif = appleExif(AppleExifOffsets());
    putU32(exif, maker + 46, 1, true);  // maker33 = 1, selecting the -70 slope.
    putU32(exif, maker + 50, 1, true);
    putU32(exif, maker + 54, static_cast<uint32_t>(boundary.first), true);
    putU32(exif, maker + 58, boundary.second, true);

    uhdr_gainmap_metadata_ext_t metadata;
    ASSERT_EQ(getMetadataFromXMP(xmp_data.data(), xmp_data.size(), exif.data(), exif.size(),
                                 &metadata)
                  .error_code,
              UHDR_CODEC_OK);
    if (boundary.first == -62) {
      EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, std::ldexp(1.0f, 127));
    } else {
      EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, std::numeric_limits<float>::max());
    }
  }
}

TEST(JpegRTest, appleExifRequiresCompleteUniqueSignedRationalPair) {
  std::vector<uint8_t> xmp = appleXmpWithoutHeadroom();
  const size_t maker = 6 + 44;
  uhdr_gainmap_metadata_ext_t metadata;

  // Neither field is optional when XMP does not carry a headroom fallback.
  for (const size_t entry : {maker + 16, maker + 28}) {
    std::vector<uint8_t> exif = appleExif(AppleExifOffsets());
    putU16(exif, entry, 49, true);
    EXPECT_EQ(getMetadataFromXMP(xmp.data(), xmp.size(), exif.data(), exif.size(), &metadata)
                  .error_code,
              UHDR_CODEC_ERROR);
  }

  // Apple Maker Note headroom fields must each be one signed rational.
  std::vector<uint8_t> wrongType = appleExif(AppleExifOffsets());
  putU16(wrongType, maker + 30, 5, true);
  EXPECT_EQ(getMetadataFromXMP(xmp.data(), xmp.size(), wrongType.data(), wrongType.size(),
                               &metadata)
                .error_code,
            UHDR_CODEC_ERROR);

  std::vector<uint8_t> wrongCount = appleExif(AppleExifOffsets());
  putU32(wrongCount, maker + 32, 2, true);
  EXPECT_EQ(getMetadataFromXMP(xmp.data(), xmp.size(), wrongCount.data(), wrongCount.size(),
                               &metadata)
                .error_code,
            UHDR_CODEC_ERROR);

  std::vector<uint8_t> zeroDenominator = appleExif(AppleExifOffsets());
  putU32(zeroDenominator, maker + 58, 0, true);
  EXPECT_EQ(getMetadataFromXMP(xmp.data(), xmp.size(), zeroDenominator.data(),
                               zeroDenominator.size(), &metadata)
                .error_code,
            UHDR_CODEC_ERROR);

  // A negative denominator remains valid signed-rational data and is divided normally.
  std::vector<uint8_t> negativeDenominator = appleExif(AppleExifOffsets());
  putU32(negativeDenominator, maker + 54, 5, true);
  putU32(negativeDenominator, maker + 58, static_cast<uint32_t>(-1000), true);
  ASSERT_EQ(getMetadataFromXMP(xmp.data(), xmp.size(), negativeDenominator.data(),
                               negativeDenominator.size(), &metadata)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, static_cast<float>(pow(2.0, 1.9)));

  // Duplicate tag 33 must be rejected even when both original values are well formed.
  std::vector<uint8_t> duplicate = appleExif(AppleExifOffsets());
  duplicate.insert(duplicate.begin() + maker + 40, 12, 0);
  putU16(duplicate, maker + 14, 3, true);
  putU32(duplicate, maker + 24, 58, true);
  putU32(duplicate, maker + 36, 66, true);
  putU16(duplicate, maker + 40, 33, true);
  putU16(duplicate, maker + 42, 10, true);
  putU32(duplicate, maker + 44, 1, true);
  putU32(duplicate, maker + 48, 58, true);
  putU32(duplicate, maker + 52, 0, true);
  EXPECT_EQ(getMetadataFromXMP(xmp.data(), xmp.size(), duplicate.data(), duplicate.size(), &metadata)
                .error_code,
            UHDR_CODEC_ERROR);
}

// Every offset read from the EXIF data must be rejected when it points past the end of the
// data, including values that would wrap around when added to the position of the TIFF
// header or of the Maker Notes.
TEST(JpegRTest, rejectAppleExifOffsetsPastEnd) {
  std::vector<uint8_t> xmpData = appleXmpWithoutHeadroom();
  const size_t exifSize = appleExif(AppleExifOffsets()).size();
  const uint32_t kBadOffsets[] = {(uint32_t)exifSize, (uint32_t)exifSize - 1,
                                  0xffffffffu,        0xfffffffau,
                                  0xfffffff2u,        0xffffffe0u,
                                  0xffffffd2u,        0x80000000u};
  for (uint32_t bad : kBadOffsets) {
    for (int field = 0; field < 5; ++field) {
      AppleExifOffsets o;
      uint32_t* fields[] = {&o.ifd0, &o.exifIfd, &o.makerNotes, &o.tag33, &o.tag48};
      *fields[field] = bad;
      std::vector<uint8_t> exif = appleExif(o);
      uhdr_gainmap_metadata_ext_t metadata;
      EXPECT_EQ(
          getMetadataFromXMP(xmpData.data(), xmpData.size(), exif.data(), exif.size(), &metadata)
              .error_code,
          UHDR_CODEC_ERROR)
          << "field " << field << " offset " << bad;
    }
  }
}

TEST(JpegRTest, readAppleHeadroomXmpAsLinearBoost) {
  const std::string nameSpace = "http://ns.adobe.com/xap/1.0/\0";
  const size_t nameSpaceLength = nameSpace.size() + 1;
  const std::string xmp =
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
      "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
      "xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">"
      "<HDRGainMap:HDRGainMapVersion>131072</HDRGainMap:HDRGainMapVersion>"
      "<HDRGainMap:HDRGainMapHeadroom>4.0</HDRGainMap:HDRGainMapHeadroom>"
      "</rdf:Description></rdf:RDF></x:xmpmeta>";

  std::vector<uint8_t> xmpData;
  xmpData.reserve(nameSpaceLength + xmp.size());
  xmpData.insert(xmpData.end(), reinterpret_cast<const uint8_t*>(nameSpace.c_str()),
                 reinterpret_cast<const uint8_t*>(nameSpace.c_str()) + nameSpaceLength);
  xmpData.insert(xmpData.end(), reinterpret_cast<const uint8_t*>(xmp.c_str()),
                 reinterpret_cast<const uint8_t*>(xmp.c_str()) + xmp.size());

  uhdr_gainmap_metadata_ext_t metadata;
  ASSERT_EQ(getMetadataFromXMP(xmpData.data(), xmpData.size(), /*exif_data=*/nullptr,
                               /*exif_size=*/0, &metadata)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_FLOAT_EQ(metadata.max_content_boost[0], 4.0f);
  EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, 4.0f);
}

TEST(JpegRTest, appleXmpValidatesVersionFormatAndHeadroom) {
  uhdr_gainmap_metadata_ext_t metadata;
  std::vector<uint8_t> good = appleXmp("131072", "4.532783", "1278226488", "1278226488");
  ASSERT_EQ(getMetadataFromXMP(good.data(), good.size(), nullptr, 0, &metadata).error_code,
            UHDR_CODEC_OK);
  EXPECT_TRUE(metadata.is_apple_gainmap);
  EXPECT_EQ(metadata.apple_gainmap_version, 131072u);
  EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, 4.532783f);

  for (const auto& bad : {appleXmp("196608", "4"), appleXmp("131072junk", "4"),
                          appleXmp("131072", "4", "1")}) {
    EXPECT_EQ(getMetadataFromXMP(const_cast<uint8_t*>(bad.data()), bad.size(), nullptr, 0,
                                 &metadata)
                  .error_code,
              UHDR_CODEC_UNSUPPORTED_FEATURE);
  }
  for (const auto& bad : {appleXmp("131072", "0.99"), appleXmp("131072", "NaN"),
                          appleXmp("131072", "4junk")}) {
    EXPECT_EQ(getMetadataFromXMP(const_cast<uint8_t*>(bad.data()), bad.size(), nullptr, 0,
                                 &metadata)
                  .error_code,
              UHDR_CODEC_ERROR);
  }
}

TEST(JpegRTest, appleVersionIsIndependentFromAdobeVersion) {
  auto withEarlierAdobeAttribute = appleXmp("131072", "3.5");
  std::string xml(reinterpret_cast<const char*>(withEarlierAdobeAttribute.data()),
                  withEarlierAdobeAttribute.size());
  const std::string description = "<rdf:Description";
  const size_t description_start = xml.find(description);
  ASSERT_NE(description_start, std::string::npos);
  const size_t attributes_end = xml.find('>', description_start);
  ASSERT_NE(attributes_end, std::string::npos);
  xml.insert(attributes_end,
             " xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\"");
  withEarlierAdobeAttribute.assign(xml.begin(), xml.end());

  uhdr_gainmap_metadata_ext_t metadata;
  ASSERT_EQ(getMetadataFromXMP(withEarlierAdobeAttribute.data(), withEarlierAdobeAttribute.size(),
                               nullptr, 0, &metadata)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_EQ(metadata.apple_gainmap_version, 131072u);

  // A later nested description can also report an Adobe version after the Apple element.
  auto withLaterAdobeAttribute = appleXmp("131072", "3.5");
  std::string later_xml(reinterpret_cast<const char*>(withLaterAdobeAttribute.data()),
                        withLaterAdobeAttribute.size());
  const std::string close_description = "</rdf:Description>";
  const size_t close_at = later_xml.find(close_description);
  ASSERT_NE(close_at, std::string::npos);
  later_xml.insert(close_at,
                   "<rdf:Description xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" "
                   "hdrgm:Version=\"1.0\"/>");
  withLaterAdobeAttribute.assign(later_xml.begin(), later_xml.end());
  ASSERT_EQ(getMetadataFromXMP(withLaterAdobeAttribute.data(), withLaterAdobeAttribute.size(),
                               nullptr, 0, &metadata)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_EQ(metadata.apple_gainmap_version, 131072u);
}

TEST(JpegRTest, parseGainMapMetadataClearsAppleStateOnIsoReuse) {
  JpegR parser;
  uhdr_gainmap_metadata_ext_t metadata;
  std::vector<uint8_t> apple_xmp = appleXmp("131072", "3.5");
  ASSERT_EQ(parser.parseGainMapMetadata(nullptr, 0, apple_xmp.data(), apple_xmp.size(), nullptr, 0,
                                        &metadata)
                .error_code,
            UHDR_CODEC_OK);
  ASSERT_TRUE(metadata.is_apple_gainmap);
  ASSERT_EQ(metadata.apple_gainmap_version, 131072u);

  uhdr_gainmap_metadata_frac fraction;
  fraction.alternateHdrHeadroomN = 2;
  std::vector<uint8_t> encoded_metadata;
  ASSERT_EQ(uhdr_gainmap_metadata_frac::encodeGainmapMetadata(&fraction, encoded_metadata)
                .error_code,
            UHDR_CODEC_OK);
  const std::string iso_namespace = "urn:iso:std:iso:ts:21496:-1\0";
  std::vector<uint8_t> iso_data(iso_namespace.begin(), iso_namespace.end());
  iso_data.push_back(0);
  iso_data.insert(iso_data.end(), encoded_metadata.begin(), encoded_metadata.end());
  ASSERT_EQ(parser.parseGainMapMetadata(iso_data.data(), iso_data.size(), nullptr, 0, nullptr, 0,
                                        &metadata)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_FALSE(metadata.is_apple_gainmap);
  EXPECT_EQ(metadata.apple_gainmap_version, 0u);
}

TEST(JpegRTest, appleHeadroomFallbackAndExifPriority) {
  uhdr_gainmap_metadata_ext_t metadata;
  std::vector<uint8_t> validXmp = appleXmp("65536", "3.5");
  AppleExifOffsets invalidOffsets;
  invalidOffsets.tag33 = 0xffffffffu;
  std::vector<uint8_t> invalidExif = appleExif(invalidOffsets);
  ASSERT_EQ(getMetadataFromXMP(validXmp.data(), validXmp.size(), invalidExif.data(),
                               invalidExif.size(), &metadata)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, 3.5f);

  std::vector<uint8_t> conflictingXmp = appleXmp("65536", "not-a-number");
  std::vector<uint8_t> validExif = appleExif(AppleExifOffsets());
  ASSERT_EQ(getMetadataFromXMP(conflictingXmp.data(), conflictingXmp.size(), validExif.data(),
                               validExif.size(), &metadata)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_FLOAT_EQ(metadata.hdr_capacity_max, static_cast<float>(pow(2.0, 1.7)));

  std::vector<uint8_t> duplicateHeadroom = appleXmp("65536", "3.5");
  const std::string duplicate = "<HDRGainMap:HDRGainMapHeadroom>4</HDRGainMap:HDRGainMapHeadroom>";
  const std::string closingDescription = "</rdf:Description>";
  const std::string payload(reinterpret_cast<const char*>(duplicateHeadroom.data()),
                            duplicateHeadroom.size());
  const size_t insertion = payload.find(closingDescription);
  ASSERT_NE(insertion, std::string::npos);
  duplicateHeadroom.insert(duplicateHeadroom.begin() + insertion, duplicate.begin(), duplicate.end());
  EXPECT_EQ(getMetadataFromXMP(duplicateHeadroom.data(), duplicateHeadroom.size(), nullptr, 0,
                               &metadata)
                .error_code,
            UHDR_CODEC_ERROR);
}

TEST(JpegRTest, decodeApple) {
  JpegR decoder;
  uhdr_compressed_image_t uhdrCompressedImg;
  for (const auto& fileName : {kOldAppleFileName, kNewAppleFileName}) {
    std::ifstream ifs;
    ASSERT_TRUE(openFileHelper(fileName, ifs, std::ios::binary))
        << "Failed to open test image: " << fileName;
    std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
    ASSERT_GT(content.size(), 4u);
    ASSERT_EQ(is_uhdr_image(content.data(), content.size()), 1);
    ASSERT_EQ(uhdr_is_supported_gainmap_image(content.data(), content.size()), 1);

    uhdrCompressedImg.data = content.data();
    uhdrCompressedImg.data_sz = content.size();
    uhdrCompressedImg.capacity = content.size();
    uhdrCompressedImg.cg = UHDR_CG_UNSPECIFIED;
    uhdrCompressedImg.ct = UHDR_CT_UNSPECIFIED;
    uhdrCompressedImg.range = UHDR_CR_UNSPECIFIED;

    uhdr_codec_private_t* dec = uhdr_create_decoder();
    ASSERT_NE(dec, nullptr);
    ASSERT_EQ(uhdr_dec_set_image(dec, &uhdrCompressedImg).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_decode(dec).error_code, UHDR_CODEC_OK);

    const uhdr_mem_block_t* gainMapImg = uhdr_dec_get_gainmap_image(dec);
    ASSERT_NE(gainMapImg, nullptr);
    EXPECT_EQ(uhdr_dec_get_gainmap_width(dec), 192u);
    EXPECT_EQ(uhdr_dec_get_gainmap_height(dec), 256u);

    const uhdr_gainmap_metadata_t* gainmapMetadata = uhdr_dec_get_gainmap_metadata(dec);
    ASSERT_NE(gainmapMetadata, nullptr);

    // A complete Apple Maker Notes pair takes precedence over the conflicting XMP value.
    const double headroom = 8.0;
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(gainmapMetadata->gamma[c], 1.0f);
      EXPECT_EQ(gainmapMetadata->offset_sdr[c], 0.0f);
      EXPECT_EQ(gainmapMetadata->offset_hdr[c], 0.0f);
      EXPECT_EQ(gainmapMetadata->min_content_boost[c], 1.0f);
      EXPECT_FLOAT_EQ(gainmapMetadata->max_content_boost[c], headroom);
    }
    EXPECT_EQ(gainmapMetadata->hdr_capacity_min, 1.0f);
    EXPECT_FLOAT_EQ(gainmapMetadata->hdr_capacity_max, headroom);
    EXPECT_TRUE(gainmapMetadata->use_base_cg);

    // Re-encode the decoded base image, gain map and metadata (API-4). This must succeed as
    // Apple gain-map images carry no ICC profile.
    uhdr_mem_block_t* baseImg = uhdr_dec_get_base_image(dec);
    ASSERT_NE(baseImg, nullptr);
    uhdr_compressed_image_t baseCompressed{baseImg->data, baseImg->data_sz, baseImg->capacity,
                                           UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED,
                                           UHDR_CR_UNSPECIFIED};
    uhdr_compressed_image_t gainmapCompressed{gainMapImg->data, gainMapImg->data_sz,
                                              gainMapImg->capacity, UHDR_CG_UNSPECIFIED,
                                              UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
    uhdr_gainmap_metadata_t metadataCopy = *gainmapMetadata;

    uhdr_codec_private_t* enc = uhdr_create_encoder();
    ASSERT_NE(enc, nullptr);
    uhdr_error_info_t status = uhdr_enc_set_compressed_image(enc, &baseCompressed, UHDR_BASE_IMG);
    ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << status.detail;
    status = uhdr_enc_set_gainmap_image(enc, &gainmapCompressed, &metadataCopy);
    ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << status.detail;
    status = uhdr_encode(enc);
    ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << status.detail;
    uhdr_compressed_image_t* reencoded = uhdr_get_encoded_stream(enc);
    ASSERT_NE(reencoded, nullptr);

    uhdr_codec_private_t* dec2 = uhdr_create_decoder();
    ASSERT_NE(dec2, nullptr);
    ASSERT_EQ(uhdr_dec_set_image(dec2, reencoded).error_code, UHDR_CODEC_OK);
    status = uhdr_decode(dec2);
    ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << status.detail;
    const uhdr_gainmap_metadata_t* reencodedMetadata = uhdr_dec_get_gainmap_metadata(dec2);
    ASSERT_NE(reencodedMetadata, nullptr);
    EXPECT_FLOAT_EQ(reencodedMetadata->hdr_capacity_max, headroom);
    EXPECT_TRUE(reencodedMetadata->use_base_cg);
    uhdr_release_decoder(dec2);
    uhdr_release_encoder(enc);

    uhdr_release_decoder(dec);
  }
}

class JpegRAPIEncodeAndDecodeTest
    : public ::testing::TestWithParam<std::tuple<ultrahdr_color_gamut, ultrahdr_color_gamut>> {
 public:
  JpegRAPIEncodeAndDecodeTest()
      : mP010ColorGamut(std::get<0>(GetParam())), mYuv420ColorGamut(std::get<1>(GetParam())) {};

  const ultrahdr_color_gamut mP010ColorGamut;
  const ultrahdr_color_gamut mYuv420ColorGamut;
};

/* Test Encode API-0 and Decode */
TEST_P(JpegRAPIEncodeAndDecodeTest, EncodeAPI0AndDecodeTest) {
  // reference encode
  UhdrUnCompressedStructWrapper rawImg(kImageWidth, kImageHeight, YCbCr_p010);
  ASSERT_TRUE(rawImg.setImageColorGamut(mP010ColorGamut));
  ASSERT_TRUE(rawImg.allocateMemory());
  ASSERT_TRUE(rawImg.loadRawResource(kYCbCrP010FileName));
  UhdrCompressedStructWrapper jpgImg(kImageWidth, kImageHeight);
  ASSERT_TRUE(jpgImg.allocateMemory());
  JpegR uHdrLib;
  ASSERT_EQ(
      uHdrLib.encodeJPEGR(rawImg.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                          jpgImg.getImageHandle(), kQuality, nullptr),
      JPEGR_NO_ERROR);

  uhdr_codec_private_t* obj = uhdr_create_encoder();
  uhdr_raw_image_t uhdrRawImg{};
  uhdrRawImg.fmt = UHDR_IMG_FMT_24bppYCbCrP010;
  uhdrRawImg.cg = map_internal_cg_to_cg(mP010ColorGamut);
  uhdrRawImg.ct = map_internal_ct_to_ct(ultrahdr_transfer_function::ULTRAHDR_TF_HLG);
  uhdrRawImg.range = UHDR_CR_LIMITED_RANGE;
  uhdrRawImg.w = kImageWidth;
  uhdrRawImg.h = kImageHeight;
  uhdrRawImg.planes[UHDR_PLANE_Y] = rawImg.getImageHandle()->data;
  uhdrRawImg.stride[UHDR_PLANE_Y] = kImageWidth;
  uhdrRawImg.planes[UHDR_PLANE_UV] =
      ((uint8_t*)(rawImg.getImageHandle()->data)) + kImageWidth * kImageHeight * 2;
  uhdrRawImg.stride[UHDR_PLANE_UV] = kImageWidth;
  uhdr_error_info_t status = uhdr_enc_set_raw_image(obj, &uhdrRawImg, UHDR_HDR_IMG);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  status = uhdr_enc_set_quality(obj, kQuality, UHDR_BASE_IMG);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  status = uhdr_enc_set_using_multi_channel_gainmap(obj, false);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  status = uhdr_enc_set_gainmap_scale_factor(obj, 4);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  status = uhdr_enc_set_quality(obj, 85, UHDR_GAIN_MAP_IMG);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  status = uhdr_enc_set_preset(obj, UHDR_USAGE_REALTIME);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  status = uhdr_encode(obj);
  ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
  uhdr_compressed_image_t* compressedImage = uhdr_get_encoded_stream(obj);
  ASSERT_NE(nullptr, compressedImage);
  ASSERT_EQ(jpgImg.getImageHandle()->length, compressedImage->data_sz);
  ASSERT_EQ(0,
            memcmp(jpgImg.getImageHandle()->data, compressedImage->data, compressedImage->data_sz));
  uhdr_release_encoder(obj);

  // encode with luma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2.setImageStride(kImageWidth + 18, 0));
    ASSERT_TRUE(rawImg2.allocateMemory());
    ASSERT_TRUE(rawImg2.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg2.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2.setImageStride(kImageWidth + 18, kImageWidth + 28));
    ASSERT_TRUE(rawImg2.setChromaMode(false));
    ASSERT_TRUE(rawImg2.allocateMemory());
    ASSERT_TRUE(rawImg2.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg2.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));

    uhdr_codec_private_t* obj = uhdr_create_encoder();
    uhdr_raw_image_t uhdrRawImg{};
    uhdrRawImg.fmt = UHDR_IMG_FMT_24bppYCbCrP010;
    uhdrRawImg.cg = map_internal_cg_to_cg(mP010ColorGamut);
    uhdrRawImg.ct = map_internal_ct_to_ct(ultrahdr_transfer_function::ULTRAHDR_TF_HLG);
    uhdrRawImg.range = UHDR_CR_LIMITED_RANGE;
    uhdrRawImg.w = kImageWidth;
    uhdrRawImg.h = kImageHeight;
    uhdrRawImg.planes[UHDR_PLANE_Y] = rawImg2.getImageHandle()->data;
    uhdrRawImg.stride[UHDR_PLANE_Y] = rawImg2.getImageHandle()->luma_stride;
    uhdrRawImg.planes[UHDR_PLANE_UV] = rawImg2.getImageHandle()->chroma_data;
    uhdrRawImg.stride[UHDR_PLANE_UV] = rawImg2.getImageHandle()->chroma_stride;
    uhdr_error_info_t status = uhdr_enc_set_raw_image(obj, &uhdrRawImg, UHDR_HDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_quality(obj, kQuality, UHDR_BASE_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_using_multi_channel_gainmap(obj, false);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_gainmap_scale_factor(obj, 4);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_quality(obj, 85, UHDR_GAIN_MAP_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_preset(obj, UHDR_USAGE_REALTIME);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_encode(obj);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    uhdr_compressed_image_t* compressedImage = uhdr_get_encoded_stream(obj);
    ASSERT_NE(nullptr, compressedImage);
    ASSERT_EQ(jpg1->length, compressedImage->data_sz);
    ASSERT_EQ(0, memcmp(jpg1->data, compressedImage->data, jpg1->length));
    uhdr_release_encoder(obj);
  }
  // encode with chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2.setImageStride(0, kImageWidth + 34));
    ASSERT_TRUE(rawImg2.setChromaMode(false));
    ASSERT_TRUE(rawImg2.allocateMemory());
    ASSERT_TRUE(rawImg2.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg2.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set but no chroma ptr
  {
    UhdrUnCompressedStructWrapper rawImg2(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2.setImageStride(kImageWidth, kImageWidth + 38));
    ASSERT_TRUE(rawImg2.allocateMemory());
    ASSERT_TRUE(rawImg2.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2.getImageHandle(), ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                            jpgImg2.getImageHandle(), kQuality, nullptr),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }

  auto jpg1 = jpgImg.getImageHandle();
#ifdef DUMP_OUTPUT
  if (!writeFile("encode_api0_output.jpeg", jpg1->data, jpg1->length)) {
    std::cerr << "unable to write output file" << std::endl;
  }
#endif

  ASSERT_NO_FATAL_FAILURE(decodeJpegRImg(jpg1, "decode_api0_output.rgb"));
}

/* Test Encode API-1 and Decode */
TEST_P(JpegRAPIEncodeAndDecodeTest, EncodeAPI1AndDecodeTest) {
  UhdrUnCompressedStructWrapper rawImgP010(kImageWidth, kImageHeight, YCbCr_p010);
  ASSERT_TRUE(rawImgP010.setImageColorGamut(mP010ColorGamut));
  ASSERT_TRUE(rawImgP010.allocateMemory());
  ASSERT_TRUE(rawImgP010.loadRawResource(kYCbCrP010FileName));
  UhdrUnCompressedStructWrapper rawImg420(kImageWidth, kImageHeight, YCbCr_420);
  ASSERT_TRUE(rawImg420.setImageColorGamut(mYuv420ColorGamut));
  ASSERT_TRUE(rawImg420.allocateMemory());
  ASSERT_TRUE(rawImg420.loadRawResource(kYCbCr420FileName));
  UhdrCompressedStructWrapper jpgImg(kImageWidth, kImageHeight);
  ASSERT_TRUE(jpgImg.allocateMemory());
  JpegR uHdrLib;
  ASSERT_EQ(uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg420.getImageHandle(),
                                ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                jpgImg.getImageHandle(), kQuality, nullptr),
            JPEGR_NO_ERROR);
  // encode with luma stride set p010
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 128, 0));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), rawImg420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set p010
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 128, kImageWidth + 256));
    ASSERT_TRUE(rawImg2P010.setChromaMode(false));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), rawImg420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with chroma stride set p010
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(0, kImageWidth + 64));
    ASSERT_TRUE(rawImg2P010.setChromaMode(false));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), rawImg420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set but no chroma ptr p010
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 64, kImageWidth + 256));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), rawImg420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma stride set 420
  {
    UhdrUnCompressedStructWrapper rawImg2420(kImageWidth, kImageHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2420.setImageColorGamut(mYuv420ColorGamut));
    ASSERT_TRUE(rawImg2420.setImageStride(kImageWidth + 14, 0));
    ASSERT_TRUE(rawImg2420.allocateMemory());
    ASSERT_TRUE(rawImg2420.loadRawResource(kYCbCr420FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg2420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set 420
  {
    UhdrUnCompressedStructWrapper rawImg2420(kImageWidth, kImageHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2420.setImageColorGamut(mYuv420ColorGamut));
    ASSERT_TRUE(rawImg2420.setImageStride(kImageWidth + 46, kImageWidth / 2 + 34));
    ASSERT_TRUE(rawImg2420.setChromaMode(false));
    ASSERT_TRUE(rawImg2420.allocateMemory());
    ASSERT_TRUE(rawImg2420.loadRawResource(kYCbCr420FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg2420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));

    uhdr_codec_private_t* obj = uhdr_create_encoder();
    uhdr_raw_image_t uhdrRawImg{};
    uhdrRawImg.fmt = UHDR_IMG_FMT_24bppYCbCrP010;
    uhdrRawImg.cg = map_internal_cg_to_cg(mP010ColorGamut);
    uhdrRawImg.ct = map_internal_ct_to_ct(ultrahdr_transfer_function::ULTRAHDR_TF_HLG);
    uhdrRawImg.range = UHDR_CR_LIMITED_RANGE;
    uhdrRawImg.w = kImageWidth;
    uhdrRawImg.h = kImageHeight;
    uhdrRawImg.planes[UHDR_PLANE_Y] = rawImgP010.getImageHandle()->data;
    uhdrRawImg.stride[UHDR_PLANE_Y] = kImageWidth;
    uhdrRawImg.planes[UHDR_PLANE_UV] =
        ((uint8_t*)(rawImgP010.getImageHandle()->data)) + kImageWidth * kImageHeight * 2;
    uhdrRawImg.stride[UHDR_PLANE_UV] = kImageWidth;
    uhdr_error_info_t status = uhdr_enc_set_raw_image(obj, &uhdrRawImg, UHDR_HDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;

    uhdrRawImg.fmt = UHDR_IMG_FMT_12bppYCbCr420;
    uhdrRawImg.cg = map_internal_cg_to_cg(mYuv420ColorGamut);
    uhdrRawImg.ct = map_internal_ct_to_ct(ultrahdr_transfer_function::ULTRAHDR_TF_SRGB);
    uhdrRawImg.range = UHDR_CR_FULL_RANGE;
    uhdrRawImg.w = kImageWidth;
    uhdrRawImg.h = kImageHeight;
    uhdrRawImg.planes[UHDR_PLANE_Y] = rawImg2420.getImageHandle()->data;
    uhdrRawImg.stride[UHDR_PLANE_Y] = rawImg2420.getImageHandle()->luma_stride;
    uhdrRawImg.planes[UHDR_PLANE_U] = rawImg2420.getImageHandle()->chroma_data;
    uhdrRawImg.stride[UHDR_PLANE_U] = rawImg2420.getImageHandle()->chroma_stride;
    uhdrRawImg.planes[UHDR_PLANE_V] = ((uint8_t*)(rawImg2420.getImageHandle()->chroma_data)) +
                                      rawImg2420.getImageHandle()->chroma_stride * kImageHeight / 2;
    uhdrRawImg.stride[UHDR_PLANE_V] = rawImg2420.getImageHandle()->chroma_stride;
    status = uhdr_enc_set_raw_image(obj, &uhdrRawImg, UHDR_SDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;

    status = uhdr_enc_set_quality(obj, kQuality, UHDR_BASE_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_using_multi_channel_gainmap(obj, false);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_gainmap_scale_factor(obj, 4);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_quality(obj, 85, UHDR_GAIN_MAP_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_preset(obj, UHDR_USAGE_REALTIME);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_encode(obj);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    uhdr_compressed_image_t* compressedImage = uhdr_get_encoded_stream(obj);
    ASSERT_NE(nullptr, compressedImage);
    ASSERT_EQ(jpgImg.getImageHandle()->length, compressedImage->data_sz);
    ASSERT_EQ(
        0, memcmp(jpgImg.getImageHandle()->data, compressedImage->data, compressedImage->data_sz));
    uhdr_release_encoder(obj);
  }
  // encode with chroma stride set 420
  {
    UhdrUnCompressedStructWrapper rawImg2420(kImageWidth, kImageHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2420.setImageColorGamut(mYuv420ColorGamut));
    ASSERT_TRUE(rawImg2420.setImageStride(0, kImageWidth / 2 + 38));
    ASSERT_TRUE(rawImg2420.setChromaMode(false));
    ASSERT_TRUE(rawImg2420.allocateMemory());
    ASSERT_TRUE(rawImg2420.loadRawResource(kYCbCr420FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg2420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set but no chroma ptr 420
  {
    UhdrUnCompressedStructWrapper rawImg2420(kImageWidth, kImageHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2420.setImageColorGamut(mYuv420ColorGamut));
    ASSERT_TRUE(rawImg2420.setImageStride(kImageWidth + 26, kImageWidth / 2 + 44));
    ASSERT_TRUE(rawImg2420.allocateMemory());
    ASSERT_TRUE(rawImg2420.loadRawResource(kYCbCr420FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg2420.getImageHandle(),
                                  ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                  jpgImg2.getImageHandle(), kQuality, nullptr),
              JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }

  auto jpg1 = jpgImg.getImageHandle();

#ifdef DUMP_OUTPUT
  if (!writeFile("encode_api1_output.jpeg", jpg1->data, jpg1->length)) {
    std::cerr << "unable to write output file" << std::endl;
  }
#endif

  ASSERT_NO_FATAL_FAILURE(decodeJpegRImg(jpg1, "decode_api1_output.rgb"));
}

/* Test Encode API-2 and Decode */
TEST_P(JpegRAPIEncodeAndDecodeTest, EncodeAPI2AndDecodeTest) {
  UhdrUnCompressedStructWrapper rawImgP010(kImageWidth, kImageHeight, YCbCr_p010);
  ASSERT_TRUE(rawImgP010.setImageColorGamut(mP010ColorGamut));
  ASSERT_TRUE(rawImgP010.allocateMemory());
  ASSERT_TRUE(rawImgP010.loadRawResource(kYCbCrP010FileName));
  UhdrUnCompressedStructWrapper rawImg420(kImageWidth, kImageHeight, YCbCr_420);
  ASSERT_TRUE(rawImg420.setImageColorGamut(mYuv420ColorGamut));
  ASSERT_TRUE(rawImg420.allocateMemory());
  ASSERT_TRUE(rawImg420.loadRawResource(kYCbCr420FileName));
  UhdrCompressedStructWrapper jpgImg(kImageWidth, kImageHeight);
  ASSERT_TRUE(jpgImg.allocateMemory());
  UhdrCompressedStructWrapper jpgSdr(kImageWidth, kImageHeight);
  ASSERT_TRUE(jpgSdr.allocateMemory());
  auto sdr = jpgSdr.getImageHandle();
  ASSERT_TRUE(readFile(kSdrJpgFileName, sdr->data, sdr->maxLength, sdr->length));
  JpegR uHdrLib;
  ASSERT_EQ(
      uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg420.getImageHandle(), sdr,
                          ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
      JPEGR_NO_ERROR);
  // encode with luma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 128, 0));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), rawImg420.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 128, kImageWidth + 256));
    ASSERT_TRUE(rawImg2P010.setChromaMode(false));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), rawImg420.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(0, kImageWidth + 64));
    ASSERT_TRUE(rawImg2P010.setChromaMode(false));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), rawImg420.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2420(kImageWidth, kImageHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2420.setImageColorGamut(mYuv420ColorGamut));
    ASSERT_TRUE(rawImg2420.setImageStride(kImageWidth + 128, 0));
    ASSERT_TRUE(rawImg2420.allocateMemory());
    ASSERT_TRUE(rawImg2420.loadRawResource(kYCbCr420FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg2420.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2420(kImageWidth, kImageHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2420.setImageColorGamut(mYuv420ColorGamut));
    ASSERT_TRUE(rawImg2420.setImageStride(kImageWidth + 128, kImageWidth + 256));
    ASSERT_TRUE(rawImg2420.setChromaMode(false));
    ASSERT_TRUE(rawImg2420.allocateMemory());
    ASSERT_TRUE(rawImg2420.loadRawResource(kYCbCr420FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg2420.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));

    uhdr_codec_private_t* obj = uhdr_create_encoder();
    uhdr_raw_image_t uhdrRawImg{};
    uhdrRawImg.fmt = UHDR_IMG_FMT_24bppYCbCrP010;
    uhdrRawImg.cg = map_internal_cg_to_cg(mP010ColorGamut);
    uhdrRawImg.ct = map_internal_ct_to_ct(ultrahdr_transfer_function::ULTRAHDR_TF_HLG);
    uhdrRawImg.range = UHDR_CR_LIMITED_RANGE;
    uhdrRawImg.w = kImageWidth;
    uhdrRawImg.h = kImageHeight;
    uhdrRawImg.planes[UHDR_PLANE_Y] = rawImgP010.getImageHandle()->data;
    uhdrRawImg.stride[UHDR_PLANE_Y] = kImageWidth;
    uhdrRawImg.planes[UHDR_PLANE_UV] =
        ((uint8_t*)(rawImgP010.getImageHandle()->data)) + kImageWidth * kImageHeight * 2;
    uhdrRawImg.stride[UHDR_PLANE_UV] = kImageWidth;
    uhdr_error_info_t status = uhdr_enc_set_raw_image(obj, &uhdrRawImg, UHDR_HDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;

    uhdrRawImg.fmt = UHDR_IMG_FMT_12bppYCbCr420;
    uhdrRawImg.cg = map_internal_cg_to_cg(mYuv420ColorGamut);
    uhdrRawImg.ct = map_internal_ct_to_ct(ultrahdr_transfer_function::ULTRAHDR_TF_SRGB);
    uhdrRawImg.range = UHDR_CR_FULL_RANGE;
    uhdrRawImg.w = kImageWidth;
    uhdrRawImg.h = kImageHeight;
    uhdrRawImg.planes[UHDR_PLANE_Y] = rawImg2420.getImageHandle()->data;
    uhdrRawImg.stride[UHDR_PLANE_Y] = rawImg2420.getImageHandle()->luma_stride;
    uhdrRawImg.planes[UHDR_PLANE_U] = rawImg2420.getImageHandle()->chroma_data;
    uhdrRawImg.stride[UHDR_PLANE_U] = rawImg2420.getImageHandle()->chroma_stride;
    uhdrRawImg.planes[UHDR_PLANE_V] = ((uint8_t*)(rawImg2420.getImageHandle()->chroma_data)) +
                                      rawImg2420.getImageHandle()->chroma_stride * kImageHeight / 2;
    uhdrRawImg.stride[UHDR_PLANE_V] = rawImg2420.getImageHandle()->chroma_stride;
    status = uhdr_enc_set_raw_image(obj, &uhdrRawImg, UHDR_SDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;

    uhdr_compressed_image_t uhdrCompressedImg;
    uhdrCompressedImg.data = sdr->data;
    uhdrCompressedImg.data_sz = sdr->length;
    uhdrCompressedImg.capacity = sdr->length;
    uhdrCompressedImg.cg = map_internal_cg_to_cg(sdr->colorGamut);
    uhdrCompressedImg.ct = UHDR_CT_UNSPECIFIED;
    uhdrCompressedImg.range = UHDR_CR_UNSPECIFIED;
    status = uhdr_enc_set_compressed_image(obj, &uhdrCompressedImg, UHDR_SDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;

    status = uhdr_enc_set_quality(obj, kQuality, UHDR_BASE_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_using_multi_channel_gainmap(obj, false);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_gainmap_scale_factor(obj, 4);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_quality(obj, 85, UHDR_GAIN_MAP_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_preset(obj, UHDR_USAGE_REALTIME);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_encode(obj);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    uhdr_compressed_image_t* compressedImage = uhdr_get_encoded_stream(obj);
    ASSERT_NE(nullptr, compressedImage);
    ASSERT_EQ(jpgImg.getImageHandle()->length, compressedImage->data_sz);
    ASSERT_EQ(
        0, memcmp(jpgImg.getImageHandle()->data, compressedImage->data, compressedImage->data_sz));
    uhdr_release_encoder(obj);
  }
  // encode with chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2420(kImageWidth, kImageHeight, YCbCr_420);
    ASSERT_TRUE(rawImg2420.setImageColorGamut(mYuv420ColorGamut));
    ASSERT_TRUE(rawImg2420.setImageStride(0, kImageWidth + 64));
    ASSERT_TRUE(rawImg2420.setChromaMode(false));
    ASSERT_TRUE(rawImg2420.allocateMemory());
    ASSERT_TRUE(rawImg2420.loadRawResource(kYCbCr420FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), rawImg2420.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }

  auto jpg1 = jpgImg.getImageHandle();

#ifdef DUMP_OUTPUT
  if (!writeFile("encode_api2_output.jpeg", jpg1->data, jpg1->length)) {
    std::cerr << "unable to write output file" << std::endl;
  }
#endif

  ASSERT_NO_FATAL_FAILURE(decodeJpegRImg(jpg1, "decode_api2_output.rgb"));
}

/* Test Encode API-3 and Decode */
TEST_P(JpegRAPIEncodeAndDecodeTest, EncodeAPI3AndDecodeTest) {
  UhdrUnCompressedStructWrapper rawImgP010(kImageWidth, kImageHeight, YCbCr_p010);
  ASSERT_TRUE(rawImgP010.setImageColorGamut(mP010ColorGamut));
  ASSERT_TRUE(rawImgP010.allocateMemory());
  ASSERT_TRUE(rawImgP010.loadRawResource(kYCbCrP010FileName));
  UhdrCompressedStructWrapper jpgImg(kImageWidth, kImageHeight);
  ASSERT_TRUE(jpgImg.allocateMemory());
  UhdrCompressedStructWrapper jpgSdr(kImageWidth, kImageHeight);
  ASSERT_TRUE(jpgSdr.allocateMemory());
  auto sdr = jpgSdr.getImageHandle();
  ASSERT_TRUE(readFile(kSdrJpgFileName, sdr->data, sdr->maxLength, sdr->length));
  JpegR uHdrLib;
  ASSERT_EQ(
      uHdrLib.encodeJPEGR(rawImgP010.getImageHandle(), sdr,
                          ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg.getImageHandle()),
      JPEGR_NO_ERROR);
  // encode with luma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 128, 0));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 128, kImageWidth + 256));
    ASSERT_TRUE(rawImg2P010.setChromaMode(false));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with chroma stride set
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(0, kImageWidth + 64));
    ASSERT_TRUE(rawImg2P010.setChromaMode(false));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }
  // encode with luma and chroma stride set and no chroma ptr
  {
    UhdrUnCompressedStructWrapper rawImg2P010(kImageWidth, kImageHeight, YCbCr_p010);
    ASSERT_TRUE(rawImg2P010.setImageColorGamut(mP010ColorGamut));
    ASSERT_TRUE(rawImg2P010.setImageStride(kImageWidth + 32, kImageWidth + 256));
    ASSERT_TRUE(rawImg2P010.allocateMemory());
    ASSERT_TRUE(rawImg2P010.loadRawResource(kYCbCrP010FileName));
    UhdrCompressedStructWrapper jpgImg2(kImageWidth, kImageHeight);
    ASSERT_TRUE(jpgImg2.allocateMemory());
    ASSERT_EQ(
        uHdrLib.encodeJPEGR(rawImg2P010.getImageHandle(), sdr,
                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG, jpgImg2.getImageHandle()),
        JPEGR_NO_ERROR);
    auto jpg1 = jpgImg.getImageHandle();
    auto jpg2 = jpgImg2.getImageHandle();
    ASSERT_EQ(jpg1->length, jpg2->length);
    ASSERT_EQ(0, memcmp(jpg1->data, jpg2->data, jpg1->length));
  }

  {
    uhdr_codec_private_t* obj = uhdr_create_encoder();
    uhdr_raw_image_t uhdrRawImg{};
    uhdrRawImg.fmt = UHDR_IMG_FMT_24bppYCbCrP010;
    uhdrRawImg.cg = map_internal_cg_to_cg(mP010ColorGamut);
    uhdrRawImg.ct = map_internal_ct_to_ct(ultrahdr_transfer_function::ULTRAHDR_TF_HLG);
    uhdrRawImg.range = UHDR_CR_LIMITED_RANGE;
    uhdrRawImg.w = kImageWidth;
    uhdrRawImg.h = kImageHeight;
    uhdrRawImg.planes[UHDR_PLANE_Y] = rawImgP010.getImageHandle()->data;
    uhdrRawImg.stride[UHDR_PLANE_Y] = kImageWidth;
    uhdrRawImg.planes[UHDR_PLANE_UV] =
        ((uint8_t*)(rawImgP010.getImageHandle()->data)) + kImageWidth * kImageHeight * 2;
    uhdrRawImg.stride[UHDR_PLANE_UV] = kImageWidth;
    uhdr_error_info_t status = uhdr_enc_set_raw_image(obj, &uhdrRawImg, UHDR_HDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;

    uhdr_compressed_image_t uhdrCompressedImg;
    uhdrCompressedImg.data = sdr->data;
    uhdrCompressedImg.data_sz = sdr->length;
    uhdrCompressedImg.capacity = sdr->length;
    uhdrCompressedImg.cg = map_internal_cg_to_cg(sdr->colorGamut);
    uhdrCompressedImg.ct = UHDR_CT_UNSPECIFIED;
    uhdrCompressedImg.range = UHDR_CR_UNSPECIFIED;
    status = uhdr_enc_set_compressed_image(obj, &uhdrCompressedImg, UHDR_SDR_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;

    status = uhdr_enc_set_quality(obj, kQuality, UHDR_BASE_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_using_multi_channel_gainmap(obj, false);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_gainmap_scale_factor(obj, 4);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_quality(obj, 85, UHDR_GAIN_MAP_IMG);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_enc_set_preset(obj, UHDR_USAGE_REALTIME);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    status = uhdr_encode(obj);
    ASSERT_EQ(UHDR_CODEC_OK, status.error_code) << status.detail;
    uhdr_compressed_image_t* compressedImage = uhdr_get_encoded_stream(obj);
    ASSERT_NE(nullptr, compressedImage);
    ASSERT_EQ(jpgImg.getImageHandle()->length, compressedImage->data_sz);
    ASSERT_EQ(
        0, memcmp(jpgImg.getImageHandle()->data, compressedImage->data, compressedImage->data_sz));
    uhdr_release_encoder(obj);
  }

  auto jpg1 = jpgImg.getImageHandle();

#ifdef DUMP_OUTPUT
  if (!writeFile("encode_api3_output.jpeg", jpg1->data, jpg1->length)) {
    std::cerr << "unable to write output file" << std::endl;
  }
#endif

  ASSERT_NO_FATAL_FAILURE(decodeJpegRImg(jpg1, "decode_api3_output.rgb"));
}

INSTANTIATE_TEST_SUITE_P(
    JpegRAPIParameterizedTests, JpegRAPIEncodeAndDecodeTest,
    ::testing::Combine(::testing::Values(ULTRAHDR_COLORGAMUT_BT709, ULTRAHDR_COLORGAMUT_P3,
                                         ULTRAHDR_COLORGAMUT_BT2100),
                       ::testing::Values(ULTRAHDR_COLORGAMUT_BT709, ULTRAHDR_COLORGAMUT_P3,
                                         ULTRAHDR_COLORGAMUT_BT2100)));

// ============================================================================
// Profiling
// ============================================================================
#ifdef _WIN32
class Profiler {
 public:
  void timerStart() { QueryPerformanceCounter(&mStartingTime); }

  void timerStop() { QueryPerformanceCounter(&mEndingTime); }

  double elapsedTime() {
    LARGE_INTEGER frequency;
    LARGE_INTEGER elapsedMicroseconds;
    QueryPerformanceFrequency(&frequency);
    elapsedMicroseconds.QuadPart = mEndingTime.QuadPart - mStartingTime.QuadPart;
    return (double)elapsedMicroseconds.QuadPart / (double)frequency.QuadPart * 1000000;
  }

 private:
  LARGE_INTEGER mStartingTime;
  LARGE_INTEGER mEndingTime;
};
#else
class Profiler {
 public:
  void timerStart() { gettimeofday(&mStartingTime, nullptr); }

  void timerStop() { gettimeofday(&mEndingTime, nullptr); }

  int64_t elapsedTime() {
    struct timeval elapsedMicroseconds;
    elapsedMicroseconds.tv_sec = mEndingTime.tv_sec - mStartingTime.tv_sec;
    elapsedMicroseconds.tv_usec = mEndingTime.tv_usec - mStartingTime.tv_usec;
    return elapsedMicroseconds.tv_sec * 1000000 + elapsedMicroseconds.tv_usec;
  }

 private:
  struct timeval mStartingTime;
  struct timeval mEndingTime;
};
#endif

class JpegRBenchmark : public JpegR {
 public:
#ifdef UHDR_ENABLE_GLES
  JpegRBenchmark(uhdr_opengl_ctxt_t* uhdrGLCtxt) : JpegR(uhdrGLCtxt) {}
#endif
  void BenchmarkGenerateGainMap(uhdr_raw_image_t* yuv420Image, uhdr_raw_image_t* p010Image,
                                uhdr_gainmap_metadata_ext_t* metadata,
                                std::unique_ptr<uhdr_raw_image_ext_t>& gainmap);
  void BenchmarkApplyGainMap(uhdr_raw_image_t* yuv420Image, uhdr_raw_image_t* map,
                             uhdr_gainmap_metadata_ext_t* metadata, uhdr_raw_image_t* dest);

 private:
  const int kProfileCount = 10;
};

void JpegRBenchmark::BenchmarkGenerateGainMap(uhdr_raw_image_t* yuv420Image,
                                              uhdr_raw_image_t* p010Image,
                                              uhdr_gainmap_metadata_ext_t* metadata,
                                              std::unique_ptr<uhdr_raw_image_ext_t>& gainmap) {
  ASSERT_EQ(yuv420Image->w, p010Image->w);
  ASSERT_EQ(yuv420Image->h, p010Image->h);
  Profiler profileGenerateMap;
  profileGenerateMap.timerStart();
  for (auto i = 0; i < kProfileCount; i++) {
    ASSERT_EQ(UHDR_CODEC_OK, generateGainMap(yuv420Image, p010Image, metadata, gainmap).error_code);
  }
  profileGenerateMap.timerStop();
  ALOGV("Generate Gain Map:- Res = %u x %u, time = %f ms", yuv420Image->w, yuv420Image->h,
        profileGenerateMap.elapsedTime() / (kProfileCount * 1000.f));
}

void JpegRBenchmark::BenchmarkApplyGainMap(uhdr_raw_image_t* yuv420Image, uhdr_raw_image_t* map,
                                           uhdr_gainmap_metadata_ext_t* metadata,
                                           uhdr_raw_image_t* dest) {
  Profiler profileRecMap;
  profileRecMap.timerStart();
  for (auto i = 0; i < kProfileCount; i++) {
    ASSERT_EQ(UHDR_CODEC_OK, applyGainMap(yuv420Image, map, metadata, UHDR_CT_HLG,
                                          UHDR_IMG_FMT_32bppRGBA1010102, FLT_MAX, dest)
                                 .error_code);
  }
  profileRecMap.timerStop();
  ALOGV("Apply Gain Map:- Res = %u x %u, time = %f ms", yuv420Image->w, yuv420Image->h,
        profileRecMap.elapsedTime() / (kProfileCount * 1000.f));
}

TEST(JpegRTest, ProfileGainMapFuncs) {
  UhdrUnCompressedStructWrapper rawImgP010(kImageWidth, kImageHeight, YCbCr_p010);
  ASSERT_TRUE(rawImgP010.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100));
  ASSERT_TRUE(rawImgP010.allocateMemory());
  ASSERT_TRUE(rawImgP010.loadRawResource(kYCbCrP010FileName));
  UhdrUnCompressedStructWrapper rawImg420(kImageWidth, kImageHeight, YCbCr_420);
  ASSERT_TRUE(rawImg420.setImageColorGamut(ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709));
  ASSERT_TRUE(rawImg420.allocateMemory());
  ASSERT_TRUE(rawImg420.loadRawResource(kYCbCr420FileName));
  uhdr_gainmap_metadata_ext_t metadata(kJpegrVersion);

  uhdr_raw_image_t hdr_intent, sdr_intent;

  {
    auto rawImg = rawImgP010.getImageHandle();
    if (rawImg->luma_stride == 0) rawImg->luma_stride = rawImg->width;
    if (!rawImg->chroma_data) {
      uint16_t* data = reinterpret_cast<uint16_t*>(rawImg->data);
      rawImg->chroma_data = data + rawImg->luma_stride * rawImg->height;
      rawImg->chroma_stride = rawImg->luma_stride;
    }
    hdr_intent.fmt = UHDR_IMG_FMT_24bppYCbCrP010;
    hdr_intent.cg = UHDR_CG_BT_2100;
    hdr_intent.ct = UHDR_CT_HLG;
    hdr_intent.range = UHDR_CR_LIMITED_RANGE;
    hdr_intent.w = rawImg->width;
    hdr_intent.h = rawImg->height;
    hdr_intent.planes[UHDR_PLANE_Y] = rawImg->data;
    hdr_intent.stride[UHDR_PLANE_Y] = rawImg->luma_stride;
    hdr_intent.planes[UHDR_PLANE_UV] = rawImg->chroma_data;
    hdr_intent.stride[UHDR_PLANE_UV] = rawImg->chroma_stride;
    hdr_intent.planes[UHDR_PLANE_V] = nullptr;
    hdr_intent.stride[UHDR_PLANE_V] = 0;
  }
  {
    auto rawImg = rawImg420.getImageHandle();
    if (rawImg->luma_stride == 0) rawImg->luma_stride = rawImg->width;
    if (!rawImg->chroma_data) {
      uint8_t* data = reinterpret_cast<uint8_t*>(rawImg->data);
      rawImg->chroma_data = data + rawImg->luma_stride * rawImg->height;
      rawImg->chroma_stride = rawImg->luma_stride / 2;
    }
    sdr_intent.fmt = UHDR_IMG_FMT_12bppYCbCr420;
    sdr_intent.cg = UHDR_CG_DISPLAY_P3;
    sdr_intent.ct = UHDR_CT_SRGB;
    sdr_intent.range = rawImg->colorRange;
    sdr_intent.w = rawImg->width;
    sdr_intent.h = rawImg->height;
    sdr_intent.planes[UHDR_PLANE_Y] = rawImg->data;
    sdr_intent.stride[UHDR_PLANE_Y] = rawImg->luma_stride;
    sdr_intent.planes[UHDR_PLANE_U] = rawImg->chroma_data;
    sdr_intent.stride[UHDR_PLANE_U] = rawImg->chroma_stride;
    uint8_t* data = reinterpret_cast<uint8_t*>(rawImg->chroma_data);
    data += (rawImg->height * rawImg->chroma_stride) / 2;
    sdr_intent.planes[UHDR_PLANE_V] = data;
    sdr_intent.stride[UHDR_PLANE_V] = rawImg->chroma_stride;
  }

  std::unique_ptr<uhdr_raw_image_ext_t> gainmap;

#ifdef UHDR_ENABLE_GLES
  uhdr_opengl_ctxt_t glCtxt;
  glCtxt.init_opengl_ctxt();
  JpegRBenchmark benchmark(glCtxt.mErrorStatus.error_code == UHDR_CODEC_OK ? &glCtxt : nullptr);
#else
  JpegRBenchmark benchmark;
#endif

  ASSERT_NO_FATAL_FAILURE(
      benchmark.BenchmarkGenerateGainMap(&sdr_intent, &hdr_intent, &metadata, gainmap));

  const int dstSize = kImageWidth * kImageWidth * 4;
  auto bufferDst = std::make_unique<uint8_t[]>(dstSize);
  uhdr_raw_image_t output;
  output.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
  output.cg = UHDR_CG_UNSPECIFIED;
  output.ct = UHDR_CT_UNSPECIFIED;
  output.range = UHDR_CR_UNSPECIFIED;
  output.w = kImageWidth;
  output.h = kImageHeight;
  output.planes[UHDR_PLANE_PACKED] = bufferDst.get();
  output.stride[UHDR_PLANE_PACKED] = kImageWidth;
  output.planes[UHDR_PLANE_U] = nullptr;
  output.stride[UHDR_PLANE_U] = 0;
  output.planes[UHDR_PLANE_V] = nullptr;
  output.stride[UHDR_PLANE_V] = 0;

  ASSERT_NO_FATAL_FAILURE(
      benchmark.BenchmarkApplyGainMap(&sdr_intent, gainmap.get(), &metadata, &output));

#ifdef UHDR_ENABLE_GLES
  glCtxt.delete_opengl_ctxt();
#endif
}

}  // namespace ultrahdr
