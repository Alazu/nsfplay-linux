// NSFPlay for Linux - Qt6 front-end
// Port of Brad Smith's NSFPlay (Windows/MFC) using the portable xgm emulation core.

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <memory>

#include "engine.h"

#ifndef NSFPLAY_LINUX_VERSION
#define NSFPLAY_LINUX_VERSION "unknown"
#endif

using xgm::NSFPlayerConfig;

// Qt 6.2-compatible "menu item with shortcut and lambda"
template <typename F>
static QAction *addMenuAction(QMenu *m, const QString &text, const QKeySequence &ks, F f) {
    QAction *a = m->addAction(text);
    a->setShortcut(ks);
    QObject::connect(a, &QAction::triggered, m, f);
    return a;
}

static QString fmtTime(int ms) {
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

static QString iniPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/nsfplay.ini";
}

// Which expansion devices does a channel belong to / is it relevant for the loaded file
static bool channelRelevant(const Engine &e, int ch) {
    int d = NSFPlayerConfig::channel_device[ch];
    switch (d) {
    case xgm::APU: case xgm::DMC: return true;
    case xgm::FDS: return e.use_fds;
    case xgm::MMC5: return e.use_mmc5;
    case xgm::FME7: return e.use_fme7;
    case xgm::VRC6: return e.use_vrc6;
    case xgm::VRC7: return e.use_vrc7;
    case xgm::N106: return e.use_n163;
    }
    return false;
}

static QColor channelColor(Engine &e, int ch) {
    std::string c = e.cfg.GetChannelConfig(ch, "COL").GetStr();
    QColor q(QString("#") + QString::fromStdString(c));
    if (!q.isValid() || q == Qt::black) q = QColor(160, 160, 160);
    return q;
}

static const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// ======================================================================== meters
class MeterWidget : public QWidget {
public:
    MeterWidget(Engine *e, QWidget *p = nullptr) : QWidget(p), eng(e) {
        setMinimumHeight(120);
        setToolTip("Channel activity. Click a channel to mute/unmute it.");
    }
    void refresh() {
        eng->GetChannels(state);
        channels.clear();
        for (int i = 0; i < xgm::NES_CHANNEL_MAX; ++i)
            if (eng->IsLoaded() ? channelRelevant(*eng, i) : (NSFPlayerConfig::channel_device[i] <= xgm::DMC))
                channels.push_back(i);
        // skip unused VRC7 rhythm/OPLL channels unless active
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(24, 24, 28));
        if (channels.empty()) return;
        int n = (int)channels.size();
        double w = (double)width() / n;
        int mask = eng->cfg["MASK"].GetInt();
        QFont f = font();
        f.setPointSizeF(std::max(6.0, std::min(9.0, w / 5.5)));
        p.setFont(f);
        int labelH = 2 * QFontMetrics(f).height() + 4;
        int barH = height() - labelH - 6;
        for (int k = 0; k < n; ++k) {
            int ch = channels[k];
            QRectF cell(k * w + 2, 3, w - 4, barH);
            p.fillRect(cell, QColor(40, 40, 46));
            bool muted = (mask >> ch) & 1;
            const ChannelState &s = state.size() > (size_t)ch ? state[ch] : ChannelState();
            if (s.valid && !muted) {
                double lv = std::min(1.0, std::max(0.0, (double)s.volume / s.max_volume));
                if (!s.key) lv *= 0.25;
                QRectF bar(cell.left(), cell.bottom() - cell.height() * lv, cell.width(), cell.height() * lv);
                p.fillRect(bar, channelColor(*eng, ch));
            }
            if (muted) {
                p.setPen(QColor(220, 60, 60));
                p.drawText(cell, Qt::AlignCenter, "M");
            }
            p.setPen(QColor(200, 200, 200));
            QString note;
            if (s.valid && s.key && s.freq > 1.0 && !muted) {
                int nn = xgm::ITrackInfo::GetNote(s.freq) - 12;
                if (nn >= 0) note = QString("%1%2").arg(NOTE_NAMES[nn % 12]).arg(nn / 12);
            }
            QRectF lab(k * w, barH + 5, w, labelH);
            p.drawText(lab, Qt::AlignHCenter | Qt::AlignTop,
                       QString(NSFPlayerConfig::channel_name[ch]) + "\n" + note);
        }
    }
    void mousePressEvent(QMouseEvent *ev) override {
        if (channels.empty()) return;
        int k = (int)(ev->position().x() / ((double)width() / channels.size()));
        if (k < 0 || k >= (int)channels.size()) return;
        int ch = channels[k];
        int mask = eng->cfg["MASK"].GetInt();
        if (ev->button() == Qt::RightButton) {
            // solo
            int all = 0;
            for (int c : channels) all |= 1 << c;
            mask = (mask == (all & ~(1 << ch))) ? 0 : (all & ~(1 << ch));
        } else {
            mask ^= 1 << ch;
        }
        eng->cfg["MASK"] = mask;
        eng->ApplyConfig();
        update();
    }

private:
    Engine *eng;
    std::vector<ChannelState> state;
    std::vector<int> channels;
};

