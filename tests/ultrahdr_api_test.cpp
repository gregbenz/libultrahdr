#if defined(__has_include)
#if __has_include("testing/base/public/gunit.h")
#include "testing/base/public/gunit.h"
#else
#include <gtest/gtest.h>
#endif
#else
#include <gtest/gtest.h>
#endif
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <memory>
#include "ultrahdr_api.h"
#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/jpegr.h"
#include "ultrahdr/jpegencoderhelper.h"
#include "ultrahdr/gainmapmath.h"
#include "ultrahdr/gainmapmetadata.h"
#include "ultrahdr/heifultrahdr.h"
#include "ultrahdr/avifultrahdr.h"
#if defined(UHDR_ENABLE_HEIF)
#include "libheif/heif.h"
#endif

namespace ultrahdr {

static const char* kYCbCrP010FileName = "raw_p010_image.p010";
static const char* kYCbCr420FileName = "raw_yuv420_image.yuv420";
static const char* kSdrJpgFileName = "jpeg_image.jpg";
static const char* kHevcGainmapFileName = "gainmap_hevc_16x16.heic";
static const size_t kImageWidth = 1280;
static const size_t kImageHeight = 720;

static bool loadFile(const char* filename, std::vector<uint8_t>& buffer) {
  std::vector<std::string> candidates = {
      filename,
      std::string("third_party/libultrahdr/tests/data/") + filename,
      std::string("tests/data/") + filename,
      std::string("./data/") + filename,
      std::string("data/") + filename,
      std::string("../tests/data/") + filename,
  };
  for (const auto& path : candidates) {
    std::ifstream stream(path, std::ios::binary);
    if (stream.is_open()) {
      stream.seekg(0, std::ios::end);
      size_t size = stream.tellg();
      stream.seekg(0, std::ios::beg);
      buffer.resize(size);
      stream.read(reinterpret_cast<char*>(buffer.data()), size);
      if (stream.good()) return true;
    }
  }
  return false;
}

static bool readMpfU16ForTest(const std::vector<uint8_t>& data, size_t offset, bool big_endian,
                              uint16_t* value) {
  if (offset > data.size() || data.size() - offset < 2) return false;
  *value = big_endian ? static_cast<uint16_t>((data[offset] << 8) | data[offset + 1])
                      : static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
  return true;
}

static bool readMpfU32ForTest(const std::vector<uint8_t>& data, size_t offset, bool big_endian,
                              uint32_t* value) {
  if (offset > data.size() || data.size() - offset < 4) return false;
  if (big_endian) {
    *value = (static_cast<uint32_t>(data[offset]) << 24) |
             (static_cast<uint32_t>(data[offset + 1]) << 16) |
             (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
  } else {
    *value = data[offset] | (static_cast<uint32_t>(data[offset + 1]) << 8) |
             (static_cast<uint32_t>(data[offset + 2]) << 16) |
             (static_cast<uint32_t>(data[offset + 3]) << 24);
  }
  return true;
}

static void writeMpfU16ForTest(std::vector<uint8_t>& data, size_t offset, bool big_endian,
                               uint16_t value) {
  data[offset] = static_cast<uint8_t>(value >> (big_endian ? 8 : 0));
  data[offset + 1] = static_cast<uint8_t>(value >> (big_endian ? 0 : 8));
}

static void writeMpfU32ForTest(std::vector<uint8_t>& data, size_t offset, bool big_endian,
                               uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    const int shift = big_endian ? 24 - 8 * i : 8 * i;
    data[offset + i] = static_cast<uint8_t>(value >> shift);
  }
}

struct MpfEntryLocations {
  bool big_endian;
  size_t signature;
  size_t ifd;
  size_t number_of_images_tag;
  size_t mp_entry_tag;
  size_t mp_entry_value;
  size_t version_id;
  size_t version_type;
  size_t version_count;
  size_t version_value;
  size_t count;
  size_t primary_attributes;
  size_t primary_offset;
  size_t secondary_attributes;
  size_t secondary_offset;
};

static bool findMpfEntryLocations(const std::vector<uint8_t>& data, MpfEntryLocations* locations) {
  const uint8_t signature[] = {'M', 'P', 'F', 0};
  const auto found =
      std::search(data.begin(), data.end(), std::begin(signature), std::end(signature));
  if (found == data.end()) return false;
  const size_t signature_offset = static_cast<size_t>(found - data.begin());
  const size_t tiff = signature_offset + sizeof signature;
  if (tiff > data.size() || data.size() - tiff < 8) return false;
  const bool big_endian = data[tiff] == 'M' && data[tiff + 1] == 'M';
  if (!big_endian && !(data[tiff] == 'I' && data[tiff + 1] == 'I')) return false;

  uint32_t ifd_relative_offset = 0;
  if (!readMpfU32ForTest(data, tiff + 4, big_endian, &ifd_relative_offset) ||
      ifd_relative_offset > data.size() - tiff) {
    return false;
  }
  const size_t ifd = tiff + ifd_relative_offset;
  uint16_t tag_count = 0;
  if (!readMpfU16ForTest(data, ifd, big_endian, &tag_count)) return false;
  bool found_version = false;
  bool found_number_of_images = false;
  bool found_entries = false;
  locations->big_endian = big_endian;
  locations->signature = signature_offset;
  locations->ifd = ifd;
  for (uint16_t i = 0; i < tag_count; ++i) {
    const size_t tag = ifd + 2 + 12 * i;
    uint16_t id = 0;
    uint32_t entries_offset = 0;
    if (!readMpfU16ForTest(data, tag, big_endian, &id) ||
        !readMpfU32ForTest(data, tag + 8, big_endian, &entries_offset)) {
      return false;
    }
    if (id == 0xb000) {
      locations->version_id = tag;
      locations->version_type = tag + 2;
      locations->version_count = tag + 4;
      locations->version_value = tag + 8;
      found_version = true;
      continue;
    }
    if (id == 0xb001) {
      locations->number_of_images_tag = tag;
      found_number_of_images = true;
      continue;
    }
    if (id != 0xb002) continue;
    if (entries_offset > data.size() - tiff || data.size() - (tiff + entries_offset) < 32) {
      return false;
    }
    const size_t entries = tiff + entries_offset;
    locations->count = tag + 4;
    locations->mp_entry_tag = tag;
    locations->mp_entry_value = tag + 8;
    locations->primary_attributes = entries;
    locations->primary_offset = entries + 8;
    locations->secondary_attributes = entries + 16;
    locations->secondary_offset = entries + 24;
    found_entries = true;
  }
  return found_version && found_number_of_images && found_entries;
}

static bool insertDuplicateMpfTag(std::vector<uint8_t>& data, uint16_t tag_id) {
  MpfEntryLocations locations;
  if (!findMpfEntryLocations(data, &locations) || locations.signature < 2) return false;

  uint16_t tag_count = 0;
  uint16_t app2_length = 0;
  uint32_t entries_offset = 0;
  uint32_t secondary_offset = 0;
  if (!readMpfU16ForTest(data, locations.ifd, locations.big_endian, &tag_count) ||
      !readMpfU16ForTest(data, locations.signature - 2, true, &app2_length) ||
      !readMpfU32ForTest(data, locations.mp_entry_value, locations.big_endian, &entries_offset) ||
      !readMpfU32ForTest(data, locations.secondary_offset, locations.big_endian,
                         &secondary_offset)) {
    return false;
  }
  const size_t source_tag =
      tag_id == 0xb001 ? locations.number_of_images_tag : locations.mp_entry_tag;
  const size_t insertion = locations.ifd + 2 + static_cast<size_t>(tag_count) * 12;
  if (source_tag > data.size() || data.size() - source_tag < 12 || insertion > data.size() ||
      app2_length > UINT16_MAX - 12) {
    return false;
  }

  const std::vector<uint8_t> duplicate(data.begin() + source_tag, data.begin() + source_tag + 12);
  data.insert(data.begin() + insertion, duplicate.begin(), duplicate.end());
  writeMpfU16ForTest(data, locations.ifd, locations.big_endian, tag_count + 1);
  writeMpfU16ForTest(data, locations.signature - 2, true, app2_length + 12);
  writeMpfU32ForTest(data, locations.mp_entry_value, locations.big_endian, entries_offset + 12);
  if (tag_id == 0xb002) {
    writeMpfU32ForTest(data, insertion + 8, locations.big_endian, entries_offset + 12);
  }
  writeMpfU32ForTest(data, locations.secondary_offset + 12, locations.big_endian,
                     secondary_offset + 12);
  return true;
}

#if defined(UHDR_ENABLE_HEIF)
static void expectSupportedHeifGainmap(const void* data, size_t size, bool decoder_available) {
#if defined(UHDR_HAS_HEIF_ITEM_API)
  EXPECT_EQ(uhdr_is_supported_gainmap_image(data, size), decoder_available ? 1 : 0);
#else
  // The legacy HEIF gain-map APIs keep encode/decode coverage available, but routing requires
  // the optional item-inspection API. The documented fallback is an unsupported routing result.
  EXPECT_EQ(uhdr_is_supported_gainmap_image(data, size), 0);
  (void)decoder_available;
#endif
}

static heif_error writeHeifToVector([[maybe_unused]] heif_context* context, const void* data,
                                    size_t size, void* userdata) {
  auto* output = static_cast<std::vector<uint8_t>*>(userdata);
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  output->insert(output->end(), bytes, bytes + size);
  return {heif_error_Ok, heif_suberror_Unspecified, nullptr};
}

static bool encodeOrdinaryAvif(std::vector<uint8_t>& output) {
  constexpr int width = 16;
  constexpr int height = 16;
  heif_context* context = heif_context_alloc();
  heif_image* image = nullptr;
  heif_encoder* encoder = nullptr;
  heif_image_handle* handle = nullptr;
  bool success = false;
  if (context == nullptr) return false;

  if (heif_image_create(width, height, heif_colorspace_YCbCr, heif_chroma_420, &image).code !=
          heif_error_Ok ||
      heif_image_add_plane(image, heif_channel_Y, width, height, 8).code != heif_error_Ok ||
      heif_image_add_plane(image, heif_channel_Cb, width / 2, height / 2, 8).code !=
          heif_error_Ok ||
      heif_image_add_plane(image, heif_channel_Cr, width / 2, height / 2, 8).code !=
          heif_error_Ok) {
    goto cleanup;
  }
  for (const heif_channel channel : {heif_channel_Y, heif_channel_Cb, heif_channel_Cr}) {
    int stride = 0;
    uint8_t* plane = heif_image_get_plane(image, channel, &stride);
    const int plane_height = channel == heif_channel_Y ? height : height / 2;
    if (plane == nullptr || stride <= 0) goto cleanup;
    std::fill(plane, plane + stride * plane_height, 128);
  }
  if (heif_context_get_encoder_for_format(context, heif_compression_AV1, &encoder).code !=
          heif_error_Ok ||
      heif_context_encode_image(context, image, encoder, nullptr, &handle).code != heif_error_Ok) {
    goto cleanup;
  }
  {
    heif_writer writer{1, writeHeifToVector};
    success = heif_context_write(context, &writer, &output).code == heif_error_Ok;
  }

cleanup:
  if (handle != nullptr) heif_image_handle_release(handle);
  if (encoder != nullptr) heif_encoder_release(encoder);
  if (image != nullptr) heif_image_release(image);
  heif_context_free(context);
  return success;
}

using HeifContextPtr = std::unique_ptr<heif_context, decltype(&heif_context_free)>;
using HeifEncoderPtr = std::unique_ptr<heif_encoder, decltype(&heif_encoder_release)>;
using HeifImagePtr = std::unique_ptr<heif_image, decltype(&heif_image_release)>;
using HeifHandlePtr =
    std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)>;
using HeifOptionsPtr =
    std::unique_ptr<heif_encoding_options, decltype(&heif_encoding_options_free)>;
using HeifProfilePtr =
    std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)>;

static bool fillHeifPlane(heif_image* image, heif_channel channel, int width, int height,
                          uint8_t value) {
  int stride = 0;
  uint8_t* plane = heif_image_get_plane(image, channel, &stride);
  if (plane == nullptr || stride < width) return false;
  for (int y = 0; y < height; ++y) {
    std::fill(plane + y * stride, plane + y * stride + width, value);
  }
  return true;
}

static HeifProfilePtr makeNclxProfile(uint16_t primaries, uint16_t transfer, uint16_t matrix) {
  HeifProfilePtr profile(heif_nclx_color_profile_alloc(), heif_nclx_color_profile_free);
  if (profile == nullptr ||
      heif_nclx_color_profile_set_color_primaries(profile.get(), primaries).code !=
          heif_error_Ok ||
      heif_nclx_color_profile_set_transfer_characteristics(profile.get(), transfer).code !=
          heif_error_Ok ||
      heif_nclx_color_profile_set_matrix_coefficients(profile.get(), matrix).code !=
          heif_error_Ok) {
    return HeifProfilePtr(nullptr, heif_nclx_color_profile_free);
  }
  profile->full_range_flag = 1;
  return profile;
}

static bool encodeGainmapSideAlphaAvif(std::vector<uint8_t>& output) {
  constexpr int base_width = 64;
  constexpr int base_height = 64;
  constexpr int gainmap_width = 32;
  constexpr int gainmap_height = 32;

  HeifContextPtr context(heif_context_alloc(), heif_context_free);
  if (context == nullptr) return false;

  heif_image* base_raw = nullptr;
  if (heif_image_create(base_width, base_height, heif_colorspace_YCbCr, heif_chroma_420,
                        &base_raw)
              .code != heif_error_Ok) {
    return false;
  }
  HeifImagePtr base(base_raw, heif_image_release);
  if (heif_image_add_plane(base.get(), heif_channel_Y, base_width, base_height, 8).code !=
          heif_error_Ok ||
      heif_image_add_plane(base.get(), heif_channel_Cb, base_width / 2, base_height / 2, 8)
              .code != heif_error_Ok ||
      heif_image_add_plane(base.get(), heif_channel_Cr, base_width / 2, base_height / 2, 8)
              .code != heif_error_Ok ||
      !fillHeifPlane(base.get(), heif_channel_Y, base_width, base_height, 128) ||
      !fillHeifPlane(base.get(), heif_channel_Cb, base_width / 2, base_height / 2, 128) ||
      !fillHeifPlane(base.get(), heif_channel_Cr, base_width / 2, base_height / 2, 128)) {
    return false;
  }

  heif_image* gainmap_raw = nullptr;
  if (heif_image_create(gainmap_width, gainmap_height, heif_colorspace_monochrome,
                        heif_chroma_monochrome, &gainmap_raw)
              .code != heif_error_Ok) {
    return false;
  }
  HeifImagePtr gainmap(gainmap_raw, heif_image_release);
  if (heif_image_add_plane(gainmap.get(), heif_channel_Y, gainmap_width, gainmap_height, 8)
              .code != heif_error_Ok ||
      heif_image_add_plane(gainmap.get(), heif_channel_Alpha, gainmap_width, gainmap_height, 8)
              .code != heif_error_Ok ||
      !fillHeifPlane(gainmap.get(), heif_channel_Y, gainmap_width, gainmap_height, 96) ||
      !fillHeifPlane(gainmap.get(), heif_channel_Alpha, gainmap_width, gainmap_height, 128)) {
    return false;
  }
  heif_image_set_premultiplied_alpha(gainmap.get(), 0);

  HeifProfilePtr base_profile =
      makeNclxProfile(heif_color_primaries_ITU_R_BT_709_5,
                      heif_transfer_characteristic_ITU_R_BT_709_5,
                      heif_matrix_coefficients_ITU_R_BT_709_5);
  HeifProfilePtr gainmap_profile =
      makeNclxProfile(heif_color_primaries_unspecified, heif_transfer_characteristic_unspecified,
                      heif_matrix_coefficients_ITU_R_BT_601_6);
  HeifProfilePtr derived_profile =
      makeNclxProfile(heif_color_primaries_ITU_R_BT_2020_2_and_2100_0,
                      heif_transfer_characteristic_ITU_R_BT_2100_0_HLG,
                      heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance);
  if (base_profile == nullptr || gainmap_profile == nullptr || derived_profile == nullptr ||
      heif_image_set_nclx_color_profile(base.get(), base_profile.get()).code != heif_error_Ok ||
      heif_image_set_nclx_color_profile(gainmap.get(), gainmap_profile.get()).code !=
          heif_error_Ok) {
    return false;
  }

  heif_encoder* encoder_raw = nullptr;
  if (heif_context_get_encoder_for_format(context.get(), heif_compression_AV1, &encoder_raw)
              .code != heif_error_Ok) {
    return false;
  }
  HeifEncoderPtr encoder(encoder_raw, heif_encoder_release);
  if (heif_encoder_set_lossy_quality(encoder.get(), 100).code != heif_error_Ok) return false;

  HeifOptionsPtr options(heif_encoding_options_alloc(), heif_encoding_options_free);
  if (options == nullptr) return false;
  options->save_alpha_channel = 1;
  options->output_nclx_profile = base_profile.get();

  heif_image_handle* base_handle_raw = nullptr;
  if (heif_context_encode_image(context.get(), base.get(), encoder.get(), options.get(),
                                &base_handle_raw)
              .code != heif_error_Ok) {
    return false;
  }
  HeifHandlePtr base_handle(base_handle_raw, heif_image_handle_release);

  uhdr_gainmap_metadata_frac metadata;
  metadata.alternateHdrHeadroomN = 2;
  std::vector<uint8_t> iso_metadata;
  if (uhdr_gainmap_metadata_frac::encodeGainmapMetadata(&metadata, iso_metadata).error_code !=
      UHDR_CODEC_OK) {
    return false;
  }

  options->output_nclx_profile = gainmap_profile.get();
  if (heif_encoder_set_lossy_quality(encoder.get(), 100).code != heif_error_Ok) return false;
  heif_image_handle* gainmap_handle_raw = nullptr;
  if (heif_context_encode_gain_map_image(
          context.get(), base_handle.get(), encoder.get(), gainmap.get(), options.get(),
          iso_metadata.data(), static_cast<int>(iso_metadata.size()), derived_profile.get(),
          &gainmap_handle_raw)
          .code != heif_error_Ok) {
    return false;
  }
  HeifHandlePtr gainmap_handle(gainmap_handle_raw, heif_image_handle_release);

  heif_writer writer{1, writeHeifToVector};
  return heif_context_write(context.get(), &writer, &output).code == heif_error_Ok &&
         !output.empty();
}
#endif

