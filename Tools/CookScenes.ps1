<#
.SYNOPSIS
    Cozinha (ou verifica) as cenas segundo a receita em Tools/Scenes.cook.json.

.DESCRIPTION
    Assets/Scenes/ esta no .gitignore, entao os .smesh/.sscene nao entram no git. O que se
    versiona e a RECEITA (Scenes.cook.json) e a BASELINE de hashes (Tools/cook-baseline.json),
    produzida por este script. Juntas elas tornam o recook reproduzivel e detectam divergencia
    entre a arvore de trabalho e a baseline usada nas medicoes.

    Ver Docs/GPU-DRIVEN-PLAN.md, Fase 0.

.PARAMETER Verify
    Nao cozinha: confere os assets atuais contra Tools/cook-baseline.json e falha se algum
    hash, versao ou arquivo divergir. E o gate "as cenas sao as da baseline?".

.PARAMETER Only
    Substring do caminho de origem; cozinha apenas as cenas que casarem.

.PARAMETER Config
    Configuracao do cooker (default Release).

.EXAMPLE
    powershell -File Tools/CookScenes.ps1
    powershell -File Tools/CookScenes.ps1 -Only Bistro
    powershell -File Tools/CookScenes.ps1 -Verify
#>
[CmdletBinding()]
param(
    [switch] $Verify,
    [switch] $Record,
    [string] $Only    = "",
    [string] $Config  = "Release"
)

$ErrorActionPreference = "Stop"

$RepoRoot     = Split-Path -Parent $PSScriptRoot
$RecipePath   = Join-Path $PSScriptRoot "Scenes.cook.json"
$BaselinePath = Join-Path $PSScriptRoot "cook-baseline.json"

if (-not (Test-Path $RecipePath)) { throw "receita nao encontrada: $RecipePath" }
$Recipe = Get-Content $RecipePath -Raw | ConvertFrom-Json

# Le o cabecalho comum de .smesh/.sscene: magic (u32), versao (u32), e dois contadores.
# O layout dos dois cabecalhos coincide nesses 16 bytes (ver Engine/Include/Smile/Scene/CookedFormat.h).
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

function Get-OutputRecord {
    param([string] $SourceRelative)
    $OutBase  = [System.IO.Path]::ChangeExtension($SourceRelative, $null).TrimEnd('.')
    $Record   = [ordered]@{ outBase = $OutBase }
    foreach ($Ext in @("smesh", "sscene")) {
        $Full = Join-Path $RepoRoot "$OutBase.$Ext"
        if (-not (Test-Path $Full)) { $Record[$Ext] = $null; continue }
        $Header = Get-CookedHeader $Full
        $Record[$Ext] = [ordered]@{
            sha256  = (Get-FileHash $Full -Algorithm SHA256).Hash
            bytes   = (Get-Item $Full).Length
            version = [int] $Header.Version
            countC  = [int] $Header.FieldC
            countD  = [int] $Header.FieldD
        }
    }
    $Record
}

$Selected = $Recipe.scenes | Where-Object { $Only -eq "" -or $_.source -like "*$Only*" }
if (-not $Selected) { throw "nenhuma cena casou com -Only '$Only'" }

# ---------------------------------------------------------------- verificacao
if ($Verify) {
    if (-not (Test-Path $BaselinePath)) { throw "baseline ausente: $BaselinePath (rode sem -Verify primeiro)" }
    $Baseline = Get-Content $BaselinePath -Raw | ConvertFrom-Json
    $Failures = 0
    foreach ($Scene in $Selected) {
        $Current  = Get-OutputRecord $Scene.source
        $Expected = $Baseline.outputs | Where-Object { $_.outBase -eq $Current.outBase }
        if (-not $Expected) {
            Write-Host ("  AUSENTE NA BASELINE  {0}" -f $Current.outBase); $Failures++; continue
        }
        foreach ($Ext in @("smesh", "sscene")) {
            $Cur = $Current[$Ext]
            $Exp = $Expected.$Ext
            if (-not $Cur)                       { Write-Host ("  FALTA ARQUIVO  {0}.{1}" -f $Current.outBase, $Ext); $Failures++; continue }
            if ($Cur.sha256 -ne $Exp.sha256)     { Write-Host ("  HASH DIFERE    {0}.{1}" -f $Current.outBase, $Ext); $Failures++; continue }
            if ($Cur.version -ne $Exp.version)   { Write-Host ("  VERSAO DIFERE  {0}.{1} ({2} != {3})" -f $Current.outBase, $Ext, $Cur.version, $Exp.version); $Failures++; continue }
            Write-Host ("  ok             {0}.{1}  v{2}" -f $Current.outBase, $Ext, $Cur.version)
        }
    }
    if ($Failures -gt 0) { Write-Host ""; throw "$Failures divergencia(s) contra a baseline" }
    Write-Host ""
    Write-Host "assets conferem com a baseline."
    exit 0
}