// ======================================================================== mixer
class MixerDialog : public QDialog {
public:
    MixerDialog(Engine *e, QWidget *p) : QDialog(p), eng(e) {
        setWindowTitle("Mixer");
        resize(560, 520);
        auto *lay = new QVBoxLayout(this);
        auto *tabs = new QTabWidget;
        lay->addWidget(tabs);

        // device volumes
        auto *dev = new QWidget;
        auto *dg = new QGridLayout(dev);
        dg->addWidget(new QLabel("<b>Device</b>"), 0, 0);
        dg->addWidget(new QLabel("<b>Volume</b>"), 0, 1);
        dg->addWidget(new QLabel("<b>Mute</b>"), 0, 3);
        static const char *dnames[xgm::NES_DEVICE_MAX] = {"APU1 (Pulse)", "APU2 (Tri/Noise/DMC)", "Sunsoft 5B",
                                                          "MMC5", "Namco 163", "VRC6", "VRC7", "FDS"};
        for (int i = 0; i < xgm::NES_DEVICE_MAX; ++i) {
            dg->addWidget(new QLabel(dnames[i]), i + 1, 0);
            auto *sl = new QSlider(Qt::Horizontal);
            sl->setRange(0, 512);
            sl->setValue(eng->cfg.GetDeviceConfig(i, "VOLUME").GetInt());
            auto *val = new QLabel(QString::number(sl->value()));
            val->setMinimumWidth(32);
            auto *mu = new QCheckBox;
            mu->setChecked(eng->cfg.GetDeviceConfig(i, "MUTE").GetInt() != 0);
            dg->addWidget(sl, i + 1, 1);
            dg->addWidget(val, i + 1, 2);
            dg->addWidget(mu, i + 1, 3);
            connect(sl, &QSlider::valueChanged, this, [=](int v) {
                val->setText(QString::number(v));
                eng->cfg.GetDeviceConfig(i, "VOLUME") = v;
                eng->ApplyConfig();
            });
            connect(mu, &QCheckBox::toggled, this, [=](bool b) {
                eng->cfg.GetDeviceConfig(i, "MUTE") = b ? 1 : 0;
                eng->ApplyConfig();
            });
        }
        dg->addWidget(new QLabel("128 = default level. NSFe 'mixe' chunks override device volume."), xgm::NES_DEVICE_MAX + 1, 0, 1, 4);
        dg->setRowStretch(xgm::NES_DEVICE_MAX + 2, 1);
        tabs->addTab(dev, "Devices");

        // channels
        auto *chw = new QWidget;
        auto *cg = new QGridLayout(chw);
        cg->addWidget(new QLabel("<b>Channel</b>"), 0, 0);
        cg->addWidget(new QLabel("<b>On</b>"), 0, 1);
        cg->addWidget(new QLabel("<b>Volume</b>"), 0, 2);
        cg->addWidget(new QLabel("<b>Pan (L-R)</b>"), 0, 3);
        for (int ch = 0; ch < xgm::NES_CHANNEL_MAX; ++ch) {
            int row = ch + 1;
            cg->addWidget(new QLabel(NSFPlayerConfig::channel_name[ch]), row, 0);
            auto *on = new QCheckBox;
            on->setChecked(!((eng->cfg["MASK"].GetInt() >> ch) & 1));
            auto *vol = new QSlider(Qt::Horizontal);
            vol->setRange(0, 256);
            vol->setValue(eng->cfg.GetChannelConfig(ch, "VOL").GetInt());
            auto *pan = new QSlider(Qt::Horizontal);
            pan->setRange(0, 255);
            pan->setValue(eng->cfg.GetChannelConfig(ch, "PAN").GetInt());
            cg->addWidget(on, row, 1);
            cg->addWidget(vol, row, 2);
            cg->addWidget(pan, row, 3);
            connect(on, &QCheckBox::toggled, this, [=](bool b) {
                int m = eng->cfg["MASK"].GetInt();
                if (b) m &= ~(1 << ch); else m |= (1 << ch);
                eng->cfg["MASK"] = m;
                eng->ApplyConfig();
            });
            connect(vol, &QSlider::valueChanged, this, [=](int v) {
                eng->cfg.GetChannelConfig(ch, "VOL") = v;
                eng->ApplyConfig();
            });
            connect(pan, &QSlider::valueChanged, this, [=](int v) {
                eng->cfg.GetChannelConfig(ch, "PAN") = v;
                eng->ApplyConfig();
            });
            ons.push_back(on);
            vols.push_back(vol);
            pans.push_back(pan);
        }
        auto *scroll = new QScrollArea;
        scroll->setWidget(chw);
        scroll->setWidgetResizable(true);
        tabs->addTab(scroll, "Channels");

        auto *bb = new QHBoxLayout;
        auto *reset = new QPushButton("Reset channels");
        auto *center = new QPushButton("Center pans");
        auto *close = new QPushButton("Close");
        bb->addWidget(reset);
        bb->addWidget(center);
        bb->addStretch();
        bb->addWidget(close);
        lay->addLayout(bb);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        connect(reset, &QPushButton::clicked, this, [=] {
            for (size_t i = 0; i < ons.size(); ++i) { ons[i]->setChecked(true); vols[i]->setValue(128); pans[i]->setValue(128); }
        });
        connect(center, &QPushButton::clicked, this, [=] { for (auto *p : pans) p->setValue(128); });
    }
    void syncMask() {
        int m = eng->cfg["MASK"].GetInt();
        for (size_t i = 0; i < ons.size(); ++i) {
            QSignalBlocker b(ons[i]);
            ons[i]->setChecked(!((m >> i) & 1));
        }
    }

private:
    Engine *eng;
    std::vector<QCheckBox *> ons;
    std::vector<QSlider *> vols, pans;
};

