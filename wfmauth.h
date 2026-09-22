// wfmauth.h — sessão do warframe.market e gestão das tuas ordens.
//
// Porquê um ficheiro à parte: o login só existe no fluxo v1 (o /v2/auth/signin
// é reservado à app oficial, exige Firebase AppCheck). O v1 está a ser
// desligado por fases. Quando morrer, é este ficheiro que se substitui — a app
// principal não sabe nada sobre autenticação além de chamar estes métodos.
//
// O estado da conta (online / ingame / invisible) NÃO passa por REST: o site
// usa um WebSocket próprio para isso, e o v2 rejeita o campo num PATCH /me.
// Ver abrirWebSocket() e alterarStatus() no .cpp.
//
// Opcional: compila com -DWFM_KEYCHAIN e liga qt6keychain para guardar o token
// no keychain do sistema entre sessões. Sem isso, o token vive só em memória e
// perde-se ao fechar a app, que é o comportamento mais seguro por omissão.

#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QNetworkRequest>
#include <QPair>
#include <QHash>
#include <QStringList>
#include <QJsonObject>
#include <functional>

class QNetworkAccessManager;
class QTimer;
class QNetworkReply;
class QWebSocket;

struct MinhaOrdem {
    QString id;
    QString itemId;
    QString slug;        // preenchido quando a resposta o traz (v1 traz, v2 não)
    QString tipo;        // "sell" | "buy"
    int platinum = 0;
    int quantity = 0;
    int rank = 0;
    bool visible = true;
};

class WfmAuth : public QObject {
    Q_OBJECT

public:
    explicit WfmAuth(QObject* parent = nullptr);

    bool autenticado() const { return !m_token.isEmpty(); }
    QString utilizador() const { return m_nome; }
    QString estado() const { return m_estado; }   // "ingame" | "online" | "invisible"

    // A password é usada no pedido e nunca é guardada, nem em memória.
    void entrar(const QString& email, const QString& password);
    void sair();

    // Muda o estado da conta. Vai por WebSocket; se a ligação ainda não
    // estiver pronta, o pedido fica guardado e aplica-se assim que estiver.
    void alterarStatus(const QString& estado);

    void criarOrdem(const QString& itemId, const QString& tipo,
                    int platinum, int quantity, int rank, bool visivel);
    void apagarOrdem(const QString& ordemId);
    void carregarMinhasOrdens();

    // Alterações parciais: passa só os campos a mudar
    // ({"platinum":120}, {"visible":false}, {"quantity":3}...).
    void alterarOrdem(const QString& ordemId, const QJsonObject& campos);

    // "Sold" no site: fecha a ordem e regista a venda.
    void marcarVendida(const QString& ordemId);

    // Aplica os mesmos campos a várias ordens, em fila e dentro do limite de
    // pedidos da API. Emite loteProgresso a cada uma.
    void alterarVarias(const QStringList& ordemIds, const QJsonObject& campos);
    void cancelarLote();

    // Tenta restaurar um token guardado (só faz algo com WFM_KEYCHAIN).
    void restaurarSessao();

signals:
    void sessaoIniciada(const QString& nome);
    void sessaoTerminada();
    void erro(const QString& mensagem);
    void ordemCriada(const QString& itemId, int platinum);
    void ordemApagada(const QString& ordemId);
    void ordemAlterada(const QString& ordemId);
    void ordemFechada(const QString& ordemId);
    void loteProgresso(int feitos, int total);
    void loteConcluido(int feitos, int falhas);
    void ordensRecebidas(const QList<MinhaOrdem>& ordens, const QString& endpointUsado);

    void estadoAlterado(const QString& estado);
    void wsLigado(bool ligado);

private:
    using Callback = std::function<void(const QByteArray& corpo)>;

    QNetworkRequest pedido(const QUrl& url, bool comToken) const;

    // Envia um pedido autenticado. Em caso de 401 na primeira tentativa,
    // troca o esquema do cabeçalho (JWT <-> Bearer) e repete uma vez —
    // a documentação do v2 diz "JWT no Authorization" sem fixar o prefixo.
    void enviarAutenticado(const QByteArray& verbo, const QUrl& url,
                           const QByteArray& corpo, Callback aoSucesso,
                           bool jaRepetiu = false);

    // O nome do endpoint das próprias ordens não está documentado publicamente
    // no v2. Tentamos os candidatos conhecidos por ordem até um responder.
    void tentarOrdens(int indiceCandidato);

    // Vários endpoints do v2 não estão documentados publicamente. Em vez de
    // adivinhar um, tentamos os candidatos conhecidos até um responder.
    // O %1 do modelo é o id da ordem. Assim que um candidato responde, fica
    // memorizado em m_caminhoConhecido e os seguintes vão logo ao certo.
    using Tentativa = QPair<QByteArray, QString>;   // verbo, modelo de url
    void tentarCaminhos(const QString& chave, const QList<Tentativa>& modelos,
                        const QString& ordemId, const QByteArray& corpo,
                        std::function<void()> aoSucesso, int indice = 0,
                        bool jaTentouConhecido = false);

    void processarLote();
    QList<MinhaOrdem> extrairOrdens(const QByteArray& corpo) const;

    void guardarToken();
    void limparToken();

    // --- WebSocket do estado ---
    void abrirWebSocket();
    void fecharWebSocket();
    void enviarWs(const QString& rota, const QJsonObject& payload = {});
    void aoReceberWs(const QString& texto);

    QNetworkAccessManager* m_net = nullptr;
    QByteArray m_token;
    QByteArray m_esquema = "JWT";   // prefixo do cabeçalho Authorization
    QString m_nome;
    QString m_deviceId;
    QString m_endpointOrdens;   // memoriza o candidato que funcionou
    QHash<QString, Tentativa> m_caminhoConhecido;

    QStringList m_lote;         // ordens à espera na fila do lote
    QJsonObject m_loteCampos;
    int m_loteTotal = 0;
    int m_loteFalhas = 0;
    QTimer* m_loteTimer = nullptr;
    bool m_loteEmCurso = false;

    QWebSocket* m_ws = nullptr;
    QTimer* m_wsRefresco = nullptr;
    QString m_estado;
    QString m_estadoPedido;     // guardado para reenviar após reconexão
    bool m_wsAutenticado = false;
};