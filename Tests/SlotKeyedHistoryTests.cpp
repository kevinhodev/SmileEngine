// Historico por slot de residencia (Smile/Scene/SlotKeyedHistory.h).
//
// Existe porque o teste anterior desta migracao verificava a estabilidade dos HANDLES e nao o
// historico: reverter qualquer um dos dois historicos para indice posicional, ou remover a
// comparacao de generation, nao mudaria nada nele. Aqui os valores guardados sao conferidos, e nao
// apenas a identidade.
//
// Duas instancias usam este tipo com ciclos de vida independentes -- o PrevMVP do raster e os
// transforms do motion vector de RT --, e o teste guarda MATRIZES DISTINTAS por frame justamente
// para que trocar uma pela outra apareca.

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Smile/Math/Mat44.h"
#include "Smile/Scene/CapturedIdentity.h"
#include "Smile/Scene/SlotKeyedHistory.h"

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    using namespace Smile;

    // Matriz reconhecivel por objeto e por frame: a translacao carrega os dois, entao trocar o
    // historico de A pelo de B, ou o do frame N pelo do N-1, muda um numero que o teste le.
    Mat44 Marca(float Objeto, float Frame) {
        Mat44 M = Mat44::Identity();
        M.M[3][0] = Objeto;
        M.M[3][1] = Frame;
        return M;
    }

    bool Igual(const Mat44& A, const Mat44& B) {
        return A.M[3][0] == B.M[3][0] && A.M[3][1] == B.M[3][1];
    }
}