// ======================================================================== options
class OptionsDialog : public QDialog {
public:
    OptionsDialog(Engine *e, QWidget *p) : QDialog(p), eng(e) {
        setWindowTitle("Options");
        resize(560, 560);
        auto *lay = new QVBoxLayout(this);
        auto *tabs = new QTabWidget;
        lay->addWidget(tabs);

        // ---- Playback
        auto *pb = new QWidget;
        auto *pf = new QFormLayout(pb);
        rate = new QComboBox;
        for (int r : {11025, 22050, 32000, 44100, 48000, 88200, 96000, 192000}) rate->addItem(QString::number(r), r);
        sel(rate, eng->cfg["RATE"].GetInt());
        pf->addRow("Sample rate (Hz)", rate);
        quality = spin(1, 40, "QUALITY");
        pf->addRow("Quality (oversampling)", quality);
        playtime = spin(1, 60 * 60, "", eng->cfg["PLAY_TIME"].GetInt() / 1000);
        pf->addRow("Default play time (s)", playtime);
        fadetime = spin(0, 600, "", eng->cfg["FADE_TIME"].GetInt() / 1000);
        pf->addRow("Default fade time (s)", fadetime);
        loopnum = spin(0, 99, "LOOP_NUM");
        pf->addRow("Loop count (detected loops)", loopnum);
        advance = new QComboBox;
        advance->addItem("Advance to next track", 0);
        advance->addItem("Play forever (no fade)", 1);
        advance->addItem("Stop after track", 2);
        advance->addItem("Repeat track", 3);
        sel(advance, eng->cfg["PLAY_ADVANCE"].GetInt());
        {
            QSettings s(iniPath() + ".ui", QSettings::IniFormat);
            if (s.value("repeat", false).toBool()) sel(advance, 3);
        }
        pf->addRow("When a track ends", advance);
        autostop = check("AUTO_STOP", "Stop on silence after");
        stopsec = spin(1, 60, "STOP_SEC");
        auto *h1 = new QHBoxLayout; h1->addWidget(autostop); h1->addWidget(stopsec); h1->addWidget(new QLabel("s")); h1->addStretch();
        pf->addRow("", h1);
        autodetect = check("AUTO_DETECT", "Auto-detect loop, detection window");
        detecttime = spin(1, 600, "", eng->cfg["DETECT_TIME"].GetInt() / 1000);
        auto *h2 = new QHBoxLayout; h2->addWidget(autodetect); h2->addWidget(detecttime); h2->addWidget(new QLabel("s")); h2->addStretch();
        pf->addRow("", h2);
        nsfeplst = check("NSFE_PLAYLIST", "Use NSFe playlist order (applies on next file open)");
        pf->addRow("", nsfeplst);
        fastseek = check("FAST_SEEK", "Fast seek (skip audio rendering while seeking)");
        pf->addRow("", fastseek);
        tabs->addTab(pb, "Playback");

        // ---- Emulation
        auto *em = new QWidget;
        auto *ef = new QFormLayout(em);
        region = new QComboBox;
        const char *rn[] = {"Auto (from file)", "Prefer NTSC", "Prefer PAL", "Prefer Dendy", "Force NTSC", "Force PAL", "Force Dendy"};
        for (int i = 0; i < 7; ++i) region->addItem(rn[i], i);
        sel(region, eng->cfg["REGION"].GetInt());
        ef->addRow("Region", region);
        vsync = check("VSYNC_ADJUST", "Use exact VSync rate instead of NSF speed field");
        ef->addRow("", vsync);
        irq = check("IRQ_ENABLE", "Enable NSF2 IRQ support");
        ef->addRow("", irq);
        multspeed = spin(64, 1024, "MULT_SPEED");
        ef->addRow("CPU speed multiplier (256 = 1x)", multspeed);
        vrc7patch = new QComboBox;
        const char *vp[] = {"0: Nuke.YKT (2019)", "1: rainwarrior (2012)", "2: Mitsutaka Okazaki", "3: Mitsutaka Okazaki 2", "4: kevtris",
                            "5: FamiTracker 0.3.5", "6: FamiTracker 0.3.6"};
        for (int i = 0; i < 7; ++i) vrc7patch->addItem(vp[i], i);
        sel(vrc7patch, eng->cfg["VRC7_PATCH"].GetInt());
        ef->addRow("VRC7 patch set", vrc7patch);
        tabs->addTab(em, "Emulation");

        // ---- Audio
        auto *au = new QWidget;
        auto *af = new QFormLayout(au);
        master = slider(0, 256, "MASTER_VOLUME");
        af->addRow("Master volume", master);
        lpf = slider(0, 400, "LPF");
        af->addRow("Low-pass filter (0 = off)", lpf);
        hpf = slider(0, 256, "HPF");
        af->addRow("High-pass (DC) filter", hpf);
        tabs->addTab(au, "Audio");

        // ---- Devices
        auto *dv = new QWidget;
        auto *dl = new QVBoxLayout(dv);
        struct Opt { int dev; int opt; const char *name; };
        static const Opt opts[] = {
            {xgm::APU, 0, "APU1: Unmute on reset"}, {xgm::APU, 1, "APU1: Phase refresh"},
            {xgm::APU, 2, "APU1: Non-linear mixer"}, {xgm::APU, 3, "APU1: Swap duty 1/2"},
            {xgm::APU, 4, "APU1: Initialize sweep unit negate"},
            {xgm::DMC, 0, "APU2: Enable $4011 writes"}, {xgm::DMC, 1, "APU2: Enable periodic noise"},
            {xgm::DMC, 2, "APU2: Unmute on reset"}, {xgm::DMC, 3, "APU2: DPCM anti-click"},
            {xgm::DMC, 4, "APU2: Non-linear mixer"}, {xgm::DMC, 5, "APU2: Randomize noise at reset"},
            {xgm::DMC, 6, "APU2: Mute ultrasonic triangle"}, {xgm::DMC, 7, "APU2: Randomize triangle at reset"},
            {xgm::DMC, 8, "APU2: Reverse DPCM bit order"},
            {xgm::MMC5, 0, "MMC5: Non-linear mixer"}, {xgm::MMC5, 1, "MMC5: Phase refresh"},
            {xgm::N106, 0, "N163: Serial multiplexing (more accurate, noisier)"},
            {xgm::N106, 1, "N163: Phase read-only"}, {xgm::N106, 2, "N163: Limit wavelength"},
            {xgm::FDS, 1, "FDS: $4085 resets modulator phase"}, {xgm::FDS, 2, "FDS: Write-protect $8000-DFFF"},
            {xgm::VRC7, 0, "VRC7: Use YM2413 (OPLL) instead of VRC7"},
        };
        for (auto &o : opts) {
            std::string key = std::string(NSFPlayerConfig::dname[o.dev]) + "_OPTION" + "0123456789ABCDEF"[o.opt];
            if (!eng->cfg.HasValue(key)) continue;
            auto *cb = new QCheckBox(o.name);
            cb->setChecked(eng->cfg[key].GetInt() != 0);
            devopts.push_back({cb, key});
            dl->addWidget(cb);
        }
        auto *fh = new QHBoxLayout;
        fdscut = new QSpinBox;
        fdscut->setRange(0, 20000);
        fdscut->setValue(eng->cfg.GetDeviceOption(xgm::FDS, 0).GetInt());
        fh->addWidget(new QLabel("FDS low-pass cutoff (Hz, 0 = off)"));
        fh->addWidget(fdscut);
        fh->addStretch();
        dl->addLayout(fh);
        dl->addStretch();
        auto *dsc = new QScrollArea;
        dsc->setWidget(dv);
        dsc->setWidgetResizable(true);
        tabs->addTab(dsc, "Devices");

        // ---- Advanced: raw key editor
        auto *adv = new QWidget;
        auto *al = new QVBoxLayout(adv);
        al->addWidget(new QLabel("All configuration values (same keys as Windows nsfplay.ini). "
                                 "Edits here override the other tabs."));
        table = new QTableWidget;
        table->setColumnCount(2);
        table->setHorizontalHeaderLabels({"Key", "Value"});
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->setVisible(false);
        auto keys = eng->cfg.Keys();
        table->setRowCount((int)keys.size());
        for (int i = 0; i < (int)keys.size(); ++i) {
            auto *k = new QTableWidgetItem(QString::fromStdString(keys[i]));
            k->setFlags(k->flags() & ~Qt::ItemIsEditable);
            table->setItem(i, 0, k);
            QString v = QString::fromStdString(eng->cfg.GetValue(keys[i]).GetStr());
            table->setItem(i, 1, new QTableWidgetItem(v));
            original[keys[i]] = v;
        }
        table->resizeColumnToContents(0);
        al->addWidget(table);
        tabs->addTab(adv, "Advanced");

        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply |
                                        QDialogButtonBox::RestoreDefaults);
        lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, this, [this] { apply(); accept(); });
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
        connect(bb->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(this, "Restore defaults", "Reset ALL settings to defaults?") != QMessageBox::Yes) return;
            NsfConfig def;
            for (auto &k : def.Keys()) eng->cfg.GetValue(k) = def.GetValue(k);
            eng->cfg["MASTER_VOLUME"] = 128;
            eng->ApplyConfig();
            eng->RefreshTrackLengths();
            changed = true;
            accept();
        });
    }
    bool changed = false;
    bool repeat = false;

