#include "wfmauth.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QSettings>
#include <QUuid>
#include <QUrl>
#include <QDebug>
#include <QTimer>

#ifdef WFM_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace {

// O login continua no v1: o /v2/auth/signin é first-party (Firebase AppCheck).
const QString kV1 = QStringLiteral("https://api.warframe.market/v1");
const QString kV2 = QStringLiteral("https://api.warframe.market/v2");

const QByteArray kUserAgent = "wf-market-analytics/6.3 (+o-teu-email@exemplo.pt)";
const QByteArray kPlataforma = "pc";

// Ritmo do lote: a API aceita 3 pedidos/segundo.
constexpr int kIntervaloLoteMs = 350;

#ifdef WFM_KEYCHAIN
const char* kServicoKeychain = "wf-market-analytics";
const char* kContaKeychain = "token";
#endif

}  // namespace

WfmAuth::WfmAuth(QObject* parent) : QObject(parent) {
    m_net = new QNetworkAccessManager(this);

    m_loteTimer = new QTimer(this);
    m_loteTimer->setInterval(kIntervaloLoteMs);
    connect(m_loteTimer, &QTimer::timeout, this, &WfmAuth::processarLote);

    // device_id estável entre arranques. Não é segredo, é só um identificador
    // de cliente que o endpoint de login espera.
    QSettings cfg;
    m_deviceId = cfg.value("wfm/deviceId").toString();
    if (m_deviceId.isEmpty()) {
        m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        cfg.setValue("wfm/deviceId", m_deviceId);
    }
}

QNetworkRequest WfmAuth::pedido(const QUrl& url, bool comToken) const {
    QNetworkRequest req{url};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("Language", "en");
    req.setRawHeader("Platform", kPlataforma);
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    if (comToken && !m_token.isEmpty()) {
        req.setRawHeader("Authorization", m_esquema + " " + m_token);
    }
    return req;
}

// ------------------------------------------------------------------ LOGIN ---

void WfmAuth::entrar(const QString& email, const QString& password) {
    if (email.isEmpty() || password.isEmpty()) {
        emit erro("Email e password são obrigatórios.");
        return;
    }

    QJsonObject corpo{
        {"auth_type", "header"},
        {"email", email},
        {"password", password},
        {"device_id", m_deviceId},
        };

    QNetworkRequest req = pedido(QUrl(kV1 + "/auth/signin"), false);
    req.setRawHeader("Authorization", "JWT");   // o v1 exige o cabeçalho vazio aqui

    QNetworkReply* reply = m_net->post(req, QJsonDocument(corpo).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray resposta = reply->readAll();

        if (reply->error() != QNetworkReply::NoError || status != 200) {
            QString detalhe;
            const QJsonObject obj = QJsonDocument::fromJson(resposta).object();
            if (obj.contains("error")) detalhe = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));

            if (status == 401 || status == 400) {
                emit erro("Credenciais recusadas pelo warframe.market.");
            } else if (status == 404) {
                emit erro("O endpoint de login v1 já não existe (404). "
                          "O fluxo de autenticação foi desligado.");
            } else {
                emit erro(QString("Login falhou (HTTP %1). %2")
                              .arg(status > 0 ? QString::number(status) : reply->errorString(), detalhe));
            }
            qWarning().noquote() << "[auth] login" << status << QString::fromUtf8(resposta.left(300));
            return;
        }

        // O token vem no cabeçalho de resposta, prefixado com "JWT ".
        const QByteArray cabecalho = reply->rawHeader("Authorization");
        if (cabecalho.isEmpty()) {
            emit erro("Login aceite mas sem cabeçalho Authorization na resposta.");
            return;
        }
        m_token = cabecalho.startsWith("JWT ") ? cabecalho.mid(4).trimmed() : cabecalho.trimmed();
        m_esquema = "JWT";

        // v1 devolve o perfil em payload.user
        const QJsonObject user = QJsonDocument::fromJson(resposta)
                                     .object().value("payload").toObject()
                                     .value("user").toObject();
        m_nome = user.value("ingame_name").toString();
        if (m_nome.isEmpty()) m_nome = user.value("ingameName").toString();
        if (m_nome.isEmpty()) m_nome = "sessão iniciada";

        guardarToken();
        emit sessaoIniciada(m_nome);
    });
}

void WfmAuth::sair() {
    m_token.clear();
    m_nome.clear();
    m_esquema = "JWT";
    limparToken();
    emit sessaoTerminada();
}

// ------------------------------------------------- pedidos autenticados -----

