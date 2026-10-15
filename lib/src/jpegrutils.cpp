/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
 * https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
 * <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
 * option. This file may not be copied, modified, or distributed
 * except according to those terms.
 */

#include <algorithm>
#include <cmath>

#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/jpegr.h"
#include "ultrahdr/jpegrutils.h"

#include "image_io/xml/xml_reader.h"
#include "image_io/xml/xml_writer.h"
#include "image_io/base/message_handler.h"
#include "image_io/xml/xml_element_rules.h"
#include "image_io/xml/xml_handler.h"
#include "image_io/xml/xml_rule.h"

using namespace photos_editing_formats::image_io;
using namespace std;

namespace ultrahdr {
/*
 * Helper function used for generating XMP metadata.
 *
 * @param prefix The prefix part of the name.
 * @param suffix The suffix part of the name.
 * @return A name of the form "prefix:suffix".
 */
static inline string Name(const string& prefix, const string& suffix) {
  std::stringstream ss;
  ss << prefix << ":" << suffix;
  return ss.str();
}

DataStruct::DataStruct(size_t s) {
  data = malloc(s);
  length = s;
  memset(data, 0, s);
  writePos = 0;
}

DataStruct::~DataStruct() {
  if (data != nullptr) {
    free(data);
  }
}

void* DataStruct::getData() { return data; }

size_t DataStruct::getLength() { return length; }

size_t DataStruct::getBytesWritten() { return writePos; }

bool DataStruct::write8(uint8_t value) {
  uint8_t v = value;
  return write(&v, 1);
}

bool DataStruct::write16(uint16_t value) {
  uint16_t v = value;
  return write(&v, 2);
}

bool DataStruct::write32(uint32_t value) {
  uint32_t v = value;
  return write(&v, 4);
}

bool DataStruct::write(const void* src, size_t size) {
  if (writePos + size > length) {
    ALOGE("Writing out of boundary: write position: %zd, size: %zd, capacity: %zd", writePos, size,
          length);
    return false;
  }
  memcpy((uint8_t*)data + writePos, src, size);
  writePos += size;
  return true;
}

/*
 * Helper function used for writing data to destination.
 */
uhdr_error_info_t Write(uhdr_compressed_image_t* destination, const void* source, size_t length,
                        size_t& position) {
  if (position + length > destination->capacity) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_MEM_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "output buffer to store compressed data is too small: write position: %zd, size: %zd, "
             "capacity: %zd",
             position, length, destination->capacity);
    return status;
  }

  memcpy((uint8_t*)destination->data + sizeof(uint8_t) * position, source, length);
  position += length;
  return g_no_error;
}

// Extremely simple XML Handler - just searches for interesting elements
class XMPXmlHandler : public XmlHandler {
 public:
  XMPXmlHandler() : XmlHandler() {
    state = NotStrarted;
    versionFound = false;
    minContentBoostFound = false;
    maxContentBoostFound = false;
    gammaFound = false;
    offsetSdrFound = false;
    offsetHdrFound = false;
    hdrCapacityMinFound = false;
    hdrCapacityMaxFound = false;
    baseRenditionIsHdrFound = false;
    appleVersionFound = false;
    appleVersionDuplicate = false;
    appleHeadroomFound = false;
    appleHeadroomDuplicate = false;
    appleNativeFormatFound = false;
    appleNativeFormatDuplicate = false;
    appleStoredFormatFound = false;
    appleStoredFormatDuplicate = false;
    isApple = false;
  }

  enum ParseState { NotStrarted, Started, Done };

  virtual DataMatchResult StartElement(const XmlTokenContext& context) {
    string val;
    if (context.BuildTokenValue(&val)) {
      if (!val.compare(containerName)) {
        state = Started;
      } else if (state == Started) {
        if (val.find(kAppleMapVersion) != string::npos) {
          lastElementName = kAppleMapVersion;
          isApple = true;
          if (appleVersionFound) appleVersionDuplicate = true;
          appleVersionFound = true;
        } else if (val.find(kAppleMapHeadroom) != string::npos) {
          lastElementName = kAppleMapHeadroom;
          if (appleHeadroomFound) appleHeadroomDuplicate = true;
          appleHeadroomFound = true;
        } else if (val.find(kAppleNativeFormat) != string::npos) {
          lastElementName = kAppleNativeFormat;
          if (appleNativeFormatFound) appleNativeFormatDuplicate = true;
          appleNativeFormatFound = true;
        } else if (val.find(kAppleStoredFormat) != string::npos) {
          lastElementName = kAppleStoredFormat;
          if (appleStoredFormatFound) appleStoredFormatDuplicate = true;
          appleStoredFormatFound = true;
        } else {
          lastElementName = "Unknown";
        }
      } else {
        if (state != Done) {
          state = NotStrarted;
        }
      }
    }
    return context.GetResult();
  }

