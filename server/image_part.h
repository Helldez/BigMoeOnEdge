// An image as a chat protocol carries it, turned into the engine's ChatImage: the bytes of the
// encoded file, decoded from base64 here and read by the projector later. Every protocol the
// server speaks sends images inline (a data URL, or a media type and a base64 body); a URL to fetch
// is refused, since a server on a network must not be made to fetch what a client names.
#pragma once

#include "bmoe/chat.h"

#include <string>

namespace bmoe::server {

// Images on one message: a few screenshots, not a folder of them. Named once here and enforced on
// every API that takes a message (the three chat protocols and the agent), with the same reason.
inline constexpr size_t k_max_images_per_message = 8;

// The two rules every protocol applies to a message's images: only a user message carries them,
// and at most k_max_images_per_message. False with `err` (the 400 reason) when `msg` breaks one.
bool images_allowed(const ChatMessage & msg, std::string & err);

// "data:<mime>;base64,<payload>" into `out`. False with `err` for an http(s) URL or anything else.
bool image_from_data_url(const std::string & url, ChatImage & out, std::string & err);

// A media type and a base64 body (the Anthropic form) into `out`. False with `err` when the body is
// not base64 or is empty.
bool image_from_base64(const std::string & mime, const std::string & data, ChatImage & out, std::string & err);

// Append `text` to a message's content, then one image marker per image in `images` (the engine's
// image_marker()): the form every protocol's parts reduce to, text first, images after it.
void place_images(std::string & content, size_t n_images);

} // namespace bmoe::server