void WfmAuth::enviarAutenticado(const QByteArray& verbo, const QUrl& url,
                                const QByteArray& corpo, Callback aoSucesso,
                                bool jaRepetiu) {
    if (!autenticado()) {
        emit erro("Sem sessão iniciada.");
        return;
    }

    QNetworkReply* reply = m_net->sendCustomRequest(pedido(url, true), verbo, corpo);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, verbo, url, corpo, aoSucesso, jaRepetiu]() {
                reply->deleteLater();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray resposta = reply->readAll();

                if (status == 401 && !jaRepetiu) {
                    // Pode ser só o prefixo errado. Troca uma vez e repete.
                    m_esquema = (m_esquema == "JWT") ? QByteArray("Bearer") : QByteArray("JWT");
                    qInfo() << "[auth] 401 — a repetir com esquema" << m_esquema;
                    enviarAutenticado(verbo, url, corpo, aoSucesso, true);
                    return;
                }
                if (status == 401) {
                    qWarning().noquote() << "[auth] 401 definitivo:" << QString::fromUtf8(resposta.left(300));
                    sair();
                    emit erro("A sessão expirou. Volta a iniciar sessão.");
                    return;
                }
                if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                    qWarning().noquote() << "[auth]" << verbo << status
                                         << QString::fromUtf8(resposta.left(300));
                    emit erro(QString("Pedido falhou (HTTP %1).")
                                  .arg(status > 0 ? QString::number(status) : reply->errorString()));
                    return;
                }
                if (aoSucesso) aoSucesso(resposta);
            });
}

void WfmAuth::criarOrdem(const QString& itemId, const QString& tipo,
                         int platinum, int quantity, int rank, bool visivel) {
    if (itemId.isEmpty() || platinum <= 0 || quantity <= 0) {
        emit erro("Dados da ordem inválidos.");
        return;
    }

    QJsonObject corpo{
        {"itemId", itemId},
        {"type", tipo},              // "sell" ou "buy"
        {"platinum", platinum},
        {"quantity", quantity},
        {"visible", visivel},
        };
    if (rank >= 0) corpo.insert("rank", rank);   // -1 = item sem ranks

    enviarAutenticado("POST", QUrl(kV2 + "/orders"),
                      QJsonDocument(corpo).toJson(QJsonDocument::Compact),
                      [this, itemId, platinum](const QByteArray&) {
                          emit ordemCriada(itemId, platinum);
                      });
}

void WfmAuth::tentarCaminhos(const QString& chave, const QList<Tentativa>& modelos,
                             const QString& ordemId, const QByteArray& corpo,
                             std::function<void()> aoSucesso, int indice,
                             bool jaTentouConhecido) {
    if (!autenticado()) { emit erro("Sem sessão iniciada."); return; }

    // Se já descobrimos o caminho para esta operação, vamos direto a ele.
    Tentativa alvo;
    const bool usarConhecido = !jaTentouConhecido && m_caminhoConhecido.contains(chave);
    if (usarConhecido) {
        alvo = m_caminhoConhecido.value(chave);
    } else {
        if (indice >= modelos.size()) {
            emit erro(QString("Nenhum endpoint respondeu para: %1. Vê o log [auth].").arg(chave));
            return;
        }
        alvo = modelos[indice];
    }

    const QByteArray verbo = alvo.first;
    const QString url = alvo.second.arg(ordemId);
    QNetworkReply* reply = m_net->sendCustomRequest(pedido(QUrl(url), true), verbo, corpo);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, chave, modelos, ordemId, corpo, aoSucesso, indice,
             verbo, url, usarConhecido, alvo]() {
                reply->deleteLater();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray resposta = reply->readAll();

                if (status == 404 || status == 405) {
                    if (usarConhecido) {
                        // O caminho memorizado deixou de servir: recomeça a descoberta.
                        m_caminhoConhecido.remove(chave);
                        tentarCaminhos(chave, modelos, ordemId, corpo, aoSucesso, 0, true);
                        return;
                    }
                    qInfo().noquote() << "[auth]" << chave << verbo << url << "->" << status
                                      << "(a tentar o seguinte)";
                    tentarCaminhos(chave, modelos, ordemId, corpo, aoSucesso, indice + 1, true);
                    return;
                }
                if (status == 401) {
                    if (m_esquema == "JWT") {
                        m_esquema = "Bearer";
                        qInfo() << "[auth] 401 — a repetir com Bearer";
                        tentarCaminhos(chave, modelos, ordemId, corpo, aoSucesso, indice, false);
                        return;
                    }
                    sair();
                    emit erro("A sessão expirou. Volta a iniciar sessão.");
                    return;
                }
                if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                    qWarning().noquote() << "[auth]" << chave << verbo << url << status
                                         << QString::fromUtf8(resposta.left(300));
                    emit erro(QString("%1 falhou (HTTP %2).").arg(chave).arg(status));
                    return;
                }

                if (!m_caminhoConhecido.contains(chave)) {
                    m_caminhoConhecido.insert(chave, alvo);
                    qInfo().noquote() << "[auth]" << chave << "OK via" << verbo << url;
                }
                if (aoSucesso) aoSucesso();
            });
}