  virtual DataMatchResult FinishElement(const XmlTokenContext& context) {
    if (state == Started) {
      if (lastElementName.empty()) {
        state = Done;
        lastAttributeName = "";
      } else {
        lastElementName = "";
      }
    }
    return context.GetResult();
  }

  virtual DataMatchResult ElementContent(const XmlTokenContext& context) {
    string val;
    if (state == Started && !lastElementName.empty()) {
      if (context.BuildTokenValue(&val)) {
        if (!lastElementName.compare(kAppleMapVersion)) {
          appleVersionStr = val;
          isApple = true;
        } else if (!lastElementName.compare(kAppleMapHeadroom)) {
          appleHeadroomStr = val;
        } else if (!lastElementName.compare(kAppleNativeFormat)) {
          appleNativeFormatStr = val;
        } else if (!lastElementName.compare(kAppleStoredFormat)) {
          appleStoredFormatStr = val;
        }
      }
    }
    return context.GetResult();
  }

  virtual DataMatchResult AttributeName(const XmlTokenContext& context) {
    string val;
    if (state == Started) {
      if (context.BuildTokenValue(&val)) {
        if (!val.compare(versionAttrName)) {
          lastAttributeName = versionAttrName;
        } else if (!val.compare(maxContentBoostAttrName)) {
          lastAttributeName = maxContentBoostAttrName;
        } else if (!val.compare(minContentBoostAttrName)) {
          lastAttributeName = minContentBoostAttrName;
        } else if (!val.compare(gammaAttrName)) {
          lastAttributeName = gammaAttrName;
        } else if (!val.compare(offsetSdrAttrName)) {
          lastAttributeName = offsetSdrAttrName;
        } else if (!val.compare(offsetHdrAttrName)) {
          lastAttributeName = offsetHdrAttrName;
        } else if (!val.compare(hdrCapacityMinAttrName)) {
          lastAttributeName = hdrCapacityMinAttrName;
        } else if (!val.compare(hdrCapacityMaxAttrName)) {
          lastAttributeName = hdrCapacityMaxAttrName;
        } else if (!val.compare(baseRenditionIsHdrAttrName)) {
          lastAttributeName = baseRenditionIsHdrAttrName;
        } else {
          lastAttributeName = "";
        }
      }
    }
    return context.GetResult();
  }

  virtual DataMatchResult AttributeValue(const XmlTokenContext& context) {
    string val;
    if (state == Started) {
      if (context.BuildTokenValue(&val, true)) {
        if (!lastAttributeName.compare(versionAttrName)) {
          versionStr = val;
          versionFound = true;
        } else if (!lastAttributeName.compare(maxContentBoostAttrName)) {
          maxContentBoostStr = val;
          maxContentBoostFound = true;
        } else if (!lastAttributeName.compare(minContentBoostAttrName)) {
          minContentBoostStr = val;
          minContentBoostFound = true;
        } else if (!lastAttributeName.compare(gammaAttrName)) {
          gammaStr = val;
          gammaFound = true;
        } else if (!lastAttributeName.compare(offsetSdrAttrName)) {
          offsetSdrStr = val;
          offsetSdrFound = true;
        } else if (!lastAttributeName.compare(offsetHdrAttrName)) {
          offsetHdrStr = val;
          offsetHdrFound = true;
        } else if (!lastAttributeName.compare(hdrCapacityMinAttrName)) {
          hdrCapacityMinStr = val;
          hdrCapacityMinFound = true;
        } else if (!lastAttributeName.compare(hdrCapacityMaxAttrName)) {
          hdrCapacityMaxStr = val;
          hdrCapacityMaxFound = true;
        } else if (!lastAttributeName.compare(baseRenditionIsHdrAttrName)) {
          baseRenditionIsHdrStr = val;
          baseRenditionIsHdrFound = true;
        }
      }
    }
    return context.GetResult();
  }

  bool getVersion(string* version, bool* present) {
    if (state == Done) {
      *version = versionStr;
      *present = versionFound;
      return true;
    } else {
      return false;
    }
  }

