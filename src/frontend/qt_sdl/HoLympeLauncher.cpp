/*
    Ho-Lympe - the launcher window (see HoLympeLauncher.h).
    Look: maquettes/launcher-ho-lympe.html - night background, gold, Ho-Oh, glass cards.
*/

#include "HGCoop.h"
#include "HoLympeLauncher.h"

#include <cmath>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "GameUi.h"
#include "HoLympeApi.h"
#include "HoLympeSession.h"
#include "HoLympeUpdate.h"


namespace
{

const QColor kGold(243, 195, 90), kGold2(255, 223, 142), kGoldDeep(185, 130, 42);
const QColor kFlame(232, 87, 42), kFlame2(255, 154, 92), kFlameDeep(168, 52, 26);
const QColor kText(246, 239, 226), kMuted(184, 171, 149), kGreen(95, 211, 138), kRed(239, 106, 90);
const QColor kCard(30, 23, 40, 205), kCardLine(255, 214, 120, 42);

QString PickFamily(const QStringList& wanted, const QString& fallback)
{
    QStringList families = QFontDatabase::families();
    for (const QString& f : wanted)
        if (families.contains(f, Qt::CaseInsensitive))
            return f;
    return fallback;
}

QString UiFamily()
{
    static QString family = PickFamily({ "Nunito", "Segoe UI Variable Text" }, "Segoe UI");
    return family;
}

QString TitleFamily()
{
    static QString family = PickFamily({ "Cinzel", "Trajan Pro" }, "Palatino Linotype");
    return family;
}

QFont UiFont(int px, int weight = QFont::Normal)
{
    QFont f(UiFamily());
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QLabel* MakeLabel(const QString& text, int px, int weight, const QColor& color, QWidget* parent = nullptr)
{
    auto* l = new QLabel(text, parent);
    l->setFont(UiFont(px, weight));
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, color);
    l->setPalette(pal);
    return l;
}

void SetLabelColor(QLabel* l, const QColor& c)
{
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, c);
    l->setPalette(pal);
}

void ClearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0))
    {
        if (QWidget* w = item->widget())
            w->deleteLater();
        if (QLayout* l = item->layout())
            ClearLayout(l);
        delete item;
    }
}

const char* kFieldStyle =
    "QLineEdit, QPlainTextEdit { background: rgba(255,255,255,0.06); color: #f6efe2; border: 1px solid rgba(255,214,120,0.22);"
    " border-radius: 12px; padding: 10px 14px; selection-background-color: #b9822a; }"
    "QLineEdit:focus, QPlainTextEdit:focus { border: 1px solid #f3c35a; background: rgba(255,255,255,0.09); }";

const char* kCheckStyle =
    "QCheckBox { color: #f6efe2; spacing: 10px; }"
    "QCheckBox::indicator { width: 20px; height: 20px; border-radius: 6px; border: 1px solid rgba(255,214,120,0.45);"
    " background: rgba(255,255,255,0.05); }"
    "QCheckBox::indicator:checked { background: #f3c35a; border: 1px solid #ffdf8e; }";

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

// Glass card with an optional colored stripe on top; the border warms up on hover
class Card : public QWidget
{
public:
    explicit Card(QWidget* parent = nullptr) : QWidget(parent)
    {
        hover.setDuration(180);
        QObject::connect(&hover, &QVariantAnimation::valueChanged, this, [this] { update(); });
    }
    void setAccent(const QColor& a, const QColor& b) { accentA = a; accentB = b; hasAccent = true; update(); }
    void setHoverable(bool on) { hoverable = on; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(r, 18, 18);
        p.fillPath(path, kCard);
        if (hasAccent)
        {
            p.save();
            p.setClipPath(path);
            QLinearGradient g(r.topLeft(), r.topRight());
            g.setColorAt(0, accentA);
            g.setColorAt(1, accentB);
            p.fillRect(QRectF(r.left(), r.top(), r.width(), 4), g);
            p.restore();
        }
        float t = hover.currentValue().isValid() ? hover.currentValue().toFloat() : 0.0f;
        QColor line = kCardLine;
        line.setAlpha(42 + (int)(75 * t));
        p.setPen(QPen(line, 1));
        p.drawPath(path);
    }
    void enterEvent(QEnterEvent*) override { animate(1); }
    void leaveEvent(QEvent*) override { animate(0); }

private:
    void animate(float to)
    {
        if (!hoverable)
            return;
        hover.stop();
        hover.setStartValue(hover.currentValue().isValid() ? hover.currentValue().toFloat() : 0.0f);
        hover.setEndValue(to);
        hover.start();
    }
    QVariantAnimation hover;
    QColor accentA, accentB;
    bool hasAccent = false;
    bool hoverable = false;
};

class FancyButton : public QAbstractButton
{
public:
    enum Style { Gold, Flame, Ghost, Danger };

    FancyButton(const QString& text, Style style, QWidget* parent = nullptr) : QAbstractButton(parent), style(style)
    {
        setText(text);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        hover.setDuration(150);
        QObject::connect(&hover, &QVariantAnimation::valueChanged, this, [this] { update(); });
        setMinimumHeight(style == Ghost ? 40 : 50);
    }
    QSize sizeHint() const override
    {
        QFontMetrics fm(buttonFont());
        return QSize(fm.horizontalAdvance(text()) + (style == Ghost ? 34 : 60), style == Ghost ? 40 : 50);
    }

protected:
    QFont buttonFont() const
    {
        QFont f = UiFont(style == Ghost ? 14 : 17, style == Ghost ? QFont::Bold : QFont::ExtraBold);
        if (style != Ghost)
            f.setLetterSpacing(QFont::AbsoluteSpacing, 2);
        return f;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        float t = hover.currentValue().isValid() ? hover.currentValue().toFloat() : 0.0f;
        bool enabled = isEnabled();
        QRectF r = QRectF(rect()).adjusted(3, 3 - 2 * t, -3, -3 - 2 * t);
        qreal radius = style == Ghost ? 12 : 14;

        if (style == Ghost)
        {
            p.setBrush(QColor(255, 255, 255, 10 + (int)(10 * t)));
            QColor line = kCardLine;
            line.setAlpha(60 + (int)(90 * t));
            p.setPen(QPen(line, 1));
            p.drawRoundedRect(r, radius, radius);
            p.setPen(enabled ? kText : kMuted);
        }
        else
        {
            QColor top = style == Gold ? kGold2 : style == Flame ? kFlame2 : QColor(240, 120, 110);
            QColor mid = style == Gold ? kGold : style == Flame ? kFlame : kRed;
            QColor bottom = style == Gold ? kGoldDeep : style == Flame ? kFlameDeep : QColor(150, 40, 40);
            if (!enabled)
                top = mid = bottom = QColor(90, 80, 95);
            // Glow under the button
            if (enabled)
            {
                QColor glow = mid;
                for (int i = 0; i < 6; i++)
                {
                    glow.setAlpha((int)((10 + 14 * t) * (6 - i) / 6.0));
                    p.setPen(Qt::NoPen);
                    p.setBrush(glow);
                    p.drawRoundedRect(r.adjusted(-i, -i + 4, i, i + 4), radius + i, radius + i);
                }
            }
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0, top);
            g.setColorAt(0.6, mid);
            g.setColorAt(1, bottom);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(r, radius, radius);
            p.setPen(!enabled ? kMuted : style == Gold ? QColor(43, 22, 6) : Qt::white);
        }
        p.setFont(buttonFont());
        p.drawText(r, Qt::AlignCenter, text());
        if (hasFocus() && style != Ghost)
        {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(255, 255, 255, 150), 2));
            p.drawRoundedRect(r.adjusted(1, 1, -1, -1), radius, radius);
        }
    }
    void enterEvent(QEnterEvent*) override { animate(1); }
    void leaveEvent(QEvent*) override { animate(0); }
    void animate(float to)
    {
        hover.stop();
        hover.setStartValue(hover.currentValue().isValid() ? hover.currentValue().toFloat() : 0.0f);
        hover.setEndValue(to);
        hover.start();
    }

    Style style;
    QVariantAnimation hover;
};

class NavButton : public QAbstractButton
{
public:
    NavButton(const QString& glyph, const QString& text, QWidget* parent = nullptr) : QAbstractButton(parent), glyph(glyph)
    {
        setText(text);
        setCheckable(true);
        setAutoExclusive(true);
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(46);
    }
    QColor tint = kMuted;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(0, 2, 0, -2);
        if (isChecked())
        {
            QLinearGradient g(r.topLeft(), r.topRight());
            g.setColorAt(0, QColor(243, 195, 90, 48));
            g.setColorAt(1, QColor(243, 195, 90, 6));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(r, 12, 12);
            p.fillRect(QRectF(r.left(), r.top() + 8, 3, r.height() - 16), kGold);
        }
        else if (underMouse())
        {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 12));
            p.drawRoundedRect(r, 12, 12);
        }
        QColor c = isChecked() ? kGold2 : underMouse() ? kText : tint;
        p.setPen(c);
        p.setFont(UiFont(16, QFont::Bold));
        p.drawText(QRectF(r.left() + 14, r.top(), 26, r.height()), Qt::AlignCenter, glyph);
        p.setFont(UiFont(15, QFont::Bold));
        p.drawText(QRectF(r.left() + 48, r.top(), r.width() - 50, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    QString glyph;
};

// The party: 6 slots with the Pokémon icons
class TeamView : public QWidget
{
public:
    explicit TeamView(QWidget* parent = nullptr) : QWidget(parent) { setFixedHeight(64); }
    void setTeam(const QJsonArray& team)
    {
        icons.clear();
        for (const QJsonValue& v : team)
        {
            int species = v.toInt();
            QImage sheet(HoLympeAssetsDir() + QString("/pokemon_icons/%1.png").arg(species + 7));
            icons.append(sheet.isNull() ? QImage() : sheet.copy(0, 0, sheet.width(), sheet.width()));
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        qreal size = std::min(60.0, (width() - 5 * 8) / 6.0);
        for (int i = 0; i < 6; i++)
        {
            QRectF slot(i * (size + 8), (height() - size) / 2, size, size);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 13));
            p.drawRoundedRect(slot, 14, 14);
            if (i < icons.size() && !icons[i].isNull())
                p.drawImage(slot.adjusted(4, 4, -4, -4), icons[i]); // pixel art: no smoothing
        }
    }

private:
    QList<QImage> icons;
};

