/*
 * Copyright (c) 2012-2024 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "encodedock.h"
#include "ui_encodedock.h"
#include "dialogs/addencodepresetdialog.h"
#include "dialogs/multifileexportdialog.h"
#include "jobqueue.h"
#include "mltcontroller.h"
#include "mainwindow.h"
#include "settings.h"
#include "qmltypes/qmlapplication.h"
#include "jobs/encodejob.h"
#include "shotcut_mlt_properties.h"
#include "util.h"
#include "dialogs/listselectiondialog.h"
#include "qmltypes/qmlfilter.h"
#include "models/markersmodel.h"

#include <Logger.h>
#include <QtWidgets>
#include <QtXml>
#include <QtMath>
#include <QTimer>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

// formulas to map absolute value ranges to percentages as int
#define TO_ABSOLUTE(min, max, rel) qRound(float(min) + float((max) - (min)) * float(rel) / 100.0f)
#define TO_RELATIVE(min, max, abs) qRound(100.0f * float((abs) - (min)) / float((max) - (min)))
static const int kOpenCaptureFileDelayMs = 1500;
static const int kCustomPresetFileNameRole = Qt::UserRole + 1;
#ifdef Q_OS_WIN
static const QString kNullTarget = "nul";
#else
static const QString kNullTarget = "/dev/null";
#endif

static double getBufferSize(Mlt::Properties &preset, const char *property);

static Mlt::Filter getReframeFilter(Mlt::Service *service)
{
    if (service && service->is_valid())
        for (auto i = 0; i < service->filter_count(); ++i) {
            Mlt::Filter filter(service->filter(i));
            if (!::qstrcmp("reframe", filter.get(kShotcutFilterProperty)) && !filter.get_int("disable"))
                return filter;
        }
    return Mlt::Filter();
}

EncodeDock::EncodeDock(QWidget *parent) :
    QDockWidget(parent),
    ui(new Ui::EncodeDock),
    m_presets(Mlt::Repository::presets()),
    m_immediateJob(0),
    m_profiles(Mlt::Profile::list()),
    m_isDefaultSettings(true),
    m_fps(0.0)
{
    LOG_DEBUG() << "begin";
    initSpecialCodecLists();
    ui->setupUi(this);
    auto tip = ui->widthSpinner->toolTip();
    ui->heightSpinner->setToolTip(tip);
    ui->resolutionComboBox->setToolTip(tip);
    ui->aspectNumSpinner->setToolTip(tip);
    ui->aspectDenSpinner->setToolTip(tip);
    ui->fpsSpinner->setToolTip(tip);
    ui->fpsComboBox->setToolTip(tip);
    ui->stopCaptureButton->hide();
    ui->advancedButton->setChecked(Settings.encodeAdvanced());
    ui->advancedCheckBox->setChecked(Settings.encodeAdvanced());
    on_advancedButton_clicked(ui->advancedButton->isChecked());
#if QT_POINTER_SIZE == 4
    // On 32-bit process, limit multi-threading to mitigate running out of memory.
    ui->parallelCheckbox->setChecked(false);
    ui->parallelCheckbox->setHidden(true);
#else
    ui->parallelCheckbox->setChecked(Settings.encodeParallelProcessing());
    ui->videoCodecThreadsSpinner->setMaximum(QThread::idealThreadCount());
#endif
    if (QThread::idealThreadCount() < 3)
        ui->parallelCheckbox->setHidden(true);
    toggleViewAction()->setIcon(windowIcon());

    connect(ui->videoBitrateCombo, SIGNAL(currentIndexChanged(int)), this,
            SLOT(on_videoBufferDurationChanged()));
    connect(ui->videoBufferSizeSpinner, SIGNAL(valueChanged(double)), this,
            SLOT(on_videoBufferDurationChanged()));

    m_presetsModel.setSourceModel(new QStandardItemModel(this));
    m_presetsModel.setFilterCaseSensitivity(Qt::CaseInsensitive);
    ui->presetsTree->setModel(&m_presetsModel);
    loadPresets();

    // populate the combos
    Mlt::Consumer c(MLT.profile(), "avformat");
    c.set("f", "list");
    c.set("acodec", "list");
    c.set("vcodec", "list");
    c.start();
    c.stop();

    Mlt::Properties *p = new Mlt::Properties(c.get_data("f"));
    ui->formatCombo->blockSignals(true);
    for (int i = 0; i < p->count(); i++) {
        if (ui->formatCombo->findText(p->get(i)) == -1)
            ui->formatCombo->addItem(p->get(i));
    }
    delete p;
    ui->formatCombo->model()->sort(0);
    ui->formatCombo->insertItem(0, tr("Automatic from extension"));
    ui->formatCombo->blockSignals(false);

    p = new Mlt::Properties(c.get_data("acodec"));
    for (int i = 0; i < p->count(); i++)
        ui->audioCodecCombo->addItem(p->get(i));
    delete p;
    ui->audioCodecCombo->model()->sort(0);
    ui->audioCodecCombo->insertItem(0, tr("Default for format"));

    p = new Mlt::Properties(c.get_data("vcodec"));
    for (int i = 0; i < p->count(); i++) {
        if (qstrcmp("nvenc", p->get(i)) // redundant codec names nvenc_...
                && qstrcmp("wrapped_avframe", p->get(i))) // not usable
            ui->videoCodecCombo->addItem(p->get(i));
    }
    delete p;
    ui->videoCodecCombo->model()->sort(0);
    ui->videoCodecCombo->insertItem(0, tr("Default for format"));

    ui->hwencodeCheckBox->setChecked(Settings.encodeUseHardware()
                                     && !Settings.encodeHardware().isEmpty());

    on_resetButton_clicked();

    LOG_DEBUG() << "end";
}

EncodeDock::~EncodeDock()
{
    if (m_immediateJob)
        m_immediateJob->stop();
    delete ui;
    delete m_presets;
    delete m_profiles;
}

void EncodeDock::loadPresetFromProperties(Mlt::Properties &preset)
{
    int audioQuality = -1;
    int videoQuality = -1;
    QStringList other;
    QChar decimalPoint = MLT.decimalPoint();
    QString acodec = QString::fromLatin1(preset.get("acodec"));
    QString vcodec = QString::fromLatin1(preset.get("vcodec"));

    if (ui->hwencodeCheckBox->isChecked()) {
        foreach (const QString &hw, Settings.encodeHardware()) {
            if ((vcodec == "libx264" && hw.startsWith("h264"))
                    || (vcodec == "libx265" && hw.startsWith("hevc"))
                    || (vcodec == "libvpx-vp9" && hw.startsWith("vp9"))
                    || (vcodec == "libsvtav1" && hw.startsWith("av1"))) {
                vcodec = hw;
                break;
            }
        }
    }

    ui->disableAudioCheckbox->setChecked(preset.get_int("an"));
    ui->disableVideoCheckbox->setChecked(preset.get_int("vn"));
    m_extension.clear();

    // Default the HEVC crf to 28 per libx265 default.
    if (vcodec == "libx265" || vcodec.contains("hevc")) {
        ui->videoQualitySpinner->setValue(45);

        // Apple needs codec ID hvc1 even though technically that is not what we are writing.
        QString f = preset.get("f");
        if (!preset.property_exists("vtag") && (f == "mp4" || f == "mov")) {
            other.append("vtag=hvc1");
        }
    }
    bool qmin_nvenc_amf = preset.get("qmin") && (vcodec.contains("nvenc") || vcodec.endsWith("_amf"));

    for (int i = 0; i < preset.count(); i++) {
        QString name(preset.get_name(i));

        // Convert numeric strings to the current MLT numeric locale.
        QString value = QString::fromUtf8(preset.get(i)).trimmed();
        if (Util::convertNumericString(value, decimalPoint))
            preset.set(name.toUtf8().constData(), value.toUtf8().constData());

        if (name == "f") {
            for (int j = 0; j < ui->formatCombo->count(); j++)
                if (ui->formatCombo->itemText(j) == value) {
                    ui->formatCombo->blockSignals(true);
                    ui->formatCombo->setCurrentIndex(j);
                    ui->formatCombo->blockSignals(false);
                    break;
                }
        } else if (name == "acodec") {
            for (int j = 0; j < ui->audioCodecCombo->count(); j++)
                if (ui->audioCodecCombo->itemText(j) == value)
                    ui->audioCodecCombo->setCurrentIndex(j);
        } else if (name == "vcodec") {
            for (int j = 0; j < ui->videoCodecCombo->count(); j++)
                if (ui->videoCodecCombo->itemText(j) == vcodec)
                    ui->videoCodecCombo->setCurrentIndex(j);
        } else if (name == "channels")
            setAudioChannels( preset.get_int("channels") );
        else if (name == "ar")
            ui->sampleRateCombo->lineEdit()->setText(value);
        else if (name == "ab")
            ui->audioBitrateCombo->lineEdit()->setText(value);
        else if (name == "vb") {
            ui->videoRateControlCombo->setCurrentIndex((preset.get_int("vb") > 0) ? RateControlAverage :
                                                       RateControlQuality);
            ui->videoBitrateCombo->lineEdit()->setText(value);
        } else if (name == "g")
            ui->gopSpinner->setValue(preset.get_int("g"));
        else if (name == "sc_threshold" && !preset.get_int("sc_threshold"))
            ui->strictGopCheckBox->setChecked(true);
        else if (name == "keyint_min" && preset.get_int("keyint_min") == preset.get_int("g"))
            ui->strictGopCheckBox->setChecked(true);
        else if (name == "bf")
            ui->bFramesSpinner->setValue(preset.get_int("bf"));
        else if (name == "deinterlace") {
            ui->scanModeCombo->setCurrentIndex(preset.get_int("deinterlace"));
            on_scanModeCombo_currentIndexChanged(ui->scanModeCombo->currentIndex());
        } else if (name == "progressive") {
            ui->scanModeCombo->setCurrentIndex(preset.get_int("progressive"));
            on_scanModeCombo_currentIndexChanged(ui->scanModeCombo->currentIndex());
        } else if (name == "top_field_first") {
            ui->fieldOrderCombo->setCurrentIndex(preset.get_int("top_field_first"));
        } else if (name == "width") {
            ui->widthSpinner->setValue(preset.get_int("width"));
        } else if (name == "height") {
            ui->heightSpinner->setValue(preset.get_int("height"));
        } else if (name == "aspect") {
            double dar = preset.get_double("aspect");
            switch (int(dar * 100)) {
            case 133:
                ui->aspectNumSpinner->setValue(4);
                ui->aspectDenSpinner->setValue(3);
                break;
            case 177:
                ui->aspectNumSpinner->setValue(16);
                ui->aspectDenSpinner->setValue(9);
                break;
            case 56:
                ui->aspectNumSpinner->setValue(9);
                ui->aspectDenSpinner->setValue(16);
                break;
            default:
                ui->aspectNumSpinner->setValue(dar * 1000);
                ui->aspectDenSpinner->setValue(1000);
                break;
            }
        } else if (name == "r") {
            ui->fpsSpinner->setValue(preset.get_double("r"));
        } else if (name == "frame_rate_num") {
            if (preset.get_int("frame_rate_den"))
                ui->fpsSpinner->setValue(preset.get_double("frame_rate_num") / preset.get_double("frame_rate_den"));
        } else if (name == "pix_fmt") {
            // Handle 10 bit encoding with hardware encoder and GPU Effects
            if (value.contains("p10le")) {
                if (Settings.playerGPU()) {
                    if (value == "yuva444p10le") {
                        other.append("mlt_image_format=rgba");
                    } else {
                        other.append("mlt_image_format=yuv444p10");
                    }
                } else if (!other.contains("mlt_image_format=rgb")) {
                    other.append("mlt_image_format=rgb");
                }
                // Hardware encoder
                if (vcodec.endsWith("_nvenc") || vcodec.endsWith("_qsv")
                        || vcodec.endsWith("_vaapi") || vcodec.endsWith("_videotoolbox")) {
                    value = "p010le";
                }
            }
            if (!value.isEmpty())
                other.append(QString("%1=%2").arg(name, value));
        } else if (name == "pass")
            ui->dualPassCheckbox->setChecked(true);
        else if (name == "v2pass")
            ui->dualPassCheckbox->setChecked(preset.get_int("v2pass"));
        else if (name == "aq") {
            ui->audioRateControlCombo->setCurrentIndex(RateControlQuality);
            audioQuality = preset.get_int("aq");
        } else if (name == "compression_level") {
            ui->audioRateControlCombo->setCurrentIndex(RateControlQuality);
            audioQuality = preset.get_int("compression_level");
        } else if (name == "vbr") {
            // libopus rate mode
            if (value == "off")
                ui->audioRateControlCombo->setCurrentIndex(RateControlConstant);
            else if (value == "constrained")
                ui->audioRateControlCombo->setCurrentIndex(RateControlAverage);
            else
                ui->audioRateControlCombo->setCurrentIndex(RateControlQuality);
        } else if (name == "abr") {
            if (preset.get_int("abr"))
                ui->audioRateControlCombo->setCurrentIndex(RateControlAverage);
        } else if (name == "vq" || name == "vqp" || name == "vglobal_quality" || name == "qscale"
                   || qmin_nvenc_amf || name == "cq" || name == "crf") {
            // On macOS videotoolbox, constant quality is only on Apple Silicon
#if defined(Q_OS_MAC) && !defined(Q_PROCESSOR_ARM)
            ui->videoRateControlCombo->setCurrentIndex(preset.get("vbufsize") ? RateControlConstrained
                                                       : vcodec.endsWith("_videotoolbox") ? RateControlAverage :
                                                       RateControlQuality);
#else
            ui->videoRateControlCombo->setCurrentIndex(preset.get("vbufsize") ? RateControlConstrained :
                                                       RateControlQuality);
#endif
            videoQuality = preset.get_int(name.toUtf8().constData());
        } else if (name == "bufsize" || name == "vbufsize") {
            // traditionally "bufsize" means video only
            if (preset.get("vq") || preset.get("qscale") || preset.get("crf") || qmin_nvenc_amf)
                ui->videoRateControlCombo->setCurrentIndex(RateControlConstrained);
            else
                ui->videoRateControlCombo->setCurrentIndex(RateControlConstant);
            ui->videoBufferSizeSpinner->setValue(getBufferSize(preset, name.toLatin1().constData()));
        } else if (name == "vmaxrate") {
            ui->videoBitrateCombo->lineEdit()->setText(value);
        } else if (name == "threads") {
            // TODO: should we save the thread count and restore it if preset is not 1?
            if (preset.get_int("threads") == 1)
                ui->videoCodecThreadsSpinner->setValue(1);
        } else if (name == "meta.preset.extension") {
            m_extension = value;
        } else if (name == "deinterlace_method" || name == "deinterlacer") {
            name = preset.get("deinterlacer") ? preset.get("deinterlacer") : preset.get("deinterlace_method");
            if (name == "onefield")
                ui->deinterlacerCombo->setCurrentIndex(0);
            else if (name == "linearblend")
                ui->deinterlacerCombo->setCurrentIndex(1);
            else if (name == "yadif-nospatial")
                ui->deinterlacerCombo->setCurrentIndex(2);
            else if (name == "yadif")
                ui->deinterlacerCombo->setCurrentIndex(3);
            else if (name == "bwdif")
                ui->deinterlacerCombo->setCurrentIndex(4);
        } else if (name == "rescale") {
            if (value == "nearest" || value == "neighbor")
                ui->interpolationCombo->setCurrentIndex(0);
            else if (value == "bilinear")
                ui->interpolationCombo->setCurrentIndex(1);
            else if (value == "bicubic")
                ui->interpolationCombo->setCurrentIndex(2);
            else if (value == "hyper" || value == "lanczos")
                ui->interpolationCombo->setCurrentIndex(3);
        } else if (name == "color_range" && (value == "pc" || value == "jpeg")) {
            ui->rangeComboBox->setCurrentIndex(1);
        } else if (name != "an" && name != "vn" && name != "threads"
                   && !(name == "frame_rate_den" && preset.property_exists("frame_rate_num"))
                   && !name.startsWith('_') && !name.startsWith("qp_") && !name.startsWith("meta.preset.")) {
            other.append(QString("%1=%2").arg(name, value));
        }
    }
    filterCodecParams(vcodec, other);
    ui->advancedTextEdit->setPlainText(other.join("\n"));

    // normalize the quality settings
    // quality depends on codec
    if (ui->audioRateControlCombo->currentIndex() == RateControlQuality && audioQuality > -1) {
        const QString &acodec = ui->audioCodecCombo->currentText();
        if (acodec == "libmp3lame") // 0 (best) - 9 (worst)
            ui->audioQualitySpinner->setValue(TO_RELATIVE(9, 0, audioQuality));
        if (acodec == "libvorbis" || acodec == "vorbis" || acodec == "libopus") // 0 (worst) - 10 (best)
            ui->audioQualitySpinner->setValue(TO_RELATIVE(0, 10, audioQuality));
        else
            // aac: 0 (worst) - 500 (best)
            ui->audioQualitySpinner->setValue(TO_RELATIVE(0, 500, audioQuality));
    }
    if (acodec == "vorbis" || acodec == "libvorbis")
        ui->audioRateControlCombo->setCurrentIndex(RateControlAverage);
    if ((ui->videoRateControlCombo->currentIndex() == RateControlQuality
            || ui->videoRateControlCombo->currentIndex() == RateControlConstrained)
            && videoQuality > -1) {
        //val = min + (max - min) * paramval;
        if (vcodec.startsWith("libx264") || vcodec == "libx265" || vcodec.contains("nvenc")
                || vcodec.endsWith("_amf") || vcodec.endsWith("_vaapi")) {
            // 0 (best, 100%) - 51 (worst)
            const auto qmax = QString::fromLatin1(preset.get("vcodec")) == "libsvtav1" ? 63 : 51;
            ui->videoQualitySpinner->setValue(TO_RELATIVE(qmax, 0, videoQuality));
        } else if (vcodec.endsWith("_videotoolbox")) {
#if defined(Q_OS_MAC) && defined(Q_PROCESSOR_ARM)
            ui->videoQualitySpinner->setValue(ui->hwencodeCheckBox->isChecked() ? videoQuality * 55.0 / 23.0 :
                                              videoQuality);
#else
            ui->videoQualitySpinner->setValue(videoQuality);
#endif
        } else if (vcodec.endsWith("_qsv")) {
            // 1 (best, 100%) - 51 (worst)
            ui->videoQualitySpinner->setValue(TO_RELATIVE(51, 1, videoQuality));
        } else if (vcodec.startsWith("libvpx") || vcodec.startsWith("libaom-") || vcodec == "libsvtav1") {
            // 0 (best, 100%) - 63 (worst)
            ui->videoQualitySpinner->setValue(TO_RELATIVE(63, 0, videoQuality));
        } else if (vcodec.startsWith("libwebp")) {
            // 100 (best) - 0 (worst)
            ui->videoQualitySpinner->setValue(TO_RELATIVE(0, 100, videoQuality));
        } else {
            // 1 (best, NOT 100%) - 31 (worst)
            ui->videoQualitySpinner->setValue(TO_RELATIVE(31, 1, videoQuality));
        }
    }
    vcodec = QString::fromLatin1(preset.get("vcodec"));
    auto resetBframes = !vcodec.contains("nvenc") && !vcodec.endsWith("_amf")
                        && !vcodec.endsWith("_qsv");
    onVideoCodecComboChanged(ui->videoCodecCombo->currentIndex(), true, resetBframes);
    on_audioRateControlCombo_activated(ui->audioRateControlCombo->currentIndex());
    on_videoRateControlCombo_activated(ui->videoRateControlCombo->currentIndex());
    if (m_extension.isEmpty()) {
        defaultFormatExtension();
    }
}

bool EncodeDock::isExportInProgress() const
{
    return !m_immediateJob.isNull();
}

bool EncodeDock::isResampleEnabled() const
{
    return ui->resampleButton->isEnabled();
}

void EncodeDock::onProducerOpened()
{
    int index = 0;
    ui->fromCombo->blockSignals(true);
    ui->fromCombo->clear();
    if (MAIN.isMultitrackValid())
        ui->fromCombo->addItem(tr("Timeline"), "timeline");
    if (MAIN.playlist() && MAIN.playlist()->count() > 0) {
        ui->fromCombo->addItem(tr("Playlist"), "playlist");
        ui->fromCombo->addItem(tr("Each Playlist Item"), "batch");
    }
    if (MLT.isClip() && qstrcmp("_hide", MLT.producer()->get("resource"))) {
        ui->fromCombo->addItem(tr("Source"), "clip");
        if (MLT.producer()->get_int(kBackgroundCaptureProperty)
                || MLT.producer()->get_int(kExportFromProperty))
            index = ui->fromCombo->count() - 1;
    } else if (MLT.savedProducer() && MLT.savedProducer()->is_valid()
               && qstrcmp("_hide", MLT.savedProducer()->get("resource"))) {
        ui->fromCombo->addItem(tr("Source"), "clip");
    }
    if (MAIN.isMultitrackValid()) {
        MarkersModel markersModel;
        markersModel.load(MAIN.multitrack());
        const auto markerStr = tr("Marker");
        auto ranges = markersModel.ranges();
        for (auto i = ranges.constBegin(); i != ranges.constEnd(); ++i) {
            QString comboText;
            if (i.value().startsWith(markerStr)) {
                comboText = i.value();
            } else {
                comboText = QString("%1: %2").arg(tr("Marker"), i.value());
            }
            comboText = ui->fromCombo->fontMetrics().elidedText(comboText, Qt::ElideRight, 400);
            ui->fromCombo->addItem(comboText, QString("marker:%1").arg(i.key()));
        }
    }
    ui->fromCombo->blockSignals(false);
    if (!m_immediateJob) {
        ui->fromCombo->setCurrentIndex(index);
        on_fromCombo_currentIndexChanged(index);
    }
    ui->otherTipLabel->setText(tr("You must enter numeric values using '%1' as the decimal point.").arg(
                                   MLT.decimalPoint()));
}

void EncodeDock::loadPresets()
{
    QStandardItemModel *sourceModel = (QStandardItemModel *) m_presetsModel.sourceModel();
    sourceModel->clear();

    QStandardItem *grandParentItem = new QStandardItem(tr("Custom"));
    QStandardItem *parentItem = grandParentItem;
    sourceModel->invisibleRootItem()->appendRow(parentItem);
    QDir dir(Settings.appDataLocation());
    if (dir.cd("presets") && dir.cd("encode")) {
        QStringList entries = dir.entryList(QDir::Files | QDir::NoDotAndDotDot | QDir::Readable);
        foreach (QString name, entries) {
            // Create a category node if the name includes a ).
            QStringList nameParts = name.split(')');
            if (nameParts.count() > 1 && !nameParts[1].isEmpty() && !name.contains('(')) {
                // See if there is already a category node with this name.
                int row;
                for (row = 0; row < grandParentItem->rowCount(); row++) {
                    if (grandParentItem->child(row)->text() == nameParts[0]) {
                        // There is already a category node; use it.
                        parentItem = grandParentItem->child(row);
                        break;
                    }
                }
                if (row == grandParentItem->rowCount()) {
                    // There is no category node yet; create it.
                    parentItem = new QStandardItem(nameParts[0]);
                    parentItem->setData(QString());
                    grandParentItem->appendRow(parentItem);
                }
                // Remove the category from the name.
                nameParts.removeFirst();
            } else {
                parentItem = grandParentItem;
            }
            QStandardItem *item = new QStandardItem(nameParts.join(')'));
            item->setData(name);
            parentItem->appendRow(item);
        }
    }

    grandParentItem = new QStandardItem(tr("Stock"));
    parentItem = grandParentItem;
    sourceModel->invisibleRootItem()->appendRow(parentItem);
    parentItem->appendRow(new QStandardItem(tr("Default")));
    QString prefix("consumer/avformat/");
    if (m_presets && m_presets->is_valid()) {
        for (int j = 0; j < m_presets->count(); j++) {
            QString name(m_presets->get_name(j));
            if (name.startsWith(prefix)) {
                Mlt::Properties preset((mlt_properties) m_presets->get_data(name.toLatin1().constData()));
                if (preset.get_int("meta.preset.hidden"))
                    continue;
                if (preset.get("meta.preset.name"))
                    name = QString::fromUtf8(preset.get("meta.preset.name"));
                else {
                    // use relative path and filename
                    name.remove(0, prefix.length());
                    QStringList textParts = name.split('/');
                    if (textParts.count() > 1) {
                        // if the path is a profile name, then change it to "preset (profile)"
                        QString profile = textParts.at(0);
                        textParts.removeFirst();
                        if (m_profiles->get_data(profile.toLatin1().constData()))
                            name = QString("%1 (%2)").arg(textParts.join('/'), profile);
                    }
                }
                // Create a category node if the name includes a slash.
                QStringList nameParts = name.split('/');
                if (nameParts.count() > 1) {
                    // See if there is already a category node with this name.
                    int row;
                    for (row = 0; row < grandParentItem->rowCount(); row++) {
                        if (grandParentItem->child(row)->text() == nameParts[0]) {
                            // There is already a category node; use it.
                            parentItem = grandParentItem->child(row);
                            break;
                        }
                    }
                    if (row == grandParentItem->rowCount()) {
                        // There is no category node yet; create it.
                        parentItem = new QStandardItem(nameParts[0]);
                        grandParentItem->appendRow(parentItem);
                    }
                    // Remove the category from the name.
                    nameParts.removeFirst();
                } else {
                    parentItem = grandParentItem;
                }
                QStandardItem *item = new QStandardItem(nameParts.join('/'));
                item->setData(QString(m_presets->get_name(j)));
                if (preset.get("meta.preset.note"))
                    item->setToolTip(QString("<p>%1</p>").arg(QString::fromUtf8(preset.get("meta.preset.note"))));
                parentItem->appendRow(item);
            }
        }
    }
    m_presetsModel.sort(0);
    ui->presetsTree->expandAll();
}

template<typename T>
static void setIfNotSet(Mlt::Properties *properties, const char *name, T value)
{
    if (!properties->get(name))
        properties->set(name, value);
}

Mlt::Properties *EncodeDock::collectProperties(int realtime, bool includeProfile)
{
    Mlt::Properties *p = new Mlt::Properties;
    if (p && p->is_valid()) {
        foreach (QString line, ui->advancedTextEdit->toPlainText().split("\n"))
            p->parse(line.toUtf8().constData());
        if (realtime)
            setIfNotSet(p, "real_time", realtime);
        if (ui->formatCombo->currentIndex() != 0)
            setIfNotSet(p, "f", ui->formatCombo->currentText().toLatin1().constData());
        if (ui->disableAudioCheckbox->isChecked()) {
            setIfNotSet(p, "an", 1);
            setIfNotSet(p, "audio_off", 1);
        } else {
            const QString &acodec = ui->audioCodecCombo->currentText();
            if (ui->audioCodecCombo->currentIndex() > 0)
                setIfNotSet(p, "acodec", ui->audioCodecCombo->currentText().toLatin1().constData());
            if (ui->audioChannelsCombo->currentIndex() == AudioChannels1) {
                setIfNotSet(p, "channels", 1);
                setIfNotSet(p, "channel_layout", "auto");
            } else if (ui->audioChannelsCombo->currentIndex() == AudioChannels2) {
                setIfNotSet(p, "channels", 2);
                setIfNotSet(p, "channel_layout", "auto");
            } else if (ui->audioChannelsCombo->currentIndex() == AudioChannels4) {
                setIfNotSet(p, "channels", 4);
                setIfNotSet(p, "channel_layout", "quad");
            } else {
                setIfNotSet(p, "channels", 6);
                setIfNotSet(p, "channel_layout", "auto");
            }
            setIfNotSet(p, "ar", ui->sampleRateCombo->currentText().toLatin1().constData());
            if (ui->audioRateControlCombo->currentIndex() == RateControlAverage
                    || ui->audioRateControlCombo->currentIndex() == RateControlConstant) {
                setIfNotSet(p, "ab", ui->audioBitrateCombo->currentText().toLatin1().constData());
                if (RateControlConstant == ui->audioRateControlCombo->currentIndex())
                    setIfNotSet(p, "vbr", "off");
                else
                    setIfNotSet(p, "vbr", "constrained");
                if (acodec == "libmp3lame" && ui->audioRateControlCombo->currentIndex() == RateControlAverage)
                    setIfNotSet(p, "abr", "1");
            } else if (acodec == "libopus") {
                setIfNotSet(p, "vbr", "on");
                setIfNotSet(p, "compression_level", TO_ABSOLUTE(0, 10, ui->audioQualitySpinner->value()));
            } else {
                setIfNotSet(p, "vbr", "on");
                double aq = ui->audioQualitySpinner->value();
                if (acodec == "libmp3lame")
                    aq = TO_ABSOLUTE(9, 0, aq);
                else if (acodec == "aac")
                    aq = 0.1 + 1.9 * aq / 100.0;
                else if (acodec == "libvorbis" || acodec == "vorbis")
                    aq = TO_ABSOLUTE(0, 10, aq);
                else
                    aq = TO_ABSOLUTE(0, 500, aq);
                setIfNotSet(p, "aq", aq);
            }
        }
        if (ui->disableVideoCheckbox->isChecked()) {
            setIfNotSet(p, "vn", 1);
            setIfNotSet(p, "video_off", 1);
        } else {
            const QString &vcodec = ui->videoCodecCombo->currentText();
            const QString &vbitrate = ui->videoBitrateCombo->currentText();
            double cvbr = ::atof(vbitrate.toLatin1().constData());
            int vq = ui->videoQualitySpinner->value();

            if (vbitrate.endsWith('M'))
                cvbr *= 1000000.0;
            else if (vbitrate.endsWith('k'))
                cvbr *= 1000.0;
            cvbr *= double(vq) / 100.0;

            if (ui->videoCodecCombo->currentIndex() > 0)
                setIfNotSet(p, "vcodec", vcodec.toLatin1().constData());
            if (vcodec == "libx265") {
                // Most x265 parameters must be supplied through x265-params.
                QString x265params = QString::fromUtf8(p->get("x265-params"));
                switch (ui->videoRateControlCombo->currentIndex()) {
                case RateControlAverage:
                    setIfNotSet(p, "vb", ui->videoBitrateCombo->currentText().toLatin1().constData());
                    break;
                case RateControlConstant: {
                    QString b = ui->videoBitrateCombo->currentText();
                    // x265 does not expect bitrate suffixes and requires Kb/s
                    b.replace('k', "").replace('M', "000");
                    x265params = QString("bitrate=%1:vbv-bufsize=%2:vbv-maxrate=%3:%4")
                                 .arg(b).arg(int(ui->videoBufferSizeSpinner->value() * 8)).arg(b).arg(x265params);
                    setIfNotSet(p, "vb", b.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    break;
                }
                case RateControlQuality: {
                    x265params = QString("crf=%1:%2").arg(TO_ABSOLUTE(51, 0, vq)).arg(x265params);
                    // Also set crf property so that custom presets can be interpreted properly.
                    setIfNotSet(p, "crf", TO_ABSOLUTE(51, 0, vq));
                    break;
                }
                case RateControlConstrained: {
                    QString b = ui->videoBitrateCombo->currentText();
                    // x265 does not expect bitrate suffixes and requires Kb/s
                    b.replace('k', "").replace('M', "000");
                    x265params = QString("crf=%1:vbv-bufsize=%2:vbv-maxrate=%3:%4")
                                 .arg(TO_ABSOLUTE(51, 0, vq)).arg(int(ui->videoBufferSizeSpinner->value() * 8)).arg(b).arg(
                                     x265params);
                    // Also set properties so that custom presets can be interpreted properly.
                    setIfNotSet(p, "crf", TO_ABSOLUTE(51, 0, vq));
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    setIfNotSet(p, "vmaxrate", ui->videoBitrateCombo->currentText().toLatin1().constData());
                    break;
                }
                }
                x265params = QString("keyint=%1:bframes=%2:%3").arg(ui->gopSpinner->value())
                             .arg(ui->bFramesSpinner->value()).arg(x265params);
                if (ui->strictGopCheckBox->isChecked()) {
                    x265params = QString("scenecut=0:%1").arg(x265params);
                    setIfNotSet(p, "sc_threshold", 0);
                }
                if (ui->scanModeCombo->currentIndex() == 0 && !x265params.contains("interlace=")) {
                    x265params = QString("interlace=%1:%2").arg(
                                     ui->fieldOrderCombo->currentIndex() ? "tff" : "bff", x265params);
                }
                // Also set some properties so that custom presets can be interpreted properly.
                setIfNotSet(p, "g", ui->gopSpinner->value());
                setIfNotSet(p, "bf", ui->bFramesSpinner->value());
                p->set("x265-params", x265params.toUtf8().constData());
            } else if (vcodec == "libsvtav1") {
                QString bitrate_text = ui->videoBitrateCombo->currentText();
                int bitrate_kbps = qMax(QString(bitrate_text).replace('k', "").replace('M', "000").toInt(), 1);
                int buffer_bits = qRound(ui->videoBufferSizeSpinner->value() * 1024.0 * 8.0);
                int buffer_ms = qRound(float(buffer_bits) / float(bitrate_kbps));
                QStringList encParams;

                switch (ui->videoRateControlCombo->currentIndex()) {
                case RateControlAverage: {
                    setIfNotSet(p, "vb", bitrate_text.toLatin1().constData());
                    break;
                }
                case RateControlConstant: {
                    encParams << QString("rc=2");
                    encParams << QString("tbr=%1").arg(bitrate_text);
                    encParams << QString("buf-sz=%1").arg(buffer_ms);
                    setIfNotSet(p, "vb", bitrate_text.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", buffer_bits);
                    break;
                }
                case RateControlQuality: {
                    setIfNotSet(p, "crf", TO_ABSOLUTE(63, 0, vq));
                    break;
                }
                case RateControlConstrained: {
                    setIfNotSet(p, "crf", TO_ABSOLUTE(63, 0, vq));
                    setIfNotSet(p, "vmaxrate", bitrate_text.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", buffer_bits); // For UI only; not used by svtav1-params
                    break;
                }
                }

                setIfNotSet(p, "g", ui->gopSpinner->value());

                if (ui->strictGopCheckBox->isChecked()) {
                    encParams << QString("scd=0");
                    encParams << QString("enable-dg=0");
                    setIfNotSet(p, "sc_threshold", 0);
                } else {
                    encParams << QString("scd=1");
                    encParams << QString("enable-dg=1");
                    setIfNotSet(p, "sc_threshold", 1);
                }

                // SVT-AV1 does not offer direct control of the
                // B-frame count, but we can control whether
                // delta frames are bi-directional or not.
                if (ui->bFramesSpinner->value() == 0)
                    encParams << QString("pred-struct=1");
                else
                    encParams << QString("pred-struct=2");
                setIfNotSet(p, "bf", ui->bFramesSpinner->value());

                encParams << QString("lp=%1").arg(ui->videoCodecThreadsSpinner->value());

                // AV1 spec does not support interlacing.
                setIfNotSet(p, "progressive", 1);

                QString origParams = QString::fromUtf8(p->get("svtav1-params"));
                if (!origParams.isEmpty())
                    encParams << origParams;

                p->set("svtav1-params", encParams.join(':').toUtf8().constData());
            } else if (vcodec.contains("nvenc")) {
                switch (ui->videoRateControlCombo->currentIndex()) {
                case RateControlAverage:
                    setIfNotSet(p, "vb", ui->videoBitrateCombo->currentText().toLatin1().constData());
                    break;
                case RateControlConstant: {
                    const QString &b = ui->videoBitrateCombo->currentText();
                    setIfNotSet(p, "cbr", 1);
                    setIfNotSet(p, "vb", b.toLatin1().constData());
                    setIfNotSet(p, "vminrate", b.toLatin1().constData());
                    setIfNotSet(p, "vmaxrate", b.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    break;
                }
                case RateControlQuality: {
                    setIfNotSet(p, "rc", "vbr");
                    setIfNotSet(p, "cq", TO_ABSOLUTE(51, 0, vq));
                    break;
                }
                case RateControlConstrained: {
                    const QString &b = ui->videoBitrateCombo->currentText();
                    setIfNotSet(p, "qmin", TO_ABSOLUTE(51, 0, vq));
                    setIfNotSet(p, "vb", qRound(cvbr));
                    setIfNotSet(p, "vmaxrate", b.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    break;
                }
                }
                if (ui->dualPassCheckbox->isChecked())
                    setIfNotSet(p, "v2pass", 1);
                if (ui->strictGopCheckBox->isChecked()) {
                    setIfNotSet(p, "sc_threshold", 0);
                    setIfNotSet(p, "strict_gop", 1);
                }
                // Also set some properties so that custom presets can be interpreted properly.
                setIfNotSet(p, "g", ui->gopSpinner->value());
                setIfNotSet(p, "bf", ui->bFramesSpinner->value());
            } else if (vcodec.endsWith("_amf")) {
                switch (ui->videoRateControlCombo->currentIndex()) {
                case RateControlAverage:
                    setIfNotSet(p, "vb", ui->videoBitrateCombo->currentText().toLatin1().constData());
                    break;
                case RateControlConstant: {
                    const QString &b = ui->videoBitrateCombo->currentText();
                    setIfNotSet(p, "rc", "cbr");
                    setIfNotSet(p, "vb", b.toLatin1().constData());
                    setIfNotSet(p, "vminrate", b.toLatin1().constData());
                    setIfNotSet(p, "vmaxrate", b.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    break;
                }
                case RateControlQuality: {
                    setIfNotSet(p, "rc", "cqp");
                    setIfNotSet(p, "qp_i", TO_ABSOLUTE(51, 0, vq));
                    setIfNotSet(p, "qp_p", TO_ABSOLUTE(51, 0, vq));
                    setIfNotSet(p, "qp_b", TO_ABSOLUTE(51, 0, vq));
                    setIfNotSet(p, "vq", TO_ABSOLUTE(51, 0, vq));
                    break;
                }
                case RateControlConstrained: {
                    setIfNotSet(p, "rc", "vbr_peak");
                    setIfNotSet(p, "qmin", TO_ABSOLUTE(51, 0, vq));
                    setIfNotSet(p, "vb", qRound(cvbr));
                    setIfNotSet(p, "vmaxrate", vbitrate.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    break;
                }
                }
                if (ui->dualPassCheckbox->isChecked())
                    setIfNotSet(p, "v2pass", 1);
                if (ui->strictGopCheckBox->isChecked()) {
                    setIfNotSet(p, "sc_threshold", 0);
                    setIfNotSet(p, "strict_gop", 1);
                }
                // Also set some properties so that custom presets can be interpreted properly.
                setIfNotSet(p, "g", ui->gopSpinner->value());
                setIfNotSet(p, "bf", ui->bFramesSpinner->value());
            } else {
                switch (ui->videoRateControlCombo->currentIndex()) {
                case RateControlAverage:
                    setIfNotSet(p, "vb", ui->videoBitrateCombo->currentText().toLatin1().constData());
                    break;
                case RateControlConstant: {
                    const QString &b = ui->videoBitrateCombo->currentText();
                    setIfNotSet(p, "vb", b.toLatin1().constData());
                    setIfNotSet(p, "vminrate", b.toLatin1().constData());
                    setIfNotSet(p, "vmaxrate", b.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    break;
                }
                case RateControlQuality: {
                    if (vcodec.startsWith("libx264")) {
                        setIfNotSet(p, "crf", TO_ABSOLUTE(51, 0, vq));
                    } else if (vcodec.startsWith("libvpx") || vcodec.startsWith("libaom-")) {
                        setIfNotSet(p, "crf", TO_ABSOLUTE(63, 0, vq));
                        setIfNotSet(p, "vb", 0); // VP9 needs this to prevent constrained quality mode.
                    } else if (vcodec.endsWith("_vaapi")) {
                        setIfNotSet(p, "vglobal_quality", TO_ABSOLUTE(51, 0, vq));
                        setIfNotSet(p, "vq", TO_ABSOLUTE(51, 0, vq));
                    } else if (vcodec.endsWith("_qsv")) {
                        setIfNotSet(p, "qscale", TO_ABSOLUTE(51, 1, vq));
                    } else if (vcodec.endsWith("_videotoolbox")) {
                        setIfNotSet(p, "qscale", vq);
                    } else if (vcodec.startsWith("libwebp")) {
                        setIfNotSet(p, "qscale", TO_ABSOLUTE(0, 100, vq));
                    } else {
                        setIfNotSet(p, "qscale", TO_ABSOLUTE(31, 1, vq));
                    }
                    break;
                }
                case RateControlConstrained: {
                    if (vcodec.startsWith("libx264")) {
                        setIfNotSet(p, "crf", TO_ABSOLUTE(51, 0, vq));
                    } else if (vcodec.startsWith("libvpx") || vcodec.startsWith("libaom-")) {
                        setIfNotSet(p, "crf", TO_ABSOLUTE(63, 0, vq));
                    } else if (vcodec.endsWith("_qsv")) {
                        setIfNotSet(p, "vb", qRound(cvbr));
                    } else if (vcodec.endsWith("_videotoolbox")) {
                        setIfNotSet(p, "vb", qRound(cvbr));
                        setIfNotSet(p, "vq", vq);
                    } else if (vcodec.endsWith("_vaapi")) {
                        setIfNotSet(p, "vb", vbitrate.toLatin1().constData());
                        setIfNotSet(p, "vglobal_quality", TO_ABSOLUTE(51, 0, vq));
                        setIfNotSet(p, "vq", TO_ABSOLUTE(51, 0, vq));
                    } else if (vcodec.startsWith("libwebp")) {
                        setIfNotSet(p, "qscale", TO_ABSOLUTE(0, 100, vq));
                    } else {
                        setIfNotSet(p, "qscale", TO_ABSOLUTE(31, 1, vq));
                    }
                    setIfNotSet(p, "vmaxrate", vbitrate.toLatin1().constData());
                    setIfNotSet(p, "vbufsize", int(ui->videoBufferSizeSpinner->value() * 8 * 1024));
                    break;
                }
                }
                setIfNotSet(p, "g", ui->gopSpinner->value());
                setIfNotSet(p, "bf", ui->bFramesSpinner->value());
                if (ui->strictGopCheckBox->isChecked()) {
                    if (vcodec.startsWith("libvpx") || vcodec.startsWith("libaom-"))
                        setIfNotSet(p, "keyint_min", ui->gopSpinner->value());
                    else
                        setIfNotSet(p, "sc_threshold", 0);
                }
                if (vcodec.endsWith("_videotoolbox")) {
                    setIfNotSet(p, "pix_fmt", "nv12");
                } else if (vcodec.endsWith("_vaapi")) {
                    setIfNotSet(p, "vprofile", "main");
                }
            }
            if (includeProfile || ui->widthSpinner->value() != MLT.profile().width()) {
                if (ui->widthSpinner->isEnabled())
                    setIfNotSet(p, "width", ui->widthSpinner->value());
            }
            if (includeProfile || ui->heightSpinner->value() != MLT.profile().height()) {
                if (ui->heightSpinner->isEnabled())
                    setIfNotSet(p, "height", ui->heightSpinner->value());
            }
            if (ui->previewScaleCheckBox->isChecked() && !p->get("scale")
                    && Settings.playerPreviewScale() > 0) {
                p->set("scale", double(Settings.playerPreviewScale()) / MLT.profile().height());
            }
            if (includeProfile ||
                    ui->aspectNumSpinner->value() != MLT.profile().display_aspect_num() ||
                    ui->aspectDenSpinner->value() != MLT.profile().display_aspect_den() ||
                    ui->widthSpinner->value() != MLT.profile().width() ||
                    ui->heightSpinner->value() != MLT.profile().height()) {
                if (ui->aspectNumSpinner->isEnabled())
                    setIfNotSet(p, "aspect", double(ui->aspectNumSpinner->value()) / double(
                                    ui->aspectDenSpinner->value()));
            }
            if (includeProfile || ui->scanModeCombo->currentIndex() != MLT.profile().progressive()) {
                setIfNotSet(p, "progressive", ui->scanModeCombo->currentIndex());
            }
            setIfNotSet(p, "top_field_first", ui->fieldOrderCombo->currentIndex());
            switch (ui->deinterlacerCombo->currentIndex()) {
            case 0:
                setIfNotSet(p, "deinterlacer", "onefield");
                break;
            case 1:
                setIfNotSet(p, "deinterlacer", "linearblend");
                break;
            case 2:
                setIfNotSet(p, "deinterlacer", "yadif-nospatial");
                break;
            case 3:
                setIfNotSet(p, "deinterlacer", "yadif");
                break;
            default:
                setIfNotSet(p, "deinterlacer", "bwdif");
                break;
            }
            switch (ui->interpolationCombo->currentIndex()) {
            case 0:
                setIfNotSet(p, "rescale", "nearest");
                break;
            case 1:
                setIfNotSet(p, "rescale", "bilinear");
                break;
            case 2:
                setIfNotSet(p, "rescale", "bicubic");
                break;
            default:
                setIfNotSet(p, "rescale", "hyper");
                break;
            }
            // If the frame rate is not specified in Other.
            if (!p->get("r") && !(p->get("frame_rate_num") && p->get("frame_rate_den"))
                    // Only if the frame rate spinner does not match the profile.
                    && (includeProfile
                        || qRound(ui->fpsSpinner->value() * 1000000.0) != qRound(MLT.profile().fps() * 1000000.0))) {
                int numerator, denominator;
                Util::normalizeFrameRate(ui->fpsSpinner->value(), numerator, denominator);
                p->set("frame_rate_num", numerator);
                p->set("frame_rate_den", denominator);
            }
            if (ui->rangeComboBox->currentIndex()) {
                setIfNotSet(p, "color_range", "pc");
            }
            if (ui->formatCombo->currentText() == "image2")
                setIfNotSet(p, "threads", 1);
            else if (ui->videoCodecThreadsSpinner->value() == 0
                     && !ui->videoCodecCombo->currentText().startsWith("libx264")
                     && ui->videoCodecCombo->currentText() != "libx265"
                     && ui->videoCodecCombo->currentText() != "libsvtav1")
                setIfNotSet(p, "threads", ui->videoCodecThreadsSpinner->maximum() - 1);
            else
                setIfNotSet(p, "threads", ui->videoCodecThreadsSpinner->value());
            if (ui->videoRateControlCombo->currentIndex() != RateControlQuality
                    && !vcodec.contains("nvenc") && !vcodec.endsWith("_amf") && !vcodec.endsWith("_qsv")
                    && !vcodec.endsWith("_videotoolbox") && !vcodec.endsWith("_vaapi") &&
                    ui->dualPassCheckbox->isEnabled() && ui->dualPassCheckbox->isChecked())
                setIfNotSet(p, "pass", 1);
            if (ui->scanModeCombo->currentIndex() == 0 && ui->fieldOrderCombo->currentIndex() == 0
                    && vcodec.startsWith("libx264")) {
                QString x264params = QString::fromUtf8(p->get("x264-params"));
                if (!x264params.contains("bff=") && !x264params.contains("tff=")) {
                    x264params.prepend("bff=1:");
                    p->set("x264-params", x264params.toUtf8().constData());
                }
            }
        }
    }
    return p;
}

void EncodeDock::collectProperties(QDomElement &node, int realtime)
{
    Mlt::Properties *p = collectProperties(realtime);
    if (p && p->is_valid()) {
        for (int i = 0; i < p->count(); i++)
            if (p->get_name(i) && strcmp(p->get_name(i), ""))
                node.setAttribute(p->get_name(i), p->get(i));
    }
    delete p;
}

void EncodeDock::setSubtitleProperties(QDomElement &node, Mlt::Producer *service)
{
    if (!service) {
        return;
    }
    int subIndex = 0;
    for (int i = 0; i < service->filter_count(); i++) {
        QScopedPointer<Mlt::Filter> filter(service->filter(i));
        if (filter  && filter->is_valid() && filter->get("mlt_service") == QString("subtitle_feed")) {
            QString key = QString("subtitle.%1.feed").arg(subIndex);
            node.setAttribute(key, filter->get("feed"));
            key = QString("subtitle.%1.lang").arg(subIndex);
            node.setAttribute(key, filter->get("lang"));
            subIndex++;
        }
    }
}

QPoint EncodeDock::addConsumerElement(Mlt::Producer *service, QDomDocument &dom,
                                      const QString &target, int realtime, int pass)
{
    QDomElement consumerNode = dom.createElement("consumer");
    QDomNodeList profiles = dom.elementsByTagName("profile");
    if (profiles.isEmpty())
        dom.documentElement().insertAfter(consumerNode, dom.documentElement());
    else
        dom.documentElement().insertAfter(consumerNode, profiles.at(profiles.length() - 1));
    consumerNode.setAttribute("mlt_service", "avformat");
    consumerNode.setAttribute("target", pass == 1 ? kNullTarget : target);
    collectProperties(consumerNode, realtime);
    if ("libx265" == ui->videoCodecCombo->currentText()) {
        if (pass == 1 || pass == 2) {
            QString x265params = consumerNode.attribute("x265-params");
            x265params = QString("pass=%1:stats=%2:%3")
                         .arg(pass).arg(QString(target).replace(":", "\\:") + "_2pass.log").arg(x265params);
            consumerNode.setAttribute("x265-params", x265params);
        }
    } else if ("libsvtav1" == ui->videoCodecCombo->currentText()) {
        if (pass == 1 || pass == 2) {
            QStringList encParams;
            encParams << QString("passes=2");
            encParams << QString("pass=%1").arg(pass);
            encParams << QString("stats=%1").arg(QString(target).replace(":", "\\:") + "_2pass.log");
            QString origParams = consumerNode.attribute("svtav1-params");
            if (!origParams.isEmpty())
                encParams << origParams;
            consumerNode.setAttribute("svtav1-params", encParams.join(':'));
        }
    } else {
        if (pass == 1 || pass == 2) {
            consumerNode.setAttribute("pass", pass);
            consumerNode.setAttribute("passlogfile", target + "_2pass.log");
        }
        if (pass == 1) {
            consumerNode.setAttribute("fastfirstpass", 1);
            consumerNode.removeAttribute("acodec");
            consumerNode.setAttribute("an", 1);
        } else {
            consumerNode.removeAttribute("fastfirstpass");
        }
    }
    if (ui->formatCombo->currentIndex() == 0 &&
            ui->audioCodecCombo->currentIndex() == 0 &&
            (target.endsWith(".mp4") || target.endsWith(".mov")))
        consumerNode.setAttribute("strict", "experimental");
    if (!ui->disableSubtitlesCheckbox->isChecked())
        setSubtitleProperties(consumerNode, service);

    return QPoint(consumerNode.hasAttribute("frame_rate_num") ?
                  consumerNode.attribute("frame_rate_num").toInt() : MLT.profile().frame_rate_num(),
                  consumerNode.hasAttribute("frame_rate_den") ?
                  consumerNode.attribute("frame_rate_den").toInt() : MLT.profile().frame_rate_den());
}

MeltJob *EncodeDock::createMeltJob(Mlt::Producer *service, const QString &target, int realtime,
                                   int pass, const QThread::Priority priority)
{
    QString caption = tr("Export File");
    if (Util::warnIfNotWritable(target, this, caption))
        return nullptr;

    // if image sequence, change filename to include number
    QString mytarget = target;
    if (!ui->disableVideoCheckbox->isChecked()) {
        const QString &codec = ui->videoCodecCombo->currentText();
        if (codec == "bmp" || codec == "dpx" || codec == "png" || codec == "ppm" || (codec == "libwebp"
                                                                                     && ui->formatCombo->currentText() == "image2") ||
                codec == "targa" || codec == "tiff" || (codec == "mjpeg"
                                                        && ui->formatCombo->currentText() == "image2")) {
            QFileInfo fi(mytarget);
            mytarget = QString("%1/%2-%05d.%3").arg(fi.path(), fi.baseName(), fi.completeSuffix());
        }
    }

    // Fix in/out points of filters on clip-only project.
    QScopedPointer<Mlt::Producer> tempProducer;
    if (MLT.isSeekable(service) && service->type() != mlt_service_chain_type)
        if (ui->fromCombo->currentData().toString() == "clip"
                || ui->fromCombo->currentData().toString() == "batch") {
            QString xml = MLT.XML(service);
            tempProducer.reset(new Mlt::Producer(MLT.profile(), "xml-string", xml.toUtf8().constData()));
            service = tempProducer.data();
            int producerIn = tempProducer->get_in();
            if (producerIn > 0) {
                int n = tempProducer->filter_count();
                for (int i = 0; i < n; i++) {
                    QScopedPointer<Mlt::Filter> filter(tempProducer->filter(i));
                    if (filter->get_in() > 0)
                        filter->set_in_and_out(filter->get_in() - producerIn, filter->get_out() - producerIn);
                }
            }
        }

    // get temp filename
    auto tmp = new QTemporaryFile {Util::writableTemporaryFile(target)};
    tmp->open();
    QString fileName = tmp->fileName();
    auto isProxy = ui->previewScaleCheckBox->isChecked() && Settings.proxyEnabled();
    MLT.saveXML(fileName, service, false /* without relative paths */, tmp, isProxy);
    tmp->close();

    // parse xml
    QFile f1(fileName);
    f1.open(QIODevice::ReadOnly);
    QXmlStreamReader xmlReader(&f1);
    QDomDocument dom(fileName);
    dom.setContent(&xmlReader, false);
    f1.close();

    // Check if the target file is a member of the project.
    QString xml = dom.toString(0);
    if (xml.contains(QDir::fromNativeSeparators(target))) {
        QMessageBox::warning(this, caption,
                             tr("You cannot write to a file that is in your project.\n"
                                "Try again with a different folder or file name."));
        return nullptr;
    }

    // Add autoclose to playlists.
    QDomNodeList playlists = dom.elementsByTagName("playlist");
    for (auto i = 0; i < playlists.length(); ++i)
        playlists.item(i).toElement().setAttribute("autoclose", 1);

    MeltJob *job = nullptr;

    // Look for the reframe filter
    for (auto i = 0; !job && i < service->filter_count(); ++i) {
        Mlt::Filter filter(service->filter(i));
        if (!::qstrcmp("reframe", filter.get(kShotcutFilterProperty)) && !filter.get_int("disable")) {
            // If it exists, make another XML with new profile based on reframe rect width and height
            auto rect = filter.anim_get_rect("rect", 0);
            LOG_DEBUG() << "FOUND REFRAME" << rect.w << "x" << rect.h << fileName;
            Mlt::Profile reframeProfile;
            reframeProfile.set_explicit(1);
            reframeProfile.set_colorspace(MLT.profile().colorspace());
            reframeProfile.set_frame_rate(MLT.profile().frame_rate_num(), MLT.profile().frame_rate_den());
            reframeProfile.set_sample_aspect(1, 1);
            reframeProfile.set_width(rect.w);
            reframeProfile.set_height(rect.h);
            auto gcd = Util::greatestCommonDivisor(rect.w, rect.h);
            reframeProfile.set_display_aspect(rect.w / gcd, rect.h / gcd);
            LOG_DEBUG() << "REFRAME profile" << reframeProfile.width() << "x" << reframeProfile.height();
            Mlt::Producer producer(reframeProfile, "consumer",
                                   (QString("xml:%1").arg(fileName)).toUtf8().constData());
            // producer.set("autoprofile", 1);
            Mlt::Filter affine(reframeProfile, "affine");
            producer.attach(affine);
            affine.set("transition.valign", "middle");
            affine.set("transition.halign", "center");

            // set affine rect width and height same as video mode
            // compute (negate) new affine rect X and Y for each reframe rect keyframe
            // melt .mlt -attach affine transition.rect='-250 0 1280 720' -consumer avformat:test.mp4 width=404 height=720 display_aspect_num=404 display_aspect_den=720 sample_aspect_num=1 sample_aspect_den=1 an=1
            auto anim = filter.get_anim("rect");
            if (anim->key_count() > 0) {
                // Handle keyframes
                for (auto k = 0; k < anim->key_count(); ++k) {
                    auto frameNum = anim->key_get_frame(k);
                    if (frameNum >= 0) {
                        rect = filter.anim_get_rect("rect", frameNum);
                        rect.x = -rect.x;
                        rect.y = -rect.y;
                        rect.w = MLT.profile().width();
                        rect.h = MLT.profile().height();
                        affine.anim_set("transition.rect", rect, frameNum, 0, anim->key_get_type(k));
                        if (k + 1 == anim->key_count())
                            affine.anim_set("transition.rect", rect, service->get_out());
                    }
                }
            } else {
                // No keyframes
                rect.x = -rect.x;
                rect.y = -rect.y;
                rect.w = MLT.profile().width();
                rect.h = MLT.profile().height();
                affine.set("transition.rect", rect);
            }

            // Serialize
            Mlt::Consumer consumer(reframeProfile, "xml", "string");
            consumer.set("root", "");
            consumer.connect(producer);
            consumer.start();

            // Parse XML to add consumer element
            QXmlStreamReader xmlReader(consumer.get("string"));
            QDomDocument dom;
            dom.setContent(&xmlReader, false);

            auto fps = addConsumerElement(service, dom, mytarget, realtime, pass);
            job = new EncodeJob(QDir::toNativeSeparators(target), dom.toString(2), fps.x(), fps.y(),
                                priority);
            tmp->setParent(job); // job gets ownership to delete object and temp file
        }
    }
    if (!job) {
        auto fps = addConsumerElement(service, dom, mytarget, realtime, pass);
        job = new EncodeJob(QDir::toNativeSeparators(target), dom.toString(2), fps.x(), fps.y(),
                            priority);
        job->setUseMultiConsumer(
            ui->widthSpinner->value() != MLT.profile().width() ||
            ui->heightSpinner->value() != MLT.profile().height() ||
            double(ui->aspectNumSpinner->value()) / double(ui->aspectDenSpinner->value()) != MLT.profile().dar()
            ||
            (ui->fromCombo->currentData().toString() != "clip"
             && qFloor(ui->fpsSpinner->value() * 10000.0) != qFloor(MLT.profile().fps() * 10000.0)));
        delete tmp;
    }

    const auto &from = ui->fromCombo->currentData().toString();
    if (MAIN.isMultitrackValid() && from.startsWith("marker:")) {
        bool ok = false;
        int index = from.mid(7).toInt(&ok);
        if (ok) {
            MarkersModel markersModel;
            markersModel.load(MAIN.multitrack());
            auto marker = markersModel.getMarker(index);
            if (marker.end > marker.start) {
                job->setInAndOut(marker.start, marker.end);
            }
        }
    }
    return job;
}

