<#
.SYNOPSIS
    Cozinha (ou verifica) as cenas segundo a receita em Tools/Scenes.cook.json.

.DESCRIPTION
    Assets/Scenes/ esta no .gitignore, entao os .smesh/.sscene nao entram no git. O que se
    versiona e a RECEITA (Scenes.cook.json) e a BASELINE (Tools/cook-baseline.json), produzida
    por este script. Juntas elas tornam o recook reproduzivel e detectam divergencia entre a
    arvore de trabalho e a baseline usada nas medicoes.

    A baseline fingerprinta as ENTRADAS do cozimento, nao so as saidas: SHA256 de cada FBX
    (tambem gitignored), da propria receita e do binario do cooker. Sem isso, outro FBX no
    mesmo caminho ou um cooker construido de arvore suja seriam indistinguiveis.

    Ver Docs/GPU-DRIVEN-PLAN.md, Fase 0.

.PARAMETER Verify
    Nao cozinha. Exige o schema completo da baseline e compara TODA a procedencia declarada:
    receita, config, binario do cooker, fonte de cada cena, formato (magic/versao) e hash das
    saidas. Ausencia de qualquer fingerprint e falha, nao "passa por omissao".

.PARAMETER Record
    Escreve a baseline a partir dos assets ja presentes, sem cozinhar. Recusa registrar
    qualquer arquivo cujo magic/versao nao case com o bloco "expect" da receita.

.PARAMETER Only
    Substring do caminho de origem; cozinha apenas as cenas que casarem. Nunca reescreve a
    baseline (seria parcial).

.PARAMETER NoBuild
    Nao reconstroi o SmileCooker antes de cozinhar. O default e deixar o CMake decidir, que e a
    unica forma de respeitar as dependencias reais do target (ufbx, CMakeLists, headers
    transitivos) em vez de adivinhar por timestamp de dois arquivos.

.PARAMETER NoOmm
    Nao roda o SmileOmmBaker depois de cozinhar. Sem ele a cena carrega do mesmo jeito, mas
    bakeia os Opacity Micro-Maps em runtime, com teto de workload (mais lento e com cobertura
    menor). O sidecar <cena>.somm se valida sozinho por hash de conteudo e NAO entra na baseline:
    um .somm velho e recusado par a par no load, nunca usado errado. Ver Docs/OMM-AUDIT.md.

.PARAMETER Config
    Configuracao do cooker (default Release).

.PARAMETER BuildDir
    Diretorio de build do CMake (default "build"; aceita relativo ao repo ou absoluto). O cooker
    e DERIVADO dele: <buildDir>/bin/<config>/<cookerName>.

.EXAMPLE
    powershell -File Tools/CookScenes.ps1
    powershell -File Tools/CookScenes.ps1 -Only Bistro
    powershell -File Tools/CookScenes.ps1 -Verify
    powershell -File Tools/CookScenes.ps1 -Record
#>
[CmdletBinding()]
param(
    [switch] $Verify,
    [switch] $Record,
    [switch] $NoBuild,
    [switch] $NoOmm,
    [string] $Only     = "",
    [string] $Config   = "Release",
    [string] $BuildDir = "build"
)

$ErrorActionPreference = "Stop"

$RepoRoot     = Split-Path -Parent $PSScriptRoot
$RecipePath   = Join-Path $PSScriptRoot "Scenes.cook.json"
$BaselinePath = Join-Path $PSScriptRoot "cook-baseline.json"

if (-not (Test-Path $RecipePath)) { throw "receita nao encontrada: $RecipePath" }
$Recipe    = Get-Content $RecipePath -Raw | ConvertFrom-Json
$RecipeSha = (Get-FileHash $RecipePath -Algorithm SHA256).Hash

# BuildDir normalizado (aceita relativo ao repo ou absoluto) e o cooker DERIVADO dele. Um
# caminho fixo na receita fazia -BuildDir construir num diretorio e executar o binario de outro.
$BuildRoot = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir }
             else { Join-Path $RepoRoot $BuildDir }
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
if (-not $Recipe.cookerName) { throw "receita sem 'cookerName'" }
$CookerPath = Join-Path $BuildRoot (Join-Path "bin" (Join-Path $Config $Recipe.cookerName))
# O bake offline de OMM: ferramenta propria porque linka a engine (o Cooker nao linka).
$OmmBakerPath = Join-Path $BuildRoot (Join-Path "bin" (Join-Path $Config "SmileOmmBaker.exe"))

