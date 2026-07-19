#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint jxl_coder.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'jxl_coder'
  s.version          = '1.0.0'
  s.summary          = 'JPEG-to-JPEG XL compression with byte-exact JPEG and metadata recovery on iOS.'
  s.description      = <<-DESC
A Flutter plugin for JPEG-to-JPEG XL compression, then converting JPEG XL back to the exact original JPEG bytes, including metadata, with statically linked libjxl.
                       DESC
  s.homepage         = 'https://github.com/stephane-archer/flutter-jxl-coder'
  s.license          = { :type => 'BSD-3-Clause', :file => '../LICENSE' }
  s.author           = { 'Stephane Archer' => 'archerstephane@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files = 'jxl_coder/Sources/**/*.{swift,h,mm}'
  s.public_header_files = 'jxl_coder/Sources/JxlCoderCore/include/**/*.h'
  s.vendored_frameworks = 'jxl_coder/Frameworks/JxlCoderNative.xcframework'
  s.dependency 'Flutter'

  s.platform = :ios, '13.0'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
  }
  s.swift_version = '5.0'
end
