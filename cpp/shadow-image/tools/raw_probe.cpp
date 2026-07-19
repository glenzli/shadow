#include <libraw/libraw.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

class Stopwatch final {
public:
    Stopwatch() : started_at_(Clock::now()) {}

    [[nodiscard]] double elapsed_ms() const {
        const auto elapsed = Clock::now() - started_at_;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

private:
    Clock::time_point started_at_;
};

void require_libraw_success(const int result, const std::string_view operation) {
    if (result == LIBRAW_SUCCESS) {
        return;
    }

    std::ostringstream message;
    message << operation << " failed: " << libraw_strerror(result) << " (" << result << ')';
    throw std::runtime_error(message.str());
}

[[nodiscard]] std::string dng_version_string(const unsigned version) {
    std::ostringstream output;
    output << ((version >> 24U) & 0xffU) << '.' << ((version >> 16U) & 0xffU) << '.'
           << ((version >> 8U) & 0xffU) << '.' << (version & 0xffU);
    return output.str();
}

[[nodiscard]] std::string cfa_pattern(LibRaw& decoder) {
    if (decoder.imgdata.idata.filters == 0U) {
        return "none/linear";
    }

    std::string result;
    result.reserve(4);
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const int color_index = decoder.COLOR(row, column);
            const bool valid_index = color_index >= 0 && color_index < 4;
            result.push_back(valid_index ? decoder.imgdata.idata.cdesc[color_index] : '?');
        }
    }
    return result;
}

void write_binary(const fs::path& path, const unsigned char* data, const std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

void write_pnm(const fs::path& path, const libraw_processed_image_t& image) {
    if (image.type != LIBRAW_IMAGE_BITMAP || (image.colors != 1U && image.colors != 3U)) {
        throw std::runtime_error("LibRaw returned an unsupported bitmap layout");
    }
    if (image.bits != 8U && image.bits != 16U) {
        throw std::runtime_error("LibRaw returned an unsupported bitmap bit depth");
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }

    const int magic = image.colors == 1U ? 5 : 6;
    const unsigned maximum = image.bits == 8U ? 255U : 65'535U;
    output << 'P' << magic << '\n' << image.width << ' ' << image.height << '\n' << maximum << '\n';

    if (image.bits == 8U) {
        output.write(
            reinterpret_cast<const char*>(image.data),
            static_cast<std::streamsize>(image.data_size)
        );
    } else {
        const std::size_t sample_count = static_cast<std::size_t>(image.data_size) / sizeof(std::uint16_t);
        for (std::size_t index = 0; index < sample_count; ++index) {
            std::uint16_t value = 0;
            std::memcpy(&value, image.data + (index * sizeof(value)), sizeof(value));
            const char bytes[2] = {
                static_cast<char>((value >> 8U) & 0xffU),
                static_cast<char>(value & 0xffU),
            };
            output.write(bytes, 2);
        }
    }

    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

void write_raw_mosaic(
    const fs::path& path,
    const std::uint16_t* pixels,
    const unsigned width,
    const unsigned height
) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }

    output << "P5\n" << width << ' ' << height << "\n65535\n";
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const std::uint16_t value = pixels[index];
        const char bytes[2] = {
            static_cast<char>((value >> 8U) & 0xffU),
            static_cast<char>(value & 0xffU),
        };
        output.write(bytes, 2);
    }

    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

