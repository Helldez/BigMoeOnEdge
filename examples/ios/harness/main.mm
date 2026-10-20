// The iOS harness: bmoe-cli, run inside an app.
//
// An app here may not start a process, so the way the Android example drives the engine (exec the
// CLI, speak to it over pipes) has no counterpart. This is the smallest thing that gets the same
// front-end onto the device instead: cli/main.cpp is compiled into the app with its entry point
// renamed, and called on a thread with whatever command line was typed. Every flag, the plan's
// rationale and the CSV are therefore the CLI's own, not a second implementation to keep in step.
//
// It is a measuring instrument, not the app: one text field, one button, the output. `--session`
// reads requests from a stdin an app does not have, and a command line the CLI rejects by calling
// exit() ends the app with it.
#import <UIKit/UIKit.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

// cli/main.cpp's main(), under the name the build gives it (examples/ios/CMakeLists.txt).
int bmoe_cli_main(int argc, char ** argv);

namespace {

// Everything after this in the app's own arguments is a command line to run once and exit on, so a
// run can be scripted from the machine the device is attached to.
const char * const k_run_marker = "--bmoe";

NSString * const k_saved_args = @"args";

// A command line as a shell would split it: whitespace separates, quotes group. No escapes and no
// expansion, which is all a model path with a space in it needs.
std::vector<std::string> split_args(const std::string & line) {
    std::vector<std::string> out;
    std::string cur;
    bool any = false;
    char quote = 0;
    for (const char c : line) {
        if (quote) {
            if (c == quote)
                quote = 0;
            else
                cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            any = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
            any = true;
        }
    }
    if (any) out.push_back(cur);
    return out;
}

int run_cli(const std::vector<std::string> & args) {
    std::vector<std::string> owned;
    owned.push_back("bmoe-cli");
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (std::string & a : owned)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    const int rc = bmoe_cli_main((int) owned.size(), argv.data());
    std::fflush(stdout);
    std::fflush(stderr);
    return rc;
}

// The CLI's main thread is the process's, with the stack that comes with it; a secondary thread's
// default is a sixteenth of that.
constexpr NSUInteger k_cli_stack_bytes = 8u << 20;

void run_cli_on_thread(std::vector<std::string> args, void (^done)(int rc)) {
    NSThread * t = [[NSThread alloc] initWithBlock:^{
        const int rc = run_cli(args);
        dispatch_async(dispatch_get_main_queue(), ^{
            done(rc);
        });
    }];
    t.stackSize = k_cli_stack_bytes;
    [t start];
}

// Send stdout and stderr to `sink` as text, and on to wherever they went before: started from a
// console, the run is still readable there.
void capture_output(void (^sink)(NSString * text)) {
    int fds[2];
    if (pipe(fds) != 0) return;
    const int console = dup(STDOUT_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    // A pipe makes stdout fully buffered, and a token would then appear a screenful late.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    const int rd = fds[0];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSMutableData * pending = [NSMutableData data];
        char buf[4096];
        ssize_t n;
        while ((n = read(rd, buf, sizeof(buf))) > 0) {
            if (console >= 0) (void) write(console, buf, (size_t) n);
            [pending appendBytes:buf length:(NSUInteger) n];
            // A read can end inside a character. Hold back the shortest tail that makes the rest
            // decode, and give up on bytes that are not text at all rather than stall on them.
            NSString * text = nil;
            NSUInteger keep = 0;
            for (; keep < 4 && keep <= pending.length; ++keep) {
                text = [[NSString alloc] initWithBytes:pending.bytes
                                                length:pending.length - keep
                                              encoding:NSUTF8StringEncoding];
                if (text) break;
            }
            if (!text) {
                text = [[NSString alloc] initWithData:pending encoding:NSISOLatin1StringEncoding];
                keep = 0;
            }
            NSData * tail = [pending subdataWithRange:NSMakeRange(pending.length - keep, keep)];
            [pending setData:tail];
            if (text.length) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    sink(text);
                });
            }
        }
    });
}

NSString * documents_dir() {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

// The models the app can see: whatever was copied into its documents folder.
NSArray<NSString *> * models_present() {
    NSArray<NSString *> * all = [NSFileManager.defaultManager contentsOfDirectoryAtPath:documents_dir() error:nil];
    NSArray<NSString *> * gguf = [all filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"pathExtension ==[c] 'gguf'"]];
    return [gguf sortedArrayUsingSelector:@selector(compare:)];
}

// What followed the marker when the app was started, or empty.
std::vector<std::string> scripted_args() {
    std::vector<std::string> out;
    bool after = false;
    for (NSString * a in NSProcessInfo.processInfo.arguments) {
        if (after)
            out.push_back(a.UTF8String);
        else if ([a isEqualToString:@(k_run_marker)])
            after = true;
    }
    return out;
}

} // namespace

