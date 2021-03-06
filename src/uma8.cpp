#include <nan.h>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <libusb.h>
#include "utils.h"

struct Input : public Nan::ObjectWrap
{
    Input();
    ~Input();

    bool isValid() const { return usb != nullptr; }

    bool open(uint8_t bus, uint8_t port);
    v8::Local<v8::Object> makeObject();

    libusb_context* usb;
    libusb_device_handle* handle;
    uv_thread_t thread;
    uv_async_t async;
    Mutex mutex;
    bool stopped, opened;
    int pendingCancels;
    std::string error;
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
        enum { NumTransfer = 10, NumPackets = 100, PacketSize = 24, EpIsoIn = 0x81 };

        struct Transfer {
            uint8_t buf[PacketSize * NumPackets];
            libusb_transfer* xfr;
        } transfers[NumTransfer];
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

v8::Local<v8::Object> Input::makeObject()
{
    Nan::EscapableHandleScope scope;
    v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>();
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    v8::Local<v8::Function> ctor = Nan::GetFunction(tpl).ToLocalChecked();
    v8::Local<v8::Object> obj = Nan::NewInstance(ctor, 0, nullptr).ToLocalChecked();
    Wrap(obj);
    return scope.Escape(obj);
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
                ret = libusb_open(dev, &handle);
                if (ret != 0) {
                    // error
                    libusb_free_device_list(list, 1);
                    Nan::ThrowError("Can't open");
                    return false;
                }
                break;
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
            if (!input->error.empty()) {
                Nan::HandleScope scope;
                Nan::ThrowError(Nan::New<v8::String>(input->error).ToLocalChecked());
                input->error.clear();
            }
        });
    uv_thread_create(&thread, Input::run, this);
    return true;
}

void Input::transferCallback(libusb_transfer* xfr)
{
    // this appears to return s32l 24khz 2ch audio even though the device spec says 24bit 16khz 2ch

    Input* input = static_cast<Input*>(xfr->user_data);
    if (xfr->status == LIBUSB_TRANSFER_CANCELLED) {
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
            MutexLocker locker(&input->mutex);
            input->error = "incomplete iso xfr";
            uv_async_send(&input->async);
            continue;
        }
        if (cur + Iso::PacketSize > end) {
            // this would be bad
            MutexLocker locker(&input->mutex);
            input->error = "overflow in iso xfr";
            uv_async_send(&input->async);
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
            const uint16_t angle = (static_cast<uint16_t>(xfr->buffer[3]) << 8) | xfr->buffer[4];
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
        input->iso.transfers[i].xfr = libusb_alloc_transfer(Iso::NumPackets);
        if (!input->iso.transfers[i].xfr) {
            // bad, handle me
            MutexLocker locker(&input->mutex);
            input->error = "Unable to allocate iso xfr";
            uv_async_send(&input->async);
            return;
        }

        libusb_fill_iso_transfer(input->iso.transfers[i].xfr, input->handle, Iso::EpIsoIn,
                                 input->iso.transfers[i].buf, sizeof(input->iso.transfers[i].buf), Iso::NumPackets,
                                 Input::transferCallback, input, 1000);
        libusb_set_iso_packet_lengths(input->iso.transfers[i].xfr, Iso::PacketSize);
        ret = libusb_submit_transfer(input->iso.transfers[i].xfr);
        if (ret < 0) {
            MutexLocker locker(&input->mutex);
            input->error = "Unable to submit iso xfr";
            uv_async_send(&input->async);
        }
    }
    // allocate irq transfer
    input->irq.xfr = libusb_alloc_transfer(0);
    if (!input->irq.xfr) {
        MutexLocker locker(&input->mutex);
        input->error = "Unable to allocate irq xfr";
        uv_async_send(&input->async);
        return;
    }
    libusb_fill_interrupt_transfer(input->irq.xfr, input->handle, Irq::EpIn,
                                   input->irq.buf, sizeof(input->irq.buf),
                                   Input::irqCallback, input, 0);
    ret = libusb_submit_transfer(input->irq.xfr);
    if (ret < 0) {
        MutexLocker locker(&input->mutex);
        input->error = "Unable to submit irq xfr";
        uv_async_send(&input->async);
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
                    libusb_cancel_transfer(input->iso.transfers[i].xfr);
                }
            }
            if (!input->pendingCancels)
                break;
        }
    }
}

NAN_METHOD(create) {
    Input* input = new Input;
    if (!input->isValid()) {
        Nan::ThrowError("Unable to initialize libusb");
        delete input;
        return;
    }
    info.GetReturnValue().Set(input->makeObject());
}

NAN_METHOD(open) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
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

    Input* input = Input::Unwrap<Input>(v8::Local<v8::Object>::Cast(info[0]));
    if (!input->open(v8::Local<v8::Uint32>::Cast(busValue)->Value(), v8::Local<v8::Uint32>::Cast(portValue)->Value())) {
        return;
    }
}

NAN_METHOD(enumerate) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external to enumerate");
        return;
    }
    Input* input = Input::Unwrap<Input>(v8::Local<v8::Object>::Cast(info[0]));
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
    if (info.Length() < 1 || !info[0]->IsObject()) {
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
    Input* input = Input::Unwrap<Input>(v8::Local<v8::Object>::Cast(info[0]));
    const std::string name = *Nan::Utf8String(info[1]);
    input->ons[name].push_back(std::make_shared<Nan::Callback>(v8::Local<v8::Function>::Cast(info[2])));
}

NAN_METHOD(removeListener) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external for removeListener");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsString()) {
        Nan::ThrowError("Need a string for removeListener");
        return;
    }
    if (info.Length() < 3 || !info[2]->IsFunction()) {
        Nan::ThrowError("Need a function for removeListener");
        return;
    }
    Input* input = Input::Unwrap<Input>(v8::Local<v8::Object>::Cast(info[0]));
    const std::string name = *Nan::Utf8String(info[1]);
    auto listeners = input->ons.find(name);
    if (listeners == input->ons.end()) {
        info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));
        return;
    }

    Nan::Callback cur(v8::Local<v8::Function>::Cast(info[2]));
    auto& listenerList = listeners->second;
    auto listener = listenerList.rbegin();
    const auto end = listenerList.rend();
    while (listener != end) {
        if (*listener && **listener == cur) {
            // got it
            listenerList.erase(std::next(listener).base());
            if (listenerList.empty()) {
                input->ons.erase(name);
            }
            info.GetReturnValue().Set(Nan::New<v8::Boolean>(true));
            return;
        }
        ++listener;
    }
    info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));
}

NAN_METHOD(removeAllListeners) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external for removeAllListeners");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsString()) {
        Nan::ThrowError("Need a string for removeAllListeners");
        return;
    }
    Input* input = Input::Unwrap<Input>(v8::Local<v8::Object>::Cast(info[0]));
    const std::string name = *Nan::Utf8String(info[1]);
    auto listeners = input->ons.find(name);
    if (listeners != input->ons.end()) {
        input->ons.erase(listeners);
        info.GetReturnValue().Set(Nan::New<v8::Boolean>(true));
    } else {
        info.GetReturnValue().Set(Nan::New<v8::Boolean>(false));
    }
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, create);
    NAN_EXPORT(target, open);
    NAN_EXPORT(target, enumerate);
    NAN_EXPORT(target, on);
    NAN_EXPORT(target, removeListener);
    NAN_EXPORT(target, removeAllListeners);
}

NODE_MODULE(uma8, Initialize)
