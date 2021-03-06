#include <node.h>
#include <node_buffer.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <uv.h>
#include <cstring>

#if NODE_MAJOR_VERSION>=10
#define NODE_WANT_INTERNALS 1
#if NODE_MAJOR_VERSION==10
#include "node_10_headers/tls_wrap.h"
#endif
#if NODE_MAJOR_VERSION==12
#include "node_12_headers/tls_wrap.h"
#include "node_12_headers/base_object-inl.h"
#endif
#if NODE_MAJOR_VERSION==13
#include "node_13_headers/tls_wrap.h"
#include "node_13_headers/base_object-inl.h"
#endif
#if NODE_MAJOR_VERSION==14
#include "node_14_headers/tls_wrap.h"
#include "node_14_headers/base_object-inl.h"
#endif

using BaseObject = node::BaseObject;
using TLSWrap = node::TLSWrap;
class TLSWrapSSLGetter : public node::TLSWrap {
    public:
        void setSSL(const v8::FunctionCallbackInfo<v8::Value> &info){
            v8::Isolate* isolate = info.GetIsolate();
            if (!ssl_){
                info.GetReturnValue().Set(v8::Null(isolate));
                return;
            }
            SSL* ptr = ssl_.get();
            info.GetReturnValue().Set(v8::External::New(isolate, ptr));
        }
};
#undef NODE_WANT_INTERNALS
#endif

using namespace std;
using namespace v8;

uWS::Hub hub(0);
uv_check_t check;
Persistent<Function> noop;

void registerCheck(Isolate *isolate) {
    uv_check_init((uv_loop_t *)hub.getLoop(), &check);
    check.data = isolate;
    uv_check_start(&check, [](uv_check_t *check) {
        Isolate *isolate = (Isolate *)check->data;
        HandleScope hs(isolate);
        // TODO: Check if we can use new callbback
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, noop), 0, nullptr);
    });
    uv_unref((uv_handle_t *)&check);
}

class NativeString {
    char *data;
    size_t length;
    char utf8ValueMemory[sizeof(String::Utf8Value)];
    String::Utf8Value *utf8Value = nullptr;

    public:
    NativeString(Isolate *isolate, const Local<Value> &value) {
        if (value->IsUndefined()) {
            data = nullptr;
            length = 0;
        } else if (value->IsString()) {
            utf8Value = new (utf8ValueMemory) String::Utf8Value(isolate, value);
            data = (**utf8Value);
            length = utf8Value->length();
        } else if (node::Buffer::HasInstance(value)) {
            data = node::Buffer::Data(value);
            length = node::Buffer::Length(value);
        } else if (value->IsTypedArray()) {
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            ArrayBuffer::Contents contents = arrayBufferView->Buffer()->GetContents();
            length = contents.ByteLength();
            data = (char *)contents.Data();
        } else if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            ArrayBuffer::Contents contents = arrayBuffer->GetContents();
            length = contents.ByteLength();
            data = (char *)contents.Data();
        } else {
            static char empty[] = "";
            data = empty;
            length = 0;
        }
    }

    char *getData() { return data; }

    size_t getLength() const { return length; }

    ~NativeString() {
        if (utf8Value) {
            utf8Value->~Utf8Value();
        }
    }
};

struct GroupData {
    Persistent<Function> connectionHandler, messageHandler, disconnectionHandler;
    int size = 0;
};

void createGroup(const FunctionCallbackInfo<Value> &args) {
    uWS::Group *group = hub.createGroup(args[0].As<Integer>()->Value(), args[1].As<Integer>()->Value());
    group->setUserData(new GroupData);
    args.GetReturnValue().Set(External::New(args.GetIsolate(), group));
}

inline Local<External> wrapSocket(uWS::WebSocket *webSocket, Isolate *isolate) {
    return External::New(isolate, webSocket);
}

inline uWS::WebSocket *unwrapSocket(Local<External> external) {
    return (uWS::WebSocket *)external->Value();
}

inline Local<Value> wrapMessage(const char *message, size_t length, uWS::OpCode opCode, Isolate *isolate) {
    if (opCode == uWS::OpCode::BINARY) {
        return node::Buffer::Copy(isolate, (char *)message, length).ToLocalChecked();
    } else {
        return String::NewFromUtf8(isolate, message, NewStringType::kNormal, length).ToLocalChecked();
    }
}

inline Local<Value> getDataV8(uWS::WebSocket *webSocket, Isolate *isolate) {
    return webSocket->getUserData() ? Local<Value>::New(isolate, *(Persistent<Value> *)webSocket->getUserData()) : Local<Value>::Cast(Undefined(isolate));
}

void getUserData(const FunctionCallbackInfo<Value> &args) {
    args.GetReturnValue().Set(getDataV8(unwrapSocket(args[0].As<External>()), args.GetIsolate()));
}