class BadgeView : public QWidget
{
public:
    explicit BadgeView(QWidget* parent = nullptr) : QWidget(parent) { setFixedSize(8 * 14, 14); }
    void setCount(int n) { count = n; update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < 8; i++)
        {
            p.save();
            p.translate(i * 14 + 7, 7);
            p.rotate(45);
            QRectF d(-4.5, -4.5, 9, 9);
            if (i < count)
            {
                QLinearGradient g(d.topLeft(), d.bottomRight());
                g.setColorAt(0, kGold2);
                g.setColorAt(1, kGoldDeep);
                p.setBrush(g);
            }
            else
                p.setBrush(QColor(255, 255, 255, 30));
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(d, 2, 2);
            p.restore();
        }
    }

private:
    int count = 0;
};

// A dot + text pill: "3 en ligne"
class Pill : public QWidget
{
public:
    explicit Pill(QWidget* parent = nullptr) : QWidget(parent) { setFixedHeight(26); }
    void set(const QString& t, const QColor& c) { text = t; color = c; setFixedWidth(QFontMetrics(UiFont(12, QFont::ExtraBold)).horizontalAdvance(t) + 34); update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor bg = color;
        bg.setAlpha(36);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(rect(), 13, 13);
        p.setBrush(color);
        p.drawEllipse(QPointF(14, height() / 2.0), 3.5, 3.5);
        p.setPen(color);
        p.setFont(UiFont(12, QFont::ExtraBold));
        p.drawText(QRectF(24, 0, width() - 28, height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    }

private:
    QString text;
    QColor color = kGreen;
};

class Avatar : public QWidget
{
public:
    Avatar(const QString& name, int size, bool online, QWidget* parent = nullptr)
        : QWidget(parent), letter(name.left(1).toUpper()), online(online) { setFixedSize(size, size); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0, online ? kGold : QColor(110, 104, 112));
        g.setColorAt(1, online ? kFlame : QColor(70, 66, 74));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(r);
        p.setPen(online ? QColor(58, 26, 8) : QColor(30, 28, 32));
        p.setFont(UiFont((int)(height() * 0.4), QFont::ExtraBold));
        p.drawText(r, Qt::AlignCenter, letter);
        if (height() < 44)
        {
            QRectF dot(r.right() - 9, r.bottom() - 9, 10, 10);
            p.setBrush(online ? kGreen : QColor(102, 102, 102));
            p.setPen(QPen(QColor(23, 18, 31), 2));
            p.drawEllipse(dot);
        }
    }

private:
    QString letter;
    bool online;
};

// Turning circle shown while something loads
class Spinner : public QWidget
{
public:
    explicit Spinner(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(54, 54);
        auto* timer = new QTimer(this);
        QObject::connect(timer, &QTimer::timeout, this, [this] { angle = (angle + 8) % 360; update(); });
        timer->start(16);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(5, 5, -5, -5);
        p.setPen(QPen(QColor(255, 255, 255, 30), 5));
        p.drawEllipse(r);
        QConicalGradient g(r.center(), -angle);
        g.setColorAt(0, kGold2);
        g.setColorAt(0.35, QColor(243, 195, 90, 0));
        p.setPen(QPen(QBrush(g), 5, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(r, -angle * 16, 120 * 16);
    }

private:
    int angle = 0;
};

QString FormatPlaytime(int minutes)
{
    if (minutes <= 0)
        return "—";
    return QString("%1 h %2").arg(minutes / 60).arg(minutes % 60, 2, 10, QChar('0'));
}

QString WorldName(const QString& id)
{
    return id == "nuzlocke" ? "Nuzlocke" : id == "classique" ? "Classique" : id;
}

// ---------------------------------------------------------------------------
// A server card on the home page
// ---------------------------------------------------------------------------

class ServerCard : public Card
{
public:
    std::function<void()> onPlay, onCemetery;

    ServerCard(bool nuzlocke, QWidget* parent = nullptr) : Card(parent), nuzlocke(nuzlocke)
    {
        setHoverable(true);
        if (nuzlocke)
            setAccent(kFlame, kFlame2);
        else
            setAccent(kGold, kGold2);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(20, 20, 20, 18);
        v->setSpacing(4);
        auto* head = new QHBoxLayout;
        title = MakeLabel(nuzlocke ? "Nuzlocke" : "Classique", 22, QFont::ExtraBold, kText);
        pill = new Pill;
        head->addWidget(title);
        head->addStretch();
        head->addWidget(pill);
        v->addLayout(head);
        mode = MakeLabel(nuzlocke ? QString::fromUtf8("RANDOMIZER · RÈGLES IMPOSÉES") : "LE VRAI HEARTGOLD", 12, QFont::Bold, kMuted);
        mode->setFont([] { QFont f = UiFont(12, QFont::Bold); f.setLetterSpacing(QFont::AbsoluteSpacing, 1); return f; }());
        v->addWidget(mode);
        v->addStretch(1);
        team = new TeamView;
        v->addWidget(team);
        rules = MakeLabel("", 12, QFont::Bold, QColor(255, 181, 154));
        if (nuzlocke)
        {
            auto* rulesRow = new QHBoxLayout;
            rulesRow->addWidget(rules);
            rulesRow->addStretch();
            auto* cemetery = new FancyButton(QString::fromUtf8("Cimetière"), FancyButton::Ghost);
            QObject::connect(cemetery, &QAbstractButton::clicked, this, [this] { if (onCemetery) onCemetery(); });
            rulesRow->addWidget(cemetery);
            v->addLayout(rulesRow);
        }
        else
            v->addWidget(rules);
        v->addStretch(1);
        auto* stats = new QHBoxLayout;
        stats->setSpacing(22);
        auto stat = [&](QLabel*& value, const QString& label, QWidget* extra = nullptr, int stretch = 0) {
            auto* box = new QVBoxLayout;
            box->setSpacing(0);
            value = MakeLabel("—", 18, QFont::ExtraBold, kText);
            box->addWidget(value);
            if (extra)
                box->addWidget(extra);
            box->addWidget(MakeLabel(label, 13, QFont::Normal, kMuted));
            stats->addLayout(box, stretch);
        };
        badges = new BadgeView;
        stat(location, "Position", nullptr, 1);
        stat(playtime, "Temps");
        stat(badgeCount, "Badges", badges);
        v->addLayout(stats);
        v->addStretch(2);
        play = new FancyButton("JOUER", nuzlocke ? FancyButton::Flame : FancyButton::Gold);
        play->setMinimumHeight(56);
        QObject::connect(play, &QAbstractButton::clicked, this, [this] { if (onPlay) onPlay(); });
        v->addWidget(play);
    }

    void setWorld(const QJsonObject& world)
    {
        int online = world.value("online").toInt();
        pill->set(QString("%1 en ligne").arg(online), online > 0 ? kGreen : kMuted);
        QJsonObject me = world.value("me").toObject();
        QJsonObject summary = me.value("summary").toObject();
        bool hasSave = me.value("hasSave").toBool();
        team->setTeam(summary.value("team").toArray());
        int attempt = me.value("attempt").toInt(1);
        if (nuzlocke)
        {
            int lost = summary.value("lost").toInt();
            rules->setText(QString::fromUtf8("☠ %1 Pokémon perdu%2 · tentative n°%3").arg(lost).arg(lost > 1 ? "s" : "").arg(attempt));
        }
        if (hasSave)
        {
            location->setText(summary.value("location").toString(QString::fromUtf8("—")));
            playtime->setText(FormatPlaytime(summary.value("playtime").toInt()));
            int n = summary.value("badges").toInt();
            badgeCount->setText(QString("%1 / 16").arg(n));
            badges->setCount(std::min(n, 8));
            play->setText("JOUER");
        }
        else
        {
            location->setText("Nouvelle partie");
            playtime->setText(QString::fromUtf8("—"));
            badgeCount->setText("0 / 16");
            badges->setCount(0);
            play->setText("COMMENCER");
        }
        play->setEnabled(true);
    }

    void setOffline(const QString& why)
    {
        pill->set(why, kMuted);
        play->setEnabled(false);
    }

private:
    bool nuzlocke;
    QLabel *title, *mode, *rules, *location, *playtime, *badgeCount;
    Pill* pill;
    TeamView* team;
    BadgeView* badges;
    FancyButton* play;
};

// Small modal question in the launcher's style
bool Confirm(QWidget* parent, const QString& title, const QString& text, const QString& yes)
{
    QDialog dialog(parent, Qt::FramelessWindowHint | Qt::Dialog);
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    auto* outer = new QVBoxLayout(&dialog);
    auto* card = new Card;
    card->setAccent(kFlame, kGold);
    outer->addWidget(card);
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(28, 26, 28, 22);
    v->setSpacing(12);
    v->addWidget(MakeLabel(title, 20, QFont::ExtraBold, kText));
    auto* body = MakeLabel(text, 14, QFont::Normal, kMuted);
    body->setWordWrap(true);
    body->setMinimumWidth(380);
    v->addWidget(body);
    auto* row = new QHBoxLayout;
    auto* no = new FancyButton("Annuler", FancyButton::Ghost);
    auto* ok = new FancyButton(yes, FancyButton::Danger);
    row->addStretch();
    row->addWidget(no);
    row->addWidget(ok);
    v->addLayout(row);
    QObject::connect(no, &QAbstractButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(ok, &QAbstractButton::clicked, &dialog, &QDialog::accept);
    return dialog.exec() == QDialog::Accepted;
}

QScrollArea* Scroller(QWidget* content);

// The Nuzlocke cemetery: the Pokemon of this run, then the previous runs
void ShowCemetery(QWidget* parent, const QJsonArray& current, const QJsonArray& past)
{
    QDialog dialog(parent, Qt::FramelessWindowHint | Qt::Dialog);
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    auto* outer = new QVBoxLayout(&dialog);
    auto* card = new Card;
    card->setAccent(QColor(150, 60, 80), QColor(90, 70, 120));
    outer->addWidget(card);
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(28, 24, 28, 20);
    v->setSpacing(10);
    v->addWidget(MakeLabel(QString::fromUtf8("☠  Cimetière"), 22, QFont::ExtraBold, kText));

    auto* content = new QWidget;
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, 8, 0);
    list->setSpacing(6);
    auto grave = [&](const QJsonObject& g) {
        auto* row = new QWidget;
        row->setStyleSheet("background: rgba(255,255,255,0.05); border-radius: 12px;");
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(10, 6, 14, 6);
        h->setSpacing(12);
        auto* icon = new QLabel;
        icon->setFixedSize(48, 48);
        icon->setStyleSheet("background: transparent;");
        QImage sheet(HoLympeAssetsDir() + QString("/pokemon_icons/%1.png").arg(g.value("species").toInt() + 7));
        if (!sheet.isNull())
            icon->setPixmap(QPixmap::fromImage(sheet.copy(0, 0, sheet.width(), sheet.width()).scaled(48, 48)));
        h->addWidget(icon);
        auto* text = new QVBoxLayout;
        text->setSpacing(0);
        auto* name = MakeLabel(QString("%1  ·  N. %2").arg(g.value("name").toString()).arg(g.value("level").toInt()), 15, QFont::Bold, kText);
        name->setStyleSheet("background: transparent;");
        text->addWidget(name);
        QString when = QDateTime::fromSecsSinceEpoch(g.value("date").toInteger()).toString("dd/MM  HH:mm");
        auto* where = MakeLabel(QString::fromUtf8("Tombé à %1  ·  %2").arg(g.value("location").toString(), when), 12, QFont::Normal, kMuted);
        where->setStyleSheet("background: transparent;");
        text->addWidget(where);
        h->addLayout(text, 1);
        list->addWidget(row);
    };
    auto section = [&](const QString& title, const QJsonArray& graves) {
        list->addSpacing(6);
        list->addWidget(MakeLabel(title, 12, QFont::Bold, QColor(255, 181, 154)));
        for (int i = graves.size() - 1; i >= 0; i--) // the last ones first
            grave(graves[i].toObject());
    };
    if (current.isEmpty())
        list->addWidget(MakeLabel(QString::fromUtf8("Personne n'est tombé pendant cette tentative. Pour l'instant…"), 14, QFont::Normal, kMuted));
    else
        section(QString::fromUtf8("CETTE TENTATIVE · %1").arg(current.size()), current);
    for (int i = 0; i < past.size(); i++)
        section(QString::fromUtf8("TENTATIVE PERDUE · %1").arg(past[i].toArray().size()), past[i].toArray());
    list->addStretch();
    auto* scroll = Scroller(content);
    scroll->setMinimumSize(460, 380);
    v->addWidget(scroll, 1);
    auto* row = new QHBoxLayout;
    auto* close = new FancyButton("Fermer", FancyButton::Ghost);
    row->addStretch();
    row->addWidget(close);
    v->addLayout(row);
    QObject::connect(close, &QAbstractButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

QScrollArea* Scroller(QWidget* content)
{
    auto* area = new QScrollArea;
    area->setWidget(content);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setStyleSheet("QScrollArea { background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }"
                        "QScrollBar:vertical { width: 8px; background: transparent; }"
                        "QScrollBar::handle:vertical { background: rgba(255,214,120,0.25); border-radius: 4px; min-height: 30px; }"
                        "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }");
    content->setAttribute(Qt::WA_TranslucentBackground);
    return area;
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

class LauncherWindow : public QDialog
{
public:
    HoLympeChoice* choice = nullptr;

    LauncherWindow()
    {
        setWindowTitle("Ho-Lympe");
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(1280, 760);
        setAcceptDrops(true);
        makePhoenix();
        for (int i = 0; i < 46; i++)
        {
            auto* rng = QRandomGenerator::global();
            sparks.append({ (float)rng->bounded(1.0), 8.0f + (float)rng->bounded(12.0), (float)rng->bounded(20.0),
                            0.5f + (float)rng->bounded(1.0) });
        }
        clock.start();
        auto* anim = new QTimer(this);
        QObject::connect(anim, &QTimer::timeout, this, [this] { update(); });
        anim->start(33);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        root->addLayout(buildTitlebar());
        pages = new QStackedWidget;
        root->addWidget(pages, 1);
        pages->addWidget(buildLogin());
        pages->addWidget(buildImport());
        pages->addWidget(buildMain());

        busy = buildBusy();
        refreshTimer = new QTimer(this);
        QObject::connect(refreshTimer, &QTimer::timeout, this, [this] { refreshHome(); });

        pages->setCurrentWidget(loginPage);
        // Updates first: an old copy can't play with the others
        showBusy(QString::fromUtf8("Recherche de mises à jour…"));
        HoLympe_CheckUpdate(
            [this](int, const QString& text) { busyText->setText(text); },
            [this](bool restart, const QString& error) {
                hideBusy();
                if (restart)
                {
                    restarting = true;
                    QTimer::singleShot(0, this, [this] { reject(); });
                    return;
                }
                if (!HoLympeApi::Get().token().isEmpty())
                    enterMain(false);
                if (!error.isEmpty())
                {
                    QString text = QString::fromUtf8("La mise à jour n'a pas marché : ") + error;
                    showToast(text);
                    showLoginError(text);
                }
            });
    }
    bool restarting = false;

protected:
    // ---- background ----
    void makePhoenix()
    {
        QImage sheet(HoLympeAssetsDir() + "/pokemon_sprites/250.png");
        if (sheet.isNull())
            return;
        QImage frame = sheet.copy(0, 0, sheet.height(), sheet.height()).scaled(520, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        // Gold silhouette
        QPainter tint(&frame);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(frame.rect(), kGold2);
        tint.end();
        // Soft blur
        QGraphicsScene scene;
        auto* item = new QGraphicsPixmapItem(QPixmap::fromImage(frame));
        auto* blur = new QGraphicsBlurEffect;
        blur->setBlurRadius(18);
        item->setGraphicsEffect(blur);
        scene.addItem(item);
        phoenix = QImage(560, 560, QImage::Format_ARGB32_Premultiplied);
        phoenix.fill(Qt::transparent);
        QPainter p(&phoenix);
        scene.render(&p, QRectF(20, 20, 520, 520), QRectF(0, 0, 520, 520));
        // Fade out towards the edges so it melts into the background
        QRadialGradient fade(QPointF(280, 280), 280);
        fade.setColorAt(0.0, QColor(0, 0, 0, 255));
        fade.setColorAt(0.65, QColor(0, 0, 0, 200));
        fade.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.fillRect(phoenix.rect(), fade);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath frame;
        frame.addRoundedRect(r, 14, 14);
        p.setClipPath(frame);

        QLinearGradient base(r.topLeft(), r.bottomRight());
        base.setColorAt(0, QColor(36, 26, 44));
        base.setColorAt(0.55, QColor(14, 11, 20));
        base.setColorAt(1, QColor(10, 8, 16));
        p.fillRect(r, base);
        QRadialGradient glow(QPointF(width() * 0.72, height() * 0.18), width() * 0.5);
        glow.setColorAt(0, QColor(243, 195, 90, 56));
        glow.setColorAt(1, QColor(243, 195, 90, 0));
        p.fillRect(r, glow);
        QRadialGradient ember(QPointF(width() * 0.15, height() * 1.1), width() * 0.55);
        ember.setColorAt(0, QColor(232, 87, 42, 70));
        ember.setColorAt(1, QColor(232, 87, 42, 0));
        p.fillRect(r, ember);

        float t = clock.elapsed() / 1000.0f;
        if (!phoenix.isNull())
        {
            p.setOpacity(0.11);
            float lift = 14 * std::sin(t * 2 * 3.14159f / 7);
            p.drawImage(QPointF(width() - 300 - 560 + 40, 10 + lift), phoenix);
            p.setOpacity(1);
        }
        // Rising sparks
        p.setPen(Qt::NoPen);
        for (const Spark& s : sparks)
        {
            float phase = std::fmod((t + s.offset) / s.duration, 1.0f);
            float y = height() + 20 - phase * (height() + 60);
            float alpha = phase < 0.15f ? phase / 0.15f : 1.0f - (phase - 0.15f) / 0.85f;
            p.setBrush(QColor(255, 223, 142, (int)(150 * alpha)));
            p.drawEllipse(QPointF(s.x * width(), y), 1.6 * s.scale, 1.6 * s.scale);
        }
        p.setClipping(false);
        p.setPen(QPen(QColor(255, 214, 120, 30), 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(frame);
    }

    // The window moves when dragging its top bar
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && e->position().y() < 44)
            dragFrom = e->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!dragFrom.isNull() && (e->buttons() & Qt::LeftButton))
            move(e->globalPosition().toPoint() - dragFrom);
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragFrom = QPoint(); }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() != Qt::Key_Escape) // Escape must not close the launcher
            QDialog::keyPressEvent(e);
    }
    void resizeEvent(QResizeEvent*) override
    {
        if (busy)
            busy->setGeometry(rect());
    }

    // ROM dropped on the window (first launch)
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (pages->currentWidget() == importPage && e->mimeData()->hasUrls())
            e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override
    {
        if (!e->mimeData()->urls().isEmpty())
            importRom(e->mimeData()->urls().first().toLocalFile());
    }

private:
    struct Spark { float x, duration, offset, scale; };

    QHBoxLayout* buildTitlebar()
    {
        auto* bar = new QHBoxLayout;
        bar->setContentsMargins(10, 8, 10, 0);
        bar->addStretch();
        auto make = [&](const QString& glyph) {
            auto* b = new FancyButton(glyph, FancyButton::Ghost);
            b->setFixedSize(40, 34);
            return b;
        };
        auto* minimize = make(QString::fromUtf8("—"));
        auto* close = make(QString::fromUtf8("✕"));
        QObject::connect(minimize, &QAbstractButton::clicked, this, [this] { showMinimized(); });
        QObject::connect(close, &QAbstractButton::clicked, this, [this] { reject(); });
        bar->addWidget(minimize);
        bar->addWidget(close);
        return bar;
    }

    QLabel* logo(int px)
    {
        auto* l = new QLabel("HO-LYMPE");
        QFont f(TitleFamily());
        f.setPixelSize(px);
        f.setWeight(QFont::Black);
        f.setLetterSpacing(QFont::AbsoluteSpacing, px / 12.0);
        l->setFont(f);
        // Gold gradient text
        l->setStyleSheet(QString("color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffdf8e, stop:0.55 #f3c35a, stop:1 #b9822a);"));
        return l;
    }

    // ---- login / account creation ----
    QWidget* buildLogin()
    {
        loginPage = new QWidget;
        auto* outer = new QVBoxLayout(loginPage);
        outer->setContentsMargins(0, 0, 0, 40);
        outer->addStretch();
        auto* row = new QHBoxLayout;
        row->addStretch();
        auto* card = new Card;
        card->setAccent(kGold, kFlame);
        card->setFixedWidth(460);
        row->addWidget(card);
        row->addStretch();
        outer->addLayout(row);
        outer->addStretch();

        auto* v = new QVBoxLayout(card);
        v->setContentsMargins(40, 36, 40, 30);
        v->setSpacing(10);
        auto* l = logo(46);
        l->setAlignment(Qt::AlignCenter);
        v->addWidget(l);
        auto* tag = MakeLabel(QString::fromUtf8("HEARTGOLD · ENTRE AMIS"), 12, QFont::Bold, kMuted);
        tag->setAlignment(Qt::AlignCenter);
        v->addWidget(tag);
        v->addSpacing(14);

        auto* tabs = new QHBoxLayout;
        loginTab = new NavButton("", "Connexion");
        registerTab = new NavButton("", QString::fromUtf8("Créer un compte"));
        loginTab->setChecked(true);
        tabs->addWidget(loginTab);
        tabs->addWidget(registerTab);
        v->addLayout(tabs);
        v->addSpacing(6);

        nameField = new QLineEdit(HoLympeApi::Settings().value("lastName").toString());
        nameField->setPlaceholderText("Pseudo (7 lettres max)");
        nameField->setMaxLength(7);
        passField = new QLineEdit;
        passField->setPlaceholderText("Mot de passe");
        passField->setEchoMode(QLineEdit::Password);
        confirmField = new QLineEdit;
        confirmField->setPlaceholderText("Confirme le mot de passe");
        confirmField->setEchoMode(QLineEdit::Password);
        confirmField->hide();
        for (QLineEdit* f : { nameField, passField, confirmField })
        {
            f->setStyleSheet(kFieldStyle);
            f->setFont(UiFont(16));
            f->setMinimumHeight(48);
            v->addWidget(f);
        }
        remember = new QCheckBox(QString::fromUtf8("Rester connecté"));
        remember->setChecked(HoLympeApi::Settings().value("rememberLogin", true).toBool());
        remember->setStyleSheet(kCheckStyle);
        remember->setFont(UiFont(14));
        v->addWidget(remember);
        loginError = MakeLabel("", 13, QFont::Bold, kRed);
        loginError->setWordWrap(true);
        loginError->hide();
        v->addWidget(loginError);
        v->addSpacing(4);
        loginButton = new FancyButton("SE CONNECTER", FancyButton::Gold);
        loginButton->setMinimumHeight(56);
        v->addWidget(loginButton);
        auto* offline = new FancyButton("Jouer hors ligne (solo)", FancyButton::Ghost);
        v->addWidget(offline);

        auto setMode = [this](bool creating) {
            confirmField->setVisible(creating);
            loginButton->setText(creating ? QString::fromUtf8("CRÉER MON COMPTE") : "SE CONNECTER");
            loginError->hide();
        };
        QObject::connect(loginTab, &QAbstractButton::clicked, this, [setMode] { setMode(false); });
        QObject::connect(registerTab, &QAbstractButton::clicked, this, [setMode] { setMode(true); });
        QObject::connect(loginButton, &QAbstractButton::clicked, this, [this] { submitLogin(); });
        for (QLineEdit* f : { nameField, passField, confirmField })
            QObject::connect(f, &QLineEdit::returnPressed, this, [this] { submitLogin(); });
        QObject::connect(offline, &QAbstractButton::clicked, this, [this] { enterMain(true); });
        return loginPage;
    }

    void showLoginError(const QString& text)
    {
        loginError->setText(text);
        loginError->setVisible(!text.isEmpty());
    }

    void submitLogin()
    {
        bool creating = registerTab->isChecked();
        QString name = nameField->text().trimmed();
        QString pass = passField->text();
        melonDS::u16 code[8];
        if (name.isEmpty())
            return showLoginError("Entre ton pseudo.");
        if (!EncodeGameText(name, code, 7))
            return showLoginError(QString::fromUtf8("Ce pseudo contient un caractère que le jeu ne connaît pas."));
        if (pass.size() < 6)
            return showLoginError(QString::fromUtf8("Le mot de passe doit faire au moins 6 caractères."));
        if (creating && pass != confirmField->text())
            return showLoginError("Les deux mots de passe ne sont pas pareils.");
        showLoginError("");
        loginButton->setEnabled(false);
        HoLympeApi::Get().post(creating ? "/api/register" : "/api/login", QJsonObject { { "name", name }, { "password", pass } },
            [this, name](bool ok, const QJsonObject& reply, const QString& error) {
                loginButton->setEnabled(true);
                if (!ok)
                    return showLoginError(error);
                HoLympeApi::Settings().setValue("rememberLogin", remember->isChecked());
                HoLympeApi::Settings().setValue("lastName", name);
                HoLympeApi::Get().setToken(reply.value("token").toString(), remember->isChecked());
                passField->clear();
                confirmField->clear();
                enterMain(false);
            });
    }

    // ---- first launch: the player's ROM ----
    QWidget* buildImport()
    {
        importPage = new QWidget;
        auto* outer = new QVBoxLayout(importPage);
        outer->setContentsMargins(0, 0, 0, 40);
        outer->addStretch();
        auto* row = new QHBoxLayout;
        row->addStretch();
        auto* card = new Card;
        card->setAccent(kGold, kGold2);
        card->setFixedWidth(560);
        row->addWidget(card);
        row->addStretch();
        outer->addLayout(row);
        outer->addStretch();
        auto* v = new QVBoxLayout(card);
        v->setContentsMargins(40, 34, 40, 30);
        v->setSpacing(12);
        v->addWidget(MakeLabel(QString::fromUtf8("Une dernière chose"), 26, QFont::ExtraBold, kText));
        auto* text = MakeLabel(QString::fromUtf8(
            "Ho-Lympe se construit à partir de ta propre cartouche.\n"
            "Glisse ici ton fichier « Pokémon - Version Or HeartGold (France) » (.nds), "
            "ou choisis-le avec le bouton. Il ne te sera demandé qu'une fois."), 15, QFont::Normal, kMuted);
        text->setWordWrap(true);
        v->addWidget(text);
        auto* drop = MakeLabel(QString::fromUtf8("⬇  Glisse ta ROM ici"), 18, QFont::Bold, kGold2);
        drop->setAlignment(Qt::AlignCenter);
        drop->setMinimumHeight(120);
        drop->setStyleSheet("border: 2px dashed rgba(255,214,120,0.45); border-radius: 16px; background: rgba(255,255,255,0.03);");
        v->addWidget(drop);
        importError = MakeLabel("", 13, QFont::Bold, kRed);
        importError->setWordWrap(true);
        importError->hide();
        v->addWidget(importError);
        auto* browse = new FancyButton("CHOISIR LE FICHIER", FancyButton::Gold);
        v->addWidget(browse);
        QObject::connect(browse, &QAbstractButton::clicked, this, [this] {
            QString file = QFileDialog::getOpenFileName(this, "Ta ROM HeartGold", QDir::homePath(), "ROM Nintendo DS (*.nds)");
            if (!file.isEmpty())
                importRom(file);
        });
        return importPage;
    }

    void importRom(const QString& path)
    {
        showBusy(QString::fromUtf8("Création du jeu…"));
        QTimer::singleShot(50, this, [this, path] {
            QString error = HoLympe_BuildGame(path);
            hideBusy();
            importError->setText(error);
            importError->setVisible(!error.isEmpty());
            if (error.isEmpty())
            {
                pages->setCurrentWidget(mainPage);
                if (pendingPlay)
                    pendingPlay();
            }
        });
    }

    // The game is built from the player's ROM, and rebuilt after each update
    bool ensureGame(std::function<void()> then)
    {
        if (HoLympe_GameReady())
            return true;
        QString base = HoLympeDir() + "/game/base.nds";
        if (QFile::exists(base) && HoLympe_BuildGame(base).isEmpty())
            return true;
        pendingPlay = then;
        pages->setCurrentWidget(importPage);
        return false;
    }

    // ---- main page: navigation, center, friends and news ----
    QWidget* buildMain()
    {
        mainPage = new QWidget;
        auto* h = new QHBoxLayout(mainPage);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        auto* nav = new QWidget;
        nav->setFixedWidth(240);
        auto* nv = new QVBoxLayout(nav);
        nv->setContentsMargins(22, 0, 22, 22);
        nv->setSpacing(2);
        nv->addWidget(logo(30));
        auto* tag = MakeLabel(QString::fromUtf8("HEARTGOLD · ENTRE AMIS"), 11, QFont::Bold, kMuted);
        nv->addWidget(tag);
        nv->addSpacing(26);
        center = new QStackedWidget;
        auto addNav = [&](const QString& glyph, const QString& text, QWidget* page) {
            auto* b = new NavButton(glyph, text);
            nv->addWidget(b);
            center->addWidget(page);
            QObject::connect(b, &QAbstractButton::clicked, this, [this, page] { showPage(page); });
            return b;
        };
        homeNav = addNav(QString::fromUtf8("⌂"), "Accueil", buildHome());
        newsNav = addNav(QString::fromUtf8("✦"), QString::fromUtf8("Nouveautés"), buildNews());
        addNav(QString::fromUtf8("⚙"), QString::fromUtf8("Réglages"), buildSettings());
        adminNav = addNav(QString::fromUtf8("♛"), "Admin", buildAdmin());
        adminNav->tint = QColor(231, 162, 143);
        adminNav->hide();
        homeNav->setChecked(true);
        nv->addStretch();
        auto* me = new Card;
        me->setFixedHeight(66);
        meLayout = new QHBoxLayout(me);
        meLayout->setContentsMargins(12, 10, 12, 10);
        nv->addWidget(me);
        h->addWidget(nav);

        auto* mid = new QWidget;
        auto* mv = new QVBoxLayout(mid);
        mv->setContentsMargins(10, 6, 10, 22);
        mv->addWidget(center);
        toast = MakeLabel("", 14, QFont::Bold, kText);
        toast->setStyleSheet("background: rgba(239,106,90,0.18); border: 1px solid rgba(239,106,90,0.5); border-radius: 12px; padding: 10px 14px;");
        toast->setWordWrap(true);
        toast->hide();
        mv->addWidget(toast);
        h->addWidget(mid, 1);

        auto* side = new QWidget;
        side->setFixedWidth(300);
        auto* sv = new QVBoxLayout(side);
        sv->setContentsMargins(8, 6, 22, 22);
        sv->setSpacing(16);
        auto* friends = new Card;
        auto* fv = new QVBoxLayout(friends);
        fv->setContentsMargins(16, 16, 16, 12);
        auto* fh = new QHBoxLayout;
        fh->addWidget(sectionTitle("AMIS"));
        fh->addStretch();
        friendCount = MakeLabel("", 13, QFont::ExtraBold, kGold2);
        fh->addWidget(friendCount);
        fv->addLayout(fh);
        friendList = new QVBoxLayout;
        friendList->setSpacing(4);
        fv->addLayout(friendList);
        sv->addWidget(friends);
        auto* news = new Card;
        auto* nv2 = new QVBoxLayout(news);
        nv2->setContentsMargins(16, 16, 16, 12);
        nv2->addWidget(sectionTitle(QString::fromUtf8("NOUVEAUTÉS")));
        newsSide = new QVBoxLayout;
        nv2->addLayout(newsSide);
        nv2->addStretch();
        sv->addWidget(news, 1);
        versionLabel = MakeLabel("", 12, QFont::Normal, kMuted);
        sv->addWidget(versionLabel);
        h->addWidget(side);
        return mainPage;
    }

    QLabel* sectionTitle(const QString& text)
    {
        auto* l = MakeLabel(text, 13, QFont::ExtraBold, kGold2);
        QFont f = l->font();
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        l->setFont(f);
        return l;
    }

    void showPage(QWidget* page)
    {
        center->setCurrentWidget(page);
        toast->hide();
        if (page == adminPage)
            refreshAdmin();
    }

    QWidget* buildHome()
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(16);
        auto* hello = new QVBoxLayout;
        hello->setSpacing(0);
        helloSmall = MakeLabel("Bon retour,", 15, QFont::Normal, kMuted);
        hello->addWidget(helloSmall);
        hello->addWidget(MakeLabel(QString::fromUtf8("prêt pour l'aventure ?"), 27, QFont::ExtraBold, kText));
        v->addLayout(hello);
        motd = MakeLabel("", 14, QFont::Bold, kGold2);
        motd->setStyleSheet("background: rgba(243,195,90,0.10); border: 1px solid rgba(243,195,90,0.28); border-radius: 12px; padding: 10px 14px;");
        motd->setWordWrap(true);
        motd->hide();
        v->addWidget(motd);
        auto* servers = new QHBoxLayout;
        servers->setSpacing(16);
        classicCard = new ServerCard(false);
        nuzlockeCard = new ServerCard(true);
        classicCard->onPlay = [this] { play("classique"); };
        nuzlockeCard->onPlay = [this] { play("nuzlocke"); };
        nuzlockeCard->onCemetery = [this] {
            HoLympeApi::Get().get("/api/cemetery/nuzlocke", [this](bool ok, const QJsonObject& reply, const QString& error) {
                if (!ok)
                {
                    showToast(error, false);
                    return;
                }
                ShowCemetery(this, reply.value("current").toArray(), reply.value("past").toArray());
            });
        };
        servers->addWidget(classicCard);
        servers->addWidget(nuzlockeCard);
        v->addLayout(servers, 1);
        auto* solo = new Card;
        solo->setHoverable(true);
        auto* sh = new QHBoxLayout(solo);
        sh->setContentsMargins(20, 14, 16, 14);
        auto* st = new QVBoxLayout;
        st->setSpacing(0);
        st->addWidget(MakeLabel("Solo", 17, QFont::ExtraBold, kText));
        st->addWidget(MakeLabel("Hors ligne, sur ce PC", 13, QFont::Normal, kMuted));
        sh->addLayout(st);
        sh->addStretch();
        auto* soloClassic = new FancyButton("Partie classique", FancyButton::Ghost);
        auto* soloNuz = new FancyButton("Partie Nuzlocke", FancyButton::Ghost);
        QObject::connect(soloClassic, &QAbstractButton::clicked, this, [this] { playSolo("classique"); });
        QObject::connect(soloNuz, &QAbstractButton::clicked, this, [this] { playSolo("nuzlocke"); });
        auto* soloCemetery = new FancyButton(QString::fromUtf8("☠"), FancyButton::Ghost);
        soloCemetery->setToolTip(QString::fromUtf8("Cimetière du Nuzlocke solo"));
        QObject::connect(soloCemetery, &QAbstractButton::clicked, this, [this] {
            QDir dir(HoLympeDir() + "/saves/solo-nuzlocke");
            auto read = [&](const QString& file) {
                QFile f(dir.filePath(file));
                return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).array() : QJsonArray();
            };
            QJsonArray past;
            QStringList old = dir.entryList({ "cemetery-*.json" }, QDir::Files, QDir::Name | QDir::Reversed);
            for (const QString& file : old)
                past.append(read(file));
            ShowCemetery(this, read("cemetery.json"), past);
        });
        sh->addWidget(soloClassic);
        sh->addWidget(soloNuz);
        sh->addWidget(soloCemetery);
        if (getenv("HOLYMPE_DEBUG_CEMETERY")) // development: screenshot of the cemetery
            QTimer::singleShot(1500, soloCemetery, &QAbstractButton::click);
        v->addWidget(solo);
        return page;
    }

    QWidget* buildNews()
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(MakeLabel(QString::fromUtf8("Nouveautés"), 27, QFont::ExtraBold, kText));
        auto* content = new QWidget;
        newsFull = new QVBoxLayout(content);
        newsFull->setContentsMargins(0, 6, 6, 0);
        newsFull->setSpacing(12);
        v->addWidget(Scroller(content), 1);
        return page;
    }