void EncodeDock::runMelt(const QString &target, int realtime)
{
    Mlt::Producer *service = fromProducer();
    if (!service) {
        // For each playlist item.
        if (MAIN.playlist() && MAIN.playlist()->count() > 0) {
            // Use the first playlist item.
            QScopedPointer<Mlt::ClipInfo> info(MAIN.playlist()->clip_info(0));
            if (!info) return;
            QString xml = MLT.XML(info->producer);
            QScopedPointer<Mlt::Producer> producer(
                new Mlt::Producer(MLT.profile(), "xml-string", xml.toUtf8().constData()));
            producer->set_in_and_out(info->frame_in, info->frame_out);
            m_immediateJob.reset(createMeltJob(producer.data(), target, realtime));
            if (m_immediateJob) {
                m_immediateJob->setIsStreaming(true);
                connect(m_immediateJob.data(), SIGNAL(finished(AbstractJob *, bool, QString)), this,
                        SLOT(onFinished(AbstractJob *, bool)));
                m_immediateJob->start();
            }
            return;
        } else {
            service = MLT.producer();
        }
    }
    m_immediateJob.reset(createMeltJob(service, target, realtime));
    if (m_immediateJob) {
        m_immediateJob->setIsStreaming(true);
        connect(m_immediateJob.data(), SIGNAL(finished(AbstractJob *, bool, QString)), this,
                SLOT(onFinished(AbstractJob *, bool)));
        m_immediateJob->start();
    }
}

