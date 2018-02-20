/*global require,process*/

const Uma8 = require("..");

let uma8 = new Uma8();
let candidates = uma8.enumerate();

console.log(candidates);

if (candidates.length > 0) {
    uma8.open(candidates[0]);
    uma8.on("audio", function(buf) {
        console.log("got data", buf.length);
    });
    uma8.on("metadata", function(meta) {
        console.log("got meta", meta, uma8);
    });
    console.log("ready");
}