void clearUserData(const FunctionCallbackInfo<Value> &args) {
    uWS::WebSocket *webSocket = unwrapSocket(args[0].As<External>());
    ((Persistent<Value> *)webSocket->getUserData())->Reset();
    delete (Persistent<Value> *)webSocket->getUserData();
}

void setUserData(const FunctionCallbackInfo<Value> &args) {
    uWS::WebSocket *webSocket = unwrapSocket(args[0].As<External>());
    if (webSocket->getUserData()) {
        ((Persistent<Value> *)webSocket->getUserData())->Reset(args.GetIsolate(), args[1]);
    } else {
        webSocket->setUserData(new Persistent<Value>(args.GetIsolate(), args[1]));
    }
}

void getAddress(const FunctionCallbackInfo<Value> &args) {
    typename uWS::WebSocket::Address address = unwrapSocket(args[0].As<External>())->getAddress();
    Isolate *isolate = args.GetIsolate();
    Local<Array> array = Array::New(isolate, 3);
    array->Set(isolate->GetCurrentContext(), 0, Integer::New(isolate, address.port));
    array->Set(isolate->GetCurrentContext(), 1, String::NewFromUtf8(isolate, address.address, NewStringType::kNormal).ToLocalChecked());
    array->Set(isolate->GetCurrentContext(), 2, String::NewFromUtf8(isolate, address.family,  NewStringType::kNormal).ToLocalChecked());
    args.GetReturnValue().Set(array);
}

uv_handle_t *getTcpHandle(void *handleWrap) {
    volatile char *memory = (volatile char *)handleWrap;
    for (volatile uv_handle_t *tcpHandle = (volatile uv_handle_t *)memory;
            tcpHandle->type != UV_TCP || tcpHandle->data != handleWrap ||
            tcpHandle->loop != uv_default_loop();
            tcpHandle = (volatile uv_handle_t *)memory) {
        memory++;
    }
    return (uv_handle_t *)memory;
}

struct SendCallbackData {
    Persistent<Function> jsCallback;
    Isolate *isolate;
};

void sendCallback(uWS::WebSocket *webSocket, void *data, bool cancelled, void *reserved) {
    SendCallbackData *sc = static_cast<SendCallbackData *>(data);
    if (!cancelled) {
        HandleScope hs(sc->isolate);
        Local<Function>::New(sc->isolate, sc->jsCallback)->Call(sc->isolate->GetCurrentContext(), Null(sc->isolate), 0, nullptr);
    }
    sc->jsCallback.Reset();
    delete sc;
}

void send(const FunctionCallbackInfo<Value> &args) {
    uWS::OpCode opCode = (uWS::OpCode)args[2].As<Integer>()->Value();
    NativeString nativeString(args.GetIsolate(), args[1]);

    SendCallbackData *sc = nullptr;
    void (*callback)(uWS::WebSocket *, void *, bool, void *) = nullptr;

    if (args[3]->IsFunction()) {
        callback = sendCallback;
        sc = new SendCallbackData;
        sc->jsCallback.Reset(args.GetIsolate(), Local<Function>::Cast(args[3]));
        sc->isolate = args.GetIsolate();
    }

    bool compress = args[4].As<Boolean>()->Value();
    unwrapSocket(args[0].As<External>())->send(nativeString.getData(), nativeString.getLength(), opCode, callback, sc, compress);
}

struct Ticket {
    uv_os_sock_t fd;
    SSL *ssl;
};

void upgrade(const FunctionCallbackInfo<Value> &args) {
    uWS::Group *serverGroup = (uWS::Group *)args[0].As<External>()->Value();
    Ticket *ticket = static_cast<Ticket *>(args[1].As<External>()->Value());
    Isolate *isolate = args.GetIsolate();
    NativeString secKey(isolate, args[2]);
    NativeString extensions(isolate, args[3]);
    NativeString subprotocol(isolate, args[4]);

    // todo: move this check into core!
    if (ticket->fd != INVALID_SOCKET) {
        hub.upgrade(ticket->fd, secKey.getData(), ticket->ssl, extensions.getData(), extensions.getLength(), subprotocol.getData(), subprotocol.getLength(), serverGroup);
    } else {
        if (ticket->ssl) {
            SSL_free(ticket->ssl);
        }
    }
    delete ticket;
}

