# JpegXL encoding and decoding for Flutter

## Usage

``` Dart
    Uint8List jpegData = await file.readAsBytes();
    final Uint8List? jxlData = await JxlCoder.jpegToJxl(jpegData);

    Uint8List jxlData = await file.readAsBytes();
    final Uint8List? jpegData = await JxlCoder.jxlToJpeg(jxlData);

    await JxlCoder.saveJpegAsJxl(inputFile.path, outputFile.path);

    await JxlCoder.saveJxlAsJpeg(inputFile.path, outputFile.path);
```