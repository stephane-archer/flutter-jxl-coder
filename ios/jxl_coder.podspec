#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint jxl_coder.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'jxl_coder'
  s.version          = '0.1.1'
  s.summary          = 'A Flutter plugin for the JxlCoder library on iOS.'
  s.description      = <<-DESC
A plugin that provides JPEG XL (JXL) encoding and decoding using JxlCoder (libjxl).
                       DESC
  s.homepage         = 'https://github.com/stephane-archer/flutter-jxl-coder'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Stephane Archer' => 'archerstephane@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  s.dependency 'Flutter'
  s.dependency 'JxlCoder'

  s.platform = :ios, '13.0'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
