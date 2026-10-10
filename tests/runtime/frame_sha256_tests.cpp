#include "application/vision/frame_sha256.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <future>
#include <iostream>
#include <vector>

int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    int failures=0,assertions=0;
    const auto check=[&](bool ok,const char*label){++assertions;if(!ok){++failures;std::cout<<"FAIL "<<label<<'\n';}};
    using relink::vision::frameSha256;
    check(frameSha256({}).bytes.toHex()=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","sha256_empty_vector");
    check(frameSha256("abc").bytes.toHex()=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","sha256_abc_vector");
    QByteArray frame;
    for(int size:{1,63,64,65,1024,65537,2560*1440*4}) {
        frame.resize(size);
        for(int i=0;i<size;++i)frame[i]=char((i*73+(i>>6))&255);
        const auto original=frame;
        const auto actual=frameSha256(frame);
        check(actual.bytes==QCryptographicHash::hash(frame,QCryptographicHash::Sha256),"identical_to_existing_full_byte_sha256");
        check(frame==original &&actual.elapsedNs>=0,"input_unchanged_and_real_timing");
        frame[size-1]=char(frame[size-1]^1);
        check(frameSha256(frame).bytes!=actual.bytes,"last_byte_is_not_omitted_or_cached");
    }
    const auto expected=QCryptographicHash::hash(frame,QCryptographicHash::Sha256);
    std::vector<std::future<QByteArray>> threads;
    for(int i=0;i<4;++i)threads.push_back(std::async(std::launch::async,[&]{return frameSha256(frame).bytes;}));
    for(auto&job:threads)check(job.get()==expected,"independent_thread_provider_and_hash_state");
    if(app.arguments().contains("--benchmark")) {
        std::vector<double> oldTimes,newTimes;
        QString provider;
        for(int i=0;i<12;++i){
            QElapsedTimer t;t.start();const auto old=QCryptographicHash::hash(frame,QCryptographicHash::Sha256);
            oldTimes.push_back(t.nsecsElapsed()/1e6);
            const auto current=frameSha256(frame);newTimes.push_back(current.elapsedNs/1e6);provider=current.provider;
            check(old==current.bytes,"benchmark_digests_equal");
        }
        std::sort(oldTimes.begin(),oldTimes.end());std::sort(newTimes.begin(),newTimes.end());
        const QJsonObject measured{{"input_bytes",frame.size()},{"samples",12},{"provider",provider},
            {"qt_median_ms",(oldTimes[5]+oldTimes[6])/2},{"current_median_ms",(newTimes[5]+newTimes[6])/2},
            {"all_hashes_equal",failures==0},{"synthetic_input",true},{"game_actions",0}};
        std::cout<<QJsonDocument(measured).toJson(QJsonDocument::Compact).constData()<<'\n';
    }
    std::cout<<"FRAME_SHA256_TESTS="<<(failures?"FAIL":"PASS")<<"; assertions="<<assertions<<"; failures="<<failures<<'\n';
    return failures?1:0;
}