if (-not $Recipe.expect) { throw "receita sem bloco 'expect' (magic/versao esperados)" }
$ExpectVersion = [int] $Recipe.expect.version
$ExpectMagic   = @{
    "smesh"  = [Convert]::ToUInt32($Recipe.expect.smeshMagic,  16)
    "sscene" = [Convert]::ToUInt32($Recipe.expect.ssceneMagic, 16)
}

# Le o cabecalho comum de .smesh/.sscene: magic (u32), versao (u32) e dois contadores.
# O layout coincide nesses 16 bytes (Engine/Include/Smile/Scene/CookedFormat.h).
function Get-CookedHeader {
    param([string] $Path)
    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Bytes = New-Object byte[] 16
        if ($Stream.Read($Bytes, 0, 16) -ne 16) { throw "arquivo curto demais: $Path" }
    } finally {
        $Stream.Dispose()
    }
    [PSCustomObject]@{
        Magic   = [BitConverter]::ToUInt32($Bytes, 0)
        Version = [BitConverter]::ToUInt32($Bytes, 4)
        FieldC  = [BitConverter]::ToUInt32($Bytes, 8)
        FieldD  = [BitConverter]::ToUInt32($Bytes, 12)
    }
}

# Devolve "" quando o arquivo casa com o formato esperado, ou a descricao do problema. E o
# guarda que impede -Record/-Verify de oficializarem um formato que este checkout rejeita:
# sem ele, -Record registraria os antigos v9 e -Verify aceitaria, porque so compara com a
# propria baseline.
function Test-CookedFormat {
    param([PSCustomObject] $Header, [string] $Ext)
    $WantMagic = $ExpectMagic[$Ext]
    if ($Header.Magic -ne $WantMagic) {
        return ("magic 0x{0:X8} != 0x{1:X8} esperado" -f $Header.Magic, $WantMagic)
    }
    if ([int] $Header.Version -ne $ExpectVersion) {
        return ("versao {0} != {1} esperada pela receita" -f $Header.Version, $ExpectVersion)
    }
    return ""
}

function Get-Fingerprint {
    param([string] $Path)
    if (-not (Test-Path $Path)) { return $null }
    [ordered]@{
        sha256 = (Get-FileHash $Path -Algorithm SHA256).Hash
        bytes  = (Get-Item $Path).Length
    }
}

# Registro de uma cena: fingerprint da FONTE mais o das duas saidas, com validacao de formato.
# -Strict faz formato invalido virar excecao (Record e pos-cook); no Verify vira divergencia.
function Get-OutputRecord {
    param([PSCustomObject] $Scene, [switch] $Strict)
    $OutBase = [System.IO.Path]::ChangeExtension($Scene.source, $null).TrimEnd('.')
    $Record  = [ordered]@{ outBase = $OutBase }

    $Record["source"]       = $Scene.source
    $Record["sourceDigest"] = Get-Fingerprint (Join-Path $RepoRoot $Scene.source)
    $Record["flags"]        = @($Scene.flags)
    # Fonte ausente nao pode virar sourceDigest=null numa baseline gravada: a procedencia
    # ficaria incompleta e o -Verify seguinte nao teria contra o que comparar.
    if ($Strict -and -not $Record["sourceDigest"]) {
        throw "fonte ausente: $($Scene.source) - a baseline nao pode ser gravada sem ela"
    }

    foreach ($Ext in @("smesh", "sscene")) {
        $Full = Join-Path $RepoRoot "$OutBase.$Ext"
        if (-not (Test-Path $Full)) { $Record[$Ext] = $null; continue }
        $Header  = Get-CookedHeader $Full
        $Problem = Test-CookedFormat $Header $Ext
        if ($Problem -ne "" -and $Strict) { throw "$OutBase.$Ext : $Problem" }
        $Record[$Ext] = [ordered]@{
            sha256        = (Get-FileHash $Full -Algorithm SHA256).Hash
            bytes         = (Get-Item $Full).Length
            version       = [int] $Header.Version
            magic         = ("0x{0:X8}" -f $Header.Magic)
            countC        = [int] $Header.FieldC
            countD        = [int] $Header.FieldD
            formatProblem = $Problem
        }
    }
    $Record
}

# HEAD sozinho engana: a baseline pode ser gerada com a arvore suja, e foi o que aconteceu na
# primeira versao deste script (a baseline dizia f725a65 enquanto o proprio script estava
# uncommitted). Gravar o estado sujo torna a ambiguidade visivel em vez de silenciosa.
function Get-RepoStamp {
    $Hash  = "desconhecido"
    $Dirty = $true
    try {
        $Hash  = (& git -C $RepoRoot rev-parse --short HEAD).Trim()
        $Dirty = -not [string]::IsNullOrWhiteSpace((& git -C $RepoRoot status --porcelain | Out-String).Trim())
    } catch { }
    [ordered]@{ commit = $Hash; dirty = $Dirty }
}

