#include "numeric_roi.h"
#include "collection_ocr_fields.h"
#include <QJsonArray>
#include <QRectF>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <limits>

namespace relink::vision {
namespace {
QJsonArray rect(const QRect& r){return {r.x(),r.y(),r.width(),r.height()};}
struct Component {QRect box;QVector<int> pixels;};
int percentile(const QVector<int>& histogram,int total,double fraction){
    int count=0;const int target=std::max(1,int(std::ceil(total*fraction)));
    for(int i=0;i<histogram.size();++i){count+=histogram[i];if(count>=target)return i;}return 255;
}
int longestRun(const QVector<unsigned char>& mask,int start,int step,int length){
    int current=0,best=0;for(int i=0;i<length;++i){current=mask[start+i*step]?current+1:0;best=std::max(best,current);}return best;
}
}
PreparedNumericRegion prepareNumericRegion(const runtime::observation::FrameEnvelope& frame,const QRect& region,int scale,int padding){
    PreparedNumericRegion out;out.sourceWidth=frame.width;out.sourceHeight=frame.height;out.sourceRegion=region;out.scale=scale;out.paddingPixels=padding*scale;
    out.metadata={{"schema","numeric-roi-v1"},{"frame_id",frame.frameId},{"same_frame",true},
        {"source_region",rect(region)},{"scale",scale},{"padding_source_pixels",padding},
        {"image_file_writes",0},{"method","observed_components_edge_line_removal_white_padding"}};
    const auto fail=[&](const QString& error){out.error=error;out.metadata["error"]=error;return out;};
    if(frame.frameId.isEmpty() ||frame.width<1 ||frame.height<1 ||frame.pixelFormat!="BGRA8"
        ||frame.strideBytes!=frame.width*4 ||frame.validBytes!=qint64(frame.strideBytes)*frame.height
        ||frame.pixels.size()!=frame.validBytes ||region.width()<12 ||region.height()<12
        ||!QRect(0,0,frame.width,frame.height).contains(region) ||scale<1 ||scale>3 ||padding<2 ||padding>32
        ||qint64(region.width()+2*padding)*scale>4096 ||qint64(region.height()+2*padding)*scale>4096)
        return fail(QStringLiteral("E_NUMERIC_REGION"));
    const int w=region.width(),h=region.height(),n=w*h;
    QVector<int> gray(n),histogram(256,0);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        const auto* p=reinterpret_cast<const unsigned char*>(frame.pixels.constData())+qint64(y+region.y())*frame.strideBytes+(x+region.x())*4;
        const int value=(int(p[0])+2*int(p[1])+int(p[2]))/4;
        gray[y*w+x]=value;++histogram[value];
    }
    const int background=percentile(histogram,n,.25),highlight=percentile(histogram,n,.995);
    out.metadata["background_level"]=background;out.metadata["highlight_level"]=highlight;
    if(highlight-background<24)return fail(QStringLiteral("E_NUMERIC_CONTRAST"));
    const int threshold=background+std::max(12,int(std::round((highlight-background)*.45)));
    out.metadata["ink_threshold"]=threshold;
    QVector<unsigned char> mask(n,0);for(int i=0;i<n;++i)mask[i]=gray[i]>=threshold;
    QVector<int> lineRows,lineColumns;
    for(int y=0;y<h;++y)if((y<4 ||y>=h-4) &&longestRun(mask,y*w,1,w)>=w*.75)lineRows.append(y);
    for(int x=0;x<w;++x)if((x<4 ||x>=w-4) &&longestRun(mask,x,w,h)>=h*.75)lineColumns.append(x);
    int removed=0;
    for(const int y:lineRows)for(int x=0;x<w;++x){removed+=mask[y*w+x]!=0;mask[y*w+x]=0;}
    for(const int x:lineColumns)for(int y=0;y<h;++y){removed+=mask[y*w+x]!=0;mask[y*w+x]=0;}
    out.metadata["edge_line_pixels_removed"]=removed;
    out.metadata["edge_line_row_count"]=lineRows.size();out.metadata["edge_line_column_count"]=lineColumns.size();
    QVector<unsigned char> visited(n,0);QVector<Component> components;
    for(int seed=0;seed<n;++seed){if(!mask[seed]||visited[seed])continue;
        Component component;QVector<int> pending{seed};visited[seed]=1;
        int l=seed%w,r=l,t=seed/w,b=t;
        while(!pending.isEmpty()){
            const int at=pending.takeLast(),x=at%w,y=at/w;component.pixels.append(at);
            l=std::min(l,x);r=std::max(r,x);t=std::min(t,y);b=std::max(b,y);
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int xx=x+dx,yy=y+dy;
                if(xx<0||xx>=w||yy<0||yy>=h)continue;
                const int next=yy*w+xx;
                if(mask[next]&&!visited[next]){visited[next]=1;pending.append(next);}
            }
        }
        component.box=QRect(l,t,r-l+1,b-t+1);
        if(component.pixels.size()>=2 &&component.box.height()>=2
            &&component.box.height()<=h*.82 &&component.box.width()<=w*.6)components.append(component);
    }
    int anchor=-1;
    for(int i=0;i<components.size();++i){const auto& box=components[i].box;
        if(box.height()>=6 &&(anchor<0 ||box.right()>components[anchor].box.right()))anchor=i;}
    if(anchor<0)return fail(QStringLiteral("E_NUMERIC_INK_MISSING"));
    const QRect anchorBox=components[anchor].box;
    if(anchorBox.right()<w-19)return fail(QStringLiteral("E_NUMERIC_RIGHT_ANCHOR"));
    QVector<int> selected{anchor};QVector<unsigned char> used(components.size(),0);used[anchor]=1;
    QRect ink=anchorBox;const int maxGap=std::max(4,int(std::round(anchorBox.height()*.45)));
    bool added=true;
    while(added){added=false;
        for(int i=0;i<components.size();++i){if(used[i])continue;const auto& box=components[i].box;
            const int verticalOverlap=std::min(box.bottom(),anchorBox.bottom())-std::max(box.top(),anchorBox.top())+1;
            const bool main=box.height()>=anchorBox.height()*.5 &&box.height()<=anchorBox.height()*1.5
                &&verticalOverlap>=std::min(box.height(),anchorBox.height())*.5;
            const bool punctuation=box.height()<anchorBox.height()*.5 &&std::abs(box.bottom()-anchorBox.bottom())<=4;
            if((main||punctuation) &&box.right()>=ink.left()-maxGap-1 &&box.left()<=ink.right()+1
                &&box.right()<=ink.right()){
                used[i]=1;selected.append(i);ink=ink.united(box);added=true;
            }
        }
    }
    if(ink.left()<=0 ||ink.right()>=w-1 ||ink.top()<=0 ||ink.bottom()>=h-1)
        return fail(QStringLiteral("E_NUMERIC_CLIPPED_INK"));
    QJsonArray componentBounds;QVector<QRect> tall;
    QVector<unsigned char> chosen(n,0);
    for(const int index:selected){const auto& component=components[index];
        componentBounds.append(rect(component.box.translated(region.topLeft())));
        if(component.box.height()>=anchorBox.height()*.5)tall.append(component.box);
        for(const int pixel:component.pixels)chosen[pixel]=1;
    }
    std::sort(tall.begin(),tall.end(),[](const QRect& a,const QRect& b){return a.left()<b.left();});
    int glyphs=0,right=-1;for(const auto& box:tall){if(box.left()>right){++glyphs;right=box.right();}else right=std::max(right,box.right());}
    // A raw x-span count is NOT a digit count: a broken zero can leave two
    // disconnected tall stems. Only broad, nearly full-height, aligned ink
    // components of the supported separated price font contribute a lower
    // bound. Thin fragments and wide joined glyphs add no count. This remains
    // a calibrated price-font contract, not a guarantee for arbitrary fonts.
    QVector<QRect> reliable;
    if(!tall.isEmpty()){
        const auto reference=*std::max_element(tall.begin(),tall.end(),[](const QRect& a,const QRect& b){return a.height()<b.height();});
        for(const int index:selected){const auto& component=components[index];const auto& box=component.box;
            if(box.height()>=reference.height()*.85 &&box.width()>=reference.height()*.35
                &&box.width()<=reference.height()*1.25 &&std::abs(box.top()-reference.top())<=2
                &&std::abs(box.bottom()-reference.bottom())<=2 &&component.pixels.size()>=box.width()*box.height()*.15)
                reliable.append(box);
        }
    }
    std::sort(reliable.begin(),reliable.end(),[](const QRect& a,const QRect& b){return a.left()<b.left();});
    QVector<QRect> reliableGroups;
    for(const auto& box:reliable){
        if(reliableGroups.isEmpty() ||box.left()>reliableGroups.last().right()+2)reliableGroups.append(box);
        else reliableGroups.last()=reliableGroups.last().united(box);
    }
    QJsonArray reliableBounds;for(const auto& box:reliableGroups)reliableBounds.append(rect(box.translated(region.topLeft())));
    out.metadata["reliable_digit_span_minimum"]=reliableGroups.size();
    out.metadata["reliable_digit_span_bounds"]=reliableBounds;
    out.metadata["digit_span_contract"]="supported_separated_price_font_not_general_ocr";
    out.inkBounds=ink.translated(region.topLeft());
    const QRect crop=ink.adjusted(-1,-1,1,1).intersected(QRect(0,0,w,h));out.cropBounds=crop.translated(region.topLeft());
    out.frame=frame;out.frame.width=(crop.width()+2*padding)*scale;out.frame.height=(crop.height()+2*padding)*scale;
    out.frame.strideBytes=out.frame.width*4;out.frame.validBytes=qint64(out.frame.strideBytes)*out.frame.height;
    out.frame.pixels=QByteArray(out.frame.validBytes,char(255));
    const int fadeFloor=std::max(background,threshold-16),range=std::max(1,highlight-fadeFloor);
    for(int y=crop.top();y<=crop.bottom();++y)for(int x=crop.left();x<=crop.right();++x){
        bool nearInk=false;
        for(int dy=-1;dy<=1&&!nearInk;++dy)for(int dx=-1;dx<=1;++dx){const int xx=x+dx,yy=y+dy;
            if(xx>=0&&xx<w&&yy>=0&&yy<h&&chosen[yy*w+xx]){nearInk=true;break;}}
        if(!nearInk)continue;
        const int value=255-std::clamp((gray[y*w+x]-fadeFloor)*255/range,0,255);
        for(int dy=0;dy<scale;++dy)for(int dx=0;dx<scale;++dx){
            const int xx=(x-crop.x()+padding)*scale+dx,yy=(y-crop.y()+padding)*scale+dy;
            auto* p=reinterpret_cast<unsigned char*>(out.frame.pixels.data())+qint64(yy)*out.frame.strideBytes+xx*4;
            p[0]=p[1]=p[2]=static_cast<unsigned char>(value);p[3]=255;
        }
    }
    out.metadata["ink_bounds"]=rect(out.inkBounds);out.metadata["crop_bounds"]=rect(out.cropBounds);
    out.metadata["component_bounds"]=componentBounds;out.metadata["glyph_span_count"]=glyphs;
    out.metadata["prepared_size"]=QJsonArray{out.frame.width,out.frame.height};out.metadata["padding_prepared_pixels"]=out.paddingPixels;
    out.metadata["source_frame_bytes_unchanged"]=true;out.ok=true;return out;
}
QJsonObject mapNumericRegionObservation(const PreparedNumericRegion& prepared,const QJsonObject& observation,QString* error){
    const auto setError=[&](const QString& value){if(error)*error=value;};setError({});
    QJsonObject out=observation;out["numeric_preprocess"]=prepared.metadata;out["coordinate_space"]="prepared_numeric_px";
    if(!prepared.ok){setError(prepared.error);return out;}
    if(observation["coordinate_space"]!="client_physical_px" ||observation["frame_id"]!=prepared.frame.frameId
        ||observation["width"].toInt()!=prepared.frame.width ||observation["height"].toInt()!=prepared.frame.height){
        setError(QStringLiteral("E_OCR_NUMERIC_MAPPING"));return out;}
    QJsonArray words;QRectF unionBox;bool first=true;
    const QRectF original(prepared.sourceRegion);
    for(const auto& value:observation["words"].toArray()){
        if(!value.isObject()){setError(QStringLiteral("E_OCR_NUMERIC_MAPPING"));return out;}
        auto word=value.toObject();
        for(const auto& name:QStringList{"x","y","width","height"})if(!word[name].isDouble() ||!std::isfinite(word[name].toDouble())){
            setError(QStringLiteral("E_OCR_NUMERIC_MAPPING"));return out;}
        if(word["width"].toDouble()<=0 ||word["height"].toDouble()<=0){setError(QStringLiteral("E_OCR_NUMERIC_MAPPING"));return out;}
        const QRectF local(word["x"].toDouble(),word["y"].toDouble(),word["width"].toDouble(),word["height"].toDouble());
        const QRectF box(prepared.cropBounds.x()+(local.x()-prepared.paddingPixels)/prepared.scale,
            prepared.cropBounds.y()+(local.y()-prepared.paddingPixels)/prepared.scale,
            local.width()/prepared.scale,local.height()/prepared.scale);
        if(!original.contains(box)){setError(QStringLiteral("E_OCR_NUMERIC_OUTSIDE_ROI"));return out;}
        word["x"]=box.x();word["y"]=box.y();word["width"]=box.width();word["height"]=box.height();words.append(word);
        unionBox=first?box:unionBox.united(box);first=false;
    }
    out["words"]=words;out["width"]=prepared.sourceWidth;out["height"]=prepared.sourceHeight;
    out["coordinate_space"]="client_physical_px";out["numeric_coordinate_transform"]="unpad_unscale_translate_crop";
    out["coverage"]="roi";out["roi_scale"]=prepared.scale;out["roi_preprocess"]="numeric_components_padded_black_on_white";
    out["frame_id"]=prepared.frame.frameId;
    if(words.isEmpty()){setError(QStringLiteral("E_OCR_NUMERIC_EMPTY"));return out;}
    if(!collectionPriceWordsAreNumeric(words)){setError(QStringLiteral("E_OCR_NUMERIC_TEXT"));return out;}
    int actualDigits=0;
    for(const auto& value:words){const auto text=value.toObject()["text"].toString().normalized(QString::NormalizationForm_KC);
        for(const auto ch:text)actualDigits+=ch>=QLatin1Char('0')&&ch<=QLatin1Char('9');}
    out["recognized_digit_count"]=actualDigits;
    const QRectF ink(prepared.inkBounds);
    if(unionBox.left()>ink.left()+3 ||unionBox.right()<ink.right()-3
        ||unionBox.top()>ink.top()+3 ||unionBox.bottom()<ink.bottom()-3){
        setError(QStringLiteral("E_OCR_NUMERIC_INCOMPLETE"));return out;}
    if(actualDigits<prepared.metadata["reliable_digit_span_minimum"].toInt()){
        setError(QStringLiteral("E_OCR_NUMERIC_DIGITS_MISSING"));return out;
    }
    return out;
}
} // namespace relink::vision