@interface HarnessViewController : UIViewController <UITextFieldDelegate>
@end

@implementation HarnessViewController {
    UITextField * _args;
    UIButton * _run;
    UITextView * _log;
    BOOL _running;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    _args = [[UITextField alloc] init];
    _args.borderStyle = UITextBorderStyleRoundedRect;
    _args.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    _args.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _args.autocorrectionType = UITextAutocorrectionTypeNo;
    _args.smartDashesType = UITextSmartDashesTypeNo; // "--auto" must stay two hyphens
    _args.smartQuotesType = UITextSmartQuotesTypeNo;
    _args.spellCheckingType = UITextSpellCheckingTypeNo;
    _args.returnKeyType = UIReturnKeyGo;
    _args.clearButtonMode = UITextFieldViewModeWhileEditing;
    _args.placeholder = @"bmoe-cli arguments";
    _args.delegate = self;

    _run = [UIButton buttonWithType:UIButtonTypeSystem];
    [_run setTitle:@"Run" forState:UIControlStateNormal];
    [_run addTarget:self action:@selector(runTapped) forControlEvents:UIControlEventTouchUpInside];
    [_run setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

    _log = [[UITextView alloc] init];
    _log.editable = NO;
    _log.alwaysBounceVertical = YES;
    _log.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];

    for (UIView * v in @[ _args, _run, _log ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:v];
    }
    UILayoutGuide * safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_args.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [_args.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:8],
        [_run.leadingAnchor constraintEqualToAnchor:_args.trailingAnchor constant:8],
        [_run.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
        [_run.centerYAnchor constraintEqualToAnchor:_args.centerYAnchor],
        [_log.topAnchor constraintEqualToAnchor:_args.bottomAnchor constant:8],
        [_log.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:4],
        [_log.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-4],
        [_log.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor],
    ]];

    __weak HarnessViewController * weak = self;
    capture_output(^(NSString * text) {
        [weak append:text];
    });

    // Relative paths on a command line mean the documents folder: it is where a model is copied to
    // and where a CSV has to land to be copied back.
    chdir(documents_dir().fileSystemRepresentation);
    NSArray<NSString *> * models = models_present();
    std::printf("documents: %s\n", documents_dir().UTF8String);
    if (models.count == 0) std::printf("no .gguf there yet; copy a model into this app's documents folder\n");
    for (NSString * m in models)
        std::printf("model: %s\n", m.UTF8String);

    NSString * saved = [NSUserDefaults.standardUserDefaults stringForKey:k_saved_args];
    if (saved.length)
        _args.text = saved;
    else if (models.count)
        _args.text = [NSString stringWithFormat:@"--auto --plan -m \"%@\"", models.firstObject];

    const std::vector<std::string> scripted = scripted_args();
    if (!scripted.empty()) {
        // Leave the reader a moment to pass the last lines on before the process goes.
        [self start:scripted
               then:^(int rc) {
                   dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
                       exit(rc);
                   });
               }];
    }
}

- (void)append:(NSString *)text {
    NSDictionary * attrs = @{NSFontAttributeName : _log.font, NSForegroundColorAttributeName : UIColor.labelColor};
    [_log.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attrs]];
    [_log scrollRangeToVisible:NSMakeRange(_log.textStorage.length, 0)];
}

- (BOOL)textFieldShouldReturn:(UITextField *)field {
    [self runTapped];
    return YES;
}

- (void)runTapped {
    [_args resignFirstResponder];
    NSString * line = _args.text ?: @"";
    [NSUserDefaults.standardUserDefaults setObject:line forKey:k_saved_args];
    [self start:split_args(line.UTF8String) then:nil];
}

- (void)start:(std::vector<std::string>)args then:(void (^)(int rc))then {
    if (_running) return;
    _running = YES;
    _run.enabled = NO;
    // A prefill of a long prompt outlasts the screen timeout, and a locked device suspends the app.
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    std::string shown = "\n$ bmoe-cli";
    for (const std::string & a : args)
        shown += " " + a;
    std::printf("%s\n", shown.c_str());
    __weak HarnessViewController * weak = self;
    run_cli_on_thread(args, ^(int rc) {
        std::printf("[exit %d]\n", rc);
        std::fflush(stdout);
        [weak finished];
        if (then) then(rc);
    });
}

- (void)finished {
    _running = NO;
    _run.enabled = YES;
    UIApplication.sharedApplication.idleTimerDisabled = NO;
}

@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow * window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *) scene];
    self.window.rootViewController = [[HarnessViewController alloc] init];
    [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
@end

int main(int argc, char ** argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