    QWidget* buildSettings()
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(14);
        v->addWidget(MakeLabel(QString::fromUtf8("Réglages"), 27, QFont::ExtraBold, kText));
        auto* card = new Card;
        auto* cv = new QVBoxLayout(card);
        cv->setContentsMargins(22, 20, 22, 20);
        cv->setSpacing(12);
        auto check = [&](const QString& text, const QString& key, bool def) {
            auto* c = new QCheckBox(text);
            c->setStyleSheet(kCheckStyle);
            c->setFont(UiFont(15));
            c->setChecked(HoLympeApi::Settings().value(key, def).toBool());
            QObject::connect(c, &QCheckBox::toggled, this, [key](bool on) { HoLympeApi::Settings().setValue(key, on); });
            cv->addWidget(c);
        };
        check(QString::fromUtf8("Jouer en plein écran"), "fullscreen", true);
        check(QString::fromUtf8("Rester connecté au démarrage"), "rememberLogin", true);
        cv->addSpacing(6);
        cv->addWidget(sectionTitle("NUZLOCKE SOLO"));
        auto ruleCheck = [&](const QString& text, int bit) {
            auto* c = new QCheckBox(text);
            c->setStyleSheet(kCheckStyle);
            c->setFont(UiFont(15));
            c->setChecked(HoLympeApi::Settings().value("soloRules", 0).toInt() & bit);
            QObject::connect(c, &QCheckBox::toggled, this, [bit](bool on) {
                int rules = HoLympeApi::Settings().value("soloRules", 0).toInt();
                HoLympeApi::Settings().setValue("soloRules", on ? (rules | bit) : (rules & ~bit));
            });
            cv->addWidget(c);
        };
        ruleCheck(QString::fromUtf8("Plafond de niveau (pas plus fort que le prochain champion)"), 2);
        ruleCheck(QString::fromUtf8("Objets de soin interdits en combat"), 4);
        cv->addSpacing(6);
        cv->addWidget(sectionTitle("ADRESSE DU SERVEUR"));
        auto* server = new QLineEdit(HoLympeApi::Settings().value("server", "15.237.121.156:8443").toString());
        server->setStyleSheet(kFieldStyle);
        server->setFont(UiFont(15));
        QObject::connect(server, &QLineEdit::editingFinished, this, [server] {
            HoLympeApi::Settings().setValue("server", server->text().trimmed());
        });
        cv->addWidget(server);
        v->addWidget(card);