class FindAnalysisFilterParser : public Mlt::Parser
{
private:
    QUuid m_uuid;
    QList<Mlt::Filter> m_filters;

public:
    FindAnalysisFilterParser() : Mlt::Parser()
    {}

    QList<Mlt::Filter> &filters()
    {
        return m_filters;
    }

    int on_start_filter(Mlt::Filter *filter)
    {
        QString serviceName = filter->get("mlt_service");
        if (serviceName == "loudness" || serviceName == "vidstab") {
            // If the results property does not exist, empty, or file does not exist.
            QString results = filter->get("results");
            if (results.isEmpty()) {
                if (serviceName == "vidstab") {
                    // vidstab requires a filename, which is only available when using a project folder.
                    QString filename = filter->get("filename");
                    if (filename.isEmpty() || filename.endsWith("vidstab.trf")) {
                        filename = QmlApplication::getNextProjectFile("stab");
                    }
                    if (!filename.isEmpty()) {
                        filter->set("filename", filename.toUtf8().constData());
                        m_filters << Mlt::Filter(*filter);

                        // Touch file to prevent overwriting the same file
                        QFile file(filename);
                        file.open(QIODevice::WriteOnly);
                        file.resize(0);
                        file.close();
                    }
                } else {
                    m_filters << Mlt::Filter(*filter);
                }
            }
        }
        return 0;
    }
    int on_start_producer(Mlt::Producer *)
    {
        return 0;
    }
    int on_end_producer(Mlt::Producer *)
    {
        return 0;
    }
    int on_start_playlist(Mlt::Playlist *)
    {
        return 0;
    }
    int on_end_playlist(Mlt::Playlist *)
    {
        return 0;
    }
    int on_start_tractor(Mlt::Tractor *)
    {
        return 0;
    }
    int on_end_tractor(Mlt::Tractor *)
    {
        return 0;
    }
    int on_start_multitrack(Mlt::Multitrack *)
    {
        return 0;
    }
    int on_end_multitrack(Mlt::Multitrack *)
    {
        return 0;
    }
    int on_start_track()
    {
        return 0;
    }
    int on_end_track()
    {
        return 0;
    }
    int on_end_filter(Mlt::Filter *)
    {
        return 0;
    }
    int on_start_transition(Mlt::Transition *)
    {
        return 0;
    }
    int on_end_transition(Mlt::Transition *)
    {
        return 0;
    }
    int on_start_chain(Mlt::Chain *)
    {
        return 0;
    }
    int on_end_chain(Mlt::Chain *)
    {
        return 0;
    }
    int on_start_link(Mlt::Link *)
    {
        return 0;
    }
    int on_end_link(Mlt::Link *)
    {
        return 0;
    }
};