void print_metadata(LibRaw& decoder) {
    const auto& identity = decoder.imgdata.idata;
    const auto& sizes = decoder.imgdata.sizes;
    const auto& color = decoder.imgdata.color;
    const auto& dng = color.dng_levels;
    const unsigned capabilities = LibRaw::capabilities();

    std::cout << "libraw.version=" << LibRaw::version() << '\n'
              << "libraw.capability.dng_sdk="
              << ((capabilities & LIBRAW_CAPS_DNGSDK) != 0U ? "yes" : "no") << '\n'
              << "libraw.capability.rawspeed="
              << ((capabilities & (LIBRAW_CAPS_RAWSPEED | LIBRAW_CAPS_RAWSPEED3)) != 0U ? "yes"
                                                                                       : "no")
              << '\n'
              << "libraw.capability.jpeg="
              << ((capabilities & LIBRAW_CAPS_JPEG) != 0U ? "yes" : "no") << '\n'
              << "camera.make=" << identity.make << '\n'
              << "camera.model=" << identity.model << '\n'
              << "camera.normalized_make=" << identity.normalized_make << '\n'
              << "camera.normalized_model=" << identity.normalized_model << '\n'
              << "raw.count=" << identity.raw_count << '\n'
              << "raw.dng_version=" << dng_version_string(identity.dng_version) << '\n'
              << "raw.dimensions=" << sizes.raw_width << 'x' << sizes.raw_height << '\n'
              << "image.dimensions=" << sizes.width << 'x' << sizes.height << '\n'
              << "image.margins=" << sizes.left_margin << ',' << sizes.top_margin << '\n'
              << "image.flip=" << sizes.flip << '\n'
              << "sensor.colors=" << identity.colors << '\n'
              << "sensor.cfa=" << cfa_pattern(decoder) << '\n'
              << "sensor.bits=" << color.raw_bps << '\n'
              << "sensor.black=" << color.black << '\n'
              << "sensor.maximum=" << color.maximum << '\n'
              << "dng.default_crop=" << dng.default_crop[0] << ',' << dng.default_crop[1]
              << ',' << dng.default_crop[2] << ',' << dng.default_crop[3] << '\n'
              << "dng.as_shot_neutral=" << dng.asshotneutral[0] << ',' << dng.asshotneutral[1]
              << ',' << dng.asshotneutral[2] << ',' << dng.asshotneutral[3] << '\n'
              << "dng.baseline_exposure=" << dng.baseline_exposure << '\n'
              << "dng.opcode_list_bytes=" << dng.rawopcodes[0].len << ','
              << dng.rawopcodes[1].len << ',' << dng.rawopcodes[2].len << '\n'
              << "thumbnail.candidates=" << decoder.imgdata.thumbs_list.thumbcount << '\n';

    for (int index = 0; index < decoder.imgdata.thumbs_list.thumbcount; ++index) {
        const auto& candidate = decoder.imgdata.thumbs_list.thumblist[index];
        const unsigned bits = candidate.tmisc & 31U;
        const unsigned colors = candidate.tmisc >> 5U;
        std::cout << "thumbnail.candidate." << index << '=' << static_cast<int>(candidate.tformat)
                  << ',' << candidate.twidth << 'x' << candidate.theight << ',' << bits << "bit,"
                  << colors << "color," << candidate.tlength << "bytes\n";
    }
}

void extract_thumbnail(LibRaw& decoder, const fs::path& output_directory) {
    const Stopwatch timer;
    const int unpack_result = decoder.unpack_thumb();
    if (unpack_result != LIBRAW_SUCCESS) {
        std::cout << "thumbnail.status=unavailable\n"
                  << "thumbnail.error=" << libraw_strerror(unpack_result) << '\n'
                  << "timing.thumbnail_ms=" << timer.elapsed_ms() << '\n';
        return;
    }

    int memory_result = LIBRAW_SUCCESS;
    libraw_processed_image_t* thumbnail = decoder.dcraw_make_mem_thumb(&memory_result);
    if (thumbnail == nullptr) {
        std::cout << "thumbnail.status=unavailable\n"
                  << "thumbnail.error=" << libraw_strerror(memory_result) << '\n'
                  << "timing.thumbnail_ms=" << timer.elapsed_ms() << '\n';
        return;
    }

    try {
        fs::path output_path;
        if (thumbnail->type == LIBRAW_IMAGE_JPEG) {
            output_path = output_directory / "embedded-preview.jpg";
            write_binary(output_path, thumbnail->data, thumbnail->data_size);
        } else if (thumbnail->type == LIBRAW_IMAGE_BITMAP) {
            const char* extension = thumbnail->colors == 1U ? ".pgm" : ".ppm";
            output_path = output_directory / (std::string("embedded-preview") + extension);
            write_pnm(output_path, *thumbnail);
        } else {
            output_path = output_directory / "embedded-preview.bin";
            write_binary(output_path, thumbnail->data, thumbnail->data_size);
        }

        std::cout << "thumbnail.status=ok\n"
                  << "thumbnail.dimensions=" << thumbnail->width << 'x' << thumbnail->height << '\n'
                  << "thumbnail.bits=" << thumbnail->bits << '\n'
                  << "thumbnail.colors=" << thumbnail->colors << '\n'
                  << "thumbnail.bytes=" << thumbnail->data_size << '\n'
                  << "thumbnail.output=" << output_path.string() << '\n'
                  << "timing.thumbnail_ms=" << timer.elapsed_ms() << '\n';
    } catch (...) {
        LibRaw::dcraw_clear_mem(thumbnail);
        throw;
    }

    LibRaw::dcraw_clear_mem(thumbnail);
}