        auto* keys = new Card;
        auto* kv = new QVBoxLayout(keys);
        kv->setContentsMargins(22, 18, 22, 18);
        kv->addWidget(sectionTitle("TOUCHES"));
        auto* list = MakeLabel(QString::fromUtf8(
            "Z Q S D ou flèches : se déplacer    ·    K ou Espace : A    ·    L ou Échap : B\n"
            "I : X (menu)    ·    J : Y    ·    Entrée : Start    ·    Retour arrière : Select    ·    A / E : L / R\n"
            "² : écran du bas    ·    F11 : plein écran    ·    Une manette est reconnue toute seule."), 13, QFont::Normal, kMuted);
        list->setWordWrap(true);
        kv->addWidget(list);
        v->addWidget(keys);

        auto* row = new QHBoxLayout;
        auto* logout = new FancyButton(QString::fromUtf8("Se déconnecter"), FancyButton::Ghost);
        QObject::connect(logout, &QAbstractButton::clicked, this, [this] {
            HoLympeApi::Get().post("/api/logout", QJsonObject(), [](bool, const QJsonObject&, const QString&) {});
            HoLympeApi::Get().forgetToken();
            refreshTimer->stop();
            pages->setCurrentWidget(loginPage);
        });
        row->addWidget(logout);
        row->addStretch();
        v->addLayout(row);
        v->addStretch();
        return page;
    }

