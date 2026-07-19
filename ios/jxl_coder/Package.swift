// swift-tools-version: 5.9

import PackageDescription

let package = Package(
  name: "jxl_coder",
  platforms: [
    .iOS("13.0"),
  ],
  products: [
    .library(name: "jxl-coder", targets: ["jxl_coder"]),
  ],
  dependencies: [
    .package(name: "FlutterFramework", path: "../FlutterFramework"),
  ],
  targets: [
    .binaryTarget(
      name: "JxlCoderNative",
      path: "Frameworks/JxlCoderNative.xcframework"
    ),
    .target(
      name: "JxlCoderCore",
      dependencies: [
        "JxlCoderNative",
      ],
      publicHeadersPath: "include"
    ),
    .target(
      name: "jxl_coder",
      dependencies: [
        .product(name: "FlutterFramework", package: "FlutterFramework"),
        "JxlCoderCore",
      ]
    ),
  ],
  cxxLanguageStandard: .cxx17
)