int main() {
    std::cout << "Smile.SlotKeyedHistory\n";

    // 1. Sem historico, devolve o fallback -- e nao o valor de outro slot.
    {
        TSlotKeyedHistory<Mat44> H;
        H.EnsureCapacity(4);
        const FGpuSceneHandle A{ 0, 1 };
        const Mat44 Atual = Marca(10, 2);
        Check(Igual(H.PreviousOr(A, Atual), Atual), "sem historico devolve o fallback");

        H.Store(FGpuSceneHandle{ 1, 1 }, Marca(99, 1));
        Check(Igual(H.PreviousOr(A, Atual), Atual),
              "historico de OUTRO slot nao vaza para este");
    }

    // 2. Round-trip por frame: cada objeto recupera o SEU valor do frame anterior.
    {
        TSlotKeyedHistory<Mat44> H;
        H.EnsureCapacity(3);
        const FGpuSceneHandle A{ 0, 1 }, B{ 1, 1 }, C{ 2, 1 };

        H.Store(A, Marca(1, 1));
        H.Store(B, Marca(2, 1));
        H.Store(C, Marca(3, 1));

        Check(Igual(H.PreviousOr(A, Marca(0, 0)), Marca(1, 1)), "A recupera o proprio valor");
        Check(Igual(H.PreviousOr(B, Marca(0, 0)), Marca(2, 1)), "B tambem");
        Check(Igual(H.PreviousOr(C, Marca(0, 0)), Marca(3, 1)), "C tambem");

        // Frame seguinte sobrescreve: o historico e do frame ANTERIOR, nao acumulado.
        H.Store(B, Marca(2, 2));
        Check(Igual(H.PreviousOr(B, Marca(0, 0)), Marca(2, 2)), "B guarda o valor mais recente");
        Check(Igual(H.PreviousOr(A, Marca(0, 0)), Marca(1, 1)), "e A nao foi afetado");
    }

    // 3. O caso que o enderecamento por POSICAO errava: o sobrevivente nao herda o valor do
    //    ex-vizinho. Com indice de lista, remover A faria B ler o historico de A.
    {
        TSlotKeyedHistory<Mat44> H;
        H.EnsureCapacity(3);
        const FGpuSceneHandle A{ 0, 1 }, B{ 1, 1 }, C{ 2, 1 };
        H.Store(A, Marca(1, 1));
        H.Store(B, Marca(2, 1));
        H.Store(C, Marca(3, 1));

        // "A foi removido": ninguem mexe no historico, e B e C continuam achando o proprio.
        Check(Igual(H.PreviousOr(B, Marca(0, 0)), Marca(2, 1)),
              "depois de o vizinho sair, B ainda le o proprio historico");
        Check(Igual(H.PreviousOr(C, Marca(0, 0)), Marca(3, 1)), "e C tambem");
    }

    // 4. Slot RECICLADO recebe o fallback, e nao o valor do ocupante anterior. E o unico caso que
    //    a comparacao de generation pega, e sem ela apareceria como smear vindo do nada.
    {
        TSlotKeyedHistory<Mat44> H;
        H.EnsureCapacity(2);
        const FGpuSceneHandle Velho{ 0, 1 };
        H.Store(Velho, Marca(1, 1));

        const FGpuSceneHandle Novo{ 0, 2 }; // mesmo slot, generation seguinte
        const Mat44 Atual = Marca(7, 5);
        Check(Igual(H.PreviousOr(Novo, Atual), Atual),
              "o novo ocupante do slot recebe o fallback, nao o valor do anterior");

        // E o valor do novo nao ressuscita o do velho.
        H.Store(Novo, Marca(7, 5));
        Check(Igual(H.PreviousOr(Velho, Atual), Atual),
              "o handle velho tambem nao le o valor do novo");
    }

    // 5. Invalidacao explicita descarta tudo -- corte de camera, troca de cena, reset.
    {
        TSlotKeyedHistory<Mat44> H;
        H.EnsureCapacity(2);
        const FGpuSceneHandle A{ 0, 1 };
        H.Store(A, Marca(1, 1));
        Check(Igual(H.PreviousOr(A, Marca(0, 0)), Marca(1, 1)), "guardado antes da invalidacao");

        H.Invalidate();
        Check(H.Empty(), "invalidar esvazia");
        const Mat44 Atual = Marca(4, 4);
        Check(Igual(H.PreviousOr(A, Atual), Atual), "depois de invalidar, devolve o fallback");

        // E volta a funcionar depois de recrescer.
        H.EnsureCapacity(2);
        H.Store(A, Marca(1, 3));
        Check(Igual(H.PreviousOr(A, Atual), Marca(1, 3)), "e volta a guardar normalmente");
    }

    // 6. Slot fora da capacidade nao escreve nem le fora do vetor.
    {
        TSlotKeyedHistory<Mat44> H;
        H.EnsureCapacity(2);
        const FGpuSceneHandle Longe{ 50, 1 };
        const Mat44 Atual = Marca(8, 8);
        H.Store(Longe, Marca(9, 9)); // nao pode estourar
        Check(Igual(H.PreviousOr(Longe, Atual), Atual), "slot fora da capacidade devolve fallback");
        Check(H.Capacity() == 2, "e nao cresce por acidente");

        // EnsureCapacity nunca ENCOLHE: encolher jogaria fora o historico de quem esta acima.
        H.Store(FGpuSceneHandle{ 1, 1 }, Marca(1, 1));
        H.EnsureCapacity(1);
        Check(H.Capacity() == 2, "EnsureCapacity nao encolhe");
        Check(Igual(H.PreviousOr(FGpuSceneHandle{ 1, 1 }, Atual), Marca(1, 1)),
              "e o historico acima do novo pedido sobrevive");
    }

    // 7. Handle invalido nunca casa com nada -- nem o default, nem o ZERADO.
    //
    // Os dois casos sao diferentes e o teste anterior so cobria o primeiro: o default tem
    // Slot = kInvalidSlot, que ja cai fora da capacidade, entao ele nao exercitava a generation.
    // O zerado ({0,0}, o que um memset produz) tem slot VALIDO, e por ele a checagem de generation
    // e a unica coisa entre um handle sem dono e o historico de quem ocupa aquele slot.
    {
        TSlotKeyedHistory<Mat44> H;
        H.EnsureCapacity(4);
        const Mat44 Atual = Marca(6, 6);

        Check(Igual(H.PreviousOr(FGpuSceneHandle{}, Atual), Atual),
              "handle default devolve o fallback");
        H.Store(FGpuSceneHandle{}, Marca(5, 5));
        Check(Igual(H.PreviousOr(FGpuSceneHandle{}, Atual), Atual),
              "e guardar por ele nao cria historico");

        // Agora o zerado, com um ocupante VALIDO no mesmo slot.
        const FGpuSceneHandle Vivo{ 0, 1 };
        FGpuSceneHandle Zerado{};
        Zerado.Slot = 0;
        Zerado.Generation = 0;

        H.Store(Vivo, Marca(7, 1));
        Check(Igual(H.PreviousOr(Vivo, Atual), Marca(7, 1)), "o ocupante valido guardou");

        Check(Igual(H.PreviousOr(Zerado, Atual), Atual),
              "o handle zerado nao le o historico do ocupante do slot 0");

        H.Store(Zerado, Marca(123, 123));
        Check(Igual(H.PreviousOr(Zerado, Atual), Atual),
              "guardar pelo zerado nao cria historico");
        Check(Igual(H.PreviousOr(Vivo, Atual), Marca(7, 1)),
              "e, sobretudo, nao APAGA o historico valido que estava la");
    }

    // 8. Round-trip do id de instancia pelo canal FLOAT da superficie temporal.
    //
    // O shader grava (id + 1) num float32 e o decoder le de volta. O decoder tinha um "+ 0.5f"
    // que parecia arredondamento defensivo e CORTAVA A FAIXA PELA METADE: a soma nao e
    // representavel a partir de 2^23, entao o id 8388608 voltava como 8388609 -- o objeto
    // vizinho, sem erro nenhum.
    //
    // Este teste espelha a conta do shader em float32 (float, nao double, senao ele nao mede o
    // que o shader faz) e cobre as duas bordas.
    {
        auto RoundTrip = [](u32 Id) {
            const float Encoded = static_cast<float>(Id + 1u);
            const u32   Value   = static_cast<u32>(Encoded);
            return Value > 0u ? Value - 1u : 0u;
        };
        // Com o "+0.5f" antigo, para provar que o teste distingue as duas versoes.
        auto RoundTripComMeio = [](u32 Id) {
            const float Encoded = static_cast<float>(Id + 1u);
            const u32   Value   = static_cast<u32>(Encoded + 0.5f);
            return Value > 0u ? Value - 1u : 0u;
        };

        for (u32 Id : { 0u, 1u, 1000u, 8388606u, 8388607u, 8388608u, 8388609u,
                        kMaxTemporalInstanceId - 1u, kMaxTemporalInstanceId }) {
            Check(RoundTrip(Id) == Id, "o id volta igual pelo canal float");
            if (RoundTrip(Id) != Id)
                std::cerr << "        id " << Id << " voltou " << RoundTrip(Id) << "\n";
        }

        // E a versao antiga erra EXATAMENTE em 2^23 -- se isto parar de valer, o decoder mudou.
        Check(RoundTripComMeio(8388608u) != 8388608u,
              "a versao com +0.5f errava em 2^23 (e por isso ela saiu)");
        Check(RoundTripComMeio(1000u) == 1000u, "e acertava abaixo da borda");
    }

    if (Failures > 0) {
        std::cerr << "  " << Failures << " falha(s)\n";
        return 1;
    }
    std::cout << "  OK\n";
    return 0;
}