    QWidget* buildAdmin()
    {
        adminPage = new QWidget;
        auto* outer = new QVBoxLayout(adminPage);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(MakeLabel("Admin", 27, QFont::ExtraBold, QColor(231, 162, 143)));
        auto* content = new QWidget;
        auto* v = new QVBoxLayout(content);
        v->setContentsMargins(0, 6, 6, 0);
        v->setSpacing(14);

        auto* motdCard = new Card;
        auto* mv = new QVBoxLayout(motdCard);
        mv->setContentsMargins(20, 16, 20, 16);
        mv->addWidget(sectionTitle("MESSAGE D'ACCUEIL"));
        auto* mrow = new QHBoxLayout;
        adminMotd = new QLineEdit;
        adminMotd->setPlaceholderText(QString::fromUtf8("Ce soir 21h : on attaque l'arène d'Irisia tous ensemble !"));
        adminMotd->setStyleSheet(kFieldStyle);
        adminMotd->setFont(UiFont(14));
        auto* saveMotd = new FancyButton("Enregistrer", FancyButton::Ghost);
        mrow->addWidget(adminMotd, 1);
        mrow->addWidget(saveMotd);
        mv->addLayout(mrow);
        QObject::connect(saveMotd, &QAbstractButton::clicked, this, [this] {
            adminPost("/api/admin/motd", QJsonObject { { "motd", adminMotd->text() } }, QString::fromUtf8("Message enregistré"));
        });
        v->addWidget(motdCard);

        auto* newsCard = new Card;
        auto* nv = new QVBoxLayout(newsCard);
        nv->setContentsMargins(20, 16, 20, 16);
        nv->addWidget(sectionTitle(QString::fromUtf8("PUBLIER UNE NOUVEAUTÉ")));
        auto* nrow = new QHBoxLayout;
        auto* version = new QLineEdit;
        version->setPlaceholderText("Version");
        version->setFixedWidth(110);
        auto* title = new QLineEdit;
        title->setPlaceholderText("Titre");
        nrow->addWidget(version);
        nrow->addWidget(title, 1);
        nv->addLayout(nrow);
        auto* body = new QPlainTextEdit;
        body->setPlaceholderText(QString::fromUtf8("Ce qui change pour les joueurs…"));
        body->setFixedHeight(80);
        nv->addWidget(body);
        for (QWidget* w : std::initializer_list<QWidget*> { version, title, body })
        {
            w->setStyleSheet(kFieldStyle);
            w->setFont(UiFont(14));
        }
        auto* publish = new FancyButton("Publier", FancyButton::Ghost);
        nv->addWidget(publish, 0, Qt::AlignRight);
        QObject::connect(publish, &QAbstractButton::clicked, this, [this, version, title, body] {
            adminPost("/api/admin/news", QJsonObject { { "version", version->text() }, { "title", title->text() }, { "body", body->toPlainText() } },
                      QString::fromUtf8("Nouveauté publiée"));
            title->clear();
            body->clear();
        });
        v->addWidget(newsCard);

        auto* rulesCard = new Card;
        auto* rcv = new QVBoxLayout(rulesCard);
        rcv->setContentsMargins(20, 16, 20, 16);
        rcv->addWidget(sectionTitle("SERVEUR NUZLOCKE · OPTIONS"));
        auto adminRule = [&](const QString& text, int bit) {
            auto* c = new QCheckBox(text);
            c->setStyleSheet(kCheckStyle);
            c->setFont(UiFont(14));
            adminRuleBoxes.append({ c, bit });
            QObject::connect(c, &QCheckBox::clicked, this, [this] {
                int rules = 0;
                for (const auto& box : adminRuleBoxes)
                    if (box.first->isChecked())
                        rules |= box.second;
                adminPost("/api/admin/rules", QJsonObject { { "rules", rules } }, QString::fromUtf8("Options du Nuzlocke enregistrées"));
            });
            rcv->addWidget(c);
        };
        adminRule(QString::fromUtf8("Plafond de niveau (pas plus fort que le prochain champion)"), 2);
        adminRule(QString::fromUtf8("Objets de soin interdits en combat"), 4);
        v->addWidget(rulesCard);

        auto* players = new Card;
        auto* pv = new QVBoxLayout(players);
        pv->setContentsMargins(20, 16, 20, 16);
        pv->addWidget(sectionTitle("JOUEURS"));
        adminPlayers = new QVBoxLayout;
        pv->addLayout(adminPlayers);
        v->addWidget(players);

        auto* reset = new Card;
        reset->setAccent(kFlame, kRed);
        auto* rv = new QVBoxLayout(reset);
        rv->setContentsMargins(20, 18, 20, 16);
        rv->addWidget(sectionTitle("NOUVELLE PARTIE POUR TOUS"));
        auto* rtext = MakeLabel(QString::fromUtf8("Toutes les parties du serveur choisi sont archivées et tout le monde recommence au début."), 13, QFont::Normal, kMuted);
        rtext->setWordWrap(true);
        rv->addWidget(rtext);
        auto* rrow = new QHBoxLayout;
        for (const QString& world : { QString("classique"), QString("nuzlocke") })
        {
            auto* b = new FancyButton("Recommencer " + WorldName(world), FancyButton::Danger);
            rrow->addWidget(b);
            QObject::connect(b, &QAbstractButton::clicked, this, [this, world] {
                if (Confirm(this, "Nouvelle partie pour tous ?",
                            QString::fromUtf8("Toutes les parties du serveur %1 seront archivées. Chacun recommencera au début.").arg(WorldName(world)),
                            "Recommencer"))
                    adminPost("/api/admin/reset", QJsonObject { { "world", world } }, QString::fromUtf8("Nouvelle partie lancée sur %1").arg(WorldName(world)));
            });
        }
        rv->addLayout(rrow);
        v->addWidget(reset);
        v->addStretch();
        outer->addWidget(Scroller(content), 1);
        return adminPage;
    }

