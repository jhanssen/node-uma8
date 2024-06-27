{
  "targets": [
    {
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
        "<!@(pkg-config libusb-1.0 --cflags-only-I | sed s/-I//g)"
      ],
      "libraries": [
        "<!@(pkg-config libusb-1.0 --libs)"
      ],
      "target_name": "uma8",
      "sources": ["src/uma8.cpp"],
      "cflags_cc": ["-std=c++17"],
      "xcode_settings": {
        "OTHER_CPLUSPLUSFLAGS": ["-std=c++17"]
      }
    }
  ]
}
