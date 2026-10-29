# Copy the model and client catalogs, the bundled agent skills and, when it has been built, the web
# UI beside bmoe-server.
# Invoked as: cmake -DSRC=<source dir> -DDEST=<binary dir> -P stage_assets.cmake

file(MAKE_DIRECTORY "${DEST}/catalog")
file(COPY "${SRC}/catalog/models.json" DESTINATION "${DEST}/catalog")
file(COPY "${SRC}/catalog/clients.json" DESTINATION "${DEST}/catalog")

# The skills the release ships (docs/agent.md). Replaced whole, like the UI: a skill removed from the
# source must not linger beside the binary.
file(REMOVE_RECURSE "${DEST}/skills")
file(COPY "${SRC}/skills/" DESTINATION "${DEST}/skills")

if(EXISTS "${SRC}/ui/dist/index.html")
    # Replace rather than merge: a stale hashed asset from an older UI build must not linger.
    file(REMOVE_RECURSE "${DEST}/ui")
    file(COPY "${SRC}/ui/dist/" DESTINATION "${DEST}/ui")
else()
    message(STATUS "bmoe-server: ui/dist not built; the server will say so at / (cd ui && npm ci && npm run build)")
endif()