    void adminPost(const QString& path, const QJsonObject& body, const QString& done)
    {
        HoLympeApi::Get().post(path, body, [this, done](bool ok, const QJsonObject&, const QString& error) {
            showToast(ok ? done : error, ok);
            refreshHome();
            refreshAdmin();
        });
    }

    void refreshAdmin()
    {
        HoLympeApi::Get().get("/api/admin", [this](bool ok, const QJsonObject& reply, const QString& error) {
            if (!ok)
                return showToast(error, false);
            if (!adminMotd->hasFocus())
                adminMotd->setText(reply.value("motd").toString());
            for (const auto& box : adminRuleBoxes)
                box.first->setChecked(reply.value("rules").toInt() & box.second);
            ClearLayout(adminPlayers);
            for (const QJsonValue& v : reply.value("accounts").toArray())
            {
                QJsonObject a = v.toObject();
                auto* row = new QHBoxLayout;
                bool online = a.value("online").toBool();
                row->addWidget(new Avatar(a.value("name").toString(), 32, online));
                auto* text = new QVBoxLayout;
                text->setSpacing(0);
                text->addWidget(MakeLabel(a.value("name").toString() + (a.value("admin").toBool() ? QString::fromUtf8("  ♛") : ""), 14, QFont::Bold, kText));
                text->addWidget(MakeLabel(online ? WorldName(a.value("world").toString()) + QString::fromUtf8(" · ") + a.value("location").toString()
                                                 : "Hors ligne", 12, QFont::Normal, kMuted));
                row->addLayout(text, 1);
                if (online && !a.value("admin").toBool())
                {
                    QString name = a.value("name").toString();
                    auto* kick = new FancyButton("Expulser", FancyButton::Ghost);
                    QObject::connect(kick, &QAbstractButton::clicked, this, [this, name] {
                        if (Confirm(this, "Expulser " + name + " ?", QString::fromUtf8("Son jeu se fermera. Il pourra revenir dans 5 minutes."), "Expulser"))
                            adminPost("/api/admin/kick", QJsonObject { { "name", name } }, name + QString::fromUtf8(" a été expulsé"));
                    });
                    row->addWidget(kick);
                }
                adminPlayers->addLayout(row);
            }
        });
    }

