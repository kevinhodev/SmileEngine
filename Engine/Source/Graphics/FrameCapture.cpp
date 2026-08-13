#include "Smile/Graphics/FrameCapture.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
// O commit da build vai no manifesto. O VersionInfo.h e carimbado a cada BUILD justamente por
// causa deste campo — ver o alvo SmileVersionStamp no Engine/CMakeLists.txt.
#include "Smile/Core/VersionInfo.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;

namespace Smile {
    namespace {

        // Diretorio do executavel. A alternativa seria o cwd, que num app Qt depende de como o
        // processo foi lancado — e uma captura que aparece em lugar diferente conforme se abriu
        // pelo VS ou pelo Explorer e uma captura que se perde.
        fs::path ExecutableDir() {
            wchar_t Buffer[MAX_PATH]{};
            const DWORD Written = GetModuleFileNameW(nullptr, Buffer, MAX_PATH);
            if (Written == 0 || Written >= MAX_PATH) return fs::current_path();
            return fs::path(Buffer).parent_path();
        }

        std::string TimeStamp() {
            const auto Now  = std::chrono::system_clock::now();
            const auto Time = std::chrono::system_clock::to_time_t(Now);
            std::tm Local{};
            localtime_s(&Local, &Time);
            char Text[32]{};
            std::snprintf(Text, sizeof(Text), "%04d%02d%02d-%02d%02d%02d",
                          Local.tm_year + 1900, Local.tm_mon + 1, Local.tm_mday,
                          Local.tm_hour, Local.tm_min, Local.tm_sec);
            return Text;
        }

        std::string IsoTime() {
            const auto Now  = std::chrono::system_clock::now();
            const auto Time = std::chrono::system_clock::to_time_t(Now);
            std::tm Local{};
            localtime_s(&Local, &Time);
            char Text[32]{};
            std::snprintf(Text, sizeof(Text), "%04d-%02d-%02dT%02d:%02d:%02d",
                          Local.tm_year + 1900, Local.tm_mon + 1, Local.tm_mday,
                          Local.tm_hour, Local.tm_min, Local.tm_sec);
            return Text;
        }

        // So o que sobrevive a um nome de arquivo. Vem de rotulo digitado e de nome de cena.
        std::string Sanitize(std::string Text) {
            for (char& C : Text) {
                const bool Ok = (C >= '0' && C <= '9') || (C >= 'a' && C <= 'z') ||
                                (C >= 'A' && C <= 'Z') || C == '-' || C == '_' || C == '.';
                if (!Ok) C = '-';
            }
            while (!Text.empty() && Text.back() == '-') Text.pop_back();
            return Text;
        }

