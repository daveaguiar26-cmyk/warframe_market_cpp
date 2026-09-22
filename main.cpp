// main.cpp — wf-market-analytics
// Refatoração MVC, coluna Rank Max, Ordenação Automática da Página
// e ligação opcional à conta warframe.market (ver wfmauth.h).

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QFont>
#include <QColor>
#include <QBrush>
#include <QPointer>
#include <QTimer>
#include <QHash>
#include <QQueue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QDebug>

#include <algorithm>
#include <limits>

#include "wfmauth.h"
#include "wfdata.h"

namespace {
const QString kApiBase = QStringLiteral("https://api.warframe.market/v2");
const QByteArray kUserAgent = "wf-market-analytics/6.3 (+o-teu-email@exemplo.pt)";
const QByteArray kPlataforma = "pc";
constexpr int kIntervaloPedidosMs = 350;
constexpr int kPorPagina = 25;
constexpr int kMinOrdensRelevante = 3;
const QStringList kCategorias = {"All Items", "Sets", "Arcanes", "Mods", "Relics", "Outros"};

QString categoriaDeTags(const QJsonArray& tags) {
    QSet<QString> t;
    for (const QJsonValue& v : tags) t.insert(v.toString());
    if (t.contains("relic"))              return "Relics";
    if (t.contains("arcane_enhancement")) return "Arcanes";
    if (t.contains("mod"))                return "Mods";
    if (t.contains("set"))                return "Sets";
    return "Outros";
}

QString plural(int n, const QString& singular, const QString& plural) {
    return QString::number(n) + " " + (n == 1 ? singular : plural);
}

bool estaDisponivel(const QString& status) {
    return status == "online" || status == "ingame";
}

// O(N) com nth_element em vez de O(N log N) do std::sort
int mediana(QList<int> v) {
    if (v.isEmpty()) return 0;
    auto nth = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), nth, v.end());
    return *nth;
}
} // namespace

// ======================================================= ESTRUTURAS DE DADOS ===

struct ItemApi {
    QString id;
    QString slug;
    QString name;
    QString category;
    int maxRank = 0;
};

struct Precos {
    int rankUsado = -1;      // -1 = item sem ranks
    int menorVenda = 0;
    int maiorCompra = 0;
    int qtdMenorVenda = 0;
    int vendedores = 0;
    int compradores = 0;
    bool apenasOffline = false;
    bool valido = false;
};

struct Movimento {
    QString slug;
    int ordensVenda = 0;
    int ordensCompra = 0;
    int medianaVenda = 0;
    int medianaCompra = 0;
    QList<int> precosVenda;
    QList<int> precosCompra;

    int ordens() const { return ordensVenda + ordensCompra; }
    int potencial() const { return medianaVenda * ordens(); }
    double procura() const {
        if (ordensVenda == 0) return ordensCompra > 0 ? 99.0 : 0.0;
        return double(ordensCompra) / double(ordensVenda);
    }
};

// ======================================================== CLASSES AUXILIARES ===

class CelulaNumero : public QTableWidgetItem {
public:
    void definir(int valor, const QString& sufixo = QString()) {
        chave = valor > 0 ? valor : -1;
        if (valor > 0) {
            setData(Qt::DisplayRole, sufixo.isEmpty() ? QVariant(valor) : QVariant(QString::number(valor) + sufixo));
        } else {
            setData(Qt::DisplayRole, QStringLiteral("—"));
        }
    }
    void definirChave(int k) { chave = k; }
    bool operator<(const QTableWidgetItem& outro) const override {
        const CelulaNumero* p = dynamic_cast<const CelulaNumero*>(&outro);
        return p ? chave < p->chave : QTableWidgetItem::operator<(outro);
    }
private:
    int chave = -1;
};

// =========================================================== CAMADA DE REDE ===

class WarframeApiClient : public QObject {
    Q_OBJECT
public:
    explicit WarframeApiClient(QObject* parent = nullptr) : QObject(parent) {
        manager = new QNetworkAccessManager(this);
        rateLimiter = new QTimer(this);
        rateLimiter->setInterval(kIntervaloPedidosMs);
        connect(rateLimiter, &QTimer::timeout, this, &WarframeApiClient::processarFilaPrecos);
    }

    const ItemApi* itemDeSlug(const QString& slug) const {
        for (const ItemApi& i : m_catalogo) if (i.slug == slug) return &i;
        return nullptr;
    }

    const QList<ItemApi>& todosItens() const { return m_catalogo; }

    void carregarCatalogo() {
        emit statusGlobal("A carregar catálogo...", false, false);
        QNetworkReply* reply = manager->get(pedidoBase(QUrl(kApiBase + "/items")));

        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (reply->error() != QNetworkReply::NoError || status != 200) {
                emit erroLog(QString("[catálogo] status %1 | %2").arg(status).arg(reply->errorString()));
                emit statusGlobal(status > 0 ? QString("Erro HTTP %1").arg(status) : reply->errorString(), true, false);
                return;
            }

            QJsonParseError perr{};
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
            if (perr.error != QJsonParseError::NoError) {
                emit statusGlobal("JSON inválido: " + perr.errorString(), true, false);
                return;
            }

            const QJsonArray arr = doc.object().value("data").toArray();
            m_porId.clear();
            m_catalogo.clear();
            m_catalogo.reserve(arr.size());

            for (const QJsonValue& val : arr) {
                const QJsonObject obj = val.toObject();
                ItemApi item;
                item.slug = obj.value("slug").toString();
                if (item.slug.isEmpty()) continue;

                item.id = obj.value("id").toString();
                item.maxRank = obj.value("maxRank").toInt(0);
                item.name = obj.value("i18n").toObject().value("en").toObject().value("name").toString();
                if (item.name.isEmpty()) item.name = item.slug;
                item.category = categoriaDeTags(obj.value("tags").toArray());

                m_catalogo.push_back(item);
            }

            for (const ItemApi& i : m_catalogo) {
                if (!i.id.isEmpty()) m_porId.insert(i.id, &i);
            }

            emit statusGlobal("Catálogo carregado", false, true);
            emit catalogoCarregado();
        });
    }

    void carregarMovimento() {
        if (m_catalogo.isEmpty()) return;
        emit statusMovimento("A analisar...");

        QNetworkReply* reply = manager->get(pedidoBase(QUrl(kApiBase + "/orders/recent")));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (reply->error() != QNetworkReply::NoError || status != 200) {
                emit statusMovimento(QString("erro %1").arg(status > 0 ? QString::number(status) : reply->errorString()));
                return;
            }

            QJsonParseError perr{};
            const QJsonValue dados = QJsonDocument::fromJson(reply->readAll(), &perr).object().value("data");

            QHash<QString, Movimento> porSlug;
            int ordensLidas = 0;

            for (const QJsonValue& v : dados.toArray()) {
                const QJsonObject o = v.toObject();
                const ItemApi* item = resolverItemMovimento(o);
                if (!item) continue;

                const int plat = o.value("platinum").toInt(0);
                if (plat <= 0) continue;

                Movimento& m = porSlug[item->slug];
                m.slug = item->slug;
                if (o.value("type").toString() == "buy") {
                    ++m.ordensCompra;
                    m.precosCompra << plat;
                } else {
                    ++m.ordensVenda;
                    m.precosVenda << plat;
                }
                ++ordensLidas;
            }

            QList<Movimento> lista;
            for (Movimento& m : porSlug) {
                m.medianaVenda = mediana(m.precosVenda);
                m.medianaCompra = mediana(m.precosCompra);
                if (m.medianaVenda == 0) m.medianaVenda = m.medianaCompra;
                if (m.ordens() >= kMinOrdensRelevante) lista << m;
            }

            std::sort(lista.begin(), lista.end(), [](const Movimento& a, const Movimento& b) {
                return a.potencial() > b.potencial();
            });

            emit movimentoCarregado(lista, ordensLidas);
        });
    }

    void agendarPrecos(const QStringList& slugs) {
        filaPrecos.clear();
        naFila.clear();
        for (const QString& s : slugs) {
            filaPrecos.enqueue(s);
            naFila.insert(s);
        }
        totalFila = filaPrecos.size();

        if (filaPrecos.isEmpty()) {
            rateLimiter->stop();
            emit filaProgresso(0, 0);          // nada a fazer: já terminou
        } else {
            rateLimiter->start();
            emit filaProgresso(0, totalFila);  // 0 de N feitos
        }
    }

    // 0 = rank base, 1 = rank máximo do item. Só afeta itens com maxRank > 0.
    void definirRankPreferido(int modo) { m_modoRank = modo; }
    int modoRank() const { return m_modoRank; }

    int rankDoItem(const ItemApi& item) const {
        if (item.maxRank <= 0) return -1;                 // item sem ranks
        return m_modoRank == 0 ? 0 : item.maxRank;
    }

    void priorizarItem(const QString& slug) {
        if (naFila.contains(slug)) {
            filaPrecos.removeAll(slug);
            filaPrecos.prepend(slug);
        }
    }

    // Recotar um item específico: usado depois de anunciares uma ordem, para
    // a tabela refletir o teu próprio anúncio.
    void recotar(const QString& slug) {
        if (naFila.contains(slug)) return;
        filaPrecos.prepend(slug);
        naFila.insert(slug);
        totalFila = std::max<int>(totalFila, filaPrecos.size());
        rateLimiter->start();
    }

