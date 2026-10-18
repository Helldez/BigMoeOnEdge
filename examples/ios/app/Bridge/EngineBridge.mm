#import "EngineBridge.h"

#import <Foundation/Foundation.h>

#include "http_fetch.h"
#include "server.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// The server's network for reading a Hugging Face repository (http_fetch.h), on the system's own
// HTTP stack: the desktop server runs curl, which an app here may not start. Called from the
// server's worker threads, which block until the answer is in, as they do on curl.
class URLSessionFetch final : public bmoe::server::HttpFetch {
public:
    URLSessionFetch() {
        NSURLSessionConfiguration * c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        c.timeoutIntervalForRequest = kTimeoutSeconds;
        c.timeoutIntervalForResource = kTimeoutSeconds;
        session_ = [NSURLSession sessionWithConfiguration:c];
    }

    bool get(const std::string & url,
             uint64_t from,
             uint64_t to,
             const std::string & token,
             std::string & body,
             std::string & error) override {
        @autoreleasepool {
            NSURL * u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (!u) {
                error = "not a URL";
                return false;
            }
            NSMutableURLRequest * r = [NSMutableURLRequest requestWithURL:u];
            if (to > 0) [r setValue:[NSString stringWithFormat:@"bytes=%llu-%llu", from, to] forHTTPHeaderField:@"Range"];
            if (!token.empty())
                [r setValue:[@"Bearer " stringByAppendingString:[NSString stringWithUTF8String:token.c_str()]]
                    forHTTPHeaderField:@"Authorization"];
            __block NSData * data = nil;
            __block NSInteger status = 0;
            __block NSString * failure = nil;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            NSURLSessionDataTask * task =
                [session_ dataTaskWithRequest:r
                            completionHandler:^(NSData * d, NSURLResponse * response, NSError * e) {
                                data = d;
                                status = [response isKindOfClass:NSHTTPURLResponse.class]
                                             ? ((NSHTTPURLResponse *) response).statusCode
                                             : 0;
                                failure = e.localizedDescription;
                                dispatch_semaphore_signal(done);
                            }];
            [task resume];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            if (failure) {
                error = failure.UTF8String;
                return false;
            }
            // What curl --fail reports as an error is one here too: a status is not a body.
            if (status >= 400) {
                error = "the server answered HTTP " + std::to_string((long) status);
                return false;
            }
            body.assign((const char *) data.bytes, data.length);
            return true;
        }
    }

private:
    static constexpr NSTimeInterval kTimeoutSeconds = 180; // what the curl adapter allows a request
    NSURLSession * session_;
};

std::mutex g_m;
std::function<void()> g_stop; // set while a server listens, empty otherwise

// The server's own thread: the accept loop and whatever a handler calls before it hands work to the
// engine's threads. A secondary thread's default stack is a fraction of the main thread's.
constexpr NSUInteger kServerStackBytes = 8u << 20;

} // namespace

void bmoe_server_start(int argc, const char * const * argv, void (^exited)(int code)) {
    // Owned here: the caller's strings are gone by the time the thread reads them.
    auto args = std::make_shared<std::vector<std::string>>(argv, argv + argc);
    NSThread * t = [[NSThread alloc] initWithBlock:^{
        std::vector<char *> raw;
        for (std::string & a : *args)
            raw.push_back(a.data());
        raw.push_back(nullptr);
        bmoe::server::Embedding embedding;
        embedding.fetch = [](const std::string &) { return std::make_unique<URLSessionFetch>(); };
        embedding.listening = [](std::function<void()> stop) {
            std::lock_guard<std::mutex> lock(g_m);
            g_stop = std::move(stop);
        };
        const int rc = bmoe::server::run((int) args->size(), raw.data(), embedding);
        exited(rc);
    }];
    t.name = @"bmoe-server";
    t.stackSize = kServerStackBytes;
    t.qualityOfService = NSQualityOfServiceUserInitiated;
    [t start];
}

bool bmoe_server_stop(void) {
    // Under the lock: the server withdraws the stop (an empty one) before what it reaches is gone,
    // and that waits here until a stop in progress has returned.
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_stop) return false;
    g_stop();
    return true;
}