static void expectGainMapsEqual(const uhdr_raw_image_t* expected,
                                const uhdr_raw_image_t* actual) {
  ASSERT_NE(expected, nullptr);
  ASSERT_NE(actual, nullptr);
  ASSERT_EQ(actual->fmt, expected->fmt);
  ASSERT_EQ(actual->w, expected->w);
  ASSERT_EQ(actual->h, expected->h);

  const size_t bytes_per_pixel =
      expected->fmt == UHDR_IMG_FMT_8bppYCbCr400 ? 1 : 4;
  const size_t row_bytes = expected->w * bytes_per_pixel;
  const auto* expected_data = static_cast<const uint8_t*>(expected->planes[UHDR_PLANE_PACKED]);
  const auto* actual_data = static_cast<const uint8_t*>(actual->planes[UHDR_PLANE_PACKED]);
  for (unsigned row = 0; row < expected->h; ++row) {
    ASSERT_EQ(0, memcmp(expected_data + row * expected->stride[UHDR_PLANE_PACKED] * bytes_per_pixel,
                        actual_data + row * actual->stride[UHDR_PLANE_PACKED] * bytes_per_pixel,
                        row_bytes))
        << "gain map differs at row " << row;
  }
}

TEST(JpegDecoderDispatchTest, CpuFallbackCoversAllHdrTransfers) {
  for (uhdr_color_transfer_t transfer : {UHDR_CT_LINEAR, UHDR_CT_HLG, UHDR_CT_PQ}) {
    EXPECT_FALSE(shouldUseGlesForJpegDecoder(true, transfer, false));
    EXPECT_FALSE(shouldUseGlesForJpegDecoder(true, transfer, true));
    EXPECT_FALSE(shouldUseGlesForJpegDecoder(false, transfer, true));
  }
  EXPECT_FALSE(shouldUseGlesForJpegDecoder(true, UHDR_CT_SRGB, false));
  EXPECT_TRUE(shouldUseGlesForJpegDecoder(true, UHDR_CT_SRGB, true));
  EXPECT_FALSE(shouldUseGlesForJpegDecoder(false, UHDR_CT_SRGB, true));
}

class UltraHdrApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(loadFile(kYCbCrP010FileName, mP010Data));
    ASSERT_TRUE(loadFile(kYCbCr420FileName, mYuv420Data));
    ASSERT_TRUE(loadFile(kSdrJpgFileName, mSdrJpgData));

    // Setup HDR raw image descriptor (P010, HLG, BT2100)
    mHdrRaw.fmt = UHDR_IMG_FMT_24bppYCbCrP010;
    mHdrRaw.cg = UHDR_CG_BT_2100;
    mHdrRaw.ct = UHDR_CT_HLG;
    mHdrRaw.range = UHDR_CR_FULL_RANGE;
    mHdrRaw.w = kImageWidth;
    mHdrRaw.h = kImageHeight;
    mHdrRaw.planes[UHDR_PLANE_Y] = mP010Data.data();
    mHdrRaw.planes[UHDR_PLANE_UV] = mP010Data.data() + kImageWidth * kImageHeight * 2;
    mHdrRaw.stride[UHDR_PLANE_Y] = kImageWidth;
    mHdrRaw.stride[UHDR_PLANE_UV] = kImageWidth;

    // Setup SDR raw image descriptor (YUV420, sRGB, BT709)
    mSdrRaw.fmt = UHDR_IMG_FMT_12bppYCbCr420;
    mSdrRaw.cg = UHDR_CG_BT_709;
    mSdrRaw.ct = UHDR_CT_SRGB;
    mSdrRaw.range = UHDR_CR_FULL_RANGE;
    mSdrRaw.w = kImageWidth;
    mSdrRaw.h = kImageHeight;
    mSdrRaw.planes[UHDR_PLANE_Y] = mYuv420Data.data();
    mSdrRaw.planes[UHDR_PLANE_U] = mYuv420Data.data() + kImageWidth * kImageHeight;
    mSdrRaw.planes[UHDR_PLANE_V] = mYuv420Data.data() + kImageWidth * kImageHeight * 5 / 4;
    mSdrRaw.stride[UHDR_PLANE_Y] = kImageWidth;
    mSdrRaw.stride[UHDR_PLANE_U] = kImageWidth / 2;
    mSdrRaw.stride[UHDR_PLANE_V] = kImageWidth / 2;

    // Setup SDR compressed image descriptor (JPEG)
    mSdrCompressed.data = mSdrJpgData.data();
    mSdrCompressed.data_sz = mSdrJpgData.size();
    mSdrCompressed.capacity = mSdrJpgData.size();
    mSdrCompressed.cg = UHDR_CG_BT_709;
    mSdrCompressed.ct = UHDR_CT_SRGB;
    mSdrCompressed.range = UHDR_CR_FULL_RANGE;
  }

  std::vector<uint8_t> mP010Data;
  std::vector<uint8_t> mYuv420Data;
  std::vector<uint8_t> mSdrJpgData;
  uhdr_raw_image_t mHdrRaw{};
  uhdr_raw_image_t mSdrRaw{};
  uhdr_compressed_image_t mSdrCompressed{};
};

#if defined(UHDR_ENABLE_HEIF)
static heif_transfer_characteristics getPrimaryImageTransfer(
    const uhdr_compressed_image_t* image) {
  heif_transfer_characteristics transfer = heif_transfer_characteristic_unspecified;
  heif_context* context = heif_context_alloc();
  if (context == nullptr) {
    ADD_FAILURE() << "failed to allocate libheif context";
    return transfer;
  }

  heif_image_handle* primary_handle = nullptr;
  heif_color_profile_nclx* nclx = nullptr;
  heif_error error =
      heif_context_read_from_memory_without_copy(context, image->data, image->data_sz, nullptr);
  if (error.code != heif_error_Ok) {
    ADD_FAILURE() << "failed to parse encoded image";
    goto CleanUp;
  }
  error = heif_context_get_primary_image_handle(context, &primary_handle);
  if (error.code != heif_error_Ok) {
    ADD_FAILURE() << "failed to get primary image";
    goto CleanUp;
  }
  error = heif_image_handle_get_nclx_color_profile(primary_handle, &nclx);
  if (error.code != heif_error_Ok) {
    ADD_FAILURE() << "failed to get primary image nclx profile";
    goto CleanUp;
  }
  transfer = nclx->transfer_characteristics;

CleanUp:
  if (nclx != nullptr) heif_nclx_color_profile_free(nclx);
  if (primary_handle != nullptr) heif_image_handle_release(primary_handle);
  heif_context_free(context);
  return transfer;
}
#endif

TEST_F(UltraHdrApiTest, GainmapExportPreservesGenericJpegAndMapDimensions) {
  constexpr unsigned int map_width = 32;
  constexpr unsigned int map_height = 24;
  std::vector<uint8_t> map_pixels(map_width * map_height);
  for (unsigned int y = 0; y < map_height; ++y) {
    for (unsigned int x = 0; x < map_width; ++x) {
      map_pixels[y * map_width + x] = static_cast<uint8_t>((x * 7 + y * 11) & 0xff);
    }
  }
  uhdr_raw_image_t map_image{};
  map_image.fmt = UHDR_IMG_FMT_8bppYCbCr400;
  map_image.cg = UHDR_CG_UNSPECIFIED;
  map_image.ct = UHDR_CT_SRGB;
  map_image.range = UHDR_CR_FULL_RANGE;
  map_image.w = map_width;
  map_image.h = map_height;
  map_image.planes[UHDR_PLANE_Y] = map_pixels.data();
  map_image.stride[UHDR_PLANE_Y] = map_width;

  JpegEncoderHelper map_encoder;
  ASSERT_EQ(map_encoder.compressImage(&map_image, 100, nullptr, 0).error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t map_jpeg = map_encoder.getCompressedImage();

  uhdr_gainmap_metadata_ext_t metadata("1.0");
  std::fill_n(metadata.min_content_boost, 3, 1.0f);
  std::fill_n(metadata.max_content_boost, 3, 4.0f);
  std::fill_n(metadata.gamma, 3, 1.0f);
  std::fill_n(metadata.offset_sdr, 3, 0.0f);
  std::fill_n(metadata.offset_hdr, 3, 0.0f);
  metadata.hdr_capacity_min = 1.0f;
  metadata.hdr_capacity_max = 4.0f;
  metadata.use_base_cg = true;

  uhdr_compressed_image_ext_t jpegr(UHDR_CG_BT_709, UHDR_CT_UNSPECIFIED,
                                    UHDR_CR_UNSPECIFIED,
                                    mSdrCompressed.data_sz + map_jpeg.data_sz + 1024 * 1024);
  JpegR composer;
  ASSERT_EQ(composer.encodeJPEGR(&mSdrCompressed, &map_jpeg, &metadata, &jpegr).error_code,
            UHDR_CODEC_OK);
  jpeg_info_struct base_info;
  jpeg_info_struct gainmap_info;
  jpegr_info_struct container_info;
  container_info.primaryImgInfo = &base_info;
  container_info.gainmapImgInfo = &gainmap_info;
  ASSERT_EQ(composer.getJPEGRInfo(&jpegr, &container_info).error_code, UHDR_CODEC_OK);

  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &jpegr).error_code, UHDR_CODEC_OK);

  uhdr_gainmap_export_t exported{};
  ASSERT_EQ(uhdr_dec_export_gainmap(decoder.get(), &exported).error_code, UHDR_CODEC_OK);
  ASSERT_NE(exported.image.data, nullptr);
  EXPECT_EQ(exported.width, map_width);
  EXPECT_EQ(exported.height, map_height);
  ASSERT_EQ(exported.image.data_sz, gainmap_info.imgData.size());
  EXPECT_EQ(std::memcmp(exported.image.data, gainmap_info.imgData.data(),
                        gainmap_info.imgData.size()),
            0);

  EXPECT_EQ(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code,
            UHDR_CODEC_INVALID_OPERATION);
  ASSERT_EQ(uhdr_decode(decoder.get()).error_code, UHDR_CODEC_OK);
  uhdr_gainmap_export_t repeated{};
  ASSERT_EQ(uhdr_dec_export_gainmap(decoder.get(), &repeated).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(repeated.image.data, exported.image.data);
  EXPECT_EQ(repeated.image.data_sz, exported.image.data_sz);
  EXPECT_EQ(repeated.width, map_width);
  EXPECT_EQ(repeated.height, map_height);

  uhdr_reset_decoder(decoder.get());
  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &jpegr).error_code, UHDR_CODEC_OK);
  uhdr_gainmap_export_t after_reset{};
  ASSERT_EQ(uhdr_dec_export_gainmap(decoder.get(), &after_reset).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(after_reset.width, map_width);
  EXPECT_EQ(after_reset.height, map_height);
  ASSERT_EQ(after_reset.image.data_sz, gainmap_info.imgData.size());
  EXPECT_EQ(std::memcmp(after_reset.image.data, gainmap_info.imgData.data(),
                        gainmap_info.imgData.size()),
            0);

  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> invalid_decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(invalid_decoder, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(invalid_decoder.get(), &jpegr).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_probe(invalid_decoder.get()).error_code, UHDR_CODEC_OK);
  auto* decoder_handle = dynamic_cast<uhdr_decoder_private*>(invalid_decoder.get());
  ASSERT_NE(decoder_handle, nullptr);
  // Emulate metadata fields that parse successfully but fail API4's semantic descriptor contract.
  decoder_handle->m_metadata.gamma[0] = 0.0f;
  uint8_t sentinel = 0x5a;
  uhdr_gainmap_export_t invalid_result{};
  invalid_result.image.data = &sentinel;
  invalid_result.image.data_sz = 1;
  invalid_result.image.capacity = 1;
  invalid_result.width = 17;
  invalid_result.height = 19;
  EXPECT_EQ(uhdr_dec_export_gainmap(invalid_decoder.get(), &invalid_result).error_code,
            UHDR_CODEC_INVALID_PARAM);
  EXPECT_EQ(invalid_result.image.data, &sentinel);
  EXPECT_EQ(invalid_result.image.data_sz, 1u);
  EXPECT_EQ(invalid_result.width, 17u);
  EXPECT_EQ(invalid_result.height, 19u);
}

TEST_F(UltraHdrApiTest, GainmapExportFailureLeavesResultUntouched) {
  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &mSdrCompressed).error_code, UHDR_CODEC_OK);

  uint8_t sentinel = 0x5a;
  uhdr_gainmap_export_t result{};
  result.image.data = &sentinel;
  result.image.data_sz = 1;
  result.image.capacity = 1;
  result.width = 17;
  result.height = 19;
  EXPECT_NE(uhdr_dec_export_gainmap(decoder.get(), &result).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(result.image.data, &sentinel);
  EXPECT_EQ(result.image.data_sz, 1u);
  EXPECT_EQ(result.width, 17u);
  EXPECT_EQ(result.height, 19u);
}

