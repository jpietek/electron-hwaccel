{
  "targets": [
    {
      "target_name": "fdpass",
      "sources": [
        "addon.cc"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "cflags": ["-O2"],
      "cflags_cc": ["-O2"]
    }
  ]
}