  bool getMaxContentBoost(float* max_content_boost, bool* present) {
    if (state == Done) {
      *present = maxContentBoostFound;
      stringstream ss(maxContentBoostStr);
      float val;
      if (ss >> val) {
        *max_content_boost = isApple ? val : exp2(val);
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getMinContentBoost(float* min_content_boost, bool* present) {
    if (state == Done) {
      *present = minContentBoostFound;
      stringstream ss(minContentBoostStr);
      float val;
      if (ss >> val) {
        *min_content_boost = exp2(val);
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getGamma(float* gamma, bool* present) {
    if (state == Done) {
      *present = gammaFound;
      stringstream ss(gammaStr);
      float val;
      if (ss >> val) {
        *gamma = val;
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getOffsetSdr(float* offset_sdr, bool* present) {
    if (state == Done) {
      *present = offsetSdrFound;
      stringstream ss(offsetSdrStr);
      float val;
      if (ss >> val) {
        *offset_sdr = val;
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getOffsetHdr(float* offset_hdr, bool* present) {
    if (state == Done) {
      *present = offsetHdrFound;
      stringstream ss(offsetHdrStr);
      float val;
      if (ss >> val) {
        *offset_hdr = val;
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getHdrCapacityMin(float* hdr_capacity_min, bool* present) {
    if (state == Done) {
      *present = hdrCapacityMinFound;
      stringstream ss(hdrCapacityMinStr);
      float val;
      if (ss >> val) {
        *hdr_capacity_min = exp2(val);
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getHdrCapacityMax(float* hdr_capacity_max, bool* present) {
    if (state == Done) {
      *present = hdrCapacityMaxFound;
      stringstream ss(hdrCapacityMaxStr);
      float val;
      if (ss >> val) {
        *hdr_capacity_max = exp2(val);
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getBaseRenditionIsHdr(bool* base_rendition_is_hdr, bool* present) {
    if (state == Done) {
      *present = baseRenditionIsHdrFound;
      if (!baseRenditionIsHdrStr.compare("False")) {
        *base_rendition_is_hdr = false;
        return true;
      } else if (!baseRenditionIsHdrStr.compare("True")) {
        *base_rendition_is_hdr = true;
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  bool getIsApple() const { return isApple; }

  bool getAppleFields(string* version, string* headroom, bool* headroom_present,
                      string* native_format, bool* native_format_present,
                      string* stored_format, bool* stored_format_present,
                      bool* headroom_duplicate) const {
    *version = appleVersionStr;
    *headroom = appleHeadroomStr;
    *headroom_present = appleHeadroomFound;
    *native_format = appleNativeFormatStr;
    *native_format_present = appleNativeFormatFound;
    *stored_format = appleStoredFormatStr;
    *stored_format_present = appleStoredFormatFound;
    *headroom_duplicate = appleHeadroomDuplicate;
    return appleVersionFound && !appleVersionDuplicate && !appleNativeFormatDuplicate &&
           !appleStoredFormatDuplicate;
  }

 private:
  static const string containerName;

  static const string versionAttrName;
  string versionStr;
  bool versionFound;
  string appleVersionStr;
  static const string maxContentBoostAttrName;
  string maxContentBoostStr;
  bool maxContentBoostFound;
  static const string minContentBoostAttrName;
  string minContentBoostStr;
  bool minContentBoostFound;
  static const string gammaAttrName;
  string gammaStr;
  bool gammaFound;
  static const string offsetSdrAttrName;
  string offsetSdrStr;
  bool offsetSdrFound;
  static const string offsetHdrAttrName;
  string offsetHdrStr;
  bool offsetHdrFound;
  static const string hdrCapacityMinAttrName;
  string hdrCapacityMinStr;
  bool hdrCapacityMinFound;
  static const string hdrCapacityMaxAttrName;
  string hdrCapacityMaxStr;
  bool hdrCapacityMaxFound;
  static const string baseRenditionIsHdrAttrName;
  string baseRenditionIsHdrStr;
  bool baseRenditionIsHdrFound;
  bool appleVersionFound;
  bool appleVersionDuplicate;
  bool appleHeadroomFound;
  bool appleHeadroomDuplicate;
  bool appleNativeFormatFound;
  bool appleNativeFormatDuplicate;
  bool appleStoredFormatFound;
  bool appleStoredFormatDuplicate;
  string appleNativeFormatStr;
  string appleStoredFormatStr;
  string appleHeadroomStr;

  string lastAttributeName;
  string lastElementName;
  ParseState state;
  bool isApple;

  static const string kAppleMapVersion;
  static const string kAppleMapHeadroom;
  static const string kAppleNativeFormat;
  static const string kAppleStoredFormat;
};

// GContainer XMP constants - URI and namespace prefix
const string kContainerUri = "http://ns.google.com/photos/1.0/container/";
const string kContainerPrefix = "Container";

// GContainer XMP constants - element and attribute names
const string kConDirectory = Name(kContainerPrefix, "Directory");
const string kConItem = Name(kContainerPrefix, "Item");

// GContainer XMP constants - names for XMP handlers
const string XMPXmlHandler::containerName = "rdf:Description";
// Item XMP constants - URI and namespace prefix
const string kItemUri = "http://ns.google.com/photos/1.0/container/item/";
const string kItemPrefix = "Item";

// Item XMP constants - element and attribute names
const string kItemLength = Name(kItemPrefix, "Length");
const string kItemMime = Name(kItemPrefix, "Mime");
const string kItemSemantic = Name(kItemPrefix, "Semantic");

// Item XMP constants - element and attribute values
const string kSemanticPrimary = "Primary";
const string kSemanticGainMap = "GainMap";
const string kMimeImageJpeg = "image/jpeg";

// GainMap XMP constants - URI and namespace prefix
const string kGainMapUri = "http://ns.adobe.com/hdr-gain-map/1.0/";
const string kGainMapPrefix = "hdrgm";

// GainMap XMP constants - element and attribute names
const string kMapVersion = Name(kGainMapPrefix, "Version");
const string kMapGainMapMin = Name(kGainMapPrefix, "GainMapMin");
const string kMapGainMapMax = Name(kGainMapPrefix, "GainMapMax");
const string kMapGamma = Name(kGainMapPrefix, "Gamma");
const string kMapOffsetSdr = Name(kGainMapPrefix, "OffsetSDR");
const string kMapOffsetHdr = Name(kGainMapPrefix, "OffsetHDR");
const string kMapHDRCapacityMin = Name(kGainMapPrefix, "HDRCapacityMin");
const string kMapHDRCapacityMax = Name(kGainMapPrefix, "HDRCapacityMax");
const string kMapBaseRenditionIsHDR = Name(kGainMapPrefix, "BaseRenditionIsHDR");

// GainMap XMP constants - names for XMP handlers
const string XMPXmlHandler::versionAttrName = kMapVersion;
const string XMPXmlHandler::minContentBoostAttrName = kMapGainMapMin;
const string XMPXmlHandler::maxContentBoostAttrName = kMapGainMapMax;
const string XMPXmlHandler::gammaAttrName = kMapGamma;
const string XMPXmlHandler::offsetSdrAttrName = kMapOffsetSdr;
const string XMPXmlHandler::offsetHdrAttrName = kMapOffsetHdr;
const string XMPXmlHandler::hdrCapacityMinAttrName = kMapHDRCapacityMin;
const string XMPXmlHandler::hdrCapacityMaxAttrName = kMapHDRCapacityMax;
const string XMPXmlHandler::baseRenditionIsHdrAttrName = kMapBaseRenditionIsHDR;
const string XMPXmlHandler::kAppleMapVersion = "HDRGainMapVersion";
const string XMPXmlHandler::kAppleMapHeadroom = "HDRGainMapHeadroom";
const string XMPXmlHandler::kAppleNativeFormat = "NativeFormat";
const string XMPXmlHandler::kAppleStoredFormat = "StoredFormat";

static bool readU16(const uint8_t* data, size_t size, uint16_t* value, size_t* offset,
                    bool isBigEndian) {
  if (*offset > size || size - *offset < 2) return false;
  if (isBigEndian) {
    *value = (data[*offset] << 8) | data[*offset + 1];
  } else {
    *value = data[*offset] | (data[*offset + 1] << 8);
  }
  *offset += 2;
  return true;
}

static bool readU32(const uint8_t* data, size_t size, uint32_t* value, size_t* offset,
                    bool isBigEndian) {
  if (*offset > size || size - *offset < 4) return false;
  if (isBigEndian) {
    *value = (data[*offset] << 24) | (data[*offset + 1] << 16) | (data[*offset + 2] << 8) |
             data[*offset + 3];
  } else {
    *value = data[*offset] | (data[*offset + 1] << 8) | (data[*offset + 2] << 16) |
             (data[*offset + 3] << 24);
  }
  *offset += 4;
  return true;
}

static bool readS32(const uint8_t* data, size_t size, int32_t* value, size_t* offset,
                    bool isBigEndian) {
  uint32_t u;
  if (!readU32(data, size, &u, offset, isBigEndian)) return false;
  *value = (int32_t)u;
  return true;
}

// Resolves an offset stored in the EXIF data, relative to base, to an offset in
// the buffer. Fails if the result lies past the end of the buffer. Every offset
// read from the EXIF data must go through this, so that the sum never wraps.
static bool resolveOffset(size_t base, uint32_t relOffset, size_t size, size_t* offset) {
  if (base > size || relOffset > size - base) return false;
  *offset = base + relOffset;
  return true;
}

static bool matchBytes(const uint8_t* data, size_t size, size_t offset, const uint8_t* expected,
                       size_t expectedSize) {
  if (offset > size || size - offset < expectedSize) return false;
  return !memcmp(&data[offset], expected, expectedSize);
}

static bool getExifAppleHeadroom(const uint8_t* exif, size_t size, float* altHeadroom) {
  *altHeadroom = 0.0f;
  size_t offset = 0;

  // Find TIFF header offset
  if (size < 6 || memcmp(exif, "Exif\0\0", 6) != 0) {
    // Some EXIF blobs might not have the APP1 EXIF marker depending on where it
    // was extracted. Try to find the TIFF header by looking for II*\0 or MM\0*
    bool found = false;
    for (size_t i = 0; i + 4 <= size; i++) {
      if ((exif[i] == 'I' && exif[i + 1] == 'I' && exif[i + 2] == 0x2A && exif[i + 3] == 0) ||
          (exif[i] == 'M' && exif[i + 1] == 'M' && exif[i + 2] == 0 && exif[i + 3] == 0x2A)) {
        offset = i;
        found = true;
        break;
      }
    }
    if (!found) return false;
  } else {
    offset = 6;
  }

  if (offset + 4 > size) return false;
  bool isBigEndian = (exif[offset] == 'M');
  offset += 4;  // Skip the TIFF header.

  uint32_t offsetToIfd;
  if (!readU32(exif, size, &offsetToIfd, &offset, isBigEndian)) return false;

  const uint8_t appleMakerNotesHeader[] = {'A', 'p', 'p',  'l',  'e',  ' ', 'i',
                                           'O', 'S', 0x00, 0x00, 0x01, 'M', 'M'};
  const size_t appleMakerNotesHeaderSize = sizeof(appleMakerNotesHeader);
  bool inAppleMakerNotes = false;

  bool hasMaker33 = false;
  bool hasMaker48 = false;
  double maker33 = 0.0;
  double maker48 = 0.0;

  int numIfds = 0;
  const int maxIfds = 3;  // Prevent infinite looping caused by malformed data.

  // Exif offset is relative to TIFF header
  const size_t tiffHeaderOffset = offset - 8;

  while (offsetToIfd != 0 && numIfds++ < maxIfds) {
    size_t ifdOffset;
    if (!resolveOffset(tiffHeaderOffset, offsetToIfd, size, &ifdOffset)) return false;
    offset = ifdOffset;
    bool offsetToNextIfdAlreadySet = false;

    uint16_t fieldCount;
    if (!readU16(exif, size, &fieldCount, &offset, isBigEndian)) return false;

    for (uint16_t field = 0; field < fieldCount; ++field) {
      uint16_t tagId;
      uint16_t dataFormat;
      uint32_t numComponents;
      uint32_t tagData;
      if (!readU16(exif, size, &tagId, &offset, isBigEndian)) return false;
      if (!readU16(exif, size, &dataFormat, &offset, isBigEndian)) return false;
      if (!readU32(exif, size, &numComponents, &offset, isBigEndian)) return false;
      if (!readU32(exif, size, &tagData, &offset, isBigEndian)) return false;

      if (tagId == 0x8769) {  // Exif Offset (offset to a sub IFD)
        offsetToIfd = tagData;
        offsetToNextIfdAlreadySet = true;
        break;
      } else if (tagId == 0x927c) {  // Maker Notes
        size_t makerNotesOffset;
        // The first IFD follows the header; its offset must fit in 32 bits too.
        if (tagData <= UINT32_MAX - appleMakerNotesHeaderSize &&
            resolveOffset(tiffHeaderOffset, tagData, size, &makerNotesOffset) &&
            matchBytes(exif, size, makerNotesOffset, appleMakerNotesHeader,
                       appleMakerNotesHeaderSize)) {
          offsetToIfd = tagData + (uint32_t)appleMakerNotesHeaderSize;
          inAppleMakerNotes = true;
          offsetToNextIfdAlreadySet = true;
          isBigEndian = true;  // Apple Maker Notes are always big endian.
          break;
        }
      } else if (inAppleMakerNotes && (tagId == 33 || tagId == 48)) {
        bool& found = tagId == 33 ? hasMaker33 : hasMaker48;
        if (found || dataFormat != 10 || numComponents != 1) return false;
        // Offsets in the Apple Maker Notes are relative to the Maker Notes
        // header, which precedes their first IFD.
        if (ifdOffset < appleMakerNotesHeaderSize) return false;
        size_t tmpOffset;
        if (!resolveOffset(ifdOffset - appleMakerNotesHeaderSize, tagData, size, &tmpOffset)) {
          return false;
        }
        int32_t numerator;
        int32_t denominator;
        if (!readS32(exif, size, &numerator, &tmpOffset, isBigEndian)) return false;
        if (!readS32(exif, size, &denominator, &tmpOffset, isBigEndian) || denominator == 0) {
          return false;
        }
        const double v = (double)numerator / (double)denominator;
        if (!std::isfinite(v)) return false;
        if (tagId == 33) {
          maker33 = v;
        } else {
          maker48 = v;
        }
        found = true;
      }
    }

    if (!offsetToNextIfdAlreadySet) {
      if (!readU32(exif, size, &offsetToIfd, &offset, isBigEndian)) return false;
    }
  }

  if (!hasMaker33 || !hasMaker48) return false;

  double stops;
  if (maker33 < 1.0) {
    if (maker48 <= 0.01) {
      stops = -20.0 * maker48 + 1.8;
    } else {
      stops = -0.101 * maker48 + 1.601;
    }
  } else {
    if (maker48 <= 0.01) {
      stops = -70.0 * maker48 + 3.0;
    } else {
      stops = -0.303 * maker48 + 2.303;
    }
  }

  const double headroom = pow(2.0, (std::max)(stops, 0.0));
  if (!std::isfinite(headroom) || headroom > FLT_MAX) return false;
  *altHeadroom = static_cast<float>(headroom);
  if (!std::isfinite(*altHeadroom) || *altHeadroom < 1.0f) return false;
  return true;
}

static bool parseAppleFloat(const string& text, float* value) {
  stringstream stream(text);
  double parsed;
  if (!(stream >> parsed)) return false;
  stream >> std::ws;
  if (!stream.eof() || !std::isfinite(parsed) || parsed > FLT_MAX || parsed < -FLT_MAX) {
    return false;
  }
  *value = static_cast<float>(parsed);
  return std::isfinite(*value);
}

static bool parseAppleUint32(const string& text, uint32_t* value) {
  stringstream stream(text);
  uint64_t parsed;
  if (!(stream >> parsed)) return false;
  stream >> std::ws;
  if (!stream.eof() || parsed > UINT32_MAX) return false;
  *value = static_cast<uint32_t>(parsed);
  return true;
}

uhdr_error_info_t getMetadataFromXMP(uint8_t* xmp_data, size_t xmp_size, uint8_t* exif_data,
                                     int exif_size, uhdr_gainmap_metadata_ext_t* metadata) {
  metadata->is_apple_gainmap = false;
  metadata->apple_gainmap_version = 0;
  string nameSpace = "http://ns.adobe.com/xap/1.0/\0";

  if (xmp_size < nameSpace.size() + 2) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "size of xmp block is expected to be atleast %zd bytes, received only %zd bytes",
             nameSpace.size() + 2, xmp_size);
    return status;
  }

  if (strncmp(reinterpret_cast<char*>(xmp_data), nameSpace.c_str(), nameSpace.size())) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail,
             "mismatch in namespace of xmp block. Expected %s, Got %.*s", nameSpace.c_str(),
             (int)nameSpace.size(), reinterpret_cast<char*>(xmp_data));
    return status;
  }

  // Position the pointers to the start of XMP XML portion
  xmp_data += nameSpace.size() + 1;
  xmp_size -= nameSpace.size() + 1;
  XMPXmlHandler handler;

  // xml parser fails to parse packet header, wrapper. remove them before handing the data to
  // parser. if there is no packet header, do nothing otherwise go to the position of '<' without
  // '?' after it.
  size_t offset = 0;
  for (size_t i = 0; i < xmp_size - 1; ++i) {
    if (xmp_data[i] == '<') {
      if (xmp_data[i + 1] != '?') {
        offset = i;
        break;
      }
    }
  }
  xmp_data += offset;
  xmp_size -= offset;

  // If there is no packet wrapper, do nothing other wise go to the position of last '>' without '?'
  // before it.
  offset = 0;
  for (size_t i = xmp_size - 1; i >= 1; --i) {
    if (xmp_data[i] == '>') {
      if (xmp_data[i - 1] != '?') {
        offset = xmp_size - (i + 1);
        break;
      }
    }
  }
  xmp_size -= offset;

  // remove padding
  while (xmp_data[xmp_size - 1] != '>' && xmp_size > 1) {
    xmp_size--;
  }

  string str(reinterpret_cast<const char*>(xmp_data), xmp_size);
  MessageHandler msg_handler;
  unique_ptr<XmlRule> rule(new XmlElementRule);
  XmlReader reader(&handler, &msg_handler);
  reader.StartParse(std::move(rule));
  reader.Parse(str);
  reader.FinishParse();
  if (reader.HasErrors()) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_UNKNOWN_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "xml parser returned with error");
    return status;
  }

  if (handler.getIsApple()) {
    string apple_version;
    string apple_headroom;
    string native_format;
    string stored_format;
    bool apple_headroom_present = false;
    bool native_format_present = false;
    bool stored_format_present = false;
    bool apple_headroom_duplicate = false;
    uint32_t version = 0;
    uint32_t native_format_value = 0;
    uint32_t stored_format_value = 0;
    if (!handler.getAppleFields(&apple_version, &apple_headroom, &apple_headroom_present,
                                &native_format, &native_format_present, &stored_format,
                                &stored_format_present, &apple_headroom_duplicate) ||
        !parseAppleUint32(apple_version, &version) ||
        (version != 65536 && version != 131072) ||
        (native_format_present &&
         (!parseAppleUint32(native_format, &native_format_value) ||
          native_format_value != 1278226488u)) ||
        (stored_format_present &&
         (!parseAppleUint32(stored_format, &stored_format_value) ||
          stored_format_value != 1278226488u))) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_UNSUPPORTED_FEATURE;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "unsupported or malformed Apple gain-map version or pixel format");
      return status;
    }

    metadata->is_apple_gainmap = true;
    metadata->apple_gainmap_version = version;
    metadata->version = kJpegrVersion;
    for (int c = 0; c < 3; ++c) {
      metadata->gamma[c] = 1.0f;
      metadata->min_content_boost[c] = 1.0f;
      metadata->offset_sdr[c] = 0.0f;
      metadata->offset_hdr[c] = 0.0f;
    }
    metadata->hdr_capacity_min = 1.0f;
    // Apple applies the gain map in the base image's color space, and its gain-map
    // image carries no ICC profile.
    metadata->use_base_cg = true;

    float max_content_boost = 0.0f;
    bool have_headroom = exif_data != nullptr && exif_size > 0 &&
                         getExifAppleHeadroom(exif_data, exif_size, &max_content_boost);
    if (!have_headroom && apple_headroom_present && !apple_headroom_duplicate &&
        parseAppleFloat(apple_headroom, &max_content_boost) && max_content_boost >= 1.0f) {
      have_headroom = true;
    }
    if (!have_headroom) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "Apple gain-map headroom is unavailable or invalid in Maker Notes and XMP");
      return status;
    }
    if (max_content_boost < 1.0f || !std::isfinite(max_content_boost)) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail,
               "Apple gain-map headroom must be finite and at least one");
      return status;
    }
    for (int c = 0; c < 3; ++c) metadata->max_content_boost[c] = max_content_boost;
    metadata->hdr_capacity_max = max_content_boost;

    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_OK;
    status.has_detail = 0;
    return status;
  }

  // Apply default values to any not-present fields, except for Version,
  // maxContentBoost, and hdrCapacityMax, which are required. Return false if
  // we encounter a present field that couldn't be parsed, since this
  // indicates it is invalid (eg. string where there should be a float).
  bool present = false;
  if (!handler.getVersion(&metadata->version, &present) || !present) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "xml parse error, could not find attribute %s",
             kMapVersion.c_str());
    return status;
  }
  if (!handler.getMaxContentBoost(&metadata->max_content_boost[0], &present) || !present) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "xml parse error, could not find attribute %s",
             kMapGainMapMax.c_str());
    return status;
  }
  if (!handler.getHdrCapacityMax(&metadata->hdr_capacity_max, &present) || !present) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "xml parse error, could not find attribute %s",
             kMapHDRCapacityMax.c_str());
    return status;
  }
  if (!handler.getMinContentBoost(&metadata->min_content_boost[0], &present)) {
    if (present) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "xml parse error, unable to parse attribute %s",
               kMapGainMapMin.c_str());
      return status;
    }
    metadata->min_content_boost[0] = 1.0f;
  }
  if (!handler.getGamma(&metadata->gamma[0], &present)) {
    if (present) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "xml parse error, unable to parse attribute %s",
               kMapGamma.c_str());
      return status;
    }
    metadata->gamma[0] = 1.0f;
  }
  if (!handler.getOffsetSdr(&metadata->offset_sdr[0], &present)) {
    if (present) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "xml parse error, unable to parse attribute %s",
               kMapOffsetSdr.c_str());
      return status;
    }
    metadata->offset_sdr[0] = 1.0f / 64.0f;
  }
  if (!handler.getOffsetHdr(&metadata->offset_hdr[0], &present)) {
    if (present) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "xml parse error, unable to parse attribute %s",
               kMapOffsetHdr.c_str());
      return status;
    }
    metadata->offset_hdr[0] = 1.0f / 64.0f;
  }
  if (!handler.getHdrCapacityMin(&metadata->hdr_capacity_min, &present)) {
    if (present) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "xml parse error, unable to parse attribute %s",
               kMapHDRCapacityMin.c_str());
      return status;
    }
    metadata->hdr_capacity_min = 1.0f;
  }

  bool base_rendition_is_hdr;
  if (!handler.getBaseRenditionIsHdr(&base_rendition_is_hdr, &present)) {
    if (present) {
      uhdr_error_info_t status;
      status.error_code = UHDR_CODEC_ERROR;
      status.has_detail = 1;
      snprintf(status.detail, sizeof status.detail, "xml parse error, unable to parse attribute %s",
               kMapBaseRenditionIsHDR.c_str());
      return status;
    }
    base_rendition_is_hdr = false;
  }
  if (base_rendition_is_hdr) {
    uhdr_error_info_t status;
    status.error_code = UHDR_CODEC_ERROR;
    status.has_detail = 1;
    snprintf(status.detail, sizeof status.detail, "hdr intent as base rendition is not supported");
    return status;
  }
  metadata->use_base_cg = true;
  std::fill_n(metadata->min_content_boost + 1, 2, metadata->min_content_boost[0]);
  std::fill_n(metadata->max_content_boost + 1, 2, metadata->max_content_boost[0]);
  std::fill_n(metadata->gamma + 1, 2, metadata->gamma[0]);
  std::fill_n(metadata->offset_hdr + 1, 2, metadata->offset_hdr[0]);
  std::fill_n(metadata->offset_sdr + 1, 2, metadata->offset_sdr[0]);

  return uhdr_validate_gainmap_metadata_descriptor(metadata);
}