TEST_F(UltraHdrApiTest, GainmapExportRejectsNonJpegWithoutFreezingDecoder) {
  uint8_t sentinel = 0x5a;
  auto makeResult = [&]() {
    uhdr_gainmap_export_t result{};
    result.image.data = &sentinel;
    result.image.data_sz = 1;
    result.image.capacity = 1;
    result.width = 17;
    result.height = 19;
    result.metadata.max_content_boost[0] = 3.25f;
    return result;
  };
  auto expectUnchanged = [&](const uhdr_gainmap_export_t& result) {
    EXPECT_EQ(result.image.data, &sentinel);
    EXPECT_EQ(result.image.data_sz, 1u);
    EXPECT_EQ(result.image.capacity, 1u);
    EXPECT_EQ(result.width, 17u);
    EXPECT_EQ(result.height, 19u);
    EXPECT_FLOAT_EQ(result.metadata.max_content_boost[0], 3.25f);
  };

  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> no_input_decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(no_input_decoder, nullptr);
  uhdr_gainmap_export_t no_input_result = makeResult();
  EXPECT_EQ(uhdr_dec_export_gainmap(no_input_decoder.get(), &no_input_result).error_code,
            UHDR_CODEC_INVALID_OPERATION);
  expectUnchanged(no_input_result);

  std::vector<uint8_t> avif_data;
  ASSERT_TRUE(loadFile("ordinary_synthetic.avif", avif_data));
  ASSERT_GE(avif_data.size(), 12u);
  EXPECT_EQ(std::string(avif_data.begin() + 4, avif_data.begin() + 8), "ftyp");
  EXPECT_EQ(std::string(avif_data.begin() + 8, avif_data.begin() + 12), "avif");
  uhdr_compressed_image_t avif_image{};
  avif_image.data = avif_data.data();
  avif_image.data_sz = avif_data.size();
  avif_image.capacity = avif_data.size();
  avif_image.cg = UHDR_CG_UNSPECIFIED;
  avif_image.ct = UHDR_CT_UNSPECIFIED;
  avif_image.range = UHDR_CR_UNSPECIFIED;

  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &avif_image).error_code, UHDR_CODEC_OK);
  uhdr_gainmap_export_t result = makeResult();
  EXPECT_EQ(uhdr_dec_export_gainmap(decoder.get(), &result).error_code,
            UHDR_CODEC_UNSUPPORTED_FEATURE);
  expectUnchanged(result);

  const uint8_t short_payload[] = {0x01};
  uhdr_compressed_image_t short_image{};
  short_image.data = const_cast<uint8_t*>(short_payload);
  short_image.data_sz = sizeof(short_payload);
  short_image.capacity = sizeof(short_payload);
  short_image.cg = UHDR_CG_UNSPECIFIED;
  short_image.ct = UHDR_CT_UNSPECIFIED;
  short_image.range = UHDR_CR_UNSPECIFIED;
  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &short_image).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_export_gainmap(decoder.get(), &result).error_code,
            UHDR_CODEC_UNSUPPORTED_FEATURE);
  expectUnchanged(result);

  std::vector<uint8_t> apple_data;
  ASSERT_TRUE(loadFile("apple_gainmap_new.jpg", apple_data));
  uhdr_compressed_image_t apple_image{};
  apple_image.data = apple_data.data();
  apple_image.data_sz = apple_data.size();
  apple_image.capacity = apple_data.size();
  apple_image.cg = UHDR_CG_UNSPECIFIED;
  apple_image.ct = UHDR_CT_UNSPECIFIED;
  apple_image.range = UHDR_CR_UNSPECIFIED;
  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &apple_image).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_export_gainmap(decoder.get(), &result).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(result.width, 384u);
  EXPECT_EQ(result.height, 512u);

  const uint8_t malformed_jpeg[] = {0xff, 0xd8, 0xff};
  uhdr_compressed_image_t malformed_image{};
  malformed_image.data = const_cast<uint8_t*>(malformed_jpeg);
  malformed_image.data_sz = sizeof(malformed_jpeg);
  malformed_image.capacity = sizeof(malformed_jpeg);
  malformed_image.cg = UHDR_CG_UNSPECIFIED;
  malformed_image.ct = UHDR_CT_UNSPECIFIED;
  malformed_image.range = UHDR_CR_UNSPECIFIED;
  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> malformed_decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(malformed_decoder, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(malformed_decoder.get(), &malformed_image).error_code,
            UHDR_CODEC_OK);
  uhdr_gainmap_export_t malformed_result = makeResult();
  EXPECT_EQ(uhdr_dec_export_gainmap(malformed_decoder.get(), &malformed_result).error_code,
            UHDR_CODEC_ERROR);
  expectUnchanged(malformed_result);
}

TEST_F(UltraHdrApiTest, AppleGainmapExportProducesFullResolutionGrayJpeg) {
  std::vector<uint8_t> apple_data;
  ASSERT_TRUE(loadFile("apple_gainmap_new.jpg", apple_data));
  uhdr_compressed_image_t apple_image{};
  apple_image.data = apple_data.data();
  apple_image.data_sz = apple_data.size();
  apple_image.capacity = apple_data.size();
  apple_image.cg = UHDR_CG_UNSPECIFIED;
  apple_image.ct = UHDR_CT_UNSPECIFIED;
  apple_image.range = UHDR_CR_UNSPECIFIED;

  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &apple_image).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_add_effect_crop(decoder.get(), 8, 376, 10, 502).error_code, UHDR_CODEC_OK);
  uhdr_gainmap_export_t exported{};
  ASSERT_EQ(uhdr_dec_export_gainmap(decoder.get(), &exported).error_code, UHDR_CODEC_OK);

  EXPECT_EQ(exported.width, static_cast<unsigned int>(uhdr_dec_get_image_width(decoder.get())));
  EXPECT_EQ(exported.height, static_cast<unsigned int>(uhdr_dec_get_image_height(decoder.get())));
  EXPECT_EQ(exported.width, 384u);
  EXPECT_EQ(exported.height, 512u);
  EXPECT_FLOAT_EQ(exported.metadata.hdr_capacity_max, 8.0f);
  EXPECT_FLOAT_EQ(exported.metadata.min_content_boost[0], 1.0f);
  EXPECT_FLOAT_EQ(exported.metadata.gamma[0], 1.0f);
  EXPECT_EQ(exported.metadata.use_base_cg, 1);

  uhdr_mem_block_t* raw_map = uhdr_dec_get_gainmap_image(decoder.get());
  ASSERT_NE(raw_map, nullptr);
  ASSERT_NE(raw_map->data, nullptr);
  const std::vector<uint8_t> raw_map_bytes(static_cast<const uint8_t*>(raw_map->data),
                                           static_cast<const uint8_t*>(raw_map->data) +
                                               raw_map->data_sz);
  const unsigned int raw_map_width = uhdr_dec_get_gainmap_width(decoder.get());
  const unsigned int raw_map_height = uhdr_dec_get_gainmap_height(decoder.get());
  ASSERT_EQ(raw_map_width, 192u);
  ASSERT_EQ(raw_map_height, 256u);
  const uhdr_gainmap_metadata_t* raw_metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  ASSERT_NE(raw_metadata, nullptr);
  const float raw_headroom = raw_metadata->hdr_capacity_max;
  const std::vector<uint8_t> canonical_map_bytes(
      static_cast<const uint8_t*>(exported.image.data),
      static_cast<const uint8_t*>(exported.image.data) + exported.image.data_sz);
  const uhdr_gainmap_metadata_t canonical_metadata = exported.metadata;

  ASSERT_EQ(uhdr_decode(decoder.get()).error_code, UHDR_CODEC_OK);
  uhdr_raw_image_t* cropped_image = uhdr_get_decoded_image(decoder.get());
  ASSERT_NE(cropped_image, nullptr);
  EXPECT_EQ(cropped_image->w, 368u);
  EXPECT_EQ(cropped_image->h, 492u);
  uhdr_gainmap_export_t repeated{};
  ASSERT_EQ(uhdr_dec_export_gainmap(decoder.get(), &repeated).error_code, UHDR_CODEC_OK);
  ASSERT_NE(repeated.image.data, nullptr);
  ASSERT_EQ(repeated.image.data_sz, canonical_map_bytes.size());
  EXPECT_EQ(std::memcmp(repeated.image.data, canonical_map_bytes.data(), canonical_map_bytes.size()),
            0);
  EXPECT_EQ(repeated.width, exported.width);
  EXPECT_EQ(repeated.height, exported.height);
  EXPECT_FLOAT_EQ(repeated.metadata.max_content_boost[0],
                  canonical_metadata.max_content_boost[0]);
  EXPECT_FLOAT_EQ(repeated.metadata.hdr_capacity_max, canonical_metadata.hdr_capacity_max);

  raw_map = uhdr_dec_get_gainmap_image(decoder.get());
  ASSERT_NE(raw_map, nullptr);
  ASSERT_EQ(raw_map->data_sz, raw_map_bytes.size());
  EXPECT_EQ(std::memcmp(raw_map->data, raw_map_bytes.data(), raw_map_bytes.size()), 0);
  EXPECT_EQ(uhdr_dec_get_gainmap_width(decoder.get()), raw_map_width);
  EXPECT_EQ(uhdr_dec_get_gainmap_height(decoder.get()), raw_map_height);
  raw_metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  ASSERT_NE(raw_metadata, nullptr);
  EXPECT_FLOAT_EQ(raw_metadata->hdr_capacity_max, raw_headroom);

  JpegDecoderHelper exported_map_decoder;
  ASSERT_EQ(exported_map_decoder.parseImage(exported.image.data, exported.image.data_sz)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_EQ(exported_map_decoder.getDecompressedImageWidth(), exported.width);
  EXPECT_EQ(exported_map_decoder.getDecompressedImageHeight(), exported.height);
  EXPECT_EQ(exported_map_decoder.getNumComponentsInImage(), 1u);
  EXPECT_EQ(exported_map_decoder.getDataPrecision(), 8u);
}

