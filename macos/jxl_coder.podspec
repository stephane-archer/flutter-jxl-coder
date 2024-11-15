#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint jxl_coder.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'jxl_coder'
  s.version          = '0.0.1'
  s.summary          = 'A Flutter plugin for the JxlCoder library on macOS.'
  s.description      = <<-DESC
    A plugin that provides JPEG XL (JXL) encoding and decoding using JxlCoder.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Stephane Archer' => 'archerstephane@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  s.dependency 'FlutterMacOS'
  s.dependency 'JxlCoder'

  s.platform = :osx, '12.0'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