# ---------------------------------------------------------------- so registrar
# Escreve a baseline a partir dos assets JA presentes, sem cozinhar. E como se adota o script
# numa arvore que ja tem cozidos validos; sem isto, gerar a primeira baseline exigiria um
# recook completo de arquivos que ja estao corretos.
if ($Record) {
    if ($Only -ne "") { throw "-Record exige o conjunto completo (nao combine com -Only)" }
    $Outputs = @()
    foreach ($Scene in $Selected) {
        $Rec = Get-OutputRecord $Scene.source
        foreach ($Ext in @("smesh", "sscene")) {
            if (-not $Rec[$Ext]) { throw "asset ausente: $($Rec.outBase).$Ext - cozinhe antes de registrar" }
        }
        Write-Host ("  {0}  v{1}" -f $Rec.outBase, $Rec["sscene"].version)
        $Outputs += ,$Rec
    }
    $CommitHash = "desconhecido"
    try { $CommitHash = (& git -C $RepoRoot rev-parse --short HEAD).Trim() } catch { }
    $Manifest = [ordered]@{
        generatedUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        commit       = $CommitHash
        config       = $Config
        outputs      = $Outputs
    }
    # PS 5.1 grava BOM com Out-File -Encoding utf8, e BOM quebra parser JSON estrito.
    $Json = $Manifest | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($BaselinePath, $Json, (New-Object System.Text.UTF8Encoding $false))
    Write-Host ""
    Write-Host ("baseline escrita a partir dos assets atuais: {0}" -f $BaselinePath)
    exit 0
}

# ---------------------------------------------------------------- cozimento
$Cooker = Join-Path $RepoRoot ($Recipe.cooker -replace "/Release/", "/$Config/")
if (-not (Test-Path $Cooker)) {
    throw "cooker nao encontrado: $Cooker`nRode antes: cmake --build build --target SmileCooker --config $Config"
}

# O binario precisa ser mais novo que o fonte do cooker. Um exe antigo cozinha num formato
# que o loader deste checkout pode rejeitar: foi exatamente o que produziu os assets v9.
$CookerSource = Join-Path $RepoRoot "Tools/Cooker/main.cpp"
$CookedFormat = Join-Path $RepoRoot "Engine/Include/Smile/Scene/CookedFormat.h"
foreach ($Dep in @($CookerSource, $CookedFormat)) {
    if ((Test-Path $Dep) -and (Get-Item $Dep).LastWriteTime -gt (Get-Item $Cooker).LastWriteTime) {
        throw "cooker desatualizado em relacao a $Dep`nRode antes: cmake --build build --target SmileCooker --config $Config"
    }
}

$Outputs = @()
$Failed  = 0
foreach ($Scene in $Selected) {
    $Source = Join-Path $RepoRoot $Scene.source
    if (-not (Test-Path $Source)) { Write-Host ("  FONTE AUSENTE  {0}" -f $Scene.source); $Failed++; continue }

    $Flags = @($Scene.flags)
    $Label = Split-Path $Scene.source -Leaf
    if ($Flags.Count -gt 0) { Write-Host ("=== {0}  [{1}]" -f $Label, ($Flags -join " ")) }
    else                    { Write-Host ("=== {0}" -f $Label) }

    $Args = @($Source) + $Flags
    & $Cooker @Args | Select-String -Pattern "Meshes:|Instancing:|ERRO" | ForEach-Object { "    $_" }
    if ($LASTEXITCODE -ne 0) { Write-Host ("    FALHOU (exit {0})" -f $LASTEXITCODE); $Failed++; continue }

    $Outputs += ,(Get-OutputRecord $Scene.source)
}

if ($Failed -gt 0) { throw "$Failed cena(s) falharam" }

$CommitHash = "desconhecido"
try { $CommitHash = (& git -C $RepoRoot rev-parse --short HEAD).Trim() } catch { }

# So reescreve a baseline num recook COMPLETO: um -Only produziria um arquivo parcial que
# depois faria o -Verify acusar ausencia nas cenas nao cozidas.
if ($Only -eq "") {
    $Manifest = [ordered]@{
        generatedUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        commit       = $CommitHash
        config       = $Config
        outputs      = $Outputs
    }
    # PS 5.1 grava BOM com Out-File -Encoding utf8, e BOM quebra parser JSON estrito.
    $Json = $Manifest | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($BaselinePath, $Json, (New-Object System.Text.UTF8Encoding $false))
    Write-Host ""
    Write-Host ("baseline escrita: {0}" -f $BaselinePath)
} else {
    Write-Host ""
    Write-Host "recook parcial (-Only): baseline NAO reescrita."
}