private:
    void sel(QComboBox *c, int v) { int i = c->findData(v); if (i >= 0) c->setCurrentIndex(i); }
    QSpinBox *spin(int lo, int hi, const char *key, int val = 0) {
        auto *s = new QSpinBox;
        s->setRange(lo, hi);
        s->setValue(key[0] ? eng->cfg[key].GetInt() : val);
        return s;
    }
    QCheckBox *check(const char *key, const char *text) {
        auto *c = new QCheckBox(text);
        c->setChecked(eng->cfg[key].GetInt() != 0);
        return c;
    }
    QSlider *slider(int lo, int hi, const char *key) {
        auto *s = new QSlider(Qt::Horizontal);
        s->setRange(lo, hi);
        s->setValue(eng->cfg[key].GetInt());
        s->setToolTip(QString::number(s->value()));
        connect(s, &QSlider::valueChanged, s, [s](int v) { s->setToolTip(QString::number(v)); });
        return s;
    }
    void apply() {
        auto &c = eng->cfg;
        c["RATE"] = rate->currentData().toInt();
        c["QUALITY"] = quality->value();
        c["PLAY_TIME"] = playtime->value() * 1000;
        c["FADE_TIME"] = fadetime->value() * 1000;
        c["LOOP_NUM"] = loopnum->value();
        int adv = advance->currentData().toInt();
        repeat = adv == 3;
        c["PLAY_ADVANCE"] = repeat ? 0 : adv;
        {
            QSettings s(iniPath() + ".ui", QSettings::IniFormat);
            s.setValue("repeat", repeat);
        }
        c["AUTO_STOP"] = autostop->isChecked() ? 1 : 0;
        c["STOP_SEC"] = stopsec->value();
        c["AUTO_DETECT"] = autodetect->isChecked() ? 1 : 0;
        c["DETECT_TIME"] = detecttime->value() * 1000;
        c["NSFE_PLAYLIST"] = nsfeplst->isChecked() ? 1 : 0;
        c["FAST_SEEK"] = fastseek->isChecked() ? 1 : 0;
        c["REGION"] = region->currentData().toInt();
        c["VSYNC_ADJUST"] = vsync->isChecked() ? 1 : 0;
        c["IRQ_ENABLE"] = irq->isChecked() ? 1 : 0;
        c["MULT_SPEED"] = multspeed->value();
        c["VRC7_PATCH"] = vrc7patch->currentData().toInt();
        c["MASTER_VOLUME"] = master->value();
        c["LPF"] = lpf->value();
        c["HPF"] = hpf->value();
        for (auto &d : devopts) c[d.second] = d.first->isChecked() ? 1 : 0;
        c.GetDeviceOption(xgm::FDS, 0) = fdscut->value();
        // advanced edits win when changed
        for (int i = 0; i < table->rowCount(); ++i) {
            std::string k = table->item(i, 0)->text().toStdString();
            QString v = table->item(i, 1)->text();
            if (original[k] != v) { c.GetValue(k) = v.toStdString(); original[k] = v; }
        }
        // refresh advanced table with resulting values
        for (int i = 0; i < table->rowCount(); ++i) {
            std::string k = table->item(i, 0)->text().toStdString();
            QString v = QString::fromStdString(c.GetValue(k).GetStr());
            table->item(i, 1)->setText(v);
            original[k] = v;
        }
        eng->ApplyConfig();
        eng->RefreshTrackLengths();
        changed = true;
    }

    Engine *eng;
    QComboBox *rate, *advance, *region, *vrc7patch;
    QSpinBox *quality, *playtime, *fadetime, *loopnum, *stopsec, *detecttime, *multspeed, *fdscut;
    QCheckBox *autostop, *autodetect, *nsfeplst, *fastseek, *vsync, *irq;
    QSlider *master, *lpf, *hpf;
    std::vector<std::pair<QCheckBox *, std::string>> devopts;
    QTableWidget *table;
    std::map<std::string, QString> original;
};

