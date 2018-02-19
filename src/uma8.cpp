#include <nan.h>
#include <unordered_set>
#include <unordered_map>
#include <libusb.h>
#include "utils.h"

struct Input {
    Input();
    ~Input();

    bool open(uint8_t bus, uint8_t port);

    libusb_context* usb;
    libusb_device_handle* handle;
    uv_thread_t thread;
    uv_async_t async;
    Mutex mutex;
    bool stopped, opened;
    int pendingCancels;
    struct Data {
        uint8_t* data;
        size_t size;
    };
    std::vector<Data> datas;
    struct Metadata {
        uint8_t vad, direction;
        uint16_t angle;
    };
    std::vector<Metadata> metas;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Nan::Callback> > > ons;

    struct Iso {
        enum { NumTransfer = 10, NumPackets = 10, PacketSize = 24, EpIsoIn = 0x81 };

        uint8_t buf[PacketSize * NumPackets];
        libusb_transfer* xfr[NumTransfer];
    } iso;
    struct Irq {
        enum { EpIn = 0x82 };

        uint8_t buf[64];
        libusb_transfer* xfr;
    } irq;

    enum { Vid = 0x2752, Pid = 0x1c, AudioIfaceNum = 2, HidIfaceNum = 4 };

    static void run(void* arg);
    static void transferCallback(libusb_transfer* xfr);
    static void irqCallback(libusb_transfer* xfr);
};

struct State {
    std::unordered_set<Input*> inputs;
};

Input::Input()
    : stopped(false), opened(false), pendingCancels(-1)
{
    if (libusb_init(&usb) != 0) {
        usb = nullptr;
        return;
    }
    async.data = this;
}

Input::~Input()
{
    if (usb) {
        if (opened) {
            {
                MutexLocker locker(&mutex);
                stopped = true;
            }

            uv_thread_join(&thread);
            uv_close(reinterpret_cast<uv_handle_t*>(&async), nullptr);

            libusb_close(handle);
        }
        libusb_exit(usb);
    }
}

bool Input::open(uint8_t bus, uint8_t port)
{
    handle = nullptr;

    libusb_device** list;
    ssize_t devices = libusb_get_device_list(usb, &list);
    if (devices < 0) {
        // error
        Nan::ThrowError("No devices");
        return false;
    }
    for (ssize_t i = 0; i < devices; ++i) {
        libusb_device* dev = list[i];
        libusb_device_descriptor desc;
        int ret = libusb_get_device_descriptor(dev, &desc);
        if (ret != 0) {
            // error
            continue;
        }
        if (desc.idVendor == Input::Vid && desc.idProduct == Input::Pid) {
            const uint8_t b = libusb_get_bus_number(dev);
            const uint8_t p = libusb_get_port_number(dev);
            if (b == bus && p == port) {
                // got it
                const int ret = libusb_open(dev, &handle);
                if (ret != 0) {
                    // error
                    Nan::ThrowError("Can't open");
                    return false;
                }
            }
        }
    }
    libusb_free_device_list(list, 1);
    if (!handle) {
        Nan::ThrowError("No handle");
        return false;
    }

    int ret;
    int ifaces[] = { AudioIfaceNum, HidIfaceNum, -1 };
    for (int i = 0;; ++i) {
        const int iface = ifaces[i];
        if (iface == -1)
            break;

        ret = libusb_kernel_driver_active(handle, iface);
        if (ret == 1) {
            ret = libusb_detach_kernel_driver(handle, iface);
            if (ret < 0) {
                // bad
                libusb_close(handle);
                Nan::ThrowError("Can't detach kernel driver");
                return false;
            }
        }
        ret = libusb_claim_interface(handle, iface);
        if (ret < 0) {
            // also bad
            libusb_close(handle);
            printf("%d\n", ret);
            Nan::ThrowError("Can't claim interface");
            return false;
        }
    }

    ret = libusb_set_interface_alt_setting(handle, AudioIfaceNum, 1);
    if (ret < 0) {
        // yep, bad
        Nan::ThrowError("Can't set alt setting");
        libusb_close(handle);
        return false;
    }

    opened = true;
    uv_async_init(uv_default_loop(), &async, [](uv_async_t* async) {
            Input* input = static_cast<Input*>(async->data);
            MutexLocker locker(&input->mutex);
            if (!input->datas.empty()) {
                const std::string name = "audio";

                auto it = input->datas.cbegin();
                const auto end = input->datas.cend();
                while (it != end) {
                    const Input::Data& data = *it;

                    // make a buffer and send up to js
                    Nan::HandleScope scope;
                    v8::Local<v8::Value> value = Nan::NewBuffer(reinterpret_cast<char*>(data.data), data.size).ToLocalChecked();

                    const auto& o = input->ons[name];
                    for (const auto& cb : o) {
                        if (!cb->IsEmpty()) {
                            cb->Call(1, &value);
                        }
                    }

                    ++it;
                }
                input->datas.clear();
            }
            if (!input->metas.empty()) {
                const std::string name = "metadata";

                auto it = input->metas.cbegin();
                const auto end = input->metas.cend();
                while (it != end) {
                    // send this up to JS
                    const Input::Metadata& meta = *it;

                    Nan::HandleScope scope;
                    v8::Local<v8::Object> obj = Nan::New<v8::Object>();
                    obj->Set(Nan::New<v8::String>("vad").ToLocalChecked(), Nan::New<v8::Boolean>(meta.vad == 1));
                    obj->Set(Nan::New<v8::String>("angle").ToLocalChecked(), Nan::New<v8::Uint32>(meta.angle));
                    obj->Set(Nan::New<v8::String>("direction").ToLocalChecked(), Nan::New<v8::Uint32>(meta.direction));
                    v8::Local<v8::Value> value = obj;

                    const auto& o = input->ons[name];
                    for (const auto& cb : o) {
                        if (!cb->IsEmpty()) {
                            cb->Call(1, &value);
                        }
                    }

                    ++it;
                }
                input->metas.clear();
            }
        });
    uv_thread_create(&thread, Input::run, this);
    return true;
}