# Falha quando qualquer peca da procedencia estiver ausente. Sem isto os guards viravam
# "if presente", e ausencia passava: uma baseline incompleta era aceita em silencio.
function Assert-BaselineSchema {
    param($Baseline)
    $Missing = @()
    foreach ($Field in @("generatedUtc", "commit", "treeDirty", "config", "expectVersion",
                         "recipeSha256", "provenanceMode", "outputs")) {
        if ($null -eq $Baseline.$Field) { $Missing += $Field }
    }
    if (-not $Baseline.cooker -or -not $Baseline.cooker.sha256) { $Missing += "cooker.sha256" }
    # Enum fechado: sem isto qualquer string desconhecida cairia no ramo "nao e cooked" e seria
    # tratada como adopted, escondendo uma baseline corrompida ou de versao futura.
    if ($null -ne $Baseline.provenanceMode -and
        @("cooked", "adopted") -notcontains $Baseline.provenanceMode) {
        $Missing += "provenanceMode invalido ('$($Baseline.provenanceMode)'; esperado cooked ou adopted)"
    }
    foreach ($Out in $Baseline.outputs) {
        foreach ($Field in @("outBase", "source", "sourceDigest", "flags", "smesh", "sscene")) {
            if ($null -eq $Out.$Field) { $Missing += "outputs[$($Out.outBase)].$Field" }
        }
        if ($Out.sourceDigest -and -not $Out.sourceDigest.sha256) {
            $Missing += "outputs[$($Out.outBase)].sourceDigest.sha256"
        }
    }
    if ($Missing.Count -gt 0) {
        throw ("baseline incompleta (regrave com -Record ou recook): faltam " + ($Missing -join ", "))
    }
}

# provenanceMode distingue o que a baseline PODE afirmar sobre o cooker:
#   cooked  = este script cozinhou as saidas com ESTE binario nesta execucao;
#   adopted = as saidas ja existiam e o hash e do binario que estava presente na hora, que pode
#             nao ser o produtor real.
# Sem o campo, outra ferramenta leria cooker.sha256 como produtor comprovado nos dois casos.
function Write-Baseline {
    param($Outputs, $CookerFingerprint, [string] $ProvenanceMode)
    $Stamp    = Get-RepoStamp
    $Manifest = [ordered]@{
        generatedUtc   = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        commit         = $Stamp.commit
        treeDirty      = $Stamp.dirty
        config         = $Config
        expectVersion  = $ExpectVersion
        recipeSha256   = $RecipeSha
        provenanceMode = $ProvenanceMode
        cooker         = $CookerFingerprint
        outputs        = $Outputs
    }
    # MESMO validador do -Verify, ANTES de gravar. Sem isto o writer aceitava sourceDigest=null
    # (FBX ausente) ou cooker=null, sobrescrevia uma baseline valida por uma quebrada e saia com
    # sucesso: um verificador rigoroso nao adianta se o escritor pode destruir o que ele confere.
    Assert-BaselineSchema $Manifest

    $Json = $Manifest | ConvertTo-Json -Depth 8
    # PS 5.1 grava BOM com Out-File -Encoding utf8, e BOM quebra parser JSON estrito.
    [System.IO.File]::WriteAllText($BaselinePath, $Json, (New-Object System.Text.UTF8Encoding $false))
    if ($Stamp.dirty) {
        Write-Host "AVISO: arvore suja na geracao (treeDirty=true na baseline)."
    }
}

$Selected = $Recipe.scenes | Where-Object { $Only -eq "" -or $_.source -like "*$Only*" }
if (-not $Selected) { throw "nenhuma cena casou com -Only '$Only'" }