void EncodeDock::enqueueAnalysis()
{
    Mlt::Producer *producer = fromProducer();
    if (producer && producer->is_valid()) {
        // Look in the producer for all filters requiring analysis.
        FindAnalysisFilterParser parser;
        parser.start(*producer);
        // If there are Filters show a dialog.
        if (parser.filters().size() > 0) {
            QMessageBox dialog(QMessageBox::Question,
                               windowTitle(),
                               tr("Shotcut found filters that require analysis jobs that have not run.\n"
                                  "Do you want to run the analysis jobs now?"),
                               QMessageBox::No | QMessageBox::Yes,
                               this);
            dialog.setDefaultButton(QMessageBox::Yes);
            dialog.setEscapeButton(QMessageBox::No);
            dialog.setWindowModality(QmlApplication::dialogModality());
            if (QMessageBox::Yes == dialog.exec()) {
                // If dialog accepted enqueue jobs.
                foreach (Mlt::Filter filter, parser.filters()) {
                    QScopedPointer<QmlMetadata> meta(new QmlMetadata);
                    QmlFilter qmlFilter(filter, meta.data());
                    bool isAudio = !::qstrcmp("loudness", filter.get("mlt_service"));
                    qmlFilter.analyze(isAudio, false);
                }
            }
        }
    }
}

