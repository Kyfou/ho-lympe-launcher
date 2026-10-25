/*
    Ho-Lympe - talks to the server's web side (tools/server/holympe_api.py) over HTTPS.

    The server uses its own certificate: the first connection remembers its fingerprint
    (coop.ini serverCert) and later connections only accept that certificate.
*/

#ifndef HOLYMPEAPI_H
#define HOLYMPEAPI_H

#include <functional>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QSettings;

class HoLympeApi : public QObject
{
public:
    using Callback = std::function<void(bool ok, const QJsonObject& reply, const QString& error)>;
    using RawCallback = std::function<void(bool ok, const QByteArray& data, const QString& error)>;

    static HoLympeApi& Get();

    QString host() const;      // server address without port, for the game connection
    QString token() const { return authToken; }
    void setToken(const QString& token, bool remember);
    void forgetToken();

    // path like "/api/home"; body = null object for GET
    void get(const QString& path, Callback done);
    void post(const QString& path, const QJsonObject& body, Callback done);
    // Raw GET (update files)
    void getRaw(const QString& path, RawCallback done);
    // Blocking version, for when the app is closing (waits at most timeoutMs)
    bool postAndWait(const QString& path, const QJsonObject& body, int timeoutMs);

    static QSettings& Settings(); // coop.ini next to the exe

private:
    HoLympeApi();
    void send(const QString& method, const QString& path, const QJsonObject* body, RawCallback done);

    QNetworkAccessManager net;
    QString authToken;
};

#endif // HOLYMPEAPI_H
