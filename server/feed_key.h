// The public half of the key the release workflow signs feed.json with (docs/desktop-app.md, "The
// feed"; how the pair is made and where the private half is kept: "Signing the feed"). Base64 of
// the 32 raw Ed25519 key bytes.
//
// A fork that publishes its own releases signs with its own key and builds with
// -DBMOE_FEED_PUBLIC_KEY=<its base64 public key> (and --feed-url, or kFeedUrl, at its releases),
// so its builds believe its feed and not this one. A build with no key reads no feed at all: the
// backend packs and the update check then say so instead of trusting an unsigned file.
#pragma once

namespace bmoe::server {

#if defined(BMOE_FEED_PUBLIC_KEY_OVERRIDE)
inline constexpr const char * kFeedPublicKey = BMOE_FEED_PUBLIC_KEY_OVERRIDE;
#else
// Set by the maintainer when the signing key is first made, and only changed with it.
inline constexpr const char * kFeedPublicKey = "hWYiUVrOmwFy6spf89ydGA6axhPdYVECUFflk0SLr/I=";
#endif

} // namespace bmoe::server
