#include "panorama_jpeg.h"
#include <cstdio>
#include <cstdlib>
#include <csetjmp>
#include <fstream>
#include <stdexcept>
#include <jpeglib.h>
#include <jerror.h>

namespace robobaton_demo {
namespace {
struct JpegError {
  jpeg_error_mgr base;
  std::jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};
void ErrorExit(j_common_ptr codec) {
  auto* error = reinterpret_cast<JpegError*>(codec->err);
  codec->err->format_message(codec, error->message);
  std::longjmp(error->jump, 1);
}
struct FixedDestination {
  jpeg_destination_mgr base;
};
void InitializeDestination(j_compress_ptr) {}
boolean DestinationFull(j_compress_ptr codec) { ERREXIT(codec, JERR_BUFFER_SIZE); return FALSE; }
void FinishDestination(j_compress_ptr) {}

// The longjmp region contains only C objects; C++ ownership stays in the caller.
bool Encode(const uint8_t* rgb, uint32_t width, uint32_t height,
            uint8_t* output, size_t* size, char* message) {
  jpeg_compress_struct codec{};
  JpegError error{};
  FixedDestination destination{};
  codec.err = jpeg_std_error(&error.base);
  error.base.error_exit = ErrorExit;
  if (setjmp(error.jump)) {
    jpeg_destroy_compress(&codec);
    std::snprintf(message, JMSG_LENGTH_MAX, "%s", error.message);
    return false;
  }
  jpeg_create_compress(&codec);
  destination.base.init_destination = InitializeDestination;
  destination.base.empty_output_buffer = DestinationFull;
  destination.base.term_destination = FinishDestination;
  destination.base.next_output_byte = output;
  destination.base.free_in_buffer = *size;
  codec.dest = &destination.base;
  codec.image_width = width; codec.image_height = height;
  codec.input_components = 3; codec.in_color_space = JCS_RGB;
  jpeg_set_defaults(&codec);
  jpeg_set_quality(&codec, 85, TRUE);
  jpeg_start_compress(&codec, TRUE);
  while (codec.next_scanline < codec.image_height) {
    JSAMPROW row = const_cast<uint8_t*>(rgb + static_cast<size_t>(codec.next_scanline) * width * 3);
    jpeg_write_scanlines(&codec, &row, 1);
  }
  jpeg_finish_compress(&codec);
  *size -= destination.base.free_in_buffer;
  jpeg_destroy_compress(&codec);
  return true;
}
bool Decode(const uint8_t* input, size_t size, uint8_t** output,
            uint32_t* width, uint32_t* height, char* message) {
  jpeg_decompress_struct codec{};
  JpegError error{};
  codec.err = jpeg_std_error(&error.base);
  error.base.error_exit = ErrorExit;
  *output = nullptr;
  if (setjmp(error.jump)) {
    jpeg_destroy_decompress(&codec);
    std::free(*output); *output = nullptr;
    std::snprintf(message, JMSG_LENGTH_MAX, "%s", error.message);
    return false;
  }
  jpeg_create_decompress(&codec);
  jpeg_mem_src(&codec, input, size);
  jpeg_read_header(&codec, TRUE);
  if (codec.image_width > 8192 || codec.image_height > 8192 ||
      static_cast<uint64_t>(codec.image_width) * codec.image_height > 16777216)
    ERREXIT1(&codec, JERR_IMAGE_TOO_BIG, 8192);
  codec.out_color_space = JCS_RGB;
  jpeg_start_decompress(&codec);
  *width = codec.output_width; *height = codec.output_height;
  *output = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(*width) * *height * 3));
  if (!*output) ERREXIT1(&codec, JERR_OUT_OF_MEMORY, 10);
  while (codec.output_scanline < codec.output_height) {
    JSAMPROW row = *output + static_cast<size_t>(codec.output_scanline) * *width * 3;
    jpeg_read_scanlines(&codec, &row, 1);
  }
  jpeg_finish_decompress(&codec);
  jpeg_destroy_decompress(&codec);
  return true;
}
}  // namespace
PanoramaRgbImage ReadPanoramaJpeg(const std::string& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() <= 0 || input.tellg() > 32 * 1024 * 1024)
    throw std::runtime_error("input JPEG must be readable and at most 32 MiB: " + path);
  std::vector<uint8_t> bytes(static_cast<size_t>(input.tellg()));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("cannot read input JPEG: " + path);
  PanoramaRgbImage image;
  uint8_t* raw = nullptr;
  char message[JMSG_LENGTH_MAX]{};
  if (!Decode(bytes.data(), bytes.size(), &raw, &image.width, &image.height, message))
    throw std::runtime_error(std::string("decode JPEG: ") + message);
  std::unique_ptr<uint8_t, decltype(&std::free)> owner(raw, &std::free);
  image.pixels.assign(raw, raw + static_cast<size_t>(image.width) * image.height * 3);
  return image;
}
std::shared_ptr<const std::vector<uint8_t>> EncodePanoramaJpeg(
    const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height) {
  if (width < 1 || height < 1 || width > 4096 || height > 2048 ||
      rgb.size() != static_cast<size_t>(width) * height * 3)
    throw std::runtime_error("invalid panorama RGB dimensions");
  auto jpeg = std::make_shared<std::vector<uint8_t>>(rgb.size() + 65536);
  size_t size = jpeg->size();
  char message[JMSG_LENGTH_MAX]{};
  if (!Encode(rgb.data(), width, height, jpeg->data(), &size, message))
    throw std::runtime_error(std::string("encode JPEG: ") + message);
  jpeg->resize(size);
  return jpeg;
}
}  // namespace robobaton_demo