// ======================================================================== main window
class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("NSFPlay");
        setAcceptDrops(true);
        resize(640, 560);
        eng.LoadSettings(iniPath().toStdString());
        eng.ApplyConfig();
        {
            QSettings s(iniPath() + ".ui", QSettings::IniFormat);
            repeat = s.value("repeat", false).toBool();
            lastDir = s.value("lastDir", QDir::homePath()).toString();
            restoreGeometry(s.value("geometry").toByteArray());
        }

        auto *central = new QWidget;
        auto *v = new QVBoxLayout(central);

        // info box
        auto *info = new QGroupBox("NSF");
        auto *ig = new QFormLayout(info);
        lTitle = new QLabel("-"); lArtist = new QLabel("-"); lCopy = new QLabel("-"); lChips = new QLabel("-");
        for (auto *l : {lTitle, lArtist, lCopy, lChips}) l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont bf = lTitle->font(); bf.setBold(true); bf.setPointSizeF(bf.pointSizeF() * 1.2); lTitle->setFont(bf);
        ig->addRow("Title:", lTitle);
        ig->addRow("Artist:", lArtist);
        ig->addRow("Copyright:", lCopy);
        ig->addRow("Chips:", lChips);
        v->addWidget(info);

        // track list
        list = new QTreeWidget;
        list->setColumnCount(3);
        list->setHeaderLabels({"#", "Track", "Length"});
        list->setRootIsDecorated(false);
        list->setAlternatingRowColors(true);
        list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        list->header()->setStretchLastSection(false);
        v->addWidget(list, 2);
        connect(list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it, int) {
            playTrack(list->indexOfTopLevelItem(it));
        });

        meters = new MeterWidget(&eng);
        v->addWidget(meters, 1);

        // position row
        auto *posRow = new QHBoxLayout;
        lPos = new QLabel("0:00");
        lLen = new QLabel("0:00");
        seek = new QSlider(Qt::Horizontal);
        seek->setRange(0, 1000);
        posRow->addWidget(lPos);
        posRow->addWidget(seek, 1);
        posRow->addWidget(lLen);
        v->addLayout(posRow);
        connect(seek, &QSlider::sliderReleased, this, [this] {
            if (!eng.IsLoaded()) return;
            int len = eng.LengthMs();
            eng.Seek((int)((long long)seek->value() * len / 1000));
        });

        setCentralWidget(central);

        // toolbar / transport
        auto *tb = addToolBar("Transport");
        tb->setMovable(false);
        auto st = [this](QStyle::StandardPixmap p) { return style()->standardIcon(p); };
        aOpen = tb->addAction(st(QStyle::SP_DialogOpenButton), "Open", this, [this] { openDialog(); });
        tb->addSeparator();
        aPrev = tb->addAction(st(QStyle::SP_MediaSkipBackward), "Previous track", this, [this] { prev(); });
        aPlay = tb->addAction(st(QStyle::SP_MediaPlay), "Play", this, [this] {
            if (eng.IsActive() && eng.IsPaused()) eng.Pause(); else if (!eng.IsActive()) eng.Play();
        });
        aPause = tb->addAction(st(QStyle::SP_MediaPause), "Pause", this, [this] { eng.Pause(); });
        aStop = tb->addAction(st(QStyle::SP_MediaStop), "Stop", this, [this] { eng.Stop(); });
        aNext = tb->addAction(st(QStyle::SP_MediaSkipForward), "Next track", this, [this] { next(true); });
        tb->addSeparator();
        auto *volIcon = new QLabel;
        volIcon->setPixmap(st(QStyle::SP_MediaVolume).pixmap(16, 16));
        tb->addWidget(volIcon);
        vol = new QSlider(Qt::Horizontal);
        vol->setRange(0, 256);
        vol->setMaximumWidth(120);
        vol->setValue(eng.cfg["MASTER_VOLUME"].GetInt());
        vol->setToolTip("Master volume");
        tb->addWidget(vol);
        connect(vol, &QSlider::valueChanged, this, [this](int x) { eng.cfg["MASTER_VOLUME"] = x; });
        tb->addSeparator();
        tb->addAction(st(QStyle::SP_FileDialogInfoView), "File info", this, [this] { showInfo(); });
        tb->addAction(st(QStyle::SP_ComputerIcon), "Mixer", this, [this] { showMixer(); });
        tb->addAction(st(QStyle::SP_FileDialogDetailedView), "Options", this, [this] { showOptions(); });
        tb->addAction(st(QStyle::SP_DialogSaveButton), "Export WAV", this, [this] { exportWav(); });

        aPlay->setShortcut(QKeySequence(Qt::Key_X));
        aPause->setShortcut(QKeySequence(Qt::Key_Space));
        aStop->setShortcut(QKeySequence(Qt::Key_V));
        aPrev->setShortcut(QKeySequence(Qt::Key_Z));
        aNext->setShortcut(QKeySequence(Qt::Key_B));
        aOpen->setShortcut(QKeySequence::Open);

        // menus
        auto *mf = menuBar()->addMenu("&File");
        mf->addAction(aOpen);
        mf->addAction("Export track to &WAV...", this, [this] { exportWav(); });
        mf->addAction("Export &all tracks to WAV...", this, [this] { exportAll(); });
        mf->addSeparator();
        addMenuAction(mf, "&Quit", QKeySequence(QKeySequence::Quit), [this] { close(); });
        auto *mp = menuBar()->addMenu("&Playback");
        for (auto *a : {aPlay, aPause, aStop, aPrev, aNext}) mp->addAction(a);
        mp->addSeparator();
        aRepeat = mp->addAction("&Repeat track");
        aRepeat->setCheckable(true);
        aRepeat->setChecked(repeat);
        connect(aRepeat, &QAction::toggled, this, [this](bool b) {
            repeat = b;
            QSettings s(iniPath() + ".ui", QSettings::IniFormat);
            s.setValue("repeat", b);
        });
        addMenuAction(mp, "Seek &forward 10s", QKeySequence(Qt::Key_Right), [this] { eng.Seek(eng.PositionMs() + 10000); });
        addMenuAction(mp, "Seek &back 10s", QKeySequence(Qt::Key_Left), [this] { eng.Seek(eng.PositionMs() - 10000); });
        auto *mt = menuBar()->addMenu("&Tools");
        addMenuAction(mt, "File &info...", QKeySequence(Qt::CTRL | Qt::Key_I), [this] { showInfo(); });
        addMenuAction(mt, "&Mixer...", QKeySequence(Qt::CTRL | Qt::Key_M), [this] { showMixer(); });
        addMenuAction(mt, "&Options...", QKeySequence(Qt::CTRL | Qt::Key_P), [this] { showOptions(); });
        mt->addAction("&Unmute all channels", this, [this] { eng.cfg["MASK"] = 0; eng.ApplyConfig(); if (mixer) mixer->syncMask(); });
        auto *mh = menuBar()->addMenu("&Help");
        mh->addAction("&About", this, [this] {
            QMessageBox::about(this, "About NSFPlay",
                QString("<b>NSFPlay for Linux</b> %1<br><br>"
                        "Linux port (Qt6/ALSA) of NSFPlay by Brad Smith, based on the original NSFPlay by Brezza.<br>"
                        "Plays NSF, NSFe, NSF2 and NEZ-style M3U playlist entries.<br><br>"
                        "Shortcuts: Z prev, X play, Space pause, V stop, B next, Left/Right seek.<br>"
                        "Meters: left-click a channel to mute, right-click to solo.<br><br>"
                        "<a href='https://bbbradsmith.github.io/nsfplay/'>bbbradsmith.github.io/nsfplay</a>")
                    .arg(NSFPLAY_LINUX_VERSION));
        });

        statusBar()->showMessage("Open an NSF/NSFe file (Ctrl+O) or drop one onto the window.");

        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { tick(); });
        timer->start(33);
    }

    void openFile(const QString &path) {
        std::string err;
        if (!eng.Open(path.toStdString(), err)) {
            QMessageBox::warning(this, "NSFPlay", QString("Could not open %1:\n%2").arg(path, QString::fromStdString(err)));
            return;
        }
        lastDir = QFileInfo(path).absolutePath();
        lTitle->setText(QString::fromLocal8Bit(eng.title.c_str()));
        lArtist->setText(QString::fromLocal8Bit(eng.artist.c_str()));
        lCopy->setText(QString::fromLocal8Bit(eng.copyright.c_str()));
        QStringList chips;
        if (eng.use_fds) chips << "FDS";
        if (eng.use_mmc5) chips << "MMC5";
        if (eng.use_fme7) chips << "5B";
        if (eng.use_vrc6) chips << "VRC6";
        if (eng.use_vrc7) chips << "VRC7";
        if (eng.use_n163) chips << "N163";
        QStringList regs;
        if (eng.region_flags & 1) regs << "NTSC";
        if (eng.region_flags & 2) regs << "PAL";
        if (eng.region_flags & 4) regs << "Dendy";
        lChips->setText((chips.isEmpty() ? QString("2A03 only") : "2A03 + " + chips.join(", ")) + "   |   " + regs.join("/"));
        setWindowTitle(QFileInfo(path).fileName() + " - NSFPlay");
        fillList();
        meters->refresh();
        int start = eng.CurrentTrack();
        playTrack(indexOfSong(start));
    }