        // Etiqueta curta do A/B no NOME do arquivo. O manifesto tem o estado inteiro; isto existe
        // para uma pasta com trinta PNGs ser legivel sem abrir trinta manifestos.
        std::string ConfigTag(const FCaptureState& S) {
            std::string Tag;
            auto Add = [&Tag](const char* Piece) {
                if (!Tag.empty()) Tag += '-';
                Tag += Piece;
            };
            if (S.UseGI)       Add("gi");
            if (S.DDGIReady)   Add("ddgi");
            if (S.ReSTIRGI)    Add("rgi");
            if (S.ReSTIRDI)    Add("rdi");
            if (S.ReGIR)       Add("regir");
            if (S.Reflections) Add("refl");
            // O S da instrumentacao entra no NOME, e nao so no manifesto: os dois regimes produzem
            // ocupacao e imagem diferentes (atomicos mudam o escalonamento das waves), entao duas
            // capturas indistinguiveis na pasta seriam um convite a compara-las.
            if (S.CacheUpdate || S.CacheQuery) {
                std::string Rc = "rc";
                if (S.CacheUpdate) Rc += 'U';
                if (S.CacheQuery)  Rc += 'Q';
                if (S.CacheStats)  Rc += 'S';
                if (S.CacheStatsDetail) Rc += 'd'; // minusculo: qualifica o S, nao e outro eixo
                // Piso de confianca, junto do Q porque e a CONSULTA que ele governa — e sai do
                // nome quando ninguem consultou, onde ele nao descreveria nada. `C1` e o regime
                // anterior a Fase 4 (uma amostra ja encerrava um caminho), que continua alcancavel
                // pelo knob e e o outro braco do A/B.
                if (S.CacheMinSamples > 0u) {
                    Rc += 'C'; Rc += std::to_string(S.CacheMinSamples);
                }
                // O PRODUTOR entra no nome pela mesma razao do S: duas capturas indistinguiveis na
                // pasta sao um convite a compara-las, e produtor legado x dedicado nao sao duas
                // amostras da mesma configuracao — sao dois estimadores. Com o dedicado vem junto
                // o que muda a convergencia dele: a fracao (em celulas do tile, 1..25) e o T do
                // terminal no cache anterior. Sem dedicado nenhum dos dois significa coisa alguma,
                // entao nao aparecem.
                if (S.CacheDedicatedUpdate) {
                    Rc += 'D';
                    Rc += std::to_string(
                        static_cast<int>(S.CacheUpdateFraction * 25.0f + 0.5f));
                    // Vertices e piso de roughness: os dois eixos de medicao do multi-bounce. O
                    // piso vai em porcento inteiro para caber num nome de arquivo sem virgula.
                    Rc += 'V'; Rc += std::to_string(S.CacheMaxVertices);
                    Rc += 'R'; Rc += std::to_string(
                        static_cast<int>(S.CacheMinRoughness * 100.0f + 0.5f));
                    if (S.CacheUsePrevTerminal) Rc += 'T';
                }
                Add(Rc.c_str());
            }
            // A politica de backface muda o que o raio ENXERGA — no gather e no produtor do cache
            // —, entao duas capturas que so diferem nela sao duas configuracoes, nao duas amostras.
            if (S.GIBackfacePolicy)       Add("bf");
            if (S.GIMeasureTerminatorOff) Add("noTerm");
            return Tag.empty() ? std::string("off") : Tag;
        }

        std::string JsonEscape(const std::string& In) {
            std::string Out;
            Out.reserve(In.size() + 8);
            for (const char C : In) {
                switch (C) {
                    case '"':  Out += "\\\""; break;
                    case '\\': Out += "\\\\"; break;
                    case '\n': Out += "\\n";  break;
                    case '\r': Out += "\\r";  break;
                    case '\t': Out += "\\t";  break;
                    default:   Out += C;      break;
                }
            }
            return Out;
        }

        std::string Utf8(const std::wstring& In) {
            if (In.empty()) return {};
            const int Bytes = WideCharToMultiByte(CP_UTF8, 0, In.c_str(),
                                                  static_cast<int>(In.size()),
                                                  nullptr, 0, nullptr, nullptr);
            std::string Out(static_cast<size_t>(Bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, In.c_str(), static_cast<int>(In.size()),
                                Out.data(), Bytes, nullptr, nullptr);
            return Out;
        }

        // PNG por WIC (a windowscodecs ja e dependencia do Texture.cpp, que DECODIFICA por ela).
        // 24bppBGR e formato nativo do encoder de PNG e evita gravar o alfa do backbuffer, que
        // nao tem significado depois do tonemap.
        bool WritePng(const std::wstring& Path, const u8* Bgr, u32 Width, u32 Height,
                      std::string& OutError) {
            using Microsoft::WRL::ComPtr;
            // Ignorar o retorno e deliberado: a render thread pode ja ter COM inicializado por
            // outro subsistema (RPC_E_CHANGED_MODE), e isso nao impede o factory de funcionar.
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            ComPtr<IWICImagingFactory2> Factory;
            HRESULT Hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&Factory));
            if (FAILED(Hr)) { OutError = "WIC indisponivel"; return false; }

            ComPtr<IWICStream> Stream;
            Hr = Factory->CreateStream(&Stream);
            if (SUCCEEDED(Hr)) Hr = Stream->InitializeFromFilename(Path.c_str(), GENERIC_WRITE);
            if (FAILED(Hr)) { OutError = "nao foi possivel criar o arquivo"; return false; }