void EncodeDock::enqueueMelt(const QStringList &targets, int realtime)
{
    Mlt::Producer *service = fromProducer();
    int pass = (ui->videoRateControlCombo->currentIndex() != RateControlQuality
                && !ui->videoCodecCombo->currentText().contains("nvenc")
                && !ui->videoCodecCombo->currentText().endsWith("_amf")
                && !ui->videoCodecCombo->currentText().endsWith("_qsv")
                && !ui->videoCodecCombo->currentText().endsWith("_videotoolbox")
                && !ui->videoCodecCombo->currentText().endsWith("_vaapi")
                &&  ui->dualPassCheckbox->isEnabled() && ui->dualPassCheckbox->isChecked()) ? 1 : 0;
    if (!service) {
        // For each playlist item.
        if (MAIN.playlist() && MAIN.playlist()->count() > 0) {
            int n = MAIN.playlist()->count();
            for (int i = 0; i < n; i++) {
                QScopedPointer<Mlt::ClipInfo> info(MAIN.playlist()->clip_info(i));
                if (!info) continue;
                QString xml = MLT.XML(info->producer);
                QScopedPointer<Mlt::Producer> producer(
                    new Mlt::Producer(MLT.profile(), "xml-string", xml.toUtf8().constData()));
                producer->set_in_and_out(info->frame_in, info->frame_out);
                MeltJob *job = createMeltJob(producer.data(), targets[i], realtime, pass);
                if (job) {
                    JOBS.add(job);
                    if (pass) {
                        job = createMeltJob(producer.data(), targets[i], realtime, 2);
                        if (job)
                            JOBS.add(job);
                    }
                }
            }
        }
    } else {
        MeltJob *job = createMeltJob(service, targets[0], realtime, pass);
        if (job) {
            JOBS.add(job);
            if (pass) {
                job = createMeltJob(service, targets[0], realtime, 2);
                if (job)
                    JOBS.add(job);
            }
        }
    }
}

void EncodeDock::encode(const QString &target)
{
    bool isMulti = true;
    Mlt::Producer *producer = new Mlt::Producer(MLT.producer());
    double volume = MLT.volume();
    MLT.closeConsumer();
    MLT.close();
    producer->seek(0);
    MLT.setProducer(producer, isMulti);
    MLT.consumer()->set("1", "avformat");
    MLT.consumer()->set("1.target", target.toUtf8().constData());
    Mlt::Properties *p = collectProperties(-1);
    if (p && p->is_valid()) {
        for (int i = 0; i < p->count(); i++)
            MLT.consumer()->set(QString("1.%1").arg(p->get_name(i)).toLatin1().constData(), p->get(i));
    }
    delete p;
    if (ui->formatCombo->currentIndex() == 0 &&
            ui->audioCodecCombo->currentIndex() == 0 &&
            (target.endsWith(".mp4") || target.endsWith(".mov")))
        MLT.consumer()->set("1.strict", "experimental");
    MLT.setVolume(volume);
    MLT.play();
}

void EncodeDock::resetOptions()
{
    // Reset all controls to default values.
    ui->formatCombo->setCurrentIndex(0);

    ui->scanModeCombo->setCurrentIndex(1);
    on_scanModeCombo_currentIndexChanged(ui->scanModeCombo->currentIndex());
    ui->deinterlacerCombo->setCurrentIndex(4);
    ui->interpolationCombo->setCurrentIndex(1);
    ui->rangeComboBox->setCurrentIndex(0);

    ui->videoRateControlCombo->setCurrentIndex(RateControlQuality);
    ui->videoBitrateCombo->lineEdit()->setText("12M");
    ui->videoBufferSizeSpinner->setValue(1500);
    ui->gopSpinner->blockSignals(true);
    ui->gopSpinner->setValue(qRound(MLT.profile().fps() * 5.0));
    ui->gopSpinner->blockSignals(false);
    ui->strictGopCheckBox->setChecked(false);
    ui->bFramesSpinner->setValue(3);
    ui->videoCodecThreadsSpinner->setValue(0);
    ui->dualPassCheckbox->setChecked(false);
    ui->disableVideoCheckbox->setChecked(false);

    setAudioChannels(MLT.audioChannels());
    ui->sampleRateCombo->lineEdit()->setText("48000");
    ui->audioRateControlCombo->setCurrentIndex(1);
    ui->audioBitrateCombo->lineEdit()->setText("384k");
    ui->audioQualitySpinner->setValue(50);
    ui->disableAudioCheckbox->setChecked(false);

    on_videoBufferDurationChanged();

    Mlt::Properties preset;
    preset.set("f", "mp4");
    preset.set("movflags", "+faststart");
    preset.set("vcodec", "libx264");
    preset.set("crf", "23");
    preset.set("preset", "fast");
    preset.set("acodec", "aac");
    preset.set("meta.preset.extension", "mp4");
    loadPresetFromProperties(preset);
}

Mlt::Producer *EncodeDock::fromProducer() const
{
    QString from = ui->fromCombo->currentData().toString();
    if (from == "clip")
        return MLT.isClip() ? MLT.producer() : MLT.savedProducer();
    else if (from == "playlist")
        return MAIN.playlist();
    else if (from == "timeline" || from.startsWith("marker:"))
        return MAIN.multitrack();
    else
        return nullptr;
}

void EncodeDock::filterCodecParams(const QString &vcodec, QStringList &other)
{
    QString codecKey;
    QStringList filterKeys;

    if (vcodec == "libx265") {
        codecKey = "x265-params=";
        filterKeys << "bitrate";
        filterKeys << "vbv-bufsize";
        filterKeys << "crf";
        filterKeys << "vbv-maxrate";
        filterKeys << "keyint";
        filterKeys << "scenecut";
        filterKeys << "bframes";
        filterKeys << "interlace";
    } else if (vcodec == "libsvtav1") {
        codecKey = "svtav1-params=";
        filterKeys << "rc";
        filterKeys << "tbr";
        filterKeys << "buf-sz";
        filterKeys << "crf";
        filterKeys << "mbr";
        filterKeys << "keyint";
        filterKeys << "scd";
        filterKeys << "enable-dg";
        filterKeys << "pred-struct";
        filterKeys << "lp";
    } else
        return;

    int i = 0;
    foreach (const QString &line, other) {
        if (line.startsWith(codecKey))
            break;
        ++i;
    }
    if (i >= other.size())
        return;

    QString origParams = other[i].mid(codecKey.length());
    QStringList keepParams;
    foreach (const QString &kv_str, origParams.split(':', Qt::SkipEmptyParts)) {
        QStringList kv_parts = kv_str.split('=', Qt::SkipEmptyParts);
        if (kv_parts.size() > 1) // found key and value
            if (!filterKeys.contains(kv_parts[0]))
                keepParams << kv_str;
    }

    other.removeAt(i);
    if (keepParams.size() > 0)
        other.insert(i, codecKey + keepParams.join(':'));
}

void EncodeDock::onVideoCodecComboChanged(int index, bool ignorePreset, bool resetBframes)
{
    Q_UNUSED(index)
    QString vcodec = ui->videoCodecCombo->currentText();
    if (vcodec.contains("nvenc")) {
        if (!ignorePreset) {
            QString newValue;
            foreach (QString line, ui->advancedTextEdit->toPlainText().split("\n")) {
                if (!line.startsWith("preset=")) {
                    newValue += line;
                    newValue += "\n";
                }
            }
            ui->advancedTextEdit->setPlainText(newValue);
        }
        if (resetBframes)
            ui->bFramesSpinner->setValue(0);
        ui->dualPassCheckbox->setChecked(false);
        ui->dualPassCheckbox->setEnabled(false);
    } else if (vcodec.endsWith("_amf")) {
        if (resetBframes && vcodec.startsWith("hevc_"))
            ui->bFramesSpinner->setValue(0);
        ui->dualPassCheckbox->setChecked(false);
        ui->dualPassCheckbox->setEnabled(false);
    } else if (vcodec.endsWith("_qsv")) {
        if (vcodec.startsWith("hevc_") && !ui->advancedTextEdit->toPlainText().contains("load_plugin="))
            ui->advancedTextEdit->appendPlainText("\nload_plugin=hevc_hw\n");
        if (resetBframes && vcodec.startsWith("av1_"))
            ui->bFramesSpinner->setValue(0);
        ui->dualPassCheckbox->setChecked(false);
        ui->dualPassCheckbox->setEnabled(false);
    } else if (vcodec.endsWith("_videotoolbox")) {
        // According to FFmpeg source code, this is only on Apple Silicon
#if defined(Q_OS_MAC) && !defined(Q_PROCESSOR_ARM)
        if (ui->videoRateControlCombo->currentIndex() == RateControlQuality) {
            ui->videoRateControlCombo->setCurrentIndex(RateControlAverage);
        }
#endif
        ui->dualPassCheckbox->setChecked(false);
        ui->dualPassCheckbox->setEnabled(false);
    } else if (vcodec.endsWith("_vaapi")) {
        ui->dualPassCheckbox->setChecked(false);
        ui->dualPassCheckbox->setEnabled(false);
    } else {
        ui->dualPassCheckbox->setEnabled(true);
    }
    on_videoQualitySpinner_valueChanged(ui->videoQualitySpinner->value());
}

static double getBufferSize(Mlt::Properties &preset, const char *property)
{
    double size = preset.get_double(property);
    const QString &s = preset.get(property);
    // evaluate suffix
    if (s.endsWith('k')) size *= 1000;
    if (s.endsWith('M')) size *= 1000000;
    // convert to KiB
    return double(qRound(size / 1024 / 8 * 100)) / 100;
}

