#include "mainwindow.h"
#include "domain.h"
#include "widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStyle>
#include <QSplitter>
#include <QScrollBar>
#include <QTimer>
#include <QResizeEvent>
#include <QGraphicsDropShadowEffect>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
const QColor mint("#0F6CBD");
const QColor muted("#727780");
const QStringList titles = {QStringLiteral("工作台"), QStringLiteral("我的关注"), QStringLiteral("自动任务"), QStringLiteral("价格中心"), QStringLiteral("运行统计"), QStringLiteral("运行日志"), QStringLiteral("设置")};
const QStringList subtitles = {
    QStringLiteral("关注价格与任务状态"),
    QStringLiteral("管理关注条目，查看报价与关联任务"),
    QStringLiteral("管理筛选条件、数量限制与运行状态"),
    QStringLiteral("报价趋势与历史样本"),
    QStringLiteral("本次运行的筛选与确认结果"),
    QStringLiteral("查找运行事件与异常记录"),
    QStringLiteral("工作空间与显示偏好")
};

QLabel* label(const QString& text, const QString& name = QString()) {
    auto* item = new QLabel(text);
    if (!name.isEmpty()) item->setObjectName(name);
    return item;
}
QPushButton* button(const QString& text, const QString& name, bool primary = false) {
    auto* item = new QPushButton(text);
    item->setObjectName(name);
    item->setProperty("primary", primary);
    item->setCursor(Qt::PointingHandCursor);
    item->setMinimumHeight(36);
    return item;
}
QFrame* panel(const QString& name = QStringLiteral("panel")) {
    auto* item = new QFrame;
    item->setObjectName(name);
    item->setProperty("panel", true);
    return item;
}
QVBoxLayout* pageLayout(QWidget* page) {
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(20);
    return layout;
}
QTableWidget* table(const QStringList& columns, const QString& name) {
    auto* item = new QTableWidget(0, columns.size());
    item->setObjectName(name);
    item->setHorizontalHeaderLabels(columns);
    item->setSelectionBehavior(QAbstractItemView::SelectRows);
    item->setSelectionMode(QAbstractItemView::SingleSelection);
    item->setEditTriggers(QAbstractItemView::NoEditTriggers);
    item->setShowGrid(false);
    item->setAlternatingRowColors(false);
    item->setWordWrap(false);
    item->setFrameShape(QFrame::NoFrame);
    item->verticalHeader()->setVisible(false);
    item->verticalHeader()->setDefaultSectionSize(48);
    item->horizontalHeader()->setHighlightSections(false);
    item->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    item->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    item->horizontalHeader()->setStretchLastSection(true);
    item->horizontalHeader()->setMinimumHeight(36);
    item->setMinimumHeight(130);
    for (int column=0; column<columns.size(); ++column) {
        const QString& title=columns[column];
        if (title.contains(QStringLiteral("报价")) || title.contains(QStringLiteral("价格")) ||
            title==QStringLiteral("数量") || title==QStringLiteral("最大磨损")) {
            item->horizontalHeaderItem(column)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    return item;
}
void put(QTableWidget* table, int row, int column, const QString& text, const QColor& color = QColor(), const QString& id = QString()) {
    auto* item = new QTableWidgetItem(text);
    if (color.isValid()) item->setForeground(color);
    if (!id.isEmpty()) item->setData(Qt::UserRole, id);
    const QString heading=table->horizontalHeaderItem(column)->text();
    if (heading.contains(QStringLiteral("报价")) || heading.contains(QStringLiteral("价格")) ||
        heading==QStringLiteral("数量") || heading==QStringLiteral("最大磨损")) {
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QFont numberFont(QStringLiteral("Segoe UI")); numberFont.setPixelSize(14);
        item->setFont(numberFont);
    }
    table->setItem(row, column, item);
}
QString amount(double value) { return QLocale(QLocale::English).toString(value, 'f', 2); }
QString changeText(double change) { return QStringLiteral("%1%2%").arg(change >= 0 ? "+" : "").arg(change, 0, 'f', 2); }
QString selectedId(QTableWidget* view) {
    if (!view || view->currentRow() < 0) return {};
    for (int c = 0; c < view->columnCount(); ++c) {
        if (auto* item = view->item(view->currentRow(), c)) {
            if (!item->data(Qt::UserRole).toString().isEmpty()) return item->data(Qt::UserRole).toString();
        }
    }
    return {};
}
const Skin* findSkin(AppState* state, const QString& id) {
    for (const auto& skin : state->skins) if (skin.id == id) return &skin;
    return nullptr;
}
QVector<double> series(double price) {
    // Deterministic synthetic samples, not observed market history.
    static const double ratios[] = {.83,.89,.94,.91,.86,.87,.92,1.0,1.10,1.03,.94,.89,.94,.98,.93,.88,.90,.94,.90,.97,.92,.94,.96,1.0};
    QVector<double> values;
    const double phase=std::fmod(price,17.0)*0.34;
    for(int i=0;i<24;++i)values.append(price*(ratios[i]+(i==23?0:0.025*std::sin(0.85*i+phase))));
    return values;
}
QString skinCategory(const QString& name) {
    const QString base=name.section(' ',0,0);
    if(base=="AWM"||base=="M700")return "sniper";
    if(base=="M249")return "lmg";
    if(base=="Vector"||base=="MP5"||base=="P90"||base=="SR-3M")return "smg";
    return "rifle";
}

int skinArtIndex(const Skin& skin) {
    const QStringList names={"AUG","M4A1","AKM","AWM","Vector","SCAR-H","MP5","G3","M700","P90","SR-3M","M249"};
    return names.indexOf(skin.name.section(' ',0,0));
}

QPixmap skinArtwork(int index) {
    static QVector<QPixmap> sprites;
    if(sprites.isEmpty()) {
        const QImage atlas(":/assets/demo_skin_atlas.png");
        if(atlas.isNull())return {};
        for(int i=0;i<12;++i) {
            const int left=(i%4)*atlas.width()/4,right=(i%4+1)*atlas.width()/4;
            const int top=(i/4)*atlas.height()/3,bottom=(i/4+1)*atlas.height()/3;
            const QImage cell=atlas.copy(left,top,right-left,bottom-top);
            int x0=cell.width(),y0=cell.height(),x1=-1,y1=-1;
            for(int y=0;y<cell.height();++y)for(int x=0;x<cell.width();++x)if(qAlpha(cell.pixel(x,y))>64){x0=qMin(x0,x);y0=qMin(y0,y);x1=qMax(x1,x);y1=qMax(y1,y);}
            // Texture coordinates trim transparent padding at render time;
            // the original generated atlas is preserved byte-for-byte.
            sprites.append(x1>=x0?QPixmap::fromImage(cell.copy(QRect(QPoint(x0,y0),QPoint(x1,y1)))):QPixmap());
        }
    }
    return index>=0&&index<sprites.size()?sprites[index]:QPixmap();
}

class WatchlistDelegate final : public QStyledItemDelegate {
public:
    explicit WatchlistDelegate(QObject* parent):QStyledItemDelegate(parent){}
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);initStyleOption(&opt,index);
        painter->save();painter->setClipRect(option.rect);painter->setRenderHint(QPainter::Antialiasing);painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->fillRect(option.rect,QColor("#FFFFFF"));
        const bool selected=option.state&QStyle::State_Selected;
        const bool hovered=option.state&QStyle::State_MouseOver;
        if(selected||hovered) {
            const auto* view=qobject_cast<const QTableWidget*>(parent());
            const QRectF backplate(view?view->columnViewportPosition(0)+2:option.rect.x(),option.rect.y()+3,
                view?view->horizontalHeader()->length()-4:option.rect.width(),option.rect.height()-6);
            painter->setPen(Qt::NoPen);painter->setBrush(QColor(selected?"#EAF2FB":"#F5F6F8"));
            painter->drawRoundedRect(backplate,6,6);
            if(selected&&index.column()==0){painter->setBrush(QColor("#0F6CBD"));painter->drawRoundedRect(QRectF(backplate.left(),backplate.center().y()-10,3,20),1.5,1.5);}
        }
        if(index.column()==1) {
            const QRect r=option.rect.adjusted(8,2,-8,-2);const int artWidth=qBound(62,r.width()/3,102);
            const QPixmap art=skinArtwork(index.data(Qt::UserRole+2).toInt());
            if(!art.isNull()){
                const QSize scaled=art.size().scaled(QSize(artWidth,r.height()-8),Qt::KeepAspectRatio);
                const QRect dst(r.x()+(artWidth-scaled.width())/2,r.y()+(r.height()-scaled.height())/2,scaled.width(),scaled.height());
                painter->drawPixmap(dst,art);
            }
            const int tx=r.x()+artWidth+12;const int tw=qMax(0,r.right()-tx);
            QFont nameFont(QStringLiteral("Microsoft YaHei UI"));nameFont.setPixelSize(13);painter->setFont(nameFont);painter->setPen(QColor("#262A30"));
            painter->drawText(QRect(tx,r.center().y()-17,tw,20),Qt::AlignVCenter,painter->fontMetrics().elidedText(index.data().toString(),Qt::ElideRight,tw));
            nameFont.setPixelSize(10);painter->setFont(nameFont);painter->setPen(QColor("#7E848C"));
            painter->drawText(QRect(tx,r.center().y()+4,tw,16),Qt::AlignVCenter,painter->fontMetrics().elidedText(index.data(Qt::UserRole+1).toString(),Qt::ElideRight,tw));
        } else if(index.column()==4) {
            const QVariantList points=index.data(Qt::UserRole+3).toList();
            if(points.size()>1){double lo=points[0].toDouble(),hi=lo;for(const auto& v:points){lo=qMin(lo,v.toDouble());hi=qMax(hi,v.toDouble());}
                const QRectF r=option.rect.adjusted(14,19,-14,-19);QPainterPath path;
                for(int i=0;i<points.size();++i){QPointF p(r.left()+r.width()*i/(points.size()-1),r.bottom()-(points[i].toDouble()-lo)/qMax(1.0,hi-lo)*r.height());if(i==0)path.moveTo(p);else path.lineTo(p);}
                painter->setPen(QPen(QColor("#669BCB"),1.1));painter->drawPath(path);
                painter->setBrush(QColor("#0F6CBD"));painter->drawEllipse(path.currentPosition(),2,2);
            }
        } else {
            painter->setFont(opt.font);
            const QVariant foreground=index.data(Qt::ForegroundRole);
            painter->setPen(foreground.canConvert<QBrush>()?foreground.value<QBrush>().color():QColor("#414851"));
            const QRect textRect=option.rect.adjusted(7,0,-7,0);
            const int alignment=index.data(Qt::TextAlignmentRole).isValid()?index.data(Qt::TextAlignmentRole).toInt():(Qt::AlignLeft|Qt::AlignVCenter);
            painter->drawText(textRect,alignment,painter->fontMetrics().elidedText(index.data().toString(),Qt::ElideRight,textRect.width()));
        }
        painter->restore();
    }
};

