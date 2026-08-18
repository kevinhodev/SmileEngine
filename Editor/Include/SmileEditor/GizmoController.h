#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Core/Types.h"

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Logica do gizmo de TRANSFORMACAO — vive no EDITOR (ferramenta de editor, nao runtime). Usa
    // os servicos que a Engine expoe: FDebugDraw (desenha os handles), WorldToScreen/ScreenToRay
    // (camera) e GetScene() (le/escreve o Transform do selecionado). Pivo = a posicao da LUZ
    // selecionada (prioridade; o editor mantem selecao de luz e de renderavel exclusivas), senao
    // o centro da AABB de MUNDO do renderavel selecionado.
    //
    // Por que a AABB e nao Transform.Position: o cooker decompoe o transform GLOBAL do no do FBX
    // (ver DecomposeToEngineTRS), e cena exportada de DCC costuma trazer nos com origem no zero
    // do mundo e vertices ja espalhados. Rodar em volta dessa origem giraria o objeto num arco
    // gigante longe dele. O centro da caixa esta sempre DENTRO do objeto, entao rotacao e escala
    // acontecem em volta dele e a Position e reajustada para o pivo ficar parado — e a semantica
    // "SelectionCenter" da UE. NAO e o "ObjectCenter" da Flax, que apesar do nome usa a
    // Translation do transform (TransformGizmoBase.UpdateGizmoPosition) — ou seja, a origem, que
    // e exatamente o que nao serve aqui.
    //
    // Hit-test = distancia 2D em tela; arraste = ray<->eixo ponto-mais-proximo (mover/escalar) ou
    // mostrador de tela com contagem de voltas (rotacionar — ver OnMouseMove, portado do
    // CDialInteraction do AxisRotateGizmo.cpp da Cry).
    class GizmoController {
    public:
        enum class EAxis : Smile::u32 { None = 0, X, Y, Z };

        // Ferramenta ativa. Espelha os 4 botoes da ViewportToolbar (Q/W/E/R). Select nao desenha
        // handle nenhum e deixa TODO clique cair no picking — os icones de luz continuam
        // visiveis e clicaveis, que e o que faz dele um modo de selecao e nao um "gizmo off".
        enum class EMode : Smile::u32 { Select = 0, Translate, Rotate, Scale };

        void SetEnabled(bool V) { Enabled = V; }
        bool GetEnabled() const { return Enabled; }
        bool IsDragging() const { return Dragging; }

        // Trocar de ferramenta no meio de um arraste e ignorado: o estado do drag e por modo, e
        // meio gesto interpretado pelo modo errado escreveria um transform sem sentido. O hover
        // e limpo junto — ele aponta pra um handle do modo ANTERIOR, e sem isso o novo nasceria
        // com um eixo aceso que o cursor nao esta tocando.
        void  SetMode(EMode M) { if (!Dragging) { Mode = M; Hovered = EAxis::None; } }

        // Base dos eixos do gizmo. Uma escolha SO, global, como em UE e Flax — a Cry guarda uma
        // por modo de edicao (m_coordSystemForEditMode em LevelEditorSharedState), o que e melhor
        // de usar mas pede um botao que muda de rotulo quando a ferramenta troca; fica para o dia
        // em que houver um terceiro espaco (a Cry tem Parent, View e UserDefined alem destes).
        enum class ESpace : Smile::u32 { World = 0, Local };

        // Por que o espaco escolhido esta sendo ignorado. A UI le isto para DIZER o motivo em vez
        // de o botao parecer quebrado — e para nao ter de reconstruir a regra por conta propria.
        enum class ESpaceRestriction : Smile::u32 { None = 0, ScaleIsLocal, LightHasNoLocal };

        // A REGRA, num lugar so. Todo mundo que precisa saber em que base o gizmo opera passa por
        // aqui — o desenho, o hit-test, o botao da toolbar e o atalho. Ter isto duplicado foi o
        // que produziu os dois defeitos desta rodada: a luz caia em mundo por um `if` solto no
        // AxisBasis enquanto o botao seguia mostrando "Local", e o Ctrl+` escrevia no espaco
        // durante o modo Escalar enquanto o botao aparecia travado.
        //
        //  - LUZ nao tem base local utilizavel: a pontual e isotropica e o spot so tem uma
        //    DIRECAO — falta o roll, entao qualquer base montada dela teria um eixo escolhido a
        //    esmo e o gizmo giraria sozinho quando a direcao passasse perto do polo. Vem primeiro
        //    porque e a restricao mais forte: nao e uma escolha de conveniencia, e ausencia de
        //    base.
        //  - ESCALA e sempre local mesmo com Global escolhido: escalar num eixo de mundo um
        //    objeto rotacionado produz CISALHAMENTO, que um TRS nao representa. UE e Flax fazem
        //    igual (FEditorModeTools::GetCoordSystem forca COORD_Local em WM_Scale; o
        //    UpdateMatrices da Flax so zera a orientacao em `World && mode != Scale`).
        ESpace SpaceFor(bool IsLight) const {
            if (IsLight)              return ESpace::World;
            if (Mode == EMode::Scale) return ESpace::Local;
            return Space;
        }
        ESpaceRestriction RestrictionFor(bool IsLight) const {
            if (IsLight)              return ESpaceRestriction::LightHasNoLocal;
            if (Mode == EMode::Scale) return ESpaceRestriction::ScaleIsLocal;
            return ESpaceRestriction::None;
        }

        // BRUTO: o que o usuario escolheu. E o que o botao mostra quando nada esta restringindo.
        // A separacao bruto/efetivo e a mesma do bGetRawValue da UE, e existe para o botao NAO
        // MENTIR — mas so metade dela: o outro lado e o SetSpace recusar quando ha restricao,
        // senao o atalho muda um estado que a UI mostra como travado e a mudanca aparece depois,
        // sozinha, quando o usuario sai do modo.
        ESpace GetSpace() const { return Space; }
        void   SetSpace(ESpace S) {
            // Guarda AQUI e nao no atalho: botao e teclado tem de obedecer a mesma regra por
            // construcao. Duplicar a condicao no chamador e exatamente o que deixou os dois
            // divergirem.
            if (Dragging || IsSpaceForced()) return;
            Space   = S;
            Hovered = EAxis::None; // os eixos giram; o que estava sob o cursor deixa de estar
        }

        // Versoes sem argumento para a UI, sobre a selecao ATUAL (ver SelectionIsLight).
        ESpace            EffectiveSpace() const { return SpaceFor(SelectionIsLight); }
        ESpaceRestriction Restriction()    const { return RestrictionFor(SelectionIsLight); }
        bool              IsSpaceForced()  const { return Restriction() != ESpaceRestriction::None; }
        EMode GetMode() const { return Mode; }

        // --- SNAPPING -----------------------------------------------------------------------
        // Semantica ABSOLUTA, como UE e Cry: o RESULTADO cai em multiplos do passo, e nao o
        // movimento. E o que faz dois objetos ficarem alinhados entre si, que e o ponto.
        //   UE:  `Point = (Point - GridBase).GridSnap(GetGridSize()) + GridBase`
        //        (FEditorViewportSnapping::SnapPointToGrid)
        //   Cry: `floor(v / step + 0.5) * step` (SSnappingPreferences::SnapLength/Angle/Scale)
        // A Flax faz o contrario por padrao — acumula o DELTA em _translationScaleSnapDelta e so
        // libera multiplos inteiros — e oferece o absoluto como AbsoluteSnapEnabled. O relativo
        // preserva o desalinhamento original do objeto, que e util e nao e o que se pede de um
        // botao chamado "snap".
        //
        // Aqui NAO existe acumulador, e nao por simplificacao: cada gesto ja recomputa do estado
        // do PRESS (ver ApplyRotate), entao o valor final e funcao pura do cursor e arredondar na
        // saida basta. O acumulador da Flax existe porque la o delta e incremental.
        struct FSnap {
            bool  TranslateOn = false;
            bool  RotateOn    = false;
            bool  ScaleOn     = false;
            float TranslateM  = 0.10f;   // metros
            float RotateDeg   = 15.0f;
            float ScaleStep   = 0.10f;   // fracao (0.10 = 10%)
        };

        const FSnap& Snap() const { return SnapCfg; }
        void SetSnapTranslateOn(bool V) { SnapCfg.TranslateOn = V; }
        void SetSnapRotateOn(bool V)    { SnapCfg.RotateOn    = V; }
        void SetSnapScaleOn(bool V)     { SnapCfg.ScaleOn     = V; }
        void SetSnapTranslateM(float V) { if (V > 0.0f) SnapCfg.TranslateM = V; }
        void SetSnapRotateDeg(float V)  { if (V > 0.0f) SnapCfg.RotateDeg  = V; }
        void SetSnapScaleStep(float V)  { if (V > 0.0f) SnapCfg.ScaleStep  = V; }

        // Ctrl segurado INVERTE o snap do modo atual. A Flax so FORCA ligado
        // (`UseSnapping => Root.GetKey(KeyboardKeys.Control)`, EditorGizmoViewport.cs) e a UE nao
        // tem modificador nenhum. Inverter e a escolha daqui: com um botao de liga/desliga na
        // toolbar, "forcar ligado" nao faz nada quando ele ja esta ligado, enquanto inverter
        // resolve os dois sentidos com a mesma tecla — que e o caso que realmente aparece
        // (encostar um objeto fora da grade sem ter de desligar o snap e religar depois).
        void SetSnapInverted(bool V) { SnapInverted = V; }

        // O snap vale AGORA, para o modo atual? Ja com a inversao do Ctrl aplicada.
        bool SnapActive() const {
            const bool On = Mode == EMode::Translate ? SnapCfg.TranslateOn
                          : Mode == EMode::Rotate    ? SnapCfg.RotateOn
                          : Mode == EMode::Scale     ? SnapCfg.ScaleOn
                                                     : false;
            return On != SnapInverted;
        }

        // Chamado a cada frame APOS UpdateCamera e ANTES de RenderFrame: submete os handles do
        // modo atual ao FDebugDraw da Engine (no-op sem selecao) + os icones billboard das luzes
        // (um em cada; toco de direcao no spot selecionado).
        void Submit(Smile::Renderer& R);

        // Picking do icone de luz — a luz nao entra no ID-buffer do FObjectPicker, entao o teste
        // e analitico em tela. Mora aqui (e nao no ViewportWidget) porque o raio SAI da mesma
        // metrica que desenha o billboard: acompanha FOV, resolucao e qualquer mudanca futura de
        // escala sem um segundo lugar pra esquecer. -1 = nenhum. Sobreposicao ganha o ULTIMO
        // submetido, que e o que aparece por cima (alpha-blend na ordem do vetor, sem depth) —
        // NAO e o mais perto da camera, e NAO e oclusao: icone e sempre visivel por design.
        int PickLightIcon(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const;

        // Eventos de mouse (coords em pixels do backbuffer = coords logicas do widget):
        // press tenta pegar um handle -> retorna true se comecou um arraste (editor NAO faz picking).
        bool OnMousePress(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void OnMouseMove(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);   // drag ou hover
        // Precisa do Renderer: e aqui que o objeto sai do conjunto dinamico do CSM e o mapa
        // estatico e invalidado, uma vez, com ele ja no lugar final.
        //
        // Devolve true se o gesto CHEGOU A ESCREVER o transform de um renderavel — e o que o
        // ViewportWidget converte em "a camada autorada (.smap) esta suja". Nao devolve true
        // para luz: aquelas moram no <cena>.lights.json e o LightsBridge::Refresh ja as marca.
        // Clique sem deslocamento devolve false: sujar o documento por um clique que nao mudou
        // nada faria o editor perguntar "salvar?" ao fechar sem haver o que salvar.
        bool OnMouseRelease(Smile::Renderer& R);

    private:
        // Pivo (posicao da luz selecionada, senao centro da AABB do renderavel selecionado) +
        // escala constante-em-tela. -1 em OutIdx = sem selecao; OutIsLight diz qual lista.
        bool   GetPivot(Smile::Renderer& R, Smile::Vec3& OutPivot, int& OutIdx,
                        bool& OutIsLight) const;
        float  ScaleFor(Smile::Renderer& R, const Smile::Vec3& Pivot) const;

        // Base de eixos dos handles, em MUNDO. Mover e rotacionar usam os eixos do mundo (o
        // seletor Global/Local da toolbar ainda nao existe); ESCALA usa sempre os eixos LOCAIS do
        // objeto, e nao por falta do seletor: escalar num eixo de mundo um objeto rotacionado
        // produz um CISALHAMENTO, que um TRS (posicao/euler/escala) nao sabe representar. A Flax
        // faz igual, e da pra ver no codigo: o UpdateMatrices dela so zera a orientacao do gizmo
        // no modo World `&& _activeMode != Mode.Scale` — a escala fica local mesmo em Global.
        void   AxisBasis(Smile::Renderer& R, bool IsLight, int Idx, Smile::Vec3 Out[3]) const;

        // Hit-test dos handles retos (mover/escalar): distancia 2D aos 3 eixos.
        EAxis  HitTestAxes(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const;
        // Hit-test dos aneis (rotacionar): distancia 2D a polilinha de cada anel, so na METADE
        // VOLTADA PRA CAMERA — o usuario mira no que ve, e o outro lado do anel passa perto do
        // cursor em vista quase de topo. OutRingPoint recebe o ponto do anel (em mundo) mais
        // proximo do cursor: e dele que sai a direcao inicial do mostrador e o sinal do giro.
        EAxis  HitTestRings(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y,
                            Smile::Vec3* OutRingPoint) const;
        EAxis  HitTest(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const; // despacha por modo

        // t do ponto do eixo (Pivot + t*AxisDir) mais proximo do raio (AxisDir e Dir unitarios).
        float  AxisParam(const Smile::Vec3& Origin, const Smile::Vec3& Dir,
                         const Smile::Vec3& AxisDir, const Smile::Vec3& Pivot) const;

        void   SubmitTranslate(Smile::Renderer& R, const Smile::Vec3& Pivot, float Scale,
                               const Smile::Vec3 Axes[3]) const;
        void   SubmitRotate(Smile::Renderer& R, const Smile::Vec3& Pivot, float Scale,
                            const Smile::Vec3 Axes[3]) const;
        void   SubmitScale(Smile::Renderer& R, const Smile::Vec3& Pivot, float Scale,
                           const Smile::Vec3 Axes[3]) const;
        void   SubmitLightShapes(Smile::Renderer& R) const; // icone billboard de cada luz
        // Ha o que girar no selecionado? Luz PONTUAL e isotropica — anel nenhum aparece nela.
        bool   HasRotationHandles(Smile::Renderer& R, bool IsLight, int Idx) const;
        // Meia-extensao do billboard do icone (mundo). FONTE UNICA: quem desenha e quem faz o
        // hit-test passam por aqui — as duas coisas nao podem divergir.
        float  IconHalfSizeFor(Smile::Renderer& R, const Smile::Vec3& Position) const;

        // Edicao de LUZ: derruba os historicos sem granularidade espacial (reservoir, reflexoes,
        // ReGIR, cache). Complementa o NotifyGIRegionChanged, que so cobre o atlas do DDGI.
        void   MarkLightEnergyChanged(Smile::Renderer& R) const;

        // Aplicado por mover/rotacionar/escalar depois de escrever o Transform: refaz a caixa de
        // mundo, avisa a TLAS e invalida as sondas do volume de ONDE SAIU e de ONDE CHEGOU.
        void   CommitRenderableEdit(Smile::Renderer& R, int Idx,
                                    const Smile::Vec3& OldMin, const Smile::Vec3& OldMax) const;

        // Arma o mostrador de rotacao no press (direcao de referencia em tela + sinal do giro).
        // false = gesto degenerado (cursor no pivo, pivo fora da tela) e o arraste nao comeca.
        bool   BeginDial(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y,
                         const Smile::Vec3& RingPoint);
        // Um por modo, chamados pelo OnMouseMove com o arraste ja em curso.
        void   ApplyTranslate(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void   ApplyRotate(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void   ApplyScale(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);

        bool        Enabled = true;
        EMode       Mode    = EMode::Translate;
        ESpace      Space   = ESpace::World;  // escolha BRUTA; ver EffectiveSpace
        // A selecao atual e uma LUZ? Cache atualizado pelo Submit (que ja resolve a selecao por
        // frame) para as consultas SEM argumento da UI poderem responder sem o Renderer. Os
        // caminhos que JA tem o IsLight em maos passam ele pelo SpaceFor: sao os de evento de
        // mouse, que podem rodar com uma selecao mais nova que o ultimo frame.
        bool        SelectionIsLight = false;
        FSnap       SnapCfg{};
        bool        SnapInverted = false;   // Ctrl segurado; ver SetSnapInverted
        EAxis       Hovered = EAxis::None;
        EAxis       Active  = EAxis::None;
        bool        Dragging = false;
        int         DragIdx = -1;            // indice do renderavel/luz sendo arrastado
        bool        DragIsLight = false;     // DragIdx aponta pra Scene.Lights() em vez de Renderables()
        Smile::Vec3 DragStartPos{};          // posicao no inicio do arraste
        Smile::Vec3 DragStartPivot{};        // pivo FIXO durante o arraste (linha do eixo)
        Smile::Vec3 DragAxisWorld{};         // eixo pego, em mundo (local ja resolvido)
        // Base CONGELADA no press, pelo mesmo motivo do DragStartPivot: no espaco Local o objeto
        // gira durante o gesto, e recomputar a base por frame faria os aneis rodarem junto com
        // ele enquanto a matematica do arraste segue no eixo pego. Congelar vale para os tres
        // modos — nos outros dois a base nem muda, entao nao custa nada e evita um caso especial.
        Smile::Vec3 DragAxes[3]{};
        float       DragStartT = 0.0f;       // offset inicial ao longo do eixo (mover/escalar)

        // --- Rotacionar. O angulo e recomputado do ESTADO INICIAL a cada evento (nunca somado
        // incrementalmente): arraste longo com acumulo incremental deriva, e voltar o mouse ao
        // ponto de partida tem de devolver o objeto exatamente a rotacao original.
        Smile::Vec3 DragStartEuler{};        // rotacao do renderavel no inicio
        Smile::Vec3 DragStartSpotDir{};      // direcao do spot no inicio (luz)
        Smile::Vec3 DragStartRingDir{};      // pivo -> ponto do anel que foi pego (mundo, unitario)
        Smile::Vec2 DialRef{};               // direcao de tela pivo->cursor no press (unitaria)
        float       DialAngle = 0.0f;        // ultimo angulo local no mostrador, em (-pi, pi]
        int         DialRevolutions = 0;     // voltas completas acumuladas (Cry)
        float       DialSign = 1.0f;         // +1 se girar positivo AUMENTA o angulo de tela
        float       DragAngle = 0.0f;        // angulo total do gesto (feedback do desenho)

        // --- Escalar.
        Smile::Vec3 DragStartScale{ 1.0f, 1.0f, 1.0f };
        float       DragGizmoLen = 1.0f;     // comprimento do handle no press: arrastar um
                                             // comprimento inteiro = dobrar (independe de resolucao)
    };
}
