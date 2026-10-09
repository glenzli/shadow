#include "libraw_runtime.hpp"

#include <shadow/image/decoder_error.hpp>

#include <sstream>
#include <string>

#if __has_include(<shadow_dng_host.h>)
#include <shadow_dng_host.h>
#define SHADOW_LIBRAW_HAS_DNG_HOST 1
#else
#define SHADOW_LIBRAW_HAS_DNG_HOST 0
#endif

namespace shadow::image {

namespace {

[[nodiscard]] DecodeErrorCode map_libraw_error(const int result) noexcept {
    switch (result) {
    case LIBRAW_FILE_UNSUPPORTED:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE:
        return DecodeErrorCode::unsupported;
    case LIBRAW_IO_ERROR:
    case LIBRAW_INPUT_CLOSED:
        return DecodeErrorCode::io;
    case LIBRAW_DATA_ERROR:
        return DecodeErrorCode::corrupt_data;
    case LIBRAW_NO_THUMBNAIL:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL:
        return DecodeErrorCode::no_preview;
    case LIBRAW_UNSUPPORTED_THUMBNAIL:
    case LIBRAW_NOT_IMPLEMENTED:
        return DecodeErrorCode::unsupported_layout;
    case LIBRAW_TOO_BIG:
    case LIBRAW_UNSUFFICIENT_MEMORY:
    case LIBRAW_MEMPOOL_OVERFLOW:
        return DecodeErrorCode::resource_limit;
    case LIBRAW_CANCELLED_BY_CALLBACK:
        return DecodeErrorCode::cancelled;
    case LIBRAW_OUT_OF_ORDER_CALL:
    case LIBRAW_BAD_CROP:
        return DecodeErrorCode::invalid_request;
    default:
        return DecodeErrorCode::internal;
    }
}

} // namespace

LibRawDngHost::LibRawDngHost() {
#if SHADOW_LIBRAW_HAS_DNG_HOST
    if ((LibRaw::capabilities() & LIBRAW_CAPS_DNGSDK) != 0U) {
        host_ = shadow_dng_host_create();
    }
#endif
}

LibRawDngHost::~LibRawDngHost() {
#if SHADOW_LIBRAW_HAS_DNG_HOST
    shadow_dng_host_destroy(host_);
#endif
}

void LibRawDngHost::attach(LibRaw& decoder) const noexcept {
    libraw_decoder_info_t info{};
    if (host_ != nullptr && decoder.get_decoder_info(&info) == LIBRAW_SUCCESS
        && info.decoder_name != nullptr
        && std::string_view(info.decoder_name) == "jxl_dng_load_raw_placeholder()") {
        decoder.set_dng_host(host_);
        // JPEG XL is admitted by LibRaw's explicit codec rule regardless of this
        // mask. Other DNGs keep their existing public LibRaw execution semantics.
        decoder.imgdata.rawparams.use_dngsdk = LIBRAW_DNG_NONE;
        // Lossy JPEG/JPEG XL DNG may encode a non-linear or colour-scaled
        // raster. Its declared Stage 2/3 opcodes restore the camera-linear
        // source before LibRaw applies white balance and colour development.
        decoder.imgdata.rawparams.options |= LIBRAW_RAWOPTIONS_DNG_STAGE23_IFPRESENT_JPGJXL;
    }
}

[[noreturn]] void throw_libraw_error(
    const int result,
    const std::string_view operation
) {
    std::ostringstream message;
    message << operation << " failed: " << libraw_strerror(result)
            << " (" << result << ')';
    throw DecodeError(map_libraw_error(result), result, message.str());
}

void require_libraw_success(
    const int result,
    const std::string_view operation
) {
    if (result != LIBRAW_SUCCESS)
        throw_libraw_error(result, operation);
}

int libraw_open_path(
    LibRaw& decoder,
    const std::filesystem::path& path
) {
#if defined(_WIN32)
    return decoder.open_file(path.c_str());
#else
    const std::string native_path = path.native();
    return decoder.open_file(native_path.c_str());
#endif
}

PendingCorrections libraw_pending_corrections(
    const libraw_data_t& data
) noexcept {
    const auto& opcodes = data.color.dng_levels.rawopcodes;
    return PendingCorrections{{
        opcodes[0].len,
        opcodes[1].len,
        opcodes[2].len,
    }};
}

} // namespace shadow::image