void transfer(const FunctionCallbackInfo<Value> &args) {
    // (_handle.fd OR _handle), SSL
    uv_handle_t *handle = nullptr;
    Ticket *ticket = new Ticket;
    if (args[0]->IsObject()) {
        Local<Context> context = args.GetIsolate()->GetCurrentContext();
        uv_fileno((handle = getTcpHandle( args[0]->ToObject(context).ToLocalChecked()->GetAlignedPointerFromInternalField(0))), (uv_os_fd_t *)&ticket->fd);
    } else {
        ticket->fd = args[0].As<Integer>()->Value();
    }

    ticket->fd = dup(ticket->fd);
    ticket->ssl = nullptr;
    if (args[1]->IsExternal()) {
        ticket->ssl = (SSL *)args[1].As<External>()->Value();
        SSL_up_ref(ticket->ssl);
    }

    // uv_close calls shutdown if not set on Windows
    if (handle) {
        // UV_HANDLE_SHARED_TCP_SOCKET
        handle->flags |= 0x40000000;
    }

    args.GetReturnValue().Set(External::New(args.GetIsolate(), ticket));
}

void onConnection(const FunctionCallbackInfo<Value> &args) {
    uWS::Group *group = (uWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *connectionCallback = &groupData->connectionHandler;
    connectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onConnection([isolate, connectionCallback, groupData](uWS::WebSocket *webSocket) {
        groupData->size++;
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate)};
        Local<Function>::New(isolate, *connectionCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 1, argv);
    });
}

void onMessage(const FunctionCallbackInfo<Value> &args) {
    uWS::Group *group = (uWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *messageCallback = &groupData->messageHandler;

    messageCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onMessage([isolate, messageCallback, group](uWS::WebSocket *webSocket, const char *message, size_t length, uWS::OpCode opCode) {
        if(length != 1 || message[0] != 65) {
            HandleScope hs(isolate);
            Local<Value> argv[] = {wrapMessage(message, length, opCode, isolate),
            getDataV8(webSocket, isolate)};
            Local<Function>::New(isolate, *messageCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 2, argv);
        }
    });
}

void onDisconnection(const FunctionCallbackInfo<Value> &args) {
    uWS::Group *group = (uWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *disconnectionCallback = &groupData->disconnectionHandler;
    disconnectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));

    group->onDisconnection([isolate, disconnectionCallback, groupData]( uWS::WebSocket *webSocket, int code, char *message, size_t length) {
        groupData->size--;
        HandleScope hs(isolate);
        Local<Value> argv[] = {
        wrapSocket(webSocket, isolate), Integer::New(isolate, code),
        wrapMessage(message, length, uWS::OpCode::CLOSE, isolate),
        getDataV8(webSocket, isolate)};
        Local<Function>::New(isolate, *disconnectionCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 4, argv);
    });
}

void closeSocket(const FunctionCallbackInfo<Value> &args) {
    NativeString nativeString(args.GetIsolate(), args[2]);
    unwrapSocket(args[0].As<External>())->close(args[1].As<Integer>()->Value(), nativeString.getData(), nativeString.getLength());
}

void closeGroup(const FunctionCallbackInfo<Value> &args) {
    NativeString nativeString(args.GetIsolate(), args[2]);
    uWS::Group *group = (uWS::Group *)args[0].As<External>()->Value();
    group->close(args[1].As<Integer>()->Value(), nativeString.getData(), nativeString.getLength());
}

void getSSLContext(const FunctionCallbackInfo<Value> &args) {
    Isolate* isolate = args.GetIsolate();
    if(args.Length() < 1 || !args[0]->IsObject()){
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Error: One object expected", NewStringType::kNormal).ToLocalChecked()));
        return;
    }
    Local<Context> context = isolate->GetCurrentContext();
    Local<Object> obj = args[0]->ToObject(context).ToLocalChecked();
#if NODE_MAJOR_VERSION < 10
    Local<Value> ext = obj->Get(String::NewFromUtf8(isolate, "_external"));
    args.GetReturnValue().Set(ext);
#else
    TLSWrapSSLGetter* tw;
    ASSIGN_OR_RETURN_UNWRAP(&tw, obj);
    tw->setSSL(args);
#endif
}

void setNoop(const FunctionCallbackInfo<Value> &args) {
    noop.Reset(args.GetIsolate(), Local<Function>::Cast(args[0]));
}

struct Namespace {
    Local<Object> object;
    Namespace(Isolate *isolate) {
        object = Object::New(isolate);
        NODE_SET_METHOD(object, "send", send);
        NODE_SET_METHOD(object, "close", closeSocket);

        Local<Object> group = Object::New(isolate);
        NODE_SET_METHOD(group, "onConnection", onConnection);
        NODE_SET_METHOD(group, "onMessage", onMessage);
        NODE_SET_METHOD(group, "onDisconnection", onDisconnection);

        NODE_SET_METHOD(group, "create", createGroup);
        NODE_SET_METHOD(group, "close", closeGroup);

        object->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "group", NewStringType::kNormal).ToLocalChecked(), group);
    }
};
