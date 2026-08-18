#pragma once

#include <QString>

class QJsonObject;

namespace SmileEditor {
    // Grava um sidecar JSON de forma ATOMICA e devolve se o arquivo em disco esta completo.
    //
    // Existe porque a receita estava COPIADA em cinco lugares e quatro deles estavam errados: o
    // QFile|Truncate apaga o conteudo ANTES de escrever, e o retorno do write() era ignorado.
    // Em disco cheio ou short write, o save devolvia true, limpava o dirty e deixava o sidecar
    // TRUNCADO — o pior desfecho possivel, porque o original ja tinha sido destruido e a proxima
    // abertura acharia um JSON corrompido.
    //
    // O QSaveFile escreve num temporario e so o troca pelo definitivo no commit(); sair por
    // qualquer caminho sem chamar commit() descarta o temporario e deixa o arquivo antigo
    // intacto. Ou o antigo continua inteiro, ou o novo esta completo — nunca um meio-termo.
    //
    // ⚠️ Essa garantia depende de o directWriteFallback ficar DESLIGADO, que e o default e que
    // este helper deliberadamente nao mexe. Com ele desligado, diretorio sem permissao para criar
    // o temporario faz o open() FALHAR — e o arquivo existente fica intacto, que e o desfecho
    // certo aqui. Ligar setDirectWriteFallback(true) trocaria isso por sobrescrever o arquivo
    // direto, sem atomicidade e sem cancelWriting(): os proprios docs do Qt separam os dois casos
    // e dizem que arquivo interno de aplicacao (configuracao, dados) deve "keep the default
    // setting which ensures atomicity". Sidecar e exatamente isso — nao ligar.
    // (Conferido em C:/Qt/Docs/Qt-6.10.2/qtcore/qsavefile.html, Qt 6.10.2.)
    //
    // Indentado sempre: estes sidecars entram no repositorio e sao lidos em diff. Compacto
    // economizaria bytes que nao faltam e esconderia a unica coisa que se quer ver num diff
    // destes, que e O QUE mudou.
    //
    // Loga o motivo preciso da falha (com o caminho) — o chamador so precisa propagar o false.
    bool WriteJsonSidecar(const QString& Path, const QJsonObject& Root, const char* Label);
}