QHBoxLayout* sectionHeader(QVBoxLayout* layout, const QString& title, const QString& caption = QString()) {
    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    row->addWidget(label(title, "sectionTitle"));
    row->addStretch();
    if (!caption.isEmpty()) row->addWidget(label(caption, "mutedLabel"));
    layout->addLayout(row);
    return row;
}
QVBoxLayout* panelLayout(QFrame* frame, int margin = 22) {
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(14);
    return layout;
}

QIcon outlineIcon(int shape, const QColor& normal = muted, const QColor& selected = mint) {
    QIcon icon;
    const auto draw = [shape](const QColor& color) {
        QPixmap pixels(36, 36);
        pixels.setDevicePixelRatio(2.0);
        pixels.fill(Qt::transparent);
        QPainter painter(&pixels);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        switch (shape) {
        case 0:
            for (int y : {2, 10}) for (int x : {2, 10}) painter.drawRoundedRect(QRectF(x, y, 5, 5), 0.8, 0.8);
            break;
        case 1: {
            QPainterPath star;
            constexpr double pi = 3.14159265358979323846;
            for (int i = 0; i < 10; ++i) {
                const double angle = -pi / 2 + i * pi / 5;
                const double radius = i % 2 == 0 ? 7.2 : 3.25;
                const QPointF point(9 + radius * std::cos(angle), 9 + radius * std::sin(angle));
                if (i == 0) star.moveTo(point); else star.lineTo(point);
            }
            star.closeSubpath();
            painter.drawPath(star);
            break;
        }
        case 2: {
            QPainterPath play;
            play.moveTo(5, 2.5); play.lineTo(14.5, 9); play.lineTo(5, 15.5); play.closeSubpath();
            painter.drawPath(play);
            break;
        }
        case 3: {
            painter.drawLine(QPointF(2, 2), QPointF(2, 15.5));
            painter.drawLine(QPointF(2, 15.5), QPointF(16, 15.5));
            QPainterPath trend;
            trend.moveTo(4, 12); trend.lineTo(7, 8); trend.lineTo(10, 10); trend.lineTo(15, 4);
            painter.drawPath(trend);
            break;
        }
        case 4:
            painter.drawRoundedRect(QRectF(2, 9, 3, 6), 0.6, 0.6);
            painter.drawRoundedRect(QRectF(7.5, 5, 3, 10), 0.6, 0.6);
            painter.drawRoundedRect(QRectF(13, 2, 3, 13), 0.6, 0.6);
            break;
        case 5:
            for (int y : {4, 9, 14}) {
                painter.drawPoint(QPointF(2.5, y));
                painter.drawLine(QPointF(6, y), QPointF(15.5, y));
            }
            break;
        case 6:
            painter.drawEllipse(QPointF(9, 9), 4.8, 4.8);
            painter.drawEllipse(QPointF(9, 9), 1.7, 1.7);
            painter.translate(9, 9);
            for (int i = 0; i < 8; ++i) {
                painter.drawLine(QPointF(0, -5.2), QPointF(0, -7.2));
                painter.rotate(45);
            }
            break;
        default:
            painter.drawLine(QPointF(6, 3.5), QPointF(6, 14.5));
            painter.drawLine(QPointF(12, 3.5), QPointF(12, 14.5));
            break;
        }
        return pixels;
    };
    icon.addPixmap(draw(normal), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(selected), QIcon::Normal, QIcon::On);
    icon.addPixmap(draw(selected), QIcon::Active, QIcon::Off);
    icon.addPixmap(draw(selected), QIcon::Active, QIcon::On);
    icon.addPixmap(draw(QColor("#56646D")), QIcon::Disabled, QIcon::Off);
    return icon;
}

QScrollArea* scrollablePage(QWidget* page, int minimumHeight) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(page->objectName() + "ScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    page->setMinimumHeight(minimumHeight);
    scroll->setWidget(page);
    return scroll;
}
}

MainWindow::MainWindow(AppState* state, QWidget* parent) : QMainWindow(parent), m_state(state) {
    setObjectName("mainWindow");
    setWindowTitle(QStringLiteral("Relink · 本地演示"));
    resize(1560, 980);
    setMinimumSize(1180, 820);
    setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    auto* central = new QWidget; central->setObjectName("terminalShell"); setCentralWidget(central);
    auto* shell = new QVBoxLayout(central); shell->setContentsMargins(0,0,0,0); shell->setSpacing(0);

    auto* top = new QFrame; top->setObjectName("terminalNavigation"); top->setFixedHeight(64);
    auto* navigation = new QHBoxLayout(top); navigation->setContentsMargins(22,10,22,10); navigation->setSpacing(4);
    auto* mark=label("R","fluentBrandMark");mark->setAlignment(Qt::AlignCenter);mark->setFixedSize(28,28);navigation->addWidget(mark);navigation->addSpacing(7);
    auto* brand=label("Relink", "terminalBrand"); brand->setFixedWidth(76); navigation->addWidget(brand);
    const QStringList navText={QStringLiteral("工作台"),QStringLiteral("关注"),QStringLiteral("任务"),QStringLiteral("价格"),QStringLiteral("统计"),QStringLiteral("日志"),QStringLiteral("设置")};
    const QStringList navNames={"navOverview","navFavorites","navTasks","navPrices","navStats","navLogs","navSettings"};
    m_nav.resize(7);
    for(int i : {1,2,3,0,4,5,6}) {
        auto* nav=button(navText[i],navNames[i]); nav->setProperty("nav",true); nav->setCheckable(true);
        nav->setFixedSize(72,36); navigation->addWidget(nav); m_nav[i]=nav;
        connect(nav,&QPushButton::clicked,this,[this,i]{setPage(i);});
    }
    navigation->addStretch();
    auto* import=button(QStringLiteral("导入"),"importConfigButton");
    auto* exportButton=button(QStringLiteral("导出"),"exportConfigButton");
    import->setProperty("quietLink",true); exportButton->setProperty("quietLink",true);
    navigation->addWidget(import); navigation->addWidget(exportButton); navigation->addSpacing(18);
    navigation->addWidget(label(QStringLiteral("本地演示"),"demoBadge")); shell->addWidget(top);

    m_pageHeader=new QFrame; m_pageHeader->setObjectName("pageHeader");
    auto* heading=new QHBoxLayout(m_pageHeader); heading->setContentsMargins(24,18,24,12);
    m_pageTitle=label(QString(),"pageTitle"); m_pageSubtitle=label(QString(),"pageSubtitle");
    heading->addWidget(m_pageTitle); heading->addSpacing(16); heading->addWidget(m_pageSubtitle); heading->addStretch();
    shell->addWidget(m_pageHeader);
    m_pages=new QStackedWidget; m_pages->setObjectName("pageStack");
    auto framedPage=[](QWidget* page,int minimum) {
        if(page->layout())page->layout()->setContentsMargins(24,8,24,20);
        return scrollablePage(page,minimum);
    };
    m_pages->addWidget(framedPage(buildOverview(),650));
    m_pages->addWidget(buildFavorites());
    m_pages->addWidget(framedPage(buildTasks(),585));
    m_pages->addWidget(framedPage(buildPrices(),710));
    m_pages->addWidget(framedPage(buildStats(),580));
    m_pages->addWidget(framedPage(buildLogs(),560));
    m_pages->addWidget(framedPage(buildSettings(),590));
    shell->addWidget(m_pages,1);
    auto* status=new QFrame; status->setObjectName("terminalStatusBar"); status->setFixedHeight(30);
    auto* footer=new QHBoxLayout(status); footer->setContentsMargins(18,0,18,0);
    m_footerStatus=label(QString(),"footerStatus"); footer->addWidget(m_footerStatus);
    footer->addWidget(label("  |  ","mutedLabel"));
    m_engineStatus=label(QString(),"engineNote");footer->addWidget(m_engineStatus);footer->addStretch();
    footer->addWidget(label(QStringLiteral("Relink 0.4  ·  本地工作空间"),"mutedLabel"));shell->addWidget(status);
    connect(import,&QPushButton::clicked,this,&MainWindow::importConfiguration);
    connect(exportButton,&QPushButton::clicked,this,&MainWindow::exportConfiguration);
    connect(m_state,&AppState::changed,this,&MainWindow::refreshAll);
    QSettings preferences;
    m_autoScrollLogs->setChecked(preferences.value("ui/autoScrollLogs",true).toBool());
    m_compactTables->setChecked(preferences.value("ui/compactTables",false).toBool());
    applyDensity();setPage(1);refreshAll();
}

