/*
    Ho-Lympe - see HoLympeApi.h.
*/

#include "HGCoop.h"
#include "HoLympeApi.h"
#include "HoLympeUpdate.h"

#include <memory>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSettings>
#include <QSslCertificate>
#include <QSslError>
#include <QTimer>

static const char* kDefaultServer = "15.237.121.156:8443";
// SHA-256 of the server's certificate (self-signed, made by install-server.sh)
static const char* kServerCert = "3c16556656a3cf5f853e4a7c2c800578bb8cee877538cab1cf4099c45c2f84a8";

QSettings& HoLympeApi::Settings()
{
    static QSettings settings(HoLympeDir() + "/coop.ini", QSettings::IniFormat);
    return settings;
}

HoLympeApi& HoLympeApi::Get()
{
    static HoLympeApi api;
    return api;
}

HoLympeApi::HoLympeApi()
{
    authToken = Settings().value("token").toString();
}

QString HoLympeApi::host() const
{
    return Settings().value("server", kDefaultServer).toString().section(':', 0, 0);
}

void HoLympeApi::setToken(const QString& token, bool remember)
{
    authToken = token;
    if (remember)
        Settings().setValue("token", token);
    else
        Settings().remove("token");
}

void HoLympeApi::forgetToken()
{
    authToken.clear();
    Settings().remove("token");
}

static HoLympeApi::RawCallback JsonReply(HoLympeApi::Callback done)
{
    return [done](bool ok, const QByteArray& data, const QString& error) {
        done(ok, QJsonDocument::fromJson(data).object(), error);
    };
}

void HoLympeApi::get(const QString& path, Callback done)
{
    send("GET", path, nullptr, JsonReply(std::move(done)));
}

void HoLympeApi::post(const QString& path, const QJsonObject& body, Callback done)
{
    send("POST", path, &body, JsonReply(std::move(done)));
}

void HoLympeApi::getRaw(const QString& path, RawCallback done)
{
    send("GET", path, nullptr, std::move(done));
}

void HoLympeApi::send(const QString& method, const QString& path, const QJsonObject* body, RawCallback done)
{
    QString server = Settings().value("server", kDefaultServer).toString();
    QString scheme = Settings().value("serverHttp", false).toBool() ? "http" : "https"; // http: local tests
    QNetworkRequest request(QUrl(scheme + "://" + server + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(15000);
    if (!authToken.isEmpty())
        request.setRawHeader("Authorization", ("Bearer " + authToken).toUtf8());
    request.setRawHeader("X-HoLympe-Version", HoLympe_LocalVersion().toUtf8());

    QNetworkReply* reply = method == "GET" ? net.get(request)
        : net.post(request, QJsonDocument(*body).toJson(QJsonDocument::Compact));

    // Our own certificate: accepted when it is the one seen the first time
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError>&) {
        QSslCertificate cert = reply->sslConfiguration().peerCertificate();
        QString fingerprint = QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex());
        QString known = Settings().value("serverCert", kServerCert).toString();
        if (known.isEmpty())
        {
            Settings().setValue("serverCert", fingerprint);
            known = fingerprint;
        }
        if (fingerprint == known)
            reply->ignoreSslErrors();
    });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done]() {
        reply->deleteLater();
        QByteArray data = reply->readAll();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200)
        {
            done(true, data, QString());
            return;
        }
        QString error = QJsonDocument::fromJson(data).object().value("error").toString();
        if (error.isEmpty())
            error = status == 0 ? QString::fromUtf8("Serveur injoignable") : QString::fromUtf8("Erreur du serveur");
        done(false, data, error);
    });
}

bool HoLympeApi::postAndWait(const QString& path, const QJsonObject& body, int timeoutMs)
{
    // The reply may come after the timeout: the callback only touches this shared state
    struct State { bool finished = false; bool ok = false; QEventLoop* loop = nullptr; };
    auto state = std::make_shared<State>();
    post(path, body, [state](bool ok, const QJsonObject&, const QString&) {
        state->finished = true;
        state->ok = ok;
        if (state->loop)
            state->loop->quit();
    });
    if (!state->finished)
    {
        QEventLoop loop;
        state->loop = &loop;
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
        state->loop = nullptr;
    }
    return state->ok;
}
