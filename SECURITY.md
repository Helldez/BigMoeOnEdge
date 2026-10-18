# Security policy

## Reporting a vulnerability

Please report security problems privately, through GitHub's
[private vulnerability reporting](https://github.com/Helldez/BigMoeOnEdge/security/advisories/new),
not in a public issue. Say what you found, how to reproduce it, and which build or release it
affects. You will get an answer within a week; a confirmed issue is fixed in a release and credited
to you in the changelog unless you ask otherwise.

## What is in scope

- `bmoe-server` and its HTTP API: authentication, the origin and host checks, request parsing,
  anything that lets a client read or write what it should not ([docs/server-api.md](docs/server-api.md)
  states what the server protects and what it does not).
- The Android app, as a client of the server it starts.
- Reading a model file: a GGUF that makes the engine read or write out of bounds.
- The release pipeline: anything that would let a release asset be built from something other
  than the tagged source.

What `docs/server-api.md` lists under "What is not" protected is documented behaviour, not a
vulnerability; a way around one of the protections it lists is.

## Supported versions

Only the latest release gets fixes.