QWidget* MainWindow::buildOverview() {
    auto* page = new QWidget; page->setObjectName("overviewPage");
    auto* layout = pageLayout(page);

    auto* control = new QFrame; control->setObjectName("overviewControlBar");
    auto* row = new QHBoxLayout(control); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(10);
    row->addWidget(label(QStringLiteral("今日概览"), "sectionTitle"));
    row->addWidget(label(QStringLiteral("仅本地模拟"), "mutedLabel"));
    row->addStretch();
    m_pauseButton = button(QStringLiteral("暂停"), "pauseSimulationButton");
    m_pauseButton->setIcon(outlineIcon(7));
    m_startButton = button(QStringLiteral("开始模拟"), "startSimulationButton", true);
    m_startButton->setIcon(outlineIcon(2, QColor("#FFFFFF"), QColor("#FFFFFF")));
    row->addWidget(m_pauseButton); row->addWidget(m_startButton);
    layout->addWidget(control);
    connect(m_startButton, &QPushButton::clicked, m_state, &AppState::startSimulation);
    connect(m_pauseButton, &QPushButton::clicked, m_state, &AppState::pauseSimulation);

    auto* metricStrip = new QFrame; metricStrip->setObjectName("metricStrip");
    auto* metrics = new QHBoxLayout(metricStrip); metrics->setContentsMargins(0, 8, 0, 8); metrics->setSpacing(0);
    const QStringList names = {QStringLiteral("我的关注"),QStringLiteral("启用任务"),QStringLiteral("扫描条目"),QStringLiteral("模拟确认")};
    const QStringList details = {QStringLiteral("本地关注条目"),QStringLiteral("任务队列"),QStringLiteral("本次模拟"),QStringLiteral("本次模拟")};
    for (int i=0;i<4;++i) {
        auto* card = new MetricCard(names[i], "0", details[i], mint);
        card->setObjectName(QString("overviewMetric%1").arg(i)); card->setProperty("separator", i<3);
        m_overviewCards.append(card); metrics->addWidget(card,1);
    }
    layout->addWidget(metricStrip);

    auto* middle = new QHBoxLayout; middle->setSpacing(20);
    auto* chartPanel = panel(); auto* chartLayout = panelLayout(chartPanel,24);
    sectionHeader(chartLayout, QStringLiteral("价格观察"), QStringLiteral("24 个样本"));
    m_overviewChart = new PriceChart; m_overviewChart->setObjectName("overviewPriceChart");
    m_overviewChart->setMinimumHeight(205); chartLayout->addWidget(m_overviewChart,1);
    auto* chartLink = button(QStringLiteral("查看全部价格  →"), "overviewPricesLink");
    chartLink->setProperty("quietLink",true); chartLink->setMinimumHeight(28); chartLayout->addWidget(chartLink,0,Qt::AlignRight);
    connect(chartLink,&QPushButton::clicked,this,[this]{setPage(3);});
    middle->addWidget(chartPanel, 6);

    auto* taskPanel = panel(); auto* taskLayout = panelLayout(taskPanel,20);
    auto* taskHeader = sectionHeader(taskLayout, QStringLiteral("任务队列"));
    m_overviewTaskCount = label(QString(), "mutedLabel"); taskHeader->addWidget(m_overviewTaskCount);
    m_overviewTasks = table({QStringLiteral("任务"),QStringLiteral("数量"),QStringLiteral("状态")}, "overviewTasksTable");
    m_overviewTasks->horizontalHeader()->setStretchLastSection(false);
    m_overviewTasks->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    m_overviewTasks->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Fixed); m_overviewTasks->setColumnWidth(1,48);
    m_overviewTasks->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Fixed); m_overviewTasks->setColumnWidth(2,82);
    m_overviewTasks->setMinimumHeight(220);
    taskLayout->addWidget(m_overviewTasks,1);
    auto* taskLink = button(QStringLiteral("管理任务  →"), "overviewTasksLink");
    taskLink->setProperty("quietLink",true); taskLink->setMinimumHeight(28); taskLayout->addWidget(taskLink,0,Qt::AlignRight);
    connect(taskLink,&QPushButton::clicked,this,[this]{setPage(2);});
    middle->addWidget(taskPanel,4); layout->addLayout(middle, 3);

    auto* logPanel = panel(); auto* logLayout = panelLayout(logPanel,20);
    auto* logHeader=sectionHeader(logLayout,QStringLiteral("最近活动"));
    auto* logLink=button(QStringLiteral("全部日志  →"),"overviewLogsLink");
    logLink->setProperty("quietLink",true); logLink->setMinimumHeight(24); logHeader->addWidget(logLink);
    connect(logLink,&QPushButton::clicked,this,[this]{setPage(5);});
    m_overviewLogs = table({QStringLiteral("时间"),QStringLiteral("级别"),QStringLiteral("事件")},"overviewLogsTable");
    m_overviewLogs->horizontalHeader()->hide();
    m_overviewLogs->setMinimumHeight(48); m_overviewLogs->setMaximumHeight(96);
    m_overviewLogs->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);
    logLayout->addWidget(m_overviewLogs,1); layout->addWidget(logPanel,1);
    return page;
}

