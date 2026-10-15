// bmoe-server: the engine behind the local web UI, and the application framework every UI of this
// project speaks to (docs/application-framework.md). Everything is in the library; this is the
// process around it.
#include "server.h"

int main(int argc, char ** argv) {
    return bmoe::server::run(argc, argv);
}