TEST_F(UltraHdrApiTest, AppleGainmapExportQ100MatchesCenteredDoubleOracle) {
  struct GrayJpeg {
    std::vector<uint8_t> bytes;
    unsigned int width;
    unsigned int height;
  };
  auto encodeGray = [](const std::vector<uint8_t>& pixels, unsigned int width,
                       unsigned int height) {
    uhdr_raw_image_t image{};
    image.fmt = UHDR_IMG_FMT_8bppYCbCr400;
    image.cg = UHDR_CG_UNSPECIFIED;
    image.ct = UHDR_CT_SRGB;
    image.range = UHDR_CR_FULL_RANGE;
    image.w = width;
    image.h = height;
    image.planes[UHDR_PLANE_Y] = const_cast<uint8_t*>(pixels.data());
    image.stride[UHDR_PLANE_Y] = width;
    JpegEncoderHelper encoder;
    const uhdr_error_info_t status = encoder.compressImage(&image, 100, nullptr, 0);
    if (status.error_code != UHDR_CODEC_OK) return GrayJpeg{};
    const uhdr_compressed_image_t compressed = encoder.getCompressedImage();
    return GrayJpeg{{static_cast<const uint8_t*>(compressed.data),
                     static_cast<const uint8_t*>(compressed.data) + compressed.data_sz},
                    width, height};
  };
  auto encodeBase = [](unsigned int width, unsigned int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    for (unsigned int y = 0; y < height; ++y) {
      for (unsigned int x = 0; x < width; ++x) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 3;
        pixels[i] = static_cast<uint8_t>(32 + (x * 17) % 192);
        pixels[i + 1] = static_cast<uint8_t>(32 + (y * 19) % 192);
        pixels[i + 2] = static_cast<uint8_t>(32 + ((x + y) * 11) % 192);
      }
    }
    uhdr_raw_image_t image{};
    image.fmt = UHDR_IMG_FMT_24bppRGB888;
    image.cg = UHDR_CG_BT_709;
    image.ct = UHDR_CT_SRGB;
    image.range = UHDR_CR_FULL_RANGE;
    image.w = width;
    image.h = height;
    image.planes[UHDR_PLANE_PACKED] = pixels.data();
    image.stride[UHDR_PLANE_PACKED] = width;
    JpegEncoderHelper encoder;
    const uhdr_error_info_t status = encoder.compressImage(&image, 100, nullptr, 0);
    if (status.error_code != UHDR_CODEC_OK) return std::vector<uint8_t>{};
    const uhdr_compressed_image_t compressed = encoder.getCompressedImage();
    return std::vector<uint8_t>(static_cast<const uint8_t*>(compressed.data),
                                static_cast<const uint8_t*>(compressed.data) +
                                    compressed.data_sz);
  };
  auto decodeGray = [](const uint8_t* data, size_t size, GrayJpeg* decoded) {
    JpegDecoderHelper decoder;
    if (decoder.decompressImage(data, size, DECODE_STREAM).error_code != UHDR_CODEC_OK) {
      return false;
    }
    const uhdr_raw_image_t image = decoder.getDecompressedImage();
    if (image.fmt != UHDR_IMG_FMT_8bppYCbCr400 || image.planes[UHDR_PLANE_Y] == nullptr ||
        image.stride[UHDR_PLANE_Y] < image.w) {
      return false;
    }
    decoded->width = image.w;
    decoded->height = image.h;
    decoded->bytes.resize(static_cast<size_t>(image.w) * image.h);
    const auto* plane = static_cast<const uint8_t*>(image.planes[UHDR_PLANE_Y]);
    for (unsigned int y = 0; y < image.h; ++y) {
      std::memcpy(decoded->bytes.data() + static_cast<size_t>(y) * image.w,
                  plane + static_cast<size_t>(y) * image.stride[UHDR_PLANE_Y], image.w);
    }
    return true;
  };

  const std::vector<float> headrooms = {
      1.0f,
      std::nextafter(1.0f, 2.0f),
      2.0f,
      8.0f,
      16.0f,
      32.0f,
      1.0e6f,
      std::ldexp(1.0f, 127),
      std::numeric_limits<float>::max(),
  };
  std::vector<std::pair<std::string, std::vector<uint8_t>>> maps;
  std::vector<uint8_t> ramp(3 * 5);
  for (unsigned int y = 0; y < 5; ++y) {
    for (unsigned int x = 0; x < 3; ++x) {
      ramp[y * 3 + x] = static_cast<uint8_t>((x * 64 + y * 47) & 0xff);
    }
  }
  maps.emplace_back("ramp", ramp);
  std::vector<uint8_t> impulse(3 * 5, 0);
  impulse[2 * 3 + 1] = 255;
  maps.emplace_back("impulse", impulse);
  std::vector<uint8_t> edge(3 * 5);
  for (unsigned int y = 0; y < 5; ++y) {
    for (unsigned int x = 0; x < 3; ++x) edge[y * 3 + x] = x == 0 ? 0 : 255;
  }
  maps.emplace_back("edge", edge);
  std::vector<uint8_t> corners(3 * 5, 0);
  corners[0] = corners[2] = corners[12] = corners[14] = 255;
  maps.emplace_back("corners", corners);
  std::vector<uint8_t> checkerboard(3 * 5);
  for (unsigned int y = 0; y < 5; ++y) {
    for (unsigned int x = 0; x < 3; ++x) {
      checkerboard[y * 3 + x] = ((x + y) & 1) ? 255 : 0;
    }
  }
  maps.emplace_back("checkerboard", checkerboard);
  maps.emplace_back("constant-0", std::vector<uint8_t>{0});
  maps.emplace_back("constant-127", std::vector<uint8_t>{127});
  maps.emplace_back("constant-255", std::vector<uint8_t>{255});

  // These budgets qualify this deterministic synthetic corpus and Q100 helper
  // path. They are not a universal JPEG codec or perceptual error guarantee.
  const double kMaxQError = 2.0 / 255.0;
  for (const auto& [name, source_pixels] : maps) {
    const unsigned int source_width = name.rfind("constant-", 0) == 0 ? 1 : 3;
    const unsigned int source_height = name.rfind("constant-", 0) == 0 ? 1 : 5;
    GrayJpeg source_jpeg = encodeGray(source_pixels, source_width, source_height);
    ASSERT_FALSE(source_jpeg.bytes.empty()) << name;
    GrayJpeg decoded_source{};
    ASSERT_TRUE(decodeGray(source_jpeg.bytes.data(), source_jpeg.bytes.size(), &decoded_source))
        << name;

    for (const auto& geometry : {std::pair<unsigned int, unsigned int>{7, 9}, {31, 33},
                                 {32, 32}}) {
      const unsigned int base_width = geometry.first;
      const unsigned int base_height = geometry.second;
      const std::vector<uint8_t> base_bytes = encodeBase(base_width, base_height);
      ASSERT_FALSE(base_bytes.empty()) << name;
      uhdr_compressed_image_t base_image{};
      base_image.data = const_cast<uint8_t*>(base_bytes.data());
      base_image.data_sz = base_image.capacity = base_bytes.size();
      base_image.cg = UHDR_CG_BT_709;
      base_image.ct = UHDR_CT_UNSPECIFIED;
      base_image.range = UHDR_CR_UNSPECIFIED;
      uhdr_compressed_image_t source_map_image{};
      source_map_image.data = source_jpeg.bytes.data();
      source_map_image.data_sz = source_map_image.capacity = source_jpeg.bytes.size();
      source_map_image.cg = UHDR_CG_UNSPECIFIED;
      source_map_image.ct = UHDR_CT_UNSPECIFIED;
      source_map_image.range = UHDR_CR_UNSPECIFIED;
      uhdr_gainmap_metadata_ext_t source_metadata("1.0");
      std::fill_n(source_metadata.min_content_boost, 3, 1.0f);
      std::fill_n(source_metadata.max_content_boost, 3, 8.0f);
      std::fill_n(source_metadata.gamma, 3, 1.0f);
      std::fill_n(source_metadata.offset_sdr, 3, 0.0f);
      std::fill_n(source_metadata.offset_hdr, 3, 0.0f);
      source_metadata.hdr_capacity_min = 1.0f;
      source_metadata.hdr_capacity_max = 8.0f;
      source_metadata.use_base_cg = true;
      uhdr_compressed_image_ext_t source_jpegr(
          UHDR_CG_BT_709, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED,
          base_bytes.size() + source_jpeg.bytes.size() + 1024 * 1024);
      JpegR composer;
      ASSERT_EQ(composer.encodeJPEGR(&base_image, &source_map_image, &source_metadata,
                                     &source_jpegr)
                    .error_code,
                UHDR_CODEC_OK)
          << name << " " << base_width << "x" << base_height;

      for (const float headroom : headrooms) {
        uhdr_gainmap_metadata_ext_t apple_metadata("1.0");
        apple_metadata.is_apple_gainmap = true;
        apple_metadata.apple_gainmap_version = 131072;
        std::fill_n(apple_metadata.min_content_boost, 3, 1.0f);
        std::fill_n(apple_metadata.max_content_boost, 3, headroom);
        std::fill_n(apple_metadata.gamma, 3, 2.2f);
        std::fill_n(apple_metadata.offset_sdr, 3, 0.0f);
        std::fill_n(apple_metadata.offset_hdr, 3, 0.0f);
        apple_metadata.hdr_capacity_min = 1.0f;
        apple_metadata.hdr_capacity_max = headroom;
        apple_metadata.use_base_cg = true;
        std::unique_ptr<uhdr_compressed_image_ext_t> normalized_jpeg;
        uhdr_gainmap_metadata_ext_t normalized_metadata("1.0");
        unsigned int normalized_width = 0;
        unsigned int normalized_height = 0;
        const uhdr_error_info_t export_status = composer.exportAppleGainMap(
            &source_jpegr, &apple_metadata, &normalized_jpeg, &normalized_metadata,
            &normalized_width, &normalized_height);
        if (headroom == std::numeric_limits<float>::max()) {
          EXPECT_EQ(export_status.error_code, UHDR_CODEC_UNSUPPORTED_FEATURE);
          EXPECT_EQ(normalized_jpeg, nullptr);
          continue;
        }
        ASSERT_EQ(export_status.error_code, UHDR_CODEC_OK)
            << name << " " << base_width << "x" << base_height << " H=" << headroom << " "
            << export_status.detail;
        ASSERT_NE(normalized_jpeg, nullptr);
        EXPECT_EQ(normalized_width, base_width);
        EXPECT_EQ(normalized_height, base_height);
        GrayJpeg decoded_output{};
        ASSERT_TRUE(decodeGray(static_cast<const uint8_t*>(normalized_jpeg->data),
                               normalized_jpeg->data_sz, &decoded_output));
        ASSERT_EQ(decoded_output.width, base_width);
        ASSERT_EQ(decoded_output.height, base_height);

        const double log_headroom = headroom > 1.0f ? std::log(static_cast<double>(headroom)) : 1.0;
        const size_t pixel_count = static_cast<size_t>(base_width) * base_height;
        std::vector<double> q_errors;
        q_errors.reserve(pixel_count);
        std::vector<double> ideal_q_values(pixel_count);
        std::vector<uint8_t> ideal_q8(pixel_count);
        std::vector<double> q_rounding_errors_codes;
        q_rounding_errors_codes.reserve(pixel_count);
        double max_identity_error_stops = 0.0;
        for (unsigned int y = 0; y < base_height; ++y) {
          for (unsigned int x = 0; x < base_width; ++x) {
            const double sx = std::clamp((x + 0.5) * source_width / base_width - 0.5, 0.0,
                                         static_cast<double>(source_width - 1));
            const double sy = std::clamp((y + 0.5) * source_height / base_height - 0.5, 0.0,
                                         static_cast<double>(source_height - 1));
            const unsigned int x0 = static_cast<unsigned int>(std::floor(sx));
            const unsigned int y0 = static_cast<unsigned int>(std::floor(sy));
            const unsigned int x1 = std::min(x0 + 1, source_width - 1);
            const unsigned int y1 = std::min(y0 + 1, source_height - 1);
            const double fx = sx - x0;
            const double fy = sy - y0;
            const auto mapAt = [&](unsigned int mx, unsigned int my) {
              return static_cast<double>(decoded_source.bytes[my * source_width + mx]) / 255.0;
            };
            const double top = mapAt(x0, y0) * (1.0 - fx) + mapAt(x1, y0) * fx;
            const double bottom = mapAt(x0, y1) * (1.0 - fx) + mapAt(x1, y1) * fx;
            const double m = top * (1.0 - fy) + bottom * fy;
            const double linear_gain = std::pow(m, 2.2);
            const double ideal_q = headroom == 1.0f
                                       ? 0.0
                                       : std::log1p((static_cast<double>(headroom) - 1.0) *
                                                   linear_gain) /
                                             log_headroom;
            const double direct_gain = 1.0 + (static_cast<double>(headroom) - 1.0) * linear_gain;
            const double iso_gain = std::pow(static_cast<double>(headroom), ideal_q);
            max_identity_error_stops = std::max(
                max_identity_error_stops, std::abs(std::log2(iso_gain / direct_gain)));
            const size_t index = static_cast<size_t>(y) * base_width + x;
            const double ideal_q_code = ideal_q * 255.0;
            const long rounded_q_code = std::lround(ideal_q_code);
            ideal_q_values[index] = ideal_q;
            ideal_q8[index] = static_cast<uint8_t>(std::clamp(rounded_q_code, 0L, 255L));
            q_rounding_errors_codes.push_back(std::abs(rounded_q_code - ideal_q_code));
            const double actual_q = static_cast<double>(decoded_output.bytes[index]) / 255.0;
            q_errors.push_back(std::abs(actual_q - ideal_q));
          }
        }
        std::sort(q_errors.begin(), q_errors.end());
        const size_t p99_index = static_cast<size_t>(std::ceil(0.99 * q_errors.size())) - 1;
        const double max_q_error = q_errors.back();
        const double p99_q_error = q_errors[p99_index];
        std::cout << std::setprecision(10) << "APPLE_Q100 pattern=" << name << " map=" << source_width << "x"
                  << source_height << " base=" << base_width << "x" << base_height << " H="
                  << headroom << " max_q=" << max_q_error << " p99_q=" << p99_q_error
                  << " direct_iso_identity_stops=" << max_identity_error_stops << '\n';
        EXPECT_LE(max_identity_error_stops, 1.0e-12)
            << name << " " << base_width << "x" << base_height << " H=" << headroom;
        EXPECT_LE(max_q_error, kMaxQError)
            << name << " " << base_width << "x" << base_height << " H=" << headroom;
        if (headroom == 1.0f) {
          EXPECT_EQ(max_q_error, 0.0) << name << " " << base_width << "x" << base_height;
        }

        if (base_width == 31 || base_width == 32) {
          GrayJpeg control_jpeg = encodeGray(ideal_q8, base_width, base_height);
          ASSERT_FALSE(control_jpeg.bytes.empty()) << name << " control JPEG";
          GrayJpeg decoded_control{};
          ASSERT_TRUE(decodeGray(control_jpeg.bytes.data(), control_jpeg.bytes.size(),
                                 &decoded_control))
              << name << " control JPEG";
          ASSERT_EQ(decoded_control.width, base_width);
          ASSERT_EQ(decoded_control.height, base_height);

          std::vector<double> codec_error_codes;
          std::vector<double> actual_control_error_codes;
          std::vector<double> full_block_q_errors;
          std::vector<double> partial_block_q_errors;
          std::vector<double> full_block_codec_errors;
          std::vector<double> partial_block_codec_errors;
          size_t actual_control_mismatches = 0;
          size_t full_block_mismatches = 0;
          size_t partial_block_mismatches = 0;
          double max_actual_control_error_codes = 0.0;
          codec_error_codes.reserve(pixel_count);
          actual_control_error_codes.reserve(pixel_count);
          for (unsigned int y = 0; y < base_height; ++y) {
            for (unsigned int x = 0; x < base_width; ++x) {
              const size_t index = static_cast<size_t>(y) * base_width + x;
              const double rounded_q_code = ideal_q8[index];
              const double codec_error =
                  std::abs(static_cast<double>(decoded_control.bytes[index]) - rounded_q_code);
              const double actual_control_error =
                  std::abs(static_cast<double>(decoded_output.bytes[index]) -
                           static_cast<double>(decoded_control.bytes[index]));
              const bool full_block = base_width == 32 || (x < 24 && y < 32);
              const double q_error =
                  std::abs(static_cast<double>(decoded_output.bytes[index]) / 255.0 -
                           ideal_q_values[index]);
              codec_error_codes.push_back(codec_error);
              actual_control_error_codes.push_back(actual_control_error);
              max_actual_control_error_codes =
                  std::max(max_actual_control_error_codes, actual_control_error);
              if (actual_control_error != 0.0) {
                ++actual_control_mismatches;
                if (full_block) {
                  ++full_block_mismatches;
                } else {
                  ++partial_block_mismatches;
                }
              }
              if (full_block) {
                full_block_q_errors.push_back(q_error);
                full_block_codec_errors.push_back(codec_error);
              } else {
                partial_block_q_errors.push_back(q_error);
                partial_block_codec_errors.push_back(codec_error);
              }
            }
          }
          std::sort(q_rounding_errors_codes.begin(), q_rounding_errors_codes.end());
          std::sort(codec_error_codes.begin(), codec_error_codes.end());
          std::sort(actual_control_error_codes.begin(), actual_control_error_codes.end());
          const auto p99 = [](const std::vector<double>& sorted_values) {
            const size_t index = static_cast<size_t>(std::ceil(0.99 * sorted_values.size())) - 1;
            return sorted_values[index];
          };
          const auto maxValue = [](const std::vector<double>& values) {
            return *std::max_element(values.begin(), values.end());
          };
          std::sort(full_block_q_errors.begin(), full_block_q_errors.end());
          std::sort(partial_block_q_errors.begin(), partial_block_q_errors.end());
          std::sort(full_block_codec_errors.begin(), full_block_codec_errors.end());
          std::sort(partial_block_codec_errors.begin(), partial_block_codec_errors.end());
          EXPECT_LE(codec_error_codes.back(), 1.0)
              << name << " " << base_width << "x" << base_height << " H=" << headroom
              << " Q100 decoder differs from rounded q by more than one code";
          EXPECT_EQ(actual_control_mismatches, 0u)
              << name << " " << base_width << "x" << base_height << " H=" << headroom
              << " export does not match independent same-Q100 control";
          std::cout << "APPLE_Q100_STAGE pattern=" << name << " base=" << base_width << "x"
                    << base_height << " H=" << headroom
                    << " round8_code_max=" << q_rounding_errors_codes.back()
                    << " round8_code_p99=" << p99(q_rounding_errors_codes)
                    << " codec_code_max=" << codec_error_codes.back()
                    << " codec_code_p99=" << p99(codec_error_codes)
                    << " actual_control_mismatches=" << actual_control_mismatches
                    << " actual_control_max_code=" << max_actual_control_error_codes
                    << " full_block_control_mismatches=" << full_block_mismatches
                    << " partial_block_control_mismatches=" << partial_block_mismatches
                    << " full_block_total_q_max=" << maxValue(full_block_q_errors)
                    << " full_block_total_q_p99=" << p99(full_block_q_errors)
                    << " full_block_codec_max_code=" << maxValue(full_block_codec_errors);
          if (partial_block_q_errors.empty()) {
            std::cout << " partial_block_total_q_max=none partial_block_total_q_p99=none"
                         " partial_block_codec_max_code=none partial_block_codec_p99=none\n";
          } else {
            std::cout << " partial_block_total_q_max=" << maxValue(partial_block_q_errors)
                      << " partial_block_total_q_p99=" << p99(partial_block_q_errors)
                      << " partial_block_codec_max_code="
                      << maxValue(partial_block_codec_errors)
                      << " partial_block_codec_p99=" << p99(partial_block_codec_errors) << '\n';
          }
          EXPECT_LE(q_rounding_errors_codes.back(), 0.5);
        }
      }
    }
  }
}