void WfmAuth::apagarOrdem(const QString& ordemId) {
    tentarCaminhos("apagar ordem", {
                                     {"DELETE", kV2 + "/orders/%1"},
                                     {"DELETE", kV2 + "/order/%1"},
                                     {"DELETE", kV1 + "/profile/orders/%1"},
                                     }, ordemId, {}, [this, ordemId]() { emit ordemApagada(ordemId); });
}

void WfmAuth::alterarOrdem(const QString& ordemId, const QJsonObject& campos) {
    const QByteArray corpo = QJsonDocument(campos).toJson(QJsonDocument::Compact);
    tentarCaminhos("alterar ordem", {
                                      {"PATCH", kV2 + "/orders/%1"},
                                      {"PUT",   kV2 + "/orders/%1"},
                                      {"PATCH", kV2 + "/order/%1"},
                                      {"PUT",   kV1 + "/profile/orders/%1"},
                                      }, ordemId, corpo, [this, ordemId]() { emit ordemAlterada(ordemId); });
}

void WfmAuth::marcarVendida(const QString& ordemId) {
    tentarCaminhos("marcar vendida", {
                                       {"POST", kV2 + "/orders/%1/close"},
                                       {"POST", kV2 + "/order/%1/close"},
                                       {"PUT",  kV1 + "/profile/orders/close_order/%1"},
                                       }, ordemId, {}, [this, ordemId]() { emit ordemFechada(ordemId); });
}

// ------------------------------------------------------------------ LOTE ---

void WfmAuth::alterarVarias(const QStringList& ordemIds, const QJsonObject& campos) {
    if (!autenticado()) { emit erro("Sem sessão iniciada."); return; }
    if (m_loteEmCurso)  { emit erro("Já há uma operação em lote a decorrer."); return; }
    if (ordemIds.isEmpty()) { emit loteConcluido(0, 0); return; }

    m_lote = ordemIds;
    m_loteCampos = campos;
    m_loteTotal = ordemIds.size();
    m_loteFalhas = 0;
    m_loteEmCurso = true;

    emit loteProgresso(0, m_loteTotal);
    processarLote();          // a primeira sai já; as seguintes ao ritmo do timer
    m_loteTimer->start();
}

void WfmAuth::cancelarLote() {
    m_loteTimer->stop();
    m_lote.clear();
    if (m_loteEmCurso) {
        m_loteEmCurso = false;
        emit loteConcluido(m_loteTotal - m_lote.size(), m_loteFalhas);
    }
}

void WfmAuth::processarLote() {
    if (m_lote.isEmpty()) {
        m_loteTimer->stop();
        if (m_loteEmCurso) {
            m_loteEmCurso = false;
            emit loteConcluido(m_loteTotal - m_loteFalhas, m_loteFalhas);
        }
        return;
    }

    const QString id = m_lote.takeFirst();
    const QByteArray corpo = QJsonDocument(m_loteCampos).toJson(QJsonDocument::Compact);

    tentarCaminhos("alterar ordem", {
                                      {"PATCH", kV2 + "/orders/%1"},
                                      {"PUT",   kV2 + "/orders/%1"},
                                      {"PATCH", kV2 + "/order/%1"},
                                      {"PUT",   kV1 + "/profile/orders/%1"},
                                      }, id, corpo, [this]() {
                       emit loteProgresso(m_loteTotal - m_lote.size(), m_loteTotal);
                   });
}

void WfmAuth::carregarMinhasOrdens() {
    if (!autenticado()) { emit erro("Sem sessão iniciada."); return; }
    tentarOrdens(0);
}