void EncodeDock::on_presetsTree_clicked(const QModelIndex &index)
{
    if (!index.parent().isValid())
        return;
    QString name = m_presetsModel.data(index, kCustomPresetFileNameRole).toString();
    if (!name.isEmpty()) {
        Mlt::Properties *preset;
        if (m_presetsModel.data(index.parent()).toString() == tr("Custom")
                || m_presetsModel.data(index.parent().parent()).toString() == tr("Custom")) {
            ui->removePresetButton->setEnabled(true);
            preset = new Mlt::Properties();
            QDir dir(Settings.appDataLocation());
            if (dir.cd("presets") && dir.cd("encode"))
                preset->load(dir.absoluteFilePath(name).toLatin1().constData());
        } else {
            ui->removePresetButton->setEnabled(false);
            preset = new Mlt::Properties((mlt_properties) m_presets->get_data(name.toLatin1().constData()));
        }
        if (preset->is_valid()) {
            QStringList textParts = name.split('/');

            resetOptions();
            if (textParts.count() > 3) {
                // textParts = ['consumer', 'avformat', profile, preset].
                QString folder = textParts.at(2);
                if (m_profiles->get_data(folder.toLatin1().constData())) {
                    // only set these fields if the folder is a profile
                    Mlt::Profile p(folder.toLatin1().constData());
                    ui->widthSpinner->setValue(p.width());
                    ui->heightSpinner->setValue(p.height());
                    ui->aspectNumSpinner->setValue(p.display_aspect_num());
                    ui->aspectDenSpinner->setValue(p.display_aspect_den());
                    ui->scanModeCombo->setCurrentIndex(p.progressive());
                    ui->fpsSpinner->setValue(p.fps());
                }
            }
            loadPresetFromProperties(*preset);
        }
        delete preset;
    } else {
        on_resetButton_clicked();
    }
}

void EncodeDock::on_presetsTree_activated(const QModelIndex &index)
{
    on_presetsTree_clicked(index);
}