string generateXmpForPrimaryImage(size_t secondary_image_length,
                                  uhdr_gainmap_metadata_ext_t& metadata) {
  const vector<string> kConDirSeq({kConDirectory, string("rdf:Seq")});
  const vector<string> kLiItem({string("rdf:li"), kConItem});

  std::stringstream ss;
  photos_editing_formats::image_io::XmlWriter writer(ss);
  writer.StartWritingElement("x:xmpmeta");
  writer.WriteXmlns("x", "adobe:ns:meta/");
  writer.WriteAttributeNameAndValue("x:xmptk", "Adobe XMP Core 5.1.2");
  writer.StartWritingElement("rdf:RDF");
  writer.WriteXmlns("rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
  writer.StartWritingElement("rdf:Description");
  writer.WriteXmlns(kContainerPrefix, kContainerUri);
  writer.WriteXmlns(kItemPrefix, kItemUri);
  writer.WriteXmlns(kGainMapPrefix, kGainMapUri);
  writer.WriteAttributeNameAndValue(kMapVersion, metadata.version);

  writer.StartWritingElements(kConDirSeq);

  size_t item_depth = writer.StartWritingElement("rdf:li");
  writer.WriteAttributeNameAndValue("rdf:parseType", "Resource");
  writer.StartWritingElement(kConItem);
  writer.WriteAttributeNameAndValue(kItemSemantic, kSemanticPrimary);
  writer.WriteAttributeNameAndValue(kItemMime, kMimeImageJpeg);
  writer.FinishWritingElementsToDepth(item_depth);

  writer.StartWritingElement("rdf:li");
  writer.WriteAttributeNameAndValue("rdf:parseType", "Resource");
  writer.StartWritingElement(kConItem);
  writer.WriteAttributeNameAndValue(kItemSemantic, kSemanticGainMap);
  writer.WriteAttributeNameAndValue(kItemMime, kMimeImageJpeg);
  writer.WriteAttributeNameAndValue(kItemLength, secondary_image_length);

  writer.FinishWriting();

  return ss.str();
}

string generateXmpForSecondaryImage(uhdr_gainmap_metadata_ext_t& metadata) {
  const vector<string> kConDirSeq({kConDirectory, string("rdf:Seq")});

  std::stringstream ss;
  photos_editing_formats::image_io::XmlWriter writer(ss);
  writer.StartWritingElement("x:xmpmeta");
  writer.WriteXmlns("x", "adobe:ns:meta/");
  writer.WriteAttributeNameAndValue("x:xmptk", "Adobe XMP Core 5.1.2");
  writer.StartWritingElement("rdf:RDF");
  writer.WriteXmlns("rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
  writer.StartWritingElement("rdf:Description");
  writer.WriteAttributeNameAndValue("rdf:about", "");
  writer.WriteXmlns(kGainMapPrefix, kGainMapUri);
  writer.WriteAttributeNameAndValue(kMapVersion, metadata.version);
  writer.WriteAttributeNameAndValue(kMapGainMapMin, log2(metadata.min_content_boost[0]));
  writer.WriteAttributeNameAndValue(kMapGainMapMax, log2(metadata.max_content_boost[0]));
  writer.WriteAttributeNameAndValue(kMapGamma, metadata.gamma[0]);
  writer.WriteAttributeNameAndValue(kMapOffsetSdr, metadata.offset_sdr[0]);
  writer.WriteAttributeNameAndValue(kMapOffsetHdr, metadata.offset_hdr[0]);
  writer.WriteAttributeNameAndValue(kMapHDRCapacityMin, log2(metadata.hdr_capacity_min));
  writer.WriteAttributeNameAndValue(kMapHDRCapacityMax, log2(metadata.hdr_capacity_max));
  writer.WriteAttributeNameAndValue(kMapBaseRenditionIsHDR, "False");
  writer.FinishWriting();

  return ss.str();
}

}  // namespace ultrahdr