void Input::transferCallback(libusb_transfer* xfr)
{
    if (xfr->status == LIBUSB_TRANSFER_CANCELLED) {
        Input* input = static_cast<Input*>(xfr->user_data);
        --input->pendingCancels;
        libusb_free_transfer(xfr);
        return;
    }

    const size_t size = Iso::PacketSize * xfr->num_iso_packets;
    uint8_t* data = static_cast<uint8_t*>(malloc(size));

    bool error = false;
    uint8_t* cur = data;
    uint8_t* end = data + size;
    size_t bytes = 0;
    for (int i = 0; i < xfr->num_iso_packets; ++i) {
        libusb_iso_packet_descriptor* pack = &xfr->iso_packet_desc[i];
        if (pack->status != LIBUSB_TRANSFER_COMPLETED) {
            // bad?
            printf("xfr not completed\n");
            continue;
        }
        if (cur + Iso::PacketSize > end) {
            // this would be bad
            printf("xfr buffer overflow\n");
            error = true;
            break;
        }
        const uint8_t* isodata = libusb_get_iso_packet_buffer_simple(xfr, i);
        memcpy(cur, isodata, Iso::PacketSize);
        cur += Iso::PacketSize;
        bytes += Iso::PacketSize;
    }

    if (error) {
        free(data);
    } else {
        // tell our async thingy
        Input* input = static_cast<Input*>(xfr->user_data);
        MutexLocker locker(&input->mutex);
        input->datas.push_back(Input::Data{ data, bytes });
        uv_async_send(&input->async);
    }

    // we're done, submit the transfer back to libusb
    libusb_submit_transfer(xfr);
}

void Input::irqCallback(libusb_transfer* xfr)
{
    if (xfr->status != LIBUSB_TRANSFER_COMPLETED) {
        if (xfr->status == LIBUSB_TRANSFER_CANCELLED) {
            Input* input = static_cast<Input*>(xfr->user_data);
            --input->pendingCancels;
            libusb_free_transfer(xfr);
        }
        return;
    }
    if (xfr->actual_length >= 6) {
        unsigned char irq1 = xfr->buffer[0];
        unsigned char irq2 = xfr->buffer[1];
        if (irq1 == 0x06 && irq2 == 0x36) {
            Input* input = static_cast<Input*>(xfr->user_data);

            // VAD / DOA change
            // byte 3 is VAD status,
            // byte 4 is high byte of angle
            // byte 5 is low byte of angle
            // byte 6 is direction
            const uint8_t vad = xfr->buffer[2];
            const uint16_t angle = (static_cast<uint16_t>(xfr->buffer[4]) << 8) | xfr->buffer[3];
            const uint8_t direction = xfr->buffer[5];

            MutexLocker locker(&input->mutex);
            input->metas.push_back(Metadata{ vad, direction, angle });
            uv_async_send(&input->async);
        }
    }

    // we're done, submit the transfer back to libusb
    libusb_submit_transfer(xfr);
}

void Input::run(void* arg)
{
    Input* input = static_cast<Input*>(arg);

    // allocate isochronous data transfers
    int ret;
    for (int i = 0; i < Iso::NumTransfer; ++i) {
        input->iso.xfr[i] = libusb_alloc_transfer(Iso::NumPackets);
        if (!input->iso.xfr[i]) {
            // bad, handle me
            printf("unable to allocate xfr\n");
        }

        libusb_fill_iso_transfer(input->iso.xfr[i], input->handle, Iso::EpIsoIn,
                                 input->iso.buf, sizeof(input->iso.buf), Iso::NumPackets,
                                 Input::transferCallback, input, 1000);
        libusb_set_iso_packet_lengths(input->iso.xfr[i], sizeof(input->iso.buf) / Iso::NumPackets);
        ret = libusb_submit_transfer(input->iso.xfr[i]);
        if (ret < 0) {
            printf("error submitting iso %d %d\n", ret, errno);
        }
    }
    // allocate irq transfer
    input->irq.xfr = libusb_alloc_transfer(0);
    libusb_fill_interrupt_transfer(input->irq.xfr, input->handle, Irq::EpIn,
                                   input->irq.buf, sizeof(input->irq.buf),
                                   Input::irqCallback, input, 0);
    ret = libusb_submit_transfer(input->irq.xfr);
    if (ret < 0) {
        printf("error submitting irq %d %d\n", ret, errno);
    }

    // 1 second
    struct timeval tv = { 1, 0 };
    for (;;) {
        libusb_handle_events_timeout_completed(input->usb, &tv, nullptr);

        MutexLocker locker(&input->mutex);
        if (input->stopped) {
            if (input->pendingCancels == -1) {
                input->pendingCancels = 1 + Iso::NumTransfer;
                libusb_cancel_transfer(input->irq.xfr);
                for (int i = 0; i < Iso::NumTransfer; ++i) {
                    libusb_cancel_transfer(input->iso.xfr[i]);
                }
            }
            if (!input->pendingCancels)
                break;
        }
    }
}

