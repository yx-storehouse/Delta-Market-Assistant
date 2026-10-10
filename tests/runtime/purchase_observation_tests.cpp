#include "application/vision/purchase_observation.h"
#include "application/vision/purchase_countdown.h"
#include <QCoreApplication>
#include <cstdio>
using namespace relink::vision;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);int checks=0,failures=0;
    const auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}};
    const auto word=[](QString s,int x,int y,int w=120,int h=28){return QJsonObject{{"text",s},{"x",x},{"y",y},{"width",w},{"height",h}};};
    QJsonObject o{{"width",2560},{"height",1440},{"coverage","full_client"},
        {"frame_id","synthetic:1"},{"frame_sha256",QString(64,'a')},
        {"words",QJsonArray{word(QStringLiteral("在售"),220,92),word(QStringLiteral("我的关注"),420,100),
            word(QStringLiteral("AS Val突击步枪-黑银先锋"),1920,245,490),
            word(QStringLiteral("2分3秒后解锁购买"),2000,1175,320),word(QStringLiteral("公示中"),2100,1220),
            word(QStringLiteral("个人信息不可投影"),8,8),word(QStringLiteral("确认"),1300,900),
            word(QStringLiteral("订单尚未开放购买"),1170,207,226,28),word(QStringLiteral("按稀有度升序"),1152,249,180,26)}}};
    QJsonObject page{{"page","watchlist_listings"},{"overlay","none"}};
    auto result=projectPurchaseObservation(o,page);
    check(result["valid"]==true,"known viewport text projection");
    check(result["actions_enabled"]==false &&result["purchase_authorized"]==false,"no action capability");
    check(result["live_calibrated"]==false,"synthetic geometry not live calibration");
    const auto regions=result["regions"].toArray();check(regions.size()==6,"bounded regions");
    for(const auto& v:regions){const auto r=v.toObject();
        check(r["frame_id"]==o["frame_id"]&&r["frame_sha256"]==o["frame_sha256"]&&r["same_frame"]==true,"frame binding");
        for(const auto& w:r["words"].toArray())check(w.toObject()["text"]!=QStringLiteral("个人信息不可投影"),"HUD excluded");
    }
    check(regions[3].toObject()["words"].toArray().size()==2,"countdown and publicity both retained");
    check(regions[4].toObject()["words"].toArray().size()==2,"dialog words not invented (确认, the sort label)");
    // The top toast straddles dialog_text's upper edge: only the toast region holds it.
    check(regions[5].toObject()["kind"]==QStringLiteral("toast")
          &&regions[5].toObject()["words"].toArray().size()==1
          &&regions[5].toObject()["words"].toArray()[0].toObject()["text"]==QStringLiteral("订单尚未开放购买"),"top toast kept, sort dropdown below it left out");
    for(const auto& mutation:QJsonArray{QJsonObject{{"width",1920}},QJsonObject{{"coverage","roi"}},
            QJsonObject{{"frame_id",""}},QJsonObject{{"frame_sha256","bad"}}}){
        auto bad=o;const auto m=mutation.toObject();for(auto i=m.begin();i!=m.end();++i)bad[i.key()]=i.value();
        check(projectPurchaseObservation(bad,page)["valid"]==false,"invalid source rejected");
    }
    auto bad=o;auto words=o["words"].toArray();auto first=words[0].toObject();first["x"]=-1;words[0]=first;bad["words"]=words;
    check(projectPurchaseObservation(bad,page)["valid"]==false,"invalid box rejected");
    bad=o;bad["words"]=QJsonArray{};check(projectPurchaseObservation(bad,page)["valid"]==true,"empty text is explicit not fabricated");
    check(projectPurchaseObservation(o,page)==result,"deterministic no mutation");
    {
        // The watch's line reading and the jump rule (user 2026-10-10: a changed
        // selection must end the follow watch at once).
        check(countdownLineSeconds(QStringLiteral("13分50秒后解锁购买"))==830,"line seconds");
        check(countdownLineSeconds(QStringLiteral("626分0秒后解锁购买"))==1560,"icon digit before the minutes");
        check(countdownLineSeconds(QStringLiteral("66分0秒后解锁购买"))==360,"icon read as 6 before a single-digit minute");
        check(countdownLineSeconds(QStringLiteral("62分45秒后解锁购买"))==165 &&countdownLineSeconds(QStringLiteral("60分39秒后解锁购买"))==39
              &&countdownLineSeconds(QStringLiteral("63分16秒后解锁购买"))==196,"live clock06/jump02 icon readings");
        check(countdownLineSeconds(QStringLiteral("72分0秒后解锁购买"))==CountdownLineUnread,"70 minutes and more unread");
        {
            // Words as the line OCR gives them (frame coordinates): the icon as its own word.
            const auto lw=[](const char* t,double x,double w){return QJsonObject{{"text",QString::fromUtf8(t)},{"x",x},{"width",w}};};
            QJsonArray icon{lw("6",2096,14),lw("2分45秒后解锁购买",2121,136)};
            check(countdownLineWithoutIcon(icon,countdownLineLayout("footer"))==QStringLiteral("2分45秒后解锁购买"),"icon word dropped by position");
            QJsonArray merged{lw("62分45秒后解锁购买",2096,161)};
            check(countdownLineWithoutIcon(merged,countdownLineLayout("footer"))==QStringLiteral("62分45秒后解锁购买"),
                  "one word only: kept as read, the minute rule applies");
            QJsonArray plain{lw("2分45秒后解锁购买",2121,136)};
            check(countdownLineWithoutIcon(plain,countdownLineLayout("footer"))==QStringLiteral("2分45秒后解锁购买"),"no icon read");
        }
        check(countdownLineSeconds(QStringLiteral("剩余：2天23小时"))==CountdownLineRemaining,"remaining line");
        check(countdownLineSeconds(QStringLiteral("1小时2分3秒后解锁购买"))==CountdownLineUnread,"hours not followed");
        check(countdownLineSeconds(QStringLiteral("0分3"))==CountdownLineUnread,"partial line unread");
        check(!countdownLineJumped(830,0,829,1000) &&!countdownLineJumped(830,0,830,20) &&!countdownLineJumped(-1,0,320,1000)
              &&!countdownLineJumped(830,0,CountdownLineUnread,1000),"a following second or an unread line is no jump");
        // live jump02: 3分17秒, then 7 s of unreadable "63分…", then 3分9秒: no jump.
        check(!countdownLineJumped(197,181,189,7604),"time passed while the line was unreadable");
        check(!countdownLineJumped(240,0,238,1000),"a whole-second re-sync of the display is no jump");
        check(countdownLineJumped(830,0,320,1000) &&countdownLineJumped(32,0,20,1000) &&countdownLineJumped(30,0,33,1000),
              "another listing");
        check(countdownLineJumped(30,0,CountdownLineRemaining,1000) &&!countdownLineJumped(1,0,CountdownLineRemaining,1000),
              "past the notice with seconds left is another listing; at the unlock it is not");
        // Against the followed zero (a selection changed between two watches).
        check(!countdownLineOffClock(830500.0,830,0) &&!countdownLineOffClock(830500.0,829,500)
              &&!countdownLineOffClock(830500.0,CountdownLineUnread,0),"the followed listing");
        check(countdownLineOffClock(830500.0,320,0) &&countdownLineOffClock(830500.0,CountdownLineRemaining,0)
              &&!countdownLineOffClock(1500.0,CountdownLineRemaining,0),"another listing, or the unlock itself");
    }
    {
        // Countdown line: a flat dark panel (30) with light text (220).
        relink::runtime::observation::FrameEnvelope frame;
        frame.width=2560;frame.height=1440;frame.strideBytes=2560*4;
        frame.pixels=QByteArray(qsizetype(frame.strideBytes)*frame.height,char(30));
        const auto paint=[&](relink::runtime::observation::FrameEnvelope& f,QRect r,int value){
            for(int y=r.top();y<=r.bottom();++y)for(int x=r.left();x<=r.right();++x){
                auto* p=reinterpret_cast<unsigned char*>(f.pixels.data()+qint64(y)*f.strideBytes+x*4);
                p[0]=p[1]=p[2]=(unsigned char)value;p[3]=255;}
        };
        paint(frame,QRect(2131,1172,25,17),220);   // "10" of 0分10秒
        const auto before=countdownInk(frame,PurchaseCountdownRoi);
        check(before.valid &&before.bright==25*17,"countdown ink counts bright text");
        auto same=frame;
        paint(same,QRect(2131,1172,25,1),120);       // anti-aliased edge flicker
        check(countdownStrongDifference(before,countdownInk(same,PurchaseCountdownRoi))==0,"edge flicker is not a tick");
        auto next=frame;
        paint(next,QRect(2131,1172,25,17),30);paint(next,QRect(2131,1172,11,17),220);  // "10" -> "9"
        const int changed=countdownStrongDifference(before,countdownInk(next,PurchaseCountdownRoi));
        check(changed==14*17 &&changed>=PurchaseCountdownChangePixels,"a changed digit is a tick");
        auto outside=frame;paint(outside,QRect(2100,1210,40,40),220);  // the button below
        check(countdownStrongDifference(before,countdownInk(outside,PurchaseCountdownRoi))==0,"button pixels are outside the line");
        auto small=frame;small.width=1920;
        check(!countdownInk(small,PurchaseCountdownRoi).valid &&countdownStrongDifference(before,{})==-1,"uncalibrated frame rejected");
        // Live clock03: the clock icon left of the text (a filled disc) was
        // read as "6". Its text region starts right of it.
        auto withIcon=frame;
        for(int y=1170;y<1194;++y)for(int x=2063;x<2083;++x){
            const int dx=2*x-2*2072-1,dy=2*y-2*1181-1;
            if(dx*dx+dy*dy<=20*20){auto* q=reinterpret_cast<unsigned char*>(withIcon.pixels.data()+qint64(y)*withIcon.strideBytes+x*4);q[0]=q[1]=q[2]=225;}
        }
        paint(withIcon,QRect(2088,1172,11,17),220);paint(withIcon,QRect(2101,1172,11,17),220);
        const auto iconText=countdownTextRegion(countdownInk(withIcon,PurchaseCountdownRoi),PurchaseCountdownRoi);
        check(iconText.iconExcluded &&iconText.region.left()>2082 &&iconText.region.left()<2088
            &&iconText.region.right()==PurchaseCountdownRoi.right(),"clock icon is left out of the line read");
        check(iconText.runStart==2063 &&iconText.runEnd==2083 &&iconText.nextRun==2088 &&iconText.runPixels>=150,
            "first ink run metrics are reported for calibration");
        // Without the icon: stroke glyphs, including two touching digits.
        auto strokes=relink::runtime::observation::FrameEnvelope(frame);
        strokes.pixels=QByteArray(qsizetype(strokes.strideBytes)*strokes.height,char(30));
        const auto outline=[&](QRect r){paint(strokes,QRect(r.left(),r.top(),r.width(),2),220);paint(strokes,QRect(r.left(),r.bottom()-1,r.width(),2),220);
            paint(strokes,QRect(r.left(),r.top(),2,r.height()),220);paint(strokes,QRect(r.right()-1,r.top(),2,r.height()),220);};
        outline(QRect(2088,1172,12,17));outline(QRect(2100,1172,12,17));outline(QRect(2120,1171,19,20));
        const auto plain=countdownTextRegion(countdownInk(strokes,PurchaseCountdownRoi),PurchaseCountdownRoi);
        check(!plain.iconExcluded &&plain.region==PurchaseCountdownRoi,"a first digit is never taken for the icon");
        // The buy button: white 公示中 to a red price is a strong change.
        // User screenshot: white 公示中 on dark -> green filled button with the price.
        auto publicity=frame;paint(publicity,QRect(2138,1231,75,24),225);
        auto priced=frame;paint(priced,QRect(1980,1208,389,70),115);paint(priced,QRect(2172,1233,46,19),190);
        const int buttonChange=countdownLevelDifference(countdownInk(publicity,PurchaseBuyButtonRect),countdownInk(priced,PurchaseBuyButtonRect));
        check(buttonChange>=PurchaseButtonChangePixels,"publicity button to green price button is a button change");
        check(countdownLevelDifference(countdownInk(publicity,PurchaseBuyButtonRect),countdownInk(publicity,PurchaseBuyButtonRect))==0,
            "a static button is no change");
        // Green fill (B,G,R ~ 110,160,30) against the dark 公示中 button.
        auto filled=frame;
        for(int y=1208;y<1278;++y)for(int x=1980;x<2369;++x){
            auto* q=reinterpret_cast<unsigned char*>(filled.pixels.data()+qint64(y)*filled.strideBytes+x*4);q[0]=110;q[1]=160;q[2]=30;}
        check(buttonGreenFraction(filled,PurchaseBuyButtonRect)>0.9 &&buttonGreenFraction(publicity,PurchaseBuyButtonRect)<0.01
            &&buttonGreenFraction(small,PurchaseBuyButtonRect)<0,"green price button fill is measured, not read");
        const auto mean=rectMeanBgr(filled,QRect(1980,1208,389,70));
        check(mean.size()==3 &&std::abs(mean[0].toDouble()-110)<0.01 &&std::abs(mean[1].toDouble()-160)<0.01
            &&rectMeanBgr(small,PurchaseBuyButtonRect).isEmpty(),"button mean colour is reported for calibration");
        CountdownArea area;
        check(countdownArea(QString(),&area) &&area.name=="footer" &&area.line==PurchaseCountdownRoi
            &&countdownArea("dialog",&area) &&area.line==PurchaseDialogCountdownRoi &&area.button==PurchaseDialogButtonRect
            &&countdownArea("toast",&area) &&area.line==PurchaseToastRect &&area.line==QRect(896,150,768,150)
            &&!countdownArea("anywhere",&area),"countdown areas are footer, dialog or the result toast only");
        check(countdownStrongDifference(countdownInk(publicity,PurchaseCountdownRoi),countdownInk(priced,PurchaseCountdownRoi))==0,
            "the button is outside the countdown line");
        const auto pair=purchaseClockPair();
        check(pair["unix_ms"].toDouble()>1.7e12 &&pair["qpc_ms"].toDouble()>0 &&pair["read_span_ms"].toDouble()<50,"clock pair reads both clocks");
    }
    std::printf("PURCHASE_OBSERVATION_TESTS=%s; assertions=%d; failures=%d; synthetic_input=true; game_input=false\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
