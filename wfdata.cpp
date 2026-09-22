#include "wfdata.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QUrl>
#include <QDebug>

#include <algorithm>

namespace {

const QString kBase = QStringLiteral("https://api.warframestat.us/wfinfo");
const QByteArray kUserAgent = "wf-market-analytics/6.4 (+o-teu-email@exemplo.pt)";

// Ordem dos campos na tabela de relíquias, com a raridade correspondente.
struct CampoDrop { const char* campo; const char* raridade; double chance; };
const CampoDrop kCampos[] = {
    {"rare1",     "rara",    kChanceRara},
    {"uncommon1", "incomum", kChanceIncomum},
    {"uncommon2", "incomum", kChanceIncomum},
    {"common1",   "comum",   kChanceComum},
    {"common2",   "comum",   kChanceComum},
    {"common3",   "comum",   kChanceComum},
    };

}  // namespace

WfStatClient::WfStatClient(QObject* parent) : QObject(parent) {
    m_net = new QNetworkAccessManager(this);
}

QString WfStatClient::chave(const QString& nome) {
    QString k = nome.trimmed().toLower();
    if (k.endsWith(" blueprint")) k.chop(10);
    return k;
}

void WfStatClient::carregar() {
    pedirReliquias();
    pedirPrecos();
}

void WfStatClient::pedirReliquias() {
    QNetworkRequest req{QUrl(kBase + "/filtered_items")};
    req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(30000);

    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray corpo = reply->readAll();

        if (reply->error() != QNetworkReply::NoError || status != 200) {
            emit erro(QString("Relíquias: HTTP %1").arg(status));
            return;
        }

        QJsonParseError perr{};
        const QJsonObject raiz = QJsonDocument::fromJson(corpo, &perr).object();
        if (perr.error != QJsonParseError::NoError) {
            emit erro("Relíquias: JSON inválido");
            return;
        }

        const QJsonObject eras = raiz.value("relics").toObject();
        if (eras.isEmpty()) {
            emit erro("Relíquias: resposta sem o objeto 'relics'");
            qWarning().noquote() << "[wfstat]" << QString::fromUtf8(corpo.left(200));
            return;
        }

        m_reliquias.clear();
        m_origens.clear();
        m_nomes.clear();
        int ativas = 0;

        for (auto itEra = eras.begin(); itEra != eras.end(); ++itEra) {
            const QJsonObject nomes = itEra.value().toObject();
            for (auto itNome = nomes.begin(); itNome != nomes.end(); ++itNome) {
                const QJsonObject o = itNome.value().toObject();

                Reliquia r;
                r.era = itEra.key();
                r.nome = itNome.key();
                r.vaulted = o.value("vaulted").toBool(true);
                if (!r.vaulted) ++ativas;

                for (const CampoDrop& c : kCampos) {
                    const QString item = o.value(c.campo).toString();
                    if (item.isEmpty()) continue;          // rare1 pode vir null
                    r.drops.push_back({item, c.raridade, c.chance});

                    Origem org;
                    org.reliquia = r.etiqueta();
                    org.raridade = c.raridade;
                    org.vaulted = r.vaulted;
                    m_origens[chave(item)].push_back(org);
                    if (!m_nomes.contains(chave(item)))     // <-- acrescentar
                    m_nomes.insert(chave(item), item); 
                }
                if (!r.drops.isEmpty()) m_reliquias.push_back(r);
            }
        }

        // Dentro de cada item, as relíquias ativas primeiro: são as únicas
        // acionáveis, e é isso que interessa ver em cima.
        for (QList<Origem>& lista : m_origens) {
            std::sort(lista.begin(), lista.end(), [](const Origem& a, const Origem& b) {
                if (a.vaulted != b.vaulted) return !a.vaulted;
                return a.reliquia < b.reliquia;
            });
        }

        emit reliquiasCarregadas(m_reliquias.size(), ativas);
    });
}

void WfStatClient::pedirPrecos() {
    QNetworkRequest req{QUrl(kBase + "/prices")};
    req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(30000);

    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            emit erro(QString("Preços de referência: HTTP %1").arg(status));
            return;
        }

        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).array();
        m_precos.clear();
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            const QString nome = o.value("name").toString();
            if (nome.isEmpty()) continue;

            // Os números vêm como strings nesta API.
            PrecoRef p;
            p.media    = o.value("custom_avg").toString().toDouble();
            p.volOntem = o.value("yesterday_vol").toString().toInt();
            p.volHoje  = o.value("today_vol").toString().toInt();
            m_precos.insert(chave(nome), p);
        }
        emit precosCarregados(m_precos.size());
    });
}

QList<Origem> WfStatClient::ondeObter(const QString& nomeItem) const {
    return m_origens.value(chave(nomeItem));
}

PrecoRef WfStatClient::preco(const QString& nomeItem) const {
    return m_precos.value(chave(nomeItem));
}

double WfStatClient::valorEsperado(const Reliquia& r) const {
    double total = 0.0;
    for (const Drop& d : r.drops) {
        // Forma não se vende: conta como zero em vez de poluir a média.
        if (d.item.startsWith("Forma", Qt::CaseInsensitive)) continue;
        total += d.chance * preco(d.item).media;
    }
    return total;
}

Drop WfStatClient::melhorDrop(const Reliquia& r) const {
    Drop melhor;
    double melhorPreco = -1.0;
    for (const Drop& d : r.drops) {
        const double p = preco(d.item).media;
        if (p > melhorPreco) { melhorPreco = p; melhor = d; }
    }
    return melhor;
}
QStringList WfStatClient::itensComOrigem() const {
    QStringList nomes = m_nomes.values();
    std::sort(nomes.begin(), nomes.end());
    return nomes;
}