TEST_F(UltraHdrApiTest, AppleGainmapExportRejectsUnrepresentableIsoHeadroom) {
  std::vector<uint8_t> apple_data;
  ASSERT_TRUE(loadFile("apple_gainmap_new.jpg", apple_data));
  uhdr_compressed_image_t apple_image{};
  apple_image.data = apple_data.data();
  apple_image.data_sz = apple_data.size();
  apple_image.capacity = apple_data.size();
  apple_image.cg = UHDR_CG_UNSPECIFIED;
  apple_image.ct = UHDR_CT_UNSPECIFIED;
  apple_image.range = UHDR_CR_UNSPECIFIED;

  auto createProbedDecoder = [&]() {
    std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(
        uhdr_create_decoder(), uhdr_release_decoder);
    if (!decoder || uhdr_dec_set_image(decoder.get(), &apple_image).error_code != UHDR_CODEC_OK ||
        uhdr_dec_probe(decoder.get()).error_code != UHDR_CODEC_OK) {
      return std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>(nullptr,
                                                                                    uhdr_release_decoder);
    }
    return decoder;
  };

  auto high_decoder = createProbedDecoder();
  ASSERT_NE(high_decoder, nullptr);
  auto* high_handle = dynamic_cast<uhdr_decoder_private*>(high_decoder.get());
  ASSERT_NE(high_handle, nullptr);
  const float representable_headroom = std::ldexp(1.0f, 127);
  std::fill_n(high_handle->m_metadata_ext.max_content_boost, 3, representable_headroom);
  high_handle->m_metadata_ext.hdr_capacity_max = representable_headroom;
  uhdr_gainmap_export_t high_export{};
  ASSERT_EQ(uhdr_dec_export_gainmap(high_decoder.get(), &high_export).error_code, UHDR_CODEC_OK);
  EXPECT_FLOAT_EQ(high_export.metadata.hdr_capacity_max, representable_headroom);
  ultrahdr::uhdr_gainmap_metadata_frac high_fraction;
  ASSERT_EQ(ultrahdr::uhdr_gainmap_metadata_frac::gainmapMetadataFloatToFraction(
                &high_handle->m_metadata_ext, &high_fraction)
                .error_code,
            UHDR_CODEC_OK);
  uhdr_gainmap_metadata_ext_t high_round_trip("1.0");
  EXPECT_EQ(ultrahdr::uhdr_gainmap_metadata_frac::gainmapMetadataFractionToFloat(
                &high_fraction, &high_round_trip)
                .error_code,
            UHDR_CODEC_OK);
  EXPECT_FLOAT_EQ(high_round_trip.hdr_capacity_max, representable_headroom);

  auto max_decoder = createProbedDecoder();
  ASSERT_NE(max_decoder, nullptr);
  auto* max_handle = dynamic_cast<uhdr_decoder_private*>(max_decoder.get());
  ASSERT_NE(max_handle, nullptr);
  const float unrepresentable_headroom = std::numeric_limits<float>::max();
  std::fill_n(max_handle->m_metadata_ext.max_content_boost, 3, unrepresentable_headroom);
  max_handle->m_metadata_ext.hdr_capacity_max = unrepresentable_headroom;
  uint8_t sentinel = 0x5a;
  uhdr_gainmap_export_t max_export{};
  max_export.image.data = &sentinel;
  max_export.image.data_sz = 1;
  max_export.image.capacity = 1;
  max_export.width = 17;
  max_export.height = 19;
  max_export.metadata.max_content_boost[0] = 3.25f;
  EXPECT_EQ(uhdr_dec_export_gainmap(max_decoder.get(), &max_export).error_code,
            UHDR_CODEC_UNSUPPORTED_FEATURE);
  EXPECT_EQ(max_export.image.data, &sentinel);
  EXPECT_EQ(max_export.image.data_sz, 1u);
  EXPECT_EQ(max_export.width, 17u);
  EXPECT_EQ(max_export.height, 19u);
  EXPECT_FLOAT_EQ(max_export.metadata.max_content_boost[0], 3.25f);
}

TEST_F(UltraHdrApiTest, RoutingProbeRejectsInvalidInputAndOrdinaryJpeg) {
  EXPECT_EQ(uhdr_is_supported_gainmap_image(nullptr, 1), 0);
  EXPECT_EQ(uhdr_is_supported_gainmap_image(mSdrJpgData.data(), 0), 0);
  EXPECT_EQ(is_uhdr_image(mSdrJpgData.data(), static_cast<int>(mSdrJpgData.size())), 0);
  EXPECT_EQ(uhdr_is_supported_gainmap_image(mSdrJpgData.data(), mSdrJpgData.size()), 0);

  const uint8_t truncated_jpeg[] = {0xff, 0xd8, 0xff};
  EXPECT_EQ(uhdr_is_supported_gainmap_image(truncated_jpeg, sizeof truncated_jpeg), 0);
}

TEST_F(UltraHdrApiTest, InvalidCompressedImageIntentDoesNotMutateEncoder) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);
  auto* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  ASSERT_NE(handle, nullptr);

  uhdr_error_info_t status = uhdr_enc_set_compressed_image(
      enc, &mSdrCompressed, static_cast<uhdr_img_label_t>(999));

  EXPECT_EQ(UHDR_CODEC_INVALID_PARAM, status.error_code) << status.detail;
  EXPECT_TRUE(handle->m_compressed_images.empty());
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, InvalidTargetBrightnessDoesNotMutateEncoder) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);
  auto* handle = dynamic_cast<uhdr_encoder_private*>(enc);
  ASSERT_NE(handle, nullptr);

  ASSERT_EQ(UHDR_CODEC_OK, uhdr_enc_set_target_display_peak_brightness(enc, 1000.0f).error_code);
  ASSERT_FLOAT_EQ(1000.0f, handle->m_target_disp_max_brightness);

  uhdr_error_info_t status = uhdr_enc_set_target_display_peak_brightness(
      enc, std::numeric_limits<float>::quiet_NaN());

  EXPECT_EQ(UHDR_CODEC_INVALID_PARAM, status.error_code) << status.detail;
  EXPECT_FLOAT_EQ(1000.0f, handle->m_target_disp_max_brightness);
  uhdr_release_encoder(enc);
}

// ============================================================================
// JPEG Tests (API-0 through API-4)
// ============================================================================

TEST_F(UltraHdrApiTest, JpegEncodeApi0AndDecode) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_quality(enc, 90, UHDR_BASE_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_quality(enc, 90, UHDR_GAIN_MAP_IMG).error_code, UHDR_CODEC_OK);

  ASSERT_EQ(uhdr_encode(enc).error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);
  ASSERT_GT(output->data_sz, 0u);
  EXPECT_EQ(is_uhdr_image(output->data, static_cast<int>(output->data_sz)), 1);
  EXPECT_EQ(uhdr_is_supported_gainmap_image(output->data, output->data_sz), 1);

  std::vector<uint8_t> broken_association(static_cast<uint8_t*>(output->data),
                                          static_cast<uint8_t*>(output->data) + output->data_sz);
  MpfEntryLocations locations;
  ASSERT_TRUE(findMpfEntryLocations(broken_association, &locations));
  uint32_t secondary_offset = 0;
  ASSERT_TRUE(readMpfU32ForTest(broken_association, locations.secondary_offset,
                                locations.big_endian, &secondary_offset));
  writeMpfU32ForTest(broken_association, locations.secondary_offset, locations.big_endian,
                     secondary_offset + 1);
  // is_uhdr_image() retains its historical decoder-probe behavior. The stricter MPF association
  // check belongs to the new routing predicate.
  EXPECT_EQ(is_uhdr_image(broken_association.data(), static_cast<int>(broken_association.size())),
            1);
  EXPECT_EQ(uhdr_is_supported_gainmap_image(broken_association.data(), broken_association.size()),
            0);

  std::vector<uint8_t> malformed_table(static_cast<uint8_t*>(output->data),
                                       static_cast<uint8_t*>(output->data) + output->data_sz);
  ASSERT_TRUE(findMpfEntryLocations(malformed_table, &locations));
  writeMpfU32ForTest(malformed_table, locations.count, locations.big_endian, 0xffffffffu);
  EXPECT_EQ(is_uhdr_image(malformed_table.data(), static_cast<int>(malformed_table.size())), 1);
  EXPECT_EQ(uhdr_is_supported_gainmap_image(malformed_table.data(), malformed_table.size()), 0);

  std::vector<uint8_t> displaced_signature(static_cast<uint8_t*>(output->data),
                                           static_cast<uint8_t*>(output->data) + output->data_sz);
  ASSERT_TRUE(findMpfEntryLocations(displaced_signature, &locations));
  ASSERT_GE(locations.signature, 2u);
  uint16_t app2_length = 0;
  ASSERT_TRUE(readMpfU16ForTest(displaced_signature, locations.signature - 2, true, &app2_length));
  displaced_signature[locations.signature + 3] = 'x';
  const size_t displaced_mpf_offset = locations.signature + 4;
  displaced_signature.insert(displaced_signature.begin() + displaced_mpf_offset, 4, uint8_t{0});
  displaced_signature[displaced_mpf_offset] = 'M';
  displaced_signature[displaced_mpf_offset + 1] = 'P';
  displaced_signature[displaced_mpf_offset + 2] = 'F';
  writeMpfU16ForTest(displaced_signature, locations.signature - 2, true, app2_length + 4);
  EXPECT_EQ(is_uhdr_image(displaced_signature.data(), static_cast<int>(displaced_signature.size())),
            1);
  EXPECT_EQ(
      uhdr_is_supported_gainmap_image(displaced_signature.data(), displaced_signature.size()), 0);

  for (const uint16_t duplicate_tag : {uint16_t{0xb001}, uint16_t{0xb002}}) {
    std::vector<uint8_t> duplicate_mandatory_tag(
        static_cast<uint8_t*>(output->data),
        static_cast<uint8_t*>(output->data) + output->data_sz);
    ASSERT_TRUE(insertDuplicateMpfTag(duplicate_mandatory_tag, duplicate_tag));
    EXPECT_EQ(is_uhdr_image(duplicate_mandatory_tag.data(),
                            static_cast<int>(duplicate_mandatory_tag.size())),
              1);
    EXPECT_EQ(uhdr_is_supported_gainmap_image(duplicate_mandatory_tag.data(),
                                              duplicate_mandatory_tag.size()),
              0);
  }

  ASSERT_TRUE(findMpfEntryLocations(malformed_table, &locations));
  struct VersionMutation {
    size_t offset;
    bool is_u16;
  };
  for (const VersionMutation mutation : {
           VersionMutation{locations.version_id, true},
           VersionMutation{locations.version_type, true},
           VersionMutation{locations.version_count, false},
           VersionMutation{locations.version_value, false},
       }) {
    std::vector<uint8_t> malformed_version(static_cast<uint8_t*>(output->data),
                                           static_cast<uint8_t*>(output->data) + output->data_sz);
    if (mutation.is_u16) {
      writeMpfU16ForTest(malformed_version, mutation.offset, locations.big_endian, 0);
    } else {
      writeMpfU32ForTest(malformed_version, mutation.offset, locations.big_endian, 0);
    }
    EXPECT_EQ(is_uhdr_image(malformed_version.data(), static_cast<int>(malformed_version.size())),
              1);
    EXPECT_EQ(uhdr_is_supported_gainmap_image(malformed_version.data(), malformed_version.size()),
              0);
  }

  for (const size_t invalid_field :
       {locations.primary_attributes, locations.primary_offset, locations.secondary_attributes}) {
    std::vector<uint8_t> malformed_entry(static_cast<uint8_t*>(output->data),
                                         static_cast<uint8_t*>(output->data) + output->data_sz);
    ASSERT_TRUE(findMpfEntryLocations(malformed_entry, &locations));
    writeMpfU32ForTest(malformed_entry, invalid_field, locations.big_endian, 1);
    EXPECT_EQ(is_uhdr_image(malformed_entry.data(), static_cast<int>(malformed_entry.size())), 1);
    EXPECT_EQ(uhdr_is_supported_gainmap_image(malformed_entry.data(), malformed_entry.size()), 0);
  }

  // Decode stream
  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_color_transfer(dec, UHDR_CT_HLG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_32bppRGBA1010102).error_code, UHDR_CODEC_OK);

  EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_get_image_width(dec), static_cast<int>(kImageWidth));
  EXPECT_EQ(uhdr_dec_get_image_height(dec), static_cast<int>(kImageHeight));
  uhdr_mem_block_t* base_image = uhdr_dec_get_base_image(dec);
  ASSERT_NE(base_image, nullptr);
  EXPECT_NE(base_image->data, nullptr);
  EXPECT_GT(base_image->data_sz, 0u);
  uhdr_mem_block_t* gainmap_image = uhdr_dec_get_gainmap_image(dec);
  ASSERT_NE(gainmap_image, nullptr);
  EXPECT_NE(gainmap_image->data, nullptr);
  EXPECT_GT(gainmap_image->data_sz, 0u);

  ASSERT_EQ(uhdr_decode(dec).error_code, UHDR_CODEC_OK);
  uhdr_raw_image_t* decoded = uhdr_get_decoded_image(dec);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->w, kImageWidth);
  EXPECT_EQ(decoded->h, kImageHeight);

  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, JpegHdrGpuRequestUsesCpuForEffectsAndKeepsOutputBounds) {
  uhdr_codec_private_t* encoder = uhdr_create_encoder();
  ASSERT_NE(encoder, nullptr);
  ASSERT_EQ(uhdr_enc_set_raw_image(encoder, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_output_format(encoder, UHDR_CODEC_JPG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_quality(encoder, 90, UHDR_BASE_IMG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_quality(encoder, 90, UHDR_GAIN_MAP_IMG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_encode(encoder).error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* encoded = uhdr_get_encoded_stream(encoder);
  ASSERT_NE(encoded, nullptr);

  std::vector<std::vector<uint16_t>> outputs(2);
  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(
      uhdr_create_decoder(), uhdr_release_decoder);
  ASSERT_NE(decoder, nullptr);
  for (int pass = 0; pass < 2; ++pass) {
    if (pass != 0) uhdr_reset_decoder(decoder.get());
    const int gpu_requested = pass == 0 ? 1 : 0;
    ASSERT_EQ(uhdr_dec_set_image(decoder.get(), encoded).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(
        uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code,
        UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code,
              UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_dec_set_out_max_display_boost(decoder.get(), 8.0f).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_enable_gpu_acceleration(decoder.get(), gpu_requested).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_add_effect_rotate(decoder.get(), 90).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_add_effect_crop(decoder.get(), 8, 712, 8, 1272).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_add_effect_resize(decoder.get(), 640, 480).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_decode(decoder.get()).error_code, UHDR_CODEC_OK);

    uhdr_raw_image_t* decoded = uhdr_get_decoded_image(decoder.get());
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->fmt, UHDR_IMG_FMT_64bppRGBAHalfFloat);
    ASSERT_EQ(decoded->w, 640u);
    ASSERT_EQ(decoded->h, 480u);
    const auto* pixels = static_cast<const uint16_t*>(decoded->planes[UHDR_PLANE_PACKED]);
    ASSERT_NE(pixels, nullptr);
    outputs[pass].resize(static_cast<size_t>(decoded->w) * decoded->h * 4);
    for (unsigned int y = 0; y < decoded->h; ++y) {
      const uint16_t* row =
          pixels + static_cast<size_t>(y) * decoded->stride[UHDR_PLANE_PACKED] * 4;
      std::memcpy(outputs[pass].data() + static_cast<size_t>(y) * decoded->w * 4, row,
                  static_cast<size_t>(decoded->w) * 4 * sizeof(uint16_t));
      for (unsigned int x = 0; x < decoded->w; ++x) {
        const size_t index = static_cast<size_t>(y) * decoded->w * 4 + x * 4;
        for (int channel = 0; channel < 3; ++channel) {
          const float value = halfToFloat(outputs[pass][index + channel]);
          EXPECT_TRUE(std::isfinite(value));
          EXPECT_GE(value, 0.0f);
          EXPECT_LE(value, kMaxPixelFloatHdrLinear);
        }
        EXPECT_FLOAT_EQ(halfToFloat(outputs[pass][index + 3]), 1.0f);
      }
    }
  }
  EXPECT_EQ(outputs[0].size(), outputs[1].size());
  EXPECT_EQ(outputs[0], outputs[1]);
  uhdr_release_encoder(encoder);
}

TEST_F(UltraHdrApiTest, JpegEncodeApi1AndDecode) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mSdrRaw, UHDR_SDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG).error_code, UHDR_CODEC_OK);

  ASSERT_EQ(uhdr_encode(enc).error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);
  ASSERT_GT(output->data_sz, 0u);

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_decode(dec).error_code, UHDR_CODEC_OK);

  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, JpegEncodeApi2AndDecode) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_compressed_image(enc, &mSdrCompressed, UHDR_SDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG).error_code, UHDR_CODEC_OK);

  ASSERT_EQ(uhdr_encode(enc).error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_decode(dec).error_code, UHDR_CODEC_OK);

  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

// ============================================================================
// HEIF / HEIC Tests (API-0, API-1, and Unsupported APIs)
// ============================================================================

