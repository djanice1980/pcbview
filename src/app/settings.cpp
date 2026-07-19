#include "app/settings.h"

#include <QDir>
#include <QIODevice>
#include <QMetaType>
#include <QStringList>
#include <QVariant>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace pcbview {
namespace {

constexpr const char* kDirName = ".pcbview";

// ---- XML (de)serialisation for QSettings::registerFormat ----------------
//
// The map QSettings hands us is flat: keys are '/'-joined group paths (we use
// no groups today, so they are bare names) and values are QVariants. We emit
// one <setting> element per entry with an explicit type= so the reader can
// round-trip int/bool/double/string, and a nested <item> list for stringlists
// (the recent-files list). Anything exotic degrades to its string form.

bool readXml(QIODevice& device, QSettings::SettingsMap& map) {
    QXmlStreamReader xml(&device);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() ||
            xml.name() != QLatin1String("setting")) {
            continue;
        }
        const QString key = xml.attributes().value("key").toString();
        const QString type = xml.attributes().value("type").toString();
        if (key.isEmpty()) continue;

        if (type == QLatin1String("stringlist")) {
            QStringList items;
            // Consume children until this <setting> closes.
            while (!xml.atEnd() &&
                   !(xml.isEndElement() &&
                     xml.name() == QLatin1String("setting"))) {
                xml.readNext();
                if (xml.isStartElement() &&
                    xml.name() == QLatin1String("item")) {
                    items << xml.readElementText();
                }
            }
            map.insert(key, items);
        } else {
            const QString text = xml.readElementText();
            if (type == QLatin1String("int")) {
                map.insert(key, text.toInt());
            } else if (type == QLatin1String("bool")) {
                map.insert(key, text == QLatin1String("true"));
            } else if (type == QLatin1String("double")) {
                map.insert(key, text.toDouble());
            } else {
                map.insert(key, text);
            }
        }
    }
    return !xml.hasError();
}

bool writeXml(QIODevice& device, const QSettings::SettingsMap& map) {
    QXmlStreamWriter xml(&device);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("pcbview-settings");
    xml.writeAttribute("version", "1");
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        const QVariant& v = it.value();
        xml.writeStartElement("setting");
        xml.writeAttribute("key", it.key());
        switch (static_cast<QMetaType::Type>(v.typeId())) {
            case QMetaType::QStringList: {
                xml.writeAttribute("type", "stringlist");
                for (const QString& s : v.toStringList())
                    xml.writeTextElement("item", s);
                break;
            }
            case QMetaType::Int:
                xml.writeAttribute("type", "int");
                xml.writeCharacters(QString::number(v.toInt()));
                break;
            case QMetaType::Bool:
                xml.writeAttribute("type", "bool");
                xml.writeCharacters(v.toBool() ? "true" : "false");
                break;
            case QMetaType::Double:
                xml.writeAttribute("type", "double");
                xml.writeCharacters(QString::number(v.toDouble()));
                break;
            default:
                xml.writeAttribute("type", "string");
                xml.writeCharacters(v.toString());
                break;
        }
        xml.writeEndElement();  // setting
    }
    xml.writeEndElement();  // pcbview-settings
    xml.writeEndDocument();
    return true;
}

// Registered once, process-wide. QSettings::registerFormat is documented as
// callable before any QSettings of that format is constructed; the static
// initialises on the first appSettings() call, before we ever build one.
QSettings::Format xmlFormat() {
    static const QSettings::Format fmt =
        QSettings::registerFormat("xml", &readXml, &writeXml);
    return fmt;
}

}  // namespace

QString settingsFilePath() {
    return QDir::homePath() + "/" + kDirName + "/settings.xml";
}

QSettings appSettings() {
    // Ensure the directory exists so the first flush can create the file.
    QDir().mkpath(QDir::homePath() + "/" + kDirName);
    return QSettings(settingsFilePath(), xmlFormat());
}

}  // namespace pcbview
