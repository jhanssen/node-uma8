/*global require,module*/

const internal = require('bindings')('uma8.node');

class Uma8
{
    constructor() {
        this._uma8 = internal.create();
    }

    enumerate() {
        return internal.enumerate(this._uma8);
    }

    open(device) {
        internal.open(this._uma8, device);
    }

    on(name, cb) {
        internal.on(this._uma8, name, cb);
    }
}

module.exports = Uma8;