QWidget* MainWindow::buildFavorites() {
    auto* page=new QWidget;page->setObjectName("favoritesPage");
    auto* layout=new QHBoxLayout(page);layout->setContentsMargins(12,8,18,8);layout->setSpacing(12);
    auto* directory=new QFrame;directory->setObjectName("catalogDirectory");directory->setFixedWidth(144);
    auto* dir=new QVBoxLayout(directory);dir->setContentsMargins(4,18,4,16);dir->setSpacing(4);
    const auto addFilter=[&](const QString& name,const QString& key) {
        auto* filter=button(name,"catalogFilter_"+key);filter->setProperty("catalogFilter",true);
        filter->setProperty("filterKey",key);filter->setProperty("filterTitle",name);filter->setCheckable(true);
        filter->setChecked(key=="all");filter->setFixedHeight(38);dir->addWidget(filter);m_catalogFilters.append(filter);
        connect(filter,&QPushButton::clicked,this,[this,key]{m_catalogFilter=key;refreshFavorites();});
    };
    dir->addWidget(label(QStringLiteral("关注目录"),"directoryCaption"));dir->addSpacing(9);
    addFilter(QStringLiteral("全部条目"),"all");addFilter(QStringLiteral("已关注"),"followed");addFilter(QStringLiteral("未关注"),"unfollowed");
    dir->addSpacing(23);dir->addWidget(label(QStringLiteral("按类型"),"directoryCaption"));dir->addSpacing(8);
    addFilter(QStringLiteral("步枪"),"rifle");addFilter(QStringLiteral("冲锋枪"),"smg");addFilter(QStringLiteral("狙击枪"),"sniper");addFilter(QStringLiteral("轻机枪"),"lmg");
    dir->addSpacing(23);dir->addWidget(label(QStringLiteral("任务关联"),"directoryCaption"));dir->addSpacing(8);
    addFilter(QStringLiteral("已有任务"),"linked");addFilter(QStringLiteral("未配置"),"unlinked");
    dir->addStretch();auto* sourceNote=label(QStringLiteral("市场 / 典藏外观\n我的关注\n\n12 条本地演示样本"),"directoryNote");
    sourceNote->setWordWrap(true);dir->addWidget(sourceNote);layout->addWidget(directory);

    auto* workspace=new QFrame;workspace->setObjectName("watchlistWorkspace");
    auto* work=new QVBoxLayout(workspace);work->setContentsMargins(12,10,12,12);work->setSpacing(8);
    auto* toolbar=new QFrame;toolbar->setObjectName("watchlistToolbar");toolbar->setFixedHeight(56);
    auto* tools=new QHBoxLayout(toolbar);tools->setContentsMargins(6,6,6,8);tools->setSpacing(8);
    m_favoritesCount=label(QString(),"watchlistCount");tools->addWidget(m_favoritesCount);tools->addSpacing(12);
    m_favoriteSearch=new QLineEdit;m_favoriteSearch->setObjectName("favoriteSearch");m_favoriteSearch->setPlaceholderText(QStringLiteral("搜索名称或系列…"));
    m_favoriteSearch->setClearButtonEnabled(true);m_favoriteSearch->setMinimumWidth(140);tools->addWidget(m_favoriteSearch,1);
    m_rarityFilter=new QComboBox;m_rarityFilter->setObjectName("favoriteRarity");m_rarityFilter->addItem(QStringLiteral("全部品级"));m_rarityFilter->setFixedWidth(120);tools->addWidget(m_rarityFilter);
    m_favoriteStateFilter=new QComboBox;m_favoriteStateFilter->setObjectName("favoriteTaskFilter");
    m_favoriteStateFilter->addItems({QStringLiteral("全部任务"),QStringLiteral("已关联"),QStringLiteral("未关联")});m_favoriteStateFilter->setFixedWidth(120);tools->addWidget(m_favoriteStateFilter);
    work->addWidget(toolbar);
    m_favoritesTable=table({QStringLiteral("关注"),QStringLiteral("物品"),QStringLiteral("磨损"),QStringLiteral("演示报价"),QStringLiteral("样本趋势"),QStringLiteral("目标价格"),QStringLiteral("任务状态")},"favoriteTable");
    m_favoritesTable->setItemDelegate(new WatchlistDelegate(m_favoritesTable));
    m_favoritesTable->horizontalHeader()->setStretchLastSection(false);
    m_favoritesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_favoritesTable->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    m_favoritesTable->setColumnWidth(0,40);m_favoritesTable->setColumnWidth(2,64);m_favoritesTable->setColumnWidth(3,86);
    m_favoritesTable->setColumnWidth(4,132);m_favoritesTable->setColumnWidth(5,86);m_favoritesTable->setColumnWidth(6,92);
    m_favoritesTable->verticalHeader()->setDefaultSectionSize(62);m_favoritesTable->setMinimumHeight(235);m_favoritesTable->setMouseTracking(true);
    work->addWidget(m_favoritesTable,1);
    auto* chartPanel=new QFrame;chartPanel->setObjectName("watchlistChartPanel");chartPanel->setFixedHeight(230);
    auto* chartLayout=new QVBoxLayout(chartPanel);chartLayout->setContentsMargins(14,12,14,10);chartLayout->setSpacing(0);
    m_favoriteChart=new PriceChart;m_favoriteChart->setObjectName("favoritePriceChart");chartLayout->addWidget(m_favoriteChart,1);
    work->addWidget(chartPanel);layout->addWidget(workspace,1);

    auto* inspector=new QFrame;inspector->setObjectName("terminalInspector");inspector->setFixedWidth(278);
    auto* detail=new QVBoxLayout(inspector);detail->setContentsMargins(20,22,20,18);detail->setSpacing(12);
    m_favoriteTitle=label(QString(),"detailTitle");m_favoriteTitle->setWordWrap(true);detail->addWidget(m_favoriteTitle);
    m_favoriteMetadata=label(QString(),"mutedLabel");detail->addWidget(m_favoriteMetadata);
    m_favoriteArt=label(QString(),"skinPreview");m_favoriteArt->setAlignment(Qt::AlignCenter);m_favoriteArt->setFixedHeight(148);
    m_favoriteArt->setToolTip(QStringLiteral("生成的演示插图，不代表游戏中的真实皮肤外观。"));detail->addWidget(m_favoriteArt);
    auto* quote=new QHBoxLayout;m_favoritePrice=label(QString(),"inspectorPrice");quote->addWidget(m_favoritePrice);quote->addStretch();
    m_favoriteChange=label(QString(),"favoriteChangeLabel");quote->addWidget(m_favoriteChange);detail->addLayout(quote);
    detail->addWidget(label(QStringLiteral("演示报价 · 非实时行情"),"mutedLabel"));
    auto* separator=new QFrame;separator->setObjectName("inspectorRule");separator->setFixedHeight(1);detail->addWidget(separator);
    m_favoriteFacts=label(QString(),"inspectorFacts");m_favoriteFacts->setWordWrap(true);detail->addWidget(m_favoriteFacts);
    detail->addSpacing(6);detail->addWidget(label(QStringLiteral("任务条件"),"inspectorSectionTitle"));
    auto* form=new QFormLayout;form->setHorizontalSpacing(12);form->setVerticalSpacing(10);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_inspectorPrice=new QDoubleSpinBox;m_inspectorPrice->setObjectName("inspectorMaxPrice");m_inspectorPrice->setRange(0,999999999);m_inspectorPrice->setDecimals(2);form->addRow(QStringLiteral("最高价格"),m_inspectorPrice);
    m_inspectorWear=new QDoubleSpinBox;m_inspectorWear->setObjectName("inspectorMaxWear");m_inspectorWear->setRange(0,100);m_inspectorWear->setDecimals(6);form->addRow(QStringLiteral("最大磨损"),m_inspectorWear);
    m_inspectorQuantity=new QSpinBox;m_inspectorQuantity->setObjectName("inspectorQuantity");m_inspectorQuantity->setRange(1,9999);form->addRow(QStringLiteral("数量上限"),m_inspectorQuantity);
    detail->addLayout(form);
    m_favoriteCreate=button(QStringLiteral("创建任务"),"favoriteCreateTaskButton",true);m_favoriteCreate->setFixedHeight(40);detail->addWidget(m_favoriteCreate);
    m_favoriteTaskInfo=label(QString(),"inspectorTaskInfo");m_favoriteTaskInfo->setWordWrap(true);detail->addWidget(m_favoriteTaskInfo);
    auto* manage=button(QStringLiteral("查看任务队列  →"),"favoritesTasksButton");manage->setProperty("quietLink",true);detail->addWidget(manage,0,Qt::AlignLeft);
    detail->addStretch();auto* note=label(QStringLiteral("本地模拟模式\n皮肤插图与价格均为演示素材。\n创建任务只保存筛选条件。"),"inspectorNote");note->setWordWrap(true);detail->addWidget(note);
    layout->addWidget(inspector);
    for(auto* surface : {workspace,inspector}) {
        auto* elevation=new QGraphicsDropShadowEffect(surface);
        elevation->setBlurRadius(12);elevation->setOffset(0,2);elevation->setColor(QColor(35,45,70,16));
        surface->setGraphicsEffect(elevation);
    }
    connect(m_favoriteSearch,&QLineEdit::textChanged,this,[this]{if(!m_refreshing)refreshFavorites();});
    connect(m_rarityFilter,&QComboBox::currentTextChanged,this,[this]{if(!m_refreshing)refreshFavorites();});
    connect(m_favoriteStateFilter,&QComboBox::currentIndexChanged,this,[this]{if(!m_refreshing)refreshFavorites();});
    connect(m_favoritesTable,&QTableWidget::itemSelectionChanged,this,[this]{if(!m_refreshing)refreshFavoriteDetail();});
    connect(m_favoriteCreate,&QPushButton::clicked,this,&MainWindow::createInspectorTask);
    connect(manage,&QPushButton::clicked,this,[this]{setPage(2);});
    return page;
}

QWidget* MainWindow::buildTasks() {
    auto* page=new QWidget; page->setObjectName("tasksPage"); auto* layout=pageLayout(page);
    auto* toolbar=new QHBoxLayout; toolbar->setSpacing(10);
    auto* add=button(QStringLiteral("＋  新增任务"),"taskNewButton",true);
    auto* edit=button(QStringLiteral("编辑"),"taskEditButton"); auto* remove=button(QStringLiteral("删除"),"taskDeleteButton");
    auto* enable=button(QStringLiteral("启用选中"),"enableSelectedTasksButton"); auto* disable=button(QStringLiteral("暂停选中"),"pauseSelectedTasksButton");
    m_taskStart=button(QStringLiteral("运行模拟"),"tasksSimulationButton");
    m_taskStart->setIcon(outlineIcon(2));
    toolbar->addWidget(add); toolbar->addWidget(edit); toolbar->addWidget(remove); toolbar->addStretch(); toolbar->addWidget(enable); toolbar->addWidget(disable); toolbar->addWidget(m_taskStart); layout->addLayout(toolbar);
    auto* box=panel(); auto* boxLayout=panelLayout(box,18); auto* heading=sectionHeader(boxLayout,QStringLiteral("自动任务")); m_taskCount=label(QString(),"mutedLabel"); heading->addWidget(m_taskCount);
    m_tasksTable=table({QStringLiteral("任务名称"),QStringLiteral("目标皮肤"),QStringLiteral("价格区间"),QStringLiteral("最大磨损"),QStringLiteral("数量"),QStringLiteral("启用"),QStringLiteral("状态")},"taskTable");
    m_tasksTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tasksTable->horizontalHeader()->setStretchLastSection(false);
    m_tasksTable->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    m_tasksTable->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    for(int c=2;c<7;++c)m_tasksTable->horizontalHeader()->setSectionResizeMode(c,QHeaderView::Fixed);
    m_tasksTable->setColumnWidth(2,150); m_tasksTable->setColumnWidth(3,86);
    m_tasksTable->setColumnWidth(4,52); m_tasksTable->setColumnWidth(5,52); m_tasksTable->setColumnWidth(6,112);
    boxLayout->addWidget(m_tasksTable,1);
    auto* note=label(QStringLiteral("Ctrl / Shift 多选  ·  双击编辑  ·  当前仅模拟运行，不执行购买"),"mutedLabel"); note->setWordWrap(true); boxLayout->addWidget(note); layout->addWidget(box,1);
    connect(add,&QPushButton::clicked,this,[this]{editTask();});
    connect(edit,&QPushButton::clicked,this,[this]{if(!selectedTaskId().isEmpty())editTask(selectedTaskId());});
    connect(m_tasksTable,&QTableWidget::itemDoubleClicked,this,[this](QTableWidgetItem*){if(!selectedTaskId().isEmpty())editTask(selectedTaskId());});
    connect(remove,&QPushButton::clicked,this,&MainWindow::deleteSelectedTasks);
    connect(enable,&QPushButton::clicked,this,[this]{setSelectedTasksEnabled(true);});
    connect(disable,&QPushButton::clicked,this,[this]{setSelectedTasksEnabled(false);});
    connect(m_taskStart,&QPushButton::clicked,m_state,&AppState::startSimulation);
    return page;
}

