/*global require,process*/

const uma8 = require("..");

let device = uma8.create();
let candidates = uma8.enumerate(device);
console.log(candidates);
if (candidates.length > 0) {
    uma8.open(device, candidates[0]);
    uma8.on(device, "audio", function(buf) {
        console.log("got data", buf.length);
    });
    uma8.on(device, "metadata", function(meta) {
        console.log("got meta", meta);
    });
    console.log("ready");
    process.on('SIGINT', function() {
        uma8.destroy(device);
    });
    // setTimeout(function() {
    //     uma8.destroy(device);
    // }, 10000);
} else {
    uma8.destroy(device);
}
