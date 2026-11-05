#include "image_part.h"

#include "feed.h"

namespace bmoe::server {

bool image_from_base64(const std::string & mime, const std::string & data, ChatImage & out, std::string & err) {
    std::string bytes;
    if (!base64_decode(data, bytes) || bytes.empty()) {
        err = "an image's data must be base64";
        return false;
    }
    out.bytes = std::move(bytes);
    out.mime = mime;
    return true;
}

bool image_from_data_url(const std::string & url, ChatImage & out, std::string & err) {
    static const std::string kScheme = "data:";
    if (url.compare(0, kScheme.size(), kScheme) != 0) {
        err = url.compare(0, 4, "http") == 0
                  ? "an image is sent inline as a data URL (data:<type>;base64,...): this server fetches no URLs"
                  : "an image URL must be a data URL (data:<type>;base64,...)";
        return false;
    }
    const size_t comma = url.find(',');
    if (comma == std::string::npos) {
        err = "a data URL has a comma between its type and its data";
        return false;
    }
    const std::string meta = url.substr(kScheme.size(), comma - kScheme.size());
    if (meta.find(";base64") == std::string::npos) {
        err = "an image's data URL must be base64 (data:<type>;base64,...)";
        return false;
    }
    const std::string mime = meta.substr(0, meta.find(';'));
    return image_from_base64(mime, url.substr(comma + 1), out, err);
}

void place_images(std::string & content, size_t n_images) {
    for (size_t i = 0; i < n_images; ++i) {
        if (!content.empty() && content.back() != '\n') content += '\n';
        content += image_marker();
    }
}

} // namespace bmoe::server