# ---------------------------------------------------------------- verificacao
if ($Verify) {
    if (-not (Test-Path $BaselinePath)) { throw "baseline ausente: $BaselinePath (rode -Record ou cozinhe primeiro)" }
    $Baseline = Get-Content $BaselinePath -Raw | ConvertFrom-Json
    Assert-BaselineSchema $Baseline
    $Failures = 0

    Write-Host ("procedencia: {0}  (commit {1}{2}, config {3})" -f
                $Baseline.provenanceMode, $Baseline.commit,
                $(if ($Baseline.treeDirty) { ", arvore suja" } else { "" }), $Baseline.config)
    if ($Baseline.provenanceMode -ne "cooked") {
        Write-Host "  NOTA: cooker.sha256 foi ADOTADO, nao comprovado como produtor destes assets."
    }
    Write-Host ""

    # Contrato declarado: receita, config e cooker sao comparados SEMPRE, nao "se presentes".
    if ([int] $Baseline.expectVersion -ne $ExpectVersion) {
        Write-Host ("  RECEITA MUDOU  baseline espera v{0}, receita espera v{1}" -f $Baseline.expectVersion, $ExpectVersion)
        $Failures++
    }
    if ($Baseline.recipeSha256 -ne $RecipeSha) {
        Write-Host "  RECEITA MUDOU  Scenes.cook.json difere da usada na baseline"
        $Failures++
    }
    if ($Baseline.config -ne $Config) {
        Write-Host ("  CONFIG DIFERE  baseline={0}, atual={1}" -f $Baseline.config, $Config)
        $Failures++
    }
    # Cooker diferente nao altera assets ja no disco, mas quebra a alegacao de que eles sao
    # reproduziveis a partir desta arvore, que e o que a baseline existe para sustentar.
    $CurrentCooker = Get-Fingerprint $CookerPath
    if (-not $CurrentCooker) {
        Write-Host ("  COOKER AUSENTE {0}" -f $CookerPath); $Failures++
    } elseif ($CurrentCooker.sha256 -ne $Baseline.cooker.sha256) {
        Write-Host "  COOKER DIFERE  binario atual nao e o registrado na baseline"; $Failures++
    }

    foreach ($Scene in $Selected) {
        $Current  = Get-OutputRecord $Scene
        $Expected = $Baseline.outputs | Where-Object { $_.outBase -eq $Current.outBase }
        if (-not $Expected) {
            Write-Host ("  AUSENTE NA BASELINE  {0}" -f $Current.outBase); $Failures++; continue
        }

        # A FONTE tambem e gitignored: outro FBX no mesmo caminho produziria saidas diferentes
        # sem nenhuma pista. Ausencia e FALHA: antes o guard "se presente" deixava passar um
        # source apagado.
        if (-not $Current["sourceDigest"]) {
            Write-Host ("  FONTE AUSENTE  {0}" -f $Current["source"]); $Failures++
        } elseif ($Expected.sourceDigest.sha256 -ne $Current["sourceDigest"].sha256) {
            Write-Host ("  FONTE DIFERE   {0}" -f $Current["source"]); $Failures++
        }

        foreach ($Ext in @("smesh", "sscene")) {
            $Cur = $Current[$Ext]
            $Exp = $Expected.$Ext
            $Tag = "{0}.{1}" -f $Current.outBase, $Ext
            if (-not $Cur)                 { Write-Host ("  FALTA ARQUIVO  {0}" -f $Tag); $Failures++; continue }
            if ($Cur.formatProblem -ne "") { Write-Host ("  FORMATO        {0}: {1}" -f $Tag, $Cur.formatProblem); $Failures++; continue }
            if ($Cur.sha256 -ne $Exp.sha256) { Write-Host ("  HASH DIFERE    {0}" -f $Tag); $Failures++; continue }
            if ($Cur.version -ne $Exp.version) { Write-Host ("  VERSAO DIFERE  {0} ({1} != {2})" -f $Tag, $Cur.version, $Exp.version); $Failures++; continue }
            Write-Host ("  ok             {0}  v{1}" -f $Tag, $Cur.version)
        }
    }
    if ($Failures -gt 0) { Write-Host ""; throw "$Failures divergencia(s) contra a baseline" }
    Write-Host ""
    Write-Host "assets conferem com a baseline e com o formato esperado (v$ExpectVersion)."
    exit 0
}

# ---------------------------------------------------------------- so registrar
# Adota o script numa arvore que ja tem cozidos validos, sem exigir um recook completo de
# arquivos que ja estao corretos. Formato invalido aqui e ERRO: registrar um v9 seria
# exatamente o furo que o bloco "expect" existe para fechar.
if ($Record) {
    if ($Only -ne "") { throw "-Record exige o conjunto completo (nao combine com -Only)" }
    $Outputs = @()
    foreach ($Scene in $Selected) {
        $Rec = Get-OutputRecord $Scene -Strict
        foreach ($Ext in @("smesh", "sscene")) {
            if (-not $Rec[$Ext]) { throw "asset ausente: $($Rec.outBase).$Ext - cozinhe antes de registrar" }
        }
        Write-Host ("  {0}  v{1}" -f $Rec.outBase, $Rec["sscene"].version)
        $Outputs += ,$Rec
    }
    $AdoptedCooker = Get-Fingerprint $CookerPath
    if (-not $AdoptedCooker) {
        throw "cooker ausente: $CookerPath - a baseline nao pode ser gravada sem o fingerprint dele"
    }
    Write-Baseline $Outputs $AdoptedCooker "adopted"
    Write-Host ""
    Write-Host ("baseline escrita a partir dos assets atuais: {0}" -f $BaselinePath)
    exit 0
}

