// swift-tools-version: 5.9
import PackageDescription

let package = Package(name: "SheikahInstaller", platforms: [.macOS(.v14)], products: [
    .executable(name: "SheikahInstaller", targets: ["SheikahInstaller"])
], targets: [.executableTarget(name: "SheikahInstaller", resources: [.copy("Resources/backdrop.jpg"), .copy("Resources/logo.png"), .copy("Resources/guide-zh.html"), .copy("Resources/guide-en.html")]),
             .testTarget(name: "SheikahInstallerTests", dependencies: ["SheikahInstaller"])])
