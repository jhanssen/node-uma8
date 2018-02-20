# node-uma8
Node module for reading data from a miniDSP UMA-8 array microphone

## Installation
```npm install --save uma8```

## Dependencies
This module needs libusb-1.0 installed.

#### Debian-like Linux
```sudo apt install libusb-1.0-0-dev```

#### OS X
```brew install libusb```

## Example
```javascript
const Uma8 = require("uma8");

const uma8 = new Uma8();
uma8.on("audio", function(buffer) {
  // buffer is a node Buffer
});
uma8.on("metadata", function(meta) {
  // meta is an object with metadata, including vad and direction
});
const devices = uma8.enumerate();
// pick one, devices is an array
uma8.open(devices[0]);
```