# ---------------------------------------------------------------- cozimento
# Deixar o CMake decidir o que esta desatualizado e a UNICA forma de respeitar as dependencias
# reais do target (ufbx.c/ufbx.h, CMakeLists, Mesh.h, headers transitivos). A heuristica anterior
# olhava so main.cpp e CookedFormat.h e aceitaria um exe defasado por mudanca em qualquer outra.
if (-not $NoBuild) {
    Write-Host "reconstruindo SmileCooker ($Config)..."
    & cmake --build $BuildRoot --target SmileCooker --config $Config | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build do SmileCooker falhou (exit $LASTEXITCODE)" }
    if (-not $NoOmm) {
        Write-Host "reconstruindo SmileOmmBaker ($Config)..."
        & cmake --build $BuildRoot --target SmileOmmBaker --config $Config | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "build do SmileOmmBaker falhou (exit $LASTEXITCODE)" }
    }
}
if (-not (Test-Path $CookerPath)) {
    throw "cooker nao encontrado: $CookerPath`nRode antes: cmake --build $BuildRoot --target SmileCooker --config $Config"
}
if (-not $NoOmm -and -not (Test-Path $OmmBakerPath)) {
    throw "SmileOmmBaker nao encontrado: $OmmBakerPath`nRode antes: cmake --build $BuildRoot --target SmileOmmBaker --config $Config, ou passe -NoOmm"
}
$CookerFingerprint = Get-Fingerprint $CookerPath
Write-Host ("cooker sha256={0}" -f $CookerFingerprint.sha256.Substring(0, 16))
Write-Host ""

$Outputs = @()
$Failed  = 0
foreach ($Scene in $Selected) {
    $Source = Join-Path $RepoRoot $Scene.source
    if (-not (Test-Path $Source)) { Write-Host ("  FONTE AUSENTE  {0}" -f $Scene.source); $Failed++; continue }

    $Flags = @($Scene.flags)
    $Label = Split-Path $Scene.source -Leaf
    if ($Flags.Count -gt 0) { Write-Host ("=== {0}  [{1}]" -f $Label, ($Flags -join " ")) }
    else                    { Write-Host ("=== {0}" -f $Label) }

    $CookArgs = @($Source) + $Flags
    & $CookerPath @CookArgs | Select-String -Pattern "Meshes:|Instancing:|ERRO" | ForEach-Object { "    $_" }
    if ($LASTEXITCODE -ne 0) { Write-Host ("    FALHOU (exit {0})" -f $LASTEXITCODE); $Failed++; continue }

    # -Strict: saida fora do formato esperado aborta ANTES de virar hash na baseline.
    $Record = Get-OutputRecord $Scene -Strict
    $Outputs += ,$Record

    # Bake offline de OMM sobre o que ACABOU de ser cozido. Incremental: pares cujo bake no .somm
    # ainda bate com a cena nova sao reaproveitados, o resto e refeito.
    if (-not $NoOmm) {
        $SscenePath = Join-Path $RepoRoot "$($Record.outBase).sscene"
        & $OmmBakerPath $SscenePath | Select-String -Pattern "OMM BAKER" | ForEach-Object { "    $_" }
        if ($LASTEXITCODE -ne 0) { Write-Host ("    OMM FALHOU (exit {0})" -f $LASTEXITCODE); $Failed++ }
    }
}

if ($Failed -gt 0) { throw "$Failed cena(s) falharam" }

# So um recook COMPLETO reescreve a baseline: um -Only produziria manifesto parcial, e o
# -Verify seguinte acusaria ausencia nas cenas nao cozidas.
if ($Only -eq "") {
    Write-Baseline $Outputs $CookerFingerprint "cooked"
    Write-Host ""
    Write-Host ("baseline escrita: {0}" -f $BaselinePath)
} else {
    Write-Host ""
    Write-Host "recook parcial (-Only): baseline NAO reescrita."
}
