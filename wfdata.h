// wfdata.h — dados da comunidade (api.warframestat.us/wfinfo).
//
// Dois endpoints públicos, sem autenticação, um pedido cada:
//   /wfinfo/filtered_items  tabela de drops de todas as relíquias + flag vaulted
//   /wfinfo/prices          preço médio e volume diário de ~700 peças prime
//
// Não substitui o warframe.market: o custom_avg é uma média suavizada, serve
// para decidir o que farmar, não a quanto anunciar. E só cobre primes —
// arcanas, mods e rivens não estão aqui.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QList>

class QNetworkAccessManager;

// Probabilidades de uma relíquia intacta.
constexpr double kChanceRara     = 0.02;
constexpr double kChanceIncomum  = 0.11;
constexpr double kChanceComum    = 0.2533;

struct Drop {
    QString item;         // nome como vem da tabela de relíquias
    QString raridade;     // "rara" | "incomum" | "comum"
    double chance = 0.0;
};

struct Reliquia {
    QString era;          // Lith, Meso, Neo, Axi, Requiem
    QString nome;         // A2, C7...
    bool vaulted = true;
    QList<Drop> drops;

    QString etiqueta() const { return era + " " + nome; }
};

// Onde é que um item cai.
struct Origem {
    QString reliquia;     // "Axi A2"
    QString raridade;
    bool vaulted = true;
};

struct PrecoRef {
    double media = 0.0;
    int volOntem = 0;
    int volHoje = 0;
};

class WfStatClient : public QObject {
    Q_OBJECT

public:
    explicit WfStatClient(QObject* parent = nullptr);

    void carregar();                       // dispara os dois pedidos

    bool temReliquias() const { return !m_reliquias.isEmpty(); }
    bool temPrecos() const    { return !m_precos.isEmpty(); }

    const QList<Reliquia>& reliquias() const { return m_reliquias; }
    QStringList itensComOrigem() const;
    // Relíquias que largam este item, ordenadas: não-vaulted primeiro.
    QList<Origem> ondeObter(const QString& nomeItem) const;

    PrecoRef preco(const QString& nomeItem) const;

    // Platina esperada por abertura de uma relíquia intacta.
    double valorEsperado(const Reliquia& r) const;

    // O melhor drop da relíquia, por preço.
    Drop melhorDrop(const Reliquia& r) const;

signals:
    void reliquiasCarregadas(int total, int ativas);
    void precosCarregados(int total);
    void erro(const QString& mensagem);

private:
    // Os dois endpoints escrevem o mesmo item de formas diferentes:
    // a tabela de relíquias diz "Nova Prime Neuroptics" e a de preços
    // "Nova Prime Neuroptics Blueprint". Normalizamos os dois para a mesma
    // chave, tirando o sufixo, senão o cruzamento falha em silêncio.
    static QString chave(const QString& nome);

    void pedirReliquias();
    void pedirPrecos();

    QNetworkAccessManager* m_net = nullptr;
    QList<Reliquia> m_reliquias;
    QHash<QString, QList<Origem>> m_origens;   // chave(item) -> relíquias
    QHash<QString, QString> m_nomes;
    QHash<QString, PrecoRef> m_precos;         // chave(item) -> preço
};