QWidget* MainWindow::buildPrices() {
    auto* page=new QWidget; page->setObjectName("pricesPage"); auto* layout=pageLayout(page);
    auto* toolbar=new QHBoxLayout; toolbar->setSpacing(12); toolbar->addWidget(label(QStringLiteral("关注目标"),"mutedLabel"));
    m_priceSkin=new QComboBox; m_priceSkin->setObjectName("pricesSkinCombo"); m_priceSkin->setMinimumWidth(330); toolbar->addWidget(m_priceSkin); toolbar->addStretch();
    auto* exportButton=button(QStringLiteral("导出 CSV"),"exportPricesButton"); toolbar->addWidget(exportButton); layout->addLayout(toolbar);
    auto* strip=new QFrame;strip->setObjectName("metricStrip");auto* metrics=new QHBoxLayout(strip);metrics->setContentsMargins(0,8,0,8);metrics->setSpacing(0);
    const QStringList names={QStringLiteral("当前演示报价"),QStringLiteral("样本最低报价"),QStringLiteral("样本最高报价")};
    for(int i=0;i<3;++i){auto* card=new MetricCard(names[i],"—",QStringLiteral("演示价格 · 非实时市场数据"),mint); card->setObjectName(QString("priceMetric%1").arg(i));card->setProperty("separator",i<2);m_priceCards.append(card);metrics->addWidget(card,1);} layout->addWidget(strip);
    auto* chartPanel=panel(); auto* chartLayout=panelLayout(chartPanel,20); sectionHeader(chartLayout,QStringLiteral("价格走势"),QStringLiteral("24 个样本 · 演示数据"));
    m_priceChart=new PriceChart; m_priceChart->setObjectName("priceHistoryChart"); m_priceChart->setMinimumHeight(160); chartLayout->addWidget(m_priceChart,1); layout->addWidget(chartPanel,3);
    auto* historyPanel=panel();auto* historyLayout=panelLayout(historyPanel,18);sectionHeader(historyLayout,QStringLiteral("样本记录"),QStringLiteral("本地合成数据"));
    m_priceHistory=table({QStringLiteral("样本"),QStringLiteral("商品"),QStringLiteral("演示价格"),QStringLiteral("类型"),QStringLiteral("数据来源")},"priceHistoryTable"); m_priceHistory->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch); historyLayout->addWidget(m_priceHistory,1);layout->addWidget(historyPanel,3);
    connect(m_priceSkin,&QComboBox::currentIndexChanged,this,[this]{if(!m_refreshing)refreshPrices();});
    connect(exportButton,&QPushButton::clicked,this,&MainWindow::exportPrices);return page;
}

QWidget* MainWindow::buildStats() {
    auto* page=new QWidget; page->setObjectName("statsPage"); auto* layout=pageLayout(page);
    auto* strip=new QFrame;strip->setObjectName("metricStrip");auto* metrics=new QHBoxLayout(strip);metrics->setContentsMargins(0,8,0,8);metrics->setSpacing(0);
    const QStringList names={QStringLiteral("模拟扫描"),QStringLiteral("模拟条件命中"),QStringLiteral("模拟确认成功"),QStringLiteral("实际成交 / 实际支出")};
    const QStringList details={QStringLiteral("本次演示扫描条目"),QStringLiteral("模拟筛选符合条件"),QStringLiteral("只计入模拟确认结果"),QStringLiteral("未连接执行模块")};
    for(int i=0;i<4;++i){auto* card=new MetricCard(names[i],i==3?"—":"0",details[i],mint);card->setObjectName(QString("statsMetric%1").arg(i));card->setProperty("separator",i<3);m_statsCards.append(card);metrics->addWidget(card,1);}layout->addWidget(strip);
    auto* content=new QHBoxLayout;content->setSpacing(18);
    auto* distribution=panel();auto* distributionLayout=panelLayout(distribution,25);sectionHeader(distributionLayout,QStringLiteral("模拟结果分布"),QStringLiteral("基于本次演示计数"));
    const QStringList reasons={QStringLiteral("未命中筛选条件"),QStringLiteral("已命中 · 尚未模拟确认"),QStringLiteral("模拟确认成功")};
    for(const auto& reason:reasons){distributionLayout->addSpacing(13);auto* row=new QHBoxLayout;row->addWidget(label(reason));row->addStretch();auto* value=label("0","mutedLabel");m_reasonLabels.append(value);row->addWidget(value);distributionLayout->addLayout(row);auto* bar=new QProgressBar;bar->setRange(0,100);bar->setValue(0);bar->setTextVisible(false);m_reasonBars.append(bar);distributionLayout->addWidget(bar);}distributionLayout->addStretch();
    m_statsNote=label(QString(),"mutedLabel");m_statsNote->setWordWrap(true);distributionLayout->addWidget(m_statsNote);content->addWidget(distribution,3);
    auto* semantics=panel();auto* sem=panelLayout(semantics,25);sem->addWidget(label(QStringLiteral("数据说明"), "eyebrow"));sem->addWidget(label(QStringLiteral("统计口径"),"detailTitle"));
    const QVector<QPair<QString,QString>> explanations={
        {QStringLiteral("01  扫描 ≠ 命中"),QStringLiteral("扫描记录观察次数；命中记录符合任务条件的次数。")},
        {QStringLiteral("02  尝试 ≠ 成交"),QStringLiteral("当前只有本地模拟，不产生实际交易和实际支出。")},
        {QStringLiteral("03  未决结果单独记"),QStringLiteral("接入执行模块后，结果未明的动作不计入成功。当前演示未决为 0。")}
    };
    for(const auto& entry:explanations){sem->addSpacing(15);sem->addWidget(label(entry.first,"sectionTitle"));auto* description=label(entry.second,"mutedLabel");description->setWordWrap(true);sem->addWidget(description);}sem->addStretch();content->addWidget(semantics,2);layout->addLayout(content,1);return page;
}

QWidget* MainWindow::buildLogs() {
    auto* page=new QWidget;page->setObjectName("logsPage");auto* layout=pageLayout(page);auto* toolbar=new QHBoxLayout;toolbar->setSpacing(12);
    m_logSearch=new QLineEdit;m_logSearch->setObjectName("logSearch");m_logSearch->setPlaceholderText(QStringLiteral("搜索事件内容…"));m_logSearch->setClearButtonEnabled(true);toolbar->addWidget(m_logSearch,1);
    m_logLevel=new QComboBox;m_logLevel->setObjectName("logLevel");m_logLevel->addItems({QStringLiteral("全部级别"),"INFO","DEMO","SUCCESS","WARN","ERROR"});m_logLevel->setMinimumWidth(150);toolbar->addWidget(m_logLevel);
    auto* clear=button(QStringLiteral("清空本地日志"),"clearLogsButton");toolbar->addWidget(clear);layout->addLayout(toolbar);
    auto* box=panel();auto* boxLayout=panelLayout(box,18);auto* header=sectionHeader(boxLayout,QStringLiteral("事件时间线"));m_logCount=label(QString(),"mutedLabel");header->addWidget(m_logCount);
    m_logsTable=table({QStringLiteral("时间"),QStringLiteral("级别"),QStringLiteral("内容")},"logsTable");m_logsTable->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);boxLayout->addWidget(m_logsTable,1);layout->addWidget(box,1);
    connect(m_logSearch,&QLineEdit::textChanged,this,[this]{if(!m_refreshing)refreshLogs();});connect(m_logLevel,&QComboBox::currentTextChanged,this,[this]{if(!m_refreshing)refreshLogs();});
    connect(clear,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,QStringLiteral("清空本地日志"),QStringLiteral("只清空当前本地演示日志，任务与配置不变。继续？"))==QMessageBox::Yes){m_state->logs.clear();m_state->notifyChanged();}});return page;
}

QWidget* MainWindow::buildSettings() {
    auto* page=new QWidget;page->setObjectName("settingsPage");auto* layout=pageLayout(page);
    auto* mode=panel();auto* modeLayout=panelLayout(mode,24);sectionHeader(modeLayout,QStringLiteral("运行模式"),QStringLiteral("本地预览"));
    auto* modeTitle=label(QStringLiteral("●  前端独立演示"),"detailTitle");modeTitle->setStyleSheet("color:#0F6CBD;");modeLayout->addWidget(modeTitle);
    auto* modeText=label(QStringLiteral("本版本使用本地演示数据，验证关注管理、任务配置、价格展示与统计交互。\n没有连接游戏进程，不读取实时市场，也不执行实际购买。"),"mutedLabel");modeText->setWordWrap(true);modeLayout->addWidget(modeText);layout->addWidget(mode);
    auto* config=panel();auto* configLayout=panelLayout(config,24);sectionHeader(configLayout,QStringLiteral("本地配置"));
    auto* form=new QFormLayout;form->setVerticalSpacing(16);form->setHorizontalSpacing(24);
    m_configDirectory=new QLineEdit;m_configDirectory->setObjectName("configDirectoryEdit");m_configDirectory->setReadOnly(true);form->addRow(QStringLiteral("配置文件"),m_configDirectory);
    m_autoScrollLogs=new QCheckBox(QStringLiteral("日志页自动滚动到最新事件"));m_autoScrollLogs->setObjectName("autoScrollLogsCheck");form->addRow(QStringLiteral("日志显示"),m_autoScrollLogs);
    m_compactTables=new QCheckBox(QStringLiteral("使用紧凑表格行高"));m_compactTables->setObjectName("compactTablesCheck");form->addRow(QStringLiteral("显示密度"),m_compactTables);configLayout->addLayout(form);
    auto* actionRow=new QHBoxLayout;m_settingsMessage=label(QString(),"settingsMessage");m_settingsMessage->setWordWrap(true);actionRow->addWidget(m_settingsMessage,1);auto* save=button(QStringLiteral("保存本地设置"),"saveSettingsButton",true);actionRow->addWidget(save);configLayout->addLayout(actionRow);layout->addWidget(config);
    auto* future=panel();auto* futureLayout=panelLayout(future,24);sectionHeader(futureLayout,QStringLiteral("数据连接"),QStringLiteral("尚未接入"));
    auto* futureText=label(QStringLiteral("游戏页面采集    /    价格识别与校验    /    自动操作与结果确认\n界面层与这些模块保持分离；前端完成后可以逐个接入。"),"mutedLabel");futureText->setWordWrap(true);futureLayout->addWidget(futureText);layout->addWidget(future);layout->addStretch();
    connect(save,&QPushButton::clicked,this,&MainWindow::saveSettings);connect(m_compactTables,&QCheckBox::toggled,this,&MainWindow::applyDensity);return page;
}