            ComPtr<IWICBitmapEncoder> Encoder;
            Hr = Factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &Encoder);
            if (SUCCEEDED(Hr)) Hr = Encoder->Initialize(Stream.Get(), WICBitmapEncoderNoCache);
            if (FAILED(Hr)) { OutError = "encoder PNG falhou"; return false; }

            ComPtr<IWICBitmapFrameEncode> Frame;
            ComPtr<IPropertyBag2> Props;
            Hr = Encoder->CreateNewFrame(&Frame, &Props);
            if (SUCCEEDED(Hr)) Hr = Frame->Initialize(Props.Get());
            if (SUCCEEDED(Hr)) Hr = Frame->SetSize(Width, Height);

            WICPixelFormatGUID Format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(Hr)) Hr = Frame->SetPixelFormat(&Format);
            if (SUCCEEDED(Hr) && Format != GUID_WICPixelFormat24bppBGR) {
                OutError = "o encoder recusou 24bppBGR";
                return false;
            }
            if (SUCCEEDED(Hr)) {
                const UINT Stride = Width * 3u;
                Hr = Frame->WritePixels(Height, Stride, Stride * Height,
                                        const_cast<BYTE*>(Bgr));
            }
            if (SUCCEEDED(Hr)) Hr = Frame->Commit();
            if (SUCCEEDED(Hr)) Hr = Encoder->Commit();
            if (FAILED(Hr)) { OutError = "falha ao gravar o PNG"; return false; }
            return true;
        }
    }

    bool FFrameCapture::Request(const FCaptureRequest& _Req) {
        if (Busy()) return false;
        // Resultado anterior ainda por consumir: aceitar aqui sobrescreveria o LastResult e a UI
        // perderia a notificacao — inclusive o caminho do PNG que acabou de ser gravado, ou o
        // motivo de um cancelamento. A janela e de um tique do poll (200 ms), e recusar por 200 ms
        // e melhor que engolir o desfecho da captura anterior.
        if (ResultReady) return false;
        PendingRequest = _Req;
        RequestPending = true;
        return true;
    }

    void FFrameCapture::BeginSession() {
        if (!RequestPending) return;
        ActiveRequest  = PendingRequest;
        RequestPending = false;
        Remaining      = ActiveRequest.WarmupFrames;
        CopyRecorded   = false;
        // N = 0 e legitimo e util: captura o primeiro frame apos o reset, que e o retrato do
        // cache VAZIO. E a outra ponta da regua de convergencia.
        CurrentStage   = (Remaining == 0) ? EStage::Shoot : EStage::Warmup;
    }

    bool FFrameCapture::AdvanceFrame() {
        switch (CurrentStage) {
            case EStage::Warmup:
                // O decremento e aqui, no FIM do frame RENDERIZADO. Nao ha caminho por onde um
                // tique de UI que nao renderizou chegue ate este ponto.
                if (Remaining > 0) --Remaining;
                if (Remaining == 0) CurrentStage = EStage::Shoot;
                return false;
            case EStage::Shoot:
                CurrentStage = EStage::Idle;
                return true;
            default:
                return false;
        }
    }

    void FFrameCapture::Cancel(const char* _Reason) {
        if (!SessionActive() && !RequestPending) return;
        // Um pedido que nem chegou a comecar nao tem estado a devolver, mas TEM de produzir
        // desfecho: a UI ja escreveu "aquecendo N frames" no clique, e sumir so com uma linha de
        // log deixaria esse texto na tela para sempre. Todo pedido aceito termina em sucesso ou em
        // falha visivel.
        if (!SessionActive()) {
            RequestPending          = false;
            LastResult              = FResult{};
            LastResult.Success      = false;
            LastResult.WarmupFrames = PendingRequest.WarmupFrames;
            LastResult.Error        = _Reason ? _Reason : "cancelada";
            ResultReady             = true;
            LogWarning("Captura descartada antes de comecar: " + LastResult.Error);
            return;
        }
        CurrentStage = EStage::Idle;
        Remaining    = 0;
        CopyRecorded = false;
        // O preset volta pelo caminho normal, no proximo frame: cancelar nao pode deixar o
        // operador com a resolucao e o upscaler da MEDICAO.
        if (HasStash) {
            HasStash   = false;
            RestoreDue = true;
        }
        LastResult             = FResult{};
        LastResult.Success     = false;
        LastResult.WarmupFrames = ActiveRequest.WarmupFrames;
        LastResult.Error       = _Reason ? _Reason : "cancelada";
        ResultReady            = true;
        LogWarning(std::string("Captura cancelada: ") + LastResult.Error);
    }

    bool FFrameCapture::EnsureReadback(ID3D12Device* _Device, u32 _Width, u32 _Height) {
        if (!_Device || _Width == 0 || _Height == 0) return false;
        const u32 Pitch = (_Width * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                          ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        const u64 Bytes = static_cast<u64>(Pitch) * _Height;
        if (Readback && ReadbackBytes >= Bytes && ReadbackWidth == _Width &&
            ReadbackHeight == _Height) {
            return true;
        }
        Readback = GpuResources::CreateReadbackBuffer(_Device, Bytes);
        if (!Readback) return false;
        Readback->SetName(L"Captura deterministica (readback)");
        ReadbackBytes  = Bytes;
        ReadbackWidth  = _Width;
        ReadbackHeight = _Height;
        ReadbackPitch  = Pitch;
        return true;
    }

    void FFrameCapture::RecordCopy(ID3D12Device* _Device, ID3D12GraphicsCommandList* _CL,
                                   ID3D12Resource* _BackBuffer, u32 _Width, u32 _Height) {
        CopyRecorded = false;
        if (!_CL || !_BackBuffer) return;
        if (!EnsureReadback(_Device, _Width, _Height)) {
            LogError("Captura: falha ao alocar o readback do backbuffer");
            return;
        }

        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource   = _BackBuffer;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        _CL->ResourceBarrier(1, &Barrier);

        D3D12_TEXTURE_COPY_LOCATION Src{};
        Src.pResource        = _BackBuffer;
        Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst{};
        Dst.pResource                          = Readback.Get();
        Dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint.Offset             = 0;
        Dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        Dst.PlacedFootprint.Footprint.Width    = _Width;
        Dst.PlacedFootprint.Footprint.Height   = _Height;
        Dst.PlacedFootprint.Footprint.Depth    = 1;
        Dst.PlacedFootprint.Footprint.RowPitch = ReadbackPitch;
        _CL->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

        // Devolve o estado: o contorno de selecao e o debug draw ainda desenham por cima, e o
        // fim do RecordPost conta com RENDER_TARGET para fechar em PRESENT.
        std::swap(Barrier.Transition.StateBefore, Barrier.Transition.StateAfter);
        _CL->ResourceBarrier(1, &Barrier);

        CopyRecorded = true;
    }

    std::wstring FFrameCapture::ResolveOutputDir() const {
        if (!ActiveRequest.OutputDir.empty()) return ActiveRequest.OutputDir;
        return (ExecutableDir() / L"Captures").wstring();
    }

    void FFrameCapture::Finish(const FCaptureState& _State) {
        FResult Result;
        Result.WarmupFrames = ActiveRequest.WarmupFrames;

        // A sessao acabou: o preset pode voltar. Fica ANTES dos early returns de erro — falhar em
        // gravar o PNG nao e motivo para deixar o operador com o upscaler desligado sem saber.
        if (HasStash) {
            HasStash   = false;
            RestoreDue = true;
        }

        if (!CopyRecorded || !Readback) {
            Result.Error = "a copia do backbuffer nao foi gravada";
            LastResult   = Result;
            ResultReady  = true;
            return;
        }
        CopyRecorded = false;

        // RGBA (backbuffer) -> BGR (encoder). O alfa fica de fora: depois do tonemap ele nao
        // carrega significado, e gravar 25% a mais de bytes por causa disso seria so peso.
        std::vector<u8> Bgr(static_cast<size_t>(ReadbackWidth) * ReadbackHeight * 3u);
        {
            void* Mapped = nullptr;
            D3D12_RANGE ReadRange{ 0, static_cast<SIZE_T>(ReadbackBytes) };
            const HRESULT Hr = Readback->Map(0, &ReadRange, &Mapped);
            if (FAILED(Hr) || !Mapped) {
                Result.Error = "falha ao mapear o readback";
                LastResult   = Result;
                ResultReady  = true;
                return;
            }
            const auto* Src = static_cast<const u8*>(Mapped);
            for (u32 Y = 0; Y < ReadbackHeight; ++Y) {
                const u8* SrcRow = Src + static_cast<size_t>(Y) * ReadbackPitch;
                u8* DstRow = Bgr.data() + static_cast<size_t>(Y) * ReadbackWidth * 3u;
                for (u32 X = 0; X < ReadbackWidth; ++X) {
                    DstRow[X * 3u + 0] = SrcRow[X * 4u + 2]; // B
                    DstRow[X * 3u + 1] = SrcRow[X * 4u + 1]; // G
                    DstRow[X * 3u + 2] = SrcRow[X * 4u + 0]; // R
                }
            }
            D3D12_RANGE NoWrite{ 0, 0 };
            Readback->Unmap(0, &NoWrite);
        }

        const fs::path Dir(ResolveOutputDir());
        std::error_code Ec;
        fs::create_directories(Dir, Ec);

        const bool Scientific = ActiveRequest.Preset == ECapturePreset::Scientific;
        std::string Label = Sanitize(ActiveRequest.Label.empty()
                                         ? (ActiveRequest.SceneName.empty()
                                                ? std::string("capture")
                                                : ActiveRequest.SceneName)
                                         : ActiveRequest.Label);
        if (Label.empty()) Label = "capture";

        std::string Base = Label;
        if (ActiveRequest.BookmarkSlot >= 0)
            Base += "_slot" + std::to_string(ActiveRequest.BookmarkSlot);
        Base += Scientific ? "_sci" : "_game";
        Base += "_N" + std::to_string(ActiveRequest.WarmupFrames);
        Base += "_" + ConfigTag(_State);
        Base += "_" + TimeStamp();

        // O carimbo tem resolucao de UM SEGUNDO, e nada impede duas capturas da mesma
        // configuracao dentro do mesmo segundo — a segunda sobrescreveria a primeira sem aviso, e
        // perder uma captura em silencio e o oposto do que este arquivo existe para fazer.
        // Sufixo numerico ate achar um nome livre; o par PNG+JSON e reservado junto, senao o
        // desempate acharia um .png livre ao lado de um .json ocupado.
        std::string Name = Base;
        for (u32 Suffix = 2; Suffix < 1000 &&
             (fs::exists(Dir / (Name + ".png")) || fs::exists(Dir / (Name + ".json")));
             ++Suffix) {
            Name = Base + "-" + std::to_string(Suffix);
        }

        const fs::path PngPath      = Dir / (Name + ".png");
        const fs::path ManifestPath = Dir / (Name + ".json");
        // Escrita em .tmp + rename no fim. PNG e manifesto so tem valor JUNTOS; falhar no meio
        // deixaria um PNG orfao que, meses depois, e indistinguivel de uma captura valida cujo
        // manifesto alguem apagou.
        const fs::path PngTmp      = Dir / (Name + ".png.tmp");
        const fs::path ManifestTmp = Dir / (Name + ".json.tmp");
        auto DropTemporaries = [&] {
            std::error_code Ignored;
            fs::remove(PngTmp, Ignored);
            fs::remove(ManifestTmp, Ignored);
        };

        if (!WritePng(PngTmp.wstring(), Bgr.data(), ReadbackWidth, ReadbackHeight,
                      Result.Error)) {
            DropTemporaries();
            LastResult  = Result;
            ResultReady = true;
            LogError("Captura: " + Result.Error);
            return;
        }

        // Manifesto PLANO, uma chave por linha — mesma forma dos sidecars que o SceneLoader ja le,
        // e legivel num diff. E ele que torna a captura comparavel meses depois.
        bool ManifestOk = false;
        {
            std::ofstream File(ManifestTmp, std::ios::binary);
            if (File) {
                auto Bool = [](bool V) { return V ? "true" : "false"; };
                File << "{\n";
                File << "  \"schema\": 1,\n";
                File << "  \"time\": \"" << IsoTime() << "\",\n";
                File << "  \"build\": \"" << SMILE_BUILD_COMMIT << "\",\n";
                File << "  \"buildDate\": \"" << SMILE_BUILD_DATE << "\",\n";
                File << "  \"image\": \"" << JsonEscape(Utf8(PngPath.filename().wstring()))
                     << "\",\n";
                File << "  \"scene\": \"" << JsonEscape(ActiveRequest.SceneName) << "\",\n";
                File << "  \"bookmarkSlot\": " << ActiveRequest.BookmarkSlot << ",\n";
                File << "  \"preset\": \"" << (Scientific ? "scientific" : "gameplay") << "\",\n";
                File << "  \"warmupFrames\": " << ActiveRequest.WarmupFrames << ",\n";
                File << "  \"temporalSampleIndex\": " << _State.TemporalSampleIndex << ",\n";
                File << "  \"frameIndex\": " << _State.FrameIndex << ",\n";
                File << "  \"outputWidth\": "  << _State.OutputWidth  << ",\n";
                File << "  \"outputHeight\": " << _State.OutputHeight << ",\n";
                File << "  \"renderWidth\": "  << _State.RenderWidth  << ",\n";
                File << "  \"renderHeight\": " << _State.RenderHeight << ",\n";
                File << "  \"renderScale\": "  << _State.RenderScale  << ",\n";
                File << "  \"upscaler\": \"" << _State.Upscaler << "\",\n";
                File << "  \"upscalerQuality\": " << _State.UpscalerQuality << ",\n";
                File << "  \"denoiser\": \"" << _State.Denoiser << "\",\n";
                File << "  \"taa\": " << Bool(_State.UseTAA) << ",\n";
                File << "  \"useGI\": "        << Bool(_State.UseGI)       << ",\n";
                File << "  \"ddgiReady\": "    << Bool(_State.DDGIReady)   << ",\n";
                File << "  \"restirGI\": "     << Bool(_State.ReSTIRGI)    << ",\n";
                File << "  \"restirDI\": "     << Bool(_State.ReSTIRDI)    << ",\n";
                // `regir` continua sendo o EFETIVO — os manifestos ja capturados da serie usam
                // esta chave com esse significado, e renomea-la quebraria a comparacao com eles.
                // Os dois novos ficam ao lado para que "pedido true, efetivo false" seja legivel
                // sem sair do arquivo.
                File << "  \"regir\": "        << Bool(_State.ReGIR)       << ",\n";
                File << "  \"regirRequested\": " << Bool(_State.ReGIRRequested) << ",\n";
                File << "  \"giPunctualLightCount\": " << _State.PunctualLightCount << ",\n";
                File << "  \"reflections\": "  << Bool(_State.Reflections) << ",\n";
                File << "  \"cacheUpdate\": "  << Bool(_State.CacheUpdate) << ",\n";
                File << "  \"cacheQuery\": "   << Bool(_State.CacheQuery)  << ",\n";
                // O ESTIMADOR que encheu a tabela, e os dois parametros que mandam na convergencia
                // dele. Ver o bloco correspondente no FCaptureState.
                File << "  \"cacheDedicatedUpdate\": "
                     << Bool(_State.CacheDedicatedUpdate) << ",\n";
                File << "  \"cacheUpdateFraction\": "
                     << _State.CacheUpdateFraction << ",\n";
                File << "  \"cacheUsePreviousTerminal\": "
                     << Bool(_State.CacheUsePrevTerminal) << ",\n";
                File << "  \"cacheMaxVertices\": "  << _State.CacheMaxVertices  << ",\n";
                File << "  \"cacheMinRoughness\": " << _State.CacheMinRoughness << ",\n";
                // Piso de confianca da CONSULTA (Fase 4). Zero = ninguem consultou neste frame.
                File << "  \"cacheMinSamples\": "   << _State.CacheMinSamples   << ",\n";
                // Regime de medicao, nao detalhe de diagnostico: com a instrumentacao ligada os
                // atomicos mudam quem vence as insercoes do cache. Comparar uma captura
                // instrumentada com uma normal e comparar duas configuracoes diferentes.
                File << "  \"cacheStatsEnabled\": " << Bool(_State.CacheStats) << ",\n";
                File << "  \"cacheStatsDetail\": " << Bool(_State.CacheStatsDetail) << ",\n";
                File << "  \"giBackfacePolicy\": " << Bool(_State.GIBackfacePolicy) << ",\n";
                File << "  \"giTerminatorOff\": " << Bool(_State.GIMeasureTerminatorOff) << ",\n";
                File << "  \"cacheOccupied\": " << _State.CacheOccupied << ",\n";
                File << "  \"cacheValid\": "    << _State.CacheValid    << ",\n";
                File << "  \"cacheSamples\": "  << _State.CacheSamples  << ",\n";
                File << "  \"cacheCapacity\": " << _State.CacheCapacity << ",\n";
                File << "  \"cacheQueries\": "  << _State.CacheQueries  << ",\n";
                File << "  \"cacheHits\": "     << _State.CacheHits     << ",\n";
                // Detalhe (zerado sem `cacheStatsDetail`). Os cinco motivos de miss pedem coisas
                // opostas — capacidade, cobertura, tempo, ou nada — e somados nao respondem
                // nenhuma; `insertFull/insertTries` e o gate de saida do balde cheio.
                File << "  \"cacheMissShortSegment\": " << _State.CacheMissShort   << ",\n";
                File << "  \"cacheMissNarrowCone\": "   << _State.CacheMissCone    << ",\n";
                File << "  \"cacheMissNoEntry\": "      << _State.CacheMissNoEntry << ",\n";
                File << "  \"cacheMissNoSamples\": "    << _State.CacheMissEmpty   << ",\n";
                File << "  \"cacheMissWarming\": "      << _State.CacheMissWarming << ",\n";
                File << "  \"cacheMissStale\": "        << _State.CacheMissStale   << ",\n";
                File << "  \"cacheInsertTries\": "      << _State.CacheInsertTries << ",\n";
                File << "  \"cacheInsertFull\": "       << _State.CacheInsertFull  << ",\n";
                File << "  \"cacheProbeSum\": "         << _State.CacheProbeSum    << ",\n";
                File << "  \"cacheProbeMax\": "         << _State.CacheProbeMax    << ",\n";
                // O produtor: caminhos, profundidade e por onde eles terminaram.
                File << "  \"cachePaths\": "            << _State.CachePaths       << ",\n";
                File << "  \"cachePathVerts\": "        << _State.CachePathVerts   << ",\n";
                File << "  \"cachePathDepthMax\": "     << _State.CachePathDepth   << ",\n";
                File << "  \"cacheTermSky\": "          << _State.CacheTermSky     << ",\n";
                File << "  \"cacheTermCache\": "        << _State.CacheTermCache   << ",\n";
                File << "  \"cacheTermKilled\": "       << _State.CacheTermKilled  << ",\n";
                File << "  \"cacheTermZero\": "         << _State.CacheTermZero    << ",\n";
                File << "  \"cameraX\": " << _State.CameraPos[0] << ",\n";
                File << "  \"cameraY\": " << _State.CameraPos[1] << ",\n";
                File << "  \"cameraZ\": " << _State.CameraPos[2] << ",\n";
                File << "  \"pitchDeg\": " << _State.PitchDeg << ",\n";
                File << "  \"yawDeg\": "   << _State.YawDeg   << ",\n";
                File << "  \"fovYDeg\": "  << _State.FovYDeg  << ",\n";
                File << "  \"sunX\": " << _State.SunDir[0] << ",\n";
                File << "  \"sunY\": " << _State.SunDir[1] << ",\n";
                File << "  \"sunZ\": " << _State.SunDir[2] << ",\n";
                File << "  \"timeOfDayHours\": " << _State.TimeOfDayHours << ",\n";
                File << "  \"timeOfDayEnabled\": " << Bool(_State.TimeOfDayEnabled) << ",\n";
                // O pin PEDIDO e o APLICADO, separados. Negativo no aplicado = a sessao nao fixou
                // hora nenhuma — ou porque ninguem pediu, ou porque o TOD estava desligado e o
                // pedido nao tinha o que fazer. Distinguir importa: duas capturas so sao
                // comparaveis em iluminacao se as duas fixaram a MESMA hora de fato.
                File << "  \"pinnedTimeOfDayRequested\": "
                     << ActiveRequest.PinTimeOfDayHours << ",\n";
                File << "  \"pinnedTimeOfDayApplied\": "
                     << _State.PinnedHoursApplied << "\n";
                File << "}\n";
                File.flush();
                ManifestOk = File.good();
            }
        }

        // Sem manifesto NAO ha captura. Uma imagem sozinha nao e comparavel — nao se sabe de que
        // configuracao, de que N nem de que build ela saiu —, e comparavel e a unica coisa que ela
        // deveria ser. Dizer "sucesso com ressalva" convidaria a usar o PNG assim mesmo, entao a
        // sessao FALHA e os dois temporarios vao embora juntos.
        if (!ManifestOk) {
            DropTemporaries();
            Result.Success = false;
            Result.Error   = "manifesto nao pode ser gravado; a captura foi descartada";
            LastResult     = Result;
            ResultReady    = true;
            LogError("Captura: " + Result.Error);
            return;
        }

        // Publicacao: dois renames, e a ORDEM importa porque nao ha como torna-los atomicos entre
        // si. O manifesto vai PRIMEIRO e o PNG por ultimo, de modo que o unico estado intermediario
        // possivel — processo morrendo entre os dois — deixe um manifesto sem imagem. Esse e
        // obviamente incompleto e ninguem o confunde com captura valida; a ordem inversa deixaria
        // um PNG orfao, que parece uma captura boa cujo manifesto alguem apagou. Falha no segundo
        // rename desfaz o primeiro.
        std::error_code RenameEc;
        fs::rename(ManifestTmp, ManifestPath, RenameEc);
        if (!RenameEc) {
            fs::rename(PngTmp, PngPath, RenameEc);
            if (RenameEc) {
                std::error_code Ignored;
                fs::remove(ManifestPath, Ignored); // desfaz o manifesto ja publicado
            }
        }
        if (RenameEc) {
            DropTemporaries();
            Result.Success = false;
            Result.Error   = "falha ao publicar os arquivos da captura";
            LastResult     = Result;
            ResultReady    = true;
            LogError("Captura: " + Result.Error);
            return;
        }

        Result.Success      = true;
        Result.PngPath      = PngPath.wstring();
        Result.ManifestPath = ManifestPath.wstring();
        LastResult          = Result;
        ResultReady         = true;

        LogInfo("Captura gravada: " + Utf8(PngPath.wstring()) +
                " (N=" + std::to_string(ActiveRequest.WarmupFrames) +
                ", tsi=" + std::to_string(_State.TemporalSampleIndex) + ")");
    }

    bool FFrameCapture::ConsumeResult(FResult& _Out) {
        if (!ResultReady) return false;
        _Out        = LastResult;
        ResultReady = false;
        return true;
    }

    void FFrameCapture::Release() {
        Readback.Reset();
        ReadbackBytes = ReadbackWidth = ReadbackHeight = ReadbackPitch = 0;
        CopyRecorded  = false;
        CurrentStage  = EStage::Idle;
        Remaining     = 0;
        RequestPending = false;
        // Nao ha proximo frame para devolver o preset; deixar o flag de pe so faria o Renderer
        // mexer em upscaler/render scale no meio do shutdown.
        HasStash   = false;
        RestoreDue = false;
    }
}