protected:
    void dragEnterEvent(QDragEnterEvent *e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
    void dropEvent(QDropEvent *e) override {
        auto urls = e->mimeData()->urls();
        if (!urls.isEmpty() && urls[0].isLocalFile()) openFile(urls[0].toLocalFile());
    }
    void closeEvent(QCloseEvent *e) override {
        eng.Stop();
        eng.SaveSettings(iniPath().toStdString());
        QSettings s(iniPath() + ".ui", QSettings::IniFormat);
        s.setValue("lastDir", lastDir);
        s.setValue("geometry", saveGeometry());
        s.setValue("repeat", repeat);
        QMainWindow::closeEvent(e);
    }

public:
    // Self-test: snapshot main window and the dialogs.
    void selfTestSnap(const QString &base) {
        grab().save(base + "-main.png");
        OptionsDialog od(&eng, this);
        od.show();
        od.grab().save(base + "-options.png");
        showMixer();
        mixer->grab().save(base + "-mixer.png");
    }

private:
    int indexOfSong(int song) {
        for (int i = 0; i < (int)eng.tracks.size(); ++i) if (eng.tracks[i].index == song) return i;
        return 0;
    }
    void fillList() {
        list->clear();
        for (auto &t : eng.tracks) {
            auto *it = new QTreeWidgetItem(list);
            it->setText(0, QString::number(t.index + 1));
            QString lbl = QString::fromLocal8Bit(t.label.c_str());
            it->setText(1, lbl.isEmpty() ? QString("Track %1").arg(t.index + 1) : lbl);
            it->setText(2, fmtTime(t.length_ms));
            it->setTextAlignment(2, Qt::AlignRight);
        }
    }
    void playTrack(int row) {
        if (!eng.IsLoaded() || eng.tracks.empty()) return;
        if (row < 0) row = 0;
        if (row >= (int)eng.tracks.size()) row = (int)eng.tracks.size() - 1;
        eng.PlayTrack(eng.tracks[row].index);
        list->setCurrentItem(list->topLevelItem(row));
    }
    int currentRow() { return indexOfSong(eng.CurrentTrack()); }
    void prev() {
        if (!eng.IsLoaded()) return;
        // restart current track if more than 3s in, like most players
        if (eng.PositionMs() > 3000) { playTrack(currentRow()); return; }
        playTrack(currentRow() - 1);
    }
    void next(bool manual) {
        if (!eng.IsLoaded()) return;
        int r = currentRow() + 1;
        if (r >= (int)eng.tracks.size()) {
            if (!manual) { eng.Stop(); return; }
            r = 0;
        }
        playTrack(r);
    }
    void tick() {
        int len = eng.IsLoaded() ? eng.LengthMs() : 0;
        int pos = eng.IsActive() ? eng.PositionMs() : 0;
        if (pos > len && len > 0 && eng.cfg["PLAY_ADVANCE"].GetInt() != 1) pos = len;
        lPos->setText(fmtTime(pos));
        lLen->setText(eng.cfg["PLAY_ADVANCE"].GetInt() == 1 ? QString::fromUtf8("∞") : fmtTime(len));
        if (!seek->isSliderDown()) seek->setValue(len > 0 ? (int)((long long)std::min(pos, len) * 1000 / len) : 0);
        meters->refresh();
        if (eng.ConsumeEnded()) {
            int adv = eng.cfg["PLAY_ADVANCE"].GetInt();
            if (repeat) playTrack(currentRow());
            else if (adv == 0) next(false);
            else eng.Stop();
        }
        QString err = QString::fromStdString(eng.AudioError());
        QString msg;
        if (!err.isEmpty()) msg = err;
        else if (eng.IsLoaded()) {
            int r = currentRow();
            msg = QString("%1  Track %2/%3").arg(eng.IsActive() ? (eng.IsPaused() ? "Paused" : "Playing") : "Stopped")
                      .arg(r + 1).arg(eng.tracks.size());
        }
        if (!msg.isEmpty() && statusBar()->currentMessage() != msg) statusBar()->showMessage(msg);
        if (list->topLevelItemCount() && eng.IsActive()) {
            auto *it = list->topLevelItem(currentRow());
            if (it && list->currentItem() != it && !list->hasFocus()) list->setCurrentItem(it);
        }
    }
    void openDialog() {
        QString f = QFileDialog::getOpenFileName(this, "Open NSF", lastDir,
                                                 "NES sound files (*.nsf *.nsfe *.NSF *.NSFE *.nsf2);;All files (*)");
        if (!f.isEmpty()) openFile(f);
    }
    void showInfo() {
        if (!eng.IsLoaded()) return;
        QDialog d(this);
        d.setWindowTitle("File info");
        d.resize(480, 420);
        auto *l = new QVBoxLayout(&d);
        auto *t = new QPlainTextEdit;
        t->setReadOnly(true);
        QString s;
        s += "File: " + QString::fromStdString(eng.filename) + "\n";
        s += QString("Format: %1 (version %2)\n").arg(eng.is_nsfe ? "NSFe" : "NSF").arg(eng.nsf_version);
        s += "Title: " + QString::fromLocal8Bit(eng.title.c_str()) + "\n";
        s += "Artist: " + QString::fromLocal8Bit(eng.artist.c_str()) + "\n";
        s += "Copyright: " + QString::fromLocal8Bit(eng.copyright.c_str()) + "\n";
        if (!eng.ripper.empty()) s += "Ripper: " + QString::fromLocal8Bit(eng.ripper.c_str()) + "\n";
        s += "Chips: " + lChips->text() + "\n";
        s += QString("Load $%1  Init $%2  Play $%3\n").arg(eng.load_addr, 4, 16, QChar('0'))
                 .arg(eng.init_addr, 4, 16, QChar('0')).arg(eng.play_addr, 4, 16, QChar('0')).toUpper();
        s += QString("Tracks: %1\n\n").arg(eng.tracks.size());
        for (auto &tr : eng.tracks)
            s += QString("%1  %2  %3\n").arg(tr.index + 1, 3).arg(fmtTime(tr.length_ms), 6)
                     .arg(QString::fromLocal8Bit(tr.label.c_str()));
        if (!eng.text.empty()) s += "\n--- Text ---\n" + QString::fromLocal8Bit(eng.text.c_str()) + "\n";
        t->setPlainText(s);
        l->addWidget(t);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        l->addWidget(bb);
        d.exec();
    }
    void showMixer() {
        if (!mixer) mixer = new MixerDialog(&eng, this);
        mixer->syncMask();
        mixer->show();
        mixer->raise();
    }
    void showOptions() {
        OptionsDialog d(&eng, this);
        d.exec();
        if (d.changed) {
            repeat = d.repeat;
            aRepeat->setChecked(repeat);
            QSignalBlocker b(vol);
            vol->setValue(eng.cfg["MASTER_VOLUME"].GetInt());
            if (eng.IsLoaded()) {
                int r = list->currentIndex().row();
                fillList();
                if (r >= 0) list->setCurrentItem(list->topLevelItem(r));
            }
            eng.SaveSettings(iniPath().toStdString());
            if (mixer) { delete mixer; mixer = nullptr; }
        }
    }
    bool exportOne(int row, const QString &out, QProgressDialog &pd, int k, int n) {
        std::string err;
        bool ok = eng.ExportWav(eng.tracks[row].index, out.toStdString(), err, [&](double f) {
            pd.setValue((int)((k + f) * 1000 / n));
            QApplication::processEvents();
            return !pd.wasCanceled();
        });
        if (!ok && !pd.wasCanceled())
            QMessageBox::warning(this, "Export", QString::fromStdString(err));
        return ok;
    }
    QString baseName() { return QFileInfo(QString::fromStdString(eng.filename)).completeBaseName(); }
    void exportWav() {
        if (!eng.IsLoaded()) return;
        int row = currentRow();
        QString def = QString("%1/%2 - %3.wav").arg(lastDir, baseName()).arg(eng.tracks[row].index + 1, 2, 10, QChar('0'));
        QString f = QFileDialog::getSaveFileName(this, "Export WAV", def, "WAV audio (*.wav)");
        if (f.isEmpty()) return;
        QProgressDialog pd("Rendering...", "Cancel", 0, 1000, this);
        pd.setWindowModality(Qt::WindowModal);
        pd.setMinimumDuration(0);
        if (exportOne(row, f, pd, 0, 1)) statusBar()->showMessage("Exported " + f, 5000);
    }
    void exportAll() {
        if (!eng.IsLoaded()) return;
        QString dir = QFileDialog::getExistingDirectory(this, "Export all tracks to folder", lastDir);
        if (dir.isEmpty()) return;
        int n = (int)eng.tracks.size();
        QProgressDialog pd("Rendering all tracks...", "Cancel", 0, 1000, this);
        pd.setWindowModality(Qt::WindowModal);
        pd.setMinimumDuration(0);
        for (int i = 0; i < n; ++i) {
            QString lbl = QString::fromLocal8Bit(eng.tracks[i].label.c_str());
            lbl.replace(QRegularExpression("[/\\\\:*?\"<>|]"), "_");
            QString f = QString("%1/%2 - %3%4.wav").arg(dir, baseName()).arg(eng.tracks[i].index + 1, 2, 10, QChar('0'))
                            .arg(lbl.isEmpty() ? "" : " - " + lbl);
            pd.setLabelText(QString("Rendering track %1 of %2...").arg(i + 1).arg(n));
            if (!exportOne(i, f, pd, i, n)) return;
        }
        statusBar()->showMessage(QString("Exported %1 tracks to %2").arg(n).arg(dir), 5000);
    }

    Engine eng;
    QLabel *lTitle, *lArtist, *lCopy, *lChips, *lPos, *lLen;
    QTreeWidget *list;
    QSlider *seek, *vol;
    MeterWidget *meters;
    MixerDialog *mixer = nullptr;
    QAction *aOpen, *aPrev, *aPlay, *aPause, *aStop, *aNext, *aRepeat;
    QTimer *timer;
    bool repeat = false;
    QString lastDir;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("nsfplay");
    QApplication::setApplicationDisplayName("NSFPlay");
    QApplication::setDesktopFileName("nsfplay");
    QApplication::setWindowIcon(QIcon(":/nsfplay.png"));
    MainWindow w;
    w.show();
    QStringList args = app.arguments();
    if (args.size() > 1) w.openFile(args[1]);
    // Self-test hook: NSFPLAY_SNAPSHOT=out.png grabs the window after 2.5 s and exits.
    QString snap = qEnvironmentVariable("NSFPLAY_SNAPSHOT");
    if (!snap.isEmpty()) {
        QTimer::singleShot(2500, &w, [&w, snap] {
            w.selfTestSnap(snap);
            QApplication::quit();
        });
    }
    return app.exec();
}
