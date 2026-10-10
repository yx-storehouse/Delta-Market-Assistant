#include "frame_sha256.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace relink::vision {
FrameDigest frameSha256(const QByteArray& data) {
    QElapsedTimer timer;timer.start();
#ifdef Q_OS_WIN
    struct Algorithm {
        BCRYPT_ALG_HANDLE handle=nullptr;
        DWORD objectSize=0;
        Algorithm() {
            DWORD bytes=0,hashSize=0;
            if(BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)handle=nullptr;
            if(handle &&(BCryptGetProperty(handle,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&bytes,0)<0
                ||bytes!=sizeof(objectSize) ||objectSize==0 ||objectSize>65536
                ||BCryptGetProperty(handle,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&hashSize),sizeof(hashSize),&bytes,0)<0
                ||bytes!=sizeof(hashSize) ||hashSize!=32)) {
                BCryptCloseAlgorithmProvider(handle,0);handle=nullptr;
            }
        }
        ~Algorithm(){if(handle)BCryptCloseAlgorithmProvider(handle,0);}
    };
    // Immutable algorithm handle per caller thread; every hash has a new hash
    // object. No pixels, digest, or hash state survive this function call.
    thread_local Algorithm algorithm;
    if(algorithm.handle &&quint64(data.size())<=std::numeric_limits<ULONG>::max()) {
        QByteArray object(algorithm.objectSize,'\0'),digest(32,'\0');
        BCRYPT_HASH_HANDLE hash=nullptr;
        if(BCryptCreateHash(algorithm.handle,&hash,reinterpret_cast<PUCHAR>(object.data()),ULONG(object.size()),nullptr,0,0)>=0) {
            const bool ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())),ULONG(data.size()),0)>=0
                &&BCryptFinishHash(hash,reinterpret_cast<PUCHAR>(digest.data()),ULONG(digest.size()),0)>=0;
            BCryptDestroyHash(hash);
            if(ok)return {digest,QStringLiteral("Windows.CNG.SHA256"),timer.nsecsElapsed()};
        }
    }
#endif
    const auto digest=QCryptographicHash::hash(data,QCryptographicHash::Sha256);
    return {digest,QStringLiteral("Qt.SHA256"),timer.nsecsElapsed()};
}
}