    // ---- data ----
    void enterMain(bool offlineMode)
    {
        offline = offlineMode;
        pages->setCurrentWidget(mainPage);
        homeNav->setChecked(true);
        center->setCurrentIndex(0);
        if (offline)
        {
            applyOffline(QString::fromUtf8("Hors ligne"));
            return;
        }
        refreshHome();
        refreshTimer->start(10000);
    }

    void setMe(const QString& name, bool online)
    {
        ClearLayout(meLayout);
        meLayout->addWidget(new Avatar(name.isEmpty() ? "?" : name, 40, online));
        auto* t = new QVBoxLayout;
        t->setSpacing(0);
        t->addWidget(MakeLabel(name.isEmpty() ? "Invité" : name, 15, QFont::ExtraBold, kText));
        t->addWidget(MakeLabel(online ? QString::fromUtf8("● Connecté") : QString::fromUtf8("● Hors ligne"), 12, QFont::Bold, online ? kGreen : kMuted));
        meLayout->addLayout(t, 1);
    }

    void applyOffline(const QString& why)
    {
        classicCard->setOffline(why);
        nuzlockeCard->setOffline(why);
        setMe(HoLympeApi::Settings().value("lastName").toString(), false);
        ClearLayout(friendList);
        friendList->addWidget(MakeLabel(QString::fromUtf8("Connecte-toi pour voir tes amis."), 13, QFont::Normal, kMuted));
        friendCount->clear();
        adminNav->hide();
        motd->hide();
        versionLabel->setText(QString::fromUtf8("Version %1").arg(HoLympe_LocalVersion()));
    }

    void refreshHome()
    {
        if (offline || pages->currentWidget() != mainPage)
            return;
        HoLympeApi::Get().get("/api/home", [this](bool ok, const QJsonObject& reply, const QString& error) {
            if (!ok)
            {
                if (error.contains("reconnecte") || error.contains("bloqué"))
                {
                    HoLympeApi::Get().forgetToken();
                    refreshTimer->stop();
                    pages->setCurrentWidget(loginPage);
                    showLoginError(error);
                    return;
                }
                applyOffline(QString::fromUtf8("Serveur injoignable"));
                return;
            }
            applyHome(reply);
        });
    }

    void applyHome(const QJsonObject& home)
    {
        name = home.value("name").toString();
        HoLympeApi::Settings().setValue("lastName", name);
        setMe(name, true);
        helloSmall->setText(QString("Bon retour, %1").arg(name));
        QString m = home.value("motd").toString();
        motd->setText(QString::fromUtf8("✦  ") + m);
        motd->setVisible(!m.isEmpty());
        isAdmin = home.value("admin").toBool();
        adminNav->setVisible(isAdmin);
        for (const QJsonValue& v : home.value("worlds").toArray())
        {
            QJsonObject w = v.toObject();
            (w.value("id").toString() == "nuzlocke" ? nuzlockeCard : classicCard)->setWorld(w);
            seeds[w.value("id").toString()] = (qint64)w.value("me").toObject().value("seed").toDouble();
        }

        ClearLayout(friendList);
        QJsonArray friends = home.value("friends").toArray();
        int onlineCount = 0;
        for (const QJsonValue& v : friends)
        {
            QJsonObject f = v.toObject();
            bool online = f.value("online").toBool();
            onlineCount += online;
            auto* row = new QHBoxLayout;
            row->setSpacing(10);
            row->addWidget(new Avatar(f.value("name").toString(), 34, online));
            auto* text = new QVBoxLayout;
            text->setSpacing(0);
            text->addWidget(MakeLabel(f.value("name").toString(), 14, QFont::Bold, online ? kText : kMuted));
            text->addWidget(MakeLabel(online ? f.value("location").toString(QString::fromUtf8("En jeu")) : "Hors ligne", 12, QFont::Normal, kMuted));
            row->addLayout(text, 1);
            if (online)
            {
                bool nuz = f.value("world").toString() == "nuzlocke";
                auto* tagLabel = MakeLabel(WorldName(f.value("world").toString()), 11, QFont::ExtraBold, nuz ? QColor(255, 181, 154) : kGold2);
                tagLabel->setFixedHeight(22);
                tagLabel->setAlignment(Qt::AlignCenter);
                tagLabel->setStyleSheet(QString("background: %1; border-radius: 8px; padding: 0 8px;")
                                            .arg(nuz ? "rgba(232,87,42,0.18)" : "rgba(243,195,90,0.14)"));
                row->addWidget(tagLabel, 0, Qt::AlignVCenter);
            }
            friendList->addLayout(row);
            if (friendList->count() >= 7)
                break;
        }
        if (friends.isEmpty())
            friendList->addWidget(MakeLabel(QString::fromUtf8("Invite tes amis à créer leur compte !"), 13, QFont::Normal, kMuted));
        friendCount->setText(QString("%1 / %2").arg(onlineCount).arg(friends.size()));

        QJsonArray news = home.value("news").toArray();
        ClearLayout(newsSide);
        ClearLayout(newsFull);
        int shown = 0;
        for (const QJsonValue& v : news)
        {
            QJsonObject n = v.toObject();
            QString head = QString("<span style='color:#f3c35a; font-weight:800'>%1</span>&nbsp;&nbsp;<b>%2</b>")
                               .arg(n.value("version").toString().toHtmlEscaped(), n.value("title").toString().toHtmlEscaped());
            if (shown++ < 3)
            {
                auto* item = new QVBoxLayout;
                item->setSpacing(2);
                auto* h = MakeLabel(head, 14, QFont::Normal, kText);
                h->setTextFormat(Qt::RichText);
                item->addWidget(h);
                auto* b = MakeLabel(n.value("body").toString(), 13, QFont::Normal, kMuted);
                b->setWordWrap(true);
                item->addWidget(b);
                item->addSpacing(8);
                newsSide->addLayout(item);
            }
            auto* card = new Card;
            auto* cv = new QVBoxLayout(card);
            cv->setContentsMargins(20, 16, 20, 16);
            auto* top = new QHBoxLayout;
            auto* h = MakeLabel(head, 17, QFont::Normal, kText);
            h->setTextFormat(Qt::RichText);
            top->addWidget(h, 1);
            if (isAdmin)
            {
                // The admin can take back a news item (mistake, no longer true)
                int id = n.value("id").toInt();
                QString title = n.value("title").toString();
                auto* remove = new FancyButton("Supprimer", FancyButton::Ghost);
                QObject::connect(remove, &QAbstractButton::clicked, this, [this, id, title] {
                    if (Confirm(this, QString::fromUtf8("Supprimer cette nouveauté ?"), QString::fromUtf8("« %1 » ne sera plus affichée.").arg(title), "Supprimer"))
                        adminPost("/api/admin/news/delete", QJsonObject { { "id", id } }, QString::fromUtf8("Nouveauté supprimée"));
                });
                top->addWidget(remove, 0, Qt::AlignTop);
            }
            cv->addLayout(top);
            auto* b = MakeLabel(n.value("body").toString(), 14, QFont::Normal, kMuted);
            b->setWordWrap(true);
            cv->addWidget(b);
            newsFull->addWidget(card);
        }
        if (news.isEmpty())
            newsSide->addWidget(MakeLabel(QString::fromUtf8("Rien de neuf pour l'instant."), 13, QFont::Normal, kMuted));
        newsFull->addStretch();

        QString serverVersion = home.value("version").toString();
        QString localVersion = HoLympe_LocalVersion();
        versionLabel->setText(serverVersion.isEmpty() || serverVersion == localVersion || localVersion == "dev"
                                  ? QString::fromUtf8("À jour · v%1").arg(localVersion == "dev" ? serverVersion : localVersion)
                                  : QString::fromUtf8("Version %1 · relance Ho-Lympe pour la %2").arg(localVersion, serverVersion));
    }