void MainWindow::setPage(int index) {
    if(index<0||index>=m_pages->count())return;
    m_pages->setCurrentIndex(index);m_pageHeader->setVisible(index!=1);m_pageTitle->setText(titles[index]);m_pageSubtitle->setText(subtitles[index]);
    for(int i=0;i<m_nav.size();++i){QSignalBlocker guard(m_nav[i]);m_nav[i]->setChecked(i==index);}
    m_nav[index]->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0,this,&MainWindow::fitWatchlistColumns);
}
QString MainWindow::currentPageName() const { return titles.value(m_pages->currentIndex()); }

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0,this,&MainWindow::fitWatchlistColumns);
}

void MainWindow::fitWatchlistColumns() {
    if(!m_favoritesTable)return;
    const int w=m_favoritesTable->viewport()->width();
    m_favoritesTable->setColumnWidth(2,qBound(56,qRound(w*.080),96));
    m_favoritesTable->setColumnWidth(3,qBound(76,qRound(w*.105),120));
    m_favoritesTable->setColumnWidth(4,qBound(96,qRound(w*.160),184));
    m_favoritesTable->setColumnWidth(5,qBound(76,qRound(w*.105),120));
    m_favoritesTable->setColumnWidth(6,qBound(80,qRound(w*.108),122));
}

void MainWindow::refreshAll() {
    if (m_refreshing) {
        return;
    }
    m_refreshing = true;
    refreshOverview();refreshFavorites();refreshTasks();refreshPrices();refreshStats();refreshLogs();
    m_startButton->setEnabled(!m_state->simulationRunning);m_pauseButton->setEnabled(m_state->simulationRunning);m_taskStart->setEnabled(!m_state->simulationRunning);
    int enabled=0;for(const auto& task:m_state->tasks)if(task.enabled)++enabled;
    m_engineStatus->setText(QStringLiteral("%1 条启用任务").arg(enabled));
    m_footerStatus->setText(m_state->simulationRunning?QStringLiteral("●  模拟运行中"):QStringLiteral("演示数据 · 未连接游戏"));
    QString path=m_state->configPath;if(path.isEmpty())path=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/config.json";m_configDirectory->setText(QDir::toNativeSeparators(path));
    m_refreshing=false;
}

void MainWindow::refreshOverview() {
    int followed=0,enabled=0;for(const auto& skin:m_state->skins)if(skin.followed)++followed;for(const auto& task:m_state->tasks)if(task.enabled)++enabled;
    m_overviewCards[0]->setValue(QString::number(followed),QStringLiteral("本地关注条目"));m_overviewCards[1]->setValue(QString::number(enabled),QStringLiteral("共 %1 条任务").arg(m_state->tasks.size()));m_overviewCards[2]->setValue(QString::number(m_state->simulatedScans),QStringLiteral("本次模拟累计"));m_overviewCards[3]->setValue(QString::number(m_state->simulatedSuccess),QStringLiteral("仅模拟结果"));
    double reference=m_state->skins.isEmpty()?100:m_state->skins.first().price;m_overviewChart->setSeries(series(reference));m_overviewChart->setCaption(m_state->skins.isEmpty()?QStringLiteral("演示价格"):m_state->skins.first().name+QStringLiteral(""));
    m_overviewTaskCount->setText(QStringLiteral("%1 条任务").arg(m_state->tasks.size()));m_overviewTasks->setRowCount(qMin(4,static_cast<int>(m_state->tasks.size())));
    for(int i=0;i<m_overviewTasks->rowCount();++i){const auto& task=m_state->tasks[i];put(m_overviewTasks,i,0,task.name);put(m_overviewTasks,i,1,QString::number(task.quantity));put(m_overviewTasks,i,2,task.status,task.enabled?mint:muted);}
    const int count=qMin(3,static_cast<int>(m_state->logs.size()));m_overviewLogs->setRowCount(count);
    for(int r=0;r<count;++r){const auto& log=m_state->logs[r];put(m_overviewLogs,r,0,log.time,muted);put(m_overviewLogs,r,1,log.level,(log.level=="SUCCESS"||log.level=="DEMO")?mint:muted);put(m_overviewLogs,r,2,log.message);}
}

QString MainWindow::selectedFavoriteId() const{return selectedId(m_favoritesTable);}
void MainWindow::refreshFavorites() {
    const QString previous=selectedFavoriteId();
    {QSignalBlocker guard(m_rarityFilter);QString rarity=m_rarityFilter->currentText();QStringList values;for(const auto& skin:m_state->skins)if(!values.contains(skin.rarity))values.append(skin.rarity);m_rarityFilter->clear();m_rarityFilter->addItem(QStringLiteral("全部品级"));m_rarityFilter->addItems(values);m_rarityFilter->setCurrentIndex(qMax(0,m_rarityFilter->findText(rarity)));}
    auto matches=[&](const Skin& skin,const QString& key) {
        bool linked=false;for(const auto& task:m_state->tasks)if(task.skinId==skin.id){linked=true;break;}
        return key=="all" || (key=="followed"&&skin.followed) || (key=="unfollowed"&&!skin.followed)
            || (key=="linked"&&linked) || (key=="unlinked"&&!linked) || key==skinCategory(skin.name);
    };
    for(auto* filter:m_catalogFilters){int count=0;QString key=filter->property("filterKey").toString();for(const auto& skin:m_state->skins)if(matches(skin,key))++count;
        QSignalBlocker guard(filter);filter->setText(filter->property("filterTitle").toString()+QStringLiteral("    %1").arg(count));filter->setChecked(key==m_catalogFilter);}
    QSignalBlocker guard(m_favoritesTable);m_favoritesTable->setRowCount(0);int selected=-1;
    for(const auto& skin:m_state->skins) {
        if(!matches(skin,m_catalogFilter))continue;
        const QString query=m_favoriteSearch->text().trimmed();if(!query.isEmpty()&&!skin.name.contains(query,Qt::CaseInsensitive)&&!skin.series.contains(query,Qt::CaseInsensitive))continue;
        if(m_rarityFilter->currentIndex()>0&&skin.rarity!=m_rarityFilter->currentText())continue;
        const Task* linked=nullptr;bool enabled=false;for(const auto& task:m_state->tasks)if(task.skinId==skin.id){if(!linked)linked=&task;enabled|=task.enabled;}
        if(m_favoriteStateFilter->currentIndex()==1&&!linked)continue;
        if(m_favoriteStateFilter->currentIndex()==2&&linked)continue;
        const int row=m_favoritesTable->rowCount();m_favoritesTable->insertRow(row);if(skin.id==previous)selected=row;
        auto* wrapper=new QWidget;wrapper->setStyleSheet("background:transparent;");auto* cell=new QHBoxLayout(wrapper);cell->setContentsMargins(0,0,0,0);cell->setSpacing(0);
        auto* check=new QCheckBox;check->setObjectName("follow_"+skin.id);check->setFixedSize(18,18);check->setChecked(skin.followed);cell->addWidget(check,0,Qt::AlignCenter);m_favoritesTable->setCellWidget(row,0,wrapper);
        connect(check,&QCheckBox::toggled,this,[this,id=skin.id](bool checked){if(m_refreshing)return;for(auto& value:m_state->skins)if(value.id==id){value.followed=checked;break;}m_state->notifyChanged();});
        put(m_favoritesTable,row,1,skin.name,QColor(),skin.id);auto* identity=m_favoritesTable->item(row,1);identity->setData(Qt::UserRole+1,skin.series);identity->setData(Qt::UserRole+2,skinArtIndex(skin));identity->setToolTip(skin.name+"\n"+skin.series+QStringLiteral(" · 演示插图"));
        put(m_favoritesTable,row,2,QString::number(skin.wear,'f',3),muted);put(m_favoritesTable,row,3,amount(skin.price));
        put(m_favoritesTable,row,4,QString());QVariantList samples;for(double value:series(skin.price))samples.append(value);m_favoritesTable->item(row,4)->setData(Qt::UserRole+3,samples);
        put(m_favoritesTable,row,5,linked?amount(linked->maxPrice):QStringLiteral("—"),linked?QColor("#444A52"):muted);
        put(m_favoritesTable,row,6,linked?(enabled?QStringLiteral("● 已启用"):QStringLiteral("● 已暂停")):QStringLiteral("○ 未配置"),enabled?mint:muted);
    }
    if(m_favoritesTable->rowCount()>0)m_favoritesTable->selectRow(selected<0?0:selected);
    m_favoritesCount->setText(QStringLiteral("我的关注  %1").arg(m_favoritesTable->rowCount()));refreshFavoriteDetail();
}