#if defined(UHDR_ENABLE_HEIF)
static bool setBackwardDirectionFlag(const uhdr_compressed_image_t* image,
                                     std::vector<uint8_t>& modified_image) {
  std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(),
                                                                      heif_context_free);
  if (context == nullptr) return false;
  heif_error error = heif_context_read_from_memory_without_copy(context.get(), image->data,
                                                                image->data_sz, nullptr);
  if (error.code != heif_error_Ok) return false;

  heif_image_handle* raw_base_handle = nullptr;
  error = heif_context_get_primary_image_handle(context.get(), &raw_base_handle);
  std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> base_handle(
      raw_base_handle, heif_image_handle_release);
  if (error.code != heif_error_Ok || base_handle == nullptr) return false;

  const size_t metadata_size = heif_image_handle_get_gain_map_metadata_size(base_handle.get());
  if (metadata_size <= 4) return false;
  std::vector<uint8_t> metadata(metadata_size);
  error = heif_image_handle_get_gain_map_metadata(base_handle.get(), metadata.data());
  if (error.code != heif_error_Ok) return false;

  modified_image.assign(static_cast<const uint8_t*>(image->data),
                        static_cast<const uint8_t*>(image->data) + image->data_sz);
  auto metadata_pos =
      std::search(modified_image.begin(), modified_image.end(), metadata.begin(), metadata.end());
  if (metadata_pos == modified_image.end()) return false;
  if (std::search(metadata_pos + metadata.size(), modified_image.end(), metadata.begin(),
                  metadata.end()) != modified_image.end()) {
    return false;
  }

  // ISO 21496-1 metadata stores its flags after two 16-bit version fields.
  // Set backwardDirection, which this decoder explicitly does not support.
  metadata_pos[4] |= 4;
  return true;
}

TEST_F(UltraHdrApiTest, RoutingProbeReflectsHevcDecoderAvailability) {
  std::vector<uint8_t> hevc_gainmap;
  ASSERT_TRUE(loadFile(kHevcGainmapFileName, hevc_gainmap));

  EXPECT_EQ(is_uhdr_image(hevc_gainmap.data(), static_cast<int>(hevc_gainmap.size())), 1);
  const bool have_hevc_decoder = heif_have_decoder_for_format(heif_compression_HEVC);
  expectSupportedHeifGainmap(hevc_gainmap.data(), hevc_gainmap.size(), have_hevc_decoder);

  if (have_hevc_decoder) {
    uhdr_compressed_image_t input{};
    input.data = hevc_gainmap.data();
    input.data_sz = hevc_gainmap.size();
    input.capacity = hevc_gainmap.size();
    input.cg = UHDR_CG_UNSPECIFIED;
    input.ct = UHDR_CT_UNSPECIFIED;
    input.range = UHDR_CR_UNSPECIFIED;
    uhdr_codec_private_t* decoder = uhdr_create_decoder();
    ASSERT_NE(decoder, nullptr);
    EXPECT_EQ(uhdr_dec_set_image(decoder, &input).error_code, UHDR_CODEC_OK);
    EXPECT_EQ(uhdr_dec_set_out_color_transfer(decoder, UHDR_CT_HLG).error_code, UHDR_CODEC_OK);
    EXPECT_EQ(uhdr_dec_set_out_img_format(decoder, UHDR_IMG_FMT_32bppRGBA1010102).error_code,
              UHDR_CODEC_OK);
    EXPECT_EQ(uhdr_dec_probe(decoder).error_code, UHDR_CODEC_OK);
    EXPECT_EQ(uhdr_decode(decoder).error_code, UHDR_CODEC_OK);
    uhdr_release_decoder(decoder);
  }
}

TEST_F(UltraHdrApiTest, RoutingProbeRejectsOrdinaryAvifButLegacyPredicateRemainsCompatible) {
  if (!heif_have_encoder_for_format(heif_compression_AV1)) {
    GTEST_SKIP() << "AV1 encoder plugin not available in environment";
  }
  std::vector<uint8_t> ordinary_avif;
  ASSERT_TRUE(encodeOrdinaryAvif(ordinary_avif));
  ASSERT_FALSE(ordinary_avif.empty());

  // The historical decoder-probe implementation returns 1 for a parseable HEIF/AVIF container
  // even when it has no gain map. Keep that compatibility behavior explicit here.
  EXPECT_EQ(is_uhdr_image(ordinary_avif.data(), static_cast<int>(ordinary_avif.size())), 1);
  expectSupportedHeifGainmap(ordinary_avif.data(), ordinary_avif.size(), false);
}

TEST(UltraHdrRoutingTest, GainmapSideAlphaIsSupported) {
  if (!heif_have_encoder_for_format(heif_compression_AV1)) {
    GTEST_SKIP() << "AV1 encoder plugin not available in environment";
  }

  std::vector<uint8_t> encoded;
  ASSERT_TRUE(encodeGainmapSideAlphaAvif(encoded));

  HeifContextPtr context(heif_context_alloc(), heif_context_free);
  ASSERT_NE(context, nullptr);
  ASSERT_EQ(heif_context_read_from_memory_without_copy(context.get(), encoded.data(),
                                                       encoded.size(), nullptr)
                .code,
            heif_error_Ok);

  heif_image_handle* primary_raw = nullptr;
  ASSERT_EQ(heif_context_get_primary_image_handle(context.get(), &primary_raw).code,
            heif_error_Ok);
  HeifHandlePtr primary(primary_raw, heif_image_handle_release);
  ASSERT_NE(primary, nullptr);

  heif_image_handle* gainmap_raw = nullptr;
  ASSERT_EQ(heif_image_handle_get_gain_map_image_handle(primary.get(), &gainmap_raw).code,
            heif_error_Ok);
  HeifHandlePtr gainmap(gainmap_raw, heif_image_handle_release);
  ASSERT_NE(gainmap, nullptr);

  EXPECT_EQ(heif_image_handle_has_alpha_channel(primary.get()), 0);
  EXPECT_EQ(heif_image_handle_has_alpha_channel(gainmap.get()), 1);
  EXPECT_EQ(heif_image_handle_is_premultiplied_alpha(gainmap.get()), 0);
  expectSupportedHeifGainmap(encoded.data(), encoded.size(),
                             heif_have_decoder_for_format(heif_compression_AV1));
}

namespace {

constexpr size_t kAlphaTestWidth = 64;
constexpr size_t kAlphaTestHeight = 64;

struct AlphaTestImages {
  std::vector<uint32_t> hdr1010102;
  std::vector<uint16_t> hdrHalfFloat;
  std::vector<uint32_t> sdr8888;
  uhdr_raw_image_t hdr1010102Desc{};
  uhdr_raw_image_t hdrHalfFloatDesc{};
  uhdr_raw_image_t sdr8888Desc{};

  AlphaTestImages()
      : hdr1010102(kAlphaTestWidth * kAlphaTestHeight),
        hdrHalfFloat(kAlphaTestWidth * kAlphaTestHeight * 4),
        sdr8888(kAlphaTestWidth * kAlphaTestHeight) {
    for (size_t y = 0; y < kAlphaTestHeight; ++y) {
      for (size_t x = 0; x < kAlphaTestWidth; ++x) {
        const size_t pixel = y * kAlphaTestWidth + x;
        const uint32_t alpha2 = static_cast<uint32_t>(x / (kAlphaTestWidth / 4));
        const uint32_t alpha8 = alpha2 * 85u;
        hdr1010102[pixel] = 700u | (500u << 10) | (300u << 20) | (alpha2 << 30);
        hdrHalfFloat[pixel * 4] = floatToHalf(2.0f);
        hdrHalfFloat[pixel * 4 + 1] = floatToHalf(1.5f);
        hdrHalfFloat[pixel * 4 + 2] = floatToHalf(1.0f);
        hdrHalfFloat[pixel * 4 + 3] = floatToHalf(alpha2 / 3.0f);
        sdr8888[pixel] = 120u | (80u << 8) | (40u << 16) | (alpha8 << 24);
      }
    }

    hdr1010102Desc.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
    hdr1010102Desc.cg = UHDR_CG_BT_2100;
    hdr1010102Desc.ct = UHDR_CT_HLG;
    hdr1010102Desc.range = UHDR_CR_FULL_RANGE;
    hdr1010102Desc.w = kAlphaTestWidth;
    hdr1010102Desc.h = kAlphaTestHeight;
    hdr1010102Desc.planes[UHDR_PLANE_PACKED] = hdr1010102.data();
    hdr1010102Desc.stride[UHDR_PLANE_PACKED] = kAlphaTestWidth;

    hdrHalfFloatDesc.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
    hdrHalfFloatDesc.cg = UHDR_CG_BT_2100;
    hdrHalfFloatDesc.ct = UHDR_CT_LINEAR;
    hdrHalfFloatDesc.range = UHDR_CR_FULL_RANGE;
    hdrHalfFloatDesc.w = kAlphaTestWidth;
    hdrHalfFloatDesc.h = kAlphaTestHeight;
    hdrHalfFloatDesc.planes[UHDR_PLANE_PACKED] = hdrHalfFloat.data();
    hdrHalfFloatDesc.stride[UHDR_PLANE_PACKED] = kAlphaTestWidth;

    sdr8888Desc.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    sdr8888Desc.cg = UHDR_CG_BT_709;
    sdr8888Desc.ct = UHDR_CT_SRGB;
    sdr8888Desc.range = UHDR_CR_FULL_RANGE;
    sdr8888Desc.w = kAlphaTestWidth;
    sdr8888Desc.h = kAlphaTestHeight;
    sdr8888Desc.planes[UHDR_PLANE_PACKED] = sdr8888.data();
    sdr8888Desc.stride[UHDR_PLANE_PACKED] = kAlphaTestWidth;
  }

  void makeHdr1010102AlphaOpaque() {
    for (uint32_t& pixel : hdr1010102) pixel |= 3u << 30;
  }
};

uhdr_error_info_t encodeRawAlphaImage(uhdr_codec_t codec, uhdr_raw_image_t* hdr,
                                      uhdr_raw_image_t* sdr, std::vector<uint8_t>* encoded) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  if (enc == nullptr) {
    uhdr_error_info_t status{};
    status.error_code = UHDR_CODEC_MEM_ERROR;
    return status;
  }

  uhdr_error_info_t status = uhdr_enc_set_raw_image(enc, hdr, UHDR_HDR_IMG);
  if (status.error_code == UHDR_CODEC_OK && sdr != nullptr) {
    status = uhdr_enc_set_raw_image(enc, sdr, UHDR_SDR_IMG);
  }
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_enc_set_output_format(enc, codec);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_enc_set_quality(enc, 100, UHDR_BASE_IMG);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_encode(enc);
  if (status.error_code == UHDR_CODEC_OK) {
    uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
    if (output == nullptr || output->data_sz == 0) {
      status.error_code = UHDR_CODEC_ERROR;
    } else {
      const uint8_t* data = static_cast<const uint8_t*>(output->data);
      encoded->assign(data, data + output->data_sz);
    }
  }
  uhdr_release_encoder(enc);
  return status;
}

::testing::AssertionResult containerHasStraightAlpha(const std::vector<uint8_t>& encoded) {
  heif_context* context = heif_context_alloc();
  if (context == nullptr) return ::testing::AssertionFailure() << "libheif allocation failed";

  heif_error error =
      heif_context_read_from_memory_without_copy(context, encoded.data(), encoded.size(), nullptr);
  if (error.code != heif_error_Ok) {
    const std::string detail = error.message != nullptr ? error.message : "no detail";
    heif_context_free(context);
    return ::testing::AssertionFailure() << "libheif read failed: " << detail;
  }

  heif_image_handle* primary = nullptr;
  error = heif_context_get_primary_image_handle(context, &primary);
  if (error.code != heif_error_Ok || primary == nullptr) {
    const std::string detail = error.message != nullptr ? error.message : "no detail";
    heif_context_free(context);
    return ::testing::AssertionFailure() << "primary image lookup failed: " << detail;
  }

  const bool has_alpha = heif_image_handle_has_alpha_channel(primary);
  const bool is_premultiplied = heif_image_handle_is_premultiplied_alpha(primary);
  heif_image_handle_release(primary);
  heif_context_free(context);
  if (!has_alpha) return ::testing::AssertionFailure() << "base image has no alpha channel";
  if (is_premultiplied) {
    return ::testing::AssertionFailure() << "base image alpha is marked premultiplied";
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult decodedAlphaMatches(const std::vector<uint8_t>& encoded) {
  const ::testing::AssertionResult container_result = containerHasStraightAlpha(encoded);
  if (!container_result) return container_result;

  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(encoded.data());
  input.data_sz = encoded.size();
  input.capacity = encoded.size();

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  if (dec == nullptr) return ::testing::AssertionFailure() << "decoder allocation failed";
  uhdr_error_info_t status = uhdr_dec_set_image(dec, &input);
  if (status.error_code == UHDR_CODEC_OK) {
    status = uhdr_dec_set_out_color_transfer(dec, UHDR_CT_SRGB);
  }
  if (status.error_code == UHDR_CODEC_OK) {
    status = uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_32bppRGBA8888);
  }
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_dec_probe(dec);
  if (status.error_code == UHDR_CODEC_OK) status = uhdr_decode(dec);
  if (status.error_code != UHDR_CODEC_OK) {
    const std::string detail = status.has_detail ? status.detail : "no detail";
    uhdr_release_decoder(dec);
    return ::testing::AssertionFailure() << "decode failed: " << detail;
  }

  uhdr_raw_image_t* decoded = uhdr_get_decoded_image(dec);
  if (decoded == nullptr || decoded->fmt != UHDR_IMG_FMT_32bppRGBA8888 ||
      decoded->w != kAlphaTestWidth || decoded->h != kAlphaTestHeight) {
    uhdr_release_decoder(dec);
    return ::testing::AssertionFailure() << "unexpected decoded image descriptor";
  }

  const auto* pixels = static_cast<const uint32_t*>(decoded->planes[UHDR_PLANE_PACKED]);
  const size_t stride = decoded->stride[UHDR_PLANE_PACKED];
  for (size_t y = 0; y < kAlphaTestHeight; ++y) {
    for (size_t x = 0; x < kAlphaTestWidth; ++x) {
      const int alpha = pixels[y * stride + x] >> 24;
      const int expected = static_cast<int>(x / (kAlphaTestWidth / 4)) * 85;
      if (std::abs(alpha - expected) > 16) {
        uhdr_release_decoder(dec);
        return ::testing::AssertionFailure() << "unexpected alpha " << alpha << ", expected "
                                             << expected << " at (" << x << ", " << y << ")";
      }
    }
  }
  uhdr_release_decoder(dec);
  return ::testing::AssertionSuccess();
}

void expectRawAlphaPreserved(uhdr_codec_t codec, uhdr_raw_image_t* hdr,
                             uhdr_raw_image_t* sdr = nullptr) {
  std::vector<uint8_t> encoded;
  const uhdr_error_info_t status = encodeRawAlphaImage(codec, hdr, sdr, &encoded);
  ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << (status.has_detail ? status.detail : "no detail");
  EXPECT_TRUE(decodedAlphaMatches(encoded));
}

bool encoderUnavailable(const uhdr_error_info_t& status) {
  return status.error_code != UHDR_CODEC_OK && status.has_detail &&
         (strstr(status.detail, "Unsupported file-type") != nullptr ||
          strstr(status.detail, "No encoder") != nullptr);
}

}  // namespace

