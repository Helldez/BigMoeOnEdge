// swift-tools-version:5.9
// BmoeKit: the client side of bmoe-server for the iOS app, with no UI and no engine in it. What
// the server says (the API, its event stream, the chat protocol), what the app keeps for itself
// (conversations, metrics files, downloads), and nothing that names an engine parameter. A package
// of its own so `swift test` runs it on any Mac without a simulator; the app compiles the same
// sources (examples/ios/app/CMakeLists.txt).
import PackageDescription

let package = Package(
    name: "BmoeKit",
    platforms: [.iOS("16.4"), .macOS(.v13)],
    products: [.library(name: "BmoeKit", targets: ["BmoeKit"])],
    targets: [
        .target(name: "BmoeKit"),
        .testTarget(name: "BmoeKitTests", dependencies: ["BmoeKit"]),
    ]
)