signals:
    void statusGlobal(const QString& msg, bool erro, bool ok);
    void statusMovimento(const QString& msg);
    void erroLog(const QString& err);
    void catalogoCarregado();
    void precoRecebido(const QString& slug, const Precos& precos);
    void movimentoCarregado(const QList<Movimento>& lista, int ordensLidas);
    void filaProgresso(int feitos, int total);   // feitos == total => acabou

private slots:
    void processarFilaPrecos() {
        if (pedidoEmCurso) return;
        if (filaPrecos.isEmpty()) {
            rateLimiter->stop();
            emit filaProgresso(totalFila, totalFila);
            return;
        }

        const QString slug = filaPrecos.dequeue();
        naFila.remove(slug);

        const ItemApi* item = itemDeSlug(slug);
        const int rank = item ? rankDoItem(*item) : -1;

        QUrl url(kApiBase + "/orders/item/" + slug + "/top");
        if (rank >= 0) {
            QUrlQuery q;
            q.addQueryItem("rank", QString::number(rank));
            url.setQuery(q);
        }

        QNetworkReply* reply = manager->get(pedidoBase(url));
        pedidoEmCurso = reply;

        connect(reply, &QNetworkReply::finished, this, [this, reply, slug, rank]() {
            reply->deleteLater();
            pedidoEmCurso = nullptr;
            if (reply->error() == QNetworkReply::OperationCanceledError) return;

            Precos p;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && status == 200) {
                QJsonParseError perr{};
                const QJsonObject dados = QJsonDocument::fromJson(reply->readAll(), &perr).object().value("data").toObject();
                if (perr.error == QJsonParseError::NoError && !dados.isEmpty()) {
                    p = resumirOrdens(dados.value("sell").toArray(),
                                      dados.value("buy").toArray(), rank);
                    p.valido = true;
                }
            }
            p.rankUsado = rank;
            emit precoRecebido(slug, p);

            if (!filaPrecos.isEmpty()) {
                emit filaProgresso(totalFila - filaPrecos.size(), totalFila);
            } else {
                rateLimiter->stop();
                emit filaProgresso(totalFila, totalFila);
            }
        });
    }

private:
    QNetworkRequest pedidoBase(const QUrl& url) const {
        QNetworkRequest req{url};
        req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("Language", "en");
        req.setRawHeader("Platform", kPlataforma);
        req.setTransferTimeout(30000);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        return req;
    }

    const ItemApi* resolverItemMovimento(const QJsonObject& o) const {
        const QJsonValue direto = o.value("itemId");
        if (direto.isString()) if (auto i = m_porId.value(direto.toString(), nullptr)) return i;
        const QJsonValue item = o.value("item");
        if (item.isString()) {
            if (auto i = m_porId.value(item.toString(), nullptr)) return i;
            return itemDeSlug(item.toString());
        }
        if (item.isObject()) {
            const QJsonObject obj = item.toObject();
            if (auto i = m_porId.value(obj.value("id").toString(), nullptr)) return i;
            return itemDeSlug(obj.value("slug").toString());
        }
        return nullptr;
    }

    // rankPedido = -1 para itens sem ranks. O filtro por rank é feito aqui e não
    // só no pedido: o ?rank= da API não é de confiança — devolve ordens de
    // outros ranks, e misturar rank 0 com rank máximo num item como o Arcane
    // Barrier dá uma venda a 4 p ao lado de uma compra a 250 p.
    static Precos resumirOrdens(const QJsonArray& vendas, const QJsonArray& compras,
                                int rankPedido) {
        Precos p;
        p.rankUsado = rankPedido;
        int menorVenda = std::numeric_limits<int>::max();

        auto processar = [rankPedido](const QJsonArray& ordens, bool online, auto&& acao) {
            for (const QJsonValue& v : ordens) {
                const QJsonObject o = v.toObject();
                const QString estado = o.value("user").toObject().value("status").toString();
                if (online != estaDisponivel(estado)) continue;
                if (rankPedido >= 0) {
                    // Ordens sem campo rank contam como rank 0.
                    if (o.value("rank").toInt(0) != rankPedido) continue;
                }
                const int plat = o.value("platinum").toInt(0);
                if (plat <= 0) continue;
                acao(plat, o.value("quantity").toInt(1));
            }
        };

        for (bool online : {true, false}) {
            processar(vendas, online, [&](int plat, int qtd) {
                if (plat < menorVenda) { menorVenda = plat; p.qtdMenorVenda = qtd; }
                else if (plat == menorVenda) { p.qtdMenorVenda += qtd; }
                ++p.vendedores;
            });
            processar(compras, online, [&](int plat, int) {
                p.maiorCompra = std::max(p.maiorCompra, plat);
                ++p.compradores;
            });
            if (p.vendedores > 0 || p.compradores > 0) { p.apenasOffline = !online; break; }
        }
        p.menorVenda = (menorVenda == std::numeric_limits<int>::max()) ? 0 : menorVenda;
        return p;
    }

    QNetworkAccessManager* manager;
    QTimer* rateLimiter;
    QQueue<QString> filaPrecos;
    QSet<QString> naFila;
    int totalFila = 0;
    QPointer<QNetworkReply> pedidoEmCurso;

    int m_modoRank = 0;
    QList<ItemApi> m_catalogo;
    QHash<QString, const ItemApi*> m_porId;
};

// ======================================================= DIÁLOGO DE LOGIN ===