static State state;

NAN_METHOD(create) {
    Input* input = new Input;
    state.inputs.insert(input);

    info.GetReturnValue().Set(Nan::New<v8::External>(input));
}

NAN_METHOD(open) {
    if (info.Length() < 1 || !info[0]->IsExternal()) {
        Nan::ThrowError("Need an external to open");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsObject()) {
        Nan::ThrowError("Need an object to open");
        return;
    }

    v8::Local<v8::Object> data = v8::Local<v8::Object>::Cast(info[1]);
    auto busKey = Nan::New<v8::String>("bus").ToLocalChecked();
    auto portKey = Nan::New<v8::String>("port").ToLocalChecked();
    if (!data->Has(busKey)) {
        Nan::ThrowError("Need a bus value");
        return;
    }
    if (!data->Has(portKey)) {
        Nan::ThrowError("Need a port value");
        return;
    }
    auto busValue = data->Get(busKey);
    auto portValue = data->Get(portKey);
    if (!busValue->IsUint32()) {
        Nan::ThrowError("Bus needs to be an int");
        return;
    }
    if (!portValue->IsUint32()) {
        Nan::ThrowError("Port needs to be an int");
        return;
    }

    Input* input = static_cast<Input*>(v8::Local<v8::External>::Cast(info[0])->Value());
    if (!input->open(v8::Local<v8::Uint32>::Cast(busValue)->Value(), v8::Local<v8::Uint32>::Cast(portValue)->Value())) {
        return;
    }
}

NAN_METHOD(destroy) {
    if (info.Length() < 1 || !info[0]->IsExternal()) {
        Nan::ThrowError("Need an external to destroy");
        return;
    }
    Input* input = static_cast<Input*>(v8::Local<v8::External>::Cast(info[0])->Value());
    // remove from set and delete
    state.inputs.erase(input);
    delete input;
}

NAN_METHOD(enumerate) {
    if (info.Length() < 1 || !info[0]->IsExternal()) {
        Nan::ThrowError("Need an external to open");
        return;
    }
    Input* input = static_cast<Input*>(v8::Local<v8::External>::Cast(info[0])->Value());
    libusb_device** list;
    ssize_t devices = libusb_get_device_list(input->usb, &list);
    if (devices < 0) {
        // error
        Nan::ThrowError("Error getting devices");
        return;
    }
    v8::Local<v8::Array> array = Nan::New<v8::Array>();
    int pos = 0;
    for (ssize_t i = 0; i < devices; ++i) {
        libusb_device* dev = list[i];
        libusb_device_descriptor desc;
        int ret = libusb_get_device_descriptor(dev, &desc);
        if (ret != 0) {
            // error
            continue;
        }
        if (desc.idVendor == Input::Vid && desc.idProduct == Input::Pid) {
            const uint8_t bus = libusb_get_bus_number(dev);
            const uint8_t port = libusb_get_port_number(dev);
            v8::Local<v8::Object> device = Nan::New<v8::Object>();
            device->Set(Nan::New<v8::String>("bus").ToLocalChecked(), Nan::New<v8::Uint32>(bus));
            device->Set(Nan::New<v8::String>("port").ToLocalChecked(), Nan::New<v8::Uint32>(port));
            array->Set(pos++, device);
            //printf("found %u %u\n", bus, port);
        }
    }
    libusb_free_device_list(list, 1);
    info.GetReturnValue().Set(array);
}

NAN_METHOD(on) {
    if (info.Length() < 1 || !info[0]->IsExternal()) {
        Nan::ThrowError("Need an external for on");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsString()) {
        Nan::ThrowError("Need a string for on");
        return;
    }
    if (info.Length() < 3 || !info[2]->IsFunction()) {
        Nan::ThrowError("Need a function for on");
        return;
    }
    Input* input = static_cast<Input*>(v8::Local<v8::External>::Cast(info[0])->Value());
    const std::string name = *Nan::Utf8String(info[1]);
    input->ons[name].push_back(std::make_shared<Nan::Callback>(v8::Local<v8::Function>::Cast(info[2])));
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, create);
    NAN_EXPORT(target, open);
    NAN_EXPORT(target, destroy);
    NAN_EXPORT(target, enumerate);
    NAN_EXPORT(target, on);
}

NODE_MODULE(uma8, Initialize)