void MainWindow::refreshFavoriteDetail() {
    const auto* skin=findSkin(m_state,selectedFavoriteId());const bool valid=skin!=nullptr;
    m_favoriteCreate->setEnabled(valid);m_inspectorPrice->setEnabled(valid);m_inspectorWear->setEnabled(valid);m_inspectorQuantity->setEnabled(valid);
    if(!skin){m_inspectorSkinId.clear();m_favoriteTitle->setText(QStringLiteral("没有匹配条目"));m_favoriteMetadata->setText(QStringLiteral("调整搜索或筛选条件"));m_favoritePrice->setText("—");m_favoriteChange->clear();m_favoriteArt->clear();m_favoriteFacts->clear();m_favoriteTaskInfo->clear();m_favoriteChart->setSeries({});return;}
    m_favoriteTitle->setText(skin->name);m_favoriteMetadata->setText(skin->series+QStringLiteral(" · 演示插图"));
    const QPixmap art=skinArtwork(skinArtIndex(*skin));
    if(art.isNull()){m_favoriteArt->setPixmap(QPixmap());m_favoriteArt->setText(QStringLiteral("暂无演示插图"));}
    else {
        const qreal dpr=m_favoriteArt->devicePixelRatioF();
        QPixmap display=art.scaled(QSize(qRound(216*dpr),qRound(128*dpr)),Qt::KeepAspectRatio,Qt::SmoothTransformation);
        display.setDevicePixelRatio(dpr);m_favoriteArt->setPixmap(display);
    }
    m_favoritePrice->setText(amount(skin->price));m_favoriteChange->setText(changeText(skin->change));
    m_favoriteChange->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(skin->change<=0?"#537466":"#9A655F"));
    m_favoriteFacts->setText(QStringLiteral("品级    %1\n成色    %2\n磨损    %3").arg(skin->rarity,skin->condition).arg(skin->wear,0,'f',3));
    m_favoriteChart->setSeries(series(skin->price));m_favoriteChart->setCaption(QStringLiteral("价格走势  /  %1").arg(skin->name));
    int tasks=0;const Task* existing=nullptr;for(const auto& task:m_state->tasks)if(task.skinId==skin->id){++tasks;if(!existing)existing=&task;}
    if(m_inspectorSkinId!=skin->id){m_inspectorSkinId=skin->id;m_inspectorPrice->setValue(existing?existing->maxPrice:skin->price);m_inspectorWear->setValue(existing?existing->maxWear:5);m_inspectorQuantity->setValue(existing?existing->quantity:1);}
    m_favoriteTaskInfo->setText(QStringLiteral("已关联 %1 条任务").arg(tasks));
}

void MainWindow::createInspectorTask() {
    const auto* skin=findSkin(m_state,selectedFavoriteId());if(!skin)return;
    m_inspectorPrice->interpretText();m_inspectorWear->interpretText();m_inspectorQuantity->interpretText();
    Task task;task.id=QUuid::createUuid().toString(QUuid::WithoutBraces);task.name=skin->name.left(45)+QStringLiteral(" · 价格关注");task.skinId=skin->id;
    task.minPrice=0;task.maxPrice=m_inspectorPrice->value();task.maxWear=m_inspectorWear->value();task.quantity=m_inspectorQuantity->value();task.enabled=true;task.status=QStringLiteral("待启动");
    m_state->pauseSimulation();m_state->tasks.append(task);m_state->addLog("INFO",QStringLiteral("从关注工作区创建本地任务：")+task.name);m_state->notifyChanged();
    m_favoriteTaskInfo->setText(QStringLiteral("已创建 · 可在任务页编辑"));
}