void inspect_mosaic(LibRaw& decoder, const fs::path& output_directory) {
    const auto* pixels = decoder.imgdata.rawdata.raw_image;
    const auto& sizes = decoder.imgdata.sizes;
    if (pixels == nullptr) {
        throw std::runtime_error("this probe currently expects a single-plane integer mosaic");
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(sizes.raw_width) * sizes.raw_height;
    std::uint16_t minimum = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t maximum = 0;
    long double total = 0.0L;
    std::uint64_t checksum = 1'469'598'103'934'665'603ULL;

    for (std::size_t index = 0; index < pixel_count; ++index) {
        const std::uint16_t value = pixels[index];
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        total += value;
        checksum ^= value;
        checksum *= 1'099'511'628'211ULL;
    }

    const fs::path output_path = output_directory / "raw-mosaic.pgm";
    write_raw_mosaic(output_path, pixels, sizes.raw_width, sizes.raw_height);

    std::cout << "mosaic.status=ok\n"
              << "mosaic.samples=" << pixel_count << '\n'
              << "mosaic.minimum=" << minimum << '\n'
              << "mosaic.maximum=" << maximum << '\n'
              << "mosaic.mean="
              << static_cast<double>(total / static_cast<long double>(pixel_count)) << '\n'
              << "mosaic.fnv1a64=" << std::hex << std::setw(16) << std::setfill('0') << checksum
              << std::dec << std::setfill(' ') << '\n'
              << "mosaic.output=" << output_path.string() << '\n';
}

void render_reference_rgb(LibRaw& decoder, const fs::path& output_directory) {
    auto& parameters = decoder.imgdata.params;
    parameters.output_bps = 16;
    parameters.use_camera_wb = 1;
    parameters.no_auto_bright = 1;
    parameters.output_color = 1;
    parameters.user_qual = 3;

    const Stopwatch timer;
    require_libraw_success(decoder.dcraw_process(), "dcraw_process");

    int memory_result = LIBRAW_SUCCESS;
    libraw_processed_image_t* image = decoder.dcraw_make_mem_image(&memory_result);
    if (image == nullptr) {
        require_libraw_success(memory_result, "dcraw_make_mem_image");
        throw std::runtime_error("dcraw_make_mem_image returned no image");
    }

    const fs::path output_path = output_directory / "reference-srgb-16bit.ppm";
    try {
        write_pnm(output_path, *image);
        std::cout << "reference_rgb.status=ok\n"
                  << "reference_rgb.dimensions=" << image->width << 'x' << image->height << '\n'
                  << "reference_rgb.bits=" << image->bits << '\n'
                  << "reference_rgb.colors=" << image->colors << '\n'
                  << "reference_rgb.bytes=" << image->data_size << '\n'
                  << "reference_rgb.output=" << output_path.string() << '\n'
                  << "timing.reference_rgb_ms=" << timer.elapsed_ms() << '\n';
    } catch (...) {
        LibRaw::dcraw_clear_mem(image);
        throw;
    }

    LibRaw::dcraw_clear_mem(image);
}

int run(const fs::path& input_path, const fs::path& output_directory) {
    fs::create_directories(output_directory);

    LibRaw decoder;
    decoder.imgdata.rawparams.max_raw_memory_mb = 2'048U;

    const Stopwatch open_timer;
    require_libraw_success(decoder.open_file(input_path.string().c_str()), "open_file");
    std::cout << std::fixed << std::setprecision(3)
              << "input=" << input_path.string() << '\n'
              << "output_directory=" << output_directory.string() << '\n'
              << "timing.open_ms=" << open_timer.elapsed_ms() << '\n';
    print_metadata(decoder);
    extract_thumbnail(decoder, output_directory);

    const Stopwatch unpack_timer;
    require_libraw_success(decoder.unpack(), "unpack");
    std::cout << "timing.unpack_ms=" << unpack_timer.elapsed_ms() << '\n';
    inspect_mosaic(decoder, output_directory);
    render_reference_rgb(decoder, output_directory);

    return 0;
}

} // namespace

int main(const int argument_count, char** arguments) {
    if (argument_count != 3) {
        std::cerr << "usage: shadow-raw-probe <input-raw> <output-directory>\n";
        return 2;
    }

    try {
        return run(arguments[1], arguments[2]);
    } catch (const std::exception& error) {
        std::cerr << "shadow-raw-probe: " << error.what() << '\n';
        return 1;
    }
}