TEST_F(UltraHdrApiTest, HeicEncodeApi0AndDecode) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_HEIF).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_quality(enc, 85, UHDR_BASE_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_quality(enc, 85, UHDR_GAIN_MAP_IMG).error_code, UHDR_CODEC_OK);

  uhdr_error_info_t enc_status = uhdr_encode(enc);
  if (enc_status.error_code != UHDR_CODEC_OK) {
    if (enc_status.has_detail &&
        (strstr(enc_status.detail, "Unsupported file-type") != nullptr ||
         strstr(enc_status.detail, "No encoder") != nullptr)) {
      std::string detail_msg = enc_status.detail;
      uhdr_release_encoder(enc);
      GTEST_SKIP() << "HEVC encoder plugin not available in environment: " << detail_msg;
      return;
    }
  }
  ASSERT_EQ(enc_status.error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);
  ASSERT_GT(output->data_sz, 0u);

  EXPECT_EQ(is_uhdr_image(output->data, static_cast<int>(output->data_sz)), 1);
  expectSupportedHeifGainmap(output->data, output->data_sz,
                             heif_have_decoder_for_format(heif_compression_HEVC));

  // Decode HEIC stream
  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_color_transfer(dec, UHDR_CT_HLG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_32bppRGBA1010102).error_code, UHDR_CODEC_OK);

  EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_get_image_width(dec), static_cast<int>(kImageWidth));
  EXPECT_EQ(uhdr_dec_get_image_height(dec), static_cast<int>(kImageHeight));
  EXPECT_EQ(uhdr_dec_get_base_image(dec), nullptr);
  EXPECT_EQ(uhdr_dec_get_gainmap_image(dec), nullptr);

  uhdr_gainmap_metadata_t* metadata = uhdr_dec_get_gainmap_metadata(dec);
  EXPECT_NE(metadata, nullptr);

  uhdr_error_info_t dec_status = uhdr_decode(dec);
  if (dec_status.error_code != UHDR_CODEC_OK) {
    std::cout << "HeicEncodeApi0 decode error: " << (dec_status.has_detail ? dec_status.detail : "no detail") << std::endl;
  }
  ASSERT_EQ(dec_status.error_code, UHDR_CODEC_OK);
  uhdr_raw_image_t* decoded = uhdr_get_decoded_image(dec);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->w, kImageWidth);
  EXPECT_EQ(decoded->h, kImageHeight);
  uhdr_raw_image_t* hdr_gainmap = uhdr_get_decoded_gainmap_image(dec);
  ASSERT_NE(hdr_gainmap, nullptr);

  uhdr_codec_private_t* sdr_dec = uhdr_create_decoder();
  ASSERT_NE(sdr_dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(sdr_dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_color_transfer(sdr_dec, UHDR_CT_SRGB).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_img_format(sdr_dec, UHDR_IMG_FMT_32bppRGBA8888).error_code,
            UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_probe(sdr_dec).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_decode(sdr_dec).error_code, UHDR_CODEC_OK);
  expectGainMapsEqual(hdr_gainmap, uhdr_get_decoded_gainmap_image(sdr_dec));

  uhdr_release_decoder(sdr_dec);
  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, HeicEncodeApi1AndDecode) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mSdrRaw, UHDR_SDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_HEIF).error_code, UHDR_CODEC_OK);

  uhdr_error_info_t enc_status = uhdr_encode(enc);
  if (enc_status.error_code != UHDR_CODEC_OK) {
    if (enc_status.has_detail &&
        (strstr(enc_status.detail, "Unsupported file-type") != nullptr ||
         strstr(enc_status.detail, "No encoder") != nullptr)) {
      std::string detail_msg = enc_status.detail;
      uhdr_release_encoder(enc);
      GTEST_SKIP() << "HEVC encoder plugin not available in environment: " << detail_msg;
      return;
    }
  }
  ASSERT_EQ(enc_status.error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);
  ASSERT_GT(output->data_sz, 0u);
  EXPECT_EQ(getPrimaryImageTransfer(output), heif_transfer_characteristic_IEC_61966_2_1);
  EXPECT_EQ(is_uhdr_image(output->data, static_cast<int>(output->data_sz)), 1);
  expectSupportedHeifGainmap(output->data, output->data_sz,
                             heif_have_decoder_for_format(heif_compression_HEVC));

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_decode(dec).error_code, UHDR_CODEC_OK);

  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, HeicEncodeRejectsUndersizedDestination) {
  std::vector<uint8_t> backing_store(6 * kImageWidth * kImageHeight, 0xa5);
  uhdr_compressed_image_t dest{};
  dest.data = backing_store.data();
  dest.capacity = 1;

  HeifUltraHdr codec;
  uhdr_error_info_t status = codec.encodeHeicUltraHdr(&mHdrRaw, &dest, 85, nullptr);
  if (status.error_code != UHDR_CODEC_OK && status.has_detail &&
      (strstr(status.detail, "Unsupported file-type") != nullptr ||
       strstr(status.detail, "No encoder") != nullptr)) {
    GTEST_SKIP() << "HEVC encoder plugin not available in environment: " << status.detail;
  }

  EXPECT_EQ(status.error_code, UHDR_CODEC_MEM_ERROR);
  EXPECT_EQ(dest.capacity, 1u);
  EXPECT_EQ(dest.data_sz, 0u);
  EXPECT_EQ(backing_store.front(), 0xa5);
}

TEST_F(UltraHdrApiTest, HeicCompressedIntentsUnsupported) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_compressed_image(enc, &mSdrCompressed, UHDR_SDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_HEIF).error_code, UHDR_CODEC_OK);

  uhdr_error_info_t err = uhdr_encode(enc);
  EXPECT_EQ(err.error_code, UHDR_CODEC_UNSUPPORTED_FEATURE);

  uhdr_release_encoder(enc);
}

// ============================================================================
// AVIF Tests (API-0, API-1, and Unsupported APIs)
// ============================================================================

TEST_F(UltraHdrApiTest, AvifEncodeApi0AndDecode) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_AVIF).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_quality(enc, 85, UHDR_BASE_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_quality(enc, 85, UHDR_GAIN_MAP_IMG).error_code, UHDR_CODEC_OK);

  uhdr_error_info_t enc_status = uhdr_encode(enc);
  if (enc_status.error_code != UHDR_CODEC_OK) {
    if (enc_status.has_detail &&
        (strstr(enc_status.detail, "Unsupported file-type") != nullptr ||
         strstr(enc_status.detail, "No encoder") != nullptr)) {
      std::string detail_msg = enc_status.detail;
      uhdr_release_encoder(enc);
      GTEST_SKIP() << "AV1 encoder plugin not available in environment: " << detail_msg;
      return;
    }
  }
  ASSERT_EQ(enc_status.error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);
  ASSERT_GT(output->data_sz, 0u);

  EXPECT_EQ(is_uhdr_image(output->data, static_cast<int>(output->data_sz)), 1);
  expectSupportedHeifGainmap(output->data, output->data_sz,
                             heif_have_decoder_for_format(heif_compression_AV1));

  // Decode AVIF stream
  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_color_transfer(dec, UHDR_CT_LINEAR).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code, UHDR_CODEC_OK);

  EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_get_image_width(dec), static_cast<int>(kImageWidth));
  EXPECT_EQ(uhdr_dec_get_image_height(dec), static_cast<int>(kImageHeight));
  EXPECT_EQ(uhdr_dec_get_base_image(dec), nullptr);
  EXPECT_EQ(uhdr_dec_get_gainmap_image(dec), nullptr);

  uhdr_error_info_t dec_status = uhdr_decode(dec);
  if (dec_status.error_code != UHDR_CODEC_OK) {
    std::cout << "AvifEncodeApi0 decode error: " << (dec_status.has_detail ? dec_status.detail : "no detail") << std::endl;
  }
  ASSERT_EQ(dec_status.error_code, UHDR_CODEC_OK);
  uhdr_raw_image_t* decoded = uhdr_get_decoded_image(dec);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->w, kImageWidth);
  EXPECT_EQ(decoded->h, kImageHeight);
  uhdr_raw_image_t* hdr_gainmap = uhdr_get_decoded_gainmap_image(dec);
  ASSERT_NE(hdr_gainmap, nullptr);

  uhdr_codec_private_t* sdr_dec = uhdr_create_decoder();
  ASSERT_NE(sdr_dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(sdr_dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_color_transfer(sdr_dec, UHDR_CT_SRGB).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_set_out_img_format(sdr_dec, UHDR_IMG_FMT_32bppRGBA8888).error_code,
            UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_probe(sdr_dec).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_decode(sdr_dec).error_code, UHDR_CODEC_OK);
  expectGainMapsEqual(hdr_gainmap, uhdr_get_decoded_gainmap_image(sdr_dec));

  uhdr_release_decoder(sdr_dec);
  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, AvifEncodeApi1AndDecode) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mSdrRaw, UHDR_SDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_AVIF).error_code, UHDR_CODEC_OK);

  uhdr_error_info_t enc_status = uhdr_encode(enc);
  if (enc_status.error_code != UHDR_CODEC_OK) {
    if (enc_status.has_detail &&
        (strstr(enc_status.detail, "Unsupported file-type") != nullptr ||
         strstr(enc_status.detail, "No encoder") != nullptr)) {
      uhdr_release_encoder(enc);
      GTEST_SKIP() << "AV1 encoder plugin not available in environment: " << enc_status.detail;
      return;
    }
  }
  ASSERT_EQ(enc_status.error_code, UHDR_CODEC_OK);
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);
  ASSERT_GT(output->data_sz, 0u);
  EXPECT_EQ(getPrimaryImageTransfer(output), heif_transfer_characteristic_IEC_61966_2_1);
  EXPECT_EQ(is_uhdr_image(output->data, static_cast<int>(output->data_sz)), 1);
  expectSupportedHeifGainmap(output->data, output->data_sz,
                             heif_have_decoder_for_format(heif_compression_AV1));

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_decode(dec).error_code, UHDR_CODEC_OK);

  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, AvifEncodeRejectsUndersizedDestination) {
  std::vector<uint8_t> backing_store(6 * kImageWidth * kImageHeight, 0xa5);
  uhdr_compressed_image_t dest{};
  dest.data = backing_store.data();
  dest.capacity = 1;

  AvifUltraHdr codec;
  uhdr_error_info_t status = codec.encodeAvifUltraHdr(&mHdrRaw, &dest, 85, nullptr);
  if (status.error_code != UHDR_CODEC_OK && status.has_detail &&
      (strstr(status.detail, "Unsupported file-type") != nullptr ||
       strstr(status.detail, "No encoder") != nullptr)) {
    GTEST_SKIP() << "AV1 encoder plugin not available in environment: " << status.detail;
  }

  EXPECT_EQ(status.error_code, UHDR_CODEC_MEM_ERROR);
  EXPECT_EQ(dest.capacity, 1u);
  EXPECT_EQ(dest.data_sz, 0u);
  EXPECT_EQ(backing_store.front(), 0xa5);
}

TEST_F(UltraHdrApiTest, HeifAndAvifPropagateGainMapMetadataErrors) {
  int tested_formats = 0;
  for (uhdr_codec_t codec : {UHDR_CODEC_AVIF, UHDR_CODEC_HEIF}) {
    SCOPED_TRACE(codec == UHDR_CODEC_AVIF ? "AVIF" : "HEIF");
    uhdr_codec_private_t* enc = uhdr_create_encoder();
    ASSERT_NE(enc, nullptr);

    ASSERT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
    ASSERT_EQ(uhdr_enc_set_output_format(enc, codec).error_code, UHDR_CODEC_OK);
    uhdr_error_info_t enc_status = uhdr_encode(enc);
    if (enc_status.error_code != UHDR_CODEC_OK && enc_status.has_detail &&
        (strstr(enc_status.detail, "Unsupported file-type") != nullptr ||
         strstr(enc_status.detail, "No encoder") != nullptr)) {
      uhdr_release_encoder(enc);
      continue;
    }
    ASSERT_EQ(enc_status.error_code, UHDR_CODEC_OK);
    ++tested_formats;

    uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
    ASSERT_NE(output, nullptr);
    std::vector<uint8_t> invalid_data;
    ASSERT_TRUE(setBackwardDirectionFlag(output, invalid_data));

    uhdr_compressed_image_t invalid_image = *output;
    invalid_image.data = invalid_data.data();
    invalid_image.data_sz = invalid_image.capacity = invalid_data.size();

    uhdr_codec_private_t* dec = uhdr_create_decoder();
    ASSERT_NE(dec, nullptr);
    ASSERT_EQ(uhdr_dec_set_image(dec, &invalid_image).error_code, UHDR_CODEC_OK);
    EXPECT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_UNSUPPORTED_FEATURE);
    uhdr_release_decoder(dec);

    // Exercise each backend error path directly as well. Sanitizer builds verify that these paths
    // release the partially decoded libheif objects before returning the metadata error.
    std::vector<uint8_t> decoded_data(kImageWidth * kImageHeight * 4);
    uhdr_raw_image_t decoded_image{};
    decoded_image.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    decoded_image.cg = UHDR_CG_BT_709;
    decoded_image.ct = UHDR_CT_SRGB;
    decoded_image.range = UHDR_CR_FULL_RANGE;
    decoded_image.w = kImageWidth;
    decoded_image.h = kImageHeight;
    decoded_image.planes[UHDR_PLANE_PACKED] = decoded_data.data();
    decoded_image.stride[UHDR_PLANE_PACKED] = kImageWidth;
    uhdr_gainmap_metadata_t metadata{};
    uhdr_error_info_t decode_status;
    if (codec == UHDR_CODEC_AVIF) {
      AvifUltraHdr avif;
      decode_status = avif.decodeAvifUltraHdr(&invalid_image, &decoded_image, FLT_MAX, UHDR_CT_SRGB,
                                              UHDR_IMG_FMT_32bppRGBA8888, nullptr, &metadata);
    } else {
      HeifUltraHdr heif;
      decode_status = heif.decodeHeicUltraHdr(&invalid_image, &decoded_image, FLT_MAX, UHDR_CT_SRGB,
                                              UHDR_IMG_FMT_32bppRGBA8888, nullptr, &metadata);
    }
    EXPECT_EQ(decode_status.error_code, UHDR_CODEC_UNSUPPORTED_FEATURE);

    uhdr_release_encoder(enc);
  }
  if (tested_formats == 0) GTEST_SKIP() << "AV1 and HEVC encoder plugins are unavailable";
}

// Returns the offset of the payload of the first box of the given type, or 0 if there is none.
static size_t findBoxPayload(const std::vector<uint8_t>& data, const char* type) {
  for (size_t i = 4; i + 4 <= data.size(); i++) {
    if (memcmp(&data[i], type, 4) == 0) return i + 4;
  }
  return 0;
}

static void writeBigEndian32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value >> 24);
  data[offset + 1] = static_cast<uint8_t>(value >> 16);
  data[offset + 2] = static_cast<uint8_t>(value >> 8);
  data[offset + 3] = static_cast<uint8_t>(value);
}

static void expectHeifDecodeFails(std::vector<uint8_t>& data) {
  uhdr_compressed_image_t image{};
  image.data = data.data();
  image.data_sz = data.size();
  image.capacity = data.size();
  image.cg = UHDR_CG_UNSPECIFIED;
  image.ct = UHDR_CT_UNSPECIFIED;
  image.range = UHDR_CR_UNSPECIFIED;
  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(dec, &image).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_set_out_color_transfer(dec, UHDR_CT_HLG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_32bppRGBA1010102).error_code,
            UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_NE(uhdr_decode(dec).error_code, UHDR_CODEC_OK);
  uhdr_release_decoder(dec);
}

// Expects the HEIF probe to reject the file, and decoding (which probes first) to fail too.
static void expectHeifProbeFails(std::vector<uint8_t>& data, const char* detail) {
  uhdr_compressed_image_t image{};
  image.data = data.data();
  image.data_sz = data.size();
  image.capacity = data.size();
  image.cg = UHDR_CG_UNSPECIFIED;
  image.ct = UHDR_CT_UNSPECIFIED;
  image.range = UHDR_CR_UNSPECIFIED;
  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(dec, &image).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_set_out_color_transfer(dec, UHDR_CT_HLG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_32bppRGBA1010102).error_code,
            UHDR_CODEC_OK);
  uhdr_error_info_t status = uhdr_dec_probe(dec);
  EXPECT_NE(status.error_code, UHDR_CODEC_OK);
  EXPECT_NE(strstr(status.detail, detail), nullptr) << status.detail;
  EXPECT_NE(uhdr_decode(dec).error_code, UHDR_CODEC_OK);
  uhdr_release_decoder(dec);
}