    void showToast(const QString& text, bool good = false)
    {
        toast->setText(text);
        toast->setStyleSheet(good ? "background: rgba(95,211,138,0.16); border: 1px solid rgba(95,211,138,0.5); border-radius: 12px; padding: 10px 14px;"
                                  : "background: rgba(239,106,90,0.18); border: 1px solid rgba(239,106,90,0.5); border-radius: 12px; padding: 10px 14px;");
        toast->show();
        QTimer::singleShot(5000, toast, [this, text] { if (toast->text() == text) toast->hide(); });
    }

    // ---- busy overlay ----
    QWidget* buildBusy()
    {
        auto* overlay = new QWidget(this);
        overlay->setAutoFillBackground(true);
        QPalette pal = overlay->palette();
        pal.setColor(QPalette::Window, QColor(8, 6, 12, 200));
        overlay->setPalette(pal);
        auto* v = new QVBoxLayout(overlay);
        v->addStretch();
        auto* spin = new Spinner;
        v->addWidget(spin, 0, Qt::AlignCenter);
        busyText = MakeLabel("", 17, QFont::Bold, kText);
        busyText->setAlignment(Qt::AlignCenter);
        v->addWidget(busyText);
        v->addStretch();
        overlay->hide();
        return overlay;
    }
    void showBusy(const QString& text)
    {
        busyText->setText(text);
        busy->setGeometry(rect());
        busy->raise();
        busy->show();
    }
    void hideBusy() { busy->hide(); }

    // ---- playing ----
    static void SetEnv(const char* key, const QString& value)
    {
        if (value.isEmpty())
            qunsetenv(key);
        else
            qputenv(key, value.toUtf8());
    }

    void prepareGame(const QString& saveDir, const QString& seed)
    {
        QString rom = HoLympe_GamePath();
        QDir().mkpath(saveDir);
        choice->romPath = rom;
        choice->saveFile = saveDir + "/" + QFileInfo(rom).completeBaseName() + ".sav";
        SetEnv("HGCOOP_SAVE_DIR", saveDir);
        SetEnv("HGCOOP_NAME", name.isEmpty() ? HoLympeApi::Settings().value("lastName").toString() : name);
        SetEnv("HGCOOP_SEED", seed);
        SetEnv("HGCOOP_WINDOWED", HoLympeApi::Settings().value("fullscreen", true).toBool() ? QString() : QString("1"));
    }

    void play(const QString& world)
    {
        if (!ensureGame([this, world] { play(world); }))
            return;
        showBusy(QString::fromUtf8("Chargement de ta partie…"));
        HoLympeApi::Get().get("/api/save/" + world, [this, world](bool ok, const QJsonObject& reply, const QString& error) {
            if (!ok)
            {
                hideBusy();
                return showToast(error);
            }
            QString dir = HoLympeDir() + "/saves/" + world;
            prepareGame(dir, reply.value("seed").toVariant().toString());
            QByteArray save = QByteArray::fromBase64(reply.value("save").toString().toLatin1());
            // The server's save is the only one: a new game when it has none
            QFile::remove(choice->saveFile);
            if (!save.isEmpty())
            {
                QFile f(choice->saveFile);
                if (!f.open(QIODevice::WriteOnly) || f.write(save) != save.size())
                {
                    hideBusy();
                    return showToast(QString::fromUtf8("Impossible d'écrire ta sauvegarde sur ce PC."));
                }
            }
            // The Nuzlocke run's state and cemetery, kept by the server with the save
            QString saveDir = QFileInfo(choice->saveFile).absolutePath();
            QFile::remove(saveDir + "/nuzlocke.dat");
            QFile::remove(saveDir + "/cemetery.json");
            QByteArray nuz = QByteArray::fromBase64(reply.value("nuzlocke").toString().toLatin1());
            if (!nuz.isEmpty())
            {
                QFile f(saveDir + "/nuzlocke.dat");
                if (f.open(QIODevice::WriteOnly))
                    f.write(nuz);
            }
            if (!reply.value("cemetery").toArray().isEmpty())
            {
                QFile f(saveDir + "/cemetery.json");
                if (f.open(QIODevice::WriteOnly))
                    f.write(QJsonDocument(reply.value("cemetery").toArray()).toJson(QJsonDocument::Compact));
            }
            SetEnv("HGCOOP_RULES", QString::number(reply.value("rules").toInt()));
            SetEnv("HGCOOP_LOOK", save.isEmpty() ? QString() : reply.value("look").toString()); // the look screen for a new game
            SetEnv("HGCOOP_MODE", "join");
            SetEnv("HGCOOP_ADDR", HoLympeApi::Get().host());
            SetEnv("HGCOOP_PORT", world == "nuzlocke" ? "7778" : "7777");
            SetEnv("HGCOOP_WORLD", world);
            SetEnv("HGCOOP_TOKEN", HoLympeApi::Get().token());
            HoLympeSession::Start(world, choice->saveFile, true);
            accept();
        });
    }

    void playSolo(const QString& mode)
    {
        if (!ensureGame([this, mode] { playSolo(mode); }))
            return;
        QString seed;
        if (mode == "nuzlocke")
        {
            seed = HoLympeApi::Settings().value("soloSeed").toString();
            if (seed.isEmpty())
            {
                seed = QString::number(QRandomGenerator::global()->bounded(1, 0x7FFFFFFF));
                HoLympeApi::Settings().setValue("soloSeed", seed);
            }
        }
        prepareGame(HoLympeDir() + "/saves/solo-" + mode, seed);
        SetEnv("HGCOOP_MODE", "");
        SetEnv("HGCOOP_WORLD", "solo-" + mode);
        SetEnv("HGCOOP_TOKEN", "");
        SetEnv("HGCOOP_RULES", QString::number(HoLympeApi::Settings().value("soloRules", 0).toInt()));
        SetEnv("HGCOOP_LOOK", QFile::exists(choice->saveFile) ? HoLympeApi::Settings().value("soloLook-" + mode).toString() : QString());
        HoLympeSession::Start("solo-" + mode, choice->saveFile, false);
        accept();
    }

    // ---- members ----
    QElapsedTimer clock;
    QList<Spark> sparks;
    QImage phoenix;
    QPoint dragFrom;
    QStackedWidget *pages, *center;
    QWidget *loginPage, *importPage, *mainPage, *adminPage, *busy = nullptr;
    NavButton *loginTab, *registerTab, *homeNav, *newsNav, *adminNav;
    QLineEdit *nameField, *passField, *confirmField, *adminMotd;
    QCheckBox* remember;
    QLabel *loginError, *importError, *helloSmall, *motd, *friendCount, *versionLabel, *toast, *busyText;
    FancyButton* loginButton;
    ServerCard *classicCard, *nuzlockeCard;
    QVBoxLayout *friendList, *newsSide, *newsFull, *adminPlayers;
    QHBoxLayout* meLayout;
    QTimer* refreshTimer;
    std::function<void()> pendingPlay;
    QMap<QString, qint64> seeds;
    QString name;
    bool offline = false;
    bool isAdmin = false;
    QList<QPair<QCheckBox*, int>> adminRuleBoxes;
};

}

bool HoLympe_RunLauncher(HoLympeChoice& choice)
{
    LauncherWindow window;
    window.choice = &choice;
    return window.exec() == QDialog::Accepted;
}
