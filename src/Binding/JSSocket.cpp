/*
   Copyright 2016 Nidium Inc. All rights reserved.
   Use of this source code is governed by a MIT license
   that can be found in the LICENSE file.
*/
#include "Binding/JSSocket.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "Binding/JSUtils.h"

namespace Nidium {
namespace Binding {

// {{{ Preamble
#define SOCKET_RESERVED_SLOT 0

enum SocketProp
{
    kSocketProp_Binary = SOCKET_RESERVED_SLOT,
    kSocketProp_Readline,
    kSocketProp_Encoding,
    kSocketProp_Timeout,
    kSocketProp_END
};

/* only use on connected clients */
#define SOCKET_JSOBJECT(socket) (static_cast<JSSocket *>(socket)->getJSObject())

static void Socket_Finalize(JSFreeOp *fop, JSObject *obj);
static void Socket_Finalize_client(JSFreeOp *fop, JSObject *obj);
static bool nidium_socket_prop_get(JSContext *cx,
                                   JS::HandleObject obj,
                                   uint8_t id,
                                   JS::MutableHandleValue vp);
static bool nidium_socket_prop_set(JSContext *cx,
                                   JS::HandleObject obj,
                                   uint8_t id,
                                   bool strict,
                                   JS::MutableHandleValue vp);
static bool nidium_socket_connect(JSContext *cx, unsigned argc, JS::Value *vp);
static bool nidium_socket_listen(JSContext *cx, unsigned argc, JS::Value *vp);
static bool nidium_socket_write(JSContext *cx, unsigned argc, JS::Value *vp);
static bool nidium_socket_close(JSContext *cx, unsigned argc, JS::Value *vp);
static bool nidium_socket_sendto(JSContext *cx, unsigned argc, JS::Value *vp);

static bool
nidium_socket_client_write(JSContext *cx, unsigned argc, JS::Value *vp);
static bool
nidium_socket_client_sendFile(JSContext *cx, unsigned argc, JS::Value *vp);
static bool
nidium_socket_client_close(JSContext *cx, unsigned argc, JS::Value *vp);

static JSClass Socket_class
    = { "Socket",
        JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(kSocketProp_END + 1),
        JS_PropertyStub,
        JS_DeletePropertyStub,
        JS_PropertyStub,
        JS_StrictPropertyStub,
        JS_EnumerateStub,
        JS_ResolveStub,
        JS_ConvertStub,
        Socket_Finalize,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        JSCLASS_NO_INTERNAL_MEMBERS };

template <>
JSClass *JSExposer<JSSocket>::jsclass = &Socket_class;

static JSClass socket_client_class = { "SocketClient",
                                       JSCLASS_HAS_PRIVATE,
                                       JS_PropertyStub,
                                       JS_DeletePropertyStub,
                                       JS_PropertyStub,
                                       JS_StrictPropertyStub,
                                       JS_EnumerateStub,
                                       JS_ResolveStub,
                                       JS_ConvertStub,
                                       Socket_Finalize_client,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       JSCLASS_NO_INTERNAL_MEMBERS };

static JSFunctionSpec socket_client_funcs[]
    = { JS_FN("sendFile", nidium_socket_client_sendFile, 1, NIDIUM_JS_FNPROPS),
        JS_FN("write", nidium_socket_client_write, 1, NIDIUM_JS_FNPROPS),
        JS_FN("disconnect",
              nidium_socket_client_close,
              0,
              NIDIUM_JS_FNPROPS), /* TODO: add force arg */
        JS_FS_END };

static JSFunctionSpec socket_funcs[]
    = { JS_FN("listen", nidium_socket_listen, 0, NIDIUM_JS_FNPROPS),
        JS_FN("connect", nidium_socket_connect, 0, NIDIUM_JS_FNPROPS),
        JS_FN("write", nidium_socket_write, 1, NIDIUM_JS_FNPROPS),
        JS_FN("disconnect",
              nidium_socket_close,
              0,
              NIDIUM_JS_FNPROPS), /* TODO: add force arg */
        JS_FN("sendTo", nidium_socket_sendto, 3, NIDIUM_JS_FNPROPS),
        JS_FS_END };

static JSPropertySpec Socket_props[] = { NIDIUM_JS_PSGS("binary",
                                                        kSocketProp_Binary,
                                                        nidium_socket_prop_get,
                                                        nidium_socket_prop_set),
                                         NIDIUM_JS_PSGS("readline",
                                                        kSocketProp_Readline,
                                                        nidium_socket_prop_get,
                                                        nidium_socket_prop_set),
                                         NIDIUM_JS_PSGS("encoding",
                                                        kSocketProp_Encoding,
                                                        nidium_socket_prop_get,
                                                        nidium_socket_prop_set),
                                         NIDIUM_JS_PSGS("timeout",
                                                        kSocketProp_Timeout,
                                                        nidium_socket_prop_get,
                                                        nidium_socket_prop_set),
                                         JS_PS_END };

// }}}

// {{{ JSSocket
JSSocket::JSSocket(JS::HandleObject obj,
                   JSContext *cx,
                   const char *host,
                   unsigned short port)
    : JSExposer<JSSocket>(obj, cx), m_Socket(NULL), m_Flags(0),
      m_FrameDelimiter('\n'), m_ParentServer(NULL), m_TCPTimeout(0)
{
    m_Host = strdup(host);
    m_Port = port;

    m_LineBuffer.pos  = 0;
    m_LineBuffer.data = NULL;

    m_Encoding = NULL;
}

void JSSocket::readFrame(const char *buf, size_t len)
{
    JS::RootedValue onread(m_Cx);
    JS::RootedValue rval(m_Cx);
    JS::AutoValueArray<2> jdata(m_Cx);
    JS::RootedString tstr(m_Cx, JSUtils::NewStringWithEncoding(
                                    m_Cx, buf, len, this->getEncoding()));
    JS::RootedString jstr(m_Cx);
    jstr = tstr;

    if (m_LineBuffer.pos
        && (this->getFlags() & JSSocket::kSocketType_Readline)) {
        JS::RootedString left(m_Cx, JSUtils::NewStringWithEncoding(
                                        m_Cx, m_LineBuffer.data,
                                        m_LineBuffer.pos, this->getEncoding()));

        jstr             = JS_ConcatStrings(m_Cx, left, tstr);
        m_LineBuffer.pos = 0;
    }

    if (isClientFromOwnServer()) {
        jdata[0].setObjectOrNull(this->getJSObject());
        jdata[1].setString(jstr);
    } else {
        jdata[0].setString(jstr);
    }

    JS::RootedObject obj(m_Cx, getReceiverJSObject());
    if (JS_GetProperty(m_Cx, obj, "onread", &onread)
        && JS_TypeOfValue(m_Cx, onread) == JSTYPE_FUNCTION) {
        PACK_TCP(m_Socket->s.fd);
        JS_CallFunctionValue(m_Cx, obj, onread, jdata, &rval);
        FLUSH_TCP(m_Socket->s.fd);
    }
}

bool JSSocket::isAttached()
{
    return (m_Socket != NULL);
}

bool JSSocket::isJSCallable()
{
    if (m_ParentServer && !m_ParentServer->getJSObject()) {
        return false;
    }
    return (this->getJSObject() != NULL);
}

void JSSocket::dettach()
{
    if (isAttached()) {
        m_Socket->ctx = NULL;
        m_Socket      = NULL;
    }
}

int JSSocket::write(unsigned char *data,
                    size_t len,
                    ape_socket_data_autorelease data_type)
{
    if (!m_Socket || !m_Socket->ctx) {
        return 0;
    }

    return APE_socket_write(m_Socket, data, len, data_type);
}

void JSSocket::disconnect()
{
    APE_socket_shutdown_now(m_Socket);
}

void JSSocket::onRead(const char *data, size_t len)
{
    JS::RootedValue onread(m_Cx);
    JS::RootedValue rval(m_Cx);

    if (!isJSCallable()) {
        return;
    }

    JS::AutoValueArray<2> jparams(m_Cx);
    int dataPosition = 0;

    if (isClientFromOwnServer()) {
        dataPosition = 1;
        JS::RootedObject obj(m_Cx, this->getJSObject());
        jparams[0].setObjectOrNull(obj);
    } else {
        dataPosition = 0;
    }

    if (this->getFlags() & JSSocket::kSocketType_Binary) {
        JS::RootedObject arrayBuffer(m_Cx, JS_NewArrayBuffer(m_Cx, len));
        uint8_t *adata = JS_GetArrayBufferData(arrayBuffer);
        memcpy(adata, data, len);

        jparams[dataPosition].setObject(*arrayBuffer);

    } else if (this->getFlags() & JSSocket::kSocketType_Readline) {
        const char *pBuf = data;
        size_t tlen      = len;
        char *eol;

        // TODO: new style cast
        while (
            tlen > 0
            && (eol = (char *)(memchr(pBuf, this->getFrameDelimiter(), tlen)))
                   != NULL) {

            size_t pLen = eol - pBuf;
            tlen -= pLen;
            if (tlen-- > 0) {
                this->readFrame(pBuf, pLen);
                pBuf = eol + 1;
            }
        }

        if (tlen && tlen + m_LineBuffer.pos <= SOCKET_LINEBUFFER_MAX) {
            memcpy(m_LineBuffer.data + m_LineBuffer.pos, pBuf, tlen);
            m_LineBuffer.pos += tlen;
        } else if (tlen) {
            m_LineBuffer.pos = 0;
        }

        return;
    } else {
        JS::RootedString jstr(m_Cx, JSUtils::NewStringWithEncoding(
                                        m_Cx, data, len, this->getEncoding()));

        jparams[dataPosition].setString(jstr);
    }

    JS::RootedObject obj(m_Cx, getReceiverJSObject());
    if (JS_GetProperty(m_Cx, obj, "onread", &onread)
        && JS_TypeOfValue(m_Cx, onread) == JSTYPE_FUNCTION) {
        PACK_TCP(m_Socket->s.fd);
        JS_CallFunctionValue(m_Cx, obj, onread, jparams, &rval);
        FLUSH_TCP(m_Socket->s.fd);
    }
}

void JSSocket::shutdown()
{
    if (!m_Socket || !m_Socket->ctx) {
        return;
    }
    APE_socket_shutdown(m_Socket);
}

JSSocket::~JSSocket()
{
    if (isAttached()) {
        m_Socket->ctx = NULL;
        this->disconnect();
    }
    free(m_Host);
    if (m_LineBuffer.data) {
        free(m_LineBuffer.data);
    }

    if (m_Encoding) {
        free(m_Encoding);
    }
}
// }}}

// {{{ Socket server/client common implementation
static bool nidium_socket_prop_get(JSContext *cx,
                                   JS::HandleObject obj,
                                   uint8_t id,
                                   JS::MutableHandleValue vp)
{
    JSSocket *nsocket = static_cast<JSSocket *>(JS_GetPrivate(obj));

    if (nsocket == NULL) {
        JS_ReportError(cx, "Invalid socket object");
        return false;
    }

    switch (id) {
        case kSocketProp_Binary:
            vp.setBoolean(nsocket->m_Flags & JSSocket::kSocketType_Binary);
            break;
        case kSocketProp_Readline:
            vp.setBoolean(nsocket->m_Flags & JSSocket::kSocketType_Readline);
            break;
        case kSocketProp_Encoding:
            vp.setString(JS_NewStringCopyZ(
                cx, nsocket->m_Encoding ? nsocket->m_Encoding : "ascii"));
            break;
        case kSocketProp_Timeout:
            vp.setInt32(nsocket->m_TCPTimeout);
            break;
        default:
            break;
    }
    return true;
}

static bool nidium_socket_prop_set(JSContext *cx,
                                   JS::HandleObject obj,
                                   uint8_t id,
                                   bool strict,
                                   JS::MutableHandleValue vp)
{
    JSSocket *nsocket = static_cast<JSSocket *>(JS_GetPrivate(obj));

    if (nsocket == NULL) {
        JS_ReportError(cx, "Invalid socket object");
        return false;
    }

    switch (id) {
        case kSocketProp_Binary: {
            if (vp.isBoolean()) {
                nsocket->m_Flags
                    = (vp.toBoolean() == true
                           ? nsocket->m_Flags | JSSocket::kSocketType_Binary
                           : nsocket->m_Flags & ~JSSocket::kSocketType_Binary);

            } else {
                vp.set(JSVAL_FALSE);
                return true;
            }
        } break;
        case kSocketProp_Readline: {
            bool isactive
                = ((vp.isBoolean() && vp.toBoolean() == true) || vp.isInt32());

            if (isactive) {

                nsocket->m_Flags |= JSSocket::kSocketType_Readline;

                if (nsocket->m_LineBuffer.data == NULL) {

                    nsocket->m_LineBuffer.data = static_cast<char *>(
                        malloc(sizeof(char) * SOCKET_LINEBUFFER_MAX));
                    nsocket->m_LineBuffer.pos = 0;
                }

                /*
                    Default delimiter is line feed.
                */
                nsocket->m_FrameDelimiter
                    = vp.isBoolean() ? '\n' : vp.toInt32() & 0xFF;

            } else {
                nsocket->m_Flags &= ~JSSocket::kSocketType_Readline;

                vp.set(JSVAL_FALSE);
                return true;
            }
        } break;
        case kSocketProp_Encoding: {
            if (vp.isString()) {
                JSAutoByteString enc(cx, vp.toString());
                nsocket->m_Encoding = strdup(enc.ptr());
            }
        } break;
        case kSocketProp_Timeout: {
            if (vp.isNumber()) {
                nsocket->m_TCPTimeout = APE_ABS(vp.toInt32());

                if (nsocket->m_Socket
                    && !APE_socket_setTimeout(nsocket->m_Socket,
                                              nsocket->m_TCPTimeout)) {

                    JS_ReportWarning(cx, "Couldn't set TCP timeout on socket");
                }
            }
        } break;
        default:
            break;
    }
    return true;
}

static bool
nidium_Socket_constructor(JSContext *cx, unsigned argc, JS::Value *vp)
{
    JS::RootedString host(cx);

    unsigned int port;
    JSSocket *nsocket;

    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (!args.isConstructing()) {
        JS_ReportError(cx, "Bad constructor");
        return false;
    }

    JS::RootedObject ret(cx,
                         JS_NewObjectForConstructor(cx, &Socket_class, args));

    if (!JS_ConvertArguments(cx, args, "Su", host.address(), &port)) {
        return false;
    }

    JSAutoByteString chost(cx, host);

    nsocket = new JSSocket(ret, cx, chost.ptr(), port);

    JS_SetPrivate(ret, nsocket);

    args.rval().setObjectOrNull(ret);

    JS_DefineFunctions(cx, ret, socket_funcs);
    JS_DefineProperties(cx, ret, Socket_props);


    return true;
}
// }}}

// {{{ Socket server/client common callbacks
static void nidium_socket_wrapper_client_onmessage(ape_socket *socket_server,
                                                   ape_global *ape,
                                                   const unsigned char *packet,
                                                   size_t len,
                                                   struct sockaddr_in *addr,
                                                   void *socket_arg)
{
    JSContext *cx;
    JSSocket *nsocket = static_cast<JSSocket *>(socket_server->ctx);


    if (nsocket == NULL || !nsocket->isJSCallable()) {
        return;
    }

    cx = nsocket->getJSContext();
    JS::AutoValueArray<2> jparams(cx);
    JS::RootedValue onmessage(cx);
    JS::RootedValue rval(cx);

    if (nsocket->m_Flags & JSSocket::kSocketType_Binary) {
        JS::RootedObject arrayBuffer(cx, JS_NewArrayBuffer(cx, len));
        uint8_t *data = JS_GetArrayBufferData(arrayBuffer);
        memcpy(data, packet, len);

        jparams[0].setObject(*arrayBuffer);

    } else {
        // TODO: new style cast
        JS::RootedString jstr(
            cx, JSUtils::NewStringWithEncoding(cx, (char *)(packet), len,
                                               nsocket->m_Encoding));

        jparams[0].setString(jstr);
    }

    JS::RootedObject obj(cx, nsocket->getJSObject());
    if (JS_GetProperty(cx, obj, "onmessage", &onmessage)
        && JS_TypeOfValue(cx, onmessage) == JSTYPE_FUNCTION) {

        JS::RootedObject remote(
            cx, JS_NewObject(cx, NULL, JS::NullPtr(), JS::NullPtr()));

        /*
            TODO: inet_ntoa is not reentrant
        */
        char *cip = inet_ntoa(addr->sin_addr);
        JS::RootedString jip(cx, JS_NewStringCopyZ(cx, cip));
        JS::RootedValue vip(cx, STRING_TO_JSVAL(jip));

        JS_SetProperty(cx, remote, "ip", vip);
        JS::RootedValue jport(cx, INT_TO_JSVAL(ntohs(addr->sin_port)));

        JS_SetProperty(cx, remote, "port", jport);

        jparams[1].setObject(*remote);

        JS_CallFunctionValue(cx, obj, onmessage, jparams, &rval);
    }
}
// }}}

// {{{ Socket server callbacks
static void nidium_socket_wrapper_onaccept(ape_socket *socket_server,
                                           ape_socket *socket_client,
                                           ape_global *ape,
                                           void *socket_arg)
{
    JSContext *m_Cx;

    JSSocket *nsocket = static_cast<JSSocket *>(socket_server->ctx);

    if (nsocket == NULL || !nsocket->isJSCallable()) {
        return;
    }

    m_Cx = nsocket->getJSContext();

    JS::RootedValue onaccept(m_Cx);
    JS::RootedValue rval(m_Cx);
    JS::AutoValueArray<1> params(m_Cx);
    JS::RootedObject jclient(m_Cx, JS_NewObject(m_Cx, &socket_client_class,
                                                JS::NullPtr(), JS::NullPtr()));

    NidiumJSObj(m_Cx)->rootObjectUntilShutdown(jclient);

    JSSocket *sobj = new JSSocket(jclient, nsocket->getJSContext(),
                                  APE_socket_ipv4(socket_client), 0);

    sobj->m_ParentServer = nsocket;
    sobj->m_Socket       = socket_client;

    if (sobj->getFlags() & JSSocket::kSocketType_Readline) {
        sobj->m_LineBuffer.data
            = static_cast<char *>(malloc(sizeof(char) * SOCKET_LINEBUFFER_MAX));
        sobj->m_LineBuffer.pos = 0;
    }

    socket_client->ctx = sobj;

    JS_SetPrivate(jclient, sobj);

    JS_DefineFunctions(m_Cx, jclient, socket_client_funcs);

    NIDIUM_JSOBJ_SET_PROP_CSTR(jclient, "ip", APE_socket_ipv4(socket_client));

    params[0].setObject(*jclient);

    if (APE_SOCKET_IS_LZ4(socket_server, tx)) {
        APE_socket_enable_lz4(socket_client,
                              APE_LZ4_COMPRESS_TX | APE_LZ4_COMPRESS_RX);
    }

    JS::RootedObject socketjs(m_Cx, nsocket->getJSObject());

    if (JS_GetProperty(m_Cx, socketjs, "onaccept", &onaccept)
        && JS_TypeOfValue(m_Cx, onaccept) == JSTYPE_FUNCTION) {

        PACK_TCP(socket_client->s.fd);
        JS::RootedValue onacceptVal(m_Cx, onaccept);
        JS_CallFunctionValue(m_Cx, socketjs, onacceptVal, params, &rval);
        FLUSH_TCP(socket_client->s.fd);
    }
}

static void nidium_socket_wrapper_client_read(ape_socket *socket_client,
                                              const uint8_t *data,
                                              size_t len,
                                              ape_global *ape,
                                              void *socket_arg)
{
    JSSocket *client = static_cast<JSSocket *>(socket_client->ctx);

    if (client == NULL) {
        return;
    }

    client->onRead(reinterpret_cast<const char *>(data), len);
}

static void nidium_socket_wrapper_client_disconnect(ape_socket *socket_client,
                                                    ape_global *ape,
                                                    void *socket_arg)
{
    JSContext *cx;

    JSSocket *csocket = static_cast<JSSocket *>(socket_client->ctx);
    if (!csocket || !csocket->isClientFromOwnServer()) {
        return;
    }

    JSSocket *ssocket = csocket->getParentServer();

    if (ssocket == NULL || !ssocket->isJSCallable()) {
        return;
    }

    cx = ssocket->getJSContext();

    JS::RootedValue ondisconnect(cx);
    JS::RootedValue rval(cx);

    JS::AutoValueArray<1> jparams(cx);
    jparams[0].setObject(*csocket->getJSObject());

    csocket->dettach();

    JS::RootedObject obj(cx, ssocket->getJSObject());
    if (JS_GetProperty(cx, obj, "ondisconnect", &ondisconnect)
        && JS_TypeOfValue(cx, ondisconnect) == JSTYPE_FUNCTION) {

        JS_CallFunctionValue(cx, obj, ondisconnect, jparams, &rval);
    }

    NidiumJSObj(cx)->unrootObject(csocket->getJSObject());
}
// }}}

// {{{ Socket server implementation
static bool nidium_socket_listen(JSContext *cx, unsigned argc, JS::Value *vp)
{
    ape_socket *socket;
    ape_socket_proto protocol = APE_SOCKET_PT_TCP;
    bool isLZ4                = false;

    ape_global *net = static_cast<ape_global *>(JS_GetContextPrivate(cx));

    NIDIUM_JS_PROLOGUE_CLASS(JSSocket, &Socket_class);

    if (CppObj->isAttached()) {
        return true;
    }

    if (args.length() > 0 && args[0].isString()) {
        JS::RootedString farg(cx, args[0].toString());

        JSAutoByteString cproto(cx, farg);

        if (strncasecmp("udp", cproto.ptr(), 3) == 0) {
            protocol = APE_SOCKET_PT_UDP;
        } else if (strncasecmp("tcp-lz4", cproto.ptr(), 7) == 0) {
            isLZ4 = true;
        }
    }

    if ((socket = APE_socket_new(protocol, 0, net)) == NULL) {
        JS_ReportError(cx, "Failed to create socket");
        return false;
    }

    socket->callbacks.on_connect    = nidium_socket_wrapper_onaccept;
    socket->callbacks.on_read       = nidium_socket_wrapper_client_read;
    socket->callbacks.on_disconnect = nidium_socket_wrapper_client_disconnect;
    socket->callbacks.on_message    = nidium_socket_wrapper_client_onmessage;
    /* TODO: need a drain for client socket */
    // socket->callbacks.on_drain      = nidium_socket_wrapper_client_ondrain;
    socket->callbacks.on_drain = NULL;
    socket->ctx                = CppObj;

    CppObj->m_Socket = socket;

    if (CppObj->m_TCPTimeout) {
        if (!APE_socket_setTimeout(socket, CppObj->m_TCPTimeout)) {
            JS_ReportWarning(cx, "Couldn't set TCP timeout on socket\n");
        }
    }

    if (APE_socket_listen(socket, CppObj->m_Port, CppObj->m_Host, 0, 0) == -1) {
        JS_ReportError(cx, "Can't listen on socket (%s:%d)", CppObj->m_Host,
                       CppObj->m_Port);
        /* TODO: close() leak */
        return false;
    }

    if (isLZ4) {
        APE_socket_enable_lz4(socket,
                              APE_LZ4_COMPRESS_TX | APE_LZ4_COMPRESS_RX);
    }

    NidiumJSObj(cx)->rootObjectUntilShutdown(thisobj);

    args.rval().setObjectOrNull(thisobj);

    CppObj->m_Flags |= JSSocket::kSocketType_Server;

    return true;
}

static void Socket_Finalize(JSFreeOp *fop, JSObject *obj)
{
    JSSocket *nsocket = static_cast<JSSocket *>(JS_GetPrivate(obj));

    if (nsocket != NULL) {
        delete nsocket;
    }
}

static bool nidium_socket_write(JSContext *cx, unsigned argc, JS::Value *vp)
{
    NIDIUM_JS_CHECK_ARGS("write", 1);

    NIDIUM_JS_PROLOGUE_CLASS(JSSocket, &Socket_class);

    if (!CppObj->isAttached()) {

        JS_ReportWarning(cx, "socket.write() Invalid socket (not connected)");
        args.rval().setInt32(-1);
        return true;
    }

    if (args[0].isString()) {
        JSAutoByteString cdata;
        JS::RootedString str(cx, args[0].toString());
        cdata.encodeUtf8(cx, str);

        int ret = CppObj->write(reinterpret_cast<unsigned char *>(cdata.ptr()),
                                strlen(cdata.ptr()), APE_DATA_COPY);

        args.rval().setInt32(ret);

    } else if (args[0].isObject()) {
        JSObject *objdata = args[0].toObjectOrNull();

        if (!objdata || !JS_IsArrayBufferObject(objdata)) {
            JS_ReportError(cx,
                           "write() invalid data (must be either a string or "
                           "an ArrayBuffer)");
            return false;
        }
        uint32_t len  = JS_GetArrayBufferByteLength(objdata);
        uint8_t *data = JS_GetArrayBufferData(objdata);

        int ret = CppObj->write(data, len, APE_DATA_COPY);

        args.rval().setInt32(ret);

    } else {
        JS_ReportError(
            cx,
            "write() invalid data (must be either a string or an ArrayBuffer)");
        return false;
    }

    return true;
}

static bool nidium_socket_close(JSContext *cx, unsigned argc, JS::Value *vp)
{
    NIDIUM_JS_PROLOGUE_CLASS(JSSocket, &Socket_class);

    if (!CppObj->isAttached()) {
        JS_ReportWarning(cx, "socket.close() Invalid socket (not connected)");
        args.rval().setInt32(-1);
        return true;
    }

    CppObj->shutdown();

    return true;
}

static bool nidium_socket_sendto(JSContext *cx, unsigned argc, JS::Value *vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS::RootedObject caller(cx, JS_THIS_OBJECT(cx, vp));

    NIDIUM_JS_CHECK_ARGS("sendto", 3);

    if (JS_InstanceOf(cx, caller, &Socket_class, &args) == false) {
        return false;
    }

    JSSocket *nsocket = static_cast<JSSocket *>(JS_GetPrivate(caller));

    if (nsocket == NULL || !nsocket->isAttached()) {
        return true;
    }

    if (!(nsocket->m_Flags & JSSocket::kSocketType_Server)) {
        JS_ReportError(cx, "sendto() is only available on listening socket");
        return false;
    }

    if (!args[0].isString()) {
        JS_ReportError(cx, "sendto() IP must be a string");
        return false;
    }

    JS::RootedString ip(cx, args[0].toString());
    unsigned int port = args[1].isNumber() ? args[1].toInt32() : 0;

    JSAutoByteString cip(cx, ip);

    if (args[2].isString()) {
        JSAutoByteString cdata(cx, args[2].toString());

        ape_socket_write_udp(nsocket->m_Socket, cdata.ptr(), cdata.length(),
                             cip.ptr(), static_cast<uint16_t>(port));
    } else if (args[2].isObject()) {
        JSObject *objdata = args[2].toObjectOrNull();

        if (!objdata || !JS_IsArrayBufferObject(objdata)) {
            JS_ReportError(cx,
                           "sendTo() invalid data (must be either a string or "
                           "an ArrayBuffer)");
            return false;
        }
        uint32_t len  = JS_GetArrayBufferByteLength(objdata);
        uint8_t *data = JS_GetArrayBufferData(objdata);

        ape_socket_write_udp(nsocket->m_Socket, reinterpret_cast<char *>(data),
                             len, cip.ptr(), static_cast<uint16_t>(port));

    } else {
        JS_ReportError(cx,
                       "sendTo() invalid data (must be either a string or an "
                       "ArrayBuffer)");
        return false;
    }

    return true;
}
// }}}

// {{{ Socket client callbacks
static void nidium_socket_wrapper_onconnected(ape_socket *s,
                                              ape_global *ape,
                                              void *socket_arg)
{
    JSContext *cx;

    JSSocket *nsocket = static_cast<JSSocket *>(s->ctx);

    if (nsocket == NULL || !nsocket->isJSCallable()) {
        return;
    }

    cx = nsocket->getJSContext();
    JS::RootedValue onconnect(cx);
    JS::RootedValue rval(cx);

    JS::RootedObject obj(cx, nsocket->getJSObject());

    if (JS_GetProperty(cx, obj, "onconnect", &onconnect)
        && JS_TypeOfValue(cx, onconnect) == JSTYPE_FUNCTION) {

        PACK_TCP(s->s.fd);
        JS_CallFunctionValue(cx, obj, onconnect, JS::HandleValueArray::empty(),
                             &rval);
        FLUSH_TCP(s->s.fd);
    }
}

static void nidium_socket_wrapper_read(ape_socket *s,
                                       const uint8_t *data,
                                       size_t len,
                                       ape_global *ape,
                                       void *socket_arg)
{
    JSSocket *nsocket = static_cast<JSSocket *>(s->ctx);

    if (nsocket == NULL || !nsocket->isJSCallable()) {
        return;
    }

    nsocket->onRead(reinterpret_cast<const char *>(data), len);
}

static void nidium_socket_wrapper_disconnect(ape_socket *s,
                                             ape_global *ape,
                                             void *socket_arg)
{
    JSContext *cx;
    JSSocket *nsocket = static_cast<JSSocket *>(s->ctx);

    if (nsocket == NULL || !nsocket->isJSCallable()) {
        return;
    }

    cx = nsocket->getJSContext();

    JS::RootedValue ondisconnect(cx);
    JS::RootedValue rval(cx);

    nsocket->dettach();

    JS::RootedObject obj(cx, nsocket->getJSObject());
    if (JS_GetProperty(cx, obj, "ondisconnect", &ondisconnect)
        && JS_TypeOfValue(cx, ondisconnect) == JSTYPE_FUNCTION) {
        JS_CallFunctionValue(cx, obj, ondisconnect,
                             JS::HandleValueArray::empty(), &rval);
    }

    NidiumJSObj(cx)->unrootObject(nsocket->getJSObject());
}

static void nidium_socket_wrapper_client_ondrain(ape_socket *socket_server,
                                                 ape_global *ape,
                                                 void *socket_arg)
{
    JSContext *cx;
    JSSocket *nsocket = static_cast<JSSocket *>(socket_server->ctx);

    if (nsocket == NULL || !nsocket->isJSCallable()) {
        return;
    }

    cx = nsocket->getJSContext();

    JS::RootedValue ondrain(cx);
    JS::RootedValue rval(cx);
    JS::RootedObject obj(cx, nsocket->getJSObject());

    if (JS_GetProperty(cx, obj, "ondrain", &ondrain)
        && JS_TypeOfValue(cx, ondrain) == JSTYPE_FUNCTION) {
        JS_CallFunctionValue(cx, obj, ondrain, JS::HandleValueArray::empty(),
                             &rval);
    }
}
// }}}

// {{{ Socket client implementation
static bool nidium_socket_connect(JSContext *cx, unsigned argc, JS::Value *vp)
{
    ape_socket *socket;
    ape_socket_proto protocol = APE_SOCKET_PT_TCP;
    uint16_t localport        = 0;
    bool isLZ4                = false;

    ape_global *net = static_cast<ape_global *>(JS_GetContextPrivate(cx));

    NIDIUM_JS_PROLOGUE_CLASS(JSSocket, &Socket_class);

    if (CppObj->isAttached()) {
        return false;
    }

    if (args.length() > 0 && args[0].isString()) {
        JS::RootedString farg(cx, args[0].toString());

        JSAutoByteString cproto(cx, farg);

        if (strncasecmp("udp", cproto.ptr(), 3) == 0) {
            protocol = APE_SOCKET_PT_UDP;
        } else if (strncasecmp("ssl", cproto.ptr(), 3) == 0) {
            protocol = APE_SOCKET_PT_SSL;
        } else if (strncasecmp("unix", cproto.ptr(), 4) == 0) {
            protocol = APE_SOCKET_PT_UNIX;
        } else if (strncasecmp("tcp-lz4", cproto.ptr(), 7) == 0) {
            isLZ4 = true;
        }

        localport = (args.length() > 1 && args[1].isNumber()
                         ? static_cast<uint16_t>(args[1].toInt32())
                         : 0);
    }

    if ((socket = APE_socket_new(protocol, 0, net)) == NULL) {
        JS_ReportError(cx, "Failed to create socket");
        return false;
    }

    socket->callbacks.on_connected  = nidium_socket_wrapper_onconnected;
    socket->callbacks.on_read       = nidium_socket_wrapper_read;
    socket->callbacks.on_disconnect = nidium_socket_wrapper_disconnect;
    socket->callbacks.on_message    = nidium_socket_wrapper_client_onmessage;
    socket->callbacks.on_drain      = nidium_socket_wrapper_client_ondrain;

    socket->ctx = CppObj;

    CppObj->m_Socket = socket;

    if (CppObj->m_TCPTimeout) {
        if (!APE_socket_setTimeout(socket, CppObj->m_TCPTimeout)) {
            JS_ReportWarning(cx, "Couldn't set TCP timeout on socket\n");
        }
    }

    if (isLZ4) {
        APE_socket_enable_lz4(socket,
                              APE_LZ4_COMPRESS_TX | APE_LZ4_COMPRESS_RX);
    }

    if (APE_socket_connect(socket, CppObj->m_Port, CppObj->m_Host, localport)
        == -1) {
        JS_ReportError(cx, "Can't connect on socket (%s:%d)", CppObj->m_Host,
                       CppObj->m_Port);
        return false;
    }

    NidiumJSObj(cx)->rootObjectUntilShutdown(thisobj);

    args.rval().setObjectOrNull(thisobj);

    return true;
}
static void Socket_Finalize_client(JSFreeOp *fop, JSObject *obj)
{
    JSSocket *nsocket = static_cast<JSSocket *>(JS_GetPrivate(obj));

    if (nsocket != NULL) {

        if (nsocket->m_Socket) {
            nsocket->m_Socket->ctx = NULL;
            APE_socket_shutdown_now(nsocket->m_Socket);
        }

        delete nsocket;
    }
}

static bool
nidium_socket_client_sendFile(JSContext *cx, unsigned argc, JS::Value *vp)
{
    JS::RootedString file(cx);

    NIDIUM_JS_CHECK_ARGS("sendFile", 1);

    NIDIUM_JS_PROLOGUE_CLASS(JSSocket, &socket_client_class);

    if (!CppObj->isAttached()) {
        JS_ReportWarning(cx,
                         "socket.sendFile() Invalid socket (not connected)");
        args.rval().setInt32(-1);
        return true;
    }
    if (!JS_ConvertArguments(cx, args, "S", file.address())) {
        return false;
    }

    JSAutoByteString cfile(cx, file);

    APE_sendfile(CppObj->m_Socket, cfile.ptr());

    return true;
}

static bool
nidium_socket_client_write(JSContext *cx, unsigned argc, JS::Value *vp)
{

    NIDIUM_JS_CHECK_ARGS("write", 1);

    NIDIUM_JS_PROLOGUE_CLASS(JSSocket, &socket_client_class);

    if (!CppObj->isAttached()) {

        JS_ReportWarning(cx, "socket.write() Invalid socket (not connected)");
        args.rval().setInt32(-1);
        return true;
    }

    if (args[0].isString()) {

        JSAutoByteString cdata;
        JS::RootedString str(cx, args[0].toString());
        cdata.encodeUtf8(cx, str);

        int ret = CppObj->write(reinterpret_cast<unsigned char *>(cdata.ptr()),
                                strlen(cdata.ptr()), APE_DATA_COPY);

        args.rval().setInt32(ret);

    } else if (args[0].isObject()) {
        JSObject *objdata = args[0].toObjectOrNull();

        if (!objdata || !JS_IsArrayBufferObject(objdata)) {
            JS_ReportError(cx,
                           "write() invalid data (must be either a string or "
                           "an ArrayBuffer)");
            return false;
        }
        uint32_t len  = JS_GetArrayBufferByteLength(objdata);
        uint8_t *data = JS_GetArrayBufferData(objdata);

        int ret = CppObj->write(data, len, APE_DATA_COPY);

        args.rval().setInt32(ret);

    } else {
        JS_ReportError(
            cx,
            "write() invalid data (must be either a string or an ArrayBuffer)");
        return false;
    }

    return true;
}

static bool
nidium_socket_client_close(JSContext *cx, unsigned argc, JS::Value *vp)
{
    NIDIUM_JS_PROLOGUE_CLASS(JSSocket, &socket_client_class);

    if (!CppObj->isAttached()) {
        JS_ReportWarning(cx, "socket.close() Invalid socket (not connected)");
        args.rval().setInt32(-1);
        return true;
    }

    CppObj->shutdown();

    return true;
}
// }}}

// {{{ Registration
NIDIUM_JS_OBJECT_EXPOSE(Socket)
// }}}

} // namespace Binding
} // namespace Nidium