// The image size reported by libheif comes from the 'ispe' and 'clap' properties, which need not
// match the coded image. Decoding must fail instead of copying the declared size out of the
// smaller decoded image.
TEST_F(UltraHdrApiTest, HeifRejectsImageSizeLargerThanDecodedImage) {
  if (!heif_have_decoder_for_format(heif_compression_HEVC)) {
    GTEST_SKIP() << "HEVC decoder plugin not available in environment";
  }
  std::vector<uint8_t> valid_data;
  ASSERT_TRUE(loadFile(kHevcGainmapFileName, valid_data));
  // The fixture codes 64x64 images ('ispe') cropped to 16x16 by a 'clap' property.
  const size_t ispe = findBoxPayload(valid_data, "ispe");
  const size_t clap = findBoxPayload(valid_data, "clap");
  ASSERT_NE(ispe, 0u);
  ASSERT_NE(clap, 0u);
  ASSERT_LE(ispe + 12, valid_data.size());
  ASSERT_LE(clap + 16, valid_data.size());

  {
    SCOPED_TRACE("ispe larger than the coded image");
    std::vector<uint8_t> data = valid_data;
    memcpy(&data[clap - 4], "free", 4);
    writeBigEndian32(data, ispe + 4, 128);
    writeBigEndian32(data, ispe + 8, 128);
    expectHeifDecodeFails(data);
  }
  {
    SCOPED_TRACE("clap larger than the coded image");
    std::vector<uint8_t> data = valid_data;
    writeBigEndian32(data, clap, 128);
    writeBigEndian32(data, clap + 8, 128);
    expectHeifDecodeFails(data);
  }
}

// Removes the association of the given 1-based property index from an item in the 'ipma' box.
static bool dropItemProperty(std::vector<uint8_t>& data, uint16_t item_id, uint8_t property) {
  const size_t ipma = findBoxPayload(data, "ipma");
  if (ipma == 0 || ipma + 8 > data.size() || data[ipma] != 0 || (data[ipma + 3] & 1) != 0) {
    return false;
  }
  const uint32_t entries = (data[ipma + 4] << 24) | (data[ipma + 5] << 16) |
                           (data[ipma + 6] << 8) | data[ipma + 7];
  size_t pos = ipma + 8;
  for (uint32_t i = 0; i < entries && pos + 3 <= data.size(); i++) {
    const uint16_t id = (data[pos] << 8) | data[pos + 1];
    const size_t count = data[pos + 2];
    pos += 3;
    if (pos + count > data.size()) return false;
    for (size_t j = 0; id == item_id && j < count; j++) {
      if ((data[pos + j] & 0x7f) == property) {
        data[pos + j] = 0;
        return true;
      }
    }
    pos += count;
  }
  return false;
}

// The image size is read from the container. An image item without an 'ispe' property has no size
// and one with a huge 'ispe' cannot be decoded; both must be rejected instead of being allocated,
// copied or sampled with that size.
TEST_F(UltraHdrApiTest, HeifRejectsUnsupportedImageSizes) {
  if (!heif_have_decoder_for_format(heif_compression_HEVC)) {
    GTEST_SKIP() << "HEVC decoder plugin not available in environment";
  }
  std::vector<uint8_t> valid_data;
  ASSERT_TRUE(loadFile(kHevcGainmapFileName, valid_data));
  const size_t ispe = findBoxPayload(valid_data, "ispe");
  ASSERT_NE(ispe, 0u);
  ASSERT_LE(ispe + 12, valid_data.size());

  {
    SCOPED_TRACE("gain map without 'ispe'");
    std::vector<uint8_t> data = valid_data;
    // In the fixture, item 3 is the gain map; property 4 is its 'ispe' and property 5 its 'clap'.
    ASSERT_TRUE(dropItemProperty(data, 3, 4));
    ASSERT_TRUE(dropItemProperty(data, 3, 5));
    expectHeifProbeFails(data, "unsupported gain map image dimensions 0x0");
  }
  // One dimension just above the configured maximum, the other kept at the fixture's 64, so that
  // libheif's own image size limits are not hit first.
  const uint64_t heif_max_pixels = heif_get_global_security_limits()->max_image_size_pixels;
  const struct {
    const char* name;
    uint64_t width, height;
  } oversized[] = {
      {"'ispe' wider than the maximum supported width", uint64_t(ultrahdr::kMaxWidth) + 1, 64},
      {"'ispe' taller than the maximum supported height", 64, uint64_t(ultrahdr::kMaxHeight) + 1},
  };
  for (const auto& size : oversized) {
    SCOPED_TRACE(size.name);
    if (size.width > uint64_t(std::numeric_limits<int>::max()) ||
        size.height > uint64_t(std::numeric_limits<int>::max()) ||
        (heif_max_pixels != 0 && size.width * size.height > heif_max_pixels)) {
      GTEST_SKIP() << "maximum supported dimensions are beyond libheif's image size limits";
    }
    std::vector<uint8_t> data = valid_data;
    ASSERT_TRUE(dropItemProperty(data, 1, 5));
    ASSERT_TRUE(dropItemProperty(data, 3, 5));
    writeBigEndian32(data, ispe + 4, static_cast<uint32_t>(size.width));
    writeBigEndian32(data, ispe + 8, static_cast<uint32_t>(size.height));
    const std::string detail = "unsupported primary image dimensions " +
                               std::to_string(size.width) + "x" + std::to_string(size.height);
    expectHeifProbeFails(data, detail.c_str());
  }
}

class OddMultichannelGainMapCodecTest : public ::testing::TestWithParam<uhdr_codec_t> {};

TEST_P(OddMultichannelGainMapCodecTest, PreservesOddGainMapEdges) {
  constexpr unsigned width = 36;
  constexpr unsigned height = 36;
  constexpr unsigned gainmap_scale_factor = 4;
  constexpr unsigned gainmap_width = width / gainmap_scale_factor;
  constexpr unsigned gainmap_height = height / gainmap_scale_factor;
  constexpr unsigned edge_sample_x = (gainmap_width - 1) * gainmap_scale_factor;
  constexpr unsigned edge_sample_y = (gainmap_height - 1) * gainmap_scale_factor;
  std::vector<uint32_t> hdr_pixels(static_cast<size_t>(width) * height,
                                   700u | (700u << 10) | (700u << 20) | (3u << 30));
  hdr_pixels[static_cast<size_t>(edge_sample_y) * width + edge_sample_x] =
      1000u | (300u << 10) | (100u << 20) | (3u << 30);
  uhdr_raw_image_t hdr{};
  hdr.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
  hdr.cg = UHDR_CG_BT_2100;
  hdr.ct = UHDR_CT_HLG;
  hdr.range = UHDR_CR_FULL_RANGE;
  hdr.w = width;
  hdr.h = height;
  hdr.planes[UHDR_PLANE_PACKED] = hdr_pixels.data();
  hdr.stride[UHDR_PLANE_PACKED] = width;

  std::vector<uint8_t> sdr_pixels(static_cast<size_t>(width) * height * 4, 128);
  for (size_t i = 3; i < sdr_pixels.size(); i += 4) sdr_pixels[i] = 255;
  uhdr_raw_image_t sdr{};
  sdr.fmt = UHDR_IMG_FMT_32bppRGBA8888;
  sdr.cg = UHDR_CG_BT_2100;
  sdr.ct = UHDR_CT_SRGB;
  sdr.range = UHDR_CR_FULL_RANGE;
  sdr.w = width;
  sdr.h = height;
  sdr.planes[UHDR_PLANE_PACKED] = sdr_pixels.data();
  sdr.stride[UHDR_PLANE_PACKED] = width;

  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);
  ASSERT_EQ(uhdr_enc_set_raw_image(enc, &hdr, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_raw_image(enc, &sdr, UHDR_SDR_IMG).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_output_format(enc, GetParam()).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_using_multi_channel_gainmap(enc, 1).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_gainmap_scale_factor(enc, gainmap_scale_factor).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_enc_set_quality(enc, 100, UHDR_GAIN_MAP_IMG).error_code, UHDR_CODEC_OK);

  uhdr_error_info_t enc_status = uhdr_encode(enc);
  if (enc_status.error_code != UHDR_CODEC_OK && enc_status.has_detail &&
      (strstr(enc_status.detail, "Unsupported file-type") != nullptr ||
       strstr(enc_status.detail, "No encoder") != nullptr)) {
    const std::string detail = enc_status.detail;
    uhdr_release_encoder(enc);
    GTEST_SKIP() << "encoder plugin not available in environment: " << detail;
  }
  ASSERT_EQ(enc_status.error_code, UHDR_CODEC_OK)
      << (enc_status.has_detail ? enc_status.detail : "");
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(enc);
  ASSERT_NE(output, nullptr);

  uhdr_codec_private_t* dec = uhdr_create_decoder();
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(uhdr_dec_set_image(dec, output).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_probe(dec).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_dec_get_gainmap_width(dec), static_cast<int>(gainmap_width));
  EXPECT_EQ(uhdr_dec_get_gainmap_height(dec), static_cast<int>(gainmap_height));
  ASSERT_EQ(uhdr_decode(dec).error_code, UHDR_CODEC_OK);

  heif_context* heif_ctx = heif_context_alloc();
  ASSERT_NE(heif_ctx, nullptr);
  ASSERT_EQ(
      heif_context_read_from_memory_without_copy(heif_ctx, output->data, output->data_sz, nullptr)
          .code,
      heif_error_Ok);
  heif_image_handle* base_handle = nullptr;
  ASSERT_EQ(heif_context_get_primary_image_handle(heif_ctx, &base_handle).code, heif_error_Ok);
  heif_image_handle* gainmap_handle = nullptr;
  ASSERT_EQ(heif_image_handle_get_gain_map_image_handle(base_handle, &gainmap_handle).code,
            heif_error_Ok);
  EXPECT_EQ(heif_image_handle_get_width(gainmap_handle), static_cast<int>(gainmap_width));
  EXPECT_EQ(heif_image_handle_get_height(gainmap_handle), static_cast<int>(gainmap_height));
  heif_image* decoded_gainmap = nullptr;
  ASSERT_EQ(heif_decode_image(gainmap_handle, &decoded_gainmap, heif_colorspace_RGB,
                              heif_chroma_interleaved_RGBA, nullptr)
                .code,
            heif_error_Ok);
  int gainmap_stride = 0;
  const uint8_t* gainmap_pixels =
      heif_image_get_plane_readonly(decoded_gainmap, heif_channel_interleaved, &gainmap_stride);
  ASSERT_NE(gainmap_pixels, nullptr);
  const size_t edge_offset =
      static_cast<size_t>(gainmap_height - 1) * gainmap_stride + (gainmap_width - 1) * 4;
  // The final source sample has much more red gain than green or blue. This verifies that the
  // partial lower-right 4:2:0 block carries its own chroma instead of retaining default values or
  // borrowing the previous complete pair.
  EXPECT_GT(gainmap_pixels[edge_offset], gainmap_pixels[edge_offset + 1] + 20);
  EXPECT_GT(gainmap_pixels[edge_offset], gainmap_pixels[edge_offset + 2] + 20);

  heif_image_release(decoded_gainmap);
  heif_image_handle_release(gainmap_handle);
  heif_image_handle_release(base_handle);
  heif_context_free(heif_ctx);

  uhdr_release_decoder(dec);
  uhdr_release_encoder(enc);
}

INSTANTIATE_TEST_SUITE_P(AvifAndHeif, OddMultichannelGainMapCodecTest,
                         ::testing::Values(UHDR_CODEC_AVIF, UHDR_CODEC_HEIF));

TEST_F(UltraHdrApiTest, AvifCompressedIntentsUnsupported) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);

  EXPECT_EQ(uhdr_enc_set_raw_image(enc, &mHdrRaw, UHDR_HDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_compressed_image(enc, &mSdrCompressed, UHDR_SDR_IMG).error_code, UHDR_CODEC_OK);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_AVIF).error_code, UHDR_CODEC_OK);

  uhdr_error_info_t err = uhdr_encode(enc);
  EXPECT_EQ(err.error_code, UHDR_CODEC_UNSUPPORTED_FEATURE);

  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, AvifPreservesRawAlpha) {
  AlphaTestImages images;
  std::vector<uint8_t> encoded;
  uhdr_error_info_t status =
      encodeRawAlphaImage(UHDR_CODEC_AVIF, &images.hdr1010102Desc, nullptr, &encoded);
  if (encoderUnavailable(status)) {
    GTEST_SKIP() << "AV1 encoder plugin not available in environment: " << status.detail;
  }
  ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << (status.has_detail ? status.detail : "no detail");
  // This fixture has straight alpha on the primary AVIF item. Its gain-map item is ordinary
  // monochrome and is not alpha-bearing. When item inspection is available, the routing assertion
  // covers the primary-alpha form; the helper below also performs a real SDR decode to RGBA.
  EXPECT_EQ(is_uhdr_image(encoded.data(), static_cast<int>(encoded.size())), 1);
  expectSupportedHeifGainmap(encoded.data(), encoded.size(),
                             heif_have_decoder_for_format(heif_compression_AV1));
  EXPECT_TRUE(decodedAlphaMatches(encoded));
  expectRawAlphaPreserved(UHDR_CODEC_AVIF, &images.hdrHalfFloatDesc);
  images.makeHdr1010102AlphaOpaque();
  expectRawAlphaPreserved(UHDR_CODEC_AVIF, &images.hdr1010102Desc, &images.sdr8888Desc);
}

TEST_F(UltraHdrApiTest, HeifPreservesRawAlpha) {
  AlphaTestImages images;
  std::vector<uint8_t> encoded;
  uhdr_error_info_t status =
      encodeRawAlphaImage(UHDR_CODEC_HEIF, &images.hdr1010102Desc, nullptr, &encoded);
  if (encoderUnavailable(status)) {
    GTEST_SKIP() << "HEVC encoder plugin not available in environment: " << status.detail;
  }
  ASSERT_EQ(status.error_code, UHDR_CODEC_OK) << (status.has_detail ? status.detail : "no detail");
  EXPECT_EQ(is_uhdr_image(encoded.data(), static_cast<int>(encoded.size())), 1);
  expectSupportedHeifGainmap(encoded.data(), encoded.size(),
                             heif_have_decoder_for_format(heif_compression_HEVC));
  EXPECT_TRUE(decodedAlphaMatches(encoded));
  expectRawAlphaPreserved(UHDR_CODEC_HEIF, &images.hdrHalfFloatDesc);
  images.makeHdr1010102AlphaOpaque();
  expectRawAlphaPreserved(UHDR_CODEC_HEIF, &images.hdr1010102Desc, &images.sdr8888Desc);
}
#else
TEST_F(UltraHdrApiTest, HeicCodecUnsupportedWhenDisabled) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_HEIF).error_code, UHDR_CODEC_UNSUPPORTED_FEATURE);
  uhdr_release_encoder(enc);
}

TEST_F(UltraHdrApiTest, AvifCodecUnsupportedWhenDisabled) {
  uhdr_codec_private_t* enc = uhdr_create_encoder();
  ASSERT_NE(enc, nullptr);
  EXPECT_EQ(uhdr_enc_set_output_format(enc, UHDR_CODEC_AVIF).error_code, UHDR_CODEC_UNSUPPORTED_FEATURE);
  uhdr_release_encoder(enc);
}
#endif

}  // namespace ultrahdr