class DialogoLogin : public QDialog {
    Q_OBJECT
public:
    explicit DialogoLogin(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("Ligar conta warframe.market");
        setModal(true);

        QVBoxLayout* raiz = new QVBoxLayout(this);

        QLabel* aviso = new QLabel(this);
        aviso->setWordWrap(true);
        aviso->setTextFormat(Qt::RichText);
        aviso->setStyleSheet("background:#111827; border:1px solid #f59e0b; border-radius:8px;"
                             " padding:12px; color:#fcd34d; font-size:12px;");
        aviso->setText(
            "O warframe.market <b>não tem OAuth para aplicações de terceiros</b>. A única "
            "forma de autenticar é enviar o email e a password da tua conta ao endpoint v1.<br><br>"
            "A password é usada apenas neste pedido e nunca é guardada. O token de sessão "
            "fica em memória e desaparece quando fechares a app.");
        raiz->addWidget(aviso);

        QFormLayout* form = new QFormLayout();
        campoEmail = new QLineEdit(this);
        campoEmail->setPlaceholderText("email da conta warframe.market");
        campoPass = new QLineEdit(this);
        campoPass->setEchoMode(QLineEdit::Password);
        form->addRow("Email:", campoEmail);
        form->addRow("Password:", campoPass);
        raiz->addLayout(form);

        QDialogButtonBox* botoes = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        botoes->button(QDialogButtonBox::Ok)->setText("Entrar");
        connect(botoes, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(botoes, &QDialogButtonBox::rejected, this, &QDialog::reject);
        raiz->addWidget(botoes);
    }

    QString email() const { return campoEmail->text().trimmed(); }
    QString password() const { return campoPass->text(); }

private:
    QLineEdit* campoEmail;
    QLineEdit* campoPass;
};

// ========================================================== CAMADA DE VISÃO ===

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("wf-market-analytics.sys v6.3 (conta ligada)");
        resize(1150, 800);
        setStyleSheet(R"(
            QMainWindow, QWidget { background-color: #0b0f19; color: #e2e8f0; font-family: 'Inter', 'Noto Sans', sans-serif; }
            QTabWidget::pane { border: 1px solid #1f2937; border-radius: 10px; top: -1px; }
            QTabBar::tab {
                background: #111827; color: #9ca3af; padding: 9px 22px;
                border: 1px solid #1f2937; border-bottom: none;
                border-top-left-radius: 8px; border-top-right-radius: 8px;
                font-weight: bold; font-size: 13px; margin-right: 4px;
            }
            QTabBar::tab:selected { background: #1d4ed8; color: #ffffff; border-color: #3b82f6; }
            QSpinBox { background:#111827; border:1px solid #1f2937; border-radius:6px;
                       padding:6px 8px; color:#f3f4f6; }
            QDialog { background:#0b0f19; }
        )");

        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        QVBoxLayout* raiz = new QVBoxLayout(central);
        raiz->setContentsMargins(20, 20, 20, 20);
        raiz->setSpacing(12);

        abas = new QTabWidget(this);
        abas->addTab(construirAbaValePena(), "Vale a pena");
        abas->addTab(construirAbaCatalogo(), "Catálogo");
        abas->addTab(construirAbaReliquias(), "Relíquias");
        abas->addTab(construirAbaMinhasOrdens(), "As minhas ordens");
        raiz->addWidget(abas);

        api = new WarframeApiClient(this);
        auth = new WfmAuth(this);
        wfstat = new WfStatClient(this);

        // Ligações da API à UI
        connect(api, &WarframeApiClient::statusGlobal, this, [this](const QString& msg, bool erro, bool ok) {
            mostrarEstado(msg, erro, ok);
        });

        connect(api, &WarframeApiClient::catalogoCarregado, this, [this]() {
            refreshBtn->setEnabled(true);
            pagina = 0;
            filtrar();
            api->carregarMovimento();
        });

        connect(api, &WarframeApiClient::precoRecebido, this, [this](const QString& slug, const Precos& p) {
            cachePrecos.insert(chaveCache(slug, p.rankUsado), p);

            // Resposta de um rank que já não é o selecionado: fica na cache
            // (é válida) mas não vai para a tabela, que está a mostrar outro.
            const ItemApi* it = api->itemDeSlug(slug);
            if (it && api->rankDoItem(*it) != p.rankUsado) return;

            preencherLinhaCatalogo(slug, p);
            if (slug == slugSelecionado()) mostrarSugestao(slug, p);
        });

        connect(api, &WarframeApiClient::filaProgresso, this, [this](int feitos, int total) {
            if (feitos >= total) {
                table->setSortingEnabled(true);
                // Decrescente: as células sem ordens têm chave -1 e vão para o fundo.
                // Em ordem crescente apareceriam todas no topo, que é o oposto do útil.
                table->sortByColumn(CatVenda, Qt::DescendingOrder);

                const int linhas = table->rowCount();
                if (linhas > 0) mostrarEstado(QString("%1 nesta página").arg(plural(linhas, "item", "itens")), false, true);
            } else {
                mostrarEstado(QString("preços %1/%2").arg(feitos).arg(total), false, false);
            }
        });

        connect(api, &WarframeApiClient::statusMovimento, this, [this](const QString& msg) {
            statusMov->setText(msg);
        });

        connect(api, &WarframeApiClient::movimentoCarregado, this, [this](const QList<Movimento>& lista, int ordensLidas) {
            refreshMovBtn->setEnabled(true);
            desenharMovimento(lista);
            statusMov->setText(QString("%1 ordens · %2 itens").arg(ordensLidas).arg(lista.size()));
        });

        // ------------------------------------------------ ligações da conta ---

        connect(auth, &WfmAuth::sessaoIniciada, this, [this](const QString& nome) {
            btnConta->setText("● " + nome);
            btnConta->setToolTip("Clica para terminar sessão");
            estadoConta("sessão iniciada", "#10b981");
            atualizarBarraOrdem();
            auth->carregarMinhasOrdens();
        });

        connect(auth, &WfmAuth::sessaoTerminada, this, [this]() {
            btnConta->setText("Ligar conta");
            btnConta->setToolTip("Iniciar sessão no warframe.market");
            atualizarBarraOrdem();
            minhasOrdens.clear();
            tabelaOrdens->setRowCount(0);
            estadoConta("sem sessão", "#9ca3af");
            btnRecarregarOrdens->setEnabled(false);
            desativarAcoes();
        });

        connect(auth, &WfmAuth::erro, this, [this](const QString& msg) {
            estadoConta(msg, "#ef4444");
            btnConta->setEnabled(true);
            btnRecarregarOrdens->setEnabled(auth->autenticado());
            atualizarAcoesOrdem();
        });

        connect(auth, &WfmAuth::ordemCriada, this, [this](const QString&, int platinum) {
            mostrarEstado(QString("Ordem publicada a %1 p").arg(platinum), false, true);
            barraCat.vender->setEnabled(true);
            barraCat.comprar->setEnabled(true);
            // Recota o item para veres o teu próprio anúncio refletido na tabela.
            const QString slug = slugSelecionado();
            if (!slug.isEmpty()) {
                cachePrecos.remove(chaveAtual(slug));
                api->recotar(slug);
            }
            auth->carregarMinhasOrdens();
        });

        connect(auth, &WfmAuth::ordensRecebidas, this,
                [this](const QList<MinhaOrdem>& ordens, const QString& endpoint) {
                    minhasOrdens = ordens;
                    desenharMinhasOrdens();
                    btnRecarregarOrdens->setEnabled(true);
                    estadoConta(ordens.isEmpty()
                                    ? "nenhuma ordem ativa"
                                    : QString("%1 · %2").arg(plural(ordens.size(), "ordem", "ordens"),
                                                             endpoint.section('/', 3)),
                                ordens.isEmpty() ? "#9ca3af" : "#10b981");
                });

        connect(auth, &WfmAuth::ordemApagada, this, [this](const QString&) {
            estadoConta("ordem apagada", "#10b981");
            auth->carregarMinhasOrdens();
        });

        connect(auth, &WfmAuth::ordemAlterada, this, [this](const QString&) {
            estadoConta("ordem alterada", "#10b981");
            auth->carregarMinhasOrdens();
        });

        connect(auth, &WfmAuth::loteProgresso, this, [this](int feitos, int total) {
            estadoConta(QString("a alterar %1/%2...").arg(feitos).arg(total), "#9ca3af");
        });

        connect(auth, &WfmAuth::loteConcluido, this, [this](int feitos, int falhas) {
            estadoConta(falhas == 0
                            ? QString("%1 alteradas").arg(plural(feitos, "ordem", "ordens"))
                            : QString("%1 alteradas, %2 falharam").arg(feitos).arg(falhas),
                        falhas == 0 ? "#10b981" : "#f59e0b");
            btnRecarregarOrdens->setEnabled(true);
            auth->carregarMinhasOrdens();
        });

        connect(auth, &WfmAuth::ordemFechada, this, [this](const QString&) {
            estadoConta("venda registada", "#10b981");
            auth->carregarMinhasOrdens();
        });

        connect(wfstat, &WfStatClient::reliquiasCarregadas, this, [this](int total, int ativas) {
            desenharReliquias();
            statusRel->setText(QString("%1 · %2 ativas").arg(plural(total, "relíquia", "relíquias")).arg(ativas));
            // Só agora se sabe a origem dos itens: refaz os dois painéis do Catálogo.
            aoMudarSelecao();
        });

        connect(wfstat, &WfStatClient::precosCarregados, this, [this](int) {
            desenharReliquias();   // o valor esperado depende dos preços
        });

        connect(wfstat, &WfStatClient::erro, this, [this](const QString& msg) {
            statusRel->setText(msg);
            statusRel->setStyleSheet("color:#ef4444; font-size:12px; font-weight:bold;");
        });

        refreshBtn->setEnabled(false);
        api->carregarCatalogo();
        wfstat->carregar();
        auth->restaurarSessao();   // só faz algo se compilado com WFM_KEYCHAIN
    }

private:
    WarframeApiClient* api;
    WfmAuth* auth;
    WfStatClient* wfstat;
    QTabWidget* abas;

    // --- UI: Catálogo ---
    QLineEdit* searchEdit;
    QLabel* statusLabel;
    QPushButton* refreshBtn;
    QPushButton* btnConta;
    QButtonGroup* filterGroup;
    QTableWidget* table;
    QPushButton* btnAnterior;
    QPushButton* btnSeguinte;
    QLabel* labelPagina;

    // Sub-tabs no fundo do Catálogo: "Vender" e "Onde obter".
    // Partilham a seleção da tabela acima — nenhuma delas tem tabela própria.
    QTabWidget* abasCatalogo;
    QLabel* painelPrecos;    // aba "Vender"
    QLabel* painelObter;     // aba "Onde obter"

    // --- UI: barra de ordem ---
    struct BarraOrdem {
        QWidget* raiz = nullptr;
        QSpinBox* preco = nullptr;
        QSpinBox* qtd = nullptr;
        QCheckBox* visivel = nullptr;
        QPushButton* vender = nullptr;
        QPushButton* comprar = nullptr;
    };
    BarraOrdem barraCat;

    // --- UI: Relíquias ---
    QTableWidget* tabelaRel;
    QLabel* statusRel;
    QCheckBox* chkSoAtivas;
    QLabel* painelRel;

    // --- UI: As minhas ordens ---
    QTableWidget* tabelaOrdens;
    QLabel* statusOrdens;
    QPushButton* btnRecarregarOrdens;
    QPushButton* btnApagarOrdem;
    QPushButton* btnVendida;
    QPushButton* btnMaisUm;
    QPushButton* btnVisivel;
    QPushButton* btnEditarPreco;
    QPushButton* btnOcultarTodas;
    QPushButton* btnMostrarTodas;
    QList<MinhaOrdem> minhasOrdens;

    // --- UI: Vale a pena ---
    QTableWidget* tabelaMov;
    QLabel* statusMov;
    QPushButton* refreshMovBtn;

    // A chave inclui o rank: rank 0 e rank máximo do mesmo item são dois
    // preços diferentes e não se podem sobrepor na cache.
    QHash<QString, Precos> cachePrecos;
    QComboBox* comboRank;
    QStringList slugsFiltrados;
    int pagina = 0;

    enum ColCat { CatNome, CatCategoria, CatRank, CatVenda, CatCompra };
    enum ColMov { MovItem, MovPreco, MovOrdens, MovProcura, MovPotencial };
    enum ColOrd { OrdItem, OrdTipo, OrdPreco, OrdQtd, OrdRank, OrdVisivel };
    enum ColRel { RelNome, RelEstado, RelValor, RelMelhor, RelPrecoMelhor };

    // Estado das coisas da conta: aparece na aba "As minhas ordens", que é
    // onde o botão de login agora vive.
    void estadoConta(const QString& msg, const QString& cor) {
        statusOrdens->setText(msg);
        statusOrdens->setStyleSheet(QString("color:%1; font-size:12px; font-weight:bold;").arg(cor));
    }

    // A chave tem de vir do rank a que o preço DIZ RESPEITO, não do rank
    // selecionado agora: uma resposta que estava em trânsito quando se muda o
    // seletor chegaria depois e seria arquivada sob o rank errado.
    static QString chaveCache(const QString& slug, int rank) {
        return slug + "|" + QString::number(rank);
    }

    QString chaveAtual(const QString& slug) const {
        const ItemApi* it = api->itemDeSlug(slug);
        return chaveCache(slug, it ? api->rankDoItem(*it) : -1);
    }

    void mostrarEstado(const QString& msg, bool erro, bool ok) {
        statusLabel->setText(msg);
        if (erro)      statusLabel->setStyleSheet("color:#ef4444; font-size:12px; font-weight:bold;");
        else if (ok)   statusLabel->setStyleSheet("color:#10b981; font-size:12px; font-weight:bold;");
        else           statusLabel->setStyleSheet("color:#9ca3af; font-size:12px; font-weight:bold;");
    }

    // ======================================================= INTERFACE DE UI ===

    QWidget* construirAbaCatalogo() {
        QWidget* aba = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(aba);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(14);

        QHBoxLayout* topo = new QHBoxLayout();
        searchEdit = new QLineEdit(aba);
        searchEdit->setPlaceholderText("Pesquisar item (ex: Glaive, Energize, Blind Rage)...");
        searchEdit->setClearButtonEnabled(true);
        searchEdit->setStyleSheet("QLineEdit { background-color:#111827; border:1px solid #1f2937; border-radius:8px; padding:10px 15px; color:#f3f4f6; font-size:14px; } QLineEdit:focus { border:1px solid #3b82f6; }");
        connect(searchEdit, &QLineEdit::textChanged, this, [this]() { pagina = 0; filtrar(); });

        refreshBtn = new QPushButton("Atualizar", aba);
        refreshBtn->setCursor(Qt::PointingHandCursor);
        refreshBtn->setStyleSheet(estiloBotao());
        connect(refreshBtn, &QPushButton::clicked, this, [this]() {
            cachePrecos.clear();
            refreshBtn->setEnabled(false);
            api->carregarCatalogo();
        });

        comboRank = new QComboBox(aba);
        comboRank->addItem("Preços: rank 0");
        comboRank->addItem("Preços: rank máx.");
        comboRank->setToolTip("Mods e arcanas: rank 0 e rank máximo são mercados "
                              "completamente diferentes");
        comboRank->setStyleSheet(
            "QComboBox { background:#111827; border:1px solid #1f2937; border-radius:8px;"
            " padding:9px 14px; color:#cbd5e1; font-weight:bold; }");
        connect(comboRank, &QComboBox::currentIndexChanged, this, [this](int i) {
            api->definirRankPreferido(i);
            desenharPagina();          // a cache é por rank, o que já foi cotado reaparece
        });

        statusLabel = new QLabel(aba);
        statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        topo->addWidget(searchEdit, 3);
        topo->addWidget(comboRank, 0);
        topo->addWidget(refreshBtn, 0);
        topo->addWidget(statusLabel, 1);
        layout->addLayout(topo);

        QHBoxLayout* cats = new QHBoxLayout();
        cats->setSpacing(10);
        filterGroup = new QButtonGroup(this);
        filterGroup->setExclusive(true);
        for (int i = 0; i < kCategorias.size(); ++i) {
            QPushButton* btn = new QPushButton(kCategorias[i], aba);
            btn->setCheckable(true);
            btn->setCursor(Qt::PointingHandCursor);
            if (i == 0) btn->setChecked(true);
            btn->setStyleSheet(estiloPilula());
            filterGroup->addButton(btn, i);
            cats->addWidget(btn);
        }
        connect(filterGroup, &QButtonGroup::idClicked, this, [this]() { pagina = 0; filtrar(); });
        cats->addStretch();
        layout->addLayout(cats);

        table = new QTableWidget(aba);
        table->setColumnCount(5);
        table->setHorizontalHeaderLabels({"NOME DO ITEM", "CATEGORIA", "RANK", "VENDA (p)", "COMPRA (p)"});
        prepararTabela(table);
        table->horizontalHeader()->setSectionResizeMode(CatNome, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(CatRank, QHeaderView::ResizeToContents);
        connect(table, &QTableWidget::itemSelectionChanged, this, &MainWindow::aoMudarSelecao);
        layout->addWidget(table, 1);

        QHBoxLayout* pag = new QHBoxLayout();
        btnAnterior = new QPushButton("‹ Anterior", aba);
        btnSeguinte = new QPushButton("Seguinte ›", aba);
        labelPagina = new QLabel(aba);
        labelPagina->setAlignment(Qt::AlignCenter);
        labelPagina->setStyleSheet("color:#9ca3af; font-size:12px; font-weight:bold;");
        for (QPushButton* b : {btnAnterior, btnSeguinte}) {
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(estiloBotao());
        }
        connect(btnAnterior, &QPushButton::clicked, this, [this]() {
            if (pagina > 0) { --pagina; desenharPagina(); }
        });
        connect(btnSeguinte, &QPushButton::clicked, this, [this]() {
            if ((pagina + 1) * kPorPagina < slugsFiltrados.size()) { ++pagina; desenharPagina(); }
        });
        pag->addWidget(btnAnterior);
        pag->addWidget(labelPagina, 1);
        pag->addWidget(btnSeguinte);
        layout->addLayout(pag);

        layout->addWidget(construirAbasInferiores(aba), 0);

        return aba;
    }

    // As duas sub-tabs do fundo. A tabela acima continua sempre visível; só
    // muda o que está aqui dentro.
    QWidget* construirAbasInferiores(QWidget* pai) {
        abasCatalogo = new QTabWidget(pai);
        abasCatalogo->setStyleSheet(
            "QTabWidget::pane { border:1px solid #1f2937; border-radius:8px; top:-1px; }"
            "QTabBar::tab { background:#0b0f19; color:#6b7280; padding:5px 16px;"
            " border:1px solid #1f2937; border-bottom:none;"
            " border-top-left-radius:5px; border-top-right-radius:5px;"
            " font-weight:bold; font-size:11px; margin-right:3px; }"
            "QTabBar::tab:selected { background:#1f2937; color:#e2e8f0; border-color:#374151; }");

        // ---- aba "Vender" ----
        QWidget* abaVender = new QWidget(abasCatalogo);
        QVBoxLayout* lv = new QVBoxLayout(abaVender);
        lv->setContentsMargins(12, 12, 12, 12);
        lv->setSpacing(10);

        painelPrecos = new QLabel(abaVender);
        painelPrecos->setTextFormat(Qt::RichText);
        painelPrecos->setWordWrap(true);
        painelPrecos->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        painelPrecos->setStyleSheet("color:#cbd5e1; font-size:13px;");
        painelPrecos->setText("<span style='color:#6b7280'>Seleciona um item para a sugestão de preço.</span>");
        lv->addWidget(painelPrecos, 1);

        barraCat = construirBarraOrdem(abaVender);
        lv->addWidget(barraCat.raiz, 0);

        abasCatalogo->addTab(abaVender, "Vender");

        // ---- aba "Onde obter" ----
        QWidget* abaObter = new QWidget(abasCatalogo);
        QVBoxLayout* lo = new QVBoxLayout(abaObter);
        lo->setContentsMargins(12, 12, 12, 12);

        painelObter = new QLabel(abaObter);
        painelObter->setTextFormat(Qt::RichText);
        painelObter->setWordWrap(true);
        painelObter->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        painelObter->setStyleSheet("color:#cbd5e1; font-size:13px;");
        painelObter->setText("<span style='color:#6b7280'>Seleciona um item no catálogo.</span>");
        lo->addWidget(painelObter, 1);

        abasCatalogo->addTab(abaObter, "Onde obter");

        abasCatalogo->setMinimumHeight(180);
        return abasCatalogo;
    }

    BarraOrdem construirBarraOrdem(QWidget* pai) {
        BarraOrdem b;
        b.raiz = new QWidget(pai);
        QHBoxLayout* l = new QHBoxLayout(b.raiz);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(10);

        QLabel* rot = new QLabel("Anunciar:", b.raiz);
        rot->setStyleSheet("color:#9ca3af; font-weight:bold; font-size:12px;");

        b.preco = new QSpinBox(b.raiz);
        b.preco->setRange(1, 999999);
        b.preco->setSuffix(" p");
        b.preco->setValue(1);

        b.qtd = new QSpinBox(b.raiz);
        b.qtd->setRange(1, 999);
        b.qtd->setPrefix("× ");
        b.qtd->setValue(1);

        b.visivel = new QCheckBox("visível", b.raiz);
        b.visivel->setChecked(true);
        b.visivel->setStyleSheet("color:#9ca3af; font-size:12px;");

        b.vender = new QPushButton("Vender", b.raiz);
        b.vender->setCursor(Qt::PointingHandCursor);
        b.vender->setStyleSheet(estiloBotao() + "QPushButton { color:#10b981; }");
        connect(b.vender, &QPushButton::clicked, this, [this]() { publicarOrdem("sell"); });

        b.comprar = new QPushButton("Comprar", b.raiz);
        b.comprar->setCursor(Qt::PointingHandCursor);
        b.comprar->setStyleSheet(estiloBotao() + "QPushButton { color:#3b82f6; }");
        connect(b.comprar, &QPushButton::clicked, this, [this]() { publicarOrdem("buy"); });

        l->addWidget(rot);
        l->addWidget(b.preco);
        l->addWidget(b.qtd);
        l->addWidget(b.visivel);
        l->addStretch();
        l->addWidget(b.vender);
        l->addWidget(b.comprar);

        b.raiz->setEnabled(false);
        return b;
    }

    QWidget* construirAbaReliquias() {
        QWidget* aba = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(aba);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(14);

        QHBoxLayout* topo = new QHBoxLayout();
        QLabel* titulo = new QLabel("Valor esperado por relíquia intacta", aba);
        titulo->setStyleSheet("color:#f8fafc; font-size:15px; font-weight:bold;");

        chkSoAtivas = new QCheckBox("Esconder vaulted", aba);
        chkSoAtivas->setChecked(true);
        chkSoAtivas->setCursor(Qt::PointingHandCursor);
        chkSoAtivas->setStyleSheet("color:#cbd5e1; font-size:13px; font-weight:bold;");
        connect(chkSoAtivas, &QCheckBox::toggled, this, [this]() { desenharReliquias(); });

        statusRel = new QLabel("a carregar...", aba);
        statusRel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        statusRel->setStyleSheet("color:#9ca3af; font-size:12px; font-weight:bold;");

        topo->addWidget(titulo, 2);
        topo->addWidget(chkSoAtivas, 0);
        topo->addWidget(statusRel, 1);
        layout->addLayout(topo);

        tabelaRel = new QTableWidget(aba);
        tabelaRel->setColumnCount(5);
        tabelaRel->setHorizontalHeaderLabels(
            {"RELÍQUIA", "ESTADO", "VALOR ESPERADO (p)", "MELHOR DROP", "PREÇO (p)"});
        prepararTabela(tabelaRel);
        tabelaRel->horizontalHeader()->setSectionResizeMode(RelMelhor, QHeaderView::Stretch);
        connect(tabelaRel, &QTableWidget::itemSelectionChanged, this, &MainWindow::mostrarDetalheReliquia);
        layout->addWidget(tabelaRel, 1);

        painelRel = new QLabel(aba);
        painelRel->setTextFormat(Qt::RichText);
        painelRel->setWordWrap(true);
        painelRel->setMinimumHeight(110);
        painelRel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        painelRel->setStyleSheet(estiloPainel());
        painelRel->setText("<span style='color:#6b7280'>Seleciona uma relíquia para ver a tabela de drops.</span>");
        layout->addWidget(painelRel, 0);

        return aba;
    }

    void desenharReliquias() {
        if (!wfstat->temReliquias()) return;

        const bool esconderVaulted = chkSoAtivas->isChecked();

        tabelaRel->setSortingEnabled(false);
        tabelaRel->setUpdatesEnabled(false);
        tabelaRel->setRowCount(0);

        int mostradas = 0;
        for (const Reliquia& r : wfstat->reliquias()) {
            if (esconderVaulted && r.vaulted) continue;

            const int row = tabelaRel->rowCount();
            tabelaRel->insertRow(row);

            QTableWidgetItem* nome = new QTableWidgetItem(r.etiqueta());
            QFont f = nome->font(); f.setBold(true); nome->setFont(f);
            nome->setData(Qt::UserRole, r.etiqueta());
            tabelaRel->setItem(row, RelNome, nome);

            QTableWidgetItem* estado = new QTableWidgetItem(r.vaulted ? "vaulted" : "ativa");
            estado->setForeground(QBrush(QColor(r.vaulted ? "#f59e0b" : "#10b981")));
            tabelaRel->setItem(row, RelEstado, estado);

            CelulaNumero* valor = new CelulaNumero();
            valor->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            const int ev = qRound(wfstat->valorEsperado(r));
            valor->definir(ev);
            valor->setForeground(QBrush(QColor(ev >= 20 ? "#38bdf8" : "#cbd5e1")));
            tabelaRel->setItem(row, RelValor, valor);

            const Drop melhor = wfstat->melhorDrop(r);
            QTableWidgetItem* dropItem = new QTableWidgetItem(melhor.item);
            dropItem->setForeground(QBrush(QColor("#cbd5e1")));
            tabelaRel->setItem(row, RelMelhor, dropItem);

            CelulaNumero* precoMelhor = new CelulaNumero();
            precoMelhor->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            precoMelhor->definir(qRound(wfstat->preco(melhor.item).media));
            precoMelhor->setForeground(QBrush(QColor("#f8fafc")));
            tabelaRel->setItem(row, RelPrecoMelhor, precoMelhor);

            ++mostradas;
        }

        tabelaRel->setUpdatesEnabled(true);
        tabelaRel->setSortingEnabled(true);
        tabelaRel->sortByColumn(RelValor, Qt::DescendingOrder);

        if (mostradas == 0 && esconderVaulted) {
            painelRel->setText("<span style='color:#f59e0b'>Nenhuma relíquia ativa na lista.</span>");
        }
    }

    void mostrarDetalheReliquia() {
        const QList<QTableWidgetItem*> sel = tabelaRel->selectedItems();
        if (sel.isEmpty()) return;
        QTableWidgetItem* base = tabelaRel->item(sel.first()->row(), RelNome);
        if (!base) return;

        const QString etiqueta = base->data(Qt::UserRole).toString();
        for (const Reliquia& r : wfstat->reliquias()) {
            if (r.etiqueta() != etiqueta) continue;

            QStringList linhas;
            linhas << QString("<b style='color:#f8fafc; font-size:14px'>%1</b>  %2")
                          .arg(r.etiqueta(),
                               r.vaulted
                                   ? "<span style='color:#f59e0b'>vaulted — não obtenível</span>"
                                   : "<span style='color:#10b981'>ativa</span>");

            for (const Drop& d : r.drops) {
                const double p = wfstat->preco(d.item).media;
                const QString cor = d.raridade == "rara" ? "#fbbf24"
                                    : d.raridade == "incomum" ? "#cbd5e1" : "#6b7280";
                linhas << QString("<span style='color:%1'>%2%</span> &nbsp; %3 &nbsp;"
                                  "<b style='color:#f8fafc'>%4</b>")
                              .arg(cor)
                              .arg(d.chance * 100, 0, 'f', d.raridade == "comum" ? 2 : 0)
                              .arg(d.item)
                              .arg(p > 0 ? QString("%1 p").arg(qRound(p)) : QString("—"));
            }
            linhas << QString("<b>Valor esperado por abertura: %1 p</b>")
                          .arg(qRound(wfstat->valorEsperado(r)));
            painelRel->setText(linhas.join("<br>"));
            return;
        }
    }

    QWidget* construirAbaMinhasOrdens() {
        QWidget* aba = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(aba);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(14);

        QHBoxLayout* topo = new QHBoxLayout();

        btnConta = new QPushButton("Ligar conta", aba);
        btnConta->setCursor(Qt::PointingHandCursor);
        btnConta->setStyleSheet(estiloBotao());
        btnConta->setToolTip("Iniciar sessão no warframe.market");
        connect(btnConta, &QPushButton::clicked, this, &MainWindow::aoClicarConta);

        QLabel* titulo = new QLabel("Ordens publicadas na tua conta", aba);
        titulo->setStyleSheet("color:#f8fafc; font-size:15px; font-weight:bold;");

        btnRecarregarOrdens = new QPushButton("Recarregar", aba);
        btnRecarregarOrdens->setCursor(Qt::PointingHandCursor);
        btnRecarregarOrdens->setStyleSheet(estiloBotao());
        btnRecarregarOrdens->setEnabled(false);
        connect(btnRecarregarOrdens, &QPushButton::clicked, this, [this]() {
            btnRecarregarOrdens->setEnabled(false);
            auth->carregarMinhasOrdens();
        });

        statusOrdens = new QLabel("sem sessão", aba);
        statusOrdens->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        statusOrdens->setStyleSheet("color:#9ca3af; font-size:12px; font-weight:bold;");

        topo->addWidget(btnConta, 0);
        topo->addWidget(titulo, 2);
        topo->addWidget(btnRecarregarOrdens, 0);
        topo->addWidget(statusOrdens, 1);
        layout->addLayout(topo);

        tabelaOrdens = new QTableWidget(aba);
        tabelaOrdens->setColumnCount(6);
        tabelaOrdens->setHorizontalHeaderLabels({"ITEM", "TIPO", "PREÇO (p)", "QTD", "RANK", "VISÍVEL"});
        prepararTabela(tabelaOrdens);
        tabelaOrdens->horizontalHeader()->setSectionResizeMode(OrdItem, QHeaderView::Stretch);
        connect(tabelaOrdens, &QTableWidget::itemSelectionChanged,
                this, &MainWindow::atualizarAcoesOrdem);
        layout->addWidget(tabelaOrdens, 1);

        // Ações sobre a linha selecionada. Preferi uma barra a botões dentro de
        // cada linha: com cellWidget, a tabela reconstrói dezenas de widgets a
        // cada recarregamento, e aqui não ganhava nada com isso.
        QHBoxLayout* acoes = new QHBoxLayout();
        acoes->setSpacing(8);

        btnVendida = new QPushButton("✔ Vendida", aba);
        btnVendida->setStyleSheet(estiloBotao() + "QPushButton { color:#10b981; }");
        connect(btnVendida, &QPushButton::clicked, this, [this]() {
            const MinhaOrdem* o = ordemSelecionada();
            if (!o) return;
            desativarAcoes();
            mostrarEstadoOrdem("a fechar a ordem...");
            auth->marcarVendida(o->id);
        });

        btnEditarPreco = new QPushButton("Editar preço", aba);
        btnEditarPreco->setStyleSheet(estiloBotao());
        connect(btnEditarPreco, &QPushButton::clicked, this, &MainWindow::editarPrecoOrdem);

        btnMaisUm = new QPushButton("+1", aba);
        btnMaisUm->setStyleSheet(estiloBotao());
        connect(btnMaisUm, &QPushButton::clicked, this, [this]() {
            const MinhaOrdem* o = ordemSelecionada();
            if (!o) return;
            desativarAcoes();
            mostrarEstadoOrdem("a somar 1 à quantidade...");
            auth->alterarOrdem(o->id, {{"quantity", o->quantity + 1}});
        });

        btnVisivel = new QPushButton("Ocultar", aba);
        btnVisivel->setStyleSheet(estiloBotao());
        connect(btnVisivel, &QPushButton::clicked, this, [this]() {
            const MinhaOrdem* o = ordemSelecionada();
            if (!o) return;
            desativarAcoes();
            mostrarEstadoOrdem(o->visible ? "a ocultar..." : "a tornar visível...");
            auth->alterarOrdem(o->id, {{"visible", !o->visible}});
        });

        btnApagarOrdem = new QPushButton("Apagar", aba);
        btnApagarOrdem->setStyleSheet(estiloBotao() + "QPushButton { color:#ef4444; }");
        connect(btnApagarOrdem, &QPushButton::clicked, this, &MainWindow::apagarOrdemSelecionada);

        for (QPushButton* b : {btnVendida, btnEditarPreco, btnMaisUm, btnVisivel, btnApagarOrdem}) {
            b->setCursor(Qt::PointingHandCursor);
            b->setEnabled(false);
            acoes->addWidget(b);
        }
        acoes->addStretch();

        // Ações sobre TODAS as ordens. Separadas das anteriores de propósito:
        // estas não dependem da linha selecionada e mexem em tudo de uma vez.
        btnOcultarTodas = new QPushButton("Ocultar todas", aba);
        btnMostrarTodas = new QPushButton("Mostrar todas", aba);
        for (QPushButton* b : {btnOcultarTodas, btnMostrarTodas}) {
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(estiloBotao());
            b->setEnabled(false);
            acoes->addWidget(b);
        }
        connect(btnOcultarTodas, &QPushButton::clicked, this, [this]() { visibilidadeEmLote(false); });
        connect(btnMostrarTodas, &QPushButton::clicked, this, [this]() { visibilidadeEmLote(true); });

        layout->addLayout(acoes);

        QLabel* nota = new QLabel(aba);
        nota->setWordWrap(true);
        nota->setTextFormat(Qt::RichText);
        nota->setStyleSheet(estiloPainel());
        nota->setText(
            "Liga a conta no botão à esquerda. Sem sessão, o resto da app funciona "
            "na mesma — só a publicação de ordens fica indisponível.<br>"
            "Apagar uma ordem aqui remove o anúncio do site — não desfaz trocas já feitas.<br>"
            "<span style='color:#6b7280'>O endpoint usado aparece no canto superior direito. "
            "Se a lista vier vazia mas tiveres ordens no site, é sinal de que o caminho da API "
            "mudou — o log [auth] na consola mostra o que foi tentado.</span>");
        layout->addWidget(nota, 0);

        return aba;
    }

    void desenharMinhasOrdens() {
        tabelaOrdens->setSortingEnabled(false);
        tabelaOrdens->setUpdatesEnabled(false);
        tabelaOrdens->setRowCount(0);

        for (const MinhaOrdem& o : minhasOrdens) {
            const int row = tabelaOrdens->rowCount();
            tabelaOrdens->insertRow(row);

            // O v2 devolve itemId; o nome vem do catálogo que já temos em memória.
            QString nomeItem = o.slug;
            for (const ItemApi& it : api->todosItens()) {
                if ((!o.itemId.isEmpty() && it.id == o.itemId) ||
                    (!o.slug.isEmpty() && it.slug == o.slug)) { nomeItem = it.name; break; }
            }
            if (nomeItem.isEmpty()) nomeItem = o.itemId;

            QTableWidgetItem* nome = new QTableWidgetItem(nomeItem);
            QFont f = nome->font(); f.setBold(true); nome->setFont(f);
            nome->setData(Qt::UserRole, o.id);          // id da ordem, para apagar
            tabelaOrdens->setItem(row, OrdItem, nome);

            const bool venda = (o.tipo == "sell");
            QTableWidgetItem* tipo = new QTableWidgetItem(venda ? "venda" : "compra");
            tipo->setForeground(QBrush(QColor(venda ? "#10b981" : "#3b82f6")));
            tabelaOrdens->setItem(row, OrdTipo, tipo);

            CelulaNumero* preco = new CelulaNumero();
            preco->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            preco->definir(o.platinum);
            tabelaOrdens->setItem(row, OrdPreco, preco);

            CelulaNumero* qtd = new CelulaNumero();
            qtd->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            qtd->definir(o.quantity);
            tabelaOrdens->setItem(row, OrdQtd, qtd);

            QTableWidgetItem* rk = new QTableWidgetItem(
                o.rank > 0 ? QString::number(o.rank) : QStringLiteral("—"));
            rk->setTextAlignment(Qt::AlignCenter);
            rk->setForeground(QBrush(QColor("#6b7280")));
            tabelaOrdens->setItem(row, OrdRank, rk);

            QTableWidgetItem* vis = new QTableWidgetItem(o.visible ? "sim" : "oculta");
            vis->setForeground(QBrush(QColor(o.visible ? "#cbd5e1" : "#6b7280")));
            tabelaOrdens->setItem(row, OrdVisivel, vis);
        }

        tabelaOrdens->setUpdatesEnabled(true);
        tabelaOrdens->setSortingEnabled(true);
        atualizarAcoesOrdem();
    }

    const MinhaOrdem* ordemSelecionada() const {
        const QList<QTableWidgetItem*> sel = tabelaOrdens->selectedItems();
        if (sel.isEmpty()) return nullptr;
        QTableWidgetItem* base = tabelaOrdens->item(sel.first()->row(), OrdItem);
        if (!base) return nullptr;
        const QString id = base->data(Qt::UserRole).toString();
        for (const MinhaOrdem& o : minhasOrdens) if (o.id == id) return &o;
        return nullptr;
    }

    void desativarAcoes() {
        for (QPushButton* b : {btnVendida, btnEditarPreco, btnMaisUm, btnVisivel, btnApagarOrdem,
                               btnOcultarTodas, btnMostrarTodas})
            b->setEnabled(false);
    }

    void mostrarEstadoOrdem(const QString& msg) { estadoConta(msg, "#9ca3af"); }

    void atualizarAcoesOrdem() {
        const MinhaOrdem* o = ordemSelecionada();
        const bool pronto = auth->autenticado() && o != nullptr;
        const bool temOrdens = auth->autenticado() && !minhasOrdens.isEmpty();
        btnOcultarTodas->setEnabled(temOrdens);
        btnMostrarTodas->setEnabled(temOrdens);
        for (QPushButton* b : {btnVendida, btnEditarPreco, btnMaisUm, btnVisivel, btnApagarOrdem})
            b->setEnabled(pronto);
        // "Vendida" só faz sentido em ordens de venda.
        if (pronto && o->tipo != "sell") btnVendida->setEnabled(false);
        btnVisivel->setText(pronto && !o->visible ? "Mostrar" : "Ocultar");
    }

    void visibilidadeEmLote(bool visivel) {
        // Só mexe nas que ainda não estão no estado pretendido: poupa pedidos,
        // que é o recurso escasso aqui.
        QStringList ids;
        for (const MinhaOrdem& o : minhasOrdens)
            if (o.visible != visivel) ids << o.id;

        if (ids.isEmpty()) {
            estadoConta(visivel ? "já estão todas visíveis" : "já estão todas ocultas", "#9ca3af");
            return;
        }

        const int segundos = (ids.size() * 350) / 1000 + 1;
        const auto resposta = QMessageBox::question(
            this, visivel ? "Mostrar todas" : "Ocultar todas",
            QString("%1 %2 ordens.\n\nSão %2 pedidos à API, cerca de %3 segundos "
                    "por causa do limite de 3 por segundo. Continuar?")
                .arg(visivel ? "Mostrar" : "Ocultar").arg(ids.size()).arg(segundos),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (resposta != QMessageBox::Yes) return;

        desativarAcoes();
        btnOcultarTodas->setEnabled(false);
        btnMostrarTodas->setEnabled(false);
        btnRecarregarOrdens->setEnabled(false);
        auth->alterarVarias(ids, {{"visible", visivel}});
    }

    void editarPrecoOrdem() {
        const MinhaOrdem* o = ordemSelecionada();
        if (!o) return;
        bool ok = false;
        const int novo = QInputDialog::getInt(this, "Editar preço",
                                              "Novo preço em platina:", o->platinum,
                                              1, 999999, 1, &ok);
        if (!ok || novo == o->platinum) return;
        desativarAcoes();
        mostrarEstadoOrdem(QString("a alterar para %1 p...").arg(novo));
        auth->alterarOrdem(o->id, {{"platinum", novo}});
    }

    void apagarOrdemSelecionada() {
        const MinhaOrdem* o = ordemSelecionada();
        if (!o) return;

        const auto resposta = QMessageBox::question(
            this, "Apagar ordem",
            QString("Apagar a ordem de %1 a %2 p?").arg(o->tipo == "sell" ? "venda" : "compra")
                .arg(o->platinum),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (resposta != QMessageBox::Yes) return;

        desativarAcoes();
        mostrarEstadoOrdem("a apagar...");
        auth->apagarOrdem(o->id);
    }

    QWidget* construirAbaValePena() {
        QWidget* aba = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(aba);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(14);

        QHBoxLayout* topo = new QHBoxLayout();
        QLabel* titulo = new QLabel("Itens com mais movimento nas últimas 4 horas", aba);
        titulo->setStyleSheet("color:#f8fafc; font-size:15px; font-weight:bold;");

        refreshMovBtn = new QPushButton("Analisar mercado", aba);
        refreshMovBtn->setCursor(Qt::PointingHandCursor);
        refreshMovBtn->setStyleSheet(estiloBotao());
        connect(refreshMovBtn, &QPushButton::clicked, this, [this]() {
            refreshMovBtn->setEnabled(false);
            api->carregarMovimento();
        });

        statusMov = new QLabel(aba);
        statusMov->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        statusMov->setStyleSheet("color:#9ca3af; font-size:12px; font-weight:bold;");

        topo->addWidget(titulo, 2);
        topo->addWidget(refreshMovBtn, 0);
        topo->addWidget(statusMov, 1);
        layout->addLayout(topo);

        tabelaMov = new QTableWidget(aba);
        tabelaMov->setColumnCount(5);
        tabelaMov->setHorizontalHeaderLabels({"ITEM", "PREÇO MEDIANO (p)", "ORDENS 4h", "PROCURA", "POTENCIAL (p)"});
        prepararTabela(tabelaMov);
        tabelaMov->horizontalHeader()->setSectionResizeMode(MovItem, QHeaderView::Stretch);
        layout->addWidget(tabelaMov, 1);

        QLabel* legenda = new QLabel(aba);
        legenda->setWordWrap(true);
        legenda->setTextFormat(Qt::RichText);
        legenda->setStyleSheet(estiloPainel());
        legenda->setText(
            "<b style='color:#f8fafc'>Como ler isto</b><br>"
            "<b>Procura</b> = ordens de compra ÷ ordens de venda. Acima de 1,0 há mais gente a "
            "querer comprar do que a vender — é aí que tu tens vantagem.<br>"
            "<b>Potencial</b> = preço mediano × nº de ordens. Aproxima a platina que o item movimentou."
            );
        layout->addWidget(legenda, 0);

        return aba;
    }

    // ============================================================== CONTA ===

    void aoClicarConta() {
        if (auth->autenticado()) {
            auth->sair();
            return;   // o sinal sessaoTerminada trata do resto
        }

        DialogoLogin dlg(this);
        if (dlg.exec() != QDialog::Accepted) return;

        btnConta->setEnabled(false);
        estadoConta("a iniciar sessão...", "#9ca3af");
        auth->entrar(dlg.email(), dlg.password());
        btnConta->setEnabled(true);
    }

    void atualizarBarraOrdem() {
        const bool sessao = auth->autenticado();
        barraCat.raiz->setEnabled(sessao && !slugSelecionado().isEmpty());
        barraCat.raiz->setToolTip(
            sessao ? QString() : QString("Liga a tua conta no separador \"As minhas ordens\""));
    }

    void publicarOrdem(const QString& tipo) {
        const QString slug = slugSelecionado();
        const ItemApi* item = slug.isEmpty() ? nullptr : api->itemDeSlug(slug);
        if (!item || item->id.isEmpty()) {
            mostrarEstado("Item sem id — não é possível anunciar.", true, false);
            return;
        }

        barraCat.vender->setEnabled(false);
        barraCat.comprar->setEnabled(false);
        mostrarEstado(QString("A publicar %1 de %2...").arg(tipo, item->name), false, false);

        // A ordem sai no mesmo rank a que o preço mostrado diz respeito.
        // -1 significa "item sem ranks" e nesse caso não se envia rank nenhum.
        const int rank = api->rankDoItem(*item);

        auth->criarOrdem(item->id, tipo, barraCat.preco->value(), barraCat.qtd->value(),
                         rank, barraCat.visivel->isChecked());
    }

    // ============================================================== LÓGICA UI ===
    void filtrar() {
        const int idCategoria = filterGroup->checkedId();
        const QString categoria = (idCategoria >= 0 && idCategoria < kCategorias.size()) ? kCategorias[idCategoria] : kCategorias.first();
        const bool todas = (categoria == kCategorias.first());
        const QString pesquisa = searchEdit->text().trimmed();

        slugsFiltrados.clear();
        for (const ItemApi& item : api->todosItens()) {
            if (!todas && item.category != categoria) continue;
            if (!pesquisa.isEmpty() && !item.name.contains(pesquisa, Qt::CaseInsensitive)) continue;
            slugsFiltrados << item.slug;
        }
        desenharPagina();
    }

    void desenharPagina() {
        const int total = slugsFiltrados.size();
        const int paginas = std::max(1, (total + kPorPagina - 1) / kPorPagina);
        pagina = std::clamp(pagina, 0, paginas - 1);
        const int inicio = pagina * kPorPagina;
        const int fim = std::min(inicio + kPorPagina, total);

        table->setSortingEnabled(false);
        table->setUpdatesEnabled(false);
        table->setRowCount(0);

        QStringList porCotar;
        for (int i = inicio; i < fim; ++i) {
            const ItemApi* item = api->itemDeSlug(slugsFiltrados[i]);
            if (!item) continue;

            const int row = table->rowCount();
            table->insertRow(row);

            QTableWidgetItem* nome = new QTableWidgetItem(item->name);
            QFont f = nome->font();
            f.setBold(true);
            nome->setFont(f);
            nome->setToolTip(item->slug);
            nome->setData(Qt::UserRole, item->slug);

            QTableWidgetItem* cat = new QTableWidgetItem(item->category);
            cat->setForeground(QBrush(QColor("#9ca3af")));

            // Mostra o rank a que os preços da linha dizem respeito, não só o
            // máximo: ver "5" ao lado de um preço de rank 0 induzia em erro.
            QTableWidgetItem* rankItem = new QTableWidgetItem();
            rankItem->setTextAlignment(Qt::AlignCenter);
            if (item->maxRank > 0) {
                rankItem->setText(QString("%1 / %2").arg(api->rankDoItem(*item)).arg(item->maxRank));
            } else {
                rankItem->setText("—");
            }
            rankItem->setForeground(QBrush(QColor("#6b7280")));

            table->setItem(row, CatNome, nome);
            table->setItem(row, CatCategoria, cat);
            table->setItem(row, CatRank, rankItem);

            if (cachePrecos.contains(chaveAtual(item->slug))) {
                escreverCelulasPreco(row, cachePrecos.value(chaveAtual(item->slug)));
            } else {
                escreverCelulasPreco(row, Precos{});
                porCotar << item->slug;
            }
        }

        table->setUpdatesEnabled(true);
        labelPagina->setText(total == 0 ? "sem resultados" : QString("página %1 de %2  ·  %3 itens").arg(pagina + 1).arg(paginas).arg(total));
        btnAnterior->setEnabled(pagina > 0);
        btnSeguinte->setEnabled(pagina < paginas - 1);

        if (!api->todosItens().isEmpty()) api->agendarPrecos(porCotar);
        atualizarBarraOrdem();
    }

    void preencherLinhaCatalogo(const QString& slug, const Precos& p) {
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem* nome = table->item(row, CatNome);
            if (!nome || nome->data(Qt::UserRole).toString() != slug) continue;
            escreverCelulasPreco(row, p);
            return;
        }
    }

    void escreverCelulasPreco(int row, const Precos& p) {
        // Com a ordenação ligada, alterar a célula da VENDA faz a QTableWidget
        // reordenar de imediato: a linha muda de índice e a célula da COMPRA
        // seguinte ia parar à linha de outro item. Escrever as duas com a
        // ordenação suspensa mantém o par junto.
        const bool ordenava = table->isSortingEnabled();
        table->setSortingEnabled(false);

        auto celula = [&](int coluna, int valor, const QColor& cor) {
            CelulaNumero* it = dynamic_cast<CelulaNumero*>(table->item(row, coluna));
            if (!it) { it = new CelulaNumero(); table->setItem(row, coluna, it); }
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            it->definir(valor);
            if (valor > 0) {
                it->setForeground(QBrush(p.apenasOffline ? QColor("#f59e0b") : cor));
                QStringList dicas;
                if (p.rankUsado >= 0) dicas << QString("preço de rank %1").arg(p.rankUsado);
                if (p.apenasOffline) dicas << "só há ordens de jogadores offline";
                it->setToolTip(dicas.join(" · "));
            } else {
                it->setForeground(QBrush(QColor("#4b5563")));
            }
        };
        celula(CatVenda, p.menorVenda, QColor("#f8fafc"));
        celula(CatCompra, p.maiorCompra, QColor("#9ca3af"));

        table->setSortingEnabled(ordenava);
    }

    QString slugSelecionado() const {
        const QList<QTableWidgetItem*> sel = table->selectedItems();
        if (sel.isEmpty()) return {};
        QTableWidgetItem* nome = table->item(sel.first()->row(), CatNome);
        return nome ? nome->data(Qt::UserRole).toString() : QString();
    }

    // Uma seleção alimenta as duas sub-tabs de uma vez.
    void aoMudarSelecao() {
        const QString slug = slugSelecionado();
        atualizarBarraOrdem();

        if (slug.isEmpty()) {
            painelObter->setText("<span style='color:#6b7280'>Seleciona um item no catálogo.</span>");
            return;
        }

        const ItemApi* item = api->itemDeSlug(slug);
        painelObter->setText(item ? montarPainelObter(item->name)
                                  : QString("<span style='color:#6b7280'>—</span>"));

        if (cachePrecos.contains(chaveAtual(slug))) {
            mostrarSugestao(slug, cachePrecos.value(chaveAtual(slug)));
        } else {
            painelPrecos->setText("<span style='color:#6b7280'>Ainda por cotar — aguarda a fila.</span>");
            api->priorizarItem(slug);
        }
    }

    // O conteúdo da sub-tab "Onde obter": nome do item mais o bloco de origens.
    QString montarPainelObter(const QString& nomeItem) const {
        if (!wfstat->temReliquias()) {
            return QString("<b style='color:#f8fafc; font-size:14px'>%1</b><br>"
                           "<span style='color:#6b7280'>A carregar a tabela de "
                           "relíquias...</span>").arg(nomeItem);
        }
        const double p = wfstat->preco(nomeItem).media;
        return QString("<b style='color:#f8fafc; font-size:14px'>%1</b>%2%3")
            .arg(nomeItem,
                 p > 0 ? QString("  <span style='color:#6b7280'>· ~%1 p de referência</span>")
                             .arg(qRound(p))
                       : QString(),
                 blocoOndeObter(nomeItem));
    }

    // Onde é que este item se obtém, e se ainda é obtenível de todo.
    QString blocoOndeObter(const QString& nomeItem) const {
        if (!wfstat->temReliquias()) return {};

        const QList<Origem> origens = wfstat->ondeObter(nomeItem);
        if (origens.isEmpty()) {
            return "<br><span style='color:#6b7280'>Não vem de relíquias "
                   "(arcana, mod, riven ou item de outra fonte).</span>";
        }

        int ativas = 0;
        for (const Origem& o : origens) if (!o.vaulted) ++ativas;

        QStringList partes;
        if (ativas == 0) {
            partes << QString("<br><span style='color:#f59e0b'><b>Vaulted.</b> As %1 relíquias "
                              "que o largam estão todas fora de circulação — só se obtém "
                              "por troca.</span>").arg(origens.size());
        } else {
            partes << QString("<br><b style='color:#10b981'>Farmável agora</b> "
                              "<span style='color:#6b7280'>(%1 ativa%2 em %3 que o largam)</span>")
                          .arg(ativas).arg(ativas == 1 ? "" : "s").arg(origens.size());
        }

        // Mostra no máximo 6: a lista completa de um Forma chega às dezenas.
        QStringList linhas;
        const int limite = std::min<int>(6, origens.size());
        for (int i = 0; i < limite; ++i) {
            const Origem& o = origens[i];
            linhas << QString("<span style='color:%1'>%2</span> "
                              "<span style='color:#6b7280'>(%3%4)</span>")
                          .arg(o.vaulted ? "#6b7280" : "#e2e8f0",
                               o.reliquia, o.raridade,
                               o.vaulted ? ", vaulted" : "");
        }
        partes << linhas.join(" &nbsp;·&nbsp; ");
        if (origens.size() > limite) {
            partes << QString("<span style='color:#6b7280'>... e mais %1</span>")
            .arg(origens.size() - limite);
        }
        return partes.join("<br>");
    }

    void mostrarSugestao(const QString& slug, const Precos& p) {
        const ItemApi* item = api->itemDeSlug(slug);
        const QString nome = item ? item->name : slug;
        const QString etiquetaRank = (p.rankUsado >= 0)
                                         ? QString("  <span style='color:#6b7280'>(rank %1%2)</span>")
                                               .arg(p.rankUsado)
                                               .arg(item && item->maxRank > 0 ? QString(" de %1").arg(item->maxRank) : QString())
                                         : QString();

        if (!p.valido) {
            painelPrecos->setText(QString("<b>%1</b><br><span style='color:#ef4444'>Não foi possível obter as ordens.</span>").arg(nome));
            return;
        }
        if (p.menorVenda == 0 && p.maiorCompra == 0) {
            painelPrecos->setText(QString("<b>%1</b><br><span style='color:#9ca3af'>"
                                          "Sem ordens ativas neste momento.</span>").arg(nome));
            return;
        }

        QStringList linhas;
        linhas << QString("<b style='color:#f8fafc; font-size:14px'>%1</b>%2").arg(nome, etiquetaRank);

        if (p.menorVenda > 0) linhas << QString("Venda mais barata: <b style='color:#f8fafc'>%1 p</b> <span style='color:#6b7280'>(%2 em stock, %3 no top 5)</span>").arg(p.menorVenda).arg(p.qtdMenorVenda).arg(plural(p.vendedores, "vendedor", "vendedores"));
        if (p.maiorCompra > 0) linhas << QString("Compra mais alta: <b style='color:#f8fafc'>%1 p</b> <span style='color:#6b7280'>(%2 no top 5)</span>").arg(p.maiorCompra).arg(plural(p.compradores, "comprador", "compradores"));
        if (p.menorVenda > 0 && p.maiorCompra > 0) linhas << QString("Spread: <b>%1 p</b>").arg(p.menorVenda - p.maiorCompra);
        if (p.menorVenda > 0) linhas << QString("<br><b style='color:#10b981'>Para vender:</b> põe <b>%1 p</b> para igualar, ou <b>%2 p</b> para ficar à frente.").arg(p.menorVenda).arg(std::max(1, p.menorVenda - 1));
        if (p.maiorCompra > 0) linhas << QString("<b style='color:#3b82f6'>Para comprar:</b> a <b>%1 p</b> passas a ser a melhor oferta.").arg(p.maiorCompra + 1);
        if (p.apenasOffline) linhas << "<span style='color:#f59e0b'>Ninguém online com ordens neste item.</span>";

        painelPrecos->setText(linhas.join("<br>"));

        // Pré-preenche o preço sugerido (um abaixo do mais barato online).
        if (p.menorVenda > 0) barraCat.preco->setValue(std::max(1, p.menorVenda - 1));
        else if (p.maiorCompra > 0) barraCat.preco->setValue(p.maiorCompra + 1);
    }

    void desenharMovimento(const QList<Movimento>& lista) {
        tabelaMov->setSortingEnabled(false);
        tabelaMov->setUpdatesEnabled(false);
        tabelaMov->setRowCount(0);

        for (const Movimento& m : lista) {
            const ItemApi* item = api->itemDeSlug(m.slug);
            const int row = tabelaMov->rowCount();
            tabelaMov->insertRow(row);

            QTableWidgetItem* nome = new QTableWidgetItem(item ? item->name : m.slug);
            QFont f = nome->font(); f.setBold(true); nome->setFont(f);
            nome->setToolTip(m.slug);
            tabelaMov->setItem(row, MovItem, nome);

            auto num = [&](int coluna, int valor, int chave, const QString& texto, const QColor& cor) {
                CelulaNumero* c = new CelulaNumero();
                c->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                if (texto.isEmpty()) c->definir(valor); else { c->setData(Qt::DisplayRole, texto); c->definirChave(chave); }
                c->setForeground(QBrush(cor));
                tabelaMov->setItem(row, coluna, c);
            };

            num(MovPreco, m.medianaVenda, m.medianaVenda, {}, QColor("#f8fafc"));
            num(MovOrdens, m.ordens(), m.ordens(), {}, QColor("#cbd5e1"));

            const double proc = m.procura();
            const QColor corProc = proc >= 1.5 ? QColor("#10b981") : proc >= 1.0 ? QColor("#cbd5e1") : QColor("#6b7280");
            num(MovProcura, 0, int(proc * 100), proc >= 99 ? QStringLiteral("só compras") : QString::number(proc, 'f', 2), corProc);
            num(MovPotencial, m.potencial(), m.potencial(), {}, QColor("#38bdf8"));
        }

        tabelaMov->setUpdatesEnabled(true);
        tabelaMov->setSortingEnabled(true);
    }

    // Estilos visuais estáticos
    static QString estiloBotao() { return "QPushButton { background-color:#1f2937; border:1px solid #374151; color:#cbd5e1; border-radius:8px; padding:10px 18px; font-weight:bold; } QPushButton:hover { background-color:#374151; } QPushButton:disabled { color:#4b5563; border-color:#1f2937; }"; }
    static QString estiloPilula() { return "QPushButton { background-color:#1f2937; border:1px solid #374151; color:#9ca3af; border-radius:20px; padding:8px 18px; font-weight:bold; font-size:13px; } QPushButton:checked { background-color:#2563eb; border:1px solid #3b82f6; color:#ffffff; } QPushButton:hover:!checked { background-color:#374151; }"; }
    static QString estiloPainel() { return "background-color:#111827; border:1px solid #1f2937; border-radius:10px; padding:14px; color:#cbd5e1; font-size:13px;"; }

    static void prepararTabela(QTableWidget* t) {
        t->verticalHeader()->setVisible(false);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        t->horizontalHeader()->setSortIndicatorShown(true);
        t->setStyleSheet("QTableWidget { background-color:#0b0f19; border:1px solid #1f2937; gridline-color:transparent; } QHeaderView::section { background-color:#0b0f19; color:#6b7280; font-weight:bold; border:none; border-bottom:1px solid #1f2937; padding:10px; } QTableWidget::item { border-bottom:1px solid #111827; padding:10px; color:#e5e7eb; } QTableWidget::item:selected { background-color:#1e293b; color:#38bdf8; }");
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("wf-market-analytics");
    QCoreApplication::setApplicationName("wf-market-analytics");
    MainWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"