QString MainWindow::selectedTaskId() const{return selectedId(m_tasksTable);}
void MainWindow::refreshTasks() {
    const QString previous=selectedTaskId();QStringList selected;for(const auto& row:m_tasksTable->selectionModel()->selectedRows()){if(auto* item=m_tasksTable->item(row.row(),0))selected.append(item->data(Qt::UserRole).toString());}
    QSignalBlocker guard(m_tasksTable);m_tasksTable->setRowCount(m_state->tasks.size());int active=0;
    for(int row=0;row<m_state->tasks.size();++row){const auto& task=m_state->tasks[row];const auto* skin=findSkin(m_state,task.skinId);if(task.enabled)++active;put(m_tasksTable,row,0,task.name,QColor(),task.id);put(m_tasksTable,row,1,skin?skin->name:QStringLiteral("目标已移除"));put(m_tasksTable,row,2,QStringLiteral("%1 — %2").arg(amount(task.minPrice),amount(task.maxPrice)));put(m_tasksTable,row,3,QString::number(task.maxWear,'f',3),muted);put(m_tasksTable,row,4,QString::number(task.quantity));
        auto* wrapper=new QWidget;wrapper->setStyleSheet("background:transparent;");auto* rowLayout=new QHBoxLayout(wrapper);rowLayout->setContentsMargins(0,0,0,0);rowLayout->setSpacing(0);auto* check=new QCheckBox;check->setFixedSize(18,18);check->setObjectName("taskEnabled_"+task.id);check->setChecked(task.enabled);rowLayout->addWidget(check,0,Qt::AlignCenter);m_tasksTable->setCellWidget(row,5,wrapper);
        connect(check,&QCheckBox::toggled,this,[this,id=task.id](bool checked){if(m_refreshing)return;for(auto& value:m_state->tasks)if(value.id==id){value.enabled=checked;value.status=checked?QStringLiteral("待启动"):QStringLiteral("演示已暂停");break;}m_state->notifyChanged();});put(m_tasksTable,row,6,task.status,task.enabled?mint:muted);
    }
    m_tasksTable->clearSelection();for(int row=0;row<m_tasksTable->rowCount();++row){if(selected.contains(m_tasksTable->item(row,0)->data(Qt::UserRole).toString()))m_tasksTable->selectionModel()->select(m_tasksTable->model()->index(row,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);if(m_tasksTable->item(row,0)->data(Qt::UserRole).toString()==previous)m_tasksTable->setCurrentCell(row,0,QItemSelectionModel::NoUpdate);}
    m_taskCount->setText(QStringLiteral("%1 条任务 · %2 条启用").arg(m_state->tasks.size()).arg(active));
}

void MainWindow::refreshPrices() {
    const QString selected=m_priceSkin->currentData().toString();{QSignalBlocker guard(m_priceSkin);m_priceSkin->clear();for(const auto& skin:m_state->skins)m_priceSkin->addItem(skin.name,skin.id);int index=m_priceSkin->findData(selected);m_priceSkin->setCurrentIndex(qMax(0,index));}
    const auto* skin=findSkin(m_state,m_priceSkin->currentData().toString());if(!skin){for(auto* card:m_priceCards)card->setValue("—",QStringLiteral("暂无皮肤样本"));m_priceChart->setSeries({});m_priceHistory->setRowCount(0);return;}
    auto values=series(skin->price);m_priceCards[0]->setValue(amount(skin->price),QStringLiteral("演示报价 · 非实际成交价"));m_priceCards[1]->setValue(amount(*std::min_element(values.begin(),values.end())),QStringLiteral("合成样本范围"));m_priceCards[2]->setValue(amount(*std::max_element(values.begin(),values.end())),QStringLiteral("合成样本范围"));m_priceChart->setSeries(values);m_priceChart->setCaption(skin->name+QStringLiteral(" · 演示数据"));m_priceHistory->setRowCount(6);
    for(int row=0;row<6;++row){int index=values.size()-1-row;put(m_priceHistory,row,0,QStringLiteral("样本 %1").arg(index+1),muted);put(m_priceHistory,row,1,skin->name);put(m_priceHistory,row,2,amount(values[index]),mint);put(m_priceHistory,row,3,QStringLiteral("演示报价"),muted);put(m_priceHistory,row,4,QStringLiteral("本地合成 · 非游戏采集"),muted);}
}

void MainWindow::refreshStats() {
    const int scans=m_state->simulatedScans,matches=m_state->simulatedMatches,success=m_state->simulatedSuccess;m_statsCards[0]->setValue(QString::number(scans),QStringLiteral("本次演示扫描条目"));m_statsCards[1]->setValue(QString::number(matches),QStringLiteral("符合模拟筛选条件"));m_statsCards[2]->setValue(QString::number(success),QStringLiteral("不是实际成交"));m_statsCards[3]->setValue("—",QStringLiteral("未连接执行模块"));
    const QVector<int> values={qMax(0,scans-matches),qMax(0,matches-success),success};for(int i=0;i<values.size();++i){double fraction=scans>0?values[i]*100.0/scans:0;m_reasonLabels[i]->setText(QStringLiteral("%1   /   %2%").arg(values[i]).arg(fraction,0,'f',1));m_reasonBars[i]->setValue(qRound(fraction));}m_statsNote->setText(QStringLiteral("模拟未决：0\n统计只描述本地模拟引擎，不代表实际游戏结果。"));
}

void MainWindow::refreshLogs() {
    m_logsTable->setRowCount(0);const QString query=m_logSearch->text().trimmed();for(const auto& log:m_state->logs){if(m_logLevel->currentIndex()>0&&log.level!=m_logLevel->currentText())continue;if(!query.isEmpty()&&!log.message.contains(query,Qt::CaseInsensitive)&&!log.level.contains(query,Qt::CaseInsensitive))continue;const int row=m_logsTable->rowCount();m_logsTable->insertRow(row);put(m_logsTable,row,0,log.time,muted);put(m_logsTable,row,1,log.level,log.level=="SUCCESS"?mint:(log.level=="WARN"?QColor("#98703D"):(log.level=="ERROR"?QColor("#B3483C"):muted)));put(m_logsTable,row,2,log.message);m_logsTable->item(row,2)->setToolTip(log.message);}
    m_logCount->setText(QStringLiteral("显示 %1 / %2 条").arg(m_logsTable->rowCount()).arg(m_state->logs.size()));if(m_autoScrollLogs&&m_autoScrollLogs->isChecked())m_logsTable->scrollToTop();
}

void MainWindow::editTask(const QString& id,const QString& skinId) {
    if (m_state->simulationRunning) {
        m_state->pauseSimulation();
    }
    Task existing;bool editing=false;for(const auto& task:m_state->tasks)if(task.id==id){existing=task;editing=true;break;}
    QDialog dialog(this);dialog.setObjectName("taskEditorDialog");dialog.setWindowTitle(editing?QStringLiteral("编辑模拟任务"):QStringLiteral("新增模拟任务"));dialog.setMinimumWidth(510);
    auto* layout=new QVBoxLayout(&dialog);layout->setContentsMargins(26,24,26,24);layout->setSpacing(20);layout->addWidget(label(editing?QStringLiteral("调整任务条件"):QStringLiteral("创建一条模拟任务"),"detailTitle"));layout->addWidget(label(QStringLiteral("这些规则只用于本地演示，不会触发实际购买。"),"mutedLabel"));
    auto* form=new QFormLayout;form->setVerticalSpacing(13);form->setHorizontalSpacing(22);
    auto* name=new QLineEdit;name->setObjectName("taskName");name->setMaxLength(60);name->setPlaceholderText(QStringLiteral("例如：天命低价关注"));if(editing)name->setText(existing.name);form->addRow(QStringLiteral("任务名称"),name);
    auto* skin=new QComboBox;skin->setObjectName("taskSkin");for(const auto& item:m_state->skins)skin->addItem(item.name,item.id);int index=skin->findData(editing?existing.skinId:skinId);if(index>=0)skin->setCurrentIndex(index);form->addRow(QStringLiteral("目标皮肤"),skin);
    auto* min=new QDoubleSpinBox;min->setObjectName("taskMinPrice");min->setRange(0,999999999);min->setDecimals(2);min->setValue(editing?existing.minPrice:0);
    auto* max=new QDoubleSpinBox;max->setObjectName("taskMaxPrice");max->setRange(0,999999999);max->setDecimals(2);const auto* defaultSkin=findSkin(m_state,skin->currentData().toString());max->setValue(editing?existing.maxPrice:(defaultSkin?defaultSkin->price:800));
    form->addRow(QStringLiteral("最低价格"),min);form->addRow(QStringLiteral("最高价格"),max);
    auto* wear=new QDoubleSpinBox;wear->setObjectName("taskWear");wear->setRange(0,100);wear->setDecimals(6);wear->setValue(editing?existing.maxWear:5);form->addRow(QStringLiteral("最大磨损"),wear);
    auto* quantity=new QSpinBox;quantity->setObjectName("taskQuantity");quantity->setRange(1,9999);quantity->setValue(editing?existing.quantity:1);form->addRow(QStringLiteral("数量上限"),quantity);
    auto* enabled=new QCheckBox(QStringLiteral("启用此任务"));enabled->setObjectName("taskEnabledCheck");enabled->setChecked(editing?existing.enabled:true);form->addRow(QStringLiteral("任务状态"),enabled);layout->addLayout(form);
    auto* validation=label(QString(),"taskValidationLabel");validation->setStyleSheet("color:#A45648;");validation->setWordWrap(true);layout->addWidget(validation);
    auto* buttons=new QDialogButtonBox;buttons->setObjectName("taskDialogButtons");auto* cancel=buttons->addButton(QStringLiteral("取消"),QDialogButtonBox::RejectRole);cancel->setObjectName("taskCancelButton");auto* save=buttons->addButton(QStringLiteral("保存任务"),QDialogButtonBox::AcceptRole);save->setObjectName("taskSaveButton");save->setProperty("primary",true);layout->addWidget(buttons);
    connect(cancel,&QPushButton::clicked,&dialog,&QDialog::reject);connect(save,&QPushButton::clicked,&dialog,[&]{if(name->text().trimmed().isEmpty()){validation->setText(QStringLiteral("请输入任务名称。"));name->setFocus();return;}if(skin->currentIndex()<0){validation->setText(QStringLiteral("请选择目标皮肤。"));return;}if(min->value()>max->value()){validation->setText(QStringLiteral("最低价格需要小于或等于最高价格。"));return;}dialog.accept();});
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    Task task;
    task.id=editing?existing.id:QUuid::createUuid().toString(QUuid::WithoutBraces);task.name=name->text().trimmed();task.skinId=skin->currentData().toString();task.minPrice=min->value();task.maxPrice=max->value();task.maxWear=wear->value();task.quantity=quantity->value();task.enabled=enabled->isChecked();task.status=task.enabled?QStringLiteral("待启动"):QStringLiteral("演示已暂停");
    if (editing) {
        for (auto& target : m_state->tasks) {
            if (target.id == id) {
                target = task;
                break;
            }
        }
        m_state->addLog("INFO", QStringLiteral("更新本地任务：") + task.name);
        m_state->resetTaskSimulation(task.id);
    } else {
        m_state->tasks.append(task);
        m_state->addLog("INFO", QStringLiteral("新增本地任务：") + task.name);
        m_state->notifyChanged();
    }
}

void MainWindow::deleteSelectedTasks() {
    QStringList ids;for(const auto& index:m_tasksTable->selectionModel()->selectedRows())if(auto* item=m_tasksTable->item(index.row(),0))ids.append(item->data(Qt::UserRole).toString());if(ids.isEmpty())return;
    if(QMessageBox::question(this,QStringLiteral("删除任务"),QStringLiteral("删除选中的 %1 条本地任务？关注列表保持不变。").arg(ids.size()))!=QMessageBox::Yes)return;
    for (int i = m_state->tasks.size() - 1; i >= 0; --i) {
        if (ids.contains(m_state->tasks[i].id)) {
            m_state->tasks.removeAt(i);
        }
    }
    m_state->addLog("INFO",QStringLiteral("删除 %1 条本地任务").arg(ids.size()));
    m_state->notifyChanged();
}

void MainWindow::setSelectedTasksEnabled(bool enabled) {
    QStringList ids;for(const auto& index:m_tasksTable->selectionModel()->selectedRows())if(auto* item=m_tasksTable->item(index.row(),0))ids.append(item->data(Qt::UserRole).toString());if(ids.isEmpty())return;
    for (auto& task : m_state->tasks) {
        if (ids.contains(task.id)) {
            task.enabled = enabled;
            task.status = enabled ? QStringLiteral("待启动") : QStringLiteral("演示已暂停");
        }
    }
    m_state->addLog("INFO",QStringLiteral("%1 %2 条选中任务").arg(enabled?QStringLiteral("启用"):QStringLiteral("暂停")).arg(ids.size()));
    m_state->notifyChanged();
}

void MainWindow::importConfiguration() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入本地配置"), QString(), QStringLiteral("JSON 配置 (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QString activeConfigPath = m_state->configPath;
    if (activeConfigPath.isEmpty()) {
        activeConfigPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/config.json";
    }
    QString error;
    m_inspectorSkinId.clear();
    if (!m_state->loadFrom(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    // Import copies settings into this workspace; subsequent saves do not edit the import source.
    m_state->configPath = activeConfigPath;
    m_state->addLog("INFO", QStringLiteral("已导入本地配置；保存位置仍为当前应用配置文件"));
    refreshAll();
}
void MainWindow::exportConfiguration(){QString path=QFileDialog::getSaveFileName(this,QStringLiteral("导出本地配置"),"relink-config.json",QStringLiteral("JSON 配置 (*.json)"));if(path.isEmpty())return;QString error;if(!m_state->saveTo(path,&error))QMessageBox::warning(this,QStringLiteral("导出失败"),error);else{m_state->addLog("INFO",QStringLiteral("已导出本地配置"));m_state->notifyChanged();}}
void MainWindow::exportPrices(){QString path=QFileDialog::getSaveFileName(this,QStringLiteral("导出演示报价"),"relink-demo-prices.csv",QStringLiteral("CSV 文件 (*.csv)"));if(path.isEmpty())return;QString error;if(!m_state->exportPricesCsv(path,&error))QMessageBox::warning(this,QStringLiteral("导出失败"),error);else{m_state->addLog("INFO",QStringLiteral("已导出演示报价 CSV（非游戏采集数据）"));m_state->notifyChanged();}}
void MainWindow::saveSettings(){QSettings preferences;preferences.setValue("ui/autoScrollLogs",m_autoScrollLogs->isChecked());preferences.setValue("ui/compactTables",m_compactTables->isChecked());preferences.sync();QString path=m_state->configPath;if(path.isEmpty())path=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/config.json";QDir().mkpath(QFileInfo(path).absolutePath());QString error;if(m_state->saveTo(path,&error)){m_settingsMessage->setText(QStringLiteral("设置与本地配置已保存"));m_state->configPath=path;m_state->addLog("INFO",QStringLiteral("已保存界面设置与本地配置"));m_state->notifyChanged();}else m_settingsMessage->setText(QStringLiteral("配置保存失败：")+error);}
void MainWindow::applyDensity(){const bool compact=m_compactTables&&m_compactTables->isChecked();const auto tables=findChildren<QTableWidget*>();for(auto* item:tables)item->verticalHeader()->setDefaultSectionSize(item==m_favoritesTable?(compact?52:62):(compact?38:48));}