void EncodeDock::on_encodeButton_clicked()
{
    if (!MLT.producer())
        return;
    if (m_immediateJob) {
        m_immediateJob->stop();
        ui->fromCombo->setEnabled(true);
        QTimer::singleShot(kOpenCaptureFileDelayMs, this, SLOT(openCaptureFile()));
        return;
    }
    if (ui->encodeButton->text() == tr("Stop Capture")) {
        MLT.closeConsumer();
        emit captureStateChanged(false);
        ui->streamButton->setDisabled(false);
        QTimer::singleShot(kOpenCaptureFileDelayMs, this, SLOT(openCaptureFile()));
        return;
    }
    bool seekable = MLT.isSeekable(fromProducer());

    if (seekable && checkForMissingFiles()) {
        return;
    }

    MLT.pause();

    QString directory = Settings.encodePath();
    auto projectBaseName = QFileInfo(MAIN.fileName()).completeBaseName();
    if (!m_extension.isEmpty()) {
        if (!MAIN.fileName().isEmpty()) {
            directory += QString("/%1.%2").arg(projectBaseName, m_extension);
        }
    } else {
        if (!MAIN.fileName().isEmpty()) {
            directory += "/" + projectBaseName;
        }
    }

    QString caption = seekable ? tr("Export File") : tr("Capture File");
    if (ui->fromCombo->currentData().toString() == "batch") {
        caption = tr("Export Files");
        MultiFileExportDialog dialog(tr("Export Each Playlist Item"), MAIN.playlist(),
                                     directory, projectBaseName, m_extension, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        m_outputFilenames = dialog.getExportFiles();
    } else {
        QString nameFilter;
        if (!m_extension.isEmpty())
            nameFilter = tr("%1 (*.%2);;All Files (*)").arg(ui->formatCombo->currentText(), m_extension);
        else
            nameFilter = tr("Determined by Export (*)");
        QString newName = QFileDialog::getSaveFileName(this, caption, directory, nameFilter,
                                                       nullptr, Util::getFileDialogOptions());
        if (!newName.isEmpty() && !m_extension.isEmpty()) {
            QFileInfo fi(newName);
            if (fi.suffix().isEmpty()) {
                newName += '.';
                newName += m_extension;
            }
        }
        m_outputFilenames = QStringList(newName);
    }

    if (m_outputFilenames.isEmpty() || m_outputFilenames[0].isEmpty()) {
        return;
    }

    if (Util::warnIfLowDiskSpace(m_outputFilenames[0])) {
        MAIN.showStatusMessage(tr("Export canceled"));
        return;
    }

    QFileInfo fi(m_outputFilenames[0]);
    Settings.setEncodePath(fi.path());

    if (seekable) {
        MLT.purgeMemoryPool();
        // Batch encode
        int threadCount = QThread::idealThreadCount();
        if (threadCount > 2 && ui->parallelCheckbox->isChecked())
            threadCount = qMin(threadCount - 1, 4);
        else
            threadCount = 1;
        enqueueAnalysis();
        enqueueMelt(m_outputFilenames, Settings.playerGPU() ? -1 : -threadCount);
    } else if (MLT.producer()->get_int(kBackgroundCaptureProperty)) {
        // Capture in background
        ui->dualPassCheckbox->setChecked(false);
#if defined(Q_OS_MAC)
        auto priority = QThread::NormalPriority;
#else
        auto priority = QThread::HighPriority;
#endif
        m_immediateJob.reset(createMeltJob(fromProducer(), m_outputFilenames[0], -1, 0, priority));
        if (m_immediateJob) {
            // Close the player's producer to prevent resource contention.
            MAIN.hideProducer();

            m_immediateJob->setIsStreaming(true);
            connect(m_immediateJob.data(), SIGNAL(finished(AbstractJob *, bool, QString)), this,
                    SLOT(onFinished(AbstractJob *, bool)));

            if (MLT.resource().startsWith("gdigrab:") || MLT.resource().startsWith("x11grab:")) {
                ui->stopCaptureButton->show();
            } else {
                ui->encodeButton->setText(tr("Stop Capture"));
                ui->fromCombo->setDisabled(true);
            }
            if (MLT.resource().startsWith("gdigrab:"))
                MAIN.showMinimized();

            int msec = MLT.producer()->get_int(kBackgroundCaptureProperty) * 1000;
            QTimer::singleShot(msec, m_immediateJob.data(), SLOT(start()));
        }
    } else {
        // Capture to file
        // use multi consumer to encode and preview simultaneously
        ui->dualPassCheckbox->setChecked(false);
        ui->encodeButton->setText(tr("Stop Capture"));
        encode(m_outputFilenames[0]);
        emit captureStateChanged(true);
        ui->streamButton->setDisabled(true);
    }
}

void EncodeDock::onAudioChannelsChanged()
{
    setAudioChannels(MLT.audioChannels());
}

void EncodeDock::onProfileChanged()
{
    int width = MLT.profile().width();
    int height = MLT.profile().height();
    double sar = MLT.profile().sar();
    int dar_numerator = width * sar;
    int dar_denominator = height;

    if (height > 0) {
        switch (int(sar * width / height * 100)) {
        case 133:
            dar_numerator = 4;
            dar_denominator = 3;
            break;
        case 177:
            dar_numerator = 16;
            dar_denominator = 9;
            break;
        case 56:
            dar_numerator = 9;
            dar_denominator = 16;
            break;
        }
    }
    ui->widthSpinner->setValue(width);
    ui->heightSpinner->setValue(height);
    ui->aspectNumSpinner->setValue(dar_numerator);
    ui->aspectDenSpinner->setValue(dar_denominator);
    ui->scanModeCombo->setCurrentIndex(MLT.profile().progressive());
    on_scanModeCombo_currentIndexChanged(ui->scanModeCombo->currentIndex());
    ui->fpsSpinner->setValue(MLT.profile().fps());
    ui->fpsSpinner->setMinimum(qRound(MLT.profile().fps() / 3.0));
    if (m_isDefaultSettings) {
        ui->gopSpinner->blockSignals(true);
        ui->gopSpinner->setValue(qRound(MLT.profile().fps() * 5.0));
        ui->gopSpinner->blockSignals(false);
    }
    auto producer = fromProducer();
    ui->reframeButton->setEnabled(producer && producer == MAIN.multitrack());
    auto reframe = getReframeFilter(producer);
    if (reframe.is_valid()) {
        auto rect = reframe.anim_get_rect("rect", 0);
        if (rect.w > 0 && rect.h > 0) {
            ui->widthSpinner->setValue(rect.w);
            ui->heightSpinner->setValue(rect.h);
            auto gcd = Util::greatestCommonDivisor(rect.w, rect.h);
            ui->aspectNumSpinner->setValue(rect.w / gcd);
            ui->aspectDenSpinner->setValue(rect.h / gcd);
            ui->resampleButton->setDisabled(true);
        }
    } else {
        ui->resampleButton->setEnabled(producer && producer->is_valid());
    }
    ui->resampleButton->setChecked(false);
    setResampleEnabled(false);
}

void EncodeDock::on_streamButton_clicked()
{
    if (m_immediateJob) {
        m_immediateJob->stop();
        return;
    }
    if (ui->streamButton->text() == tr("Stop Stream")) {
        bool isMulti = false;
        MLT.closeConsumer();
        MLT.setProducer(MLT.producer(), isMulti);
        MLT.play();
        ui->streamButton->setText(tr("Stream"));
        emit captureStateChanged(false);
        ui->encodeButton->setDisabled(false);
        return;
    }
    QInputDialog dialog(this);
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setWindowTitle(tr("Stream"));
    dialog.setLabelText(
        tr("Enter the network protocol scheme, address, port, and parameters as an URL:"));
    dialog.setTextValue("udp://224.224.224.224:1234?pkt_size=1316&reuse=1");
    dialog.setWindowModality(QmlApplication::dialogModality());
    int r = dialog.exec();
    QString url = dialog.textValue();
    if (r == QDialog::Accepted && !url.isEmpty()) {
        MLT.pause();
        ui->dualPassCheckbox->setChecked(false);
        ui->streamButton->setText(tr("Stop Stream"));
        if (MLT.isSeekable())
            // Stream in background
            runMelt(url, 1);
        else if (MLT.producer()->get_int(kBackgroundCaptureProperty)) {
            // Stream Shotcut screencast
            MLT.stop();
            runMelt(url, 1);
            ui->stopCaptureButton->show();
        } else {
            // Live streaming in foreground
            encode(url);
            emit captureStateChanged(true);
            ui->encodeButton->setDisabled(true);
        }
        m_outputFilenames.clear();
    }
}

void EncodeDock::on_addPresetButton_clicked()
{
    QScopedPointer<Mlt::Properties> data(collectProperties(0, true));
    AddEncodePresetDialog dialog(this);
    QStringList ls;


    if (data && data->is_valid()) {
        // Revert collectProperties() overwriting user-specified advanced options (x265-params).
        foreach (QString line, ui->advancedTextEdit->toPlainText().split("\n"))
            data->parse(line.toUtf8().constData());

        for (int i = 0; i < data->count(); i++)
            if (strlen(data->get_name(i)) > 0)
                ls << QString("%1=%2").arg(data->get_name(i), data->get(i));
    }

    dialog.setWindowTitle(tr("Add Export Preset"));
    dialog.setProperties(ls.join("\n"));
    if (dialog.exec() == QDialog::Accepted) {
        QString preset = dialog.presetName();
        QDir dir(Settings.appDataLocation());
        QString subdir("encode");

        if (!preset.isEmpty()) {
            if (!dir.exists())
                dir.mkpath(dir.path());
            if (!dir.cd("presets")) {
                if (dir.mkdir("presets"))
                    dir.cd("presets");
            }
            if (!dir.cd(subdir)) {
                if (dir.mkdir(subdir))
                    dir.cd(subdir);
            }
            {
                QFile f(dir.filePath(preset));
                if (f.open(QIODevice::WriteOnly | QIODevice::Text))
                    f.write(dialog.properties().toUtf8());
            }

            // add the preset and select it
            loadPresets();
            QModelIndex parentIndex = m_presetsModel.index(0, 0);
            int n = m_presetsModel.rowCount(parentIndex);
            for (int i = 0; i < n; i++) {
                QModelIndex index = m_presetsModel.index(i, 0, parentIndex);
                if (m_presetsModel.data(index, kCustomPresetFileNameRole).toString() == preset) {
                    ui->presetsTree->setCurrentIndex(index);
                    break;
                }
            }
        }
    }
}

void EncodeDock::on_removePresetButton_clicked()
{
    QModelIndex index = ui->presetsTree->currentIndex();
    QString preset = m_presetsModel.data(index, kCustomPresetFileNameRole).toString();
    QMessageBox dialog(QMessageBox::Question,
                       tr("Delete Preset"),
                       tr("Are you sure you want to delete %1?").arg(preset),
                       QMessageBox::No | QMessageBox::Yes,
                       this);
    dialog.setDefaultButton(QMessageBox::Yes);
    dialog.setEscapeButton(QMessageBox::No);
    dialog.setWindowModality(QmlApplication::dialogModality());
    int result = dialog.exec();
    if (result == QMessageBox::Yes) {
        QDir dir(Settings.appDataLocation());
        if (dir.cd("presets") && dir.cd("encode")) {
            dir.remove(preset);
            m_presetsModel.removeRow(index.row(), index.parent());
        }
    }
}

void EncodeDock::onFinished(AbstractJob *job, bool isSuccess)
{
    Q_UNUSED(job)
    Q_UNUSED(isSuccess)

    on_fromCombo_currentIndexChanged(0);
    ui->streamButton->setText(tr("Stream"));
    m_immediateJob.reset();
    emit captureStateChanged(false);
    ui->encodeButton->setDisabled(false);
}

void EncodeDock::on_stopCaptureButton_clicked()
{
    ui->stopCaptureButton->hide();
    if (m_immediateJob)
        m_immediateJob->stop();
    if (!m_outputFilenames.isEmpty())
        QTimer::singleShot(kOpenCaptureFileDelayMs, this, SLOT(openCaptureFile()));
}

void EncodeDock::on_videoRateControlCombo_activated(int index)
{
    switch (index) {
    case RateControlAverage:
        ui->videoBitrateCombo->show();
        ui->videoBufferSizeSpinner->hide();
        ui->videoQualitySpinner->hide();
        ui->dualPassCheckbox->show();
        ui->videoBitrateLabel->show();
        ui->videoBitrateSuffixLabel->show();
        ui->videoBufferSizeLabel->hide();
        ui->videoBufferSizeSuffixLabel->hide();
        ui->videoQualityLabel->hide();
        ui->videoQualitySuffixLabel->hide();
        break;
    case RateControlConstant:
        ui->videoBitrateCombo->show();
        ui->videoBufferSizeSpinner->show();
        ui->videoQualitySpinner->hide();
        ui->dualPassCheckbox->show();
        ui->videoBitrateLabel->show();
        ui->videoBitrateSuffixLabel->show();
        ui->videoBufferSizeLabel->show();
        ui->videoBufferSizeSuffixLabel->show();
        ui->videoQualityLabel->hide();
        ui->videoQualitySuffixLabel->hide();
        break;
    case RateControlQuality:
        ui->videoBitrateCombo->hide();
        ui->videoBufferSizeSpinner->hide();
        ui->videoQualitySpinner->show();
        ui->dualPassCheckbox->hide();
        ui->videoBitrateLabel->hide();
        ui->videoBitrateSuffixLabel->hide();
        ui->videoBufferSizeLabel->hide();
        ui->videoBufferSizeSuffixLabel->hide();
        ui->videoQualityLabel->show();
        ui->videoQualitySuffixLabel->show();
        break;
    case RateControlConstrained:
        ui->videoBitrateCombo->show();
        ui->videoBufferSizeSpinner->show();
        ui->videoQualitySpinner->show();
        ui->dualPassCheckbox->show();
        ui->videoBitrateLabel->show();
        ui->videoBitrateSuffixLabel->show();
        ui->videoBufferSizeLabel->show();
        ui->videoBufferSizeSuffixLabel->show();
        ui->videoQualityLabel->show();
        ui->videoQualitySuffixLabel->show();
        break;
    }
    on_videoQualitySpinner_valueChanged(ui->videoQualitySpinner->value());
}

void EncodeDock::on_audioRateControlCombo_activated(int index)
{
    switch (index) {
    case RateControlAverage:
        ui->audioBitrateCombo->show();
        ui->audioQualitySpinner->hide();
        ui->audioBitrateLabel->show();
        ui->audioBitrateSuffixLabel->show();
        ui->audioQualityLabel->hide();
        ui->audioQualitySuffixLabel->hide();
        break;
    case RateControlConstant:
        ui->audioBitrateCombo->show();
        ui->audioQualitySpinner->hide();
        ui->audioBitrateLabel->show();
        ui->audioBitrateSuffixLabel->show();
        ui->audioQualityLabel->hide();
        ui->audioQualitySuffixLabel->hide();
        break;
    case RateControlQuality:
        ui->audioBitrateCombo->hide();
        ui->audioQualitySpinner->show();
        ui->audioBitrateLabel->hide();
        ui->audioBitrateSuffixLabel->hide();
        ui->audioQualityLabel->show();
        ui->audioQualitySuffixLabel->show();
        break;
    }
}

void EncodeDock::on_scanModeCombo_currentIndexChanged(int index)
{
    if (index == 0) { // Interlaced
        ui->fieldOrderCombo->removeItem(2); // None, if it exists
    } else { // Progressive
        if (ui->fieldOrderCombo->count() < 3)
            ui->fieldOrderCombo->addItem(tr("None"));
        ui->fieldOrderCombo->setCurrentIndex(ui->fieldOrderCombo->count() - 1);
    }
    ui->fieldOrderCombo->setDisabled(index);
}

void EncodeDock::on_presetsSearch_textChanged(const QString &search)
{
    m_presetsModel.setFilterFixedString(search);
    if (search.isEmpty())
        ui->presetsTree->expandAll();
}

bool PresetsProxyModel::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const
{
    QModelIndex index = sourceModel()->index(source_row, 0, source_parent);

    // Show categories with descendants that match.
    for (int i = 0; i < sourceModel()->rowCount(index); i++)
        if (filterAcceptsRow(i, index)) return true;

    return sourceModel()->data(index).toString().contains(filterRegularExpression()) ||
           sourceModel()->data(index, Qt::ToolTipRole).toString().contains(filterRegularExpression());
}

void EncodeDock::on_resetButton_clicked()
{
    m_isDefaultSettings = true;
    resetOptions();
    onProfileChanged();
    ui->presetsTree->setCurrentIndex(QModelIndex());
}

void EncodeDock::openCaptureFile()
{
    MAIN.open(m_outputFilenames[0]);
}

void EncodeDock::on_formatCombo_currentIndexChanged(int index)
{
    Q_UNUSED(index);
    m_extension.clear();
    if (index > 0)
        defaultFormatExtension();
}

void EncodeDock::on_videoBufferDurationChanged()
{
    QString vb = ui->videoBitrateCombo->currentText();
    vb.replace('k', "").replace('M', "000");
    double duration = (double)ui->videoBufferSizeSpinner->value() * 8.0 / vb.toDouble();
    QString label = QString(tr("KiB (%1s)")).arg(duration);
    ui->videoBufferSizeSuffixLabel->setText(label);
}

void EncodeDock::on_gopSpinner_valueChanged(int value)
{
    Q_UNUSED(value);
    m_isDefaultSettings = false;
}

void EncodeDock::on_fromCombo_currentIndexChanged(int index)
{
    Q_UNUSED(index)
    auto producer = fromProducer();
    ui->reframeButton->setEnabled(producer && producer == MAIN.multitrack());
    ui->resampleButton->setEnabled(!getReframeFilter(producer).is_valid());
    if (MLT.isSeekable(producer))
        ui->encodeButton->setText(tr("Export File"));
    else
        ui->encodeButton->setText(tr("Capture File"));
}

void EncodeDock::on_videoCodecCombo_currentIndexChanged(int index)
{
    auto lossy = !m_losslessVideoCodecs.contains(ui->videoCodecCombo->currentText());
    ui->videoRateControlCombo->setEnabled(lossy);
    ui->videoBitrateCombo->setEnabled(lossy);
    ui->videoBufferSizeSpinner->setEnabled(lossy);
    ui->videoQualitySpinner->setEnabled(lossy);
    auto intraOnly = m_intraOnlyCodecs.contains(ui->videoCodecCombo->currentText());
    ui->gopSpinner->setEnabled(!intraOnly);
    ui->strictGopCheckBox->setEnabled(!intraOnly);
    ui->bFramesSpinner->setEnabled(!intraOnly);
    onVideoCodecComboChanged(index);
}

void EncodeDock::on_audioCodecCombo_currentIndexChanged(int index)
{
    Q_UNUSED(index)
    auto lossy = !m_losslessAudioCodecs.contains(ui->audioCodecCombo->currentText());
    ui->audioRateControlCombo->setEnabled(lossy);
    ui->audioBitrateCombo->setEnabled(lossy);
    ui->audioQualitySpinner->setEnabled(lossy);
    on_audioQualitySpinner_valueChanged(ui->audioQualitySpinner->value());
}

void EncodeDock::setAudioChannels( int channels )
{
    switch (channels) {
    case 1:
        ui->audioChannelsCombo->setCurrentIndex(AudioChannels1);
        break;
    case 2:
        ui->audioChannelsCombo->setCurrentIndex(AudioChannels2);
        break;
    case 4:
        ui->audioChannelsCombo->setCurrentIndex(AudioChannels4);
        break;
    case 6:
        ui->audioChannelsCombo->setCurrentIndex(AudioChannels6);
        break;
    }
}

void EncodeDock::on_widthSpinner_editingFinished()
{
    ui->widthSpinner->setValue(Util::coerceMultiple(ui->widthSpinner->value()));
}

void EncodeDock::on_heightSpinner_editingFinished()
{
    ui->heightSpinner->setValue(Util::coerceMultiple(ui->heightSpinner->value()));
}

void EncodeDock::on_advancedButton_clicked(bool checked)
{
    ui->advancedCheckBox->setVisible(checked);
    ui->streamButton->setVisible(false);
    ui->formatLabel->setVisible(checked);
    ui->formatCombo->setVisible(checked);
    ui->tabWidget->setVisible(checked);
    ui->helpLabel->setVisible(!checked);
}

static QStringList codecs()
{
    QStringList codecs;
#if defined(Q_OS_WIN)
    codecs << "h264_nvenc";
    codecs << "hevc_nvenc";
    codecs << "av1_nvenc";
    codecs << "h264_amf";
    codecs << "hevc_amf";
    codecs << "av1_amf";
    codecs << "h264_qsv";
    codecs << "hevc_qsv";
    codecs << "vp9_qsv";
    codecs << "av1_qsv";
#elif defined(Q_OS_MAC)
    codecs << "h264_videotoolbox";
    codecs << "hevc_videotoolbox";
#else
    codecs << "av1_nvenc";
    codecs << "h264_nvenc";
    codecs << "hevc_nvenc";
    codecs << "h264_vaapi";
    codecs << "hevc_vaapi";
    codecs << "av1_vaapi";
#endif
    return codecs;
}

void EncodeDock::on_hwencodeCheckBox_clicked(bool checked)
{
    if (checked && Settings.encodeHardware().isEmpty()) {
        if (!detectHardwareEncoders())
            ui->hwencodeCheckBox->setChecked(false);
    }
    Settings.setEncodeUseHardware(ui->hwencodeCheckBox->isChecked());
    std::unique_ptr<Mlt::Properties> properties {collectProperties(0, true)};
    resetOptions();
    if (properties && properties->is_valid()) {
        QString value = QString::fromLatin1(properties->get("vcodec"));
        if (value.startsWith("av1_")) {
            value = "libsvtav1";
        } else if (value.startsWith("h264_")) {
            value = "libx264";
        } else if (value.startsWith("hevc_")) {
            value = "libx265";
        }
        properties->set("vcodec", value.toUtf8().constData());
        value = QString::fromLatin1(properties->get("pix_fmt"));
        if (value.contains("p010le"))
            properties->set("pix_fmt", "yuv420p10le");
        loadPresetFromProperties(*properties);
    }
}

void EncodeDock::on_hwencodeButton_clicked()
{
    ListSelectionDialog dialog(codecs(), this);
    dialog.setWindowModality(QmlApplication::dialogModality());
    dialog.setWindowTitle(tr("Configure Hardware Encoding"));
    dialog.setSelection(Settings.encodeHardware());
    QPushButton *button = dialog.buttonBox()->addButton(tr("Detect"), QDialogButtonBox::ResetRole);
    connect(button, SIGNAL(clicked()), &dialog, SLOT(reject()));
    connect(button, SIGNAL(clicked()), this, SLOT(detectHardwareEncoders()));

    // Show the dialog.
    if (dialog.exec() == QDialog::Accepted) {
        Settings.setEncodeHardware(dialog.selection());
        if (dialog.selection().isEmpty()) {
            ui->hwencodeCheckBox->setChecked(false);
            Settings.setEncodeUseHardware(false);
        }
    }
}

void EncodeDock::on_advancedCheckBox_clicked(bool checked)
{
    Settings.setEncodeAdvanced(checked);
}

void EncodeDock::on_fpsSpinner_editingFinished()
{
    if (ui->fpsSpinner->value() != m_fps) {
        const QString caption(tr("Export Frames/sec"));
        if (ui->fpsSpinner->value() == 23.98 || ui->fpsSpinner->value() == 23.976) {
            Util::showFrameRateDialog(caption, 24000, ui->fpsSpinner, this);
        } else if (ui->fpsSpinner->value() == 29.97) {
            Util::showFrameRateDialog(caption, 30000, ui->fpsSpinner, this);
        } else if (ui->fpsSpinner->value() == 47.95) {
            Util::showFrameRateDialog(caption, 48000, ui->fpsSpinner, this);
        } else if (ui->fpsSpinner->value() == 59.94) {
            Util::showFrameRateDialog(caption, 60000, ui->fpsSpinner, this);
        }
        m_fps = ui->fpsSpinner->value();
    }
}

void EncodeDock::on_fpsComboBox_activated(int arg1)
{
    if (!ui->fpsComboBox->itemText(arg1).isEmpty())
        ui->fpsSpinner->setValue(ui->fpsComboBox->itemText(arg1).toDouble());
}

void EncodeDock::on_videoQualitySpinner_valueChanged(int vq)
{
    const QString &vcodec = ui->videoCodecCombo->currentText();
    QString s;
    if (vcodec.startsWith("libx264") || vcodec == "libx265") {
        s = QString("crf=%1").arg(TO_ABSOLUTE(51, 0, vq));
    } else if (vcodec.startsWith("libvpx") || vcodec.startsWith("libaom-") || vcodec == "libsvtav1") {
        s = QString("crf=%1").arg(TO_ABSOLUTE(63, 0, vq));
    } else if (vcodec.contains("nvenc")) {
        vq = TO_ABSOLUTE(51, 0, vq);
        if (ui->videoRateControlCombo->currentIndex() == RateControlQuality)
            s = QString("cq=%1 %2").arg(vq).arg(vq == 0 ? tr("(auto)") : "");
        else
            s = QString("qmin=%1").arg(TO_ABSOLUTE(51, 0, vq));
    } else if (vcodec.endsWith("_amf")) {
        s = QString("qp_i=qp_p=qp_b=%1").arg(TO_ABSOLUTE(51, 0, vq));
    } else if (vcodec.endsWith("_vaapi")) {
        s = QString("vglobal_quality=%1").arg(TO_ABSOLUTE(51, 0, vq));
    } else if (vcodec.endsWith("_qsv")) {
        s = QString("qscale=%1").arg(TO_ABSOLUTE(51, 1, vq));
    } else if (vcodec.endsWith("_videotoolbox")) {
        s = QString("qscale=%1").arg(vq);
    } else if (vcodec.startsWith("libwebp")) {
        s = QString("qscale=%1").arg(TO_ABSOLUTE(0, 100, vq));
    } else {
        s = QString("qscale=%1").arg(TO_ABSOLUTE(31, 1, vq));
    }
    ui->videoQualitySuffixLabel->setText(s);
}

void EncodeDock::on_audioQualitySpinner_valueChanged(int aq)
{
    const QString &acodec = ui->audioCodecCombo->currentText();
    QString s("aq=%1");
    if (acodec == "aac") {
        auto a = 0.1 + 1.9 * aq / 100.0;
        ui->audioQualitySuffixLabel->setText(s.arg(a));
    } else {
        if (acodec == "libmp3lame")
            aq = TO_ABSOLUTE(9, 0, aq);
        else if (acodec == "libvorbis" || acodec == "vorbis")
            aq = TO_ABSOLUTE(0, 10, aq);
        else
            aq = TO_ABSOLUTE(0, 500, aq);
        ui->audioQualitySuffixLabel->setText(s.arg(aq));
    }
}

void EncodeDock::on_parallelCheckbox_clicked(bool checked)
{
    Settings.setEncodeParallelProcessing(checked);
}

bool EncodeDock::detectHardwareEncoders()
{
    MAIN.showStatusMessage(tr("Detecting hardware encoders..."));
    QStringList hwlist;
    QFileInfo ffmpegPath(qApp->applicationDirPath(), "ffmpeg");
    foreach (const QString &codec, codecs()) {
        LOG_INFO() << "checking for" << codec;
        QProcess proc;
        QStringList args;
        args << "-hide_banner" << "-f" << "lavfi" << "-i" << "color=s=640x360" << "-frames" << "1" << "-an";
        if (codec.endsWith("_vaapi"))
            args << "-init_hw_device" << "vaapi=vaapi0:" << "-filter_hw_device" << "vaapi0" << "-vf" <<
                 "format=nv12,hwupload";
        else if (codec == "hevc_qsv")
            args << "-load_plugin" << "hevc_hw";
        else if (codec.endsWith("_videotoolbox"))
            args << "-pix_fmt" << "nv12";
        args << "-c:v" << codec << "-f" << "rawvideo" << "pipe:";
        LOG_DEBUG() << ffmpegPath.absoluteFilePath() + " " + args.join(' ');
        proc.setStandardOutputFile(QProcess::nullDevice());
        proc.setReadChannel(QProcess::StandardError);
        proc.start(ffmpegPath.absoluteFilePath(), args, QIODevice::ReadOnly);
        bool started = proc.waitForStarted(2000);
        bool finished = false;
        QCoreApplication::processEvents();
        if (started) {
            finished = proc.waitForFinished(4000);
            QCoreApplication::processEvents();
        }
        if (started && finished && proc.exitStatus() == QProcess::NormalExit && !proc.exitCode()) {
            hwlist << codec;
        } else {
            QString output = proc.readAll();
            foreach (const QString &line, output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts))
                LOG_DEBUG() << line;
        }
    }
    if (hwlist.isEmpty()) {
        MAIN.showStatusMessage(tr("Nothing found"), 10);
    } else {
        MAIN.showStatusMessage(tr("Found %1").arg(hwlist.join(", ")));
        Settings.setEncodeHardware(hwlist);
    }
    return !hwlist.isEmpty();
}

