#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonValue>
#include <QPair>
#include <QReadWriteLock>
#include <QTimer>
#include <QTranslator>

// Follows the language picked under Omarchy's Setup > Language. Catalogs are
// flat JSON objects from English source text to translation, one per Omarchy
// language code (i18n/da.json, i18n/pt-BR.json), so QML keeps plain
// qsTr("Save") and C++ keeps tr("Save"). A counted string,
// qsTr("%Ln clips", "", count), maps to an object of plural forms keyed by
// CLDR category ({"one": "%Ln klip", "other": "%Ln klip"}); %Ln rather than
// %n writes the count in the language's own digits.
class Translator : public QTranslator {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)

public:
    explicit Translator(const QString &languageFile = {}, const QString &catalogDirectory = ":/i18n",
                        QObject *parent = nullptr);
    ~Translator() override;

    QString language() const;
    bool isEmpty() const override;
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;
    // The CLDR cardinal category ("one", "few", "other"…) of a whole count
    // in a language, by its Omarchy code.
    static QString pluralCategory(const QString &code, long long n);

    // The strings for `language` from the catalogs in `directory`: English
    // first, then the base language and the regional one over it (pt, then
    // pt-BR), so a missing string falls back toward English one step at a
    // time. Each string comes with the code of the catalog it came from.
    // Plugins' catalogs are read the same way.
    static QHash<QString, QPair<QString, QJsonValue>> catalog(const QString &directory, const QString &language);

signals:
    void changed();

private:
    void reload();

    QString m_languageFile;
    QString m_catalogDirectory;
    // Worker threads translate too (file names, errors), so the catalog is
    // swapped under a lock rather than freed beneath them.
    mutable QReadWriteLock m_lock;
    QString m_language;
    // Each translation with the code of the catalog it came from, whose
    // plural rule its forms follow: a string still in English counts the
    // English way.
    QHash<QString, QPair<QString, QJsonValue>> m_strings;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