// Extrai a lista tanto da forma v2 ({data:[...]}) como da v1
// ({payload:{sell_orders:[...], buy_orders:[...]}}).
QList<MinhaOrdem> WfmAuth::extrairOrdens(const QByteArray& corpo) const {
    QList<MinhaOrdem> ordens;

    auto ler = [&](const QJsonArray& arr, const QString& tipoForcado) {
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            MinhaOrdem m;
            m.id       = o.value("id").toString();
            m.platinum = o.value("platinum").toInt();
            m.quantity = o.value("quantity").toInt();
            m.rank     = o.value("rank").toInt();
            m.visible  = o.value("visible").toBool(true);

            m.tipo = o.value("type").toString();                       // v2
            if (m.tipo.isEmpty()) m.tipo = o.value("order_type").toString();  // v1
            if (m.tipo.isEmpty()) m.tipo = tipoForcado;

            m.itemId = o.value("itemId").toString();                   // v2
            const QJsonValue item = o.value("item");
            if (item.isObject()) {                                     // v1
                const QJsonObject io = item.toObject();
                if (m.itemId.isEmpty()) m.itemId = io.value("id").toString();
                m.slug = io.value("url_name").toString();
                if (m.slug.isEmpty()) m.slug = io.value("slug").toString();
            } else if (item.isString() && m.itemId.isEmpty()) {
                m.itemId = item.toString();
            }
            if (m.slug.isEmpty()) m.slug = o.value("slug").toString();

            if (!m.id.isEmpty()) ordens << m;
        }
    };

    const QJsonObject raiz = QJsonDocument::fromJson(corpo).object();

    if (raiz.value("data").isArray()) {
        ler(raiz.value("data").toArray(), {});
        return ordens;
    }
    const QJsonObject dados = raiz.value("data").toObject();
    const QJsonObject payload = raiz.value("payload").toObject();
    for (const QJsonObject& fonte : {dados, payload}) {
        if (fonte.isEmpty()) continue;
        if (fonte.value("orders").isArray()) ler(fonte.value("orders").toArray(), {});
        ler(fonte.value("sell_orders").toArray(), "sell");
        ler(fonte.value("buy_orders").toArray(), "buy");
    }
    return ordens;
}

void WfmAuth::tentarOrdens(int indice) {
    // Candidatos conhecidos, do mais provável ao mais antigo. O %1 é o nome
    // in-game, que só o endpoint v1 de perfil usa.
    const QStringList candidatos = {
        kV2 + "/orders/my",
        kV2 + "/me/orders",
        kV1 + "/profile/orders",
        kV1 + "/profile/" + m_nome + "/orders",
    };

    // Se já sabemos qual funciona, vamos direto a esse.
    const QString url = (!m_endpointOrdens.isEmpty() && indice == 0)
                            ? m_endpointOrdens
                            : (indice < candidatos.size() ? candidatos[indice] : QString());

    if (url.isEmpty()) {
        emit erro("Nenhum endpoint de ordens respondeu. Vê o log [auth] para os códigos.");
        return;
    }

    QNetworkReply* reply = m_net->get(pedido(QUrl(url), true));

    connect(reply, &QNetworkReply::finished, this, [this, reply, url, indice]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray corpo = reply->readAll();

        if (status == 404 || status == 405) {
            qInfo().noquote() << "[auth] ordens:" << url << "->" << status << "(a tentar o seguinte)";
            m_endpointOrdens.clear();
            tentarOrdens(indice + 1);
            return;
        }
        if (status == 401) {
            // Pode ser o prefixo do cabeçalho. Troca e repete este mesmo candidato.
            if (m_esquema == "JWT") {
                m_esquema = "Bearer";
                qInfo() << "[auth] 401 nas ordens — a repetir com Bearer";
                tentarOrdens(indice);
                return;
            }
            sair();
            emit erro("A sessão expirou. Volta a iniciar sessão.");
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            qWarning().noquote() << "[auth] ordens:" << url << status
                                 << QString::fromUtf8(corpo.left(300));
            emit erro(QString("Não foi possível obter as tuas ordens (HTTP %1).").arg(status));
            return;
        }

        m_endpointOrdens = url;
        qInfo().noquote() << "[auth] endpoint de ordens em uso:" << url;
        emit ordensRecebidas(extrairOrdens(corpo), url);
    });
}

// ----------------------------------------------------- token persistente ----

void WfmAuth::guardarToken() {
#ifdef WFM_KEYCHAIN
    auto* job = new QKeychain::WritePasswordJob(kServicoKeychain, this);
    job->setKey(kContaKeychain);
    job->setTextData(QString::fromUtf8(m_token));
    job->setAutoDelete(true);
    job->start();
#endif
    // Sem keychain: o token fica só em memória. Nunca é escrito em disco.
}

void WfmAuth::limparToken() {
#ifdef WFM_KEYCHAIN
    auto* job = new QKeychain::DeletePasswordJob(kServicoKeychain, this);
    job->setKey(kContaKeychain);
    job->setAutoDelete(true);
    job->start();
#endif
}

void WfmAuth::restaurarSessao() {
#ifdef WFM_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(kServicoKeychain, this);
    job->setKey(kContaKeychain);
    job->setAutoDelete(true);
    connect(job, &QKeychain::Job::finished, this, [this, job]() {
        if (job->error() || job->textData().isEmpty()) return;
        m_token = job->textData().toUtf8();
        m_nome = "sessão restaurada";
        emit sessaoIniciada(m_nome);
        carregarMinhasOrdens();   // confirma que o token ainda serve
    });
    job->start();
#endif
}