QString &EncodeDock::defaultFormatExtension()
{
    auto format = ui->formatCombo->currentText();
    QFileInfo ffmpegPath(qApp->applicationDirPath(), "ffmpeg");
    QProcess proc;
    QStringList args;
    args << "-hide_banner" << "-h" << format.prepend("muxer=");
    LOG_DEBUG() << ffmpegPath.absoluteFilePath() << args.join(' ');
    proc.setStandardErrorFile(QProcess::nullDevice());
    proc.setReadChannel(QProcess::StandardOutput);
    proc.start(ffmpegPath.absoluteFilePath(), args, QIODevice::ReadOnly);
    bool started = proc.waitForStarted(2000);
    bool finished = false;
    QCoreApplication::processEvents();
    if (started) {
        finished = proc.waitForFinished(4000);
        QCoreApplication::processEvents();
    }
    if (started && finished && proc.exitStatus() == QProcess::NormalExit && !proc.exitCode()) {
        QString output = proc.readAll();
        for (auto &line : output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts)) {
            LOG_DEBUG() << line;
            if (line.startsWith("    Common extensions:")) {
                auto parts = line.split(':').last().split(',');
                m_extension = parts.first().replace('.', "").trimmed();
                LOG_DEBUG() << "extension =" << m_extension;
                break;
            }
        }
    } else {
        LOG_ERROR() << "ffmpeg failed with" << proc.exitCode();
    }
    return m_extension;
}

void EncodeDock::initSpecialCodecLists()
{
    m_intraOnlyCodecs << "a64multi";
    m_intraOnlyCodecs << "a64multi5";
    m_intraOnlyCodecs << "alias_pix";
    m_intraOnlyCodecs << "amv";
    m_intraOnlyCodecs << "apng";
    m_intraOnlyCodecs << "asv1";
    m_intraOnlyCodecs << "asv2";
    m_intraOnlyCodecs << "avrp";
    m_intraOnlyCodecs << "avui";
    m_intraOnlyCodecs << "ayuv";
    m_intraOnlyCodecs << "bitpacked";
    m_intraOnlyCodecs << "bmp";
    m_intraOnlyCodecs << "cljr";
    m_intraOnlyCodecs << "dnxhd";
    m_intraOnlyCodecs << "dpx";
    m_intraOnlyCodecs << "dvvideo";
    m_intraOnlyCodecs << "exr";
    m_intraOnlyCodecs << "ffvhuff";
    m_intraOnlyCodecs << "fits";
    m_intraOnlyCodecs << "gif";
    m_intraOnlyCodecs << "hdr";
    m_intraOnlyCodecs << "huffyuv";
    m_intraOnlyCodecs << "jpeg2000";
    m_intraOnlyCodecs << "jpegls";
    m_intraOnlyCodecs << "ljpeg";
    m_intraOnlyCodecs << "magicyuv";
    m_intraOnlyCodecs << "mjpeg";
    m_intraOnlyCodecs << "mjpeg_vaapi";
    m_intraOnlyCodecs << "pam";
    m_intraOnlyCodecs << "pbm";
    m_intraOnlyCodecs << "pcx";
    m_intraOnlyCodecs << "pfm";
    m_intraOnlyCodecs << "pgm";
    m_intraOnlyCodecs << "pgmyuv";
    m_intraOnlyCodecs << "phm";
    m_intraOnlyCodecs << "png";
    m_intraOnlyCodecs << "ppm";
    m_intraOnlyCodecs << "prores";
    m_intraOnlyCodecs << "prores_aw";
    m_intraOnlyCodecs << "prores_ks";
    m_intraOnlyCodecs << "qoi";
    m_intraOnlyCodecs << "r10k";
    m_intraOnlyCodecs << "r210";
    m_intraOnlyCodecs << "rawvideo";
    m_intraOnlyCodecs << "sgi";
    m_intraOnlyCodecs << "speedhq";
    m_intraOnlyCodecs << "sunrast";
    m_intraOnlyCodecs << "targa";
    m_intraOnlyCodecs << "tiff";
    m_intraOnlyCodecs << "utvideo";
    m_intraOnlyCodecs << "v210";
    m_intraOnlyCodecs << "v308";
    m_intraOnlyCodecs << "v408";
    m_intraOnlyCodecs << "v410";
    m_intraOnlyCodecs << "wbmp";
    m_intraOnlyCodecs << "libwebp_anim";
    m_intraOnlyCodecs << "libwebp";
    m_intraOnlyCodecs << "xbm";
    m_intraOnlyCodecs << "xface";
    m_intraOnlyCodecs << "xwd";
    m_intraOnlyCodecs << "y41p";
    m_intraOnlyCodecs << "yuv4";
    m_intraOnlyCodecs << "zlib";

    m_losslessVideoCodecs << "alias_pix";
    m_losslessVideoCodecs << "apng";
    m_losslessVideoCodecs << "avrp";
    m_losslessVideoCodecs << "avui";
    m_losslessVideoCodecs << "ayuv";
    m_losslessVideoCodecs << "bitpacked";
    m_losslessVideoCodecs << "bmp";
    m_losslessVideoCodecs << "dpx";
    m_losslessVideoCodecs << "ffv1";
    m_losslessVideoCodecs << "ffvhuff";
    m_losslessVideoCodecs << "fits";
    m_losslessVideoCodecs << "flashsv";
    m_losslessVideoCodecs << "gif";
    m_losslessVideoCodecs << "huffyuv";
    m_losslessVideoCodecs << "ljpeg";
    m_losslessVideoCodecs << "magicyuv";
    m_losslessVideoCodecs << "pam";
    m_losslessVideoCodecs << "pbm";
    m_losslessVideoCodecs << "pcx";
    m_losslessVideoCodecs << "pfm";
    m_losslessVideoCodecs << "pgm";
    m_losslessVideoCodecs << "pgmyuv";
    m_losslessVideoCodecs << "phm";
    m_losslessVideoCodecs << "png";
    m_losslessVideoCodecs << "ppm";
    m_losslessVideoCodecs << "qoi";
    m_losslessVideoCodecs << "qtrle";
    m_losslessVideoCodecs << "r10k";
    m_losslessVideoCodecs << "r210";
    m_losslessVideoCodecs << "rawvideo";
    m_losslessVideoCodecs << "sgi";
    m_losslessVideoCodecs << "sunrast";
    m_losslessVideoCodecs << "targa";
    m_losslessVideoCodecs << "tiff";
    m_losslessVideoCodecs << "utvideo";
    m_losslessVideoCodecs << "v210";
    m_losslessVideoCodecs << "v308";
    m_losslessVideoCodecs << "v408";
    m_losslessVideoCodecs << "v410";
    m_losslessVideoCodecs << "wbmp";
    m_losslessVideoCodecs << "wrapped_avframe";
    m_losslessVideoCodecs << "xbm";
    m_losslessVideoCodecs << "xwd";
    m_losslessVideoCodecs << "y41p";
    m_losslessVideoCodecs << "yuv4";
    m_losslessVideoCodecs << "zlib";
    m_losslessVideoCodecs << "zmbv";

    m_losslessAudioCodecs << "alac";
    m_losslessAudioCodecs << "flac";
    m_losslessAudioCodecs << "mlp";
    m_losslessAudioCodecs << "pcm_bluray";
    m_losslessAudioCodecs << "pcm_dvd";
    m_losslessAudioCodecs << "pcm_f32be";
    m_losslessAudioCodecs << "pcm_f32le";
    m_losslessAudioCodecs << "pcm_f64be";
    m_losslessAudioCodecs << "pcm_f64le";
    m_losslessAudioCodecs << "pcm_s16be";
    m_losslessAudioCodecs << "pcm_s16be_planar";
    m_losslessAudioCodecs << "pcm_s16le";
    m_losslessAudioCodecs << "pcm_s16le_planar";
    m_losslessAudioCodecs << "pcm_s24be";
    m_losslessAudioCodecs << "pcm_s24daud";
    m_losslessAudioCodecs << "pcm_s24le";
    m_losslessAudioCodecs << "pcm_s24le_planar";
    m_losslessAudioCodecs << "pcm_s32be";
    m_losslessAudioCodecs << "pcm_s32le";
    m_losslessAudioCodecs << "pcm_s32le_planar";
    m_losslessAudioCodecs << "pcm_s64be";
    m_losslessAudioCodecs << "pcm_s64le";
    m_losslessAudioCodecs << "pcm_s8";
    m_losslessAudioCodecs << "pcm_s8_planar";
    m_losslessAudioCodecs << "pcm_u16be";
    m_losslessAudioCodecs << "pcm_u16le";
    m_losslessAudioCodecs << "pcm_u24be";
    m_losslessAudioCodecs << "pcm_u24le";
    m_losslessAudioCodecs << "pcm_u32be";
    m_losslessAudioCodecs << "pcm_u32le";
    m_losslessAudioCodecs << "pcm_u8";
    m_losslessAudioCodecs << "s302m";
    m_losslessAudioCodecs << "truehd";
    m_losslessAudioCodecs << "tta";
}

bool EncodeDock::checkForMissingFiles()
{
    Mlt::Producer *service = fromProducer();
    if (!service) {
        service = MAIN.playlist();
    }
    if (!service) {
        LOG_ERROR() << "Encode: No service to encode";
        return true;
    }
    QScopedPointer<QTemporaryFile> tmp;
    if (MAIN.fileName().isEmpty()) {
        tmp.reset(Util::writableTemporaryFile(QStandardPaths::writableLocation(
                                                  QStandardPaths::AppDataLocation) + "/"));
    } else {
        QFileInfo info(MAIN.fileName());
        QString templateFileName = QString("%1.XXXXXX").arg(QCoreApplication::applicationName());
        tmp.reset(new QTemporaryFile(info.dir().filePath(templateFileName)));
    }
    tmp->open();
    QString fileName = tmp->fileName();
    auto isProxy = ui->previewScaleCheckBox->isChecked() && Settings.proxyEnabled();
    MLT.saveXML(fileName, service, false /* without relative paths */, tmp.get(), isProxy);
    tmp->close();
    MltXmlChecker checker;
    if (checker.check(fileName) != QXmlStreamReader::NoError) {
        LOG_ERROR() << "Encode: Unable to check XML - skipping check";
    } else if (checker.unlinkedFilesModel().rowCount() > 0) {
        QMessageBox dialog(QMessageBox::Critical,
                           qApp->applicationName(),
                           tr("Your project is missing some files.\n\n"
                              "Save your project, close it, and reopen it.\n"
                              "Shotcut will attempt to repair your project."),
                           QMessageBox::Ok | QMessageBox::Ignore,
                           this);
        dialog.setWindowModality(QmlApplication::dialogModality());
        dialog.setDefaultButton(QMessageBox::Ok);
        if (QMessageBox::Ignore == dialog.exec()) {
            return false;
        }
        return true;
    }
    return false;
}

void EncodeDock::on_resolutionComboBox_activated(int arg1)
{
    if (ui->resolutionComboBox->itemText(arg1).isEmpty()) return;
    auto parts = ui->resolutionComboBox->itemText(arg1).split(' ');
    ui->widthSpinner->setValue(parts[0].toInt());
    ui->heightSpinner->setValue(parts[2].toInt());
}

void EncodeDock::on_reframeButton_clicked()
{
    Mlt::Filter filter(MLT.profile(), "mask_start");
    filter.set(kShotcutFilterProperty, "reframe");
    filter.set("filter", "0");
    filter.set("transition.valign", "middle");
    filter.set("transition.halign", "center");
    auto width = qRound(9.0 / 16.0 * MLT.profile().height());
    width += width % 2;
    mlt_rect rect;
    rect.x = qRound(0.5 * (MLT.profile().width() - width));
    rect.y = 0.0;
    rect.w = width;
    rect.h = MLT.profile().height();
    rect.o = 1.0;
    filter.set("rect", rect);
    emit createOrEditFilterOnOutput(&filter);
}


void EncodeDock::on_resampleButton_clicked(bool checked)
{
    if (!Settings.askResample() || ("clip" == ui->fromCombo->currentData().toString()
                                    && !MLT.profile().is_explicit())) {
        setResampleEnabled(checked);
    } else if (checked) {
        QMessageBox dialog(QMessageBox::Question,
                           tr("Resample Export"),
                           tr("<p>Are you sure you want to resample instead of change the <b>Video Mode</b>?</p>"
                              "<p>Changing the aspect ratio adds black bars.</p>"
                              "<p>Increasing resolution or frame rate is limited by <b>Video Mode</b>.</p>"),
                           QMessageBox::No | QMessageBox::Yes,
                           this);
        dialog.addButton(tr("Open Settings > Video Mode"), QMessageBox::ResetRole);
        dialog.setDefaultButton(QMessageBox::Yes);
        dialog.setEscapeButton(QMessageBox::Cancel);
        dialog.setWindowModality(QmlApplication::dialogModality());
        dialog.setCheckBox(new QCheckBox(tr("Do not show this anymore.",
                                            "Resample export warning dialog")));
        switch (dialog.exec()) {
        case QMessageBox::Accepted:
            setResampleEnabled(checked);
            break;
        case QMessageBox::No:
            ui->resampleButton->setChecked(false);
            break;
        default:
            ui->resampleButton->setChecked(false);
            MAIN.showSettingsMenu();
            break;
        }
        if (dialog.checkBox()->isChecked())
            Settings.setAskResample(false);
    } else {
        setResampleEnabled(false);
    }
}

void EncodeDock::setResampleEnabled(bool enabled)
{
    ui->widthSpinner->setEnabled(enabled);
    ui->heightSpinner->setEnabled(enabled);
    ui->resolutionComboBox->setEnabled(enabled);
    ui->aspectNumSpinner->setEnabled(enabled);
    ui->aspectDenSpinner->setEnabled(enabled);
    ui->fpsSpinner->setEnabled(enabled);
    ui->fpsComboBox->setEnabled(enabled